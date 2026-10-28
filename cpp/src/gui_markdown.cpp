// cpp/src/gui_markdown.cpp -- see gui_markdown.h.
#include "meradb/gui_markdown.h"
#include "meradb/gui_editor.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace meradb::gui {

namespace {

// ---------------------------------------------------------------- text helpers

bool isSpaceByte(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

// U+00A0 (no-break space) is a character like any other for wrapping, but counts as blank when a line is checked for
// being empty or indented.
bool isNbspAt(const std::string& s, std::size_t i) {
    return i + 1 < s.size() && static_cast<unsigned char>(s[i]) == 0xC2 && static_cast<unsigned char>(s[i + 1]) == 0xA0;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    for (;;) {
        if (a < b && isSpaceByte(s[a])) ++a;
        else if (isNbspAt(s, a) && a + 2 <= b) a += 2;
        else break;
    }
    for (;;) {
        if (b > a && isSpaceByte(s[b - 1])) --b;
        else if (b >= a + 2 && isNbspAt(s, b - 2)) b -= 2;
        else break;
    }
    return s.substr(a, b - a);
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\n' || c == '\r') {
            lines.push_back(std::move(current));
            current.clear();
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else if (c == '\0') {
            current += ' ';   // a NUL byte never reaches the drawing code
        } else {
            current += c;
        }
    }
    lines.push_back(std::move(current));
    return lines;
}

// Leading blanks of a line: their width in columns (a tab counts 4) and their length in bytes.
struct Lead {
    int columns = 0;
    std::size_t bytes = 0;
};
Lead leadOf(const std::string& line) {
    Lead lead;
    for (;;) {
        if (lead.bytes < line.size() && line[lead.bytes] == ' ') {
            ++lead.columns;
            ++lead.bytes;
        } else if (lead.bytes < line.size() && line[lead.bytes] == '\t') {
            lead.columns += 4;
            ++lead.bytes;
        } else if (isNbspAt(line, lead.bytes)) {
            ++lead.columns;
            lead.bytes += 2;
        } else {
            return lead;
        }
    }
}

bool isBlank(const std::string& line) { return leadOf(line).bytes == line.size() || trim(line).empty(); }

bool startsWith(const std::string& s, std::size_t at, const char* prefix) {
    const std::size_t n = std::strlen(prefix);
    return s.size() >= at + n && s.compare(at, n, prefix) == 0;
}

bool isFence(const std::string& rest) { return startsWith(rest, 0, "```") || startsWith(rest, 0, "~~~"); }

// "#", "##" ... "######" followed by a blank or the end: the level, else 0.
int headingLevel(const std::string& rest) {
    std::size_t k = 0;
    while (k < rest.size() && rest[k] == '#') ++k;
    if (k == 0 || k > 6) return 0;
    if (k < rest.size() && !isSpaceByte(rest[k])) return 0;
    return static_cast<int>(k);
}

bool isRule(const std::string& rest) {
    char mark = 0;
    int count = 0;
    for (char c : rest) {
        if (isSpaceByte(c)) continue;
        if (c != '-' && c != '*' && c != '_') return false;
        if (mark != 0 && c != mark) return false;
        mark = c;
        ++count;
    }
    return count >= 3;
}

bool isBullet(const std::string& rest) {
    return rest.size() >= 2 && (rest[0] == '-' || rest[0] == '*' || rest[0] == '+') && (rest[1] == ' ' || rest[1] == '\t');
}

// "12. " or "12) ": the number text, or an empty string.
std::string numberedMarker(const std::string& rest) {
    std::size_t k = 0;
    while (k < rest.size() && k < 9 && rest[k] >= '0' && rest[k] <= '9') ++k;
    if (k == 0 || k + 1 >= rest.size()) return "";
    if ((rest[k] == '.' || rest[k] == ')') && (rest[k + 1] == ' ' || rest[k + 1] == '\t')) return rest.substr(0, k) + ".";
    return "";
}

std::vector<std::string> splitCells(const std::string& line) {
    std::string row = trim(line);
    if (!row.empty() && row[0] == '|') row.erase(0, 1);
    std::vector<std::string> cells;
    std::string cell;
    for (std::size_t i = 0; i < row.size(); ++i) {
        if (row[i] == '\\' && i + 1 < row.size() && row[i + 1] == '|') {
            cell += '|';
            ++i;
        } else if (row[i] == '|') {
            cells.push_back(trim(cell));
            cell.clear();
        } else {
            cell += row[i];
        }
    }
    if (!trim(cell).empty() || cells.empty()) cells.push_back(trim(cell));   // no phantom cell after a closing |
    return cells;
}

bool isDivider(const std::string& line) {
    if (line.find('|') == std::string::npos) return false;
    const std::vector<std::string> cells = splitCells(line);
    if (cells.empty()) return false;
    for (const std::string& cell : cells) {
        std::size_t a = 0, b = cell.size();
        if (a < b && cell[a] == ':') ++a;
        if (b > a && cell[b - 1] == ':') --b;
        if (b <= a) return false;
        for (std::size_t i = a; i < b; ++i)
            if (cell[i] != '-') return false;
    }
    return true;
}

// Does this line begin a block of its own (so a paragraph ends before it)?
bool beginsBlock(const std::string& line, const std::string* next) {
    const std::string rest = line.substr(leadOf(line).bytes);
    if (rest.empty()) return true;
    if (isFence(rest) || headingLevel(rest) > 0 || isBullet(rest) || !numberedMarker(rest).empty() || rest[0] == '>') return true;
    if (isRule(rest)) return true;
    if (rest[0] == '|' && next != nullptr && isDivider(*next)) return true;
    return false;
}

void pushSpan(std::vector<MdSpan>& out, const std::string& text, bool bold, bool italic, bool code) {
    if (text.empty()) return;
    if (!out.empty() && out.back().bold == bold && out.back().italic == italic && out.back().code == code && !code) {
        out.back().text += text;
        return;
    }
    out.push_back(MdSpan{text, bold, italic, code});
}

struct InlineState {
    bool noCode = false, noBold = false, noItalic = false;   // a search for the closing marker already failed: it will again
};

void parseInlineInto(const std::string& text, bool bold, bool italic, std::vector<MdSpan>& out, InlineState& state) {
    std::string plain;
    auto flush = [&] {
        pushSpan(out, plain, bold, italic, false);
        plain.clear();
    };
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const char c = text[i];
        if (c == '\\' && i + 1 < n && std::ispunct(static_cast<unsigned char>(text[i + 1])) != 0) {
            plain += text[i + 1];
            i += 2;
            continue;
        }
        if (c == '`' && !state.noCode) {
            std::size_t run = 0;
            while (i + run < n && text[i + run] == '`') ++run;
            const std::string ticks(run, '`');
            const std::size_t close = text.find(ticks, i + run);
            if (close == std::string::npos) {
                state.noCode = true;
            } else {
                flush();
                std::string code = text.substr(i + run, close - i - run);
                if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ') code = code.substr(1, code.size() - 2);
                pushSpan(out, code, bold, italic, true);
                i = close + run;
                continue;
            }
        }
        if (c == '*' && i + 1 < n && text[i + 1] == '*' && !state.noBold) {
            const bool opens = i + 2 < n && !isSpaceByte(text[i + 2]);
            const std::size_t close = opens ? text.find("**", i + 2) : std::string::npos;
            if (opens && close == std::string::npos) state.noBold = true;
            if (close != std::string::npos && close > i + 2) {
                flush();
                InlineState inner;
                parseInlineInto(text.substr(i + 2, close - i - 2), true, italic, out, inner);
                i = close + 2;
                continue;
            }
        }
        if (c == '*' && !state.noItalic && !(i + 1 < n && text[i + 1] == '*')) {
            const bool opens = i + 1 < n && !isSpaceByte(text[i + 1]);
            std::size_t close = std::string::npos;
            if (opens) {
                close = i + 1;
                while ((close = text.find('*', close)) != std::string::npos && close + 1 < n && text[close + 1] == '*') close += 2;
                if (close == std::string::npos) state.noItalic = true;
            }
            if (close != std::string::npos && close > i + 1) {
                flush();
                InlineState inner;
                parseInlineInto(text.substr(i + 1, close - i - 1), bold, true, out, inner);
                i = close + 1;
                continue;
            }
        }
        plain += c;
        ++i;
    }
    flush();
}

