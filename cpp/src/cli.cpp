// cpp/src/cli.cpp -- see cli.h.
#include "meradb/cli.h"
#include "meradb/ast.h"
#include "meradb/client.h"
#include "meradb/errors.h"
#include "meradb/fs_util.h"
#include "meradb/protocol.h"
#include "meradb/repl.h"
#include "meradb/server.h"
#include "meradb/server_control.h"
#include "meradb/sys_compat.h"
#include "meradb/term_style.h"
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

// Python's int(): surrounding ASCII whitespace, an optional sign, ASCII digits with single
// underscores between them ("1_0"). (Unicode digits and values beyond int are not accepted.)
std::optional<int> parseInt(const std::string& raw) {
    const char* space = " \t\n\r\f\v";
    std::size_t first = raw.find_first_not_of(space);
    if (first == std::string::npos) return std::nullopt;
    std::string text = raw.substr(first, raw.find_last_not_of(space) - first + 1);
    std::size_t at = (text[0] == '+' || text[0] == '-') ? 1 : 0;
    std::string digits;
    bool lastWasDigit = false;
    for (std::size_t i = at; i < text.size(); ++i) {
        char c = text[i];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            digits += c;
            lastWasDigit = true;
        } else if (c == '_' && lastWasDigit && i + 1 < text.size()) {
            lastWasDigit = false;
        } else {
            return std::nullopt;
        }
    }
    if (!lastWasDigit) return std::nullopt;
    try {
        return std::stoi((text[0] == '-' ? "-" : "") + digits);
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

// argparse lays option help out in a column starting at 24, wrapped to the terminal width (80) minus
// 2, i.e. 54 characters of help text per line. A port of textwrap's line filling: lengths count
// characters (UTF-8 code points), a word longer than a whole line is chopped, and the first piece
// fills what is left of the current line. (Python also breaks after a hyphen inside a word; not
// reproduced.)
std::size_t charCount(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

// Byte offset of the `chars`-th character of `s` (s.size() when it has fewer).
std::size_t charOffset(const std::string& s, std::size_t chars) {
    std::size_t seen = 0, i = 0;
    for (; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (seen == chars) return i;
            ++seen;
        }
    }
    return s.size();
}

std::string wrapHelp(const std::string& text, std::size_t width) {
    std::vector<std::string> chunks;  // words and single spaces, in order
    {
        std::istringstream words(text);
        std::string word;
        while (words >> word) {
            if (!chunks.empty()) chunks.push_back(" ");
            chunks.push_back(word);
        }
    }
    std::vector<std::string> lines;
    std::size_t next = 0;
    while (next < chunks.size()) {
        std::vector<std::string> current;
        std::size_t currentLen = 0;
        if (!lines.empty() && chunks[next] == " ") ++next;  // no leading space on a continuation line
        while (next < chunks.size() && currentLen + charCount(chunks[next]) <= width) {
            currentLen += charCount(chunks[next]);
            current.push_back(chunks[next++]);
        }
        if (next < chunks.size() && charCount(chunks[next]) > width) {
            std::size_t room = width > currentLen ? width - currentLen : 0;
            std::size_t cut = charOffset(chunks[next], room);
            current.push_back(chunks[next].substr(0, cut));
            chunks[next].erase(0, cut);
        }
        if (!current.empty() && current.back().find_first_not_of(' ') == std::string::npos) current.pop_back();
        std::string line;
        for (const auto& c : current) line += c;
        if (!current.empty()) lines.push_back(line);
    }
    if (lines.empty()) lines.emplace_back();
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += (i == 0 ? "  -D DATA, --data DATA  " : std::string(24, ' ')) + lines[i] + "\n";
    }
    return out;
}

// What `python -m meradb COMMAND --help` prints at 80 columns (COLUMNS is not consulted): the usage
// lines, then argparse's option list. The data folder's default is the one this process would use.
std::string subcommandHelp(const std::string& command, const std::string& dataDir) {
    const bool server = command == "server" || command == "start";
    const bool client = command == "shell" || command == "workbench" || command == "run";
    std::string out = subcommandUsage(command) + "\n";
    if (command == "run") out += "positional arguments:\n  files                 script files\n\n";
    out += "options:\n";
    out += "  -h, --help            show this help message and exit\n";
    out += wrapHelp("data folder (default: " + dataDir + ", ya MERADB_DATA)", 54);
    if (server) {
        out += "  --host HOST           kis address par suno (default 127.0.0.1; LAN ke liye\n"
               "                        0.0.0.0)\n"
               "  --port PORT           TCP port (default 6372, ya MERADB_PORT)\n"
               "  --password PASSWORD   clients ko ye password dena hoga (ya MERADB_PASSWORD)\n"
               "  -v, --verbose         har query log karo\n";
    } else {
        if (client) {
            out += "  -H HOST, --host HOST  server ka address (default 127.0.0.1, ya MERADB_HOST)\n"
                   "  -p PORT, --port PORT  server ka port (default 6372, ya MERADB_PORT)\n"
                   "  -d DATABASE, --database DATABASE\n"
                   "                        shuru mein ye database istemal karo\n";
        }
        out += "  -W, --password        password poocho (ya MERADB_PASSWORD)\n";
        if (command == "stop") out += "  --force               agar normal shutdown na ho to process kill karo\n";
        if (client) {
            out += "  -U USER, --user USER  is USERNAME se login karo (ya MERADB_USER) --\n"
                   "                        privileges ke saath, server-wide password ki jagah\n"
                   "  --local               server ke bina, seedha data folder kholo\n";
        }
    }
    return out;
}

