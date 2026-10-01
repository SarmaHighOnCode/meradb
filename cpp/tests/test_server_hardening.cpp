// cpp/tests/test_server_hardening.cpp -- a hostile or careless client must not hurt the server or other clients.
#include <catch2/catch_test_macros.hpp>
#include "meradb/client.h"
#include "meradb/engine.h"
#include "server_fixture.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace meradb;
using namespace meradb_test;
using protocol::Json;

namespace {

ConnectOptions to(const RunningServer& s) {
    ConnectOptions options;
    options.port = s.port();
    return options;
}

bool serverStillWorks(RunningServer& s) {
    Connection db(to(s));
    return db.runScript("DIKHAO TABLES")[0].error.empty();
}

std::string errorOf(const Json& reply) { return reply.value("error", std::string("(no error)")); }

}  // namespace

TEST_CASE("hardening_server an oversize message is refused and the session survives", "[hardening][server]") {
    RunningServer s("", false, /*maxMessageBytes=*/256);
    RawClient c(s.port());
    c.hello();
    c.sendRaw(std::string(5000, 'x') + "\n");
    CHECK(errorOf(c.receive().value()) == "[Protocol Galti] Message bahut bada hai");
    CHECK(c.request(Json{{"type", "ping"}})["ok"] == true);  // the rest of the long line was skipped, not parsed
    Json big = Json::object();
    big["type"] = "query";
    big["text"] = std::string(400, ' ') + "DIKHAO TABLES";
    c.send(big);
    CHECK(errorOf(c.receive().value()) == "[Protocol Galti] Message bahut bada hai");
    CHECK(c.request(Json{{"type", "ping"}})["ok"] == true);
}

TEST_CASE("hardening_server deeply nested JSON is a protocol error not a crash", "[hardening][server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    c.sendRaw(std::string(200000, '[') + std::string(200000, ']') + "\n");
    std::string error = errorOf(c.receive().value());
    CHECK(error.rfind("[Protocol Galti] Galat message: ", 0) == 0);
    c.sendRaw("{\"type\": \"query\", \"text\": " + std::string(100000, '[') + "}\n");
    CHECK(errorOf(c.receive().value()).rfind("[Protocol Galti] ", 0) == 0);
    CHECK(c.request(Json{{"type", "ping"}})["ok"] == true);
}

TEST_CASE("hardening_server invalid UTF-8 and binary junk are protocol errors", "[hardening][server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    c.sendRaw("{\"type\": \"query\", \"text\": \"\xC3(\"}\n");
    CHECK(errorOf(c.receive().value()) == "[Protocol Galti] Galat message: 'utf-8' codec can't decode byte 0xc3 in position 27: invalid continuation byte");
    std::string junk;
    for (int i = 0; i < 300; ++i) junk += static_cast<char>((i * 37) % 251 + 1 == '\n' ? 'x' : (i * 37) % 251 + 1);
    c.sendRaw(junk + "\n");
    CHECK(errorOf(c.receive().value()).rfind("[Protocol Galti] ", 0) == 0);
    CHECK(c.request(Json{{"type", "ping"}})["ok"] == true);
}

TEST_CASE("hardening_server a client that vanishes mid-message does no harm", "[hardening][server]") {
    RunningServer s;
    {
        RawClient c(s.port());
        c.hello();
        c.sendRaw("{\"type\": \"query\", \"text\": \"DIKHAO ");  // half a message, then gone
    }
    {
        RawClient c(s.port());  // never completes the handshake either
        c.sendRaw("{\"type\": \"hel");
    }
    CHECK(waitFor([&] { return s.server().sessions() == 0; }));
    CHECK(serverStillWorks(s));
}

