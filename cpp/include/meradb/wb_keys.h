// cpp/include/meradb/wb_keys.h -- what each key means. The only place that knows event bytes.
//
// Everything is table driven: to change a binding (for a terminal that sends other bytes) edit the tables in
// bindings() / editorKeys() below; nothing else in the workbench knows a byte sequence. The tables list several
// sequences per key where terminals disagree (xterm, rxvt, the Windows console).
#pragma once
#include "meradb/wb_editor.h"
#include <ftxui/component/event.hpp>
#include <string>
#include <vector>

namespace meradb::wb::keys {

inline ftxui::Event ctrl(char letter) { return ftxui::Event::Special(std::string(1, static_cast<char>(letter & 0x1f))); }

// Shift + arrows / Home / End as xterm-style terminals (and Windows Terminal) send them: ESC [ 1 ; 2 <final>.
inline const std::string kShiftUp = "\x1b[1;2A", kShiftDown = "\x1b[1;2B", kShiftRight = "\x1b[1;2C",
                         kShiftLeft = "\x1b[1;2D", kShiftHome = "\x1b[1;2H", kShiftEnd = "\x1b[1;2F";

// ---- global bindings (they work whichever panel has the focus, as Python's priority bindings do) ----
enum class Action { Run, Explain, HistoryPrev, HistoryNext, Export, Connect, ClearLog, Help, Quit, Interrupt, SelectAll };

struct Binding {
    Action action;
    std::vector<ftxui::Event> events;   // any of these triggers the action
};

inline const std::vector<Binding>& bindings() {
    static const std::vector<Binding> table = {
        {Action::Run, {ftxui::Event::F5, ctrl('R')}},
        {Action::Explain, {ftxui::Event::F6}},
        // Ctrl+Up / Ctrl+Down are not delivered by every terminal, so Ctrl+P / Ctrl+N are aliases.
        {Action::HistoryPrev, {ftxui::Event::ArrowUpCtrl, ctrl('P')}},
        {Action::HistoryNext, {ftxui::Event::ArrowDownCtrl, ctrl('N')}},
        {Action::Export, {ctrl('S')}},
        {Action::Connect, {ctrl('O')}},
        {Action::ClearLog, {ctrl('L')}},
        {Action::Help, {ftxui::Event::F1}},
        {Action::Quit, {ctrl('Q')}},
        {Action::Interrupt, {ctrl('C')}},
        {Action::SelectAll, {ctrl('A')}},
    };
    return table;
}

inline bool matches(Action action, const ftxui::Event& e) {
    for (const Binding& b : bindings())
        if (b.action == action)
            for (const ftxui::Event& candidate : b.events)
                if (candidate == e) return true;
    return false;
}

inline bool isRun(const ftxui::Event& e) { return matches(Action::Run, e); }
inline bool isExplain(const ftxui::Event& e) { return matches(Action::Explain, e); }
inline bool isHistoryPrev(const ftxui::Event& e) { return matches(Action::HistoryPrev, e); }
inline bool isHistoryNext(const ftxui::Event& e) { return matches(Action::HistoryNext, e); }
inline bool isExport(const ftxui::Event& e) { return matches(Action::Export, e); }
inline bool isConnect(const ftxui::Event& e) { return matches(Action::Connect, e); }
inline bool isClearLog(const ftxui::Event& e) { return matches(Action::ClearLog, e); }
inline bool isHelp(const ftxui::Event& e) { return matches(Action::Help, e); }
inline bool isQuit(const ftxui::Event& e) { return matches(Action::Quit, e); }
inline bool isInterrupt(const ftxui::Event& e) { return matches(Action::Interrupt, e); }
inline bool isSelectAll(const ftxui::Event& e) { return matches(Action::SelectAll, e); }

// ---- editor cursor keys ----
struct EditorKey {
    ftxui::Event event;
    Move move;
    bool extendSelection;
};

inline const std::vector<EditorKey>& editorKeys() {
    static const std::vector<EditorKey> table = {
        {ftxui::Event::ArrowLeft, Move::Left, false},
        {ftxui::Event::ArrowRight, Move::Right, false},
        {ftxui::Event::ArrowUp, Move::Up, false},
        {ftxui::Event::ArrowDown, Move::Down, false},
        {ftxui::Event::Home, Move::Home, false},
        {ftxui::Event::End, Move::End, false},
        {ftxui::Event::PageUp, Move::PageUp, false},
        {ftxui::Event::PageDown, Move::PageDown, false},
        {ftxui::Event::ArrowLeftCtrl, Move::WordLeft, false},
        {ftxui::Event::ArrowRightCtrl, Move::WordRight, false},
        {ftxui::Event::Special("\x1b[1;5H"), Move::DocStart, false},   // Ctrl+Home
        {ftxui::Event::Special("\x1b[1;5F"), Move::DocEnd, false},     // Ctrl+End
        // Shift + movement. Alternative sequences: rxvt sends ESC [ a..d.
        {ftxui::Event::Special(kShiftLeft), Move::Left, true},
        {ftxui::Event::Special(kShiftRight), Move::Right, true},
        {ftxui::Event::Special(kShiftUp), Move::Up, true},
        {ftxui::Event::Special(kShiftDown), Move::Down, true},
        {ftxui::Event::Special(kShiftHome), Move::Home, true},
        {ftxui::Event::Special(kShiftEnd), Move::End, true},
        {ftxui::Event::Special("\x1b[a"), Move::Up, true},
        {ftxui::Event::Special("\x1b[b"), Move::Down, true},
        {ftxui::Event::Special("\x1b[c"), Move::Right, true},
        {ftxui::Event::Special("\x1b[d"), Move::Left, true},
        {ftxui::Event::Special("\x1b[1;6D"), Move::WordLeft, true},    // Ctrl+Shift+Left
        {ftxui::Event::Special("\x1b[1;6C"), Move::WordRight, true},   // Ctrl+Shift+Right
    };
    return table;
}

inline const EditorKey* findEditorKey(const ftxui::Event& e) {
    for (const EditorKey& k : editorKeys())
        if (k.event == e) return &k;
    return nullptr;
}

}  // namespace meradb::wb::keys
