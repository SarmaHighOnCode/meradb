// cpp/include/meradb/cli.h
//
// The `meradb` command line (mirrors meradb/cli.py):
//
//   meradb start | stop | status | server | run FILE... | shell | workbench
//
// `meradb x.mdb` means `run`, `meradb --tui` means `workbench`, and no command
// means `shell` (the interactive shell, see repl.h). The workbench arrives in a later phase; here it says so.
#pragma once
#include "meradb/backend.h"
#include <istream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace meradb {

struct CliArgs {
    std::string command;  // server start stop status shell workbench run
    std::string dataDir;
    // Server commands: where to listen. Client commands: where to connect; unset = not given.
    std::optional<std::string> host;
    std::optional<int> port;
    std::optional<std::string> password;  // server / start: --password VALUE
    bool askPassword = false;             // client commands and stop / status: -W
    bool verbose = false;
    bool force = false;
    bool local = false;
    std::optional<std::string> database;
    std::optional<std::string> user;
    std::vector<std::string> files;
    bool showHelp = false;
    bool showVersion = false;
    std::string error;  // non-empty: the arguments were bad (exit code 2, like argparse)
    // argparse reports errors found while parsing a subcommand's options with that subcommand's
    // usage ("meradb run: error: ..."); unrecognized arguments are reported by the top-level parser.
    bool errorInSubcommand = false;
    std::string errorCommand;  // the subcommand when errorInSubcommand
};

CliArgs parseCliArgs(std::vector<std::string> argv);

// Opens the backend a client command talks to. Prints side notes on stderr,
// exactly as Python does. Throws MeraDBError.
std::unique_ptr<Backend> openBackend(const CliArgs& args);

// A test seam: while one of these is alive, `meradb shell` reads its lines from `in` instead of the
// console or stdin, and prints without colour, so a test can drive the shell even from a terminal.
class ShellInputOverride {
public:
    explicit ShellInputOverride(std::istream& in);
    ~ShellInputOverride();
    ShellInputOverride(const ShellInputOverride&) = delete;
    ShellInputOverride& operator=(const ShellInputOverride&) = delete;
};

int cliMain(std::vector<std::string> argv);

}  // namespace meradb
