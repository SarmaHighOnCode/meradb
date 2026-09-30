// cpp/tests/test_client_server.cpp
//
// Client <-> server over real loopback sockets. A port of the scenarios in
// tests/test_server.py and the "users over the wire" class of tests/test_phase_b.py,
// plus the concurrency checks the thread-per-connection design promises.
#include <catch2/catch_test_macros.hpp>
#include "meradb/client.h"
#include "meradb/errors.h"
#include "server_fixture.h"
#include <atomic>
#include <thread>
#include <vector>

using namespace meradb;
using namespace meradb_test;

namespace {

ConnectOptions to(const RunningServer& s) {
    ConnectOptions options;
    options.port = s.port();
    return options;
}

std::vector<std::vector<Value>> rowsOf(Backend& db, const std::string& text) { return db.execute(text)[0].rows; }

}  // namespace

TEST_CASE("client_server sessions have their own current database", "[client_server]") {
    RunningServer s;
    Connection a(to(s)), b(to(s));
    a.execute("BANAO DATABASE college; ISTEMAL college; BANAO TABLE t (x INT)");
    CHECK(a.currentDb() == "college");
    CHECK(b.currentDb() == "main");
    CHECK(rowsOf(b, "DIKHAO TABLES").empty());
    ConnectOptions options = to(s);
    options.database = "college";
    Connection c(options);
    auto tables = rowsOf(c, "DIKHAO TABLES");
    REQUIRE(tables.size() == 1);
    CHECK(std::get<std::string>(tables[0][0].data) == "t");
}

TEST_CASE("client_server writes are visible to other clients and constraints are shared", "[client_server]") {
    RunningServer s;
    Connection a(to(s)), b(to(s));
    a.execute("BANAO TABLE t (id INT MUKHYA KUNJI); DAALO MEIN t MAAN (1)");
    auto rows = rowsOf(b, "DIKHAO * SE t JAHAN id = 1");
    REQUIRE(rows.size() == 1);
    CHECK(std::get<int64_t>(rows[0][0].data) == 1);
    CHECK_THROWS_AS(b.execute("DAALO MEIN t MAAN (1)"), MeraDBError);  // uniqueness is shared too
}

TEST_CASE("client_server a transaction spans several requests on one session", "[client_server]") {
    RunningServer s;
    Connection a(to(s)), b(to(s));
    a.execute("BANAO TABLE t (x INT)");
    a.execute("SHURU");
    a.execute("DAALO MEIN t MAAN (1)");  // separate requests, same session, same server thread
    CHECK(a.inTransaction());
    a.execute("PAKKA");
    CHECK_FALSE(a.inTransaction());
    CHECK(rowsOf(b, "DIKHAO * SE t").size() == 1);
}

TEST_CASE("client_server transactions are isolated between clients", "[client_server]") {
    RunningServer s;
    Connection a(to(s)), b(to(s));
    a.execute("BANAO TABLE t (x INT)");
    a.execute("SHURU; DAALO MEIN t MAAN (1)");
    CHECK(a.inTransaction());
    CHECK(b.runScript("DIKHAO * SE t")[0].error.find("busy") != std::string::npos);  // b must wait for a
    a.execute("WAPAS");
    CHECK(rowsOf(b, "DIKHAO * SE t").empty());
}

TEST_CASE("client_server a waiting client proceeds when the transaction ends", "[client_server]") {
    RunningServer s;
    s.instance().lockTimeoutSeconds = 5.0;  // long enough to outwait the sleep below
    Connection a(to(s)), b(to(s));
    a.execute("BANAO TABLE t (x INT); SHURU; DAALO MEIN t MAAN (1)");
    std::atomic<bool> done{false};
    std::string error = "unset";
    std::size_t rowCount = 0;
    std::thread waiter([&] {
        auto r = b.runScript("DIKHAO * SE t");
        error = r[0].error;
        rowCount = r[0].rows.size();
        done = true;
    });
    sleepMs(250);
    CHECK_FALSE(done.load());  // still waiting for a's lock
    a.execute("PAKKA");
    waiter.join();
    CHECK(error.empty());
    CHECK(rowCount == 1);  // b sees the committed row
}

TEST_CASE("client_server a disconnect rolls the open transaction back and frees the lock", "[client_server]") {
    RunningServer s;
    {
        Connection a(to(s));
        a.execute("BANAO TABLE t (x INT); DAALO MEIN t MAAN (1); SHURU; MITAO SE t");
        a.close();
    }
    Connection b(to(s));
    auto started = std::chrono::steady_clock::now();
    auto rows = rowsOf(b, "DIKHAO * SE t");  // would report "busy" after 0.5 s if the lock were still held
    double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(rows.size() == 1);
    CHECK(waited < 0.4);
    CHECK(s.logContains("disconnected"));
}

