// meradb_cli: local-mode script runner (mirrors `meradb run --local`).
//
//   meradb_cli run <script.mdb> [more.mdb ...] [--data <dir>]
//
// Each statement's result is printed followed by a blank line. A failing
// statement prints its error and the next ones still run; the exit code is 1
// if any statement (or file) failed.
#include "meradb/cli_format.h"
#include "meradb/engine.h"
#include "meradb/sys_compat.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Python's text mode (universal newlines): "\r\n" and a lone "\r" both become "\n".
std::string normalizeNewlines(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\r') {
            out += '\n';
            if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
        } else {
            out += in[i];
        }
    }
    return out;
}

bool runFile(meradb::Engine& engine, const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        // Same wording as Python's OSError text: "[Errno 2] No such file or directory: 'x'"
        std::string quoted;
        for (char c : path) {
            if (c == '\\' || c == '\'') quoted += '\\';
            quoted += c;
        }
        std::error_code ec;
        bool missing = !std::filesystem::exists(std::filesystem::u8path(path), ec);
        std::cout << "File nahi khuli: "
                  << (missing ? "[Errno 2] No such file or directory: '" : "[Errno 13] Permission denied: '")
                  << quoted << "'\n";
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string text = buffer.str();
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);  // utf-8-sig, like Python
    text = normalizeNewlines(text);

    bool ok = true;
    for (const auto& result : engine.runScript(text)) {
        std::string out = meradb::formatResult(result);
        if (!out.empty()) std::cout << out << "\n";
        std::cout << "\n";
        if (!result.error.empty()) ok = false;
    }
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    // Same default as Python's default_data_dir(): MERADB_DATA, else a per-user folder.
    std::string dataDir;
    if (auto envData = meradb::sys::getEnv("MERADB_DATA"); envData && !envData->empty()) {
        dataDir = *envData;
    } else {
        std::filesystem::path base = std::filesystem::path(meradb::sys::homeDir()) / ".local" / "share";
#ifdef _WIN32
        if (auto local = meradb::sys::getEnv("LOCALAPPDATA"); local && !local->empty())
            base = std::filesystem::path(*local);
#endif
        dataDir = (base / "MeraDB" / "data").string();
    }
    std::vector<std::string> files;
    bool runCommand = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "run") runCommand = true;
        else if ((arg == "--data" || arg == "-D") && i + 1 < argc) dataDir = argv[++i];
        else if (arg == "--local") continue;
        else if (runCommand) files.push_back(arg);
    }
    if (!runCommand || files.empty()) {
        std::cerr << "Usage: meradb_cli run <script.mdb> [more.mdb ...] [--data <dir>]\n";
        return 1;
    }

    try {
        meradb::Engine engine(dataDir);
        for (const auto& db : engine.instance().recovered())
            std::cerr << "RECOVERY: database '" << db << "' ka adhoora transaction WAPAS kiya (pichli baar crash hua tha)\n";
        bool ok = true;
        for (const auto& path : files)
            if (!runFile(engine, path)) ok = false;  // run ALL files, even after a failure
        engine.close();
        return ok ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
