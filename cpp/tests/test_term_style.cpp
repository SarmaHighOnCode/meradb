// cpp/tests/test_term_style.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/sys_compat.h"
#include "meradb/term_style.h"
#include <optional>
#include <vector>

using namespace meradb;

TEST_CASE("term Style wraps text in SGR codes only when colour is on", "[term]") {
    CHECK(term::Style::colored().c("hi", {1, 36}) == "\x1b[1;36mhi\x1b[0m");
    CHECK(term::Style::colored().c("hi", {2}) == "\x1b[2mhi\x1b[0m");
    CHECK(term::Style::colored().c("hi", {}) == "hi");
    CHECK(term::Style::none().c("hi", {1, 36}) == "hi");
    CHECK(term::Style().c("hi", {31}) == "hi");
    CHECK(term::Style::colored().on());
    CHECK_FALSE(term::Style::none().on());
}

TEST_CASE("term supportsColor follows the Python rule", "[term]") {
    term::ColorEnv env;
    CHECK_FALSE(term::supportsColor(env));  // not a terminal
    env.stdoutIsTerminal = true;
    CHECK(term::supportsColor(env));        // a Linux / macOS terminal
    env.noColor = true;
    CHECK_FALSE(term::supportsColor(env));  // NO_COLOR wins
    env.noColor = false;

    env.windows = true;  // from here on: Windows
    int asked = 0;
    env.enableConsoleAnsi = [&] {
        ++asked;
        return true;
    };
    env.windowsAnsiHint = true;
    CHECK(term::supportsColor(env));  // Windows Terminal, mintty, ConEmu: trusted, the console is not asked
    CHECK(asked == 0);
    env.windowsAnsiHint = false;
    CHECK(term::supportsColor(env));  // classic console host: asked, says yes
    CHECK(asked == 1);
    env.enableConsoleAnsi = [&] {
        ++asked;
        return false;
    };
    CHECK_FALSE(term::supportsColor(env));  // ... says no
    CHECK(asked == 2);
    env.enableConsoleAnsi = nullptr;
    CHECK_FALSE(term::supportsColor(env));
    env.stdoutIsTerminal = false;
    env.windowsAnsiHint = true;
    CHECK_FALSE(term::supportsColor(env));  // a pipe stays plain whatever the hint
}

namespace {
// Restores the environment variables a test sets.
class EnvRestore {
public:
    explicit EnvRestore(std::vector<std::string> names) : names_(std::move(names)) {
        for (const auto& name : names_) saved_.push_back(sys::getEnv(name));
    }
    ~EnvRestore() {
        for (std::size_t i = 0; i < names_.size(); ++i) sys::setEnv(names_[i], saved_[i].value_or(""));
    }

private:
    std::vector<std::string> names_;
    std::vector<std::optional<std::string>> saved_;
};
}  // namespace

TEST_CASE("term currentColorEnv reads NO_COLOR and the Windows hints", "[term]") {
    EnvRestore restore({"NO_COLOR", "WT_SESSION", "TERM", "ConEmuANSI"});
    sys::setEnv("NO_COLOR", "");
    sys::setEnv("WT_SESSION", "");
    sys::setEnv("TERM", "");
    sys::setEnv("ConEmuANSI", "");
    auto env = term::currentColorEnv(nullptr);
    CHECK_FALSE(env.noColor);
    CHECK_FALSE(env.windowsAnsiHint);

    sys::setEnv("NO_COLOR", "1");
    CHECK(term::currentColorEnv(nullptr).noColor);
    sys::setEnv("NO_COLOR", "");

    sys::setEnv("TERM", "xterm");
    CHECK(term::currentColorEnv(nullptr).windowsAnsiHint);
    sys::setEnv("TERM", "");
    sys::setEnv("WT_SESSION", "abc");
    CHECK(term::currentColorEnv(nullptr).windowsAnsiHint);
    sys::setEnv("WT_SESSION", "");
    sys::setEnv("ConEmuANSI", "OFF");
    CHECK_FALSE(term::currentColorEnv(nullptr).windowsAnsiHint);
    sys::setEnv("ConEmuANSI", "ON");
    CHECK(term::currentColorEnv(nullptr).windowsAnsiHint);
}

TEST_CASE("term detectStyle is plain when output is not a terminal", "[term]") {
    // ctest runs tests with stdout captured, so this is plain; run by hand in a terminal it may not be.
    if (sys::isTerminal(1)) SKIP("stdout is a terminal");
    CHECK_FALSE(term::detectStyle(nullptr).on());
}

TEST_CASE("term sys: unknown descriptors are not terminals, and the guards construct", "[term]") {
    CHECK_FALSE(sys::isTerminal(57));
    CHECK_FALSE(sys::isTerminal(-1));
    {
        sys::AnsiConsole ansi;
        (void)ansi.enable();  // must neither crash nor leave a console changed
    }
    {
        sys::InterruptGuard guard;
        CHECK_FALSE(sys::InterruptGuard::consume());
        sys::InterruptGuard::trigger();
        CHECK(sys::InterruptGuard::consume());
        CHECK_FALSE(sys::InterruptGuard::consume());
    }
}