// "-p/--port" style name argparse puts into "argument ...: ..." messages.
std::string optionDisplayName(const std::string& name, bool isServer) {
    if (name == "-D" || name == "--data") return "-D/--data";
    if (name == "-H" || name == "--host") return isServer ? "--host" : "-H/--host";
    if (name == "-p" || name == "--port") return isServer ? "--port" : "-p/--port";
    if (name == "-d" || name == "--database") return "-d/--database";
    if (name == "-U" || name == "--user") return "-U/--user";
    if (name == "-h" || name == "--help") return "-h/--help";
    if (name == "-W" || name == "--password") return isServer ? "--password" : "-W/--password";
    if (name == "-v" || name == "--verbose") return "-v/--verbose";
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

    // The single-letter options of this command: 0 = none, 1 = flag, 2 = takes a value.
    auto shortKind = [&](const std::string& opt) -> int {
        if (opt == "-h") return 1;
        if (opt == "-D") return 2;
        if (isClient && (opt == "-H" || opt == "-p" || opt == "-d" || opt == "-U")) return 2;
        if ((isClient || isControl) && opt == "-W") return 1;
        if (isServer && opt == "-v") return 1;
        return 0;
    };

    std::vector<std::string> unrecognized;
    // argparse takes the files as ONE run of words: an option after the first file ends it, and a
    // later bare word is "unrecognized". The first "--" is dropped and makes every word after it a
    // positional, even one that looks like an option; any other command has no positionals.
    bool optionsEnded = false;
    bool filesClosed = false;
    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        if (optionsEnded || (arg == "--" && args.command == "run" && !filesClosed)) {
            if (!optionsEnded) {
                optionsEnded = true;  // the "--" itself
            } else if (args.command == "run" && !filesClosed) {
                args.files.push_back(arg);
            } else {
                unrecognized.push_back(arg);
            }
            continue;
        }
        if (arg == "--") {
            optionsEnded = true;
            unrecognized.push_back(arg);
            continue;
        }
        if (!args.files.empty() && looksLikeOption(arg)) filesClosed = true;
        // One word can carry several options, like argparse: "--name=value", "-DDIR", "-D=DIR",
        // "-p7" and bundles such as "-WD DIR" or "-WDDIR" (a flag, then the next option letter).
        struct Piece {
            std::string name;
            bool hasInline;
            std::string inlineValue;
        };
        std::vector<Piece> pieces;
        if (arg.rfind("--", 0) == 0 && arg.find('=') != std::string::npos) {
            pieces.push_back({arg.substr(0, arg.find('=')), true, arg.substr(arg.find('=') + 1)});
        } else if (arg.size() > 2 && arg[0] == '-' && arg[1] != '-' && shortKind("-" + arg.substr(1, 1)) != 0) {
            std::string rest = arg.substr(1);  // option letters still to read
            while (!rest.empty()) {
                std::string opt = "-" + rest.substr(0, 1);
                std::string tail = rest.substr(1);
                int kind = shortKind(opt);
                if (kind == 0) {  // not an option of this command: argparse leaves the remainder over
                    unrecognized.push_back("-" + rest);
                    break;
                }
                if (kind == 2) {  // takes a value: the rest of the word, minus one leading "="
                    if (tail.empty()) {
                        pieces.push_back({opt, false, ""});
                    } else {
                        pieces.push_back({opt, true, tail[0] == '=' ? tail.substr(1) : tail});
                    }
                    break;
                }
                if (!tail.empty() && tail[0] == '=') {  // a flag cannot take "=value"
                    pieces.push_back({opt, true, tail.substr(1)});
                    break;
                }
                pieces.push_back({opt, false, ""});
                rest = tail;
            }
        } else {
            pieces.push_back({arg, false, ""});
        }
        for (const Piece& piece : pieces) {
            const std::string& name = piece.name;
            const bool hasInline = piece.hasInline;
            const std::string& inlineValue = piece.inlineValue;
            const bool isFlag = name == "-h" || name == "--help" || ((isClient || isControl) && (name == "-W" || name == "--password")) ||
                                (isServer && (name == "-v" || name == "--verbose")) || (args.command == "stop" && name == "--force") ||
                                (isClient && name == "--local");
            if (isFlag && hasInline) {
                args.error = "argument " + optionDisplayName(name, isServer) + ": ignored explicit argument '" + inlineValue + "'";
                args.errorInSubcommand = true;
                args.errorCommand = args.command;
                return args;
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
            } else if (args.command == "run" && !filesClosed && (arg.empty() || arg[0] != '-' || arg == "-")) {
                args.files.push_back(arg);
            } else {
                unrecognized.push_back(arg);  // argparse collects them all and reports once, after the required check
            }
        }
    }
    if (args.command == "run" && args.files.empty()) {
        args.error = "the following arguments are required: files";
        args.errorInSubcommand = true;
        args.errorCommand = args.command;
    } else if (!unrecognized.empty()) {
        std::string joined;  // ' '.join(extras): an empty word still gets its separator
        for (std::size_t k = 0; k < unrecognized.size(); ++k) joined += (k ? " " : "") + unrecognized[k];
        args.error = "unrecognized arguments: " + joined;
    }
    return args;
}

