// cpp/tests/tools/wb_keyprobe.cpp -- run it in a real terminal: shows what FTXUI delivers for each key.
// Press the keys of the workbench (F1 F5 F6, Ctrl+Up/Down, Shift+arrows, Ctrl+S/R/O/L/Q/P/N/A/C, Esc, Tab,
// Shift+Tab, PageUp); press 'x' to quit.
//
//   wb_keyprobe           the terminal exactly as FTXUI leaves it (note what Ctrl+C / Ctrl+S / Ctrl+Q do)
//   wb_keyprobe --guard   with the workbench's sys::TerminalModeGuard: Ctrl+C, Ctrl+S, Ctrl+Q, Ctrl+O and Ctrl+R must
//                         show up as bytes 03 13 11 0F 12 and must neither end nor freeze the probe
#include "meradb/sys_compat.h"
#include "meradb/wb_keys.h"
#include <cstdio>
#include <deque>
#include <memory>
#include <string>
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

using namespace ftxui;

namespace {
std::string hex(const std::string& s) {
    std::string out;
    char buf[8];
    for (unsigned char c : s) {
        std::snprintf(buf, sizeof buf, "%02X ", c);
        out += buf;
    }
    return out;
}
std::string nameOf(const Event& e) {
    struct Named { const char* name; Event event; };
    const Named known[] = {{"ArrowUpCtrl", Event::ArrowUpCtrl}, {"ArrowDownCtrl", Event::ArrowDownCtrl},
                           {"ArrowLeftCtrl", Event::ArrowLeftCtrl}, {"ArrowRightCtrl", Event::ArrowRightCtrl},
                           {"F1", Event::F1}, {"F5", Event::F5}, {"F6", Event::F6}, {"Tab", Event::Tab},
                           {"TabReverse", Event::TabReverse}, {"Escape", Event::Escape}, {"Return", Event::Return},
                           {"Backspace", Event::Backspace}, {"Delete", Event::Delete}, {"PageUp", Event::PageUp},
                           {"Home", Event::Home}, {"End", Event::End}};
    for (const auto& k : known)
        if (e == k.event) return std::string(" = ") + k.name;
    return "";
}

// What the workbench does with the event (wb_keys.h), so a terminal's keys can be checked against the tables.
std::string meaningOf(const Event& raw) {
    using namespace meradb::wb;
    const Event e = keys::normalize(raw);
    struct Named { keys::Action action; const char* name; };
    const Named actions[] = {{keys::Action::Run, "Run"}, {keys::Action::Explain, "Explain"},
                             {keys::Action::HistoryPrev, "HistoryPrev"}, {keys::Action::HistoryNext, "HistoryNext"},
                             {keys::Action::Export, "Export"}, {keys::Action::Connect, "Connect"},
                             {keys::Action::ClearLog, "ClearLog"}, {keys::Action::Help, "Help"},
                             {keys::Action::Quit, "Quit"}, {keys::Action::Interrupt, "Interrupt"},
                             {keys::Action::SelectAll, "SelectAll"}};
    for (const auto& a : actions)
        if (keys::matches(a.action, e)) return std::string(" -> action ") + a.name;
    if (const keys::EditorKey* k = keys::findEditorKey(e)) {
        static const char* moves[] = {"Left", "Right", "Up", "Down", "Home", "End", "DocStart", "DocEnd",
                                      "PageUp", "PageDown", "WordLeft", "WordRight"};
        return std::string(" -> editor ") + moves[static_cast<int>(k->move)] + (k->extendSelection ? " +select" : "");
    }
    return "";
}
}  // namespace

int main(int argc, char** argv) {
    meradb::sys::Utf8Console console;
    const bool useGuard = argc > 1 && std::string(argv[1]) == "--guard";
    std::unique_ptr<meradb::sys::TerminalModeGuard> guard;   // before the loop, like the workbench
    if (useGuard) guard.reset(new meradb::sys::TerminalModeGuard);
    auto screen = ScreenInteractive::Fullscreen();
    std::deque<std::string> seen;
    auto view = Renderer([&] {
        Elements rows;
        rows.push_back(text("Key probe: press keys, newest first; 'x' quits.") | bold);
        for (const auto& line : seen) rows.push_back(text(line));
        return vbox(std::move(rows)) | border;
    });
    auto app = CatchEvent(view, [&](Event e) {
        if (e == Event::Character('x')) {
            screen.ExitLoopClosure()();
            return true;
        }
        std::string line = "bytes: " + hex(e.input()) + (e.is_character() ? "[character] " : "") +
                           (e.is_mouse() ? "[mouse] " : "") + nameOf(e) + meaningOf(e);
        seen.push_front(line);
        if (seen.size() > 20) seen.pop_back();
        return true;
    });
    screen.Loop(app);
    return 0;
}
