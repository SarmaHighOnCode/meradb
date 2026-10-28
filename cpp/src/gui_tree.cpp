// cpp/src/gui_tree.cpp -- see gui_tree.h.
#include "meradb/gui_tree.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace meradb::gui {

namespace {

using Kind = wb::NodeKey::Kind;

std::string lowerAscii(const std::string& text) {
    std::string out = text;
    for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}

std::string trimmed(const std::string& text) {
    const std::size_t a = text.find_first_not_of(' ');
    if (a == std::string::npos) return std::string();
    return text.substr(a, text.find_last_not_of(' ') - a + 1);
}

TreeEntry makeEntry(const wb::TreeRow& row) {
    TreeEntry e;
    e.key = row.key;
    e.depth = row.depth;
    e.name = nodeName(row.key);
    e.expandable = row.expandable;
    e.expanded = row.expanded;
    if (row.key.kind == Kind::Database) e.current = !row.label.empty() && row.label[0].style.fg == wb::palette::kGreen;
    if (row.key.kind == Kind::Column) {   // label: "name " + type + " PK" / " UQ" + " NN"
        if (row.label.size() > 1) e.type = trimmed(row.label[1].text);
        for (std::size_t i = 2; i < row.label.size(); ++i) {
            const std::string tag = trimmed(row.label[i].text);
            if (!tag.empty()) e.tags.push_back(tag);
        }
    }
    return e;
}

// ---- small drawn glyphs ----

void drawChevron(ImDrawList* dl, ImVec2 centre, float size, bool open, std::uint32_t color) {
    const float h = size * 0.5f;
    ImVec2 a, b, c;
    if (open) {
        a = ImVec2(centre.x - h * 0.7f, centre.y - h * 0.35f);
        b = ImVec2(centre.x, centre.y + h * 0.35f);
        c = ImVec2(centre.x + h * 0.7f, centre.y - h * 0.35f);
    } else {
        a = ImVec2(centre.x - h * 0.35f, centre.y - h * 0.7f);
        b = ImVec2(centre.x + h * 0.35f, centre.y);
        c = ImVec2(centre.x - h * 0.35f, centre.y + h * 0.7f);
    }
    const ImVec2 pts[3] = {a, b, c};
    dl->AddPolyline(pts, 3, toU32(color), std::max(1.0f, size * 0.14f), ImDrawFlags_None);
}

void drawDatabaseGlyph(ImDrawList* dl, ImVec2 centre, float size, std::uint32_t color) {
    const float rx = size * 0.42f, ry = size * 0.16f, height = size * 0.62f;
    const float top = centre.y - height * 0.5f + ry, bottom = centre.y + height * 0.5f - ry;
    const float thick = std::max(1.0f, size * 0.1f);
    const ImU32 col = toU32(color);
    dl->AddEllipse(ImVec2(centre.x, top), ImVec2(rx, ry), col, 0.0f, 20, thick);
    dl->AddLine(ImVec2(centre.x - rx, top), ImVec2(centre.x - rx, bottom), col, thick);
    dl->AddLine(ImVec2(centre.x + rx, top), ImVec2(centre.x + rx, bottom), col, thick);
    dl->PathEllipticalArcTo(ImVec2(centre.x, bottom), ImVec2(rx, ry), 0.0f, 0.0f, 3.14159265f, 12);
    dl->PathStroke(col, thick);
    dl->PathEllipticalArcTo(ImVec2(centre.x, (top + bottom) * 0.5f), ImVec2(rx, ry), 0.0f, 0.0f, 3.14159265f, 12);
    dl->PathStroke(col, thick);
}

void drawTableGlyph(ImDrawList* dl, ImVec2 centre, float size, std::uint32_t color) {
    const float half = size * 0.42f;
    const float thick = std::max(1.0f, size * 0.1f);
    const ImU32 col = toU32(color);
    const ImVec2 lo(centre.x - half, centre.y - half * 0.85f), hi(centre.x + half, centre.y + half * 0.85f);
    dl->AddRect(lo, hi, col, size * 0.12f, thick);
    dl->AddLine(ImVec2(lo.x, lo.y + (hi.y - lo.y) * 0.36f), ImVec2(hi.x, lo.y + (hi.y - lo.y) * 0.36f), col, thick);
    dl->AddLine(ImVec2(lo.x + (hi.x - lo.x) * 0.4f, lo.y + (hi.y - lo.y) * 0.36f), ImVec2(lo.x + (hi.x - lo.x) * 0.4f, hi.y), col, thick);
}

void drawColumnGlyph(ImDrawList* dl, ImVec2 centre, float size, std::uint32_t color) {
    const float thick = std::max(1.0f, size * 0.1f);
    const ImU32 col = toU32(color);
    const ImVec2 lo(centre.x - size * 0.16f, centre.y - size * 0.4f), hi(centre.x + size * 0.16f, centre.y + size * 0.4f);
    dl->AddRect(lo, hi, col, size * 0.08f, thick);
    dl->AddLine(ImVec2(lo.x, centre.y - size * 0.1f), ImVec2(hi.x, centre.y - size * 0.1f), col, thick);
}

}  // namespace

