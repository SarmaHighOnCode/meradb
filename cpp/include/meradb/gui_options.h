// cpp/include/meradb/gui_options.h -- the command line of meradb_gui.
//
//   meradb_gui [-h] [-D DATA] [-H HOST] [-p PORT] [-d DATABASE] [-W] [-U USER] [--local] [--version]
//
// The same options with the same meaning as `meradb workbench` (they are parsed by parseCliArgs). Eight more are
// accepted but not listed in --help; they exist for tests and screenshots:
//   --screenshot FILE.png   draw with a hidden window, write the picture, exit 0
//   --script TEXT           with --screenshot: run these statements first and wait until they are done
//   --theme light|dark      the colours (default: the system setting; dark for a screenshot)
//   --size WxH              window size in pixels, default 1280x800 (between 320x240 and 8192x8192)
//   --editor TEXT           with --screenshot: put TEXT into the query editor (after the script ran)
//   --expand-tables         with --screenshot: open every table in the schema tree
//   --open connect|madad|palette   with --screenshot: open that dialog, window or the command menu
//   --type TEXT             with --screenshot and --open palette: the text typed into its filter box
#pragma once
#include "meradb/cli.h"
#include "meradb/gui_theme.h"
#include <optional>
#include <string>
#include <vector>

namespace meradb::gui {

struct GuiOptions {
    CliArgs cli;                      // command is "workbench"; host, port, data folder, -d, -U, -W, --local
    bool showHelp = false;
    bool showVersion = false;
    std::string error;                // non-empty: bad arguments (exit code 2)
    std::optional<std::string> screenshot;
    std::optional<std::string> script;
    std::optional<ThemeMode> theme;
    std::optional<std::string> editorText;
    std::optional<std::string> open;       // "connect", "madad" or "palette"
    std::optional<std::string> typeText;
    bool expandTables = false;
    int width = 1280;
    int height = 800;
};

constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 800;

// `argv` WITHOUT the program name.
GuiOptions parseGuiArgs(std::vector<std::string> argv);
std::string guiUsage();                          // the "usage: meradb_gui ..." lines
std::string guiHelp(const std::string& dataDir); // usage + option list (the hidden options are not in it)
// "1280x800" -> (1280, 800); nullopt for anything else or a size outside 320x240 .. 8192x8192.
std::optional<std::pair<int, int>> parseSize(const std::string& text);

}  // namespace meradb::gui
