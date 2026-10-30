// cpp/tests/test_server_limits.cpp -- who may log in, and how much one client may take from the others.
#include <catch2/catch_test_macros.hpp>
#include "meradb/client.h"
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "meradb/sys_compat.h"
#include "server_fixture.h"
#include <memory>
#include <string>
#include <vector>

using namespace meradb;
using namespace meradb_test;
using protocol::Json;

// ---- no username: superuser only while there are no users ----

TEST_CASE("login without a username is refused once users exist and no shared password is set", "[server][login]") {
    RunningServer s;
    {
        RawClient first(s.port());  // no users yet: the first connection is how the first user gets made
        REQUIRE(first.hello().value("ok", false));
        REQUIRE(first.query("BANAO USER ravi GUPT 'pw'")["results"][0]["error"] == "");
    }
    RawClient refused(s.port());
    Json reply = refused.hello();
    CHECK_FALSE(reply.value("ok", true));
    const std::string error = reply.value("error", std::string());
    CHECK(error.find("-U") != std::string::npos);       // says how to log in
    CHECK(error.find("--local") != std::string::npos);  // and where the way back is
    CHECK_FALSE(refused.receive().has_value());         // and hangs up
    CHECK(s.logContains("bina username login mana"));

    RawClient ravi(s.port());
    CHECK(ravi.hello("pw", nullptr, "ravi").value("ok", false));
}

TEST_CASE("a role alone does not close the no-username door", "[server][login]") {
    RunningServer s;
    s.instance().users().createRole("reader");  // a role cannot log in: nobody could get back in if this locked the door
    RawClient c(s.port());
    CHECK(c.hello().value("ok", false));
}

TEST_CASE("with a shared password the no-username session is still the superuser", "[server][login]") {
    RunningServer s("s3cret");
    s.instance().users().create("ravi", "pw");
    {
        RawClient wrong(s.port());
        CHECK_FALSE(wrong.hello("nope").value("ok", true));
    }
    RawClient admin(s.port());
    REQUIRE(admin.hello("s3cret").value("ok", false));
    CHECK(admin.query("BANAO TABLE t (x INT)")["results"][0]["error"] == "");
}

TEST_CASE("dropping the last user opens the no-username door again", "[server][login]") {
    RunningServer s;
    s.instance().users().create("ravi", "pw");
    s.instance().users().drop("ravi");
    RawClient c(s.port());
    CHECK(c.hello().value("ok", false));
}

TEST_CASE("the local path stays full access with users present", "[server][login]") {
    TempDir dir;
    Engine local(dir.str());
    local.execute("BANAO USER ravi GUPT 'pw'; BANAO TABLE t (x INT)");
    CHECK(local.runScript("DAALO MEIN t MAAN (1)")[0].error.empty());
}

TEST_CASE("the client reports the no-username refusal as a failed connection", "[server][login]") {
    RunningServer s;
    s.instance().users().create("ravi", "pw");
    ConnectOptions options;
    options.port = s.port();
    CHECK_THROWS_AS(Connection(options), ConnectionFailed);
    options.user = "ravi";
    options.password = "pw";
    CHECK_NOTHROW(Connection(options));
}

// ---- connections: one address, idle sessions, password hashing ----

TEST_CASE("one address cannot take every connection slot", "[server][connlimit]") {
    RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) { o.maxConnectionsPerAddress = 3; });
    std::vector<std::unique_ptr<RawClient>> held;
    for (int i = 0; i < 3; ++i) {
        held.push_back(std::make_unique<RawClient>(s.port()));
        REQUIRE(held.back()->hello().value("ok", false));
    }
    {
        RawClient extra(s.port());
        Json reply = extra.receive().value();
        CHECK_FALSE(reply.value("ok", true));
        CHECK(reply.value("error", std::string()).find("Is address se bahut zyada connections") != std::string::npos);
    }
    CHECK(s.logContains("is address ke connections ki limit (3)"));
    held.pop_back();  // a closed connection gives its place back
    CHECK(waitFor([&] {
        RawClient again(s.port());
        return again.hello().value("ok", false);
    }));
}

TEST_CASE("an idle session is told so and closed; a ping does not keep it open", "[server][idle]") {
    RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) { o.idleTimeoutSeconds = 0.5; });
    RawClient c(s.port());
    REQUIRE(c.hello().value("ok", false));
    for (int i = 0; i < 3; ++i) {
        sleepMs(100);
        CHECK(c.request(Json{{"type", "ping"}}).value("ok", false));
    }
    CHECK(waitFor([&] { return s.server().sessions() == 0; }, 3.0));
    Json notice = c.receive().value();
    CHECK(notice.value("error", std::string()).find("khali raha") != std::string::npos);
    CHECK_FALSE(c.receive().has_value());
    CHECK(s.logContains("idle time poora"));
}

TEST_CASE("a query keeps a session open past the idle limit", "[server][idle]") {
    RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) { o.idleTimeoutSeconds = 0.6; });
    RawClient c(s.port());
    REQUIRE(c.hello().value("ok", false));
    for (int i = 0; i < 5; ++i) {
        sleepMs(200);
        CHECK(c.query("DIKHAO TABLES")["results"][0]["error"] == "");
    }
    CHECK(s.server().sessions() == 1);
}