bool matchesFilter(const std::string& name, const std::string& filter) {
    if (filter.empty()) return true;
    return lowerAscii(name).find(lowerAscii(filter)) != std::string::npos;
}

std::string nodeName(const wb::NodeKey& key) {
    switch (key.kind) {
        case Kind::Database: return key.db;
        case Kind::Table: return key.table;
        case Kind::Column: return key.column;
        case Kind::Root: break;
    }
    return std::string();
}

std::vector<std::string> topRowsScripts(const wb::NodeKey& key, const std::string& currentDb) {
    std::vector<std::string> scripts;
    if (key.kind != Kind::Table) return scripts;
    if (key.db != currentDb) scripts.push_back("ISTEMAL " + key.db + ";");
    scripts.push_back("DIKHAO * SE " + key.table + " SIRF 100;");
    return scripts;
}

std::vector<TreeEntry> treeEntries(const std::vector<wb::TreeRow>& nodes, const std::string& filter) {
    std::vector<TreeEntry> out;
    if (filter.empty()) {
        int hideBelow = -1;   // depth of a collapsed node whose descendants are skipped
        for (const wb::TreeRow& row : nodes) {
            if (row.key.kind == Kind::Root) continue;
            if (hideBelow >= 0 && row.depth > hideBelow) continue;
            hideBelow = -1;
            out.push_back(makeEntry(row));
            if (row.expandable && !row.expanded) hideBelow = row.depth;
        }
        return out;
    }
    const std::size_t n = nodes.size();
    std::vector<char> keep(n, 0);
    for (std::size_t i = 0; i < n; ++i)
        if (nodes[i].key.kind != Kind::Root && matchesFilter(nodeName(nodes[i].key), filter)) keep[i] = 1;
    for (std::size_t i = n; i-- > 0;) {   // a kept node keeps its parent
        if (!keep[i] || nodes[i].depth <= 1) continue;
        for (std::size_t j = i; j-- > 0;) {
            if (nodes[j].depth == nodes[i].depth - 1) {
                keep[j] = 1;
                break;
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!keep[i]) continue;
        TreeEntry e = makeEntry(nodes[i]);
        e.expanded = e.expandable && i + 1 < n && keep[i + 1] && nodes[i + 1].depth > nodes[i].depth;
        out.push_back(std::move(e));
    }
    return out;
}

void SchemaTree::setFilter(const std::string& text) {
    std::strncpy(filter_, text.c_str(), sizeof filter_ - 1);
    filter_[sizeof filter_ - 1] = '\0';
}

int SchemaTree::findRow(const wb::NodeKey& key) const {
    const auto& rows = session_.tree().rows();
    for (std::size_t i = 0; i < rows.size(); ++i)
        if (rows[i].key == key) return static_cast<int>(i);
    return -1;
}

int SchemaTree::ensureVisible(const wb::NodeKey& key) {
    wb::NodeKey database;
    database.kind = Kind::Database;
    database.db = key.db;
    wb::NodeKey table;
    table.kind = Kind::Table;
    table.db = key.db;
    table.table = key.table;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const int at = findRow(key);
        if (at >= 0) return at;
        const auto& rows = session_.tree().rows();
        const int d = findRow(database);
        if (d >= 0 && !rows[static_cast<std::size_t>(d)].expanded) {
            session_.tree().toggle(d);
            continue;
        }
        if (key.kind == Kind::Column) {
            const int t = findRow(table);
            if (t >= 0 && !rows[static_cast<std::size_t>(t)].expanded) {
                session_.tree().toggle(t);
                continue;
            }
        }
        break;
    }
    return -1;
}

