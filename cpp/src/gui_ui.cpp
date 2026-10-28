// cpp/src/gui_ui.cpp -- see gui_ui.h.
#include "meradb/gui_ui.h"
#include "meradb/protocol.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
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

std::string connectionLabel(const std::string& description) {
    static const std::string prefix = "local (";
    if (description.size() <= prefix.size() || description.compare(0, prefix.size(), prefix) != 0 || description.back() != ')')
        return description;
    std::string path = description.substr(prefix.size(), description.size() - prefix.size() - 1);
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.pop_back();
    const std::size_t slash = path.find_last_of("/\\");
    const std::string folder = slash == std::string::npos ? path : path.substr(slash + 1);
    return folder.empty() ? std::string("local") : "local (" + folder + ")";
}

const std::vector<KeyBinding>& keyBindings() {
    static const std::vector<KeyBinding> table = {
        {ImGuiMod_Ctrl | ImGuiKey_Enter, Action::Run},
        {ImGuiMod_Ctrl | ImGuiKey_KeypadEnter, Action::Run},
        {ImGuiKey_F5, Action::Run},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Enter, Action::Explain},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_KeypadEnter, Action::Explain},
        {ImGuiKey_F6, Action::Explain},
        {ImGuiMod_Alt | ImGuiKey_UpArrow, Action::Previous},
        {ImGuiMod_Ctrl | ImGuiKey_UpArrow, Action::Previous},
        {ImGuiMod_Alt | ImGuiKey_DownArrow, Action::Next},
        {ImGuiMod_Ctrl | ImGuiKey_DownArrow, Action::Next},
        {ImGuiMod_Ctrl | ImGuiKey_S, Action::ExportCsv},
        {ImGuiMod_Ctrl | ImGuiKey_L, Action::ClearLog},
        {ImGuiMod_Ctrl | ImGuiKey_O, Action::Connect},
        {ImGuiKey_F1, Action::Madad},
        {ImGuiMod_Ctrl | ImGuiKey_Q, Action::Quit},
    };
    return table;
}

GuiUi::GuiUi(wb::Session& session, const FontSet& fonts, ThemeMode mode)
    : session_(session),
      fonts_(fonts),
      mode_(mode),
      appliedMode_(mode),
      editor_(session.editor()),
      tree_(session,
            [this](const std::function<void()>& change) {
                const std::string before = session_.editor().text();
                editor_.external(change);
                if (session_.editor().text() != before) {   // a column name went in: typing continues there
                    editor_.setFocused(true);
                    tree_.setFocused(false);
                }
            }),
      seenVersion_(session.resultVersion()),
      seenRevision_(session.log().revision()),
      seenGeneration_(session.log().generation()) {}

void GuiUi::perform(Action action) {
    switch (action) {
        case Action::Run: session_.runEditorText(); break;
        case Action::Explain: session_.explainEditorText(); break;
        case Action::Previous: editor_.external([this] { session_.historyStep(-1); }); break;
        case Action::Next: editor_.external([this] { session_.historyStep(+1); }); break;
        case Action::ExportCsv:
            session_.exportCsv();
            seenRevision_ = session_.log().revision();   // its message is not the end of a run: no tab switch
            break;
        case Action::ClearLog: session_.clearLog(); break;
        case Action::Connect:
            if (!session_.quitting()) session_.openConnectDialog();
            break;
        case Action::Madad:
            if (session_.modal() == wb::Modal::Help) session_.closeModal();
            else if (session_.modal() == wb::Modal::None && !session_.quitting()) session_.showHelp();
            break;
        case Action::ToggleTheme: mode_ = otherMode(mode_); break;
        case Action::Quit: session_.requestQuit(); break;
    }
}

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

bool GuiUi::overlayOpen() const { return session_.modal() != wb::Modal::None; }

void GuiUi::handleShortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    const bool dialog = overlayOpen();
    for (const KeyBinding& binding : keyBindings()) {
        if (!ImGui::IsKeyChordPressed(binding.chord)) continue;
        if (dialog && binding.action != Action::Quit && !(binding.action == Action::Madad && session_.modal() == wb::Modal::Help)) continue;
        perform(binding.action);
        break;
    }
    // copying from the grid: only when no text field wants Ctrl+C
    if (!dialog && !editor_.focused() && !io.WantTextInput && grid_.hasSelection() && session_.table() != nullptr) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) grid_.copy(*session_.table(), false);
        else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C)) grid_.copy(*session_.table(), true);
    }
}