// Every code point of a line as its own piece: [begin, end) byte offsets, by the lead byte (a stray byte is one piece).
std::size_t cpLength(const std::string& s, std::size_t i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::size_t len = c < 0x80 ? 1 : (c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : (c >= 0xC0 ? 2 : 1)));
    if (i + len > s.size()) len = s.size() - i;
    return len;
}

}  // namespace

std::vector<MdSpan> parseInline(const std::string& text) {
    std::vector<MdSpan> spans;
    InlineState state;
    parseInlineInto(text, false, false, spans, state);
    return spans;
}

std::string plainText(const std::vector<MdSpan>& spans) {
    std::string out;
    for (const MdSpan& span : spans) out += span.text;
    return out;
}

std::vector<MdBlock> parseMarkdown(const std::string& markdown) {
    const std::vector<std::string> lines = splitLines(markdown);
    const std::size_t n = lines.size();
    std::vector<MdBlock> blocks;
    std::vector<int> listIndents;   // the open list's indentation levels
    std::size_t i = 0;
    while (i < n) {
        const std::string& line = lines[i];
        if (isBlank(line)) {
            ++i;
            listIndents.clear();
            continue;
        }
        const Lead lead = leadOf(line);
        const std::string rest = line.substr(lead.bytes);
        const std::string* next = i + 1 < n ? &lines[i + 1] : nullptr;
        MdBlock block;
        bool isList = false;

        if (isFence(rest)) {
            block.kind = MdBlock::Kind::Code;
            block.info = trim(rest.substr(3));
            ++i;
            while (i < n) {
                const std::string closing = lines[i].substr(leadOf(lines[i]).bytes);
                ++i;
                if (isFence(closing) && trim(closing.substr(3)).empty()) break;
                block.codeLines.push_back(lines[i - 1]);
            }
        } else if (const int level = headingLevel(rest); level > 0) {
            block.kind = MdBlock::Kind::Heading;
            block.level = level;
            std::string title = trim(rest.substr(static_cast<std::size_t>(level)));
            while (!title.empty() && title.back() == '#') title.pop_back();   // closing #s
            block.spans = parseInline(trim(title));
            ++i;
        } else if (isRule(rest)) {
            block.kind = MdBlock::Kind::Rule;
            ++i;
        } else if (rest[0] == '|' && next != nullptr && isDivider(*next)) {
            block.kind = MdBlock::Kind::Table;
            std::vector<std::vector<std::string>> cells;
            cells.push_back(splitCells(line));
            i += 2;
            while (i < n && !isBlank(lines[i]) && lines[i].substr(leadOf(lines[i]).bytes)[0] == '|') {
                cells.push_back(splitCells(lines[i]));
                ++i;
            }
            std::size_t columns = 0;
            for (const auto& row : cells) columns = std::max(columns, row.size());
            columns = std::min<std::size_t>(columns, MdBlock::kMaxColumns);
            block.columns = static_cast<int>(columns);
            for (auto& row : cells) {
                row.resize(columns);
                std::vector<std::vector<MdSpan>> parsed;
                for (const std::string& cell : row) parsed.push_back(parseInline(cell));
                block.rows.push_back(std::move(parsed));
            }
        } else if (isBullet(rest) || !numberedMarker(rest).empty()) {
            isList = true;
            const bool bullet = isBullet(rest);
            block.kind = bullet ? MdBlock::Kind::Bullet : MdBlock::Kind::Numbered;
            std::size_t skip = 2;
            if (!bullet) {
                block.marker = numberedMarker(rest);
                skip = rest.find_first_of(".)") + 1;
            }
            std::string body = trim(rest.substr(skip));
            while (!listIndents.empty() && listIndents.back() > lead.columns) listIndents.pop_back();
            if (listIndents.empty() || listIndents.back() < lead.columns) listIndents.push_back(lead.columns);
            block.level = std::min(static_cast<int>(listIndents.size()) - 1, MdBlock::kMaxDepth);
            ++i;
            while (i < n && !isBlank(lines[i]) && leadOf(lines[i]).columns > lead.columns && !beginsBlock(lines[i], i + 1 < n ? &lines[i + 1] : nullptr)) {
                body += " " + trim(lines[i]);   // a continuation line
                ++i;
            }
            block.spans = parseInline(body);
        } else if (rest[0] == '>') {
            block.kind = MdBlock::Kind::Quote;
            std::string body;
            while (i < n && !isBlank(lines[i])) {
                std::string q = lines[i].substr(leadOf(lines[i]).bytes);
                if (q.empty() || q[0] != '>') break;
                q.erase(0, 1);
                if (!body.empty()) body += " ";
                body += trim(q);
                ++i;
            }
            block.spans = parseInline(body);
        } else {
            block.kind = MdBlock::Kind::Paragraph;
            std::string body = trim(line);
            ++i;
            while (i < n && !isBlank(lines[i]) && !beginsBlock(lines[i], i + 1 < n ? &lines[i + 1] : nullptr)) {
                body += " " + trim(lines[i]);
                ++i;
            }
            block.spans = parseInline(body);
        }
        if (!isList) listIndents.clear();
        blocks.push_back(std::move(block));
    }
    return blocks;
}

