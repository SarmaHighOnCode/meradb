// cpp/src/wb_text.cpp
#include "meradb/wb_text.h"
#include "meradb/datatypes.h"
#include "meradb/fs_util.h"
#include "meradb/highlight.h"
#include "meradb/pytext.h"
#include "meradb/sys_compat.h"
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ftxui/screen/string.hpp>
#include <system_error>
#include <variant>

namespace meradb::wb {

namespace fs = std::filesystem;

// ---- styles ----

Style fgStyle(int rgb, bool bold, bool dim, bool italic) {
    Style s;
    s.fg = rgb;
    s.bold = bold;
    s.dim = dim;
    s.italic = italic;
    return s;
}

std::string plainText(const Line& line) {
    std::string out;
    for (const Segment& s : line) out += s.text;
    return out;
}

void appendSegment(Line& line, const std::string& text, const Style& style) {
    if (text.empty()) return;
    if (!line.empty() && line.back().style == style) {
        line.back().text += text;
        return;
    }
    line.push_back(Segment{text, style});
}

// ---- the log ----

const char* logKindName(LogKind kind) {
    switch (kind) {
        case LogKind::Plain: return "plain";
        case LogKind::Bold: return "bold";
        case LogKind::Dim: return "dim";
        case LogKind::Error: return "error";
        case LogKind::Message: return "message";
        case LogKind::Connected: return "connected";
        case LogKind::Warn: return "warn";
        case LogKind::Echo: return "echo";
    }
    return "plain";
}

Style styleForLogKind(LogKind kind) {
    switch (kind) {
        case LogKind::Plain: return Style{};
        case LogKind::Bold: return fgStyle(-1, true);
        case LogKind::Dim: return fgStyle(-1, false, true);
        case LogKind::Error: return fgStyle(palette::kRed, true);
        case LogKind::Message: return fgStyle(palette::kGreen);
        case LogKind::Connected: return fgStyle(palette::kGreen, true);
        case LogKind::Warn: return fgStyle(palette::kYellow);
        case LogKind::Echo: return Style{};
    }
    return Style{};
}

void LogBuffer::add(LogEntry entry) {
    lines_ += entry.lines.size();
    entries_.push_back(std::move(entry));
    ++revision_;
    while (lines_ > kMaxLines && entries_.size() > 1) {
        lines_ -= entries_.front().lines.size();
        entries_.pop_front();
        ++trimmed_;
    }
}

void LogBuffer::addText(LogKind kind, const std::string& text) {
    LogEntry entry;
    entry.kind = kind;
    const Style style = styleForLogKind(kind);
    std::size_t start = 0;
    for (;;) {
        const std::size_t nl = text.find('\n', start);
        const std::string piece = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        Line line;
        appendSegment(line, piece, style);
        entry.lines.push_back(std::move(line));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    add(std::move(entry));
}

void LogBuffer::clear() {
    entries_.clear();
    lines_ = 0;
    ++revision_;
    ++generation_;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> out;
    const std::size_t n = text.size();
    std::size_t start = 0, i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t sepLen = 0;
        if (c == '\n' || c == 0x0b || c == 0x0c || c == 0x1c || c == 0x1d || c == 0x1e) sepLen = 1;
        else if (c == '\r') sepLen = (i + 1 < n && text[i + 1] == '\n') ? 2 : 1;
        else if (c == 0xC2 && i + 1 < n && static_cast<unsigned char>(text[i + 1]) == 0x85) sepLen = 2;
        else if (c == 0xE2 && i + 2 < n && static_cast<unsigned char>(text[i + 1]) == 0x80 &&
                 (static_cast<unsigned char>(text[i + 2]) == 0xA8 || static_cast<unsigned char>(text[i + 2]) == 0xA9))
            sepLen = 3;
        if (sepLen == 0) {
            ++i;
            continue;
        }
        out.push_back(text.substr(start, i - start));
        i += sepLen;
        start = i;
    }
    if (start < n) out.push_back(text.substr(start));
    return out;
}

Line highlightLine(const std::string& line) { return highlightRange(line, 0, line.size()); }

Line highlightRange(const std::string& line, std::size_t from, std::size_t to) {
    to = std::min(to, line.size());
    Line out;
    if (from >= to) return out;
    std::size_t pos = from;  // everything before `pos` in the range has been emitted
    for (const highlight::Span& span : highlight::spans(line, from, to)) {
        const std::size_t start = std::max(span.start, from);
        const std::size_t end = std::min(span.end, to);
        if (start > pos) appendSegment(out, line.substr(pos, start - pos), Style{});
        const highlight::RichStyle rich = highlight::richStyle(span.kind);
        Style style;
        style.fg = rich.rgb;
        style.bold = rich.bold;
        style.italic = rich.italic;
        appendSegment(out, line.substr(start, end - start), style);
        pos = end;
    }
    if (pos < to) appendSegment(out, line.substr(pos, to - pos), Style{});
    return out;
}

LogEntry echoEntry(const std::string& db, const std::string& text) {
    LogEntry entry;
    entry.kind = LogKind::Echo;
    const std::vector<std::string> pieces = splitLines(pytext::strip(text));
    const Style prompt = fgStyle(-1, false, true);
    if (pieces.empty()) {
        Line line;
        appendSegment(line, db + "> ", prompt);
        entry.lines.push_back(std::move(line));
        return entry;
    }
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        Line line;
        if (i == 0) appendSegment(line, db + "> ", prompt);
        for (const Segment& seg : highlightLine(pieces[i])) appendSegment(line, seg.text, seg.style);
        entry.lines.push_back(std::move(line));
    }
    return entry;
}

