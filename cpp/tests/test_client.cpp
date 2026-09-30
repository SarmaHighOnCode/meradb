// cpp/tests/test_client.cpp -- Backend, LocalBackend and the client Connection.
#include <catch2/catch_test_macros.hpp>
#include "meradb/client.h"
#include "meradb/errors.h"
#include "server_fixture.h"

using namespace meradb;
using namespace meradb_test;

namespace {

ConnectOptions to(const RunningServer& s) {
    ConnectOptions options;
    options.port = s.port();
    return options;
}

}  // namespace

// ---- LocalBackend ----

TEST_CASE("client LocalBackend wraps an embedded engine", "[client]") {
    TempDir dir;
    LocalBackend b(dir.str());
    CHECK(b.description() == "local (" + b.engine().instance().dataDir() + ")");
    CHECK(b.currentDb() == "main");
    b.execute("BANAO TABLE t (x INT); SHURU");
    CHECK(b.inTransaction());
    CHECK(b.runScript("DIKHAO * SE nahi_hai")[0].error.find("exist nahi karta") != std::string::npos);
    CHECK_THROWS_AS(b.execute("DIKHAO SE"), ParseError);  // the real exception, like Engine.execute
    CHECK(b.schemaTree()[0]["tables"][0]["name"] == "t");
    b.close();  // rolls the transaction back
    CHECK_FALSE(b.inTransaction());
}

// ---- Connection ----

TEST_CASE("client Connection round-trips types and dates", "[client]") {
    RunningServer s;
    Connection db(to(s));
    CHECK(db.serverVersion() == "MeraDB 1.0.0");
    CHECK(db.description() == "127.0.0.1:" + std::to_string(s.port()));
    CHECK(db.currentDb() == "main");
    db.execute("BANAO TABLE t (i INT, f FLOAT, s TEXT, b BOOL, d DATE); "
               "DAALO MEIN t MAAN (1, 2.5, 'Ravi''s', SACH, '2005-06-15'), (KHALI, 7.0, KHALI, JHOOTH, KHALI)");
    auto result = db.execute("DIKHAO * SE t KRAM i")[0];
    REQUIRE(result.rows.size() == 2);
    CHECK(result.columns == std::vector<std::string>{"i", "f", "s", "b", "d"});
    const auto& null_row = result.rows[0];  // KHALI sorts first
    CHECK(null_row[0].isNull());
    CHECK(std::holds_alternative<double>(null_row[1].data));  // 7.0 stays a float
    CHECK(std::get<double>(null_row[1].data) == 7.0);
    CHECK(std::get<bool>(null_row[3].data) == false);
    const auto& full_row = result.rows[1];
    CHECK(std::get<int64_t>(full_row[0].data) == 1);
    CHECK(std::get<std::string>(full_row[2].data) == "Ravi's");
    CHECK(std::get<Date>(full_row[4].data) == parseDate("2005-06-15"));
}

TEST_CASE("client Connection runScript reports errors per statement", "[client]") {
    RunningServer s;
    Connection db(to(s));
    auto results = db.runScript("DIKHAO * SE nahi_hai; DIKHAO TABLES;");
    REQUIRE(results.size() == 2);
    CHECK(results[0].error.find("exist nahi karta") != std::string::npos);
    CHECK(results[1].error.empty());
    CHECK(db.runScript("DIKHAO SE")[0].error.find("Parser") != std::string::npos);
}

TEST_CASE("client Connection execute throws the first error wrapped once more", "[client]") {
    RunningServer s;
    Connection db(to(s));
    try {
        db.execute("DIKHAO * SE nahi_hai");
        FAIL("expected an error");
    } catch (const MeraDBError& e) {
        // the server's text already has its own tag; Connection.execute adds the plain one, like Python
        CHECK(e.message() == "[Execution Galti] Table 'nahi_hai' exist nahi karta");
        CHECK(std::string(e.what()) == "[MeraDB Galti] [Execution Galti] Table 'nahi_hai' exist nahi karta");
    }
}

TEST_CASE("client Connection tracks database and transaction state", "[client]") {
    RunningServer s;
    Connection db(to(s));
    db.execute("BANAO DATABASE college; ISTEMAL college");
    CHECK(db.currentDb() == "college");
    db.execute("SHURU");
    CHECK(db.inTransaction());
    db.execute("WAPAS");
    CHECK_FALSE(db.inTransaction());
}

TEST_CASE("client Connection can start in a database", "[client]") {
    RunningServer s;
    {
        Connection admin(to(s));
        admin.execute("BANAO DATABASE college");
    }
    ConnectOptions options = to(s);
    options.database = "college";
    Connection db(options);
    CHECK(db.currentDb() == "college");
    options.database = "nope";
    try {
        Connection bad(options);
        FAIL("expected a refusal");
    } catch (const ConnectionFailed& e) {
        CHECK(std::string(e.what()) == "[Connection Galti] Database 'nope' exist nahi karta");
    }
}