// ---------------------------------------------------------------- flow layout

Flow layoutFlow(const std::vector<MdSpan>& spans, float width, const MeasureFn& measure) {
    struct Token {
        std::string text;
        MdSpan style;
        bool spaceBefore = false;
    };
    std::vector<Token> tokens;
    bool pendingSpace = false;
    for (const MdSpan& span : spans) {
        MdSpan style = span;
        style.text.clear();
        if (span.code && span.text.size() <= 48) {
            std::string text = span.text;
            for (char& c : text)
                if (isSpaceByte(c)) c = ' ';
            if (!text.empty()) {
                tokens.push_back(Token{text, style, pendingSpace});
                pendingSpace = false;
            }
            continue;
        }
        std::string word;
        auto endWord = [&] {
            if (word.empty()) return;
            tokens.push_back(Token{word, style, pendingSpace});
            word.clear();
            pendingSpace = false;
        };
        for (char c : span.text) {
            if (isSpaceByte(c)) {
                endWord();
                pendingSpace = true;
            } else {
                word += c;
            }
        }
        endWord();
    }

    Flow flow;
    if (tokens.empty()) return flow;
    if (!(width > 1.0f)) width = 1.0f;
    MdSpan plain;
    const float space = std::max(0.0f, measure(" ", plain));
    float x = 0.0f;
    int line = 0;
    bool first = true;   // nothing placed on this line yet
    auto place = [&](const std::string& text, const MdSpan& style, float w, float gap) {
        FlowWord out;
        out.text = text;
        out.style = style;
        out.x = x + gap;
        out.width = w;
        out.line = line;
        x += gap + w;
        first = false;
        flow.words.push_back(std::move(out));
    };
    for (const Token& token : tokens) {
        const float w = measure(token.text, token.style);
        float gap = first || !token.spaceBefore ? 0.0f : space;
        if (!first && x + gap + w > width) {
            ++line;
            x = 0.0f;
            first = true;
            gap = 0.0f;
        }
        if (w <= width) {
            place(token.text, token.style, w, gap);
            continue;
        }
        // wider than a whole line: cut between characters (one measurement per character, so the work stays linear)
        std::size_t pos = 0;
        std::string piece;
        float pieceWidth = 0.0f;
        while (pos < token.text.size()) {
            const std::size_t len = cpLength(token.text, pos);
            const std::string ch = token.text.substr(pos, len);
            const float cw = measure(ch, token.style);
            if (!piece.empty() && x + gap + pieceWidth + cw > width) {
                place(piece, token.style, pieceWidth, gap);
                piece.clear();
                pieceWidth = 0.0f;
                ++line;
                x = 0.0f;
                first = true;
                gap = 0.0f;
            }
            piece += ch;
            pieceWidth += cw;
            pos += len;
        }
        if (!piece.empty()) place(piece, token.style, pieceWidth, gap);
    }
    flow.lines = line + 1;
    return flow;
}