// After every run the window shows what it produced: a table goes to Results, messages and errors without a table go
// to Log. It waits until the worker is idle, so the echo line that arrives before the answer does not decide.
void GuiUi::followRuns() {
    if (session_.busy()) return;
    const wb::LogBuffer& log = session_.log();
    const int version = session_.resultVersion();
    if (version == seenVersion_ && log.revision() == seenRevision_ && log.generation() == seenGeneration_) return;
    const bool newTable = version != seenVersion_;
    bool messages = false;
    const auto& entries = log.entries();
    if (log.generation() == seenGeneration_ && log.revision() > seenRevision_) {
        const std::size_t fresh = std::min<std::size_t>(log.revision() - seenRevision_, entries.size());
        for (std::size_t i = entries.size() - fresh; i < entries.size(); ++i) {
            const wb::LogKind kind = entries[i].kind;
            if (kind == wb::LogKind::Error || kind == wb::LogKind::Message || kind == wb::LogKind::Warn) messages = true;
        }
    }
    if (newTable) {
        requestTab_ = Tab::Results;
        std::size_t scanned = 0;
        for (auto it = entries.rbegin(); it != entries.rend() && scanned < 8; ++it, ++scanned) {   // "(3.1 ms)"
            if (it->kind != wb::LogKind::Dim || it->lines.size() != 1) continue;
            double ms = 0.0;
            if (std::sscanf(wb::plainText(it->lines[0]).c_str(), "(%lf ms)", &ms) == 1) {
                lastMs_ = ms;
                break;
            }
        }
    } else if (messages) {
        requestTab_ = Tab::Log;
    }
    seenVersion_ = version;
    seenRevision_ = log.revision();
    seenGeneration_ = log.generation();
}

