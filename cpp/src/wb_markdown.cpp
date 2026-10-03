// cpp/src/wb_markdown.cpp
#include "meradb/wb_markdown.h"
#include "meradb/pytext.h"
#include <algorithm>
#include <cstddef>

namespace meradb::wb {

namespace {

struct Gl {
    std::string text;
    int width = 0;
    Style style;
    bool isSpace() const { return text == " "; }
};

std::vector<Gl> flatten(const Line& line) {
    std::vector<Gl> out;
    for (const Segment& seg : line) {
        for (const Glyph& g : glyphs(seg.text)) out.push_back(Gl{g.text, g.width, seg.style});
    }
    return out;
}

Line build(const std::vector<Gl>& g, std::size_t from, std::size_t to) {
    while (to > from && g[to - 1].isSpace()) --to;  // no trailing blanks
    Line line;
    for (std::size_t i = from; i < to; ++i) appendSegment(line, g[i].text, g[i].style);
    return line;
}

// Break at the last space that fits, hard-break words longer than the width, drop the space at a break.
std::vector<Line> wrapWords(const Line& line, int width) {
    if (width < 1) width = 1;
    const std::vector<Gl> g = flatten(line);
    std::vector<Line> out;
    std::size_t start = 0;
    while (start < g.size()) {
        int w = 0;
        std::size_t e = start;
        while (e < g.size() && (e == start || w + g[e].width <= width)) {
            w += g[e].width;
            ++e;
        }
        if (e >= g.size()) {
            out.push_back(build(g, start, g.size()));
            break;
        }
        std::size_t next;
        if (g[e].isSpace()) {
            out.push_back(build(g, start, e));
            next = e;
        } else {
            std::size_t k = std::string::npos;
            for (std::size_t i = e; i > start + 1; --i) {
                if (g[i - 1].isSpace()) {
                    k = i - 1;
                    break;
                }
            }
            if (k != std::string::npos) {
                out.push_back(build(g, start, k));
                next = k;
            } else {
                out.push_back(build(g, start, e));
                next = e;
            }
        }
        while (next < g.size() && g[next].isSpace()) ++next;
        start = next;
    }
    if (out.empty()) out.push_back(Line{});
    return out;
}

Style withBold(Style s) {
    s.bold = true;
    return s;
}

void inlineInto(Line& out, const std::string& s, const Style& st) {
    std::string lit;
    auto flush = [&] {
        appendSegment(out, lit, st);
        lit.clear();
    };
    std::size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size() && std::string("\\`*|#>-_[]()").find(s[i + 1]) != std::string::npos) {
            lit += s[i + 1];
            i += 2;
        } else if (c == '`') {
            const std::size_t j = s.find('`', i + 1);
            if (j != std::string::npos && j > i + 1) {
                flush();
                Style code = st;
                code.fg = palette::kCyan;
                appendSegment(out, s.substr(i + 1, j - i - 1), code);
                i = j + 1;
            } else {
                lit += c;
                ++i;
            }
        } else if (c == '*' && i + 1 < s.size() && s[i + 1] == '*') {
            const std::size_t j = s.find("**", i + 2);
            if (j != std::string::npos && j > i + 2) {
                flush();
                inlineInto(out, s.substr(i + 2, j - i - 2), withBold(st));
                i = j + 2;
            } else {
                lit += "**";
                i += 2;
            }
        } else if (c == '*') {
            std::size_t j = std::string::npos;
            if (i + 1 < s.size() && s[i + 1] != ' ') {
                for (std::size_t k = i + 1; k < s.size(); ++k) {
                    if (s[k] == '*' && s[k - 1] != ' ' && (k + 1 >= s.size() || s[k + 1] != '*')) {
                        j = k;
                        break;
                    }
                }
            }
            if (j != std::string::npos) {
                flush();
                Style it = st;
                it.italic = true;
                inlineInto(out, s.substr(i + 1, j - i - 1), it);
                i = j + 1;
            } else {
                lit += c;
                ++i;
            }
        } else {
            lit += c;
            ++i;
        }
    }
    flush();
}

Line parseInline(const std::string& s, const Style& base) {
    Line out;
    inlineInto(out, s, base);
    return out;
}

std::string trim(const std::string& s) { return pytext::strip(s); }

