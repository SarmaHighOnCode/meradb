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

// True when every lead byte is followed by exactly the continuation bytes it announces and there is no stray
// continuation byte, so that stepping back from a code point boundary finds the previous boundary.
bool wellFormedUtf8(const std::string& s) {
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char lead = static_cast<unsigned char>(s[i]);
        if (lead >= 0x80 && lead < 0xC0) return false;
        const std::size_t len = cpLen(lead);
        if (len > s.size() - i) return false;
        for (std::size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

bool isWordByte(unsigned char c) {
    return c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// Start offsets of the code points (used only to step backwards over text that is not well formed).
std::vector<std::size_t> cpStarts(const std::string& s) {
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < s.size();) {
        out.push_back(i);
        i += std::min(cpLen(static_cast<unsigned char>(s[i])), s.size() - i);
    }
    return out;
}

std::size_t cpLenAt(const std::string& s, std::size_t b) { return std::min(cpLen(static_cast<unsigned char>(s[b])), s.size() - b); }

// Terminal cells of the code point at s[b] as wb::glyphs() counts them (a stray continuation byte is one cell).
int cpWidth(const std::string& s, std::size_t b) {
    const unsigned char lead = static_cast<unsigned char>(s[b]);
    if (lead >= 0x20 && lead < 0x7F) return 1;
    if (lead >= 0x80 && lead < 0xC0) return 1;
    return ftxui::string_width(s.substr(b, cpLenAt(s, b)));
}

// A code point that glyphs() merges into the character before it (zero width, not the first one).
bool isMarkAt(const std::string& s, std::size_t b) { return b > 0 && cpWidth(s, b) == 0; }

std::size_t prevStart(const std::string& s, std::size_t b) {  // b > 0
    std::size_t p = b - 1;
    while (p > 0 && b - p < 4 && (static_cast<unsigned char>(s[p]) & 0xC0) == 0x80) --p;
    return p;
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

void TextBuffer::fillCache(int row) const {
    if (cache_.row == row && cache_.count >= 0) return;
    const std::string& s = lines_[static_cast<std::size_t>(row)];
    cache_ = RowCache{};
    cache_.row = row;
    cache_.count = cpCount(s);
    cache_.wellFormed = wellFormedUtf8(s);
}

// The cached (column, byte) pair must describe a real position: a cursor column past the end of the row (possible
// after malformed bytes merge into one character) means the end.
void TextBuffer::clampPair(int row) const {
    if (cache_.col > cache_.count) {
        cache_.col = cache_.count;
        cache_.byte = lines_[static_cast<std::size_t>(row)].size();
    }
}

int TextBuffer::lineLength(int row) const {
    fillCache(row);
    return cache_.count;
}

std::size_t TextBuffer::byteOffset(int row, int col) const {
    const std::string& s = lines_[static_cast<std::size_t>(row)];
    if (col <= 0) return 0;
    fillCache(row);
    if (col >= cache_.count) return s.size();
    int c = cache_.col;
    std::size_t b = cache_.byte;
    if (col < c) {
        if (cache_.wellFormed && c - col <= col) {
            while (c > col) {
                --b;
                while ((static_cast<unsigned char>(s[b]) & 0xC0) == 0x80) --b;
                --c;
            }
        } else {
            c = 0;
            b = 0;
        }
    }
    while (c < col && b < s.size()) {
        b += std::min(cpLen(static_cast<unsigned char>(s[b])), s.size() - b);
        ++c;
    }
    cache_.col = c;
    cache_.byte = b;
    return b;
}

Pos TextBuffer::clampPos(Pos p) const {
    p.row = std::max(0, std::min(p.row, lineCount() - 1));
    p.col = std::max(0, std::min(p.col, lineLength(p.row)));
    return p;
}

void TextBuffer::setText(const std::string& text) {
    lines_ = splitOnNewline(normalised(text));
    cache_ = RowCache{};
    anchor_.reset();
    cursor_ = Pos{lineCount() - 1, lineLength(lineCount() - 1)};
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
        const std::size_t a = byteOffset(from.row, from.col);
        return l.substr(a, byteOffset(from.row, to.col) - a);
    }
    const std::string& first = lines_[static_cast<std::size_t>(from.row)];
    std::string out = first.substr(byteOffset(from.row, from.col));
    for (int r = from.row + 1; r < to.row; ++r) out += "\n" + lines_[static_cast<std::size_t>(r)];
    const std::string& last = lines_[static_cast<std::size_t>(to.row)];
    out += "\n" + last.substr(0, byteOffset(to.row, to.col));
    return out;
}

void TextBuffer::selectRange(Pos anchor, Pos cursor) {
    anchor_ = clampPos(anchor);
    cursor_ = clampPos(cursor);
    desiredCol_ = cursor_.col;
}

void TextBuffer::selectAll() {
    anchor_ = Pos{0, 0};
    cursor_ = Pos{lineCount() - 1, lineLength(lineCount() - 1)};
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
    const std::string head = lines_[static_cast<std::size_t>(from.row)].substr(0, byteOffset(from.row, from.col));
    const std::string& lastLine = lines_[static_cast<std::size_t>(to.row)];
    const std::string tail = lastLine.substr(byteOffset(to.row, to.col));
    cache_ = RowCache{};
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
    const int row = cursor_.row;
    std::string& cur = lines_[static_cast<std::size_t>(row)];
    fillCache(row);
    const std::size_t at = byteOffset(row, cursor_.col);
    if (pieces.size() == 1) {
        // In place: typing into a very long line must not copy or rescan it.
        const int added = cpCount(pieces[0]);
        const int count = cache_.count;
        const bool wellFormed = cache_.wellFormed && wellFormedUtf8(pieces[0]);
        cur.insert(at, pieces[0]);
        cursor_.col += added;
        if (wellFormed) {
            cache_.count = count + added;
            cache_.col = cursor_.col;
            cache_.byte = at + pieces[0].size();
            clampPair(row);
        } else {
            cache_ = RowCache{};
        }
    } else {
        const std::string after = cur.substr(at);
        const std::string before = cur.substr(0, at);
        cache_ = RowCache{};
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
    const int row = cursor_.row;
    std::string& cur = lines_[static_cast<std::size_t>(row)];
    if (cursor_.col > 0) {
        fillCache(row);
        const std::size_t b = byteOffset(row, cursor_.col - 1);
        const std::size_t e = byteOffset(row, cursor_.col);
        const int count = cache_.count;
        const bool wellFormed = cache_.wellFormed;
        cur.erase(b, e - b);
        --cursor_.col;
        if (wellFormed) {
            cache_.count = e > b ? count - 1 : count;  // a cursor beyond the end erases nothing
            cache_.col = cursor_.col;
            cache_.byte = b;
            clampPair(row);
        } else {
            cache_ = RowCache{};
        }
    } else if (cursor_.row > 0) {
        std::string& prev = lines_[static_cast<std::size_t>(cursor_.row - 1)];
        const int col = lineLength(cursor_.row - 1);
        cache_ = RowCache{};
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
    const int row = cursor_.row;
    std::string& cur = lines_[static_cast<std::size_t>(row)];
    if (cursor_.col < lineLength(row)) {
        const std::size_t b = byteOffset(row, cursor_.col);
        const std::size_t e = byteOffset(row, cursor_.col + 1);
        const int count = cache_.count;
        const bool wellFormed = cache_.wellFormed;
        cur.erase(b, e - b);
        if (wellFormed) {
            cache_.count = count - 1;
            cache_.col = cursor_.col;
            cache_.byte = b;
            clampPair(row);
        } else {
            cache_ = RowCache{};
        }
    } else if (cursor_.row + 1 < lineCount()) {
        cache_ = RowCache{};
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
    switch (m) {
        case Move::Left:
            if (target.col > 0) --target.col;
            else if (target.row > 0) {
                --target.row;
                target.col = lineLength(target.row);
            }
            break;
        case Move::Right:
            if (target.col < lineLength(cursor_.row)) ++target.col;
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
                target.col = std::min(desiredCol_, lineLength(target.row));
            }
            break;
        }
        case Move::Down:
        case Move::PageDown: {
            const int step = (m == Move::Down) ? 1 : 5;
            keepDesired = true;
            if (target.row == lastRow) {
                target.col = lineLength(cursor_.row);
                keepDesired = false;
            } else {
                target.row = std::min(lastRow, target.row + step);
                target.col = std::min(desiredCol_, lineLength(target.row));
            }
            break;
        }
        case Move::Home: target.col = 0; break;
        case Move::End: target.col = lineLength(cursor_.row); break;
        case Move::DocStart: target = Pos{0, 0}; break;
        case Move::DocEnd: target = Pos{lastRow, lineLength(lastRow)}; break;
        case Move::WordLeft: {
            if (target.col == 0) {
                if (target.row > 0) {
                    --target.row;
                    target.col = lineLength(target.row);
                }
                break;
            }
            const std::string& cur = lines_[static_cast<std::size_t>(cursor_.row)];
            int c = std::min(target.col, lineLength(cursor_.row));
            std::size_t b = byteOffset(cursor_.row, c);
            const bool wellFormed = cache_.wellFormed;
            std::vector<std::size_t> starts;  // only for text that is not well formed
            if (!wellFormed) starts = cpStarts(cur);
            auto before = [&]() {  // start of the code point before (c, b)
                if (!wellFormed) return starts[static_cast<std::size_t>(c - 1)];
                std::size_t p = b - 1;
                while ((static_cast<unsigned char>(cur[p]) & 0xC0) == 0x80) --p;
                return p;
            };
            while (c > 0 && !isWordByte(static_cast<unsigned char>(cur[before()]))) { b = before(); --c; }
            while (c > 0 && isWordByte(static_cast<unsigned char>(cur[before()]))) { b = before(); --c; }
            target.col = c;
            break;
        }
        case Move::WordRight: {
            const std::string& cur = lines_[static_cast<std::size_t>(cursor_.row)];
            const int n = lineLength(cursor_.row);
            if (target.col >= n) {
                if (target.row < lastRow) {
                    ++target.row;
                    target.col = 0;
                }
                break;
            }
            int c = target.col;
            std::size_t b = byteOffset(cursor_.row, c);
            while (c < n && !isWordByte(static_cast<unsigned char>(cur[b]))) { b += cpLenAt(cur, b); ++c; }
            while (c < n && isWordByte(static_cast<unsigned char>(cur[b]))) { b += cpLenAt(cur, b); ++c; }
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
        const unsigned char lead = static_cast<unsigned char>(line[i]);
        const std::size_t len = cpLenAt(line, i);
        cells += (lead >= 0x20 && lead < 0x7F) ? 1 : ftxui::string_width(line.substr(i, len));
        i += len;
    }
    return cells;
}

namespace {

// The row styled for the editor, restricted to the bytes [lo, hi) (both on code point boundaries): highlighted,
// the selection on a lighter background, the cursor's whole character (base letter and its combining marks) in
// inverse, or a trailing inverse space when the cursor is past the end of the row and hi reaches it.
Line buildRow(const TextBuffer& buffer, int row, bool focused, std::size_t lo, std::size_t hi) {
    const std::string& text = buffer.line(row);
    const std::size_t none = static_cast<std::size_t>(-1);
    std::size_t selFrom = none, selTo = none;  // bytes of the selection on this row
    if (buffer.hasSelection()) {
        const auto sel = buffer.selection();
        if (row >= sel.first.row && row <= sel.second.row) {
            selFrom = (row == sel.first.row) ? buffer.byteOffset(row, sel.first.col) : 0;
            selTo = (row == sel.second.row) ? buffer.byteOffset(row, sel.second.col) : text.size();
            // a combining mark never gets a background of its own: the selection covers whole characters
            while (selFrom > 0 && selFrom < text.size() && isMarkAt(text, selFrom)) selFrom = prevStart(text, selFrom);
            while (selTo < text.size() && isMarkAt(text, selTo)) selTo += cpLenAt(text, selTo);
        }
    }
    std::size_t curFrom = none, curTo = none;  // bytes of the cursor's character
    bool endSpace = false;
    if (focused && buffer.cursor().row == row) {
        const int col = buffer.cursor().col;
        if (col < buffer.lineLength(row)) {
            const std::size_t b = buffer.byteOffset(row, col);
            std::size_t from = b;
            while (from > 0 && isMarkAt(text, from)) from = prevStart(text, from);
            std::size_t to = b + cpLenAt(text, b);
            while (to < text.size() && isMarkAt(text, to)) to += cpLenAt(text, to);
            curFrom = from;
            curTo = to;
        } else {
            endSpace = true;
        }
    }
    std::vector<std::size_t> cuts;
    for (std::size_t c : {selFrom, selTo, curFrom, curTo})
        if (c != none) cuts.push_back(c);
    std::sort(cuts.begin(), cuts.end());

    Line out;
    std::size_t pos = lo;
    for (const Segment& seg : highlightRange(text, lo, hi)) {
        const std::size_t segStart = pos;
        const std::size_t segEnd = pos + seg.text.size();
        pos = segEnd;
        std::size_t x = segStart;
        const std::size_t limit = segEnd;
        while (x < limit) {
            std::size_t y = limit;
            for (std::size_t c : cuts)
                if (c > x && c < y) { y = c; break; }
            Style style = seg.style;
            if (selFrom != none && x >= selFrom && y <= selTo) style.bg = palette::kCurrentLine;
            if (curFrom != none && x >= curFrom && y <= curTo) style.inverse = true;
            appendSegment(out, seg.text.substr(x - segStart, y - x), style);
            x = y;
        }
    }
    if (endSpace && hi >= text.size()) {
        Style s;
        s.inverse = true;
        appendSegment(out, " ", s);
    }
    return out;
}

}  // namespace

Line editorRowLine(const TextBuffer& buffer, int row, bool focused) {
    return buildRow(buffer, row, focused, 0, buffer.line(row).size());
}

Line editorRowWindow(const TextBuffer& buffer, int row, bool focused, int skipCells, int takeCells) {
    Line none;
    if (takeCells <= 0) return none;
    const std::string& text = buffer.line(row);
    const int end = skipCells + takeCells;
    // Walk glyph by glyph to the one holding cell `skipCells`, then on to the first glyph that starts at or past `end`.
    std::size_t lo = text.size(), hi = text.size();
    int pos = 0;       // cell where the glyph at b starts
    int loPos = 0;     // cell where the glyph at lo starts
    bool haveLo = skipCells <= 0;  // from the left edge: everything, even a leading zero-width character
    if (haveLo) lo = 0;
    for (std::size_t b = 0; b < text.size();) {
        const int w = cpWidth(text, b);
        const bool mark = isMarkAt(text, b);
        if (!mark) {
            if (haveLo && pos >= end) {
                hi = b;
                break;
            }
            if (!haveLo && w > 0 && pos + w > skipCells) {
                lo = b;
                loPos = pos;
                haveLo = true;
            }
        }
        pos += w;
        b += cpLenAt(text, b);
    }
    if (!haveLo) loPos = pos;
    return clipLine(buildRow(buffer, row, focused, lo, hi), skipCells - loPos, takeCells);
}

}  // namespace meradb::wb
