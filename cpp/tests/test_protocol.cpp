// cpp/tests/test_protocol.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/errors.h"
#include "meradb/protocol.h"
#include "test_util.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace meradb;
using namespace meradb::protocol;

namespace {

// A connected pair of loopback sockets: `client` writes, `server` reads.
struct Pair {
    net::Socket listener = net::listenOn("127.0.0.1", 0);
    net::Socket client = net::connectTo("127.0.0.1", listener.localPort(), 2.0);
    net::Socket server = net::acceptWithTimeout(listener, 2.0);
};

}  // namespace

// ---- framing ----

TEST_CASE("protocol reader splits messages and keeps the remainder", "[protocol]") {
    Pair p;
    MessageReader reader(p.server);
    p.client.sendAll("{\"a\": 1}\n{\"b\"");
    auto first = reader.receive();
    REQUIRE(first.has_value());
    CHECK(pyjson::dump(*first) == "{\"a\": 1}");
    p.client.sendAll(": 2}\n{\"c\": 3}\n");
    CHECK(pyjson::dump(*reader.receive()) == "{\"b\": 2}");
    CHECK(pyjson::dump(*reader.receive()) == "{\"c\": 3}");
    p.client.close();
    CHECK_FALSE(reader.receive().has_value());  // clean end of stream
}

TEST_CASE("protocol send writes one line of Python-style JSON", "[protocol]") {
    Pair p;
    Json message = Json::object();
    message["type"] = "query";
    message["text"] = "DIKHAO * SE t";
    send(p.client, message);
    p.client.close();
    MessageReader reader(p.server);
    auto got = reader.receive();
    REQUIRE(got.has_value());
    CHECK(got->at("text") == "DIKHAO * SE t");
    char buffer[4];
    CHECK(p.server.recvSome(buffer, sizeof buffer) == 0);  // nothing but that one line was sent
}

TEST_CASE("protocol reader reports bad messages and carries on", "[protocol]") {
    Pair p;
    MessageReader reader(p.server);
    p.client.sendAll("not json\n[1, 2]\n\n{\"ok\": true}\n");
    try {
        reader.receive();
        FAIL("expected a ProtocolError");
    } catch (const ProtocolError& e) {
        CHECK(std::string(e.what()).rfind("Galat message: ", 0) == 0);
    }
    try {
        reader.receive();
        FAIL("expected a ProtocolError");
    } catch (const ProtocolError& e) {
        CHECK(std::string(e.what()) == "Message ek JSON object hona chahiye");
    }
    CHECK_THROWS_AS(reader.receive(), ProtocolError);  // the empty line
    auto good = reader.receive();
    REQUIRE(good.has_value());
    CHECK(good->at("ok") == true);
}

TEST_CASE("protocol reader rejects invalid UTF-8", "[protocol]") {
    Pair p;
    MessageReader reader(p.server);
    p.client.sendAll("{\"a\": \"\xC3(\"}\n{\"b\": 1}\n");
    CHECK_THROWS_AS(reader.receive(), ProtocolError);
    CHECK(reader.receive().has_value());
}

TEST_CASE("protocol reader accepts an unterminated last line like readline does", "[protocol]") {
    Pair p;
    MessageReader reader(p.server);
    p.client.sendAll("{\"last\": true}");
    p.client.close();
    auto got = reader.receive();
    REQUIRE(got.has_value());
    CHECK(got->at("last") == true);
    CHECK_FALSE(reader.receive().has_value());
}

TEST_CASE("protocol reader discards an oversize line and recovers", "[protocol]") {
    Pair p;
    MessageReader reader(p.server, 64);  // tiny cap for the test
    p.client.sendAll(std::string(200, 'x') + "\n{\"after\": 1}\n");
    try {
        reader.receive();
        FAIL("expected a ProtocolError");
    } catch (const ProtocolError& e) {
        CHECK(std::string(e.what()) == "Message bahut bada hai");
    }
    auto after = reader.receive();  // the rest of the long line is skipped, not read as a message
    REQUIRE(after.has_value());
    CHECK(after->at("after") == 1);
}