void GuiUi::drawSplitters(float topH, float bodyH, float schemaW, float queryH, const ImVec2& display) {
    const Theme& th = theme();
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse;   // on top of the panels
    const float grab = px(6.0f);
    auto strip = [&](const char* id, ImVec2 pos, ImVec2 size, bool vertical, float* delta) {
        ImGui::SetNextWindowPos(pos);
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        const bool open = ImGui::Begin(id, nullptr, flags);
        ImGui::PopStyleVar(2);
        if (open) {
            ImGui::InvisibleButton("##grip", size);
            const bool active = ImGui::IsItemActive();
            if (ImGui::IsItemHovered() || active) {
                ImGui::SetMouseCursor(vertical ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
                const ImVec2 mid(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
                const float w = px(2.0f);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImU32 color = toU32(th.focus, active ? 1.0f : 0.6f);
                if (vertical) dl->AddRectFilled(ImVec2(mid.x - w * 0.5f, pos.y), ImVec2(mid.x + w * 0.5f, pos.y + size.y), color);
                else dl->AddRectFilled(ImVec2(pos.x, mid.y - w * 0.5f), ImVec2(pos.x + size.x, mid.y + w * 0.5f), color);
            }
            if (active) *delta = vertical ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
        }
        ImGui::End();
    };
    float dx = 0.0f, dy = 0.0f;
    strip("##split_side", ImVec2(schemaW - grab * 0.5f, topH), ImVec2(grab, bodyH), true, &dx);
    strip("##split_query", ImVec2(schemaW, topH + queryH - grab * 0.5f), ImVec2(display.x - schemaW, grab), false, &dy);
    if (dx != 0.0f) schemaWidth_ = std::max(180.0f, std::min(schemaWidth_ + dx / scale_, display.x / scale_ * 0.5f));
    if (dy != 0.0f && bodyH > 0.0f) queryFraction_ = std::max(0.15f, std::min(queryFraction_ + dy / bodyH, 0.85f));
}

void GuiUi::draw() {
    if (scale_ <= 0.0f) scale_ = 1.0f;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float topH = std::min(px(kTopBarHeight), display.y);
    const float statusH = std::min(px(kStatusBarHeight), std::max(0.0f, display.y - topH));
    const float bodyH = std::max(0.0f, display.y - topH - statusH);
    const float schemaW = std::min(px(schemaWidth_), display.x * 0.5f);
    const float rightW = std::max(0.0f, display.x - schemaW);
    const float minPart = std::min(px(120.0f), bodyH * 0.5f);
    const float queryH = std::max(minPart, std::min(std::floor(bodyH * queryFraction_), bodyH - minPart));

    ImGui::PushFont(fonts_.ui, kUiFontSize);
    handleShortcuts();
    followRuns();
    if (const std::optional<wb::Panel> focus = session_.takeFocusRequest()) {
        if (*focus == wb::Panel::Editor) {
            editor_.setFocused(true);
            tree_.setFocused(false);
        }
    }
    if (overlayOpen()) {   // the keys belong to the overlay, not to the panels behind it
        editor_.setFocused(false);
        tree_.setFocused(false);
    }
    drawTopBar({ImVec2(0, 0), ImVec2(display.x, topH)});
    drawSchema({ImVec2(0, topH), ImVec2(schemaW, bodyH)});
    drawQuery({ImVec2(schemaW, topH), ImVec2(rightW, queryH)});
    drawResults({ImVec2(schemaW, topH + queryH), ImVec2(rightW, bodyH - queryH)});
    drawStatusBar({ImVec2(0, display.y - statusH), ImVec2(display.x, statusH)});
    drawSplitters(topH, bodyH, schemaW, queryH, display);
    connectDialog_.draw(session_, theme(), fonts_, scale_);
    helpWindow_.draw(session_, theme(), fonts_, scale_);
    const bool overlay = overlayOpen();
    if (overlayWasOpen_ && !overlay) editor_.setFocused(true);   // typing continues where it was
    overlayWasOpen_ = overlay;
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
        if (ImGui::Button("##theme", ImVec2(buttonH, buttonH))) perform(Action::ToggleTheme);
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
            if (ImGui::Button(labels[i], ImVec2(labelWidths[i], buttonH))) perform(i == 1 ? Action::Connect : Action::Madad);
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
        const std::string label = connectionLabel(description);
        const std::string db = session_.currentDb();
        const float sepWidth = ImGui::CalcTextSize("/").x + px(20.0f);
        const float dbWidth = db.empty() ? 0.0f : sepWidth + ImGui::CalcTextSize(db.c_str()).x;
        const bool inTransaction = session_.inTransaction();
        badgeShown_ = inTransaction;
        const char* badge = "TRANSACTION";
        const float badgeWidth = inTransaction ? ImGui::CalcTextSize(badge).x + px(20.0f) : 0.0f;
        const float badgeGap = inTransaction ? px(14.0f) : 0.0f;
        const float limit = right - px(8.0f);
        const float descMax = std::max(px(60.0f), limit - x - sepWidth - dbWidth - badgeGap - badgeWidth);

        auto separator = [&]() {
            dl->AddText(ImVec2(x + px(10.0f), textY), toU32(th.faint), "/");
            x += sepWidth;
        };
        if (!label.empty()) {
            separator();
            const std::string shown = ellipsizeMiddle(label, descMax, [](const std::string& s) {
                return ImGui::CalcTextSize(s.c_str()).x;
            });
            dl->AddText(ImVec2(x, textY), toU32(th.muted), shown.c_str());
            const float shownWidth = ImGui::CalcTextSize(shown.c_str()).x;
            if (ImGui::IsMouseHoveringRect(ImVec2(x, origin.y), ImVec2(x + shownWidth, origin.y + height))) {
                ImGui::SetTooltip("%s", description.c_str());   // the full description: the whole folder or host:port
            }
            x += shownWidth;
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
        tree_.draw(th, fonts_, scale_);
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

        const float buttonH = px(32.0f);
        const float actionsH = buttonH + ImGui::GetStyle().ItemSpacing.y;
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        const float editorH = std::max(px(48.0f), avail.y - actionsH);
        editor_.draw("##editor", ImVec2(0, editorH), th, fonts_.mono, kMonoFontSize, scale_);

        // ---- actions row ----
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImGui::PushFont(fonts_.uiMedium, kUiFontSize);
        const float runLabelW = ImGui::CalcTextSize("Chalao").x;
        ImGui::PopFont();
#if defined(__APPLE__)
        const char* hint = "Cmd+Enter";
#else
        const char* hint = "Ctrl+Enter";
#endif
        ImGui::PushFont(fonts_.ui, 12.0f);
        const float hintW = ImGui::CalcTextSize(hint).x;
        const float hintH = ImGui::GetTextLineHeight();
        ImGui::PopFont();
        const ImVec2 runSize(px(14.0f) + runLabelW + px(10.0f) + hintW + px(14.0f), buttonH);
        ImGui::PushStyleColor(ImGuiCol_Button, toVec4(th.primary));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toVec4(th.primary, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, toVec4(th.primary, 0.7f));
        ImGui::PushStyleColor(ImGuiCol_Border, toVec4(th.primary));
        if (ImGui::Button("##chalao", runSize)) perform(Action::Run);
        ImGui::PopStyleColor(4);
        {
            const ImVec2 lo = ImGui::GetItemRectMin();
            ImGui::PushFont(fonts_.uiMedium, kUiFontSize);
            dl->AddText(ImVec2(lo.x + px(14.0f), lo.y + (buttonH - ImGui::GetTextLineHeight()) * 0.5f), toU32(th.primaryText), "Chalao");
            ImGui::PopFont();
            ImGui::PushFont(fonts_.ui, 12.0f);
            dl->AddText(ImVec2(lo.x + px(14.0f) + runLabelW + px(10.0f), lo.y + (buttonH - hintH) * 0.5f), toU32(th.primaryText, 0.55f), hint);
            ImGui::PopFont();
        }
        ImGui::SameLine();
        ImGui::PushFont(fonts_.uiMedium, kUiFontSize);
        if (ImGui::Button("Samjhao", ImVec2(ImGui::CalcTextSize("Samjhao").x + px(24.0f), buttonH))) perform(Action::Explain);
        ImGui::SameLine();
        const float previousW = ImGui::CalcTextSize("Pichli").x + px(24.0f);
        const float nextW = ImGui::CalcTextSize("Agli").x + px(24.0f);
        ImGui::PopFont();
        const bool quitting = session_.quitting();
        const char* status = quitting ? "band ho raha hai..." : (session_.busy() ? "chal raha hai..." : "");
        const float statusW = status[0] != '\0' ? ImGui::CalcTextSize(status).x + px(24.0f) : 0.0f;   // text + spinner
        const float rightEdge = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
        const float groupW = previousW + nextW + px(8.0f) * 2 + statusW + (statusW > 0 ? px(8.0f) : 0.0f);
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), rightEdge - groupW));
        ImGui::PushFont(fonts_.uiMedium, kUiFontSize);
        ImGui::BeginDisabled(session_.history().size() == 0);
        if (ImGui::Button("Pichli", ImVec2(previousW, buttonH))) perform(Action::Previous);
        ImGui::SameLine();
        if (ImGui::Button("Agli", ImVec2(nextW, buttonH))) perform(Action::Next);
        ImGui::EndDisabled();
        ImGui::PopFont();
        if (statusW > 0) {
            ImGui::SameLine();
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float radius = px(6.0f);
            const ImVec2 centre(at.x + px(8.0f) + radius, at.y + buttonH * 0.5f);
            const float t = static_cast<float>(ImGui::GetTime()) * 6.0f;
            dl->PathArcTo(centre, radius, t, t + 4.4f, 20);
            dl->PathStroke(toU32(th.muted), px(1.6f));
            dl->AddText(ImVec2(centre.x + radius + px(8.0f), at.y + (buttonH - ImGui::GetTextLineHeight()) * 0.5f), toU32(th.muted), status);
            ImGui::Dummy(ImVec2(statusW, buttonH));
        }
    }
    ImGui::End();
}

