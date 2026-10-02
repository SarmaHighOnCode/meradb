// cpp/tests/test_cli.cpp -- argument parsing, backend selection and `run` output.
#include <catch2/catch_test_macros.hpp>
#include "meradb/cli.h"
#include "meradb/errors.h"
#include "meradb/protocol.h"
#include "meradb/sys_compat.h"
#include "golden_help.h"
#include "server_fixture.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <vector>

using namespace meradb;
using namespace meradb_test;

namespace {

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

// Environment variables the parser reads, cleared for the duration of a test.
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
    const char* names_[6] = {"MERADB_DATA", "MERADB_HOST", "MERADB_PORT", "MERADB_PASSWORD", "MERADB_USER", "MERADB_X"};
    std::vector<std::optional<std::string>> saved_;
};

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

}  // namespace

TEST_CASE("cli shortcuts: a .mdb file means run, --tui means workbench, nothing means shell", "[cli]") {
    CleanEnv env;
    CHECK(parseCliArgs({"demo.mdb"}).command == "run");
    CHECK(parseCliArgs({"demo.mdb"}).files == std::vector<std::string>{"demo.mdb"});
    CHECK(parseCliArgs({"--tui"}).command == "workbench");
    CHECK(parseCliArgs({"tui"}).command == "workbench");
    CHECK(parseCliArgs({}).command == "shell");
    CHECK(parseCliArgs({"--local"}).command == "shell");  // an unknown first word becomes `shell ...`
    CHECK(parseCliArgs({"--local"}).local);
    CHECK(parseCliArgs({"--version"}).showVersion);
    CHECK(parseCliArgs({"-h"}).showHelp);
    CHECK(parseCliArgs({"status", "--help"}).showHelp);
}

TEST_CASE("cli server options and their defaults", "[cli]") {
    CleanEnv env;
    CliArgs args = parseCliArgs({"server"});
    CHECK(args.host.value() == "127.0.0.1");
    CHECK(args.port.value() == 6372);
    CHECK_FALSE(args.password.has_value());
    CHECK_FALSE(args.verbose);

    args = parseCliArgs({"start", "-D", "d", "--host", "0.0.0.0", "--port", "7000", "--password", "pw", "-v"});
    CHECK(args.command == "start");
    CHECK(args.dataDir == "d");
    CHECK(args.host.value() == "0.0.0.0");
    CHECK(args.port.value() == 7000);
    CHECK(args.password.value() == "pw");
    CHECK(args.verbose);

    args = parseCliArgs({"server", "--data=x", "--port=1234"});
    CHECK(args.dataDir == "x");
    CHECK(args.port.value() == 1234);
}

TEST_CASE("cli environment variables set the defaults", "[cli]") {
    CleanEnv env;
    sys::setEnv("MERADB_PORT", "7001");
    sys::setEnv("MERADB_PASSWORD", "envpw");
    sys::setEnv("MERADB_USER", "envuser");
    CHECK(parseCliArgs({"server"}).port.value() == 7001);
    CHECK(parseCliArgs({"server"}).password.value() == "envpw");
    CHECK(parseCliArgs({"server", "--port", "9"}).port.value() == 9);  // a flag beats the environment
    CHECK(parseCliArgs({"shell"}).user.value() == "envuser");
    CHECK_FALSE(parseCliArgs({"shell"}).port.has_value());  // client: MERADB_PORT is applied later, as a non-explicit default
    sys::setEnv("MERADB_DATA", "envdata");
    CHECK(parseCliArgs({"status"}).dataDir == "envdata");
}

TEST_CASE("cli client options", "[cli]") {
    CleanEnv env;
    CliArgs args = parseCliArgs({"run", "a.mdb", "b.mdb", "-H", "10.0.0.5", "-p", "6400", "-d", "school", "-W", "-U", "asha", "--local"});
    CHECK(args.command == "run");
    CHECK(args.files == std::vector<std::string>{"a.mdb", "b.mdb"});
    CHECK(args.host.value() == "10.0.0.5");
    CHECK(args.port.value() == 6400);
    CHECK(args.database.value() == "school");
    CHECK(args.askPassword);
    CHECK(args.user.value() == "asha");
    CHECK(args.local);
}

