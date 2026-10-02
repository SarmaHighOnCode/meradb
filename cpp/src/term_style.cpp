// cpp/src/term_style.cpp -- see term_style.h.
#include "meradb/term_style.h"
#include "meradb/sys_compat.h"

namespace meradb::term {

std::string Style::c(const std::string& text, std::initializer_list<int> codes) const {
    if (!on_ || codes.size() == 0) return text;
    std::string joined;
    for (int code : codes) {
        if (!joined.empty()) joined += ';';
        joined += std::to_string(code);
    }
    return "\x1b[" + joined + "m" + text + "\x1b[0m";
}

bool supportsColor(const ColorEnv& env) {
    if (env.noColor || !env.stdoutIsTerminal) return false;
    if (!env.windows) return true;  // real terminals on Linux and macOS understand ANSI as standard
    // Modern terminal programs on Windows already understand ANSI; asking the classic console host
    // below could wrongly report "unsupported" on one of them.
    if (env.windowsAnsiHint) return true;
    return env.enableConsoleAnsi ? env.enableConsoleAnsi() : false;
}

ColorEnv currentColorEnv(std::function<bool()> enableConsoleAnsi) {
    const auto nonEmpty = [](const char* name) {
        const auto value = sys::getEnv(name);
        return value.has_value() && !value->empty();
    };
    ColorEnv env;
    env.noColor = nonEmpty("NO_COLOR");
    env.stdoutIsTerminal = sys::isTerminal(1);
#ifdef _WIN32
    env.windows = true;
#endif
    const auto conEmu = sys::getEnv("ConEmuANSI");
    env.windowsAnsiHint = nonEmpty("WT_SESSION") || nonEmpty("TERM") || (conEmu.has_value() && *conEmu == "ON");
    env.enableConsoleAnsi = std::move(enableConsoleAnsi);
    return env;
}

Style detectStyle(std::function<bool()> enableConsoleAnsi) {
    return supportsColor(currentColorEnv(std::move(enableConsoleAnsi))) ? Style::colored() : Style::none();
}

}  // namespace meradb::term