TEST_CASE("hardening_server odd hello fields never crash the handshake", "[hardening][server]") {
    RunningServer s;
    s.instance().users().create("5", "pw");
    const std::vector<std::string> hellos = {
        "{\"type\": \"hello\"}",
        "{\"type\": \"hello\", \"user\": 5, \"password\": \"pw\"}",  // str(5) == "5": a valid login
        "{\"type\": \"hello\", \"user\": 0, \"password\": 12}",       // falsy user: not a login at all
        "{\"type\": \"hello\", \"user\": [\"a\"], \"password\": {\"x\": 1}}",
        "{\"type\": \"hello\", \"database\": 7}",
        "{\"type\": \"hello\", \"database\": \"\", \"password\": null, \"user\": null}",
        "{\"type\": [\"hello\"]}",
        "{\"type\": \"HELLO\"}",
        "{}",
    };
    for (const auto& text : hellos) {
        RawClient c(s.port());
        c.sendRaw(text + "\n");
        try {
            c.receive();  // an answer or a plain close: both are fine
        } catch (const net::NetError&) {
        }
    }
    CHECK(serverStillWorks(s));
    RawClient login(s.port());
    CHECK(login.hello("pw", nullptr, 5)["ok"] == true);
}

TEST_CASE("hardening_server hostile queries get an error result and the session goes on", "[hardening][server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    std::string deep = "DIKHAO * SE t JAHAN " + std::string(1000000, '(');
    Json reply = c.query(deep);
    CHECK(reply["ok"] == true);
    CHECK(reply["results"][0]["error"].get<std::string>().rfind("[Parser Galti] Query bahut gehri (nested) hai", 0) == 0);
    std::string chain = "DIKHAO * SE t JAHAN x = 1";
    for (int i = 0; i < 100000; ++i) chain += " + 1";
    reply = c.query(chain);
    CHECK(reply["results"][0]["error"].get<std::string>().rfind("[Parser Galti] Query bahut gehri (nested) hai", 0) == 0);
    CHECK(c.query("DIKHAO TABLES")["results"][0]["error"] == "");
}

TEST_CASE("hardening_server a large script goes through in one message", "[hardening][server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    c.query("BANAO TABLE t (id INT MUKHYA KUNJI, s TEXT)");
    std::string insert = "DAALO MEIN t MAAN ";
    for (int i = 0; i < 3000; ++i) insert += (i ? ", (" : "(") + std::to_string(i) + ", 'row number " + std::to_string(i) + "')";
    Json reply = c.query(insert);
    CHECK(reply["results"][0]["error"] == "");
    CHECK(reply["results"][0]["message"] == "3000 row(s) daal di");
    std::string many;
    for (int i = 0; i < 400; ++i) many += "DIKHAO * SE t JAHAN id = " + std::to_string(i) + ";\n";
    reply = c.query(many);
    CHECK(reply["results"].size() == 400);
}

TEST_CASE("hardening_server pipelined requests are answered in order", "[hardening][server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    std::string burst;
    for (int i = 0; i < 100; ++i) {
        Json q = Json::object();
        q["type"] = "query";
        q["text"] = "DIKHAO TABLES; DIKHAO * SE gayab_" + std::to_string(i);  // the second statement names its request
        burst += pyjson::dump(q) + "\n";
    }
    c.sendRaw(burst);
    for (int i = 0; i < 100; ++i) {
        Json reply = c.receive().value();
        REQUIRE(reply["results"].size() == 2);
        CHECK(reply["results"][1]["error"] ==
              "[Execution Galti] Table 'gayab_" + std::to_string(i) + "' exist nahi karta");
    }
    CHECK(c.request(Json{{"type", "ping"}})["ok"] == true);
}

TEST_CASE("hardening_server shutdown with a transaction open rolls it back cleanly", "[hardening][server]") {
    TempDir dir;  // outlives the server so the folder can be reopened afterwards
    std::string dataDir;
    {
        // a server on a folder we own: RunningServer makes its own temp dir, so copy the idea by hand
        ServerOptions options;
        options.dataDir = dir.str();
        options.port = 0;
        options.log = [](const std::string&) {};
        Server server(options);
        server.instance().lockTimeoutSeconds = 0.5;
        std::thread runner([&] { server.serveForever(); });
        ConnectOptions connect;
        connect.port = server.port();
        Connection a(connect);
        a.execute("BANAO TABLE t (x INT); DAALO MEIN t MAAN (1); SHURU; DAALO MEIN t MAAN (2); MITAO SE t");
        REQUIRE(a.inTransaction());
        server.requestStop();  // the client is still connected, mid-transaction
        runner.join();         // returns only after the session thread rolled its transaction back
        dataDir = dir.str();
    }
    Engine reopened(dataDir);
    CHECK(reopened.instance().recovered().empty());  // nothing left for crash recovery to undo
    CHECK_FALSE(std::filesystem::exists(dir.path() / ".wapas" / "main"));
    auto rows = reopened.execute("DIKHAO * SE t")[0].rows;
    REQUIRE(rows.size() == 1);
    CHECK(std::get<int64_t>(rows[0][0].data) == 1);  // the transaction's insert and delete were both undone
}