void GuiUi::drawTabBarExtras(float tabY, float rightX) {
    const Theme& th = theme();
    const float height = ImGui::GetFrameHeight();
    float right = rightX;
    ImGui::PushFont(fonts_.uiMedium, kUiFontSize);
    if (activeTab_ == Tab::Log) {
        const float w = ImGui::CalcTextSize("Log saaf karo").x + px(20.0f);
        ImGui::SetCursorScreenPos(ImVec2(right - w, tabY));
        if (ImGui::Button("Log saaf karo", ImVec2(w, height))) perform(Action::ClearLog);
    } else if (const wb::ResultTable* table = session_.table()) {
        const float w = ImGui::CalcTextSize("CSV").x + px(20.0f);
        ImGui::SetCursorScreenPos(ImVec2(right - w, tabY));
        if (ImGui::Button("CSV", ImVec2(w, height))) perform(Action::ExportCsv);
        right -= w + px(10.0f);
        ImGui::PopFont();
        ImGui::PushFont(fonts_.ui, kUiFontSize);
        const std::string summary = rowsSummary(table->rows.size(), lastMs_);
        const float tw = ImGui::CalcTextSize(summary.c_str()).x;
        ImGui::GetWindowDrawList()->AddText(ImVec2(right - tw, tabY + (height - ImGui::GetTextLineHeight()) * 0.5f), toU32(th.muted),
                                            summary.c_str());
    }
    ImGui::PopFont();
}

