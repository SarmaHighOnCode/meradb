// cpp/src/gui_dialogs.cpp -- see gui_dialogs.h.
#include "meradb/gui_dialogs.h"
#include "meradb/wb_help.h"
#include <algorithm>
#include <cfloat>
#include <cctype>
#include <cstring>
#include "imgui_internal.h"

namespace meradb::gui {

namespace {

constexpr ImGuiWindowFlags kDialogFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                          ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
                                          ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse;

void copyInto(char* out, std::size_t capacity, const std::string& text) {
    std::strncpy(out, text.c_str(), capacity - 1);
    out[capacity - 1] = '\0';
}

// The dialogs share one look: a dimmed window behind (the modal dim colour, at full strength at once instead of
// fading in), a 1px border in the border token, the surface colour, radius 8px. Returns the BeginPopupModal result.
bool beginDialogPopup(const char* id, const Theme& th, float scale, ImVec2 padding, std::uint32_t background) {
    ImGui::GetCurrentContext()->DimBgRatio = 1.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 8.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, toVec4(background));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, toVec4(background));
    ImGui::PushStyleColor(ImGuiCol_Border, toVec4(th.border));
    const bool open = ImGui::BeginPopupModal(id, nullptr, kDialogFlags);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(5);
    return open;
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
        {"Alt+Enter / F9", "Yeh query: sirf woh query chalao jahan cursor hai (ya jo select hai)", Action::RunStatement},
        {MERADB_CTRL "+Shift+Enter / F6", "Samjhao: query plan dikhao, bina chalaye", Action::Explain},
        {"Alt+Up / " MERADB_CTRL "+Up", "Pichli query (history)", Action::Previous},
        {"Alt+Down / " MERADB_CTRL "+Down", "Agli query (history)", Action::Next},
        {MERADB_CTRL "+S", "Results ko CSV file mein save karo (exports/ folder)", Action::ExportCsv},
        {MERADB_CTRL "+O", "Connect: doosre server se jodo, ya Local mode", Action::Connect},
        {"F1", "Madad kholo / band karo", Action::Madad},
        {MERADB_CTRL "+L", "Log saaf karo", Action::ClearLog},
        {MERADB_CTRL "+Q", "Bahar: chalta statement khatam hone tak ruko, khula transaction WAPAS ho jata hai", Action::Quit},
        {MERADB_CTRL "+K", "Command menu: action, table ya purani query dhoondo aur chalao", Action::Palette},
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
    user_[0] = '\0';
    password_[0] = '\0';
    database_[0] = '\0';
    tls_[0] = '\0';
}

void ConnectDialog::submit(wb::Session& session, bool local) {
    session.connect(wb::makeConnectRequest(local, host_, port_, password_, database_, user_, tls_));
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
    const bool open = beginDialogPopup(id, th, scale, ImVec2(24.0f * scale, 20.0f * scale), th.surface);
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
    auto remember = [&](Widget w) {
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        centres_[w] = ImVec2((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f);
    };
    ImGui::SetNextItemWidth(hostWidth);
    if (focusHost_) {
        ImGui::SetKeyboardFocusHere();
        focusHost_ = false;
    }
    if (ImGui::InputText("##host", host_, sizeof host_, fieldFlags)) connect = true;
    remember(kHost);
    ImGui::SameLine(portX);
    ImGui::SetNextItemWidth(portWidth);
    if (ImGui::InputText("##port", port_, sizeof port_, fieldFlags | ImGuiInputTextFlags_CallbackCharFilter, digitsOnly)) connect = true;
    remember(kPort);

    label("Username (agar server par users hain)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##user", user_, sizeof user_, fieldFlags)) connect = true;
    remember(kUser);

    label("Password (agar server par hai)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##password", password_, sizeof password_, fieldFlags | ImGuiInputTextFlags_Password)) connect = true;
    remember(kPassword);

    label("Database (optional)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##database", database_, sizeof database_, fieldFlags)) connect = true;
    remember(kDatabase);

    label("TLS: 'on' ya certificate file (khali = TLS nahi)");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##tls", tls_, sizeof tls_, fieldFlags)) connect = true;
    remember(kTls);

    ImGui::Dummy(ImVec2(0.0f, 8.0f * scale));
    const float buttonH = 32.0f * scale;
    ImGui::PushFont(fonts.uiMedium, kUiFontSize);
    const char* names[3] = {"Cancel", "Local mode", "Connect"};
    float widths[3];
    for (int i = 0; i < 3; ++i) widths[i] = ImGui::CalcTextSize(names[i]).x + 28.0f * scale;
    const float total = widths[0] + widths[1] + widths[2] + gap * 2.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - total));
    if (ImGui::Button("Cancel", ImVec2(widths[0], buttonH))) cancel = true;
    remember(kCancel);
    ImGui::SameLine();
    if (ImGui::Button("Local mode", ImVec2(widths[1], buttonH))) local = true;
    remember(kLocal);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, toVec4(th.primary));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, toVec4(th.primary, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, toVec4(th.primary, 0.7f));
    ImGui::PushStyleColor(ImGuiCol_Text, toVec4(th.primaryText));
    ImGui::PushStyleColor(ImGuiCol_Border, toVec4(th.primary));
    if (ImGui::Button("Connect", ImVec2(widths[2], buttonH))) connect = true;
    remember(kConnect);
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
    const bool open = beginDialogPopup(id, th, scale, ImVec2(0.0f, 0.0f), th.surface);
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

