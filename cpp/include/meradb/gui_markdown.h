// cpp/include/meradb/gui_markdown.h -- a small Markdown reader and renderer for the Madad window.
//
// The subset docs/LANGUAGE.md uses: # .. ###### headings, paragraphs, "- " / "* " / "+ " bullets and "1. " numbered
// items (nested by indentation), fenced ``` code blocks (an unterminated fence runs to the end), | tables with a
// |---| divider line, "> " quotes, horizontal rules. Inline: **bold**, *italic*, `code`.
//
// parseMarkdown() and layoutFlow() are pure (no Dear ImGui) and linear in the size of the input: every loop advances,
// so no input can hang them. MarkdownView draws the result with Dear ImGui.
#pragma once
#include "meradb/gui_fonts.h"
#include "meradb/gui_theme.h"
#include "imgui.h"
#include <functional>
#include <string>
#include <vector>

namespace meradb::gui {

struct MdSpan {
    std::string text;
    bool bold = false, italic = false, code = false;
};

struct MdBlock {
    enum class Kind { Heading, Paragraph, Bullet, Numbered, Code, Table, Rule, Quote };
    Kind kind = Kind::Paragraph;
    int level = 0;                                      // Heading: 1..6; Bullet / Numbered: nesting depth 0..kMaxDepth
    std::string marker;                                 // Numbered: "1." (the number as written)
    std::vector<MdSpan> spans;                          // Heading, Paragraph, Bullet, Numbered, Quote
    std::string info;                                   // Code: the word after the opening ```
    std::vector<std::string> codeLines;                 // Code: verbatim
    std::vector<std::vector<std::vector<MdSpan>>> rows; // Table: rows[0] is the header; every row has `columns` cells
    int columns = 0;
    static constexpr int kMaxDepth = 6;
    static constexpr int kMaxColumns = 24;
};

std::vector<MdBlock> parseMarkdown(const std::string& markdown);
std::vector<MdSpan> parseInline(const std::string& text);
std::string plainText(const std::vector<MdSpan>& spans);   // the spans without their markup

// ---- flowing inline text ----
struct FlowWord {
    std::string text;
    MdSpan style;      // the flags of the span the word came from (its text is empty)
    float x = 0.0f;    // left edge, from the left edge of the flow
    float width = 0.0f;
    int line = 0;
};
struct Flow {
    std::vector<FlowWord> words;
    int lines = 0;     // at least 1 for a non-empty text, 0 for none
};
using MeasureFn = std::function<float(const std::string& text, const MdSpan& style)>;
// Places the words of `spans` in lines of at most `width` pixels. A word wider than the line is cut between characters.
// A short `code` span stays in one piece (spaces inside it included); a long one breaks at its spaces.
Flow layoutFlow(const std::vector<MdSpan>& spans, float width, const MeasureFn& measure);

// ---- drawing ----
class MarkdownView {
public:
    void setText(const std::string& markdown);
    const std::vector<MdBlock>& blocks() const { return blocks_; }
    // At the cursor of the current window: the whole document, in a column of at most `maxWidth` pixels centred in the
    // available width. Only the blocks in view are drawn.
    void draw(const Theme& theme, const FontSet& fonts, float scale, float maxWidth);
    int tablesDrawn() const { return tablesDrawn_; }   // in the last draw(), for the tests

private:
    struct Laid {
        bool ready = false;
        Flow flow;                          // not tables, not code
        std::vector<std::string> codeRows;  // Code: the lines cut to the width
        float height = 0.0f;
    };
    float measure(const std::string& text, const MdSpan& style, const FontSet& fonts, float baseSize, bool forceMedium) const;
    void drawFlow(const Flow& flow, ImVec2 origin, float lineHeight, float baseSize, bool forceMedium, std::uint32_t color,
                  const Theme& theme, const FontSet& fonts) const;
    void drawTable(const MdBlock& block, float width, const Theme& theme, const FontSet& fonts, float scale);

    std::vector<MdBlock> blocks_;
    std::vector<Laid> laid_;
    float laidWidth_ = -1.0f, laidScale_ = 0.0f;
    int tablesDrawn_ = 0;
};

}  // namespace meradb::gui
