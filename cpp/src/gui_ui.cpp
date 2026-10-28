// cpp/src/gui_ui.cpp -- see gui_ui.h.
#include "meradb/gui_ui.h"
#include "meradb/protocol.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace meradb::gui {

namespace {

constexpr float kPi = 3.14159265f;
constexpr ImGuiWindowFlags kPanelFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse;

std::uint32_t colorForLog(const Theme& theme, wb::LogKind kind) {
    switch (kind) {
        case wb::LogKind::Error: return theme.error;
        case wb::LogKind::Message:
        case wb::LogKind::Connected: return theme.success;
        case wb::LogKind::Dim: return theme.muted;
        case wb::LogKind::Warn: return theme.warning;
        case wb::LogKind::Plain:
        case wb::LogKind::Bold:
        case wb::LogKind::Echo: break;
    }
    return theme.text;
}

}  // namespace

std::string ellipsizeMiddle(const std::string& text, float maxWidth, const std::function<float(const std::string&)>& measure) {
    if (measure(text) <= maxWidth) return text;
    std::vector<std::size_t> starts;   // byte offset of every code point
    for (std::size_t i = 0; i < text.size(); ++i)
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) starts.push_back(i);
    const std::size_t n = starts.size();
    for (std::size_t keep = n > 0 ? n - 1 : 0; keep >= 1; --keep) {
        const std::size_t head = (keep + 1) / 2;
        const std::size_t tail = keep / 2;
        std::string candidate = text.substr(0, starts[head]) + "...";
        if (tail > 0) candidate += text.substr(starts[n - tail]);
        if (measure(candidate) <= maxWidth) return candidate;
    }
    return "...";
}

GuiUi::GuiUi(wb::Session& session, const FontSet& fonts, ThemeMode mode)
    : session_(session), fonts_(fonts), mode_(mode), appliedMode_(mode) {}

void GuiUi::prepare(float scale) {
    if (!(scale > 0.0f)) scale = 1.0f;
    if (scale == appliedScale_ && mode_ == appliedMode_) return;
    applyTheme(ImGui::GetStyle(), theme(), scale);
    scale_ = scale;
    appliedScale_ = scale;
    appliedMode_ = mode_;
}

bool GuiUi::beginPanel(const char* id, const Rect& rect, std::uint32_t background, ImVec2 padding) {
    ImGui::SetNextWindowPos(rect.pos);
    ImGui::SetNextWindowSize(rect.size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, toVec4(background));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const bool open = ImGui::Begin(id, nullptr, kPanelFlags);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    return open;
}

void GuiUi::handleShortcuts() {
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Q)) session_.requestQuit();
}

void GuiUi::draw() {
    if (scale_ <= 0.0f) scale_ = 1.0f;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float topH = std::min(px(kTopBarHeight), display.y);
    const float statusH = std::min(px(kStatusBarHeight), std::max(0.0f, display.y - topH));
    const float bodyH = std::max(0.0f, display.y - topH - statusH);
    const float schemaW = std::min(px(kSchemaWidth), display.x * 0.5f);
    const float rightW = std::max(0.0f, display.x - schemaW);
    const float queryH = std::floor(bodyH * 0.42f);

    ImGui::PushFont(fonts_.ui, kUiFontSize);
    handleShortcuts();
    drawTopBar({ImVec2(0, 0), ImVec2(display.x, topH)});
    drawSchema({ImVec2(0, topH), ImVec2(schemaW, bodyH)});
    drawQuery({ImVec2(schemaW, topH), ImVec2(rightW, queryH)});
    drawResults({ImVec2(schemaW, topH + queryH), ImVec2(rightW, bodyH - queryH)});
    drawStatusBar({ImVec2(0, display.y - statusH), ImVec2(display.x, statusH)});
    ImGui::PopFont();
}