// ---------------------------------------------------------------- the command menu

bool commandMatches(const std::string& text, const std::string& query) {
    auto lower = [](const std::string& s) {
        std::string out;
        for (unsigned char c : s) out += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
        return out;
    };
    const std::string hay = lower(text), q = lower(query);
    std::size_t pos = 0;
    while (pos < q.size()) {
        while (pos < q.size() && (q[pos] == ' ' || q[pos] == '\t')) ++pos;
        std::size_t end = pos;
        while (end < q.size() && q[end] != ' ' && q[end] != '\t') ++end;
        if (end > pos && hay.find(q.substr(pos, end - pos)) == std::string::npos) return false;
        pos = end;
    }
    return true;
}

std::vector<CommandItem> commandItems(wb::Session& session) {
    std::vector<CommandItem> items;
    auto action = [&](const char* label, const char* detail, Action a) {
        CommandItem item;
        item.kind = CommandItem::Kind::Action;
        item.label = label;
        item.detail = detail;
        item.action = a;
        items.push_back(std::move(item));
    };
    action("Chalao", MERADB_CTRL "+Enter", Action::Run);
    action("Yeh query", "Alt+Enter", Action::RunStatement);
    action("Samjhao", MERADB_CTRL "+Shift+Enter", Action::Explain);
    action("CSV", MERADB_CTRL "+S", Action::ExportCsv);
    action("Connect", MERADB_CTRL "+O", Action::Connect);
    action("Madad", "F1", Action::Madad);
    action("Log saaf karo", MERADB_CTRL "+L", Action::ClearLog);
    action("Light / dark", "", Action::ToggleTheme);
    for (const wb::TreeRow& row : session.tree().allNodes()) {
        if (row.key.kind != wb::NodeKey::Kind::Table) continue;
        CommandItem item;
        item.kind = CommandItem::Kind::Table;
        item.label = "DIKHAO * SE " + row.key.table + " SIRF 100";
        item.detail = row.key.db;
        item.table = row.key;
        items.push_back(std::move(item));
    }
    const std::vector<std::string>& history = session.history().items();
    std::size_t taken = 0;
    for (auto it = history.rbegin(); it != history.rend() && taken < 20; ++it, ++taken) {
        CommandItem item;
        item.kind = CommandItem::Kind::History;
        std::string flat;
        bool blank = true;
        for (char c : *it) {   // one line: blanks and line breaks collapse
            const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
            if (space) {
                if (!blank) flat += ' ';
                blank = true;
            } else {
                flat += c;
                blank = false;
            }
        }
        if (flat.size() > 120) {
            std::size_t cut = 117;
            while (cut > 0 && (static_cast<unsigned char>(flat[cut]) & 0xC0) == 0x80) --cut;
            flat = flat.substr(0, cut) + "...";
        }
        item.label = flat;
        item.detail = "history";
        item.text = *it;
        items.push_back(std::move(item));
    }
    return items;
}

void CommandMenu::open(wb::Session& session) {
    items_ = commandItems(session);
    buffer_[0] = '\0';
    query_.clear();
    highlighted_ = 0;
    open_ = true;
    focusInput_ = true;
    openedFrame_ = ImGui::GetCurrentContext() != nullptr ? ImGui::GetFrameCount() : -1;
    refilter();
}

void CommandMenu::setQuery(const std::string& text) {
    std::strncpy(buffer_, text.c_str(), sizeof buffer_ - 1);
    buffer_[sizeof buffer_ - 1] = '\0';
    query_ = buffer_;
    highlighted_ = 0;
    refilter();
}

void CommandMenu::refilter() {
    visible_.clear();
    for (std::size_t i = 0; i < items_.size(); ++i)
        if (commandMatches(items_[i].label + " " + items_[i].detail, query_)) visible_.push_back(static_cast<int>(i));
    highlighted_ = visible_.empty() ? 0 : std::max(0, std::min(highlighted_, static_cast<int>(visible_.size()) - 1));
}

void CommandMenu::move(int delta) {
    if (visible_.empty()) return;
    highlighted_ = std::max(0, std::min(highlighted_ + delta, static_cast<int>(visible_.size()) - 1));
    scrollToHighlight_ = true;
}

