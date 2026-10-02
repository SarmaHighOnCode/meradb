// cpp/include/meradb/term_style.h
//
// Colour for the command line (mirrors the colour helpers at the top of meradb/repl.py): plain ANSI
// escape codes, used only when stdout is a terminal that understands them. NO_COLOR (no-color.org) and
// redirected output both turn them off, so scripted output and test captures stay plain text.
#pragma once
#include <functional>
#include <initializer_list>
#include <string>

namespace meradb::term {

class Style {
public:
    Style() = default;
    explicit Style(bool on) : on_(on) {}

    static Style none() { return Style(false); }
    static Style colored() { return Style(true); }

    bool on() const { return on_; }

    // `text` wrapped in ANSI SGR codes (1 bold, 2 dim, 31 red, 32 green, 33 yellow, 35 magenta, 36 cyan),
    // or unchanged when colour is off or no code is given. Python: _c(text, *codes).
    std::string c(const std::string& text, std::initializer_list<int> codes) const;

private:
    bool on_ = false;
};

// Everything Python's _supports_color() looks at, as plain values so the rule can be tested.
struct ColorEnv {
    bool noColor = false;           // NO_COLOR is set to something non-empty
    bool stdoutIsTerminal = false;  // stdout is a terminal, not a pipe or a file
    bool windows = false;
    bool windowsAnsiHint = false;   // WT_SESSION or TERM non-empty, or ConEmuANSI == "ON"
    // Classic Windows console host only: ask it to render escape codes. Called at most once, last.
    std::function<bool()> enableConsoleAnsi;
};

bool supportsColor(const ColorEnv& env);

// The ColorEnv of this process.
ColorEnv currentColorEnv(std::function<bool()> enableConsoleAnsi);

// Style::colored() when supportsColor(currentColorEnv(...)), otherwise Style::none().
Style detectStyle(std::function<bool()> enableConsoleAnsi);

}  // namespace meradb::term
