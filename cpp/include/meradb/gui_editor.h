// cpp/include/meradb/gui_editor.h -- the query editor of the desktop window: a widget drawn with ImDrawList over
// wb::TextBuffer (the model the terminal workbench uses), plus the pure helpers behind it (column <-> pixel mapping,
// hit testing, word bounds, the undo stack) so they can be tested without a window.
#pragma once
#include "meradb/gui_theme.h"
#include "meradb/highlight.h"
#include "meradb/wb_editor.h"
#include "imgui.h"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace meradb::gui {

// ---- text geometry (all in pixels of one line, x = 0 at the first character) ----

// How wide a code point is. `ascii` is the monospace advance; `other` (optional) answers for the rest.
struct Advance {
    float ascii = 8.0f;
    std::function<float(char32_t)> other;
    float of(char32_t cp) const { return cp < 0x80 || !other ? ascii : other(cp); }
};

// Reads one character starting at `p` (advances it). The length comes from the lead byte, like wb::TextBuffer's
// columns do, and is cut at `end`; a stray byte is one character (U+FFFD).
char32_t decodeUtf8(const char*& p, const char* end);
std::string encodeUtf8(char32_t cp);   // U+FFFD for surrogates and values beyond U+10FFFF

float textWidth(const char* begin, const char* end, const Advance& advance);
float columnX(const std::string& line, int col, const Advance& advance);     // left edge of code point column `col`
int columnAt(const std::string& line, float x, const Advance& advance);       // the column boundary nearest to x

// The part of a line that overlaps the horizontal range [fromX, toX).
struct Slice {
    int firstCol = 0, endCol = 0;
    std::size_t firstByte = 0, endByte = 0;
    float firstX = 0.0f;   // left edge of firstCol
};
Slice visibleSlice(const std::string& line, float fromX, float toX, const Advance& advance);

// [begin, end) columns of the word (or run of blanks, or run of punctuation) at `col`; empty line: (0, 0).
std::pair<int, int> wordBounds(const std::string& line, int col);

// x and y are relative to the top-left of the first line's text.
wb::Pos hitTest(const wb::TextBuffer& buffer, float x, float y, float lineHeight, const Advance& advance);

std::uint32_t colorForKind(const Theme& theme, highlight::Kind kind);   // 0xRRGGBB

// Draws line[from, to) (byte offsets on character boundaries) with syntax colours; `pos` is where line[from] goes.
void drawHighlighted(ImDrawList* list, ImFont* font, float fontSize, ImVec2 pos, const std::string& line, std::size_t from,
                     std::size_t to, const Theme& theme, const Advance& advance);

// ---- undo ----

struct EditorSnapshot {
    std::string text;
    wb::Pos cursor;
    std::optional<wb::Pos> anchor;   // set when a selection was open
};
EditorSnapshot takeSnapshot(const wb::TextBuffer& buffer);
void restoreSnapshot(wb::TextBuffer& buffer, const EditorSnapshot& snapshot);

// Steps of text + cursor snapshots. Typing (characters, Backspace, Delete) joins the previous step until
// kTypingPause seconds pass or something else happens (any other edit, a cursor movement: breakMerge()).
class UndoStack {
public:
    static constexpr double kTypingPause = 1.0;
    static constexpr std::size_t kMaxSteps = 200;
    static constexpr std::size_t kMaxBytes = 32u * 1024u * 1024u;

    // Call before an edit. `before` is only called when the edit starts a new step. Returns true then.
    bool record(bool typing, double now, const std::function<EditorSnapshot()>& before);
    void cancelLast();                    // the edit that record() started changed nothing
    void breakMerge() { merging_ = false; }
    std::optional<EditorSnapshot> undo(EditorSnapshot current);
    std::optional<EditorSnapshot> redo(EditorSnapshot current);
    void clear();
    std::size_t undoSteps() const { return undo_.size(); }
    std::size_t redoSteps() const { return redo_.size(); }

private:
    void trim();
    std::vector<EditorSnapshot> undo_, redo_;
    bool merging_ = false;   // the last edit was typing and may take more
    double lastTime_ = 0.0;
};

// ---- the widget ----

class EditorWidget {
public:
    explicit EditorWidget(wb::TextBuffer& buffer) : buffer_(buffer) {}

    // Where things were the last time draw() ran (screen pixels), for hit testing in tests.
    struct Layout {
        ImVec2 origin;            // top-left of the first line's text (scroll applied)
        ImVec2 viewMin, viewMax;  // the visible area
        float lineHeight = 0.0f, charWidth = 0.0f, gutter = 0.0f, inset = 0.0f;
        int firstRow = 0, lastRow = -1;   // rows drawn
    };

    // Draws the editor into the current window, `size` big (0 = fill). `mono` is the font of the text.
    void draw(const char* id, ImVec2 size, const Theme& theme, ImFont* mono, float monoSize, float scale);

    bool focused() const { return focused_; }
    void setFocused(bool focused) { focused_ = focused; if (focused) lastInput_ = ImGui::GetTime(); }
    // Runs `edit` (which changes the buffer behind the widget's back, e.g. history or a column name from the tree)
    // as one undo step, and keeps the caret in view.
    void external(const std::function<void()>& edit);
    UndoStack& undo() { return undo_; }
    const Layout& layout() const { return layout_; }
    float scrollX() const { return scrollX_; }
    float scrollY() const { return scrollY_; }

private:
    void edit(bool typing, const std::function<void()>& change);
    void handleKeys();
    void performUndo(bool redo);

    wb::TextBuffer& buffer_;
    UndoStack undo_;
    bool focused_ = false;
    bool dragging_ = false;
    bool caretMoved_ = false;
    int settleFrames_ = 0;
    double lastInput_ = 0.0;
    float scrollX_ = 0.0f, scrollY_ = 0.0f;
    // the widest line, found again only when the text changed
    unsigned textStamp_ = 1, widthStamp_ = 0;
    int widthLineCount_ = -1, widthRow_ = -1;
    std::size_t widthRowSize_ = 0;
    int maxColumns_ = 0;
    Layout layout_;
    double now_ = 0.0;
    int pageRows_ = 10;
};

}  // namespace meradb::gui