std::optional<CommandItem> CommandMenu::draw(const Theme& th, const FontSet& fonts, float scale) {
    if (!open_) return std::nullopt;
    const char* id = "##command_menu";
    if (!ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float width = std::min(560.0f * scale, std::max(200.0f, display.x - 24.0f * scale));
    const float rowH = 32.0f * scale;
    const int rows = std::max(1, std::min(10, static_cast<int>(visible_.size())));
    const float listH = rowH * static_cast<float>(rows);
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, std::max(8.0f, display.y * 0.22f)), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f));
    const bool opened = beginDialogPopup(id, th, scale, ImVec2(0.0f, 0.0f), th.surface);
    if (!opened) return std::nullopt;

    std::optional<CommandItem> chosen;
    const bool justOpened = openedFrame_ == ImGui::GetFrameCount();
    const bool close = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || (!justOpened && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_K, false));
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) move(1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) move(-1);
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true)) move(rows);
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true)) move(-rows);
    bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);

    // One container (the popup, with its border): the filter on top, a hairline, then the list; both use the full width.
    const float inset = 8.0f * scale;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(inset + 10.0f * scale, 12.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ImGui::SetCursorPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextItemWidth(width);
    const bool refocus = focusInput_ || !ImGui::IsAnyItemActive();
    if (refocus) {
        ImGui::SetKeyboardFocusHere();
        focusInput_ = false;
    }
    if (ImGui::InputTextWithHint("##command_filter", "Kya karna hai? dhoondo...", buffer_, sizeof buffer_)) {
        if (query_ != buffer_) {
            query_ = buffer_;
            highlighted_ = 0;
            refilter();
        }
    }
    if (refocus) {   // focusing selects everything: keep what is typed unselected, the caret at its end
        if (ImGuiInputTextState* state = ImGui::GetInputTextState(ImGui::GetItemID())) {
            state->ClearSelection();
        }
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
    {
        const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(lo.x, hi.y), ImVec2(lo.x + width, hi.y), toU32(th.border));
    }
    ImGui::SetCursorPos(ImVec2(0.0f, ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y + 1.0f));

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(inset, inset));
    ImGui::BeginChild("##command_list", ImVec2(width, listH + 2.0f * inset), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    if (visible_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, toVec4(th.faint));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0f * scale);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 10.0f * scale);
        ImGui::TextUnformatted("Kuch nahi mila.");
        ImGui::PopStyleColor();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 mouseDelta = ImGui::GetIO().MouseDelta;
    rowCentres_.clear();
    for (int pos = 0; pos < static_cast<int>(visible_.size()); ++pos) {
        const CommandItem& item = items_[static_cast<std::size_t>(visible_[static_cast<std::size_t>(pos)])];
        ImGui::PushID(pos);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float rowW = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##row", ImVec2(rowW, rowH));
        rowCentres_.push_back(ImVec2(at.x + rowW * 0.5f, at.y + rowH * 0.5f));
        if (ImGui::IsItemHovered() && (mouseDelta.x != 0.0f || mouseDelta.y != 0.0f)) highlighted_ = pos;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            highlighted_ = pos;
            enter = true;
        }
        const bool selected = pos == highlighted_;
        if (selected) {
            dl->AddRectFilled(at, ImVec2(at.x + rowW, at.y + rowH), toU32(th.hover), 6.0f * scale);
            if (scrollToHighlight_) {
                ImGui::SetScrollHereY(0.5f);
                scrollToHighlight_ = false;
            }
        }
        const bool mono = item.kind != CommandItem::Kind::Action;
        ImFont* font = mono ? fonts.mono : fonts.ui;
        ImGui::PushFont(font, mono ? kMonoFontSize : kUiFontSize);
        const float px = ImGui::GetFontSize();
        ImGui::PopFont();
        ImGui::PushFont(fonts.ui, 12.0f);
        const float dpx = ImGui::GetFontSize();
        const float dw = ImGui::CalcTextSize(item.detail.c_str()).x;
        ImGui::PopFont();
        const float textX = at.x + 10.0f * scale;
        const float maxLabel = rowW - 20.0f * scale - dw - 12.0f * scale;
        // the label is clipped to its room (a long statement) rather than running under the detail
        dl->PushClipRect(ImVec2(textX, at.y), ImVec2(textX + std::max(0.0f, maxLabel), at.y + rowH), true);
        dl->AddText(font, px, ImVec2(textX, at.y + (rowH - px) * 0.5f), toU32(th.text), item.label.c_str());
        dl->PopClipRect();
        dl->AddText(fonts.ui, dpx, ImVec2(at.x + rowW - 10.0f * scale - dw, at.y + (rowH - dpx) * 0.5f), toU32(th.faint), item.detail.c_str());
        if (selected && enter) chosen = item;
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    scrollToHighlight_ = false;

    if (chosen || close) {
        open_ = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    return chosen;
}

}  // namespace meradb::gui