TEST_CASE("cli stop and status options", "[cli]") {
    CleanEnv env;
    CliArgs stop = parseCliArgs({"stop", "--force", "-W", "-D", "d"});
    CHECK(stop.force);
    CHECK(stop.askPassword);
    CHECK(stop.dataDir == "d");
    CHECK_FALSE(parseCliArgs({"status"}).force);
}

TEST_CASE("cli bad arguments are reported", "[cli]") {
    CleanEnv env;
    CHECK(parseCliArgs({"run"}).error == "the following arguments are required: files");
    CHECK(parseCliArgs({"server", "--port", "abc"}).error == "argument --port: invalid int value: 'abc'");
    CHECK(parseCliArgs({"server", "--port"}).error == "argument --port: expected one argument");
    CHECK(parseCliArgs({"status", "--nope"}).error == "unrecognized arguments: --nope");
    CHECK(parseCliArgs({"stop", "extra"}).error == "unrecognized arguments: extra");
    CHECK(parseCliArgs({"server", "--force"}).error == "unrecognized arguments: --force");  // stop only
    CHECK(parseCliArgs({"shell", "-p"}).error == "argument -p/--port: expected one argument");
    CHECK(parseCliArgs({"stop", "-D", "--force"}).error == "argument -D/--data: expected one argument");
    CHECK(parseCliArgs({"run", "-p", "x", "a.mdb"}).error == "argument -p/--port: invalid int value: 'x'");
    CHECK(parseCliArgs({"status", "--nope", "more"}).error == "unrecognized arguments: --nope more");
    CHECK_FALSE(parseCliArgs({"status", "--nope"}).errorInSubcommand);
    CHECK(parseCliArgs({"server", "--port", "abc"}).errorInSubcommand);
}

TEST_CASE("cli the default data folder is the per-user one", "[cli]") {
    CleanEnv env;
    CHECK(parseCliArgs({"run", "x.mdb"}).dataDir == protocol::defaultDataDir());
}

TEST_CASE("cli cliMain prints version and help", "[cli]") {
    Capture capture;
    CHECK(cliMain({"--version"}) == 0);
    CHECK(capture.out() == "MeraDB 1.0.0\n");
    CHECK(cliMain({"--help"}) == 0);
    CHECK(capture.out().find("usage: meradb") != std::string::npos);
    CHECK(cliMain({"run"}) == 2);
    CHECK(capture.err().find("usage: meradb run [-h] [-D DATA] [-H HOST]") == 0);
    CHECK(capture.err().find("meradb run: error: the following arguments are required: files\n") != std::string::npos);
}

TEST_CASE("cli run --local executes a script and returns 1 on any failure", "[cli]") {
    CleanEnv env;
    TempDir dir;
    writeFile(dir.file("good.mdb"), "BANAO TABLE t (x INT); DAALO MEIN t MAAN (1); DIKHAO * SE t;");
    writeFile(dir.file("bad.mdb"), "DIKHAO * SE gayab;");
    {
        Capture capture;
        CHECK(cliMain({"run", dir.file("good.mdb"), "--local", "--data", dir.file("data")}) == 0);
        CHECK(capture.out().find("1 row(s)") != std::string::npos);
    }
    {
        Capture capture;
        CHECK(cliMain({"run", dir.file("bad.mdb"), dir.file("good.mdb"), "--local", "--data", dir.file("data2")}) == 1);
        CHECK(capture.out().find("[Execution Galti] Table 'gayab' exist nahi karta") != std::string::npos);
        CHECK(capture.out().find("1 row(s)") != std::string::npos);  // the second file still ran
    }
    {
        Capture capture;
        CHECK(cliMain({"run", dir.file("missing.mdb"), "--local", "--data", dir.file("data3")}) == 1);
        CHECK(capture.out().find("File nahi khuli: [Errno 2] No such file or directory: '") == 0);
    }
}