// ---------------------------------------------------------------- drawing

namespace {

constexpr float kBodySize = kUiFontSize;
constexpr float kCodeSize = kMonoFontSize;

float headingSize(int level) {
    static const float sizes[] = {24.0f, 19.0f, 16.0f, 14.0f, 14.0f, 14.0f};
    return sizes[std::max(1, std::min(level, 6)) - 1];
}

// The font of a span at `baseSize`: bold = Geist Medium, code = Geist Mono, else Geist.
struct Pick {
    ImFont* font;
    float base;
};
Pick pickFont(const MdSpan& style, const FontSet& fonts, float baseSize, bool forceMedium) {
    if (style.code) return Pick{fonts.mono, kCodeSize};
    if (style.bold || forceMedium) return Pick{fonts.uiMedium, baseSize};
    return Pick{fonts.ui, baseSize};
}

bool highlightsAsQuery(const std::string& info) {
    std::string lower;
    for (unsigned char c : info) lower += static_cast<char>(std::tolower(c));
    return lower.empty() || lower == "sql" || lower == "mql" || lower == "meradb";
}

std::vector<std::string> cutCode(const std::vector<std::string>& lines, int maxChars) {
    std::vector<std::string> rows;
    for (const std::string& raw : lines) {
        std::string line;
        for (char c : raw) {
            if (c == '\t') line += "    ";
            else line += c;
        }
        if (line.empty()) {
            rows.emplace_back();
            continue;
        }
        std::size_t pos = 0;
        while (pos < line.size()) {
            std::size_t end = pos;
            int count = 0;
            while (end < line.size() && count < maxChars) {
                end += cpLength(line, end);
                ++count;
            }
            rows.push_back(line.substr(pos, end - pos));
            pos = end;
        }
    }
    return rows;
}

}  // namespace