void GuiUi::drawTopBar(const Rect& rect) {
    const Theme& th = theme();
    if (beginPanel("##topbar", rect, th.background, ImVec2(0, 0))) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = rect.pos;
        const float width = rect.size.x, height = rect.size.y;
        dl->AddLine(ImVec2(origin.x, origin.y + height - 1.0f), ImVec2(origin.x + width, origin.y + height - 1.0f),
                    toU32(th.border));

        // ---- right side: Connect, Madad, theme (stubs in this batch except the theme switch) ----
        const float buttonH = px(32.0f);
        const float buttonY = origin.y + (height - buttonH) * 0.5f;
        float right = origin.x + width - px(16.0f);
        ImGui::PushFont(fonts_.uiMedium, kUiFontSize);
        const char* labels[2] = {"Madad", "Connect"};
        float labelWidths[2];
        for (int i = 0; i < 2; ++i) labelWidths[i] = ImGui::CalcTextSize(labels[i]).x + px(28.0f);
        ImGui::PopFont();

        ImGui::SetCursorScreenPos(ImVec2(right - buttonH, buttonY));
        if (ImGui::Button("##theme", ImVec2(buttonH, buttonH))) mode_ = otherMode(mode_);
        ImGui::SetItemTooltip("%s", "Light / dark");
        {
            const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
            const ImVec2 centre((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
            const float radius = px(7.0f);
            dl->AddCircle(centre, radius, toU32(th.text), 32, px(1.5f));
            dl->PathArcTo(centre, radius - px(1.0f), kPi * 0.5f, kPi * 1.5f, 16);
            dl->PathFillConvex(toU32(th.text));
        }
        right -= buttonH + px(8.0f);
        ImGui::PushFont(fonts_.uiMedium, kUiFontSize);
        for (int i = 0; i < 2; ++i) {
            ImGui::SetCursorScreenPos(ImVec2(right - labelWidths[i], buttonY));
            (void)ImGui::Button(labels[i], ImVec2(labelWidths[i], buttonH));   // opened in a later batch
            right -= labelWidths[i] + px(8.0f);
        }
        ImGui::PopFont();

        // ---- left side: mark, then the breadcrumb  MeraDB / <connection> / <database> ----
        const float lineH = ImGui::GetTextLineHeight();
        const float textY = origin.y + (height - lineH) * 0.5f;
        float x = origin.x + px(16.0f);
        const float markSize = px(20.0f);
        const ImVec2 markPos(x, origin.y + (height - markSize) * 0.5f);
        dl->AddRectFilled(markPos, ImVec2(markPos.x + markSize, markPos.y + markSize), toU32(th.text), px(5.0f));
        {
            const float unit = markSize / 20.0f;
            const ImVec2 points[5] = {ImVec2(markPos.x + 5 * unit, markPos.y + 15 * unit), ImVec2(markPos.x + 5 * unit, markPos.y + 5 * unit),
                                      ImVec2(markPos.x + 10 * unit, markPos.y + 11 * unit), ImVec2(markPos.x + 15 * unit, markPos.y + 5 * unit),
                                      ImVec2(markPos.x + 15 * unit, markPos.y + 15 * unit)};
            dl->AddPolyline(points, 5, toU32(th.background), px(2.0f));
        }
        x += markSize + px(10.0f);

        ImGui::PushFont(fonts_.uiMedium, kUiFontSize);
        dl->AddText(ImVec2(x, textY), toU32(th.text), "MeraDB");
        x += ImGui::CalcTextSize("MeraDB").x;
        ImGui::PopFont();

        const std::string description = session_.description();
        const std::string db = session_.currentDb();
        const float sepWidth = ImGui::CalcTextSize("/").x + px(20.0f);
        const float dbWidth = db.empty() ? 0.0f : sepWidth + ImGui::CalcTextSize(db.c_str()).x;
        const bool inTransaction = session_.inTransaction();
        const char* badge = "TRANSACTION";
        const float badgeWidth = inTransaction ? ImGui::CalcTextSize(badge).x + px(20.0f) : 0.0f;
        const float badgeGap = inTransaction ? px(14.0f) : 0.0f;
        const float limit = right - px(8.0f);
        const float descMax = std::max(px(60.0f), limit - x - sepWidth - dbWidth - badgeGap - badgeWidth);

        auto separator = [&]() {
            dl->AddText(ImVec2(x + px(10.0f), textY), toU32(th.faint), "/");
            x += sepWidth;
        };
        if (!description.empty()) {
            separator();
            const std::string shown = ellipsizeMiddle(description, descMax, [](const std::string& s) {
                return ImGui::CalcTextSize(s.c_str()).x;
            });
            dl->AddText(ImVec2(x, textY), toU32(th.muted), shown.c_str());
            x += ImGui::CalcTextSize(shown.c_str()).x;
        }
        if (!db.empty()) {
            separator();
            dl->AddText(ImVec2(x, textY), toU32(th.text), db.c_str());
            x += ImGui::CalcTextSize(db.c_str()).x;
        }
        if (inTransaction) {
            const ImVec2 lo(x + badgeGap, origin.y + (height - lineH - px(8.0f)) * 0.5f);
            const ImVec2 hi(lo.x + badgeWidth, lo.y + lineH + px(8.0f));
            dl->AddRectFilled(lo, hi, toU32(th.warning, 0.14f), px(4.0f));
            dl->AddRect(lo, hi, toU32(th.warning, 0.6f), px(4.0f));
            dl->AddText(ImVec2(lo.x + px(10.0f), lo.y + px(4.0f)), toU32(th.warning), badge);
            if (ImGui::IsMouseHoveringRect(lo, hi)) ImGui::SetTooltip("%s", "PAKKA se save karo, WAPAS se cancel.");
        }
    }
    ImGui::End();
}

void GuiUi::drawSchema(const Rect& rect) {
    const Theme& th = theme();
    if (beginPanel("##schema", rect, th.surface, ImVec2(px(16.0f), px(12.0f)))) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(ImVec2(rect.pos.x + rect.size.x - 1.0f, rect.pos.y), ImVec2(rect.pos.x + rect.size.x - 1.0f, rect.pos.y + rect.size.y),
                    toU32(th.border));
        ImGui::PushFont(fonts_.uiMedium, kUiFontSize);
        ImGui::TextUnformatted("Schema");
        ImGui::PopFont();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##schemafilter", "dhoondo...", schemaFilter_, sizeof schemaFilter_);

        const auto& rows = session_.tree().rows();
        if (rows.size() <= 1) {
            ImGui::PushStyleColor(ImGuiCol_Text, toVec4(th.faint));
            ImGui::TextUnformatted("Koi database nahi.");
            ImGui::PopStyleColor();
        } else {
            ImGui::BeginChild("##schematree", ImVec2(0, 0), ImGuiChildFlags_None);
            for (std::size_t i = 1; i < rows.size(); ++i) {   // row 0 is the "Databases" root
                const wb::TreeRow& row = rows[i];
                std::string label = row.expandable ? (row.expanded ? "v " : "> ") : "  ";
                label += wb::plainText(row.label);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + px(14.0f) * static_cast<float>(std::max(0, row.depth - 1)));
                const bool isColumn = row.key.kind == wb::NodeKey::Kind::Column;
                ImGui::PushStyleColor(ImGuiCol_Text, toVec4(isColumn ? th.muted : th.text));
                ImGui::TextUnformatted(label.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::EndChild();
        }
    }
    ImGui::End();
}

void GuiUi::drawQuery(const Rect& rect) {
    const Theme& th = theme();
    if (beginPanel("##query", rect, th.background, ImVec2(px(16.0f), px(12.0f)))) {
        ImGui::PushStyleColor(ImGuiCol_Text, toVec4(th.muted));
        ImGui::PushFont(fonts_.uiMedium, kUiFontSize);
        ImGui::TextUnformatted("Query");
        ImGui::PopFont();
        ImGui::PopStyleColor();

        const float actionsH = px(32.0f) + ImGui::GetStyle().ItemSpacing.y;
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float editorH = std::max(px(48.0f), avail.y - actionsH);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, toVec4(th.surface));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(12.0f), px(8.0f)));
        ImGui::BeginChild("##editor", ImVec2(0, editorH), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        {
            ImGui::PushFont(fonts_.mono, kMonoFontSize);
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float lineH = ImGui::GetTextLineHeight();
            const float gutter = ImGui::CalcTextSize("000").x;
            const wb::TextBuffer& buffer = session_.editor();
            if (buffer.empty()) {
                dl->AddText(start, toU32(th.faint), "1");
                dl->AddText(ImVec2(start.x + gutter, start.y), toU32(th.faint),
                            "Yahan query likho, jaise:  DIKHAO * SE students;   (Ctrl+Enter se chalao)");
            } else {
                const int rows = std::min(buffer.lineCount(), 500);
                for (int i = 0; i < rows; ++i) {
                    const float y = start.y + lineH * static_cast<float>(i);
                    if (y > start.y + editorH) break;
                    const std::string number = std::to_string(i + 1);
                    dl->AddText(ImVec2(start.x + gutter - ImGui::CalcTextSize(number.c_str()).x, y), toU32(th.faint), number.c_str());
                    dl->AddText(ImVec2(start.x + gutter + px(12.0f), y), toU32(th.text), buffer.line(i).c_str());
                }
            }
            ImGui::PopFont();
        }
        ImGui::EndChild();

        // ---- actions row ----
        ImGui::PushFont(fonts_.uiMedium, kUiFontSize);
        const ImVec2 runSize(ImGui::CalcTextSize("Chalao").x + px(24.0f) + ImGui::CalcTextSize("  Ctrl+Enter").x, px(32.0f));
        ImGui::PushStyleColor(ImGuiCol_Button, toVec4(th.primary));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toVec4(th.primary, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, toVec4(th.primary, 0.7f));
        ImGui::PushStyleColor(ImGuiCol_Text, toVec4(th.primaryText));
        ImGui::PushStyleColor(ImGuiCol_Border, toVec4(th.primary));
        if (ImGui::Button("Chalao  Ctrl+Enter", runSize)) session_.runEditorText();
        ImGui::PopStyleColor(5);
        ImGui::SameLine();
        if (ImGui::Button("Samjhao", ImVec2(ImGui::CalcTextSize("Samjhao").x + px(24.0f), px(32.0f)))) session_.explainEditorText();
        ImGui::SameLine();
        const float previousW = ImGui::CalcTextSize("Pichli").x + px(24.0f);
        const float nextW = ImGui::CalcTextSize("Agli").x + px(24.0f);
        const char* status = session_.quitting() ? "band ho raha hai..." : (session_.busy() ? "chal raha hai..." : "");
        ImGui::PopFont();
        const float statusW = ImGui::CalcTextSize(status).x;
        const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        const float groupW = previousW + nextW + px(8.0f) * 2 + statusW + (statusW > 0 ? px(16.0f) : 0.0f);
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), rightEdge - groupW));
        ImGui::PushFont(fonts_.uiMedium, kUiFontSize);
        if (ImGui::Button("Pichli", ImVec2(previousW, px(32.0f)))) session_.historyStep(-1);
        ImGui::SameLine();
        if (ImGui::Button("Agli", ImVec2(nextW, px(32.0f)))) session_.historyStep(+1);
        ImGui::PopFont();
        if (statusW > 0) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, toVec4(th.muted));
            ImGui::TextUnformatted(status);
            ImGui::PopStyleColor();
        }
    }
    ImGui::End();
}

