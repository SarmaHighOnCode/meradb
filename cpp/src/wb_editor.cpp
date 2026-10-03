// cpp/src/wb_editor.cpp
#include "meradb/wb_editor.h"
#include <algorithm>
#include <ftxui/screen/string.hpp>

namespace meradb::wb {

namespace {

std::size_t cpLen(unsigned char lead) { return lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1; }

int cpCount(const std::string& s) {
    int n = 0;
    for (std::size_t i = 0; i < s.size(); i += std::min(cpLen(static_cast<unsigned char>(s[i])), s.size() - i)) ++n;
    return n;
}

std::size_t byteAt(const std::string& s, int col) {  // byte offset of code point column `col` (clamped to the end)
    std::size_t i = 0;
    while (col > 0 && i < s.size()) {
        i += std::min(cpLen(static_cast<unsigned char>(s[i])), s.size() - i);
        --col;
    }
    return i;
}

std::vector<std::string> codePoints(const std::string& s) {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < s.size();) {
        const std::size_t len = std::min(cpLen(static_cast<unsigned char>(s[i])), s.size() - i);
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}

bool isWordCp(const std::string& cp) {
    const unsigned char c = static_cast<unsigned char>(cp[0]);
    return c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

std::string normalised(const std::string& in) {
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '\r') {
            if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
            out += '\n';
        } else {
            out += c;
        }
    }
    return out;
}

std::vector<std::string> splitOnNewline(const std::string& s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t nl = s.find('\n', start);
        if (nl == std::string::npos) {
            out.push_back(s.substr(start));
            return out;
        }
        out.push_back(s.substr(start, nl - start));
        start = nl + 1;
    }
}

}  // namespace

Pos TextBuffer::clampPos(Pos p) const {
    p.row = std::max(0, std::min(p.row, lineCount() - 1));
    p.col = std::max(0, std::min(p.col, cpCount(lines_[static_cast<std::size_t>(p.row)])));
    return p;
}

void TextBuffer::setText(const std::string& text) {
    lines_ = splitOnNewline(normalised(text));
    anchor_.reset();
    cursor_ = Pos{lineCount() - 1, cpCount(lines_.back())};
    desiredCol_ = cursor_.col;
}

std::string TextBuffer::text() const {
    std::string out;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        if (i) out += '\n';
        out += lines_[i];
    }
    return out;
}

std::pair<Pos, Pos> TextBuffer::selection() const {
    if (!anchor_.has_value()) return {cursor_, cursor_};
    if (*anchor_ < cursor_) return {*anchor_, cursor_};
    return {cursor_, *anchor_};
}

std::string TextBuffer::selectedText() const {
    if (!hasSelection()) return "";
    const auto sel = selection();
    const Pos from = sel.first;
    const Pos to = sel.second;
    if (from.row == to.row) {
        const std::string& l = lines_[static_cast<std::size_t>(from.row)];
        const std::size_t a = byteAt(l, from.col);
        return l.substr(a, byteAt(l, to.col) - a);
    }
    const std::string& first = lines_[static_cast<std::size_t>(from.row)];
    std::string out = first.substr(byteAt(first, from.col));
    for (int r = from.row + 1; r < to.row; ++r) out += "\n" + lines_[static_cast<std::size_t>(r)];
    const std::string& last = lines_[static_cast<std::size_t>(to.row)];
    out += "\n" + last.substr(0, byteAt(last, to.col));
    return out;
}

void TextBuffer::selectRange(Pos anchor, Pos cursor) {
    anchor_ = clampPos(anchor);
    cursor_ = clampPos(cursor);
    desiredCol_ = cursor_.col;
}

void TextBuffer::selectAll() {
    anchor_ = Pos{0, 0};
    cursor_ = Pos{lineCount() - 1, cpCount(lines_.back())};
    desiredCol_ = cursor_.col;
}

void TextBuffer::deleteSelection() {
    if (!hasSelection()) {
        anchor_.reset();
        return;
    }
    const auto sel = selection();
    const Pos from = sel.first;
    const Pos to = sel.second;
    const std::string head = lines_[static_cast<std::size_t>(from.row)].substr(0, byteAt(lines_[static_cast<std::size_t>(from.row)], from.col));
    const std::string& lastLine = lines_[static_cast<std::size_t>(to.row)];
    const std::string tail = lastLine.substr(byteAt(lastLine, to.col));
    lines_.erase(lines_.begin() + from.row, lines_.begin() + to.row + 1);
    lines_.insert(lines_.begin() + from.row, head + tail);
    cursor_ = from;
    desiredCol_ = from.col;
    anchor_.reset();
}