void GuiUi::drawResults(const Rect& rect) {
    const Theme& th = theme();
    if (beginPanel("##results", rect, th.background, ImVec2(px(16.0f), px(8.0f)))) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddLine(rect.pos, ImVec2(rect.pos.x + rect.size.x, rect.pos.y), toU32(th.border));
        const ImVec2 tabStart = ImGui::GetCursorScreenPos();
        const float rightX = tabStart.x + ImGui::GetContentRegionAvail().x;
        drawTabBarExtras(tabStart.y, rightX);
        ImGui::SetCursorScreenPos(tabStart);
        if (ImGui::BeginTabBar("##resulttabs", ImGuiTabBarFlags_None)) {
            const std::optional<Tab> wanted = requestTab_;
            requestTab_.reset();
            if (ImGui::BeginTabItem("Results", nullptr, wanted == Tab::Results ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
                activeTab_ = Tab::Results;
                drawResultsTab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Log", nullptr, wanted == Tab::Log ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None)) {
                activeTab_ = Tab::Log;
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
    grid_.draw(*table, session_.resultVersion(), th, fonts_, scale_);
}

void GuiUi::rebuildLogCache() {
    const wb::LogBuffer& log = session_.log();
    if (log.revision() == logRevision_ && log.generation() == logGeneration_) return;
    logLines_.clear();
    logMaxChars_ = 0;
    for (const wb::LogEntry& entry : log.entries()) {
        bool first = true;
        for (const wb::Line& line : entry.lines) {
            LogLine out{entry.kind, wb::plainText(line), 0};
            if (entry.kind == wb::LogKind::Echo && first && !line.empty() && line[0].style.dim) out.prefix = line[0].text.size();
            first = false;
            int chars = 0;
            for (unsigned char ch : out.text) chars += (ch & 0xC0) != 0x80 ? 1 : 0;
            logMaxChars_ = std::max(logMaxChars_, chars);
            logLines_.push_back(std::move(out));
        }
    }
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
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(fonts_.mono, kMonoFontSize);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float cw = ImGui::CalcTextSize("0").x;
    const float rowH = ImGui::GetTextLineHeight() + px(5.0f);
    const float rowW = static_cast<float>(logMaxChars_ + 2) * cw + px(16.0f);
    const float width = std::max(rowW, ImGui::GetContentRegionAvail().x);
    ImFont* font = ImGui::GetFont();
    const float fontSize = ImGui::GetFontSize();
    ImFontBaked* baked = ImGui::GetFontBaked();
    Advance adv;
    adv.ascii = cw;
    adv.other = [baked](char32_t cp) { return baked->GetCharAdvance(static_cast<ImWchar>(cp)); };
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(logLines_.size()), rowH);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const LogLine& line = logLines_[static_cast<std::size_t>(i)];
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(width, rowH));
            const ImVec2 text(at.x + px(2.0f), at.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f);
            if (line.kind == wb::LogKind::Echo) {
                dl->AddRectFilled(at, ImVec2(at.x + width, at.y + rowH), toU32(th.surface));
                if (line.prefix > 0) dl->AddText(font, fontSize, text, toU32(th.faint), line.text.data(), line.text.data() + line.prefix);
                const float x = text.x + textWidth(line.text.data(), line.text.data() + line.prefix, adv);
                drawHighlighted(dl, font, fontSize, ImVec2(x, text.y), line.text, line.prefix, line.text.size(), th, adv);
            } else {
                dl->AddText(font, fontSize, text, toU32(colorForLog(th, line.kind)), line.text.c_str());
            }
        }
    }
    ImGui::PopFont();
    const bool changed = previousRevision != logRevision_;
    if (changed) {
        if (logFollow_) ImGui::SetScrollHereY(1.0f);
    } else {
        logFollow_ = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
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