void GuiUi::drawResults(const Rect& rect) {
    const Theme& th = theme();
    if (beginPanel("##results", rect, th.background, ImVec2(px(16.0f), px(8.0f)))) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(rect.pos, ImVec2(rect.pos.x + rect.size.x, rect.pos.y), toU32(th.border));
        if (ImGui::BeginTabBar("##resulttabs", ImGuiTabBarFlags_None)) {
            if (ImGui::BeginTabItem("Results")) {
                drawResultsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Log")) {
                drawLogTab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void GuiUi::drawResultsTab() {
    const Theme& th = theme();
    ImGui::Spacing();
    const wb::ResultTable* table = session_.table();
    if (table == nullptr) {
        ImGui::PushStyleColor(ImGuiCol_Text, toVec4(th.faint));
        ImGui::TextUnformatted("Abhi koi result nahi. Query likho aur Chalao dabao.");
        ImGui::PopStyleColor();
        return;
    }
    // The grid arrives in a later batch; until then say how much came back.
    const std::string summary = wb::resultsTitle(table);
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(th.muted));
    ImGui::TextUnformatted(summary.c_str());
    ImGui::PopStyleColor();
}

void GuiUi::rebuildLogCache() {
    const wb::LogBuffer& log = session_.log();
    if (log.revision() == logRevision_ && log.generation() == logGeneration_) return;
    logLines_.clear();
    for (const wb::LogEntry& entry : log.entries())
        for (const wb::Line& line : entry.lines) logLines_.push_back({entry.kind, wb::plainText(line)});
    logRevision_ = log.revision();
    logGeneration_ = log.generation();
}

void GuiUi::drawLogTab() {
    const Theme& th = theme();
    const std::size_t previousRevision = logRevision_;
    rebuildLogCache();
    ImGui::Spacing();
    if (logLines_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, toVec4(th.faint));
        ImGui::TextUnformatted("Log khaali hai.");
        ImGui::PopStyleColor();
        return;
    }
    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    const bool followEnd = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    ImGui::PushFont(fonts_.mono, kMonoFontSize);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(logLines_.size()), ImGui::GetTextLineHeightWithSpacing());
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const LogLine& line = logLines_[static_cast<std::size_t>(i)];
            ImGui::PushStyleColor(ImGuiCol_Text, toVec4(colorForLog(th, line.kind)));
            ImGui::TextUnformatted(line.text.empty() ? " " : line.text.c_str());
            ImGui::PopStyleColor();
        }
    }
    ImGui::PopFont();
    if (followEnd && previousRevision != logRevision_) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