namespace {

std::optional<std::string> clientPassword(const CliArgs& args) {
    if (args.askPassword) return sys::readHidden("Password: ");
    auto env = sys::getEnv("MERADB_PASSWORD");
    if (env) return env;  // Python: os.environ.get -- an empty value stays "" (which means no password)
    return std::nullopt;
}

std::string absolute(const std::string& path) { return absolutePathOf(path); }

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
        if (args.user && !args.user->empty())  // Python tests truthiness: -U "" says nothing
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

// Python's _supports_color(): decided once, for the command that is about to print results.
term::Style detectStyle(sys::AnsiConsole& ansi) {
    return term::detectStyle([&ansi] { return ansi.enable(); });
}

// Test seam (see ShellInputOverride in cli.h): when set, the shell reads this stream, never the console.
std::istream* g_shellInput = nullptr;

// Python's `finally: backend.close()`: the backend is closed (an open transaction rolled back) on every way
// out, including an exception that is not a MeraDBError. On a normal return the caller closes explicitly.
class BackendCloser {
public:
    explicit BackendCloser(Backend& backend) : backend_(backend) {}
    ~BackendCloser() {
        if (!done_) {
            try {
                backend_.close();
            } catch (...) {  // already unwinding: the original exception is the one that matters
            }
        }
    }
    void close() {
        done_ = true;
        backend_.close();
    }
    BackendCloser(const BackendCloser&) = delete;
    BackendCloser& operator=(const BackendCloser&) = delete;

private:
    Backend& backend_;
    bool done_ = false;
};

// Python's cmd_shell: open the backend, run the shell on it, close the backend.
int runShellCommand(const CliArgs& args) {
    auto backend = openBackend(args);
    BackendCloser closer(*backend);
    int code = 0;
    if (g_shellInput != nullptr) {
        repl::StreamLineSource source(*g_shellInput, std::cout);
        code = repl::run(*backend, source, std::cout, term::Style::none(), protocol::kProgramVersion, 0);
    } else if (sys::isTerminal(0)) {
        sys::AnsiConsole ansi;
        const term::Style style = detectStyle(ansi);
        sys::InterruptGuard interrupts;  // Ctrl+C is reported to the shell instead of killing the process
        repl::ConsoleLineSource source(std::cout);
        code = repl::run(*backend, source, std::cout, style, protocol::kProgramVersion, style.on() ? 40 : 0);
    } else {
        sys::setStdinBinary();  // bytes exactly as sent: no CRLF translation, Ctrl+Z is not end-of-file
        sys::AnsiConsole ansi;
        const term::Style style = detectStyle(ansi);
        repl::StreamLineSource source(std::cin, std::cout);
        code = repl::run(*backend, source, std::cout, style, protocol::kProgramVersion, style.on() ? 40 : 0);
    }
    closer.close();
    return code;
}

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

ShellInputOverride::ShellInputOverride(std::istream& in) { g_shellInput = &in; }
ShellInputOverride::~ShellInputOverride() { g_shellInput = nullptr; }

int cliMain(std::vector<std::string> argv) {
    CliArgs args = parseCliArgs(std::move(argv));
    if (args.showHelp) {
        if (args.command.empty()) std::cout << kUsage;
        else std::cout << subcommandHelp(args.command, protocol::defaultDataDir());
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
            BackendCloser closer(*backend);
            sys::AnsiConsole ansi;
            const term::Style style = detectStyle(ansi);
            bool allOk = true;
            for (const auto& path : args.files)
                if (!repl::runFile(*backend, path, std::cout, style)) allOk = false;  // run ALL files, even after a failure
            closer.close();
            return allOk ? 0 : 1;
        }
        if (args.command == "shell") return runShellCommand(args);
        // workbench
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