// ---- the results table ----

const char* cellKindName(CellKind kind) {
    switch (kind) {
        case CellKind::Null: return "null";
        case CellKind::True: return "true";
        case CellKind::False: return "false";
        case CellKind::Number: return "number";
        case CellKind::Plain: return "plain";
    }
    return "plain";
}

Cell makeCell(const Value& value) {
    Cell cell;
    if (std::holds_alternative<std::monostate>(value.data)) {
        cell.text = "KHALI";
        cell.kind = CellKind::Null;
        return cell;
    }
    if (const bool* b = std::get_if<bool>(&value.data)) cell.kind = *b ? CellKind::True : CellKind::False;
    else if (std::holds_alternative<int64_t>(value.data) || std::holds_alternative<double>(value.data))
        cell.kind = CellKind::Number;
    else cell.kind = CellKind::Plain;
    const std::string raw = formatValue(value);
    for (char c : raw) {
        if (c == '\n' || c == '\r') cell.text += "\xE2\x86\xB5";  // R16: a table row is one line
        else if (c == '\t') cell.text += ' ';
        else cell.text += c;
    }
    return cell;
}

Style cellStyle(CellKind kind) {
    switch (kind) {
        case CellKind::Null: return fgStyle(-1, false, true, true);
        case CellKind::True: return fgStyle(palette::kGreen);
        case CellKind::False: return fgStyle(palette::kRed);
        case CellKind::Number: return fgStyle(palette::kPurple);
        case CellKind::Plain: return Style{};
    }
    return Style{};
}

ResultTable makeTable(const Result& result) {
    ResultTable table;
    table.columns = result.columns;
    for (const auto& row : result.rows) {
        std::vector<Cell> cells;
        for (const Value& v : row) cells.push_back(makeCell(v));
        table.rows.push_back(std::move(cells));
    }
    return table;
}

std::vector<int> columnWidths(const ResultTable& table) {
    std::vector<int> widths;
    for (const std::string& c : table.columns) widths.push_back(ftxui::string_width(c));
    for (const auto& row : table.rows) {
        for (std::size_t i = 0; i < row.size(); ++i) {
            if (i >= widths.size()) widths.push_back(0);
            widths[i] = std::max(widths[i], ftxui::string_width(row[i].text));
        }
    }
    return widths;
}

std::string resultsTitle(const ResultTable* table) {
    if (table == nullptr) return "Results";
    return "Results -- " + std::to_string(table->rows.size()) + " row(s)";
}

// ---- cell geometry ----

std::vector<Glyph> glyphs(const std::string& utf8) {
    std::vector<Glyph> out;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char lead = static_cast<unsigned char>(utf8[i]);
        std::size_t len = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        len = std::min(len, utf8.size() - i);
        const std::string cp = utf8.substr(i, len);
        i += len;
        const bool stray = lead >= 0x80 && lead < 0xC0;  // a continuation byte with no lead
        const int w = stray ? 1 : ftxui::string_width(cp);
        if (w == 0 && !out.empty()) {
            out.back().text += cp;  // a combining mark rides on the previous character
        } else {
            out.push_back(Glyph{cp, w});
        }
    }
    return out;
}

Line clipLine(const Line& line, int skipCells, int takeCells) {
    Line out;
    if (takeCells <= 0) return out;
    const int end = skipCells + takeCells;
    int pos = 0;
    for (const Segment& seg : line) {
        for (const Glyph& g : glyphs(seg.text)) {
            const int gs = pos;
            const int ge = pos + g.width;
            pos = ge;
            if (gs >= end) return out;
            if (g.width > 0 && ge <= skipCells) continue;
            if (gs >= skipCells && ge <= end) {
                appendSegment(out, g.text, seg.style);
            } else {
                const int blanks = std::min(ge, end) - std::max(gs, skipCells);
                if (blanks > 0) appendSegment(out, std::string(static_cast<std::size_t>(blanks), ' '), seg.style);
            }
        }
    }
    return out;
}