void GuiUi::drawStatusBar(const Rect& rect) {
    const Theme& th = theme();
    if (beginPanel("##status", rect, th.surface, ImVec2(0, 0))) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(rect.pos, ImVec2(rect.pos.x + rect.size.x, rect.pos.y), toU32(th.border));
        ImGui::PushFont(fonts_.ui, 12.0f);
        const std::string description = session_.description();
        const bool local = description.rfind("local", 0) == 0;
        const std::string db = session_.currentDb();
        std::string text = local ? "local mode" : "server " + description;
        text += "  -  db: " + (db.empty() ? std::string("-") : db);
        text += std::string("  -  ") + protocol::kServerName;
        const float y = rect.pos.y + (rect.size.y - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText(ImVec2(rect.pos.x + px(16.0f), y), toU32(th.muted), text.c_str());
        const char* busy = session_.quitting() ? "band ho raha hai..." : (session_.busy() ? "chal raha hai..." : nullptr);
        if (busy != nullptr) {
            const float w = ImGui::CalcTextSize(busy).x;
            dl->AddText(ImVec2(rect.pos.x + rect.size.x - px(16.0f) - w, y), toU32(th.text), busy);
        }
        ImGui::PopFont();
    }
    ImGui::End();
}

}  // namespace meradb::gui
