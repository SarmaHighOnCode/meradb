// cpp/tests/test_gui_options.cpp -- the command line of meradb_gui, and `meradb_cli gui`.
#include <catch2/catch_test_macros.hpp>
#include "meradb/cli.h"
#include "meradb/gui_options.h"
#include "meradb/sys_compat.h"
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace meradb;
using namespace meradb::gui;

namespace {

// The environment variables the shared parser reads, cleared for the duration of a test.
class CleanEnv {
public:
    CleanEnv() {
        for (const char* name : names_) {
            saved_.push_back(sys::getEnv(name));
            sys::setEnv(name, "");
        }
    }
    ~CleanEnv() {
        for (std::size_t i = 0; i < saved_.size(); ++i) sys::setEnv(names_[i], saved_[i].value_or(""));
    }

private:
    const char* names_[5] = {"MERADB_DATA", "MERADB_HOST", "MERADB_PORT", "MERADB_PASSWORD", "MERADB_USER"};
    std::vector<std::optional<std::string>> saved_;
};

class Capture {
public:
    Capture() : oldOut_(std::cout.rdbuf(out_.rdbuf())), oldErr_(std::cerr.rdbuf(err_.rdbuf())) {}
    ~Capture() {
        std::cout.rdbuf(oldOut_);
        std::cerr.rdbuf(oldErr_);
    }
    std::string out() const { return out_.str(); }
    std::string err() const { return err_.str(); }

private:
    std::ostringstream out_, err_;
    std::streambuf* oldOut_;
    std::streambuf* oldErr_;
};

}  // namespace

TEST_CASE("gui options: no arguments behave like `meradb workbench`", "[gui][cli]") {
    CleanEnv env;
    const GuiOptions options = parseGuiArgs({});
    CHECK(options.error.empty());
    CHECK_FALSE(options.showHelp);
    CHECK_FALSE(options.showVersion);
    CHECK(options.cli.command == "workbench");
    CHECK_FALSE(options.cli.local);
    CHECK_FALSE(options.cli.host.has_value());
    CHECK_FALSE(options.cli.port.has_value());
    CHECK_FALSE(options.screenshot.has_value());
    CHECK_FALSE(options.script.has_value());
    CHECK_FALSE(options.theme.has_value());
    CHECK(options.width == 1280);
    CHECK(options.height == 800);
    const CliArgs reference = parseCliArgs({"workbench"});
    // the window opens the checkout's data folder when it runs from one, else the same default as the CLI
    CHECK(options.cli.dataDir == (projectDataDir().empty() ? reference.dataDir : projectDataDir()));
}

TEST_CASE("gui options: the workbench options keep their meaning", "[gui][cli]") {
    CleanEnv env;
    const GuiOptions options = parseGuiArgs({"-D", "dd", "-H", "10.0.0.5", "-p", "7000", "-d", "college", "-W", "-U", "asha", "--local"});
    CHECK(options.error.empty());
    CHECK(options.cli.dataDir == "dd");
    CHECK(options.cli.host == std::optional<std::string>("10.0.0.5"));
    CHECK(options.cli.port == std::optional<int>(7000));
    CHECK(options.cli.database == std::optional<std::string>("college"));
    CHECK(options.cli.user == std::optional<std::string>("asha"));
    CHECK(options.cli.askPassword);
    CHECK(options.cli.local);
    const GuiOptions longForm = parseGuiArgs({"--data=xx", "--host", "h", "--port=9", "--database", "d", "--user=u"});
    CHECK(longForm.error.empty());
    CHECK(longForm.cli.dataDir == "xx");
    CHECK(longForm.cli.port == std::optional<int>(9));
    CHECK(longForm.cli.user == std::optional<std::string>("u"));
}

TEST_CASE("gui options: the environment gives the same defaults as the workbench", "[gui][cli]") {
    CleanEnv env;
    sys::setEnv("MERADB_USER", "envuser");
    sys::setEnv("MERADB_DATA", "envdata");
    const GuiOptions options = parseGuiArgs({});
    CHECK(options.cli.user == std::optional<std::string>("envuser"));
    CHECK(options.cli.dataDir == "envdata");
}

TEST_CASE("gui options: the hidden test options", "[gui][cli]") {
    CleanEnv env;
    const GuiOptions options = parseGuiArgs({"--local", "--screenshot", "out.png", "--script", "SHURU;", "--theme", "light", "--size", "800x600"});
    CHECK(options.error.empty());
    CHECK(options.cli.local);
    CHECK(options.screenshot == std::optional<std::string>("out.png"));
    CHECK(options.script == std::optional<std::string>("SHURU;"));
    CHECK(options.theme == std::optional<ThemeMode>(ThemeMode::Light));
    CHECK(options.width == 800);
    CHECK(options.height == 600);
    const GuiOptions inlineForm = parseGuiArgs({"--screenshot=a b.png", "--theme=DARK", "--size=1000X700", "--script=DIKHAO 1;"});
    CHECK(inlineForm.error.empty());
    CHECK(inlineForm.screenshot == std::optional<std::string>("a b.png"));
    CHECK(inlineForm.theme == std::optional<ThemeMode>(ThemeMode::Dark));
    CHECK(inlineForm.width == 1000);
    CHECK(inlineForm.height == 700);
    CHECK(inlineForm.script == std::optional<std::string>("DIKHAO 1;"));
}