TEST_CASE("client Connection reports a missing server as ServerUnavailable", "[client]") {
    int port;
    {
        net::Socket listener = net::listenOn("127.0.0.1", 0);
        port = listener.localPort();
    }
    ConnectOptions options;
    options.port = port;
    try {
        Connection db(options);
        FAIL("expected ServerUnavailable");
    } catch (const ServerUnavailable& e) {
        std::string text = e.message();
        CHECK(text.rfind("127.0.0.1:" + std::to_string(port) + " par MeraDB server nahi mila (", 0) == 0);
        CHECK(text.back() == ')');
    }
}

TEST_CASE("client Connection wrong password is ConnectionFailed not ServerUnavailable", "[client]") {
    RunningServer s("s3cret");
    ConnectOptions options = to(s);
    options.password = "nope";
    try {
        Connection db(options);
        FAIL("expected a refusal");
    } catch (const ServerUnavailable&) {
        FAIL("must not be ServerUnavailable");
    } catch (const ConnectionFailed& e) {
        CHECK(std::string(e.what()) == "[Connection Galti] Password galat hai");
    }
    options.password = "s3cret";
    Connection ok(options);
    CHECK(ok.execute("DIKHAO TABLES")[0].rows.empty());
    ConnectOptions none = to(s);  // no password at all
    CHECK_THROWS_AS(Connection(none), ConnectionFailed);
}

TEST_CASE("client Connection logs in as a user", "[client]") {
    RunningServer s;
    s.instance().users().create("ravi", "secret123");
    ConnectOptions options = to(s);
    options.user = "ravi";
    options.password = "wrong";
    try {
        Connection db(options);
        FAIL("expected a refusal");
    } catch (const ConnectionFailed& e) {
        CHECK(std::string(e.what()) == "[Connection Galti] User ya password galat hai");
    }
    options.password = "secret123";
    Connection db(options);
    CHECK(db.runScript("BANAO TABLE t2 (a INT)")[0].error.find("superuser nahi hai") != std::string::npos);
}

TEST_CASE("client Connection schemaTree and status", "[client]") {
    RunningServer s;
    Connection db(to(s));
    db.execute("BANAO TABLE t (id INT MUKHYA KUNJI)");
    auto tree = db.schemaTree();
    CHECK(tree[0]["name"] == "main");
    CHECK(tree[0]["tables"][0]["columns"][0]["primary_key"] == true);
    auto st = db.status();
    CHECK(st["ok"] == true);
    CHECK(st["sessions"] == 1);
}

TEST_CASE("client Connection fails cleanly after close or when the server goes away", "[client]") {
    auto s = std::make_unique<RunningServer>();
    Connection db(to(*s));
    Connection closed(to(*s));
    closed.close();
    CHECK_THROWS_AS(closed.runScript("DIKHAO TABLES"), ConnectionFailed);

    s->stop();  // the server hangs up on live connections
    try {
        db.runScript("DIKHAO TABLES");
        FAIL("expected ConnectionFailed");
    } catch (const ConnectionFailed& e) {
        // a clean hang-up, or a reset while writing -- both are Python's two ConnectionFailed texts
        const std::string text = e.message();
        CHECK((text.rfind("Server ne connection band kar diya", 0) == 0 ||
               text.rfind("Server se connection toot gaya: ", 0) == 0));
    }
}

TEST_CASE("client Connection times out on a server that never answers", "[client]") {
    net::Socket mute = net::listenOn("127.0.0.1", 0);  // accepts connections at the OS level, says nothing
    ConnectOptions options;
    options.port = mute.localPort();
    options.timeoutSeconds = 0.3;
    try {
        Connection db(options);
        FAIL("expected a timeout");
    } catch (const ConnectionFailed& e) {
        CHECK(e.message() == "Server se connection toot gaya: timed out");
    }
}

TEST_CASE("client Connection shutdown stops the server", "[client]") {
    RunningServer s;
    Connection db(to(s));
    db.shutdown();
    CHECK(waitFor([&] { return s.stopped(); }));
}

TEST_CASE("client Local and remote backends give the same answers through Backend&", "[client]") {
    RunningServer s;
    TempDir dir;
    LocalBackend local(dir.str());
    Connection remote(to(s));
    const std::string script =
        "BANAO TABLE t (i INT, s TEXT); DAALO MEIN t MAAN (1, 'a'), (2, KHALI); DIKHAO * SE t KRAM i; DIKHAO * SE nahi_hai";
    for (Backend* db : {static_cast<Backend*>(&local), static_cast<Backend*>(&remote)}) {
        auto results = db->runScript(script);
        REQUIRE(results.size() == 4);
        CHECK(results[2].rows.size() == 2);
        CHECK(results[2].columns == std::vector<std::string>{"i", "s"});
        CHECK(results[3].error == "[Execution Galti] Table 'nahi_hai' exist nahi karta");
        CHECK(db->currentDb() == "main");
        CHECK_FALSE(db->inTransaction());
    }
}
