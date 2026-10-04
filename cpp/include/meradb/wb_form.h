// cpp/include/meradb/wb_form.h -- the connect dialog's data: a one-line text field and the form of four fields.
#pragma once
#include "meradb/wb_session.h"
#include <array>
#include <string>
#include <utility>

namespace meradb::wb {

// A one-line text field: code-point cursor, insert, Backspace, Delete, Left/Right/Home/End. `password` shows '*' per character.
class LineEdit {
public:
    explicit LineEdit(std::string text = "", bool password = false) : text_(std::move(text)), password_(password) {
        cursor_ = length();
    }
    const std::string& text() const { return text_; }
    std::string shown() const;               // text, or one '*' per code point
    int cursor() const { return cursor_; }   // code points
    int length() const;
    bool isPassword() const { return password_; }
    void insert(const std::string& utf8);    // control characters ignored; with digitsOnly only 0-9 are kept
                                             // (the selected text, if any, is replaced; nothing kept = nothing replaced)
    void backspace();
    void del();
    void left();
    void right();
    void home();
    void end();
    void setText(const std::string& text);   // cursor to the end, nothing selected
    // Textual's Input selects its whole text when it gets the focus (select_on_focus): the next character typed, Backspace or
    // Delete replaces / removes it, Left / Home go to the start, Right / End to the end, and every one of them ends the selection.
    void selectAll();                        // no effect on an empty field
    bool selected() const { return selected_; }
    bool digitsOnly = false;                 // the Port field: only 0-9 are accepted (Textual: type="integer")
private:
    std::string text_;
    bool password_;
    int cursor_ = 0;
    bool selected_ = false;                  // the whole text is selected
};

enum class FormAction { None, Connect, Local, Cancel };

// tui.py ConnectScreen: Host, Port, Password, Database, then the buttons Connect / Local mode / Cancel.
class ConnectForm {
public:
    static constexpr int kFields = 4;
    static constexpr int kButtons = 3;
    ConnectForm(const std::string& host, const std::string& port);   // password hidden; port digits only
    LineEdit& field(int i) { return fields_[static_cast<std::size_t>(i)]; }
    const LineEdit& field(int i) const { return fields_[static_cast<std::size_t>(i)]; }
    int active() const { return active_; }   // 0..3 fields, 4..6 buttons
    void next();                             // Tab / Down: wraps
    void previous();                         // Shift+Tab / Up: wraps
    void setActive(int index);
    // Enter: on a field = Connect (Textual: Input.Submitted); on a button = that button. Escape = Cancel.
    FormAction enter() const;
    FormAction escape() const { return FormAction::Cancel; }
    ConnectRequest request(bool local) const;   // makeConnectRequest(local, host, port, password, database)
private:
    void focusChanged();
    std::array<LineEdit, 4> fields_;
    int active_ = 0;
};

}  // namespace meradb::wb
