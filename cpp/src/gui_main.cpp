// meradb_gui: the desktop workbench window (see gui_app.h and docs/CPP.md).
//
// On Windows this is a GUI-subsystem program: double-clicking it opens no console window. Started from a terminal
// it attaches to that terminal's console so --help, --version and error messages print there; with no console at
// all (a double-click) the same text goes to a message box.
#include "meradb/gui_app.h"
#include "meradb/sys_compat.h"
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {

#ifdef _WIN32
bool usable(HANDLE handle) { return handle != nullptr && handle != INVALID_HANDLE_VALUE; }

// True when text written to stdout / stderr can reach somebody: the streams were redirected (a pipe or a file), or
// this process could attach to the console of the terminal that started it.
bool attachParentConsole() {
    const bool haveOut = usable(GetStdHandle(STD_OUTPUT_HANDLE));
    const bool haveErr = usable(GetStdHandle(STD_ERROR_HANDLE));
    if (haveOut && haveErr) return true;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return haveOut || haveErr;
    if (!haveOut) std::freopen("CONOUT$", "w", stdout);
    if (!haveErr) std::freopen("CONOUT$", "w", stderr);
    if (!usable(GetStdHandle(STD_INPUT_HANDLE))) std::freopen("CONIN$", "r", stdin);
    return true;
}
#endif

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    const bool haveConsole = attachParentConsole();
#else
    const bool haveConsole = true;
#endif
    meradb::sys::Utf8Console console;   // UTF-8 on a Windows console; untouched when piped
    const meradb::gui::Reporter reporter(haveConsole);
    std::vector<std::string> args = meradb::sys::commandLineArgs(argc, argv);
    return meradb::gui::guiMain(std::move(args), reporter);
}
