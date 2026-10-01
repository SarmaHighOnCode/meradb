// cpp/tests/test_server_control.cpp -- process helpers and start/stop/status.
//
// serverStop's --force path is not tested here: it would kill THIS test process
// (the pid file of an in-process server holds our own pid). The end-to-end
// ctest cli_lifecycle.py covers start / status / stop / --force with real
// server processes.
#include <catch2/catch_test_macros.hpp>
#include "meradb/errors.h"
#include "meradb/server_control.h"
#include "meradb/sys_compat.h"
#include "server_fixture.h"
#include <filesystem>
#include <iostream>
#include <sstream>

using namespace meradb;
using namespace meradb_test;

namespace {

// Captures stdout and stderr while alive.
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

// Make an in-process server look like a `meradb start`ed one: a pid file for its folder.
void writeFakePidFile(RunningServer& s) {
    protocol::Json info = protocol::Json::object();
    info["pid"] = 999999;
    info["host"] = "127.0.0.1";
    info["port"] = s.port();
    info["started"] = "2026-01-01 00:00:00";
    protocol::writePidFile(s.dataDir(), info);
}

}  // namespace

TEST_CASE("sys_process setEnv and getEnv round trip", "[sys]") {
    sys::setEnv("MERADB_TEST_ENV_VALUE", "hello world");
    CHECK(sys::getEnv("MERADB_TEST_ENV_VALUE").value() == "hello world");
    sys::setEnv("MERADB_TEST_ENV_VALUE", "");
    auto value = sys::getEnv("MERADB_TEST_ENV_VALUE");
    CHECK((!value || value->empty()));
}

TEST_CASE("sys_process executablePath names this running program", "[sys]") {
    std::string path = sys::executablePath();
    REQUIRE_FALSE(path.empty());
    CHECK(std::filesystem::exists(std::filesystem::u8path(path)));
}

TEST_CASE("sys_process spawnDetached runs a program and reports its exit", "[sys]") {
    TempDir dir;
    auto child = sys::spawnDetached(sys::executablePath(), {"--list-tests"}, dir.file("child.log"));
    REQUIRE(child);
    CHECK(child->pid() > 0);
    CHECK(waitFor([&] { return child->exited(); }, 10.0));
    CHECK(readText(dir.file("child.log")).find("sys_process spawnDetached") != std::string::npos);  // its stdout went to the log
}

TEST_CASE("sys_process spawnDetached passes awkward arguments through intact", "[sys]") {
    TempDir dir;
    // Catch2 lists the tests matching this filter: the argument must arrive as ONE token.
    auto child = sys::spawnDetached(sys::executablePath(), {"--list-tests", "sys_process setEnv and getEnv round trip"},
                                    dir.file("child.log"));
    REQUIRE(waitFor([&] { return child->exited(); }, 10.0));
    std::string log = readText(dir.file("child.log"));
    CHECK(log.find("sys_process setEnv and getEnv round trip") != std::string::npos);
    CHECK(log.find("sys_process executablePath") == std::string::npos);
}

TEST_CASE("sys_process spawnDetached of a missing program fails clearly or exits at once", "[sys]") {
    TempDir dir;
    bool refused = false;
    try {
        auto child = sys::spawnDetached(dir.file("no_such_program.exe"), {}, dir.file("child.log"));
        refused = waitFor([&] { return child->exited(); }, 5.0);  // POSIX: exec fails inside the child
    } catch (const MeraDBError&) {
        refused = true;  // Windows: CreateProcess fails in the parent
    }
    CHECK(refused);
}

TEST_CASE("sys_process killProcess ends a spawned process", "[sys]") {
    TempDir dir;
    // The "sleeper" test below only sleeps when the environment says so, so the child stays alive until killed
    // while an ordinary run of the whole suite (and ctest's per-test discovery) passes straight through it.
    sys::setEnv("MERADB_TEST_SLEEPER", "1");
    auto child = sys::spawnDetached(sys::executablePath(), {"sys_process sleeper for kill test"}, dir.file("child.log"));
    sys::setEnv("MERADB_TEST_SLEEPER", "");
    REQUIRE(child);
    sleepMs(300);
    CHECK_FALSE(child->exited());
    CHECK(sys::killProcess(child->pid()).empty());
    CHECK(waitFor([&] { return child->exited(); }, 10.0));
}

TEST_CASE("sys_process sleeper for kill test", "[sleeper]") {
    auto flag = sys::getEnv("MERADB_TEST_SLEEPER");
    if (flag && !flag->empty()) sleepMs(30000);  // only the child of the kill test sleeps
}

TEST_CASE("sys_process killProcess of a process that does not exist reports an error", "[sys]") {
    CHECK_FALSE(sys::killProcess(2000000000).empty());
}

TEST_CASE("control status: says so when no server runs", "[control]") {
    TempDir dir;
    ControlOptions options;
    options.dataDir = dir.str();
    Capture capture;
    CHECK(serverStatus(options) == 3);
    CHECK(capture.out().find("MeraDB server nahi chal raha  (data: ") == 0);
    CHECK(capture.out().find("Start karne ke liye: meradb start\n") != std::string::npos);
}

