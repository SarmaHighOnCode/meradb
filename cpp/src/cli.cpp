// cpp/src/cli.cpp -- see cli.h.
#include "meradb/cli.h"
#include "meradb/ast.h"
#include "meradb/cli_format.h"
#include "meradb/client.h"
#include "meradb/errors.h"
#include "meradb/protocol.h"
#include "meradb/server.h"
#include "meradb/server_control.h"
#include "meradb/sys_compat.h"
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <vector>

namespace meradb {
namespace {

const std::set<std::string> kCommands = {"server", "start", "stop", "status", "shell", "workbench", "tui", "run"};

void note(const std::string& message) { std::cerr << message << "\n"; }

std::optional<int> parseInt(const std::string& text) {
    if (text.empty()) return std::nullopt;
    std::size_t used = 0;
    try {
        int value = std::stoi(text, &used);
        if (used != text.size()) return std::nullopt;
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

int envPort() {
    auto text = sys::getEnv("MERADB_PORT");
    if (text && !text->empty())
        if (auto value = parseInt(*text)) return *value;
    return protocol::kDefaultPort;
}

// Python's text mode (universal newlines): "\r\n" and a lone "\r" both become "\n".
std::string normalizeNewlines(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\r') {
            out += '\n';
            if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
        } else {
            out += in[i];
        }
    }
    return out;
}

const char* kUsage =
    "usage: meradb [-h] [--version] COMMAND ...\n"
    "\n"
    "MeraDB -- apna database, apni bhasha.\n"
    "\n"
    "positional arguments:\n"
    "  COMMAND\n"
    "    server           server isi terminal mein chalao (Ctrl+C se band)\n"
    "    start            server background mein chalao\n"
    "    stop             background server band karo\n"
    "    status           server chal raha hai ya nahi\n"
    "    shell            interactive command-line shell\n"
    "    workbench (tui)  full-screen UI (MySQL Workbench jaisa)\n"
    "    run              .mdb script files chalao\n"
    "\n"
    "options:\n"
    "  -h, --help         show this help message and exit\n"
    "  --version          show program's version number and exit\n"
    "\n"
    "examples:\n"
    "  meradb start                      server ko background mein chalao\n"
    "  meradb shell                      server se connect karo (command-line shell)\n"
    "  meradb workbench                  full-screen UI (MySQL Workbench jaisa)\n"
    "  meradb run examples/demo.mdb      script chalao\n"
    "  meradb status / meradb stop\n"
    "  meradb shell --local              bina server ke, seedha data folder par\n";

// argparse's usage line for each subcommand (80-column wrapping), copied from running Python's parser.
std::string subcommandUsage(const std::string& command) {
    if (command == "server")
        return "usage: meradb server [-h] [-D DATA] [--host HOST] [--port PORT]\n"
               "                     [--password PASSWORD] [-v]\n";
    if (command == "start")
        return "usage: meradb start [-h] [-D DATA] [--host HOST] [--port PORT]\n"
               "                    [--password PASSWORD] [-v]\n";
    if (command == "stop") return "usage: meradb stop [-h] [-D DATA] [-W] [--force]\n";
    if (command == "status") return "usage: meradb status [-h] [-D DATA] [-W]\n";
    if (command == "shell")
        return "usage: meradb shell [-h] [-D DATA] [-H HOST] [-p PORT] [-d DATABASE] [-W]\n"
               "                    [-U USER] [--local]\n";
    if (command == "workbench")
        return "usage: meradb workbench [-h] [-D DATA] [-H HOST] [-p PORT] [-d DATABASE] [-W]\n"
               "                        [-U USER] [--local]\n";
    return "usage: meradb run [-h] [-D DATA] [-H HOST] [-p PORT] [-d DATABASE] [-W]\n"
           "                  [-U USER] [--local]\n"
           "                  files [files ...]\n";
}

const char* kTopUsage = "usage: meradb [-h] [--version] COMMAND ...\n";

// "-p/--port" style name argparse puts into "argument ...: ..." messages.
std::string optionDisplayName(const std::string& name, bool isServer) {
    if (name == "-D" || name == "--data") return "-D/--data";
    if (name == "-H" || name == "--host") return isServer ? "--host" : "-H/--host";
    if (name == "-p" || name == "--port") return isServer ? "--port" : "-p/--port";
    if (name == "-d" || name == "--database") return "-d/--database";
    if (name == "-U" || name == "--user") return "-U/--user";
    return name;
}

// argparse: a following word that looks like an option is not a value.
bool looksLikeOption(const std::string& word) {
    if (word.size() < 2 || word[0] != '-') return false;
    if (std::isdigit(static_cast<unsigned char>(word[1])) || word[1] == '.') return false;  // a negative number
    return true;
}

}  // namespace

CliArgs parseCliArgs(std::vector<std::string> argv) {
    CliArgs args;
    // shortcuts: `meradb demo.mdb` = run, `meradb --tui` = workbench, `meradb` = shell
    auto endsWithMdb = [](const std::string& s) { return s.size() >= 4 && s.compare(s.size() - 4, 4, ".mdb") == 0; };
    if (!argv.empty() && endsWithMdb(argv[0])) argv.insert(argv.begin(), "run");
    else if (!argv.empty() && argv[0] == "--tui") argv[0] = "workbench";
    if (argv.empty() || (!kCommands.count(argv[0]) && argv[0] != "-h" && argv[0] != "--help" && argv[0] != "--version"))
        argv.insert(argv.begin(), "shell");

    if (argv[0] == "-h" || argv[0] == "--help") {
        args.showHelp = true;
        return args;
    }
    if (argv[0] == "--version") {
        args.showVersion = true;
        return args;
    }
    args.command = argv[0] == "tui" ? "workbench" : argv[0];
    args.dataDir = protocol::defaultDataDir();

    const bool isServer = args.command == "server" || args.command == "start";
    const bool isClient = args.command == "shell" || args.command == "workbench" || args.command == "run";
    const bool isControl = args.command == "stop" || args.command == "status";
    if (isServer) {
        args.host = protocol::kDefaultHost;
        args.port = envPort();
        if (auto pw = sys::getEnv("MERADB_PASSWORD"); pw && !pw->empty()) args.password = *pw;
    }
    if (isClient) {
        if (auto user = sys::getEnv("MERADB_USER"); user && !user->empty()) args.user = *user;
    }

    std::vector<std::string> unrecognized;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        // "--name=value" is accepted like argparse does
        std::string name = arg, inlineValue;
        bool hasInline = false;
        if (arg.rfind("--", 0) == 0 && arg.find('=') != std::string::npos) {
            name = arg.substr(0, arg.find('='));
            inlineValue = arg.substr(arg.find('=') + 1);
            hasInline = true;
        }
        auto value = [&](std::string& out) -> bool {
            if (hasInline) {
                out = inlineValue;
                return true;
            }
            if (i + 1 >= argv.size() || looksLikeOption(argv[i + 1])) {
                args.error = "argument " + optionDisplayName(name, isServer) + ": expected one argument";
                args.errorInSubcommand = true;
                args.errorCommand = args.command;
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string text;
        if (name == "-h" || name == "--help") {
            args.showHelp = true;
            return args;
        } else if (name == "-D" || name == "--data") {
            if (!value(text)) return args;
            args.dataDir = text;
        } else if (isServer && name == "--host") {
            if (!value(text)) return args;
            args.host = text;
        } else if (isClient && (name == "-H" || name == "--host")) {
            if (!value(text)) return args;
            args.host = text;
        } else if ((isServer && name == "--port") || (isClient && (name == "-p" || name == "--port"))) {
            if (!value(text)) return args;
            auto number = parseInt(text);
            if (!number) {
                args.error = "argument " + optionDisplayName(name, isServer) + ": invalid int value: '" + text + "'";
                args.errorInSubcommand = true;
                args.errorCommand = args.command;
                return args;
            }
            args.port = *number;
        } else if (isServer && name == "--password") {
            if (!value(text)) return args;
            args.password = text;
        } else if ((isClient || isControl) && (name == "-W" || name == "--password")) {
            args.askPassword = true;
        } else if (isServer && (name == "-v" || name == "--verbose")) {
            args.verbose = true;
        } else if (args.command == "stop" && name == "--force") {
            args.force = true;
        } else if (isClient && (name == "-d" || name == "--database")) {
            if (!value(text)) return args;
            args.database = text;
        } else if (isClient && (name == "-U" || name == "--user")) {
            if (!value(text)) return args;
            args.user = text;
        } else if (isClient && name == "--local") {
            args.local = true;
        } else if (args.command == "run" && (arg.empty() || arg[0] != '-' || arg == "-")) {
            args.files.push_back(arg);
        } else {
            unrecognized.push_back(arg);  // argparse collects them all and reports once, after the required check
        }
    }
    if (args.command == "run" && args.files.empty()) {
        args.error = "the following arguments are required: files";
        args.errorInSubcommand = true;
        args.errorCommand = args.command;
    } else if (!unrecognized.empty()) {
        for (const auto& word : unrecognized) args.error += (args.error.empty() ? "" : " ") + word;
        args.error = "unrecognized arguments: " + args.error;
    }
    return args;
}

bool runFile(Backend& backend, const std::string& path) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
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
    for (const auto& result : backend.runScript(text)) {
        std::string out = formatResult(result);
        if (!out.empty()) std::cout << out << "\n";
        std::cout << "\n";
        if (!result.error.empty()) ok = false;
    }
    return ok;
}

namespace {

std::optional<std::string> clientPassword(const CliArgs& args) {
    if (args.askPassword) return sys::readHidden("Password: ");
    auto env = sys::getEnv("MERADB_PASSWORD");
    if (env) return env;  // Python: os.environ.get -- an empty value stays "" (which means no password)
    return std::nullopt;
}

std::string absolute(const std::string& path) {
    std::error_code ec;
    auto result = std::filesystem::absolute(std::filesystem::u8path(path), ec);
    return ec ? path : result.lexically_normal().u8string();
}

std::unique_ptr<Backend> openLocal(const CliArgs& args) {
    auto backend = std::make_unique<LocalBackend>(args.dataDir);
    for (const auto& db : backend->engine().instance().recovered())
        note("RECOVERY: database '" + db + "' ka adhoora transaction WAPAS kiya (pichli baar crash hua tha)");
    if (args.database) {
        ast::UseDatabase use;
        use.name = *args.database;
        backend->engine().executeStatement(use);
    }
    return backend;
}

}  // namespace

std::unique_ptr<Backend> openBackend(const CliArgs& args) {
    std::optional<std::string> password = clientPassword(args);
    if (args.local) {
        if (args.user)
            note("(--local mode mein -U/--user '" + *args.user +
                 "' ka koi matlab nahi -- ignore kiya, superuser ki tarah chal raha hai)");
        return openLocal(args);
    }

    // Flags are an explicit choice (no fallback to local mode if that server is down);
    // MERADB_HOST / MERADB_PORT only change the defaults.
    const bool explicitTarget = args.host.has_value() || args.port.has_value();
    std::string host = args.host ? *args.host : std::string();
    if (host.empty()) host = sys::getEnv("MERADB_HOST").value_or("");
    if (host.empty()) host = protocol::kDefaultHost;
    int port = 0;
    if (args.port) port = *args.port;
    else if (auto text = sys::getEnv("MERADB_PORT"); text && !text->empty()) {
        auto number = parseInt(*text);
        if (!number) throw MeraDBError("MERADB_PORT ek number hona chahiye: '" + *text + "'");
        port = *number;
    } else {
        auto info = protocol::readPidFile(absolute(args.dataDir));  // a server on another port for this folder?
        port = (info && info->contains("port")) ? static_cast<int>((*info)["port"].get<std::int64_t>())
                                                : protocol::kDefaultPort;
    }

    ConnectOptions options;
    options.host = host;
    options.port = port;
    if (password && !password->empty()) options.password = password;
    options.database = args.database;
    options.user = args.user;
    try {
        return std::make_unique<Connection>(options);
    } catch (const ServerUnavailable&) {
        if (explicitTarget) throw;
        note("(" + host + ":" + std::to_string(port) + " par server nahi mila -- LOCAL mode: seedha '" +
             absolute(args.dataDir) + "' khol rahe hain. Server ke liye: meradb start)");
        return openLocal(args);
    }
}

namespace {

ControlOptions controlOptions(const CliArgs& args) {
    ControlOptions options;
    options.dataDir = args.dataDir;
    if (args.host) options.host = *args.host;
    if (args.port) options.port = *args.port;
    options.password = args.password;
    options.verbose = args.verbose;
    options.force = args.force;
    if (args.askPassword) options.password = sys::readHidden("Password: ");
    return options;
}

}  // namespace

int cliMain(std::vector<std::string> argv) {
    CliArgs args = parseCliArgs(std::move(argv));
    if (args.showHelp) {
        if (args.command.empty()) std::cout << kUsage;
        else std::cout << subcommandUsage(args.command);  // the full per-command option list is Python's argparse text
        return 0;
    }
    if (args.showVersion) {
        std::cout << protocol::kServerName << "\n";
        return 0;
    }
    if (!args.error.empty()) {
        if (args.errorInSubcommand)
            std::cerr << subcommandUsage(args.errorCommand) << "meradb " << args.errorCommand << ": error: " << args.error << "\n";
        else
            std::cerr << kTopUsage << "meradb: error: " << args.error << "\n";
        return 2;
    }
    try {
        if (args.command == "server") {
            ServerOptions options;
            options.dataDir = args.dataDir;
            options.host = args.host.value_or(protocol::kDefaultHost);
            options.port = args.port.value_or(protocol::kDefaultPort);
            options.password = args.password.value_or("");
            options.verbose = args.verbose;
            return serve(options);
        }
        if (args.command == "start") return serverStart(controlOptions(args));
        if (args.command == "stop") return serverStop(controlOptions(args));
        if (args.command == "status") return serverStatus(controlOptions(args));
        if (args.command == "run") {
            auto backend = openBackend(args);
            bool allOk = true;
            for (const auto& path : args.files)
                if (!runFile(*backend, path)) allOk = false;  // run ALL files, even after a failure
            backend->close();
            return allOk ? 0 : 1;
        }
        // shell / workbench
        note("`meradb " + args.command + "` abhi C++ version mein nahi hai (aage ke phase mein aayega). "
             "Python version istemal karo, ya scripts ke liye:  meradb run FILE");
        return 1;
    } catch (const MeraDBError& e) {
        note(e.what());
        return 1;
    } catch (const std::exception& e) {
        note(e.what());
        return 1;
    }
}

}  // namespace meradb