TEST_CASE("hardening_server one busy transaction does not stop the server from stopping", "[hardening][server]") {
    auto s = std::make_unique<RunningServer>();
    Connection holder(to(*s));
    holder.execute("BANAO TABLE t (x INT); SHURU");
    std::thread blocked([&] {
        Connection other(to(*s));
        other.runScript("DIKHAO * SE t");  // waits for the lock (0.5 s), then reports busy
    });
    sleepMs(100);
    auto started = std::chrono::steady_clock::now();
    s->stop();
    blocked.join();
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(took < 4.0);
}

TEST_CASE("hardening_server a hundred idle connections stop promptly", "[hardening][server]") {
    auto s = std::make_unique<RunningServer>();
    std::vector<std::unique_ptr<RawClient>> clients;
    for (int i = 0; i < 100; ++i) {
        clients.push_back(std::make_unique<RawClient>(s->port()));
        clients.back()->hello();
    }
    CHECK(waitFor([&] { return s->server().sessions() == 100; }, 5.0));
    auto started = std::chrono::steady_clock::now();
    s->stop();
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(took < 4.0);
    CHECK(s->server().sessions() == 0);
}

TEST_CASE("hardening_server a client that stops reading cannot keep the server from shutting down", "[hardening][server]") {
    RunningServer s;
    {
        RawClient filler(s.port());
        filler.hello();
        filler.query("BANAO TABLE big (id INT, s TEXT)");
        const std::string row = std::string(2000, 'x');
        std::string insert = "DAALO MEIN big MAAN ";
        for (int i = 0; i < 1500; ++i) insert += (i ? ", (" : "(") + std::to_string(i) + ", '" + row + "')";
        CHECK(filler.query(insert)["results"][0]["error"] == "");
    }
    RawClient stalled(s.port());
    stalled.hello();
    for (int i = 0; i < 12; ++i) stalled.send(Json{{"type", "query"}, {"text", "DIKHAO * SE big"}});  // ~3 MB each, never read
    sleepMs(300);  // let the server fill its socket buffers and block in send()
    {
        RawClient other(s.port());
        other.hello();
        CHECK(other.request(Json{{"type", "shutdown"}})["ok"] == true);
    }
    CHECK(waitFor([&] { return s.stopped(); }, 8.0));
}

TEST_CASE("hardening_server unknown request types are shown like Python's repr", "[hardening][server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    CHECK(errorOf(c.request(Json::parse("{\"type\": [\"a\"]}"))) == "Unknown request type: ['a']");
    CHECK(errorOf(c.request(Json::parse("{\"type\": {\"a\": [1, \"x\", null, false]}}"))) ==
          "Unknown request type: {'a': [1, 'x', None, False]}");
    CHECK(errorOf(c.request(Json::parse("{\"type\": 1.5}"))) == "Unknown request type: 1.5");
    CHECK(errorOf(c.request(Json::parse("{\"type\": null}"))) == "Unknown request type: None");
    CHECK(errorOf(c.request(Json::parse("{\"type\": \"nope\"}"))) == "Unknown request type: 'nope'");
    CHECK(errorOf(c.request(Json::parse("{}"))) == "Unknown request type: None");
}

TEST_CASE("hardening_server serve() reports an unusable data folder like Python instead of aborting", "[hardening][server]") {
    TempDir dir;
    const std::string blocker = dir.file("a_file");
    {
        std::ofstream(blocker) << "not a folder";
    }
    ServerOptions options;
    options.dataDir = (std::filesystem::path(blocker) / "sub").string();
    options.port = 0;
    std::ostringstream captured;
    std::streambuf* saved = std::cerr.rdbuf(captured.rdbuf());
    int code = -1;
    try {
        code = serve(options);
    } catch (...) {
        std::cerr.rdbuf(saved);
        throw;
    }
    std::cerr.rdbuf(saved);
    CHECK(code == 1);
    CHECK(captured.str().rfind("Server start nahi hua (127.0.0.1:0): ", 0) == 0);
}