TEST_CASE("protocol reader size limit counts the newline like Python", "[protocol]") {
    Pair p;
    MessageReader reader(p.server, 16);
    // 15 bytes + "\n" = 16: allowed. 16 bytes + "\n" = 17: too big.
    p.client.sendAll("{\"k\": \"123456\"}\n");  // 15 bytes
    CHECK(reader.receive().has_value());
    p.client.sendAll("{\"k\": \"1234567\"}\n");  // 16 bytes
    CHECK_THROWS_AS(reader.receive(), ProtocolError);
}

TEST_CASE("protocol reader gives up when its stop flag is set", "[protocol]") {
    Pair p;
    std::atomic<bool> stop{false};
    MessageReader reader(p.server, kMaxMessageBytes, &stop);
    std::atomic<bool> gaveUp{false};
    std::thread t([&] {
        auto got = reader.receive();  // blocks: the peer says nothing
        gaveUp = !got.has_value();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK_FALSE(gaveUp.load());
    stop = true;
    t.join();
    CHECK(gaveUp.load());
}

// ---- cells and results ----

TEST_CASE("protocol results encode exactly like Python's Result.to_dict", "[protocol]") {
    Result r;
    r.columns = {"a", "d", "t", "b", "n"};
    r.rows.push_back({Value(1.0), Value(parseDate("2024-01-05")),
                      Value(std::string("h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x98\x80 \\\" \x7f")), Value(true),
                      Value()});
    r.message = "1 row(s)";
    Json message = Json::object();
    message["ok"] = true;
    message["results"] = Json::array({resultToJson(r)});
    CHECK(pyjson::dump(message) ==
          "{\"ok\": true, \"results\": [{\"columns\": [\"a\", \"d\", \"t\", \"b\", \"n\"], \"rows\": "
          "[[1.0, {\"$date\": \"2024-01-05\"}, \"h\\u00e9llo \\u4e16\\u754c \\ud83d\\ude00 \\\\\\\" \\u007f\", true, null]], "
          "\"message\": \"1 row(s)\", \"error\": \"\"}]}");
}

TEST_CASE("protocol results survive a round trip and keep int/float apart", "[protocol]") {
    Result r;
    r.columns = {"i", "f", "s", "b", "n", "d"};
    r.rows.push_back({Value(int64_t{7}), Value(7.0), Value(std::string("x")), Value(false), Value(),
                      Value(parseDate("2005-06-15"))});
    r.rows.push_back({Value(int64_t{-9007199254740993}), Value(1e20), Value(std::string("")), Value(true), Value(),
                      Value()});
    Result back = resultFromJson(pyjson::parse(pyjson::dump(resultToJson(r))));
    REQUIRE(back.rows.size() == 2);
    CHECK(back.columns == r.columns);
    CHECK(std::holds_alternative<int64_t>(back.rows[0][0].data));
    CHECK(std::holds_alternative<double>(back.rows[0][1].data));  // 7.0 stays a float
    CHECK(std::get<int64_t>(back.rows[1][0].data) == -9007199254740993);
    CHECK(std::holds_alternative<Date>(back.rows[0][5].data));
    CHECK(std::get<Date>(back.rows[0][5].data) == parseDate("2005-06-15"));
    CHECK(back.rows[0][4].isNull());
}

TEST_CASE("protocol result decoding tolerates missing keys and rejects garbage", "[protocol]") {
    Result empty = resultFromJson(pyjson::parse("{}"));
    CHECK(empty.columns.empty());
    CHECK(empty.rows.empty());
    CHECK(empty.error.empty());
    CHECK_THROWS_AS(resultFromJson(pyjson::parse("[]")), ProtocolError);
    CHECK_THROWS_AS(resultFromJson(pyjson::parse("{\"rows\": [[[1]]]}")), ProtocolError);
    CHECK_THROWS_AS(resultFromJson(pyjson::parse("{\"rows\": [[{\"$date\": \"2024-13-45\"}]]}")), ProtocolError);
    CHECK_THROWS_AS(resultFromJson(pyjson::parse("{\"message\": 5}")), ProtocolError);
}

// ---- data folder and pid file ----

TEST_CASE("protocol defaultDataDirFrom follows Python's default_data_dir", "[protocol]") {
    namespace fs = std::filesystem;
    CHECK(defaultDataDirFrom("/custom/data", "C:\\Users\\x\\AppData\\Local", "/home/x", true) == "/custom/data");
    CHECK(defaultDataDirFrom(std::string(""), std::string("C:\\L"), "/home/x", true) ==
          (fs::path("C:\\L") / "MeraDB" / "data").string());  // an empty MERADB_DATA counts as unset
    CHECK(defaultDataDirFrom(std::nullopt, std::string("C:\\L"), "/home/x", false) ==
          (fs::path("/home/x") / ".local" / "share" / "MeraDB" / "data").string());  // LOCALAPPDATA is Windows-only
    CHECK(defaultDataDirFrom(std::nullopt, std::nullopt, "/home/x", true) ==
          (fs::path("/home/x") / ".local" / "share" / "MeraDB" / "data").string());
    CHECK_FALSE(defaultDataDir().empty());
}

TEST_CASE("protocol pid file has Python's json.dump(indent=2) shape", "[protocol]") {
    meradb_test::TempDir dir;
    Json info = Json::object();
    info["pid"] = 1234;
    info["host"] = "127.0.0.1";
    info["port"] = 6372;
    info["started"] = "2026-09-29T14:03:07";
    writePidFile(dir.str(), info);
    CHECK(meradb_test::readText(pidFilePath(dir.str())) ==
          "{\n  \"pid\": 1234,\n  \"host\": \"127.0.0.1\",\n  \"port\": 6372,\n  \"started\": \"2026-09-29T14:03:07\"\n}");
    auto back = readPidFile(dir.str());
    REQUIRE(back.has_value());
    CHECK(back->at("port") == 6372);
}

TEST_CASE("protocol removePidFile only removes the file of the given process", "[protocol]") {
    meradb_test::TempDir dir;
    Json info = Json::object();
    info["pid"] = 42;
    writePidFile(dir.str(), info);
    removePidFile(dir.str(), 41);
    CHECK(readPidFile(dir.str()).has_value());
    removePidFile(dir.str(), 42);
    CHECK_FALSE(readPidFile(dir.str()).has_value());
    removePidFile(dir.str(), 42);  // already gone: harmless
}

TEST_CASE("protocol readPidFile ignores junk", "[protocol]") {
    meradb_test::TempDir dir;
    CHECK_FALSE(readPidFile(dir.str()).has_value());  // no file
    std::ofstream(pidFilePath(dir.str())) << "not json";
    CHECK_FALSE(readPidFile(dir.str()).has_value());
    std::ofstream(pidFilePath(dir.str())) << "[1, 2]";
    CHECK_FALSE(readPidFile(dir.str()).has_value());
}

TEST_CASE("protocol runningServer needs a live listener behind the pid file", "[protocol]") {
    meradb_test::TempDir dir;
    CHECK_FALSE(runningServer(dir.str()).has_value());

    net::Socket listener = net::listenOn("127.0.0.1", 0);
    Json info = Json::object();
    info["pid"] = 1;
    info["host"] = "0.0.0.0";  // a server bound to every interface is reached via loopback
    info["port"] = listener.localPort();
    writePidFile(dir.str(), info);
    auto alive = runningServer(dir.str());
    REQUIRE(alive.has_value());
    CHECK(alive->at("pid") == 1);

    listener.close();
    CHECK_FALSE(runningServer(dir.str()).has_value());  // stale: nothing listens any more
}