bool startsWithStr(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

int headingLevel(const std::string& line) {
    std::size_t n = 0;
    while (n < line.size() && line[n] == '#') ++n;
    if (n >= 1 && n <= 6 && n < line.size() && line[n] == ' ') return static_cast<int>(n);
    return 0;
}

bool isRule(const std::string& line) {
    const std::string t = trim(line);
    if (t.size() < 3) return false;
    const char c = t[0];
    if (c != '-' && c != '*' && c != '_') return false;
    for (char x : t)
        if (x != c) return false;
    return true;
}

// "- text" / "* text" / "+ text" (after the indentation): the length of the marker, else 0.
std::size_t bulletMarker(const std::string& t) {
    if (t.size() >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') return 2;
    return 0;
}

// "12. text": the length of the marker including the space, else 0.
std::size_t numberMarker(const std::string& t) {
    std::size_t n = 0;
    while (n < t.size() && t[n] >= '0' && t[n] <= '9') ++n;
    if (n >= 1 && n + 1 < t.size() && t[n] == '.' && t[n + 1] == ' ') return n + 2;
    return 0;
}

// How far a line is indented, in cells: a space is 1, a tab 4, any other leading whitespace character 1 (a
// multi-byte one such as NBSP counts once). Capped so absurd indentation cannot blow up the output.
std::size_t indentOf(const std::string& line) {
    constexpr std::size_t kMaxIndent = 32;
    const std::size_t lead = line.find(trim(line));
    const std::size_t end = lead == std::string::npos ? 0 : lead;
    std::size_t cells = 0;
    for (std::size_t k = 0; k < end; ++k) {
        const unsigned char c = static_cast<unsigned char>(line[k]);
        if (c == '\t') cells += 4;
        else if ((c & 0xC0) != 0x80) cells += 1;  // not a UTF-8 continuation byte
    }
    return cells < kMaxIndent ? cells : kMaxIndent;
}

std::vector<std::string> splitRows(const std::string& md) {
    std::vector<std::string> rows;
    std::string cur;
    for (char c : md) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            rows.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty() && cur.back() == '\r') cur.pop_back();
    if (!cur.empty()) rows.push_back(cur);
    return rows;
}

std::vector<std::string> splitCells(const std::string& row) {
    std::string t = trim(row);
    if (!t.empty() && t[0] == '|') t.erase(0, 1);
    std::vector<std::string> cells;
    std::string cur;
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|') {
            cur += '|';
            ++i;
        } else if (t[i] == '|') {
            cells.push_back(trim(cur));
            cur.clear();
        } else {
            cur += t[i];
        }
    }
    if (!trim(cur).empty()) cells.push_back(trim(cur));
    return cells;
}

bool isDividerRow(const std::vector<std::string>& cells) {
    if (cells.empty()) return false;
    for (const std::string& c : cells) {
        if (c.empty()) return false;
        for (char x : c)
            if (x != '-' && x != ':') return false;
    }
    return true;
}

int lineWidth(const Line& l) {
    int w = 0;
    for (const Segment& s : l)
        for (const Glyph& g : glyphs(s.text)) w += g.width;
    return w;
}

void appendPadded(Line& line, int cells) {
    if (cells > 0) appendSegment(line, std::string(static_cast<std::size_t>(cells), ' '), Style{});
}

void appendLine(Line& to, const Line& from) {
    for (const Segment& s : from) appendSegment(to, s.text, s.style);
}

}  // namespace

