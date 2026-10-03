// cpp/include/meradb/wb_dialogs.h -- FTXUI drawing and key handling of the two modal dialogs.
#pragma once
#include "meradb/wb_form.h"
#include "meradb/wb_session.h"
#include "meradb/wb_text.h"
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <vector>

namespace meradb::wb {

// Draws `dialog` (an element that clears and centres its own box, as the two below do) on top of `base`.
ftxui::Element modalOverlay(ftxui::Element base, ftxui::Element dialog);

// tui.py ConnectScreen: a centred box 64 wide (or the terminal width - 2).
ftxui::Element renderConnectDialog(const ConnectForm& form);
// Applies one key to the form. Connect / Local / Cancel call session.connect(...) / session.closeModal().
// Returns true when the key was used (everything is swallowed by an open dialog except what the window routes first).
bool handleConnectEvent(ConnectForm& form, const ftxui::Event& event, Session& session);

// tui.py HelpScreen: a box 90 % of the screen with the key list and the language reference, scrollable.
class HelpDialog {
public:
    ftxui::Element render();
    bool onEvent(const ftxui::Event& event, Session& session);   // Esc / F1 / q close; arrows, pages, Home, End scroll
    void scrollWheel(int direction);                              // -1 up, +1 down (3 rows)
    void reset();                                                 // back to the top (called when it opens)
private:
    ScrollState scroll_;
    std::vector<Line> lines_;
    int width_ = -1;
};

}  // namespace meradb::wb
