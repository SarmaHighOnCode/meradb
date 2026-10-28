// cpp/src/gui_options.cpp -- see gui_options.h.
#include "meradb/gui_options.h"
#include <cctype>
#include <utility>

namespace meradb::gui {

std::string guiUsage() {
    return "usage: meradb_gui [-h] [-D DATA] [-H HOST] [-p PORT] [-d DATABASE] [-W] [-U USER]\n"
           "                  [--local] [--version]\n";
}

std::string guiHelp(const std::string& dataDir) {
    std::string out = guiUsage();
    out += "\n";
    out += "MeraDB Workbench -- Hinglish desktop window (terminal wala: meradb workbench).\n";
    out += "\n";
    out += "options:\n";
    out += "  -h, --help            show this help message and exit\n";
    out += "  --version             show program's version number and exit\n";
    out += "  -D DATA, --data DATA  data folder (default: " + dataDir + ", ya MERADB_DATA)\n";
    out += "  -H HOST, --host HOST  server ka address (default 127.0.0.1, ya MERADB_HOST)\n";
    out += "  -p PORT, --port PORT  server ka port (default 6372, ya MERADB_PORT)\n";
    out += "  -d DATABASE, --database DATABASE\n";
    out += "                        shuru mein ye database istemal karo\n";
    out += "  -W, --password        password poocho (ya MERADB_PASSWORD)\n";
    out += "  -U USER, --user USER  is USERNAME se login karo (ya MERADB_USER) --\n";
    out += "                        privileges ke saath, server-wide password ki jagah\n";
    out += "  --local               server ke bina, seedha data folder kholo\n";
    return out;
}

std::optional<std::pair<int, int>> parseSize(const std::string& text) {
    const std::size_t x = text.find_first_of("xX");
    if (x == std::string::npos || x == 0 || x + 1 >= text.size()) return std::nullopt;
    auto number = [](const std::string& digits) -> std::optional<int> {
        if (digits.empty() || digits.size() > 5) return std::nullopt;
        int value = 0;
        for (unsigned char c : digits) {
            if (!std::isdigit(c)) return std::nullopt;
            value = value * 10 + (c - '0');
        }
        return value;
    };
    const std::optional<int> w = number(text.substr(0, x));
    const std::optional<int> h = number(text.substr(x + 1));
    if (!w || !h || *w < 320 || *h < 240 || *w > 8192 || *h > 8192) return std::nullopt;
    return std::make_pair(*w, *h);
}

GuiOptions parseGuiArgs(std::vector<std::string> argv) {
    GuiOptions options;
    std::vector<std::string> rest;   // what parseCliArgs gets after the command word
    // The extra options are taken out first, so the shared parser never sees them. A value is the next word or
    // follows "=" (like argparse).
    for (std::size_t i = 0; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (arg == "--version") {
            options.showVersion = true;
            continue;
        }
        if (arg == "--expand-tables") {
            options.expandTables = true;
            continue;
        }
        const char* names[] = {"--screenshot", "--script", "--theme", "--size", "--editor"};
        const char* matched = nullptr;
        std::string inlineValue;
        bool hasInline = false;
        for (const char* name : names) {
            const std::string n = name;
            if (arg == n) {
                matched = name;
                break;
            }
            if (arg.size() > n.size() && arg.compare(0, n.size(), n) == 0 && arg[n.size()] == '=') {
                matched = name;
                hasInline = true;
                inlineValue = arg.substr(n.size() + 1);
                break;
            }
        }
        if (matched == nullptr) {
            rest.push_back(arg);
            continue;
        }
        const std::string name = matched;
        std::string value;
        if (hasInline) {
            value = inlineValue;
        } else if (i + 1 < argv.size()) {
            value = argv[++i];
        } else {
            if (options.error.empty()) options.error = "argument " + name + ": expected one argument";
            continue;
        }
        if (!options.error.empty()) continue;   // report the first problem only
        if (name == "--screenshot") {
            options.screenshot = value;
        } else if (name == "--script") {
            options.script = value;
        } else if (name == "--editor") {
            options.editorText = value;
        } else if (name == "--theme") {
            const std::optional<ThemeMode> mode = parseThemeName(value);
            if (!mode) options.error = "argument --theme: invalid choice: '" + value + "' (choose from 'light', 'dark')";
            else options.theme = mode;
        } else {
            const auto size = parseSize(value);
            if (!size) options.error = "argument --size: invalid size: '" + value + "' (use WIDTHxHEIGHT, e.g. 1280x800)";
            else {
                options.width = size->first;
                options.height = size->second;
            }
        }
    }

    std::vector<std::string> cliArgv;
    cliArgv.push_back("workbench");
    cliArgv.insert(cliArgv.end(), rest.begin(), rest.end());
    options.cli = parseCliArgs(std::move(cliArgv));
    if (options.cli.showHelp) options.showHelp = true;
    if (options.error.empty() && !options.cli.error.empty()) options.error = options.cli.error;
    return options;
}

}  // namespace meradb::gui
