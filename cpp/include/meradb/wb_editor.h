// cpp/include/meradb/wb_editor.h
//
// The workbench's query editor model: a multi-line text buffer with a cursor and a selection (no terminal).
#pragma once
#include "meradb/wb_text.h"
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace meradb::wb {

struct Pos {
    int row = 0;
    int col = 0;  // code points, not bytes or cells
    bool operator==(const Pos& o) const { return row == o.row && col == o.col; }
    bool operator!=(const Pos& o) const { return !(*this == o); }
    bool operator<(const Pos& o) const { return row < o.row || (row == o.row && col < o.col); }
};

enum class Move { Left, Right, Up, Down, Home, End, DocStart, DocEnd, PageUp, PageDown, WordLeft, WordRight };

class TextBuffer {
public:
    TextBuffer() : lines_(1) {}
    void setText(const std::string& text);   // normalises \r\n and \r to \n; cursor to the end; selection cleared
    std::string text() const;                 // lines joined by "\n"
    bool empty() const { return lines_.size() == 1 && lines_[0].empty(); }
    int lineCount() const { return static_cast<int>(lines_.size()); }
    const std::string& line(int row) const { return lines_[static_cast<std::size_t>(row)]; }
    int lineLength(int row) const;                          // code points in the row (cached for the row last used)
    std::size_t byteOffset(int row, int col) const;         // byte offset of code point column `col` (clamped to the end)

    Pos cursor() const { return cursor_; }
    bool hasSelection() const { return anchor_.has_value() && *anchor_ != cursor_; }
    std::pair<Pos, Pos> selection() const;    // ordered (from <= to); only meaningful if hasSelection()
    std::string selectedText() const;         // "" when nothing is selected (tui.py: `selected_text or text`)
    void selectRange(Pos anchor, Pos cursor);
    void selectAll();

    void insert(const std::string& utf8);     // replaces the selection; '\n' splits lines; '\t' -> 4 spaces; other control chars dropped
    void newline() { insert("\n"); }
    void backspace();
    void del();
    void move(Move m, bool extendSelection);
    void moveTo(Pos p, bool extendSelection);

    // The text of the selection if any, else the whole text (what F5 / F6 run).
    std::string runnableText() const { return hasSelection() ? selectedText() : text(); }

private:
    std::vector<std::string> lines_;
    Pos cursor_;
    std::optional<Pos> anchor_;
    int desiredCol_ = 0;  // the column Up / Down try to return to
    // Derived data of the one row last worked on (the cursor's row while typing), so a keystroke on a very long line
    // does not rescan it: its length in code points, whether its UTF-8 is well formed, and one known
    // (column, byte offset) pair to walk from. Any edit that is not updated in place resets it. Not thread safe.
    struct RowCache {
        int row = -1;
        int count = -1;  // -1: not computed yet
        bool wellFormed = false;
        int col = 0;
        std::size_t byte = 0;
    };
    mutable RowCache cache_;
    void fillCache(int row) const;
    void clampPair(int row) const;
    void deleteSelection();
    Pos clampPos(Pos p) const;
};

// ---- what the view draws ----
int gutterWidth(int lineCount);                          // digits of lineCount (min 2) + 1
std::string gutterText(int row, int lineCount);          // 1-based row number, right aligned, plus a space
int displayColumn(const std::string& line, int cpCol);   // terminal cells before code point column cpCol
// One row of the editor body: highlighted, the selection on a lighter background, and (when `focused`) the cursor
// as an inverse cell (a space past the end of the line).
Line editorRowLine(const TextBuffer& buffer, int row, bool focused);
// The same row restricted to terminal cells [skipCells, skipCells + takeCells), exactly what
// clipLine(editorRowLine(...), skipCells, takeCells) gives (for text without combining marks split from their
// letter at the window edge), but only the visible part is turned into styled text. This is what the view
// should draw: on a row of megabytes it stays fast, while editorRowLine builds every character of the row.
Line editorRowWindow(const TextBuffer& buffer, int row, bool focused, int skipCells, int takeCells);

}  // namespace meradb::wb