TEST_CASE("cli --local with a user says it is ignored", "[cli]") {
    CleanEnv env;
    TempDir dir;
    writeFile(dir.file("s.mdb"), "DIKHAO TABLES;");
    Capture capture;
    CHECK(cliMain({"run", dir.file("s.mdb"), "--local", "-U", "asha", "--data", dir.file("d")}) == 0);
    CHECK(capture.err() == "(--local mode mein -U/--user 'asha' ka koi matlab nahi -- ignore kiya, superuser ki tarah chal raha hai)\n");
}

TEST_CASE("cli falls back to local mode when no server is found", "[cli]") {
    CleanEnv env;
    TempDir dir;
    RunningServer other;  // find a free port by borrowing one, then stop the server so nothing listens on it
    int freePort = other.port();
    other.stop();
    sys::setEnv("MERADB_PORT", std::to_string(freePort));
    writeFile(dir.file("s.mdb"), "DIKHAO TABLES;");
    Capture capture;
    CHECK(cliMain({"run", dir.file("s.mdb"), "--data", dir.file("d")}) == 0);
    CHECK(capture.err().find("(127.0.0.1:" + std::to_string(freePort) + " par server nahi mila -- LOCAL mode: seedha '") == 0);
    CHECK(capture.err().find("Server ke liye: meradb start)\n") != std::string::npos);
}

TEST_CASE("cli an explicit --port does not fall back", "[cli]") {
    CleanEnv env;
    TempDir dir;
    RunningServer other;
    int freePort = other.port();
    other.stop();
    writeFile(dir.file("s.mdb"), "DIKHAO TABLES;");
    Capture capture;
    CHECK(cliMain({"run", dir.file("s.mdb"), "--port", std::to_string(freePort), "--data", dir.file("d")}) == 1);
    CHECK(capture.err().find("par MeraDB server nahi mila") != std::string::npos);
}

TEST_CASE("cli run goes through a server when one answers", "[cli]") {
    CleanEnv env;
    TempDir dir;
    RunningServer s;
    writeFile(dir.file("s.mdb"), "BANAO TABLE via_server (x INT); DIKHAO TABLES;");
    Capture capture;
    CHECK(cliMain({"run", dir.file("s.mdb"), "--port", std::to_string(s.port())}) == 0);
    CHECK(capture.out().find("via_server") != std::string::npos);
    CHECK(std::filesystem::exists(std::filesystem::path(s.dataDir()) / "main"));  // it really ran in the server's folder
}

TEST_CASE("cli run with a user logs in and honours privileges", "[cli]") {
    CleanEnv env;
    TempDir dir;
    RunningServer s;
    s.instance().users().create("asha", "pw");
    writeFile(dir.file("s.mdb"), "DIKHAO TABLES;");
    {
        Capture capture;
        sys::setEnv("MERADB_PASSWORD", "pw");
        // logged in as asha: a restricted user, so DDL / admin commands are refused
        CHECK(cliMain({"run", dir.file("s.mdb"), "--port", std::to_string(s.port()), "-U", "asha"}) == 1);
        CHECK(capture.out().find("'asha' superuser nahi hai") != std::string::npos);
        CHECK(capture.err().empty());
    }
    {
        Capture capture;
        sys::setEnv("MERADB_PASSWORD", "wrong");
        CHECK(cliMain({"run", dir.file("s.mdb"), "--port", std::to_string(s.port()), "-U", "asha"}) == 1);
        CHECK_FALSE(capture.err().empty());
    }
}

TEST_CASE("cli the workbench says it is not here yet", "[cli]") {
    CleanEnv env;
    Capture capture;
    CHECK(cliMain({"workbench"}) == 1);
    CHECK(capture.err().find("abhi C++ version mein nahi hai") != std::string::npos);
}

TEST_CASE("cli the shell banner version is the one the server reports", "[cli][shell]") {
    CHECK(std::string(protocol::kServerName) == std::string("MeraDB ") + protocol::kProgramVersion);
}