void MarkdownView::setText(const std::string& markdown) {
    blocks_ = parseMarkdown(markdown);
    laid_.clear();
    laidWidth_ = -1.0f;
}

float MarkdownView::measure(const std::string& text, const MdSpan& style, const FontSet& fonts, float baseSize, bool forceMedium) const {
    const Pick pick = pickFont(style, fonts, baseSize, forceMedium);
    ImGui::PushFont(pick.font, pick.base);
    const float w = ImGui::CalcTextSize(text.c_str(), text.c_str() + text.size()).x;
    ImGui::PopFont();
    return w;
}

void MarkdownView::drawFlow(const Flow& flow, ImVec2 origin, float lineHeight, float baseSize, bool forceMedium, std::uint32_t color,
                            const Theme& th, const FontSet& fonts) const {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float dpi = ImGui::GetStyle().FontScaleDpi;
    for (const FlowWord& word : flow.words) {
        const Pick pick = pickFont(word.style, fonts, baseSize, forceMedium);
        ImGui::PushFont(pick.font, pick.base);
        const float px = ImGui::GetFontSize();
        ImGui::PopFont();
        const float y = origin.y + static_cast<float>(word.line) * lineHeight;
        const float x = origin.x + word.x;
        if (word.style.code) {
            const float boxH = std::min(lineHeight, px * 1.45f);
            const float top = y + (lineHeight - boxH) * 0.5f;
            const float pad = 4.0f * dpi;
            dl->AddRectFilled(ImVec2(x - pad, top), ImVec2(x + word.width + pad, top + boxH), toU32(th.surface), 4.0f * dpi);
            dl->AddRect(ImVec2(x - pad, top), ImVec2(x + word.width + pad, top + boxH), toU32(th.border), 4.0f * dpi);
        }
        const std::uint32_t textColor = word.style.italic && !word.style.code ? th.muted : color;
        dl->AddText(pick.font, px, ImVec2(x, y + (lineHeight - px) * 0.5f), toU32(textColor), word.text.c_str(),
                    word.text.c_str() + word.text.size());
    }
}

void MarkdownView::drawTable(const MdBlock& block, float width, const Theme& th, const FontSet& fonts, float scale) {
    const int columns = std::max(1, block.columns);
    constexpr ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuterH |
                                      ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_PadOuterX;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(10.0f * scale, 6.0f * scale));
    if (ImGui::BeginTable("##mdtable", columns, flags, ImVec2(width, 0.0f))) {
        ++tablesDrawn_;
        for (int c = 0; c < columns; ++c) {   // wider columns for longer text
            std::size_t longest = 4;
            for (const auto& row : block.rows) longest = std::max(longest, plainText(row[static_cast<std::size_t>(c)]).size());
            ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch, static_cast<float>(std::min<std::size_t>(longest, 100)));
        }
        const float lineHeight = std::floor(kBodySize * 1.5f * scale);
        for (std::size_t r = 0; r < block.rows.size(); ++r) {
            ImGui::TableNextRow();
            const bool header = r == 0;
            if (header) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, toU32(th.surface));
            for (int c = 0; c < columns; ++c) {
                if (!ImGui::TableSetColumnIndex(c)) continue;
                std::vector<MdSpan> spans = block.rows[r][static_cast<std::size_t>(c)];
                if (header)
                    for (MdSpan& s : spans) s.bold = true;
                const float avail = std::max(20.0f, ImGui::GetContentRegionAvail().x);
                const Flow flow = layoutFlow(spans, avail, [&](const std::string& t, const MdSpan& s) {
                    return measure(t, s, fonts, kBodySize, false);
                });
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(avail, std::max(1, flow.lines) * lineHeight));
                drawFlow(flow, at, lineHeight, kBodySize, false, header ? th.muted : th.text, th, fonts);
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

