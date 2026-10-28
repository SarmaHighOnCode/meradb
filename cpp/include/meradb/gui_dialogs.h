// cpp/include/meradb/gui_dialogs.h -- the overlays of the desktop window: the connect dialog, the Madad (help) window
// and the command menu. Each draws between ImGui::NewFrame() and Render(), on top of the layout, as a modal popup.
#pragma once
#include "meradb/gui_actions.h"
#include "meradb/gui_fonts.h"
#include "meradb/gui_markdown.h"
#include "meradb/gui_theme.h"
#include "meradb/wb_session.h"
#include "imgui.h"
#include <optional>
#include <string>
#include <vector>

namespace meradb::gui {

// tui.py's ConnectScreen: Host, Port, Password (masked), Database; buttons Connect / Local mode / Cancel.
// Shown while session.modal() == Modal::Connect. The fields start from Session::connectDefaults() each time the
// dialog opens and select their whole text when they get the focus. Enter = Connect, Esc = Cancel. The outcome goes
// through Session::connect (which closes the modal and logs success or failure).
class ConnectDialog {
public:
    void draw(wb::Session& session, const Theme& theme, const FontSet& fonts, float scale);

    bool shown() const { return shown_; }
    std::string host() const { return host_; }
    std::string port() const { return port_; }
    std::string database() const { return database_; }
    bool passwordEmpty() const { return password_[0] == '\0'; }

private:
    void load(const wb::ConnectDefaults& defaults);
    void submit(wb::Session& session, bool local);

    bool shown_ = false;
    bool focusHost_ = false;
    char host_[256] = {0};
    char port_[8] = {0};
    char password_[256] = {0};
    char database_[256] = {0};
};

// One line of the key table in the Madad window.
struct KeyHelp {
    std::string keys;                 // "Ctrl+Enter / F5" (Cmd instead of Ctrl on macOS)
    std::string what;                 // Hinglish
    std::optional<Action> action;     // the action this row documents, when it is a key binding of the window
};
const std::vector<KeyHelp>& keyHelp();
// The Madad window's text: the key table of this window, then the language reference (docs/LANGUAGE.md). That is
// wb::helpMarkdown() with its first part, the terminal workbench's own key table, replaced by this window's keys.
std::string madadMarkdown();

// The Madad window (keys and language reference). Shown while session.modal() == Modal::Help; Esc, F1 and the
// Band karo button close it.
class HelpWindow {
public:
    void draw(wb::Session& session, const Theme& theme, const FontSet& fonts, float scale);
    bool shown() const { return shown_; }
    MarkdownView& view() { return view_; }
    float scrollY() const { return scrollY_; }

private:
    MarkdownView view_;
    bool loaded_ = false;
    bool shown_ = false;
    float scrollY_ = 0.0f;
};

}  // namespace meradb::gui