TEST_CASE("cli shell --local runs a piped session: banner, statements, goodbye", "[cli][shell]") {
    CleanEnv env;
    TempDir dir;
    std::istringstream input("BANAO TABLE t (x INT);\nDAALO MEIN t MAAN (1);\nDIKHAO * SE t;\n.exit\n");
    ShellInputOverride feed(input);
    Capture capture;
    CHECK(cliMain({"shell", "--local", "--data", dir.file("d")}) == 0);
    const std::string out = capture.out();
    CHECK(out.find("connected: local (") != std::string::npos);
    CHECK(out.find("meradb:main> Table 't' ban gaya (1 columns)\n\n") != std::string::npos);
    CHECK(out.find("| 1 |") != std::string::npos);
    const std::string goodbye = "meradb:main> Phir milenge!\n";  // .exit: no newline before it
    REQUIRE(out.size() > goodbye.size());
    CHECK(out.substr(out.size() - goodbye.size()) == goodbye);
    CHECK(capture.err().empty());
    CHECK(std::filesystem::exists(dir.file("d")));  // it really opened the folder it was told to
}

TEST_CASE("cli no command at all is the shell, and end of input says goodbye", "[cli][shell]") {
    CleanEnv env;
    TempDir dir;
    std::istringstream input("");
    ShellInputOverride feed(input);
    Capture capture;
    CHECK(cliMain({"--local", "--data", dir.file("d")}) == 0);
    const std::string out = capture.out();
    const std::string tail = "meradb:main> \nPhir milenge!\n";
    REQUIRE(out.size() > tail.size());
    CHECK(out.substr(out.size() - tail.size()) == tail);
}

TEST_CASE("cli shell goes through a server when one answers", "[cli][shell]") {
    CleanEnv env;
    RunningServer s;
    std::istringstream input("BANAO TABLE via_shell (x INT);\nDIKHAO TABLES;\n");
    ShellInputOverride feed(input);
    Capture capture;
    CHECK(cliMain({"shell", "--port", std::to_string(s.port())}) == 0);
    const std::string out = capture.out();
    CHECK(out.find("connected: 127.0.0.1:" + std::to_string(s.port()) + "\n") != std::string::npos);
    CHECK(out.find("via_shell") != std::string::npos);
    CHECK(std::filesystem::exists(std::filesystem::path(s.dataDir()) / "main"));
}

TEST_CASE("cli shell reports a connection failure before any banner", "[cli][shell]") {
    CleanEnv env;
    RunningServer other;
    const int freePort = other.port();
    other.stop();
    std::istringstream input(".exit\n");
    ShellInputOverride feed(input);
    Capture capture;
    CHECK(cliMain({"shell", "--port", std::to_string(freePort)}) == 1);
    CHECK(capture.out().empty());
    CHECK(capture.err().find("par MeraDB server nahi mila") != std::string::npos);
}

TEST_CASE("cli each subcommand's --help matches argparse's text", "[cli]") {
    // golden_help.h holds `python -m meradb COMMAND --help` at COLUMNS=80 (gen_help_golden.py); the
    // data folder's default comes from MERADB_DATA, a short one and a long one that wraps.
    CleanEnv env;
    int count = 0;
    const golden_help::Case* cases = golden_help::cases(count);
    REQUIRE(count == 14);
    for (int i = 0; i < count; ++i) {
        INFO(cases[i].command << " with data folder " << cases[i].dataDir);
        sys::setEnv("MERADB_DATA", cases[i].dataDir);
        Capture capture;
        CHECK(cliMain({cases[i].command, "--help"}) == 0);
        CHECK(capture.out() == cases[i].text);
        CHECK(capture.err().empty());
    }
    sys::setEnv("MERADB_DATA", "/srv/mdb");
    Capture capture;
    CHECK(cliMain({"tui", "-h"}) == 0);  // the alias prints as the workbench, like Python
    CHECK(capture.out().rfind("usage: meradb workbench [-h]", 0) == 0);
}