TEST_CASE("gui options: size parsing", "[gui][cli]") {
    CHECK(parseSize("1280x800") == std::optional<std::pair<int, int>>(std::make_pair(1280, 800)));
    CHECK(parseSize("320x240").has_value());
    CHECK(parseSize("8192x8192").has_value());
    CHECK_FALSE(parseSize("319x800").has_value());
    CHECK_FALSE(parseSize("800x239").has_value());
    CHECK_FALSE(parseSize("8193x800").has_value());
    CHECK_FALSE(parseSize("").has_value());
    CHECK_FALSE(parseSize("x").has_value());
    CHECK_FALSE(parseSize("800x").has_value());
    CHECK_FALSE(parseSize("x600").has_value());
    CHECK_FALSE(parseSize("800").has_value());
    CHECK_FALSE(parseSize("80a0x600").has_value());
    CHECK_FALSE(parseSize("-800x600").has_value());
    CHECK_FALSE(parseSize("999999999999x600").has_value());
}

TEST_CASE("gui options: bad arguments are reported", "[gui][cli]") {
    CleanEnv env;
    CHECK(parseGuiArgs({"--bogus"}).error == "unrecognized arguments: --bogus");
    CHECK(parseGuiArgs({"file.mdb"}).error == "unrecognized arguments: file.mdb");
    CHECK(parseGuiArgs({"-p", "abc"}).error.find("invalid int value") != std::string::npos);
    CHECK(parseGuiArgs({"-D"}).error.find("expected one argument") != std::string::npos);
    CHECK(parseGuiArgs({"--theme", "purple"}).error.find("--theme") != std::string::npos);
    CHECK(parseGuiArgs({"--theme"}).error.find("expected one argument") != std::string::npos);
    CHECK(parseGuiArgs({"--size", "10x10"}).error.find("--size") != std::string::npos);
    CHECK(parseGuiArgs({"--screenshot"}).error.find("expected one argument") != std::string::npos);
}

TEST_CASE("gui options: --help and --version", "[gui][cli]") {
    CleanEnv env;
    CHECK(parseGuiArgs({"-h"}).showHelp);
    CHECK(parseGuiArgs({"--help"}).showHelp);
    CHECK(parseGuiArgs({"--version"}).showVersion);
    CHECK(parseGuiArgs({"--local", "--version"}).showVersion);
    const std::string help = guiHelp("C:\\data");
    CHECK(help.rfind("usage: meradb_gui [-h] [-D DATA]", 0) == 0);
    CHECK(help.find("meradb workbench") != std::string::npos);
    CHECK(help.find("C:\\data") != std::string::npos);
    CHECK(help.find("--local") != std::string::npos);
    CHECK(help.find("--version") != std::string::npos);
    // the test options are not advertised
    CHECK(help.find("--screenshot") == std::string::npos);
    CHECK(help.find("--script") == std::string::npos);
    CHECK(help.find("--theme") == std::string::npos);
    CHECK(help.find("--size") == std::string::npos);
    CHECK(guiUsage().find("meradb_gui") != std::string::npos);
}

TEST_CASE("cli gui: arguments parse like the workbench and help is its own", "[gui][cli]") {
    CleanEnv env;
    const CliArgs args = parseCliArgs({"gui", "--local", "-D", "dd", "-d", "college"});
    CHECK(args.error.empty());
    CHECK(args.command == "gui");
    CHECK(args.local);
    CHECK(args.dataDir == "dd");
    CHECK(args.database == std::optional<std::string>("college"));
    CHECK(parseCliArgs({"gui", "--nope"}).error == "unrecognized arguments: --nope");
    {
        Capture capture;
        CHECK(cliMain({"gui", "-h"}) == 0);
        CHECK(capture.out().rfind("usage: meradb gui [-h]", 0) == 0);
        CHECK(capture.err().empty());
    }
    {
        Capture capture;
        CHECK(cliMain({"--help"}) == 0);
        CHECK(capture.out().find("gui ") != std::string::npos);
    }
    {
        Capture capture;
        CHECK(cliMain({"gui", "-p", "x"}) == 2);
        CHECK(capture.err().find("meradb gui: error: argument -p/--port: invalid int value") != std::string::npos);
    }
}

TEST_CASE("cli gui says clearly when meradb_gui is not next to the program", "[gui][cli]") {
    CleanEnv env;
    Capture capture;   // the test program is in tests/, meradb_gui is one folder up
    CHECK(cliMain({"gui", "--local"}) == 1);
    CHECK(capture.out().empty());
    CHECK(capture.err().find("meradb_gui nahi mila") != std::string::npos);
    CHECK(capture.err().find("meradb workbench") != std::string::npos);
}