std::vector<Line> renderMarkdown(const std::string& markdown, int width) {
    if (width < 1) width = 1;
    const std::vector<std::string> rows = splitRows(markdown);
    std::vector<Line> out;
    auto blockStart = [&out] {
        if (!out.empty()) out.push_back(Line{});
    };
    const Style plain;

    std::size_t i = 0;
    while (i < rows.size()) {
        const std::string& raw = rows[i];
        const std::string t = trim(raw);
        if (t.empty()) {
            ++i;
            continue;
        }
        // fenced code block
        if (startsWithStr(t, "```")) {
            blockStart();
            ++i;
            Style code;
            code.bg = palette::kCurrentLine;
            while (i < rows.size() && !startsWithStr(trim(rows[i]), "```")) {
                Line l;
                appendSegment(l, rows[i], code);
                out.push_back(l);
                ++i;
            }
            if (i < rows.size()) ++i;  // the closing fence
            continue;
        }
        // heading
        if (const int level = headingLevel(t)) {
            blockStart();
            Style st;
            st.bold = true;
            if (level == 1) st.fg = palette::kPink;
            else if (level == 2) st.fg = palette::kCyan;
            for (const Line& l : wrapWords(parseInline(trim(t.substr(static_cast<std::size_t>(level) + 1)), st), width))
                out.push_back(l);
            ++i;
            continue;
        }
        // horizontal rule
        if (isRule(t)) {
            blockStart();
            Line l;
            std::string bar;
            for (int k = 0; k < width; ++k) bar += "\xE2\x94\x80";  // U+2500
            appendSegment(l, bar, plain);
            out.push_back(l);
            ++i;
            continue;
        }
        // table
        if (t[0] == '|') {
            std::vector<std::vector<std::string>> cellRows;
            bool header = false;
            std::size_t headerRows = 0;
            while (i < rows.size() && !trim(rows[i]).empty() && trim(rows[i])[0] == '|') {
                std::vector<std::string> cells = splitCells(rows[i]);
                if (cellRows.size() == 1 && isDividerRow(cells)) {
                    header = true;
                    headerRows = 1;
                } else {
                    cellRows.push_back(std::move(cells));
                }
                ++i;
            }
            std::size_t cols = 0;
            for (const auto& r : cellRows) cols = std::max(cols, r.size());
            std::vector<std::vector<Line>> styled;
            std::vector<int> colWidth(cols, 0);
            for (std::size_t r = 0; r < cellRows.size(); ++r) {
                std::vector<Line> lineCells;
                Style base;
                base.bold = header && r < headerRows;
                for (std::size_t c = 0; c < cols; ++c) {
                    Line cell = c < cellRows[r].size() ? parseInline(cellRows[r][c], base) : Line{};
                    colWidth[c] = std::max(colWidth[c], lineWidth(cell));
                    lineCells.push_back(std::move(cell));
                }
                styled.push_back(std::move(lineCells));
            }
            blockStart();
            for (std::size_t r = 0; r < styled.size(); ++r) {
                Line l;
                for (std::size_t c = 0; c < cols; ++c) {
                    if (c) appendSegment(l, " \xE2\x94\x82 ", plain);  // " │ "
                    appendLine(l, styled[r][c]);
                    if (c + 1 < cols) appendPadded(l, colWidth[c] - lineWidth(styled[r][c]));
                }
                out.push_back(l);
                if (header && r + 1 == headerRows) {
                    Line d;
                    std::string text;
                    for (std::size_t c = 0; c < cols; ++c) {
                        if (c) text += "\xE2\x94\x80\xE2\x94\xBC\xE2\x94\x80";  // "─┼─"
                        for (int k = 0; k < colWidth[c]; ++k) text += "\xE2\x94\x80";
                    }
                    appendSegment(d, text, plain);
                    out.push_back(d);
                }
            }
            continue;
        }
        // quote
        if (t[0] == '>') {
            std::string text;
            while (i < rows.size() && trim(rows[i]).size() > 0 && trim(rows[i])[0] == '>') {
                std::string part = trim(rows[i]).substr(1);
                part = trim(part);
                if (!text.empty() && !part.empty()) text += ' ';
                text += part;
                ++i;
            }
            blockStart();
            Style dim;
            dim.dim = true;
            const int inner = std::max(1, width - 2);
            for (const Line& l : wrapWords(parseInline(text, dim), inner)) {
                Line q;
                appendSegment(q, "\xE2\x96\x8C ", dim);  // "▌ "
                appendLine(q, l);
                out.push_back(q);
            }
            continue;
        }
        // list (bullets and numbered items)
        if (bulletMarker(trim(raw)) || numberMarker(trim(raw))) {
            struct Item {
                std::size_t indent;
                std::string marker;
                std::string text;
            };
            std::vector<Item> items;
            while (i < rows.size()) {
                const std::string& r = rows[i];
                const std::string tt = trim(r);
                if (tt.empty()) break;
                // The block is entered on trim(raw), so the item loop must see the same text: the body is the
                // trimmed line (tabs, NBSP and other Unicode whitespace included) and the indent is how far in it sat.
                const std::size_t ind = indentOf(r);
                const std::string& body = tt;
                if (const std::size_t bm = bulletMarker(body)) {
                    items.push_back(Item{ind, "\xE2\x80\xA2 ", trim(body.substr(bm))});  // "• "
                } else if (const std::size_t nm = numberMarker(body)) {
                    items.push_back(Item{ind, body.substr(0, nm), trim(body.substr(nm))});
                } else if (!items.empty() && ind > 0) {
                    items.back().text += " " + tt;  // a continuation line
                } else {
                    break;
                }
                ++i;
            }
            if (items.empty()) {  // cannot happen (the block is entered on a marker), but never stall
                ++i;
                continue;
            }
            blockStart();
            for (const Item& item : items) {
                const int markerWidth = lineWidth(parseInline(item.marker, plain));
                const int prefix = static_cast<int>(item.indent) + markerWidth;
                const int inner = std::max(1, width - prefix);
                bool first = true;
                for (const Line& l : wrapWords(parseInline(item.text, plain), inner)) {
                    Line row;
                    appendPadded(row, static_cast<int>(item.indent));
                    if (first) appendSegment(row, item.marker, plain);
                    else appendPadded(row, markerWidth);
                    appendLine(row, l);
                    out.push_back(row);
                    first = false;
                }
            }
            continue;
        }
        // paragraph
        {
            std::string text;
            while (i < rows.size()) {
                const std::string tt = trim(rows[i]);
                if (tt.empty() || startsWithStr(tt, "```") || headingLevel(tt) || isRule(tt) || tt[0] == '|' ||
                    tt[0] == '>' || bulletMarker(tt) || numberMarker(tt)) {
                    if (!text.empty()) break;
                }
                if (!text.empty()) text += ' ';
                text += tt;
                ++i;
            }
            blockStart();
            for (const Line& l : wrapWords(parseInline(text, plain), width)) out.push_back(l);
        }
    }
    return out;
}

}  // namespace meradb::wb