void TextBuffer::insert(const std::string& utf8) {
    std::string clean;
    const std::string src = normalised(utf8);
    for (char ch : src) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '\n') clean += '\n';
        else if (c == '\t') clean += "    ";
        else if (c < 0x20 || c == 0x7F) continue;
        else clean += ch;
    }
    deleteSelection();
    if (clean.empty()) return;
    const std::vector<std::string> pieces = splitOnNewline(clean);
    std::string& cur = lines_[static_cast<std::size_t>(cursor_.row)];
    const std::size_t at = byteAt(cur, cursor_.col);
    const std::string after = cur.substr(at);
    const std::string before = cur.substr(0, at);
    if (pieces.size() == 1) {
        cur = before + pieces[0] + after;
        cursor_.col += cpCount(pieces[0]);
    } else {
        cur = before + pieces[0];
        std::vector<std::string> rest(pieces.begin() + 1, pieces.end());
        rest.back() += after;
        lines_.insert(lines_.begin() + cursor_.row + 1, rest.begin(), rest.end());
        cursor_.row += static_cast<int>(pieces.size()) - 1;
        cursor_.col = cpCount(pieces.back());
    }
    desiredCol_ = cursor_.col;
}

void TextBuffer::backspace() {
    if (hasSelection()) {
        deleteSelection();
        return;
    }
    anchor_.reset();
    std::string& cur = lines_[static_cast<std::size_t>(cursor_.row)];
    if (cursor_.col > 0) {
        const std::size_t b = byteAt(cur, cursor_.col - 1);
        const std::size_t e = byteAt(cur, cursor_.col);
        cur.erase(b, e - b);
        --cursor_.col;
    } else if (cursor_.row > 0) {
        std::string& prev = lines_[static_cast<std::size_t>(cursor_.row - 1)];
        const int col = cpCount(prev);
        prev += cur;
        lines_.erase(lines_.begin() + cursor_.row);
        --cursor_.row;
        cursor_.col = col;
    }
    desiredCol_ = cursor_.col;
}

void TextBuffer::del() {
    if (hasSelection()) {
        deleteSelection();
        return;
    }
    anchor_.reset();
    std::string& cur = lines_[static_cast<std::size_t>(cursor_.row)];
    if (cursor_.col < cpCount(cur)) {
        const std::size_t b = byteAt(cur, cursor_.col);
        const std::size_t e = byteAt(cur, cursor_.col + 1);
        cur.erase(b, e - b);
    } else if (cursor_.row + 1 < lineCount()) {
        cur += lines_[static_cast<std::size_t>(cursor_.row + 1)];
        lines_.erase(lines_.begin() + cursor_.row + 1);
    }
    desiredCol_ = cursor_.col;
}

void TextBuffer::moveTo(Pos p, bool extendSelection) {
    if (extendSelection) {
        if (!anchor_.has_value()) anchor_ = cursor_;
    } else {
        anchor_.reset();
    }
    cursor_ = clampPos(p);
    desiredCol_ = cursor_.col;
}