std::vector<Line> wrapLine(const Line& line, int width) {
    if (width < 1) width = 1;
    std::vector<Line> out;
    Line cur;
    int curW = 0;
    for (const Segment& seg : line) {
        for (const Glyph& g : glyphs(seg.text)) {
            if (curW > 0 && curW + g.width > width) {
                out.push_back(std::move(cur));
                cur = Line{};
                curW = 0;
            }
            appendSegment(cur, g.text, seg.style);
            curW += g.width;
        }
    }
    out.push_back(std::move(cur));
    return out;
}

// ---- CSV ----

std::string csvRow(const std::vector<std::string>& fields) {
    std::string out;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) out += ',';
        const std::string& f = fields[i];
        const bool lone = fields.size() == 1 && f.empty();
        const bool quote = lone || f.find_first_of(",\"\r\n") != std::string::npos;
        if (!quote) {
            out += f;
            continue;
        }
        out += '"';
        for (char c : f) {
            if (c == '"') out += '"';
            out += c;
        }
        out += '"';
    }
    out += "\r\n";
    return out;
}

std::string csvDocument(const Result& result) {
    std::string out = csvRow(result.columns);
    for (const auto& row : result.rows) {
        std::vector<std::string> fields;
        for (const Value& v : row) fields.push_back(v.isNull() ? std::string() : formatValue(v));
        out += csvRow(fields);
    }
    return out;
}

std::string exportFileStamp() {
    std::string stamp = sys::localLogStamp();  // YYYY-MM-DD HH:MM:SS
    std::string out;
    for (char c : stamp) {
        if (c == '-' || c == ':') continue;
        out += (c == ' ') ? '-' : c;
    }
    return out;
}

ExportOutcome exportCsv(const Result& result, const std::string& baseDir, const std::string& stamp) {
    ExportOutcome outcome;
    const fs::path dir = pathOf(baseDir.empty() ? "." : baseDir) / "exports";
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        outcome.error = ec.message();
        return outcome;
    }
    const fs::path file = dir / pathOf("meradb-" + stamp + ".csv");
    const std::string document = csvDocument(result);
    errno = 0;
    std::ofstream out(file, std::ios::binary);
    if (!out) {
        const int err = errno;
        outcome.error = err != 0 ? std::generic_category().message(err) : "file nahi khul paayi";
        return outcome;
    }
    out.write(document.data(), static_cast<std::streamsize>(document.size()));
    out.flush();
    if (!out) {
        outcome.error = "likhna fail ho gaya";
        return outcome;
    }
    out.close();
    std::error_code abs;
    fs::path full = fs::absolute(file, abs);
    if (abs) full = file;
    outcome.path = textOf(full.lexically_normal());
    outcome.ok = true;
    return outcome;
}

// ---- history and scrolling ----

void History::add(const std::string& text) {
    if (items_.empty() || items_.back() != text) items_.push_back(text);
    pos_ = items_.size();
}

std::optional<std::string> History::step(int delta) {
    if (items_.empty()) return std::nullopt;
    const std::int64_t size = static_cast<std::int64_t>(items_.size());
    std::int64_t next = static_cast<std::int64_t>(pos_) + delta;
    next = std::max<std::int64_t>(0, std::min<std::int64_t>(size, next));
    pos_ = static_cast<std::size_t>(next);
    if (pos_ >= items_.size()) return std::string();
    return items_[pos_];
}

int ScrollState::maxTop() const { return std::max(0, count_ - height_); }

void ScrollState::clamp() {
    top_ = std::max(0, std::min(maxTop(), top_));
    cursor_ = std::max(0, std::min(std::max(0, count_ - 1), cursor_));
}

void ScrollState::setCount(int n) {
    count_ = std::max(0, n);
    clamp();
}

void ScrollState::setHeight(int h) {
    height_ = std::max(1, h);
    clamp();
}

void ScrollState::ensureVisible(int row) {
    row = std::max(0, std::min(std::max(0, count_ - 1), row));
    if (row < top_) top_ = row;
    else if (row >= top_ + height_) top_ = row - height_ + 1;
    clamp();
}

void ScrollState::setCursor(int row) {
    cursor_ = row;
    clamp();
    ensureVisible(cursor_);
}

void ScrollState::moveCursor(int delta) { setCursor(cursor_ + delta); }

void ScrollState::pageCursor(int pages) { moveCursor(pages * std::max(1, height_ - 1)); }

void ScrollState::scrollBy(int delta) {
    top_ += delta;
    clamp();
}

void ScrollState::followEnd() { top_ = maxTop(); }

bool ScrollState::atBottom() const { return top_ >= maxTop(); }

}  // namespace meradb::wb
