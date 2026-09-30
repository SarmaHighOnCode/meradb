// cpp/tests/test_server.cpp -- the server, spoken to with the raw protocol (no client library yet).
#include <catch2/catch_test_macros.hpp>
#include "server_fixture.h"

using namespace meradb;
using namespace meradb_test;
using protocol::Json;

namespace {

std::string dumped(const Json& j) { return pyjson::dump(j); }

bool isClosed(RawClient& c) {
    try {
        return !c.receive().has_value();
    } catch (const net::NetError&) {
        return true;
    }
}

}  // namespace

TEST_CASE("server hello is answered with the version banner", "[server]") {
    RunningServer s;
    RawClient c(s.port());
    CHECK(dumped(c.hello()) == "{\"ok\": true, \"server\": \"MeraDB 1.0.0\", \"protocol\": 1, \"database\": \"main\"}");
}

TEST_CASE("server ping and unknown request types", "[server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    CHECK(dumped(c.request(Json{{"type", "ping"}})) == "{\"ok\": true}");
    CHECK(dumped(c.request(Json{{"type", "foo"}})) == "{\"ok\": false, \"error\": \"Unknown request type: 'foo'\"}");
    CHECK(dumped(c.request(Json::object())) == "{\"ok\": false, \"error\": \"Unknown request type: None\"}");
    CHECK(dumped(c.request(Json{{"type", 5}})) == "{\"ok\": false, \"error\": \"Unknown request type: 5\"}");
}

TEST_CASE("server refuses a wrong shared password and then closes", "[server]") {
    RunningServer s("s3cret");
    {
        RawClient c(s.port());
        CHECK(dumped(c.hello("nope")) == "{\"ok\": false, \"error\": \"Password galat hai\"}");
        CHECK(isClosed(c));
    }
    {
        RawClient c(s.port());
        CHECK(dumped(c.hello(nullptr)) == "{\"ok\": false, \"error\": \"Password galat hai\"}");  // no password at all
    }
    RawClient good(s.port());
    CHECK(good.hello("s3cret")["ok"] == true);
    CHECK(waitFor([&] { return s.logContains("login fail (galat password)"); }));
}

TEST_CASE("server per-user login supersedes the shared password", "[server]") {
    RunningServer s("s3cret");
    s.instance().users().create("ravi", "pw1");
    {
        RawClient c(s.port());
        CHECK(dumped(c.hello("pw1", nullptr, "ravi")) ==
              "{\"ok\": true, \"server\": \"MeraDB 1.0.0\", \"protocol\": 1, \"database\": \"main\"}");
    }
    {
        RawClient c(s.port());  // the SHARED password does not open a user login
        CHECK(dumped(c.hello("s3cret", nullptr, "ravi")) == "{\"ok\": false, \"error\": \"User ya password galat hai\"}");
        CHECK(isClosed(c));
    }
    {
        RawClient c(s.port());
        CHECK(dumped(c.hello("pw1", nullptr, "ghost")) == "{\"ok\": false, \"error\": \"User ya password galat hai\"}");
    }
    CHECK(waitFor([&] { return s.logContains("login fail (galat user/password: 'ghost')"); }));
}

TEST_CASE("server hello can pick the database", "[server]") {
    RunningServer s;
    {
        RawClient admin(s.port());
        admin.hello();
        admin.query("BANAO DATABASE college");
    }
    RawClient c(s.port());
    CHECK(c.hello(nullptr, "college")["database"] == "college");
    RawClient missing(s.port());
    CHECK(dumped(missing.hello(nullptr, "nope")) == "{\"ok\": false, \"error\": \"Database 'nope' exist nahi karta\"}");
    CHECK(isClosed(missing));
}

TEST_CASE("server refuses a restricted user who asks for a database at hello", "[server]") {
    RunningServer s;
    s.instance().users().create("ravi", "pw1");
    RawClient c(s.port());
    Json reply = c.hello("pw1", "main", "ravi");  // ISTEMAL is superuser-only, exactly like Python
    CHECK(reply["ok"] == false);
    CHECK(reply["error"].get<std::string>().find("superuser nahi hai") != std::string::npos);
}

TEST_CASE("server closes a connection whose first message is not a hello", "[server]") {
    RunningServer s;
    {
        RawClient c(s.port());
        c.send(Json{{"type", "ping"}});
        CHECK(isClosed(c));
    }
    {
        RawClient c(s.port());
        c.sendRaw("garbage\n");
        CHECK(isClosed(c));
    }
    {
        RawClient c(s.port());
        c.socket().shutdownBoth();  // connect and say nothing at all
    }
    RawClient fine(s.port());
    CHECK(fine.hello()["ok"] == true);  // the server is unharmed
}

TEST_CASE("server runs a script and returns Python-shaped results", "[server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    Json reply = c.query("BANAO TABLE t (i INT, f FLOAT, s TEXT, b BOOL); "
                         "DAALO MEIN t MAAN (1, 2.5, 'Ravi''s', SACH), (KHALI, 7.0, KHALI, JHOOTH); DIKHAO * SE t");
    CHECK(dumped(reply) ==
          "{\"ok\": true, \"results\": ["
          "{\"columns\": [], \"rows\": [], \"message\": \"Table 't' ban gaya (4 columns)\", \"error\": \"\"}, "
          "{\"columns\": [], \"rows\": [], \"message\": \"2 row(s) daal di\", \"error\": \"\"}, "
          "{\"columns\": [\"i\", \"f\", \"s\", \"b\"], \"rows\": [[1, 2.5, \"Ravi's\", true], [null, 7.0, null, false]], "
          "\"message\": \"2 row(s)\", \"error\": \"\"}], \"database\": \"main\", \"in_transaction\": false}");
}

