// cpp/src/workbench.cpp -- runs the full-screen workbench: FTXUI loop on this thread, statements on the session's worker.
//
// The terminal is owned by FTXUI while the loop runs. Neither sys::AnsiConsole nor sys::InterruptGuard /
// ConsoleLineSource (the shell's console helpers) may be used in here: they change the same console modes. The Windows
// console code page stays at UTF-8 through main()'s sys::Utf8Console for the whole run.
#include "meradb/workbench.h"
#include "meradb/sys_compat.h"
#include "meradb/wb_bridge.h"
#include "meradb/wb_session.h"
#include "meradb/wb_ui.h"
#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <cstring>
#include <iostream>
#ifndef _WIN32
#include <signal.h>
#endif

namespace meradb::wb {

namespace {

// The first frame drawn means the loop is running (its task channel exists), so that is the moment the bridge starts
// forwarding what the worker has already posted (the start-up schema load usually is).
class RootWithStart : public ftxui::ComponentBase {
public:
    RootWithStart(ftxui::Component inner, std::function<void()> onFirstFrame)
        : inner_(std::move(inner)), onFirstFrame_(std::move(onFirstFrame)) {}
    ftxui::Element Render() override {
        if (onFirstFrame_) {
            std::function<void()> once = std::move(onFirstFrame_);
            onFirstFrame_ = nullptr;
            once();
        }
        return inner_->Render();
    }
    bool OnEvent(ftxui::Event event) override { return inner_->OnEvent(std::move(event)); }
    bool Focusable() const override { return true; }

private:
    ftxui::Component inner_;
    std::function<void()> onFirstFrame_;
};

// Closes the bridge on every way out of runWorkbench (normal end, a signal that ended the loop, an exception).
struct BridgeCloser {
    UiBridge& bridge;
    ~BridgeCloser() { bridge.close(); }
};

// Asks the terminal to wrap pasted text in ESC[200~ ... ESC[201~ (FTXUI 5 does not), so a pasted Tab can be told from a
// typed one. Terminals that do not know the mode ignore it.
struct BracketedPaste {
    BracketedPaste() { std::cout << "\x1b[?2004h" << std::flush; }
    ~BracketedPaste() { std::cout << "\x1b[?2004l" << std::flush; }
};

#ifndef _WIN32
// SIGHUP (the terminal went away) ends the loop like Ctrl+Q does, so the code after it waits for the worker and closes
// the backend, which rolls an open transaction back. (FTXUI itself only handles TERM, INT, SEGV, ILL, ABRT, FPE.)
ftxui::ScreenInteractive* g_hangupScreen = nullptr;
extern "C" void onHangup(int) {
    ftxui::ScreenInteractive* screen = g_hangupScreen;
    if (screen) screen->Exit();
}
class HangupHandler {
public:
    explicit HangupHandler(ftxui::ScreenInteractive* screen) {
        g_hangupScreen = screen;
        struct sigaction action;
        std::memset(&action, 0, sizeof action);
        action.sa_handler = onHangup;
        sigemptyset(&action.sa_mask);
        installed_ = ::sigaction(SIGHUP, &action, &previous_) == 0;
    }
    ~HangupHandler() {
        if (installed_) ::sigaction(SIGHUP, &previous_, nullptr);
        g_hangupScreen = nullptr;
    }
private:
    struct sigaction previous_;
    bool installed_ = false;
};
#endif

}  // namespace

int runWorkbench(std::unique_ptr<Backend> backend, const CliArgs& args) {
    ftxui::ScreenInteractive screen = ftxui::ScreenInteractive::Fullscreen();
    // FTXUI redraws after events, not after posted closures, so every closure is followed by a no-op event: without
    // it a finished statement would not show until the next key press.
    UiBridge bridge([&screen](UiBridge::Task task) {
        screen.Post([&screen, task = std::move(task)]() {
            task();
            screen.PostEvent(ftxui::Event::Custom);
        });
    });
    SessionOptions options;
    options.dataDir = args.dataDir;
    Session session(std::move(backend), options, [&bridge](std::function<void()> f) { bridge.post(std::move(f)); });
    BridgeCloser closer{bridge};  // declared after the session: it closes first when an exception unwinds
    // Quit protocol (wb_session.h): Ctrl+Q calls requestQuit(); once the running statement is done and the backend is
    // closed the session calls this, on the UI thread, and the loop ends.
    // Close the bridge first: Exit() makes FTXUI drop its task channel, which the worker must not be posting into then.
    session.setOnExit([&screen, &bridge] {
        bridge.close();
        screen.Exit();
    });
    WorkbenchUi ui(session);
    ftxui::Component root = std::make_shared<RootWithStart>(ui.component(), [&bridge] { bridge.start(); });
    {
        sys::TerminalModeGuard guard;  // before Loop (see its header comment)
        BracketedPaste paste;
#ifndef _WIN32
        HangupHandler hangup(&screen);
#endif
        screen.Loop(root);
    }
    bridge.close();
    if (session.busy())
        std::cerr << "Chalta hua statement khatam hone ka intezaar hai, phir transaction WAPAS hoga ...\n";
    session.shutdown();  // waits for the worker, closes the backend (rolls an open transaction back)
    return 0;
}

}  // namespace meradb::wb