void SchemaTree::select(const wb::NodeKey& key) {
    selected_ = key;
    const int at = findRow(key);
    if (at >= 0) session_.tree().select(at);
}

bool SchemaTree::activate(const wb::NodeKey& key) {
    const int at = ensureVisible(key);
    if (at < 0) return false;
    selected_ = key;
    session_.tree().select(at);
    editing_([&] { session_.activateTreeRow(at); });
    return true;
}

void SchemaTree::handleKeys() {
    if (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyAlt) return;   // those are the window's history keys
    int step = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) step = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) step = 1;
    if (step != 0 && !entries_.empty()) {
        int at = -1;
        for (std::size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].key == selected_) at = static_cast<int>(i);
        at = at < 0 ? (step > 0 ? 0 : static_cast<int>(entries_.size()) - 1)
                    : std::max(0, std::min(at + step, static_cast<int>(entries_.size()) - 1));
        select(entries_[static_cast<std::size_t>(at)].key);
        scrollToSelected_ = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)) {
        if (selected_.kind != Kind::Root) activate(selected_);
    }
    if (filter_[0] == '\0' && selected_.kind != Kind::Root) {
        const bool left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true);
        const bool right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true);
        if (left || right) {
            select(selected_);
            if (left) session_.tree().collapseOrAscend();
            else session_.tree().expandOrDescend();
            const auto& rows = session_.tree().rows();
            const int at = session_.tree().selected();
            if (at >= 0 && at < static_cast<int>(rows.size())) selected_ = rows[static_cast<std::size_t>(at)].key;
            scrollToSelected_ = true;
        }
    }
}

void SchemaTree::drawMenu() {
    if (openMenu_) {
        ImGui::OpenPopup("##treemenu");
        openMenu_ = false;
    }
    if (!ImGui::BeginPopup("##treemenu")) return;
    const bool isTable = menuKey_.kind == Kind::Table;
    if (ImGui::MenuItem("Top 100 rows dikhao", nullptr, false, isTable)) {
        for (const std::string& script : topRowsScripts(menuKey_, session_.currentDb())) session_.runText(script);
    }
    if (ImGui::MenuItem("Naam editor mein daalo", nullptr, false, menuKey_.kind != Kind::Root)) {
        const std::string name = nodeName(menuKey_);
        editing_([&] { session_.editor().insert(name); });
    }
    if (ImGui::MenuItem("Naam copy karo", nullptr, false, menuKey_.kind != Kind::Root)) ImGui::SetClipboardText(nodeName(menuKey_).c_str());
    ImGui::EndPopup();
}

