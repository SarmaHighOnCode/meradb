// cpp/include/meradb/gui_app.h -- the program: arguments, the window, the main loop, screenshots.
#pragma once
#include "meradb/backend.h"
#include "meradb/gui_options.h"
#include <memory>
#include <string>
#include <vector>

namespace meradb::gui {

// Exit codes (besides 0 = normal, 1 = an error, 2 = bad arguments).
constexpr int kExitNoDisplay = 3;   // no usable display or OpenGL 3: the window could not be created

// Where text for the user goes: the terminal that started the program when there is one, else a message box
// (Windows) -- a program started by double-click has no console.
class Reporter {
public:
    explicit Reporter(bool haveConsole) : haveConsole_(haveConsole) {}
    void info(const std::string& text) const;    // --help, --version
    void error(const std::string& text) const;   // anything that stops the program
    bool haveConsole() const { return haveConsole_; }

private:
    bool haveConsole_;
};

// main() after the console is set up: parses `argv` (without the program name), handles --help / --version / bad
// arguments, opens the backend exactly like `meradb workbench` and runs the window. Returns the exit code.
int guiMain(std::vector<std::string> argv, const Reporter& reporter);

// Opens the window for an already opened backend and runs until the user quits (or, with --screenshot, until the
// picture is written). Takes ownership of the backend and closes it.
int runGui(std::unique_ptr<Backend> backend, const GuiOptions& options, const Reporter& reporter);

}  // namespace meradb::gui
