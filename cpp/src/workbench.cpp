// cpp/src/workbench.cpp -- runs the full-screen workbench: FTXUI loop on this thread, statements on the session's worker.
//
// The terminal is owned by FTXUI while the loop runs. Neither sys::AnsiConsole nor sys::InterruptGuard /
// ConsoleLineSource (the shell's console helpers) may be used in here: they change the same console modes. The Windows
// console code page stays at UTF-8 through main()'s sys::Utf8Console for the whole run.
#include "meradb/workbench.h"
#include "meradb/sys_compat.h"
#include "meradb/wb_session.h"
#include "meradb/wb_ui.h"
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <iostream>
#include <mutex>

namespace meradb::wb {

namespace {

// Hands closures from the worker to the UI thread, and stops once the loop has ended (a closure posted after
// Loop() returns would be dropped by FTXUI anyway; this makes it explicit and race-free). The Session's own alive
// token makes a late delivery harmless; this only keeps the worker from talking to a finished screen.
class UiBridge {
public:
    explicit UiBridge(ftxui::ScreenInteractive* screen) : screen_(screen) {}
    void post(std::function<void()> f) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!open_) return;
        // FTXUI redraws after events, not after posted closures, so every closure is followed by a no-op event:
        // without it a finished statement would not show until the next key press.
        ftxui::ScreenInteractive* screen = screen_;
        screen_->Post([screen, f = std::move(f)]() {
            f();
            screen->PostEvent(ftxui::Event::Custom);
        });
    }
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        open_ = false;
    }

private:
    std::mutex mutex_;
    ftxui::ScreenInteractive* screen_;
    bool open_ = true;
};

}  // namespace

int runWorkbench(std::unique_ptr<Backend> backend, const CliArgs& args) {
    ftxui::ScreenInteractive screen = ftxui::ScreenInteractive::Fullscreen();
    UiBridge bridge(&screen);
    SessionOptions options;
    options.dataDir = args.dataDir;
    Session session(std::move(backend), options, [&bridge](std::function<void()> f) { bridge.post(std::move(f)); });
    // Quit protocol (wb_session.h): Ctrl+Q calls requestQuit(); once the running statement is done and the backend is
    // closed the session calls this, on the UI thread, and the loop ends.
    // Close the bridge first: Exit() makes FTXUI drop its task channel, which the worker must not be posting into then.
    session.setOnExit([&screen, &bridge] {
        bridge.close();
        screen.Exit();
    });
    WorkbenchUi ui(session);
    {
        sys::TerminalModeGuard guard;  // before Loop (see its header comment)
        screen.Loop(ui.component());
    }
    bridge.close();
    if (session.busy())
        std::cerr << "Chalta hua statement khatam hone ka intezaar hai, phir transaction WAPAS hoga ...\n";
    session.shutdown();  // waits for the worker, closes the backend (rolls an open transaction back)
    return 0;
}

}  // namespace meradb::wb
