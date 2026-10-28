// cpp/src/gui_grid.cpp -- see gui_grid.h.
#include "meradb/gui_grid.h"
#include <algorithm>
#include <cstdio>

namespace meradb::gui {

namespace {

int characters(const std::string& text) {
    int count = 0;
    for (unsigned char ch : text) count += (ch & 0xC0) != 0x80 ? 1 : 0;
    return count;
}

// The first `maxBytes` bytes of `text`, cut so that no character is split.
std::string head(const std::string& text, std::size_t maxBytes, bool* cut = nullptr) {
    if (text.size() <= maxBytes) {
        if (cut != nullptr) *cut = false;
        return text;
    }
    std::size_t end = maxBytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    if (cut != nullptr) *cut = true;
    return text.substr(0, end);
}

}  // namespace

std::string cellClipboardText(const wb::ResultTable& table, int row, int col) {
    if (row < 0 || col < 0 || row >= static_cast<int>(table.rows.size())) return std::string();
    const auto& cells = table.rows[static_cast<std::size_t>(row)];
    if (col >= static_cast<int>(cells.size())) return std::string();
    return cells[static_cast<std::size_t>(col)].text;
}

std::string rowClipboardText(const wb::ResultTable& table, int row) {
    if (row < 0 || row >= static_cast<int>(table.rows.size())) return std::string();
    std::string out;
    const auto& cells = table.rows[static_cast<std::size_t>(row)];
    for (std::size_t i = 0; i < cells.size(); ++i) {
        if (i > 0) out += '\t';
        out += cells[i].text;
    }
    return out;
}

std::vector<int> initialColumnChars(const wb::ResultTable& table) {
    std::vector<int> chars;
    for (const std::string& name : table.columns) chars.push_back(characters(name));
    const std::size_t sample = std::min<std::size_t>(table.rows.size(), 1000);
    for (std::size_t r = 0; r < sample; ++r) {
        const auto& cells = table.rows[r];
        for (std::size_t c = 0; c < cells.size() && c < chars.size(); ++c)
            chars[c] = std::max(chars[c], characters(head(cells[c].text, 256)));
    }
    for (int& n : chars) n = std::max(4, std::min(n, 60));
    return chars;
}

std::uint32_t colorForCell(const Theme& theme, wb::CellKind kind) {
    switch (kind) {
        case wb::CellKind::Null: return theme.faint;
        case wb::CellKind::True: return theme.success;
        case wb::CellKind::False: return theme.error;
        case wb::CellKind::Number: return theme.number;
        case wb::CellKind::Plain: break;
    }
    return theme.text;
}

std::string rowsSummary(std::size_t rows, double ms) {
    char buffer[64];
    if (ms < 0.0) std::snprintf(buffer, sizeof buffer, "%zu row(s)", rows);
    else std::snprintf(buffer, sizeof buffer, "%zu row(s), %.1f ms", rows, ms);
    return buffer;
}

bool ResultsGrid::copy(const wb::ResultTable& table, bool wholeRow) const {
    if (!hasSelection() || row_ >= static_cast<int>(table.rows.size())) return false;
    const std::string text = wholeRow ? rowClipboardText(table, row_) : cellClipboardText(table, row_, col_);
    ImGui::SetClipboardText(text.c_str());
    return true;
}

void ResultsGrid::draw(const wb::ResultTable& table, int version, const Theme& th, const FontSet& fonts, float scale) {
    if (version != version_ || chars_.size() != table.columns.size()) {
        version_ = version;
        row_ = col_ = -1;
        chars_ = initialColumnChars(table);
    }
    hits_.clear();
    const int columns = static_cast<int>(table.columns.size());
    const int rows = static_cast<int>(table.rows.size());

    ImGui::PushFont(fonts.mono, kMonoFontSize);
    const float cw = ImGui::CalcTextSize("0").x;
    const float lh = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    const float rowH = lh + 8.0f * scale;
    int digits = 1;
    for (int n = rows; n >= 10; n /= 10) ++digits;
    const float numberW = static_cast<float>(std::max(digits, 2)) * cw + 22.0f * scale;

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f * scale, 0.0f));
    const ImGuiTableFlags flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_BordersOuterH |
                                  ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings;
    ImGui::PushID(version);
    if (ImGui::BeginTable("##grid", columns + 1, flags, ImVec2(0.0f, 0.0f))) {
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, numberW);
        for (int c = 0; c < columns; ++c) {
            const float width = static_cast<float>(chars_[static_cast<std::size_t>(c)]) * cw + 18.0f * scale;
            ImGui::TableSetupColumn(table.columns[static_cast<std::size_t>(c)].c_str(), ImGuiTableColumnFlags_WidthFixed, width);
        }
        ImGui::PushFont(fonts.uiMedium, kUiFontSize);
        ImGui::PushStyleColor(ImGuiCol_Text, toVec4(th.muted));
        ImGui::TableHeadersRow();
        ImGui::PopStyleColor();
        ImGui::PopFont();

        ImGui::PushFont(fonts.mono, kMonoFontSize);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGuiListClipper clipper;
        clipper.Begin(rows, rowH);
        while (clipper.Step()) {
            for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
                if (r == row_) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, toU32(th.hover, 0.5f));
                ImGui::TableSetColumnIndex(0);
                {
                    const std::string number = std::to_string(r + 1);
                    const ImVec2 lo = ImGui::GetCursorScreenPos();
                    const float width = ImGui::GetContentRegionAvail().x;
                    const float y = lo.y + (rowH - lh) * 0.5f;
                    dl->AddText(ImVec2(lo.x + width - ImGui::CalcTextSize(number.c_str()).x, y), toU32(th.faint), number.c_str());
                }
                const auto& cells = table.rows[static_cast<std::size_t>(r)];
                for (int c = 0; c < columns; ++c) {
                    if (!ImGui::TableSetColumnIndex(c + 1)) continue;
                    if (c >= static_cast<int>(cells.size())) continue;
                    const wb::Cell& cell = cells[static_cast<std::size_t>(c)];
                    ImGui::PushID(r);
                    ImGui::PushID(c);
                    const ImVec2 lo = ImGui::GetCursorScreenPos();
                    const float width = ImGui::GetContentRegionAvail().x;
                    if (ImGui::Selectable("##cell", false, ImGuiSelectableFlags_None, ImVec2(0.0f, rowH))) {
                        row_ = r;
                        col_ = c;
                    }
                    const bool hovered = ImGui::IsItemHovered();
                    const ImVec2 hi(lo.x + width, lo.y + rowH);
                    hits_.push_back({r, c, lo, hi});
                    if (r == row_ && c == col_) {
                        ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, toU32(th.focus, 0.28f));
                        dl->AddRect(ImVec2(lo.x - 4.0f * scale, lo.y), ImVec2(hi.x + 4.0f * scale, hi.y), toU32(th.focus, 0.9f));
                    }
                    bool cut = false;
                    const std::string shown = head(cell.text, 400, &cut);
                    const float textW = ImGui::CalcTextSize(shown.c_str()).x;
                    float x = lo.x;
                    if (wb::cellRightAligned(cell.kind) && textW < width) x = lo.x + width - textW;
                    const float y = lo.y + (rowH - lh) * 0.5f;
                    dl->PushClipRect(ImVec2(lo.x, lo.y), ImVec2(hi.x, hi.y), true);
                    dl->AddText(ImVec2(x, y), toU32(colorForCell(th, cell.kind)), shown.c_str());
                    dl->PopClipRect();
                    if (hovered && (cut || textW > width)) {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos(480.0f * scale);
                        const std::string full = head(cell.text, 4000, nullptr);
                        ImGui::TextUnformatted(full.c_str());
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                    ImGui::PopID();
                    ImGui::PopID();
                }
            }
        }
        ImGui::PopFont();
        ImGui::EndTable();
    }
    ImGui::PopID();
    ImGui::PopStyleVar();
}

}  // namespace meradb::gui
