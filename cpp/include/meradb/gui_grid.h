// cpp/include/meradb/gui_grid.h -- the results grid of the desktop window: an ImGui table over wb::ResultTable with a
// list clipper (only the rows in view are drawn, so 100,000 rows stay smooth).
#pragma once
#include "meradb/gui_fonts.h"
#include "meradb/gui_theme.h"
#include "meradb/wb_text.h"
#include "imgui.h"
#include <cstdint>
#include <string>
#include <vector>

namespace meradb::gui {

// What Ctrl+C and Ctrl+Shift+C put on the clipboard.
std::string cellClipboardText(const wb::ResultTable& table, int row, int col);   // "" for a cell that does not exist
std::string rowClipboardText(const wb::ResultTable& table, int row);             // the cells separated by tabs
// Column widths to start with, in characters: the widest of the header and the first rows, between 4 and 60.
std::vector<int> initialColumnChars(const wb::ResultTable& table);
std::uint32_t colorForCell(const Theme& theme, wb::CellKind kind);                // 0xRRGGBB
// "N row(s), X ms"; "N row(s)" when the time is unknown (negative).
std::string rowsSummary(std::size_t rows, double ms);

class ResultsGrid {
public:
    // `version` changes when a new table arrives (the selection and the column widths start again).
    void draw(const wb::ResultTable& table, int version, const Theme& theme, const FontSet& fonts, float scale);

    int selectedRow() const { return row_; }
    int selectedColumn() const { return col_; }
    void select(int row, int col) { row_ = row; col_ = col; }
    bool hasSelection() const { return row_ >= 0 && col_ >= 0; }
    // Ctrl+C / Ctrl+Shift+C on the selected cell; true when something was copied.
    bool copy(const wb::ResultTable& table, bool wholeRow) const;

    struct CellHit {
        int row, col;
        ImVec2 min, max;
    };
    const std::vector<CellHit>& cellHits() const { return hits_; }   // the cells drawn in the last frame, screen pixels

private:
    int row_ = -1, col_ = -1;
    int version_ = -1;
    std::vector<int> chars_;
    std::vector<CellHit> hits_;
    bool hScroll_ = false;   // the table showed a horizontal scroll bar in the last frame
};

}  // namespace meradb::gui