TEST_CASE("control status: describes a running server", "[control]") {
    RunningServer s;
    writeFakePidFile(s);
    ControlOptions options;
    options.dataDir = s.dataDir();
    Capture capture;
    CHECK(serverStatus(options) == 0);
    std::string out = capture.out();
    CHECK(out.find("MeraDB server chal raha hai\n  address:   127.0.0.1:" + std::to_string(s.port()) +
                   "\n  pid:       999999\n") == 0);
    CHECK(out.find("  started:   2026-01-01 00:00:00\n  version:   MeraDB 1.0.0\n  sessions:  ") != std::string::npos);
    CHECK(out.find(" connected\n  databases: main\n") != std::string::npos);
}

TEST_CASE("control status: asks for the password when the server has one", "[control]") {
    RunningServer s("sekrit");
    writeFakePidFile(s);
    ControlOptions options;
    options.dataDir = s.dataDir();
    {
        Capture capture;
        CHECK(serverStatus(options) == 0);  // still "running": only the details are missing
        CHECK(capture.out().find("  (details nahi mile: ") != std::string::npos);
    }
    options.password = "sekrit";
    Capture capture;
    CHECK(serverStatus(options) == 0);
    CHECK(capture.out().find("  version:   MeraDB 1.0.0\n") != std::string::npos);
}

TEST_CASE("control stop: with nothing running removes a stale pid file", "[control]") {
    TempDir dir;
    protocol::Json info = protocol::Json::object();
    info["pid"] = 999999;
    info["host"] = "127.0.0.1";
    info["port"] = 1;  // nobody listens there
    protocol::writePidFile(dir.str(), info);
    ControlOptions options;
    options.dataDir = dir.str();
    Capture capture;
    CHECK(serverStop(options) == 0);
    CHECK(capture.out() == "Server nahi chal raha.\n");
    CHECK_FALSE(std::filesystem::exists(protocol::pidFilePath(dir.str())));
}

TEST_CASE("control stop: shuts a running server down", "[control]") {
    RunningServer s;
    writeFakePidFile(s);
    ControlOptions options;
    options.dataDir = s.dataDir();
    Capture capture;
    CHECK(serverStop(options) == 0);
    CHECK(capture.out() == "MeraDB server band ho gaya (pid 999999).\n");
    CHECK(waitFor([&] { return s.stopped(); }));
}

TEST_CASE("control stop: without the password fails and points at --force", "[control]") {
    RunningServer s("sekrit");
    writeFakePidFile(s);
    ControlOptions options;
    options.dataDir = s.dataDir();
    Capture capture;
    CHECK(serverStop(options) == 1);
    CHECK(capture.err().find("Zabardasti band karne ke liye:  meradb stop --force\n") != std::string::npos);
    CHECK_FALSE(s.stopped());
}

TEST_CASE("control stop --force: a hand-edited fractional pid is never used to kill anything", "[control]") {
    RunningServer s("sekrit");
    protocol::Json info = protocol::Json::object();
    info["pid"] = 3.5;
    info["host"] = "127.0.0.1";
    info["port"] = s.port();
    protocol::writePidFile(s.dataDir(), info);
    ControlOptions options;
    options.dataDir = s.dataDir();
    options.force = true;
    Capture capture;
    CHECK(serverStop(options) == 1);
    CHECK(capture.err().find("Kill fail: pid galat hai: 0") != std::string::npos);
    CHECK_FALSE(s.stopped());
}

TEST_CASE("control start: refuses when a server already runs for the folder", "[control]") {
    RunningServer s;
    writeFakePidFile(s);
    ControlOptions options;
    options.dataDir = s.dataDir();
    Capture capture;
    CHECK(serverStart(options) == 0);
    CHECK(capture.out() == "Server pehle se chal raha hai: 127.0.0.1:" + std::to_string(s.port()) + " (pid 999999)\n");
}

TEST_CASE("control start: refuses a port something else is using", "[control]") {
    RunningServer other;  // holds a port, but has no pid file for the folder we start in
    TempDir dir;
    ControlOptions options;
    options.dataDir = dir.str();
    options.port = other.port();
    Capture capture;
    CHECK(serverStart(options) == 1);
    CHECK(capture.err().find("Port " + std::to_string(other.port()) + " par pehle se kuch aur chal raha hai.") == 0);
}

TEST_CASE("control start: reports a child that dies at once, with its log", "[control]") {
    TempDir dir;
    ControlOptions options;
    options.dataDir = dir.str();
    options.port = 1;  // the child (this test program) rejects the unknown arguments and exits
    options.startTimeoutSeconds = 10.0;
    Capture capture;
    CHECK(serverStart(options) == 1);
    CHECK(capture.err().find("Server start nahi hua. Log (") == 0);
    CHECK(std::filesystem::exists(dir.path() / "server.log"));
}