void MarkdownView::draw(const Theme& th, const FontSet& fonts, float scale, float maxWidth) {
    tablesDrawn_ = 0;
    if (!(scale > 0.0f)) scale = 1.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float width = std::max(80.0f, std::min(maxWidth, avail));
    const float baseX = ImGui::GetCursorPosX();
    const float offsetX = std::max(0.0f, (avail - width) * 0.5f);
    if (laid_.size() != blocks_.size() || laidWidth_ != width || laidScale_ != scale) {
        laid_.assign(blocks_.size(), Laid{});
        laidWidth_ = width;
        laidScale_ = scale;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bodyLine = std::floor(kBodySize * 1.55f * scale);
    auto measureBody = [&](float size, bool medium) {
        return [this, &fonts, size, medium](const std::string& t, const MdSpan& s) { return measure(t, s, fonts, size, medium); };
    };

    for (std::size_t i = 0; i < blocks_.size(); ++i) {
        const MdBlock& block = blocks_[i];
        Laid& laid = laid_[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::SetCursorPosX(baseX + offsetX);
        const bool nextIsList = i + 1 < blocks_.size() && (blocks_[i + 1].kind == MdBlock::Kind::Bullet || blocks_[i + 1].kind == MdBlock::Kind::Numbered);
        switch (block.kind) {
            case MdBlock::Kind::Heading: {
                const float size = headingSize(block.level);
                const float lh = std::floor(size * 1.3f * scale);
                const float top = (i == 0 ? 0.0f : (block.level == 1 ? 24.0f : block.level == 2 ? 30.0f : 20.0f)) * scale;
                const float bottom = (block.level <= 2 ? 10.0f : 6.0f) * scale;
                if (!laid.ready) {
                    laid.flow = layoutFlow(block.spans, width, measureBody(size, true));
                    laid.height = top + std::max(1, laid.flow.lines) * lh + bottom;
                    laid.ready = true;
                }
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(width, laid.height));
                if (ImGui::IsRectVisible(p, ImVec2(p.x + width, p.y + laid.height))) {
                    drawFlow(laid.flow, ImVec2(p.x, p.y + top), lh, size, true, th.text, th, fonts);
                    if (block.level == 2) {
                        const float y = p.y + laid.height - 3.0f * scale;
                        dl->AddLine(ImVec2(p.x, y), ImVec2(p.x + width, y), toU32(th.border));
                    }
                }
                break;
            }
            case MdBlock::Kind::Paragraph:
            case MdBlock::Kind::Quote: {
                const bool quote = block.kind == MdBlock::Kind::Quote;
                const float indent = quote ? 16.0f * scale : 0.0f;
                if (!laid.ready) {
                    laid.flow = layoutFlow(block.spans, width - indent, measureBody(kBodySize, false));
                    laid.height = std::max(1, laid.flow.lines) * bodyLine + 12.0f * scale;
                    laid.ready = true;
                }
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(width, laid.height));
                if (ImGui::IsRectVisible(p, ImVec2(p.x + width, p.y + laid.height))) {
                    if (quote) dl->AddRectFilled(p, ImVec2(p.x + 2.0f * scale, p.y + laid.height - 12.0f * scale), toU32(th.faint));
                    drawFlow(laid.flow, ImVec2(p.x + indent, p.y), bodyLine, kBodySize, false, quote ? th.muted : th.text, th, fonts);
                }
                break;
            }
            case MdBlock::Kind::Bullet:
            case MdBlock::Kind::Numbered: {
                const float indent = (22.0f + 20.0f * static_cast<float>(block.level)) * scale;
                const float bottom = (nextIsList ? 3.0f : 12.0f) * scale;
                if (!laid.ready) {
                    laid.flow = layoutFlow(block.spans, width - indent, measureBody(kBodySize, false));
                    laid.height = std::max(1, laid.flow.lines) * bodyLine + bottom;
                    laid.ready = true;
                }
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(width, laid.height));
                if (ImGui::IsRectVisible(p, ImVec2(p.x + width, p.y + laid.height))) {
                    if (block.kind == MdBlock::Kind::Bullet) {
                        dl->AddCircleFilled(ImVec2(p.x + indent - 12.0f * scale, p.y + bodyLine * 0.5f), 2.0f * scale, toU32(th.muted), 12);
                    } else {
                        ImGui::PushFont(fonts.ui, kBodySize);
                        const float mw = ImGui::CalcTextSize(block.marker.c_str()).x;
                        const float px = ImGui::GetFontSize();
                        ImGui::PopFont();
                        dl->AddText(fonts.ui, px, ImVec2(p.x + indent - 8.0f * scale - mw, p.y + (bodyLine - px) * 0.5f), toU32(th.muted),
                                    block.marker.c_str());
                    }
                    drawFlow(laid.flow, ImVec2(p.x + indent, p.y), bodyLine, kBodySize, false, th.text, th, fonts);
                }
                break;
            }
            case MdBlock::Kind::Rule: {
                const float h = 28.0f * scale;
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(width, h));
                dl->AddLine(ImVec2(p.x, p.y + h * 0.5f), ImVec2(p.x + width, p.y + h * 0.5f), toU32(th.border));
                break;
            }
            case MdBlock::Kind::Code: {
                const float pad = 12.0f * scale;
                ImGui::PushFont(fonts.mono, kCodeSize);
                const float cw = ImGui::CalcTextSize("0").x;
                const float px = ImGui::GetFontSize();
                ImFontBaked* baked = ImGui::GetFontBaked();
                ImGui::PopFont();
                const float codeLine = std::floor(kCodeSize * 1.55f * scale);
                if (!laid.ready) {
                    const int maxChars = std::max(8, static_cast<int>((width - 2.0f * pad) / std::max(1.0f, cw)));
                    laid.codeRows = cutCode(block.codeLines, maxChars);
                    laid.height = std::max<std::size_t>(1, laid.codeRows.size()) * codeLine + 2.0f * pad + 12.0f * scale;
                    laid.ready = true;
                }
                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(width, laid.height));
                const float boxH = laid.height - 12.0f * scale;
                if (ImGui::IsRectVisible(p, ImVec2(p.x + width, p.y + laid.height))) {
                    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + boxH), toU32(th.surface), 6.0f * scale);
                    dl->AddRect(p, ImVec2(p.x + width, p.y + boxH), toU32(th.border), 6.0f * scale);
                    const bool query = highlightsAsQuery(block.info);
                    Advance adv;
                    adv.ascii = cw;
                    adv.other = [baked](char32_t cp) { return baked->GetCharAdvance(static_cast<ImWchar>(cp)); };
                    for (std::size_t r = 0; r < laid.codeRows.size(); ++r) {
                        const std::string& row = laid.codeRows[r];
                        const float y = p.y + pad + static_cast<float>(r) * codeLine + (codeLine - px) * 0.5f;
                        if (y + codeLine < ImGui::GetWindowPos().y || y > ImGui::GetWindowPos().y + ImGui::GetWindowHeight()) continue;
                        if (row.empty()) continue;
                        if (query) drawHighlighted(dl, fonts.mono, px, ImVec2(p.x + pad, y), row, 0, row.size(), th, adv);
                        else dl->AddText(fonts.mono, px, ImVec2(p.x + pad, y), toU32(th.text), row.c_str(), row.c_str() + row.size());
                    }
                }
                break;
            }
            case MdBlock::Kind::Table: {
                if (block.rows.empty()) break;
                drawTable(block, width, th, fonts, scale);
                ImGui::SetCursorPosX(baseX + offsetX);
                ImGui::Dummy(ImVec2(width, 14.0f * scale));
                break;
            }
        }
        ImGui::PopID();
    }
}

}  // namespace meradb::gui
