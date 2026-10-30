// cpp/src/gui_app.cpp -- see gui_app.h.
#include "meradb/gui_app.h"
#include "meradb/errors.h"
#include "meradb/protocol.h"
#include "meradb/sys_compat.h"
#include <iostream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace meradb::gui {

namespace {

#ifdef _WIN32
std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), &out[0], n);
    return out;
}
#endif

}  // namespace

void Reporter::info(const std::string& text) const {
#ifdef _WIN32
    if (!haveConsole_) {
        MessageBoxW(nullptr, widen(text).c_str(), L"MeraDB Workbench", MB_OK | MB_ICONINFORMATION);
        return;
    }
#endif
    std::cout << text;
    if (text.empty() || text.back() != '\n') std::cout << "\n";
    std::cout.flush();
}

void Reporter::error(const std::string& text) const {
#ifdef _WIN32
    if (!haveConsole_) {
        MessageBoxW(nullptr, widen(text).c_str(), L"MeraDB Workbench", MB_OK | MB_ICONERROR);
        return;
    }
#endif
    std::cerr << text;
    if (text.empty() || text.back() != '\n') std::cerr << "\n";
    std::cerr.flush();
}

int guiMain(std::vector<std::string> argv, const Reporter& reporter) {
    const GuiOptions options = parseGuiArgs(std::move(argv));
    if (options.showHelp) {
        reporter.info(guiHelp(!sys::getEnv("MERADB_DATA") && !projectDataDir().empty() ? projectDataDir() : protocol::defaultDataDir()));
        return 0;
    }
    if (options.showVersion) {
        reporter.info(protocol::kServerName);
        return 0;
    }
    if (!options.error.empty()) {
        reporter.error(guiUsage() + "meradb_gui: error: " + options.error);
        return 2;
    }
    if (options.cli.askPassword && !reporter.haveConsole()) {
        reporter.error("-W ke liye terminal chahiye (password wahin poocha jata hai). Terminal se chalao, ya MERADB_PASSWORD set karo.");
        return 1;
    }
    std::unique_ptr<Backend> backend;
    try {
        backend = openBackend(options.cli);   // notes on stderr, like `meradb workbench`; may throw MeraDBError
    } catch (const std::exception& e) {
        reporter.error(e.what());
        return 1;
    }
    return runGui(std::move(backend), options, reporter);
}

}  // namespace meradb::gui