TEST_CASE("client_server status counts live sessions", "[client_server]") {
    RunningServer s;
    Connection a(to(s));
    {
        Connection b(to(s));
        CHECK(a.status()["sessions"] == 2);
    }
    CHECK(waitFor([&] { return a.status()["sessions"] == 1; }));
    CHECK(a.status()["databases"][0] == "main");
}

TEST_CASE("client_server many short connections leave no sessions behind", "[client_server]") {
    RunningServer s;
    for (int i = 0; i < 60; ++i) {
        Connection c(to(s));
        c.execute("DIKHAO TABLES");
    }
    CHECK(waitFor([&] { return s.server().sessions() == 0; }));
}

TEST_CASE("client_server users over the wire get exactly what they were granted", "[client_server]") {
    RunningServer s;
    {
        Connection admin(to(s));
        admin.execute("BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT)");
        admin.execute("DAALO MEIN students MAAN (1, 'Ravi')");
    }
    // The same as `BANAO USER ravi GUPT 'secret123'; ADHIKAR DO DIKHAO PAR students KO ravi`.
    s.instance().users().create("ravi", "secret123");
    s.instance().users().grant("ravi", "main", "students", {"DIKHAO"});

    ConnectOptions options = to(s);
    options.user = "ravi";
    options.password = "wrong";
    CHECK_THROWS_AS(Connection(options), ConnectionFailed);
    options.user = "ghost";
    options.password = "whatever";
    CHECK_THROWS_AS(Connection(options), ConnectionFailed);

    options.user = "ravi";
    options.password = "secret123";
    Connection ravi(options);
    auto rows = rowsOf(ravi, "DIKHAO * SE students");
    REQUIRE(rows.size() == 1);
    CHECK(std::get<std::string>(rows[0][1].data) == "Ravi");

    auto denied = ravi.runScript("DAALO MEIN students MAAN (2, 'Simran')");
    CHECK(denied[0].error.find("adhikar nahi hai") != std::string::npos);
    auto ddl = ravi.runScript("BANAO TABLE t2 (a INT)");
    CHECK(ddl[0].error.find("superuser nahi hai") != std::string::npos);

    Connection nobody(to(s));  // no user= at all: exactly the unrestricted behaviour
    nobody.execute("DAALO MEIN students MAAN (3, 'Anjali')");
    CHECK(rowsOf(nobody, "DIKHAO * SE students").size() == 2);
}

TEST_CASE("client_server concurrent clients do not lose or corrupt writes", "[client_server]") {
    RunningServer s;
    s.instance().lockTimeoutSeconds = 10.0;
    {
        Connection admin(to(s));
        admin.execute("BANAO TABLE t (id INT MUKHYA KUNJI, who INT)");
    }
    constexpr int kThreads = 8;
    constexpr int kPerThread = 25;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            try {
                Connection c(to(s));
                for (int i = 0; i < kPerThread; ++i) {
                    auto r = c.runScript("DAALO MEIN t MAAN (" + std::to_string(t * 1000 + i) + ", " + std::to_string(t) + ")");
                    if (!r[0].error.empty()) ++failures;
                }
            } catch (const std::exception&) {
                ++failures;
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(failures.load() == 0);
    Connection check(to(s));
    auto count = rowsOf(check, "DIKHAO GINO(*) SE t");
    CHECK(std::get<int64_t>(count[0][0].data) == kThreads * kPerThread);
    // the primary key index survived the storm: every id is findable
    CHECK(rowsOf(check, "DIKHAO * SE t JAHAN id = 7024").size() == 1);
    CHECK_THROWS_AS(check.execute("DAALO MEIN t MAAN (7024, 7)"), MeraDBError);
}

TEST_CASE("client_server each session keeps its own transaction while others work", "[client_server]") {
    RunningServer s;
    s.instance().lockTimeoutSeconds = 5.0;
    Connection setup(to(s));
    setup.execute("BANAO DATABASE one; BANAO DATABASE two");
    // two sessions, each transacting in a DIFFERENT database, one after the other on the same lock
    ConnectOptions optionsOne = to(s);
    optionsOne.database = "one";
    Connection a(optionsOne);
    a.execute("BANAO TABLE t (x INT); SHURU; DAALO MEIN t MAAN (1); PAKKA");
    ConnectOptions optionsTwo = to(s);
    optionsTwo.database = "two";
    Connection b(optionsTwo);
    b.execute("BANAO TABLE t (x INT); SHURU; DAALO MEIN t MAAN (2); WAPAS");
    CHECK(rowsOf(a, "DIKHAO * SE t").size() == 1);
    CHECK(rowsOf(b, "DIKHAO * SE t").empty());
}