void SchemaTree::draw(const Theme& th, const FontSet& fonts, float scale) {
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##schemafilter", "dhoondo...", filter_, sizeof filter_);
    ImGui::Spacing();

    const std::string filter = filter_;
    entries_ = filter.empty() ? treeEntries(session_.tree().rows(), filter) : treeEntries(session_.tree().allNodes(), filter);
    hits_.clear();
    const bool noDatabases = session_.tree().rows().size() <= 1;
    if (entries_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, toVec4(th.faint));
        ImGui::TextUnformatted(noDatabases ? "Koi database nahi." : "Kuch nahi mila.");
        ImGui::PopStyleColor();
        return;
    }

    const float rowH = 26.0f * scale;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##schematree", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoNavInputs);
    const bool childHovered = ImGui::IsWindowHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    int selectedAt = -1;
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].key == selected_) selectedAt = static_cast<int>(i);
    if (scrollToSelected_ && selectedAt >= 0) {
        const float top = static_cast<float>(selectedAt) * rowH, bottom = top + rowH;
        const float viewH = ImGui::GetWindowHeight();
        if (top < ImGui::GetScrollY()) ImGui::SetScrollY(top);
        else if (bottom > ImGui::GetScrollY() + viewH) ImGui::SetScrollY(bottom - viewH);
        scrollToSelected_ = false;
    }

    const TreeEntry* clickedRow = nullptr;
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(entries_.size()), rowH);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const TreeEntry& e = entries_[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            const ImVec2 lo = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            ImGui::InvisibleButton("##row", ImVec2(width, rowH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            const ImVec2 hi(lo.x + width, lo.y + rowH);
            const bool hovered = ImGui::IsItemHovered();
            const bool isSelected = e.key == selected_;
            if (isSelected) dl->AddRectFilled(lo, hi, toU32(th.border), 4.0f * scale);
            else if (hovered) dl->AddRectFilled(lo, hi, toU32(th.hover), 4.0f * scale);
            hits_.push_back({e.key, lo, hi});

            const float chevronX = lo.x + 12.0f * scale + static_cast<float>(e.depth - 1) * 16.0f * scale;
            const float midY = lo.y + rowH * 0.5f;
            const bool onChevron = e.expandable && ImGui::GetIO().MousePos.x < chevronX + 8.0f * scale;
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                focused_ = true;
                clickedRow = &e;
                if (onChevron && filter.empty()) {
                    const int at = findRow(e.key);
                    if (at >= 0) session_.tree().toggle(at);
                    select(e.key);
                } else if (ImGui::GetIO().MouseClickedCount[0] >= 2) {
                    activate(e.key);
                } else {
                    select(e.key);
                }
            }
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                focused_ = true;
                clickedRow = &e;
                select(e.key);
                menuKey_ = e.key;
                openMenu_ = true;
            }

            dl->PushClipRect(lo, hi, true);
            if (e.expandable) drawChevron(dl, ImVec2(chevronX, midY), 10.0f * scale, e.expanded, th.muted);
            const ImVec2 iconAt(chevronX + 17.0f * scale, midY);
            const std::uint32_t iconColor = isSelected || e.current ? th.text : th.muted;
            if (e.key.kind == Kind::Database) drawDatabaseGlyph(dl, iconAt, 14.0f * scale, iconColor);
            else if (e.key.kind == Kind::Table) drawTableGlyph(dl, iconAt, 14.0f * scale, iconColor);
            else drawColumnGlyph(dl, iconAt, 14.0f * scale, iconColor);
            float x = iconAt.x + 13.0f * scale;
            ImFont* font = e.current || e.key.kind == Kind::Table ? fonts.uiMedium : fonts.ui;
            ImGui::PushFont(font, kUiFontSize);
            const float lineH = ImGui::GetTextLineHeight();
            const float textY = midY - lineH * 0.5f;
            const float nameW = ImGui::CalcTextSize(e.name.c_str()).x;
            dl->AddText(ImVec2(x, textY), toU32(e.key.kind == Kind::Column ? th.text : th.text), e.name.c_str());
            ImGui::PopFont();
            if (e.key.kind == Kind::Column) {
                ImGui::PushFont(fonts.mono, 12.0f);
                std::string tags;
                for (const std::string& t : e.tags) tags += (tags.empty() ? "" : " ") + t;
                const float typeW = ImGui::CalcTextSize(e.type.c_str()).x;
                const float tagsW = tags.empty() ? 0.0f : ImGui::CalcTextSize(tags.c_str()).x;
                const float gap = 8.0f * scale;
                const float need = typeW + (tags.empty() ? 0.0f : gap + tagsW);
                float tx = hi.x - 10.0f * scale - need;
                tx = std::max(tx, x + nameW + gap);
                const float ty = midY - ImGui::GetTextLineHeight() * 0.5f;
                dl->AddText(ImVec2(tx, ty), toU32(th.muted), e.type.c_str());
                if (!tags.empty()) dl->AddText(ImVec2(tx + typeW + gap, ty), toU32(th.faint), tags.c_str());
                ImGui::PopFont();
            }
            dl->PopClipRect();
            ImGui::PopID();
        }
    }
    clipper.End();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && clickedRow == nullptr && !childHovered) focused_ = false;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && clickedRow == nullptr && childHovered) focused_ = true;
    if (focused_ && !ImGui::GetIO().WantTextInput) handleKeys();
    drawMenu();
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

}  // namespace meradb::gui