TEST_CASE("an idle transaction is rolled back, the lock freed and the connection closed", "[server][idle]") {
    RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) {
        o.idleTimeoutSeconds = 0;  // only the transaction limit is at work here
        o.idleTransactionTimeoutSeconds = 0.5;
    });
    RawClient setup(s.port());
    REQUIRE(setup.hello().value("ok", false));
    REQUIRE(setup.query("BANAO TABLE t (x INT); DAALO MEIN t MAAN (1)")["results"][1]["error"] == "");

    RawClient holder(s.port());
    REQUIRE(holder.hello().value("ok", false));
    Json opened = holder.query("SHURU; DAALO MEIN t MAAN (2)");
    REQUIRE(opened.value("in_transaction", false));
    for (int i = 0; i < 3; ++i) {  // pings would otherwise hold the lock for ever
        sleepMs(100);
        CHECK(holder.request(Json{{"type", "ping"}}).value("ok", false));
    }
    CHECK(waitFor([&] { return s.logContains("transaction WAPAS"); }, 3.0));
    Json notice = holder.receive().value();
    CHECK(notice.value("error", std::string()).find("transaction WAPAS") != std::string::npos);
    CHECK_FALSE(holder.receive().has_value());

    Json after = setup.query("DIKHAO * SE t");  // the lock is free and the insert is gone
    CHECK(after["results"][0]["error"] == "");
    CHECK(after["results"][0]["rows"].size() == 1);
}

TEST_CASE("the transaction limit does not touch a session outside a transaction", "[server][idle]") {
    RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) { o.idleTransactionTimeoutSeconds = 0.2; });
    RawClient c(s.port());
    REQUIRE(c.hello().value("ok", false));
    sleepMs(500);
    CHECK(c.request(Json{{"type", "ping"}}).value("ok", false));
}

TEST_CASE("a client that stops reading inside a transaction cannot hold the lock", "[server][idle]") {
    RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) { o.sendStallSeconds = 0.5; });
    RawClient setup(s.port());
    REQUIRE(setup.hello().value("ok", false));
    setup.query("BANAO TABLE big (id INT, s TEXT)");
    const std::string row = std::string(2000, 'x');
    std::string insert = "DAALO MEIN big MAAN ";
    for (int i = 0; i < 1500; ++i) insert += (i ? ", (" : "(") + std::to_string(i) + ", '" + row + "')";
    REQUIRE(setup.query(insert)["results"][0]["error"] == "");

    RawClient stalled(s.port());
    REQUIRE(stalled.hello().value("ok", false));
    Json opened = Json::object();
    opened["type"] = "query";
    opened["text"] = "SHURU";
    stalled.send(opened);
    for (int i = 0; i < 12; ++i) stalled.send(Json{{"type", "query"}, {"text", "DIKHAO * SE big"}});  // ~3 MB each, never read
    CHECK(waitFor([&] { return s.server().sessions() == 1; }, 10.0));  // dropped, and the transaction with it
    CHECK(setup.query("DAALO MEIN big MAAN (9999, 'x')")["results"][0]["error"] == "");
}

namespace {

// `count` logins at the same moment; returns how many got the "busy" answer.
int loginsAtOnce(RunningServer& s, int count) {
    std::vector<std::unique_ptr<RawClient>> clients;
    for (int i = 0; i < count; ++i) {
        clients.push_back(std::make_unique<RawClient>(s.port(), 30.0));
        Json hello = Json::object();
        hello["type"] = "hello";
        hello["user"] = "ravi";
        hello["password"] = "pw";
        clients.back()->send(hello);
    }
    int busy = 0;
    for (auto& client : clients) {
        Json reply = client->receive().value();
        if (reply.value("error", std::string()).find("busy") != std::string::npos) ++busy;
        else CHECK(reply.value("ok", false));
    }
    return busy;
}

}  // namespace

TEST_CASE("password hashing runs only so many at a time", "[server][connlimit]") {
    RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) {
        o.maxConcurrentLogins = 1;
        o.loginHashWaitSeconds = 30.0;
    });
    s.instance().users().create("ravi", "pw");
    CHECK(loginsAtOnce(s, 6) == 0);
    CHECK(s.server().peakHashingLogins() == 1);
}

TEST_CASE("a login that cannot get a hashing slot in time hears the server is busy", "[server][connlimit]") {
    RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) {
        o.maxConcurrentLogins = 1;
        o.loginHashWaitSeconds = 0.0;
    });
    s.instance().users().create("ravi", "pw");
    CHECK(loginsAtOnce(s, 6) > 0);
    CHECK(waitFor([&] {  // and being busy is not a wrong password: no lockout follows
        RawClient again(s.port());
        return again.hello("pw", nullptr, "ravi").value("ok", false);
    }, 10.0));
}

TEST_CASE("limit settings come from the environment and bad ones are refused", "[server][connlimit]") {
    struct Env {
        ~Env() {
            sys::setEnv("MERADB_IDLE_TIMEOUT", "");
            sys::setEnv("MERADB_MAX_CONNECTIONS_PER_ADDRESS", "");
        }
    } restore;
    sys::setEnv("MERADB_IDLE_TIMEOUT", "90");
    sys::setEnv("MERADB_MAX_CONNECTIONS_PER_ADDRESS", "0");
    ServerOptions options;
    CHECK(applyLimitEnvironment(options).empty());
    CHECK(options.idleTimeoutSeconds == 90.0);
    CHECK(options.maxConnectionsPerAddress == 0);
    sys::setEnv("MERADB_IDLE_TIMEOUT", "soon");
    CHECK(applyLimitEnvironment(options).find("MERADB_IDLE_TIMEOUT") != std::string::npos);
    sys::setEnv("MERADB_IDLE_TIMEOUT", "-5");
    CHECK_FALSE(applyLimitEnvironment(options).empty());
}