void TextBuffer::move(Move m, bool extendSelection) {
    if (!extendSelection && hasSelection() && (m == Move::Left || m == Move::Right)) {
        const auto sel = selection();
        cursor_ = (m == Move::Left) ? sel.first : sel.second;
        desiredCol_ = cursor_.col;
        anchor_.reset();
        return;
    }
    Pos target = cursor_;
    bool keepDesired = false;
    const int lastRow = lineCount() - 1;
    const std::string& cur = lines_[static_cast<std::size_t>(cursor_.row)];
    switch (m) {
        case Move::Left:
            if (target.col > 0) --target.col;
            else if (target.row > 0) {
                --target.row;
                target.col = cpCount(lines_[static_cast<std::size_t>(target.row)]);
            }
            break;
        case Move::Right:
            if (target.col < cpCount(cur)) ++target.col;
            else if (target.row < lastRow) {
                ++target.row;
                target.col = 0;
            }
            break;
        case Move::Up:
        case Move::PageUp: {
            const int step = (m == Move::Up) ? 1 : 5;
            keepDesired = true;
            if (target.row == 0) {
                target.col = 0;
                keepDesired = false;
            } else {
                target.row = std::max(0, target.row - step);
                target.col = std::min(desiredCol_, cpCount(lines_[static_cast<std::size_t>(target.row)]));
            }
            break;
        }
        case Move::Down:
        case Move::PageDown: {
            const int step = (m == Move::Down) ? 1 : 5;
            keepDesired = true;
            if (target.row == lastRow) {
                target.col = cpCount(cur);
                keepDesired = false;
            } else {
                target.row = std::min(lastRow, target.row + step);
                target.col = std::min(desiredCol_, cpCount(lines_[static_cast<std::size_t>(target.row)]));
            }
            break;
        }
        case Move::Home: target.col = 0; break;
        case Move::End: target.col = cpCount(cur); break;
        case Move::DocStart: target = Pos{0, 0}; break;
        case Move::DocEnd: target = Pos{lastRow, cpCount(lines_.back())}; break;
        case Move::WordLeft: {
            if (target.col == 0) {
                if (target.row > 0) {
                    --target.row;
                    target.col = cpCount(lines_[static_cast<std::size_t>(target.row)]);
                }
                break;
            }
            const std::vector<std::string> cps = codePoints(cur);
            int c = target.col;
            while (c > 0 && !isWordCp(cps[static_cast<std::size_t>(c - 1)])) --c;
            while (c > 0 && isWordCp(cps[static_cast<std::size_t>(c - 1)])) --c;
            target.col = c;
            break;
        }
        case Move::WordRight: {
            const std::vector<std::string> cps = codePoints(cur);
            const int n = static_cast<int>(cps.size());
            if (target.col >= n) {
                if (target.row < lastRow) {
                    ++target.row;
                    target.col = 0;
                }
                break;
            }
            int c = target.col;
            while (c < n && !isWordCp(cps[static_cast<std::size_t>(c)])) ++c;
            while (c < n && isWordCp(cps[static_cast<std::size_t>(c)])) ++c;
            target.col = c;
            break;
        }
    }
    const int wanted = desiredCol_;
    moveTo(target, extendSelection);
    if (keepDesired) desiredCol_ = wanted;
}

int gutterWidth(int lineCount) {
    int digits = 1;
    for (int n = lineCount; n >= 10; n /= 10) ++digits;
    return std::max(2, digits) + 1;
}

std::string gutterText(int row, int lineCount) {
    const std::string number = std::to_string(row + 1);
    const int width = gutterWidth(lineCount) - 1;
    const int pad = std::max(0, width - static_cast<int>(number.size()));
    return std::string(static_cast<std::size_t>(pad), ' ') + number + " ";
}

int displayColumn(const std::string& line, int cpCol) {
    int cells = 0;
    int col = 0;
    for (std::size_t i = 0; i < line.size() && col < cpCol; ++col) {
        const std::size_t len = std::min(cpLen(static_cast<unsigned char>(line[i])), line.size() - i);
        cells += ftxui::string_width(line.substr(i, len));
        i += len;
    }
    return cells;
}

Line editorRowLine(const TextBuffer& buffer, int row, bool focused) {
    const std::string& text = buffer.line(row);
    std::vector<std::pair<std::string, Style>> cells;
    for (const Segment& seg : highlightLine(text))
        for (const std::string& cp : codePoints(seg.text)) cells.emplace_back(cp, seg.style);
    if (buffer.hasSelection()) {
        const auto sel = buffer.selection();
        if (row >= sel.first.row && row <= sel.second.row) {
            const int from = (row == sel.first.row) ? sel.first.col : 0;
            const int to = (row == sel.second.row) ? sel.second.col : static_cast<int>(cells.size());
            for (int c = from; c < to && c < static_cast<int>(cells.size()); ++c)
                cells[static_cast<std::size_t>(c)].second.bg = palette::kCurrentLine;
        }
    }
    if (focused && buffer.cursor().row == row) {
        const int col = buffer.cursor().col;
        if (col < static_cast<int>(cells.size())) cells[static_cast<std::size_t>(col)].second.inverse = true;
        else {
            Style s;
            s.inverse = true;
            cells.emplace_back(" ", s);
        }
    }
    Line out;
    for (const auto& cell : cells) appendSegment(out, cell.first, cell.second);
    return out;
}

}  // namespace meradb::wb
