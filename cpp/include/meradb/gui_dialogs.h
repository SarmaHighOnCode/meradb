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
    std::string user() const { return user_; }
    bool passwordEmpty() const { return password_[0] == '\0'; }
    // Where the widgets were drawn in the last frame (screen pixels, centre), for the tests.
    enum Widget { kHost, kPort, kUser, kPassword, kDatabase, kCancel, kLocal, kConnect, kWidgetCount };
    ImVec2 centre(Widget w) const { return centres_[w]; }

private:
    void load(const wb::ConnectDefaults& defaults);
    void submit(wb::Session& session, bool local);

    bool shown_ = false;
    bool focusHost_ = false;
    char host_[256] = {0};
    char port_[8] = {0};
    char user_[256] = {0};
    char password_[256] = {0};
    char database_[256] = {0};
    ImVec2 centres_[kWidgetCount] = {};
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

// ---- the command menu (Ctrl+K) ----
struct CommandItem {
    enum class Kind { Action, Table, History };
    Kind kind = Kind::Action;
    std::string label;        // what is listed and filtered
    std::string detail;       // right side, muted: the key, the database, "history"
    Action action = Action::Run;
    wb::NodeKey table;        // Kind::Table
    std::string text;         // Kind::History: the statement
};
// Every term of `query` (split at blanks, ASCII case-insensitive) must occur in `text`; an empty query matches all.
bool commandMatches(const std::string& text, const std::string& query);
// Actions, then every table (DIKHAO * SE t SIRF 100), then the last 20 history entries, newest first.
std::vector<CommandItem> commandItems(wb::Session& session);

class CommandMenu {
public:
    void open(wb::Session& session);
    void close() { open_ = false; }
    bool isOpen() const { return open_; }
    void setQuery(const std::string& text);
    const std::string& query() const { return query_; }
    const std::vector<CommandItem>& items() const { return items_; }
    const std::vector<int>& visible() const { return visible_; }   // indexes into items(), in the order shown
    int highlighted() const { return highlighted_; }               // position in visible()
    const std::vector<ImVec2>& rowCentres() const { return rowCentres_; }   // the rows drawn in the last frame, screen pixels
    void move(int delta);
    // The chosen item (Enter or a click); the menu has closed by then. Esc and Ctrl+K close it without a choice.
    std::optional<CommandItem> draw(const Theme& theme, const FontSet& fonts, float scale);

private:
    void refilter();
    bool open_ = false;
    bool focusInput_ = false;
    int openedFrame_ = -1;   // the frame it was opened in: the key that opened it must not close it
    bool scrollToHighlight_ = false;
    char buffer_[256] = {0};
    std::string query_;
    std::vector<CommandItem> items_;
    std::vector<int> visible_;
    std::vector<ImVec2> rowCentres_;
    int highlighted_ = 0;
};

}  // namespace meradb::gui
