// cpp/src/gui_dialogs.cpp -- see gui_dialogs.h.
#include "meradb/gui_dialogs.h"
#include "meradb/wb_help.h"
#include <algorithm>
#include <cfloat>
#include <cstring>

namespace meradb::gui {

namespace {

constexpr ImGuiWindowFlags kDialogFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                          ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse;

void copyInto(char* out, std::size_t capacity, const std::string& text) {
    std::strncpy(out, text.c_str(), capacity - 1);
    out[capacity - 1] = '\0';
}

int digitsOnly(ImGuiInputTextCallbackData* data) { return data->EventChar >= '0' && data->EventChar <= '9' ? 0 : 1; }

#if defined(__APPLE__)
#define MERADB_CTRL "Cmd"
#else
#define MERADB_CTRL "Ctrl"
#endif

}  // namespace

const std::vector<KeyHelp>& keyHelp() {
    static const std::vector<KeyHelp> rows = {
        {MERADB_CTRL "+Enter / F5", "Chalao: jo text select hai woh, nahi to poora editor", Action::Run},
        {MERADB_CTRL "+Shift+Enter / F6", "Samjhao: query plan dikhao, bina chalaye", Action::Explain},
        {"Alt+Up / Alt+Down, " MERADB_CTRL "+Up / " MERADB_CTRL "+Down", "Pichli / Agli query (history)", Action::Previous},
        {MERADB_CTRL "+S", "Results ko CSV file mein save karo (exports/ folder)", Action::ExportCsv},
        {MERADB_CTRL "+O", "Connect: doosre server se jodo, ya Local mode", Action::Connect},
        {"F1", "Madad kholo / band karo", Action::Madad},
        {MERADB_CTRL "+L", "Log saaf karo", Action::ClearLog},
        {MERADB_CTRL "+Q", "Bahar: chalta statement khatam hone tak ruko, khula transaction WAPAS ho jata hai", Action::Quit},
        {"Esc", "Dialog, Madad ya command menu band karo", std::nullopt},
        {MERADB_CTRL "+A / C / X / V", "Editor mein sab select, copy, cut, paste", std::nullopt},
        {MERADB_CTRL "+Z / " MERADB_CTRL "+Y", "Editor mein undo / redo (redo: " MERADB_CTRL "+Shift+Z bhi)", std::nullopt},
        {"Tab", "Editor mein 4 space daalo", std::nullopt},
        {"Enter / double-click", "Schema tree mein: database ISTEMAL, table ke pehle 100 rows, column ka naam editor mein", std::nullopt},
        {MERADB_CTRL "+C / " MERADB_CTRL "+Shift+C", "Results mein chuna hua cell / poori row (tab se alag) copy karo", std::nullopt},
    };
    return rows;
}

std::string madadMarkdown() {
    std::string md = "## Keys\n\n| Key | Kaam |\n|-----|------|\n";
    for (const KeyHelp& row : keyHelp()) md += "| **" + row.keys + "** | " + row.what + " |\n";
    md += "\n" + wb::languageDoc();
    return md;
}

void ConnectDialog::load(const wb::ConnectDefaults& defaults) {
    copyInto(host_, sizeof host_, defaults.host);
    copyInto(port_, sizeof port_, defaults.port);
    password_[0] = '\0';
    database_[0] = '\0';
}

void ConnectDialog::submit(wb::Session& session, bool local) {
    session.connect(wb::makeConnectRequest(local, host_, port_, password_, database_));
    password_[0] = '\0';   // do not keep it around
}

void ConnectDialog::draw(wb::Session& session, const Theme& th, const FontSet& fonts, float scale) {
    if (session.modal() != wb::Modal::Connect) {
        shown_ = false;
        return;
    }
    if (!shown_) {
        load(session.connectDefaults());
        shown_ = true;
        focusHost_ = true;
    }
    const char* id = "##connect_dialog";
    if (!ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float width = std::min(420.0f * scale, std::max(200.0f, display.x - 24.0f * scale));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f * scale, 20.0f * scale));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, toVec4(th.surface));
    ImGui::PushStyleColor(ImGuiCol_Border, toVec4(th.faint, 0.55f));
    const bool open = ImGui::BeginPopupModal(id, nullptr, kDialogFlags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (!open) return;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, toVec4(th.background));   // inputs sit lower than the dialog

    bool connect = false, local = false, cancel = ImGui::IsKeyPressed(ImGuiKey_Escape, false);

    ImGui::PushFont(fonts.uiMedium, kUiFontSize + 2.0f);
    ImGui::TextUnformatted("MeraDB server se connect karo");
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, 4.0f * scale));

    const ImGuiInputTextFlags fieldFlags = ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_EnterReturnsTrue;
    auto label = [&](const char* text) {
        ImGui::PushStyleColor(ImGuiCol_Text, toVec4(th.muted));
        ImGui::TextUnformatted(text);
        ImGui::PopStyleColor();
    };
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float portWidth = 96.0f * scale;
    const float hostWidth = ImGui::GetContentRegionAvail().x - portWidth - gap;

    const float portX = ImGui::GetCursorPosX() + hostWidth + gap;
    label("Host");
    ImGui::SameLine(portX);
    label("Port");
    ImGui::SetNextItemWidth(hostWidth);
    if (focusHost_) {
        ImGui::SetKeyboardFocusHere();
        focusHost_ = false;
    }
    if (ImGui::InputText("##host", host_, sizeof host_, fieldFlags)) connect = true;
    ImGui::SameLine(portX);
    ImGui::SetNextItemWidth(portWidth);
    if (ImGui::InputText("##port", port_, sizeof port_, fieldFlags | ImGuiInputTextFlags_CallbackCharFilter, digitsOnly)) connect = true;

    label("Password (agar server par hai)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##password", password_, sizeof password_, fieldFlags | ImGuiInputTextFlags_Password)) connect = true;

    label("Database (optional)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##database", database_, sizeof database_, fieldFlags)) connect = true;

    ImGui::Dummy(ImVec2(0.0f, 8.0f * scale));
    const float buttonH = 32.0f * scale;
    ImGui::PushFont(fonts.uiMedium, kUiFontSize);
    const char* names[3] = {"Cancel", "Local mode", "Connect"};
    float widths[3];
    for (int i = 0; i < 3; ++i) widths[i] = ImGui::CalcTextSize(names[i]).x + 28.0f * scale;
    const float total = widths[0] + widths[1] + widths[2] + gap * 2.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - total));
    if (ImGui::Button("Cancel", ImVec2(widths[0], buttonH))) cancel = true;
    ImGui::SameLine();
    if (ImGui::Button("Local mode", ImVec2(widths[1], buttonH))) local = true;
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, toVec4(th.primary));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toVec4(th.primary, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, toVec4(th.primary, 0.7f));
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(th.primaryText));
    ImGui::PushStyleColor(ImGuiCol_Border, toVec4(th.primary));
    if (ImGui::Button("Connect", ImVec2(widths[2], buttonH))) connect = true;
    ImGui::PopStyleColor(5);
    ImGui::PopFont();

    // Enter with the focus on no field (after a click on the background) is Connect as well
    if (!connect && !local && !cancel && !ImGui::IsAnyItemActive() && !ImGui::IsAnyItemFocused() &&
        (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
        connect = true;

    ImGui::Dummy(ImVec2(0.0f, 2.0f * scale));
    ImGui::PushFont(fonts.ui, 12.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(th.faint));
    ImGui::TextUnformatted("Enter = Connect    Esc = Cancel");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PopStyleColor();   // FrameBg

    if (cancel) {
        session.closeModal();
        ImGui::CloseCurrentPopup();
    } else if (local || connect) {
        submit(session, local);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void HelpWindow::draw(wb::Session& session, const Theme& th, const FontSet& fonts, float scale) {
    if (session.modal() != wb::Modal::Help) {
        shown_ = false;
        return;
    }
    if (!loaded_) {
        view_.setText(madadMarkdown());
        loaded_ = true;
    }
    shown_ = true;
    const char* id = "##madad_window";
    if (!ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float margin = std::min(40.0f * scale, display.x * 0.1f);
    const float width = std::max(160.0f, std::min(display.x - 2.0f * margin, 980.0f * scale));
    const float height = std::max(120.0f, display.y - 2.0f * std::min(margin, display.y * 0.1f));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, toVec4(th.background));
    ImGui::PushStyleColor(ImGuiCol_Border, toVec4(th.faint, 0.55f));
    const bool open = ImGui::BeginPopupModal(id, nullptr, kDialogFlags);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (!open) return;

    bool close = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
    const float headerH = 52.0f * scale;
    const ImVec2 origin = ImGui::GetWindowPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(ImVec2(origin.x, origin.y + headerH), ImVec2(origin.x + width, origin.y + headerH), toU32(th.border));
    ImGui::PushFont(fonts.uiMedium, kUiFontSize + 2.0f);
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 24.0f * scale, origin.y + (headerH - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextUnformatted("Madad");
    ImGui::PopFont();
    {
        ImGui::PushFont(fonts.uiMedium, kUiFontSize);
        const float buttonW = ImGui::CalcTextSize("Band karo").x + 28.0f * scale;
        const float buttonH = 32.0f * scale;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + width - 20.0f * scale - buttonW, origin.y + (headerH - buttonH) * 0.5f));
        if (ImGui::Button("Band karo", ImVec2(buttonW, buttonH))) close = true;
        ImGui::PopFont();
        ImGui::PushFont(fonts.ui, 12.0f);
        const char* hint = "F1 ya Esc se band karo";
        const float hw = ImGui::CalcTextSize(hint).x;
        dl->AddText(ImVec2(origin.x + width - 20.0f * scale - buttonW - 16.0f * scale - hw, origin.y + (headerH - ImGui::GetTextLineHeight()) * 0.5f),
                    toU32(th.faint), hint);
        ImGui::PopFont();
    }

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + headerH + 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f * scale, 8.0f * scale));
    ImGui::BeginChild("##madad_body", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_None);
    ImGui::PopStyleVar();
    {   // the keys of a reader: arrows, page keys, Home / End
        const float line = ImGui::GetTextLineHeight() * 1.6f;
        const float page = ImGui::GetWindowHeight() * 0.9f;
        float y = ImGui::GetScrollY();
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) y += line;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) y -= line;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true) || ImGui::IsKeyPressed(ImGuiKey_Space, true)) y += page;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true)) y -= page;
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) y = 0.0f;
        if (ImGui::IsKeyPressed(ImGuiKey_End, false)) y = ImGui::GetScrollMaxY();
        if (y != ImGui::GetScrollY()) ImGui::SetScrollY(y);
    }
    ImGui::Dummy(ImVec2(0.0f, 8.0f * scale));
    view_.draw(th, fonts, scale, 760.0f * scale);
    ImGui::Dummy(ImVec2(0.0f, 24.0f * scale));
    scrollY_ = ImGui::GetScrollY();
    ImGui::EndChild();

    if (close) {
        session.closeModal();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace meradb::gui