// Every expectation below was taken from `python -m meradb` (argparse, Python 3.12).
TEST_CASE("cli a file name that starts like an option letter is still a file", "[cli]") {
    CleanEnv env;
    auto run = parseCliArgs({"run", "adir", "pdata.sql", "Hfile", "Ufoo", "Wx", "-d", "db"});
    CHECK(run.error.empty());
    CHECK(run.files == std::vector<std::string>{"adir", "pdata.sql", "Hfile", "Ufoo", "Wx"});
    CHECK(run.database == "db");
    CHECK(parseCliArgs({"status", "adir"}).error == "unrecognized arguments: adir");
}

TEST_CASE("cli glued short options, bundles and explicit values follow argparse", "[cli]") {
    CleanEnv env;
    CHECK(parseCliArgs({"status", "-DX"}).dataDir == "X");
    CHECK(parseCliArgs({"status", "-D=X"}).dataDir == "X");
    CHECK(parseCliArgs({"status", "-D==X"}).dataDir == "=X");
    CHECK(parseCliArgs({"status", "-D="}).dataDir.empty());
    CHECK(parseCliArgs({"status", "-Dx=y"}).dataDir == "x=y");
    auto bundle = parseCliArgs({"status", "-WD", "dir"});
    CHECK(bundle.askPassword);
    CHECK(bundle.dataDir == "dir");
    CHECK(parseCliArgs({"status", "-WDdir"}).dataDir == "dir");
    CHECK(parseCliArgs({"status", "-WD=dir"}).dataDir == "dir");
    auto run = parseCliArgs({"run", "-Hlocalhost", "-p7", "-d=db", "--local", "x.mdb"});
    CHECK(run.error.empty());
    CHECK(run.host == "localhost");
    CHECK(run.port == 7);
    CHECK(run.database == "db");
    CHECK(parseCliArgs({"run", "-p-1", "--local", "x.mdb"}).port == -1);
    CHECK(parseCliArgs({"server", "-vv"}).verbose);

    // a flag takes no value
    auto flag = parseCliArgs({"run", "--local=1", "a.mdb"});
    CHECK(flag.error == "argument --local: ignored explicit argument '1'");
    CHECK(flag.errorInSubcommand);
    CHECK(parseCliArgs({"stop", "--force="}).error == "argument --force: ignored explicit argument ''");
    CHECK(parseCliArgs({"status", "-W=x"}).error == "argument -W/--password: ignored explicit argument 'x'");
    CHECK(parseCliArgs({"status", "--password=x"}).error == "argument -W/--password: ignored explicit argument 'x'");
    CHECK(parseCliArgs({"server", "-v=1"}).error == "argument -v/--verbose: ignored explicit argument '1'");
    CHECK(parseCliArgs({"status", "--help=1"}).error == "argument -h/--help: ignored explicit argument '1'");

    // the part of a bundle that is not an option is left over
    CHECK(parseCliArgs({"status", "-WX"}).error == "unrecognized arguments: -X");
    CHECK(parseCliArgs({"stop", "--force", "-Wx"}).error == "unrecognized arguments: -x");
    CHECK(parseCliArgs({"status", "-Q"}).error == "unrecognized arguments: -Q");
}

TEST_CASE("cli integers are read like Python's int()", "[cli]") {
    CleanEnv env;
    CHECK(parseCliArgs({"run", "-p", " 7 ", "--local", "x"}).port == 7);
    CHECK(parseCliArgs({"run", "-p", "1_0", "--local", "x"}).port == 10);
    CHECK(parseCliArgs({"run", "-p", "+7", "--local", "x"}).port == 7);
    for (const char* bad : {"7_", "_7", "1__0", "", " ", "-", "7x", "0x10", "1.5"})
        CHECK(parseCliArgs({"run", "-p", bad, "--local", "x"}).error.rfind("argument -p/--port: invalid int value", 0) == 0);
}

TEST_CASE("cli an empty unrecognized word keeps its separator", "[cli]") {
    CleanEnv env;
    CHECK(parseCliArgs({"status", "", "a"}).error == "unrecognized arguments:  a");
}