TEST_CASE("server reports errors per statement and keeps going", "[server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    Json reply = c.query("DIKHAO * SE nahi_hai; DIKHAO TABLES;");
    REQUIRE(reply["results"].size() == 2);
    CHECK(reply["results"][0]["error"].get<std::string>().find("exist nahi karta") != std::string::npos);
    CHECK(reply["results"][1]["error"] == "");
    Json parse = c.query("DIKHAO SE");
    CHECK(parse["results"][0]["error"].get<std::string>().find("[Parser Galti]") == 0);
}

TEST_CASE("server tracks the session's database and transaction in every reply", "[server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    Json reply = c.query("BANAO DATABASE college; ISTEMAL college; SHURU");
    CHECK(reply["database"] == "college");
    CHECK(reply["in_transaction"] == true);
    reply = c.query("WAPAS");
    CHECK(reply["in_transaction"] == false);
}

TEST_CASE("server survives bad lines and keeps the connection open", "[server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    c.sendRaw("this is not json\n");
    Json bad = c.receive().value();
    CHECK(bad["ok"] == false);
    CHECK(bad["error"].get<std::string>().rfind("[Protocol Galti] Galat message: ", 0) == 0);
    c.sendRaw("[1, 2, 3]\n");
    CHECK(dumped(c.receive().value()) ==
          "{\"ok\": false, \"error\": \"[Protocol Galti] Message ek JSON object hona chahiye\"}");
    CHECK(dumped(c.request(Json{{"type", "ping"}})) == "{\"ok\": true}");
}

TEST_CASE("server status reports pid folder start time sessions and databases", "[server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    Json st = c.request(Json{{"type", "status"}});
    CHECK(st["ok"] == true);
    CHECK(st["server"] == "MeraDB 1.0.0");
    CHECK(st["pid"].get<int>() > 0);
    CHECK(st["data_dir"] == s.dataDir());
    CHECK(st["started"].get<std::string>().size() == 19);
    CHECK(st["sessions"] == 1);
    CHECK(dumped(st["databases"]) == "[\"main\"]");
    std::vector<std::string> keys;
    for (auto it = st.begin(); it != st.end(); ++it) keys.push_back(it.key());
    CHECK(keys == std::vector<std::string>{"ok", "server", "pid", "data_dir", "started", "sessions", "databases"});
}

TEST_CASE("server schema request returns the tree", "[server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    c.query("BANAO TABLE t (id INT MUKHYA KUNJI)");
    Json reply = c.request(Json{{"type", "schema"}});
    CHECK(reply["ok"] == true);
    CHECK(reply["tree"][0]["name"] == "main");
    CHECK(reply["tree"][0]["tables"][0]["columns"][0]["primary_key"] == true);
}

TEST_CASE("server counts sessions and logs connects and disconnects", "[server]") {
    RunningServer s;
    {
        RawClient a(s.port());
        a.hello();
        RawClient b(s.port());
        b.hello();
        CHECK(waitFor([&] { return s.server().sessions() == 2; }));
        CHECK(s.logContains("connected  (active sessions: "));
    }
    CHECK(waitFor([&] { return s.server().sessions() == 0; }));
    CHECK(s.logContains("disconnected  (active sessions: 0)"));
    for (const auto& line : s.logLines()) CHECK(line.size() > 21);  // "YYYY-MM-DD HH:MM:SS  " prefix
}

TEST_CASE("server logs queries only in verbose mode", "[server]") {
    RunningServer quiet;
    RunningServer loud("", /*verbose=*/true);
    for (RunningServer* s : {&quiet, &loud}) {
        RawClient c(s->port());
        c.hello();
        c.query("DIKHAO   TABLES");
        c.query("DIKHAO * SE gayab");
    }
    CHECK(loud.logContains("[main]  DIKHAO TABLES"));  // whitespace collapsed like Python
    CHECK_FALSE(quiet.logContains("[main]  DIKHAO TABLES"));
    CHECK(quiet.logContains("Table 'gayab' exist nahi karta"));  // errors are always logged
}

TEST_CASE("server accepts a shutdown request from this machine and stops", "[server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    CHECK(dumped(c.request(Json{{"type", "shutdown"}})) == "{\"ok\": true}");
    CHECK(waitFor([&] { return s.stopped(); }));
    CHECK(s.logContains("shutdown requested"));
    CHECK_THROWS_AS(RawClient(s.port()), net::NetError);  // the port is closed
}

TEST_CASE("server stops promptly even with idle clients connected", "[server]") {
    auto s = std::make_unique<RunningServer>();
    RawClient idle(s->port());
    idle.hello();
    RawClient silent(s->port());  // never even says hello
    auto started = std::chrono::steady_clock::now();
    s->stop();
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(took < 3.0);
    CHECK(isClosed(idle));
    CHECK(isClosed(silent));
}
