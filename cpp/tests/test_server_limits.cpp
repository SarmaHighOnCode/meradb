// cpp/tests/test_server_limits.cpp -- who may log in, and how much one client may take from the others.
#include <catch2/catch_test_macros.hpp>
#include "meradb/client.h"
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "meradb/query_limits.h"
#include "meradb/sys_compat.h"
#include "server_fixture.h"
#include <chrono>
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

// ---- what one query may use ----

namespace {

// A table of n rows, for joins that grow quickly.
void makeNumbers(RawClient& c, const std::string& table, int n) {
    REQUIRE(c.query("BANAO TABLE " + table + " (x INT)")["results"][0]["error"] == "");
    std::string insert = "DAALO MEIN " + table + " MAAN ";
    for (int i = 0; i < n; ++i) insert += (i ? ", (" : "(") + std::to_string(i) + ")";
    REQUIRE(c.query(insert)["results"][0]["error"] == "");
}

std::string firstError(const Json& reply) { return reply["results"][0]["error"].get<std::string>(); }

}  // namespace

TEST_CASE("a cross join runs into the memory limit, and the session and the others go on", "[server][qlimit]") {
    RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) { o.maxQueryMemoryBytes = 5 * 1024 * 1024; });
    RawClient c(s.port());
    REQUIRE(c.hello().value("ok", false));
    makeNumbers(c, "a", 400);
    const std::string error = firstError(c.query("DIKHAO * SE a p MILAO a q PAR 1 = 1 MILAO a r PAR 1 = 1"));
    CHECK(error.find("[Execution Galti] Query bahut zyada memory") != std::string::npos);
    CHECK(error.find("5 MB") != std::string::npos);
    CHECK(firstError(c.query("DIKHAO * SE a SIRF 1")) == "");  // nothing stays charged to the next request
    RawClient other(s.port());
    REQUIRE(other.hello().value("ok", false));
    CHECK(firstError(other.query("DIKHAO * SE a SIRF 1")) == "");
}

TEST_CASE("a correlated sub-select is not charged once per row", "[server][qlimit]") {
    RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) { o.maxQueryMemoryBytes = 8 * 1024 * 1024; });
    RawClient c(s.port());
    REQUIRE(c.hello().value("ok", false));
    makeNumbers(c, "a", 1000);
    // 1000 scans of 1000 rows: far over the limit if added up, and well under it at any one moment
    Json reply = c.query("DIKHAO COUNT(*) SE a p JAHAN p.x = (DIKHAO MAX(q.x) SE a q JAHAN q.x = p.x)");
    CHECK(firstError(reply) == "");
    CHECK(reply["results"][0]["rows"][0][0] == 1000);
}

TEST_CASE("a query past its time limit is stopped and the lock is free again", "[server][qlimit]") {
    RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) {
        o.maxQuerySeconds = 0.3;
        o.maxQueryMemoryBytes = 0;
    });
    RawClient c(s.port());
    REQUIRE(c.hello().value("ok", false));
    makeNumbers(c, "a", 300);
    auto started = std::chrono::steady_clock::now();
    const std::string error = firstError(c.query("DIKHAO COUNT(*) SE a p MILAO a q PAR p.x + q.x >= 0 MILAO a r PAR q.x + r.x >= 0"));
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(error.find("Query ka time khatam") != std::string::npos);
    CHECK(took < 10.0);  // a real run of these 27 million rows takes far longer
    CHECK(firstError(c.query("DIKHAO * SE a SIRF 1")) == "");  // the next request has its own time
}

TEST_CASE("a stored procedure that never ends is stopped by the time limit", "[server][qlimit]") {
    RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) { o.maxQuerySeconds = 0.3; });
    RawClient c(s.port());
    REQUIRE(c.hello().value("ok", false));
    REQUIRE(firstError(c.query("BANAO PROCEDURE ghoomo () SHURU DECLARE n INT DEFAULT 0; WHILE 1 = 1 DO SET n = n + 1; END WHILE; KHATAM")) == "");
    CHECK(firstError(c.query("CHALAO ghoomo()")).find("Query ka time khatam") != std::string::npos);
}

TEST_CASE("repeated CONCAT doubling stops at the value limit", "[server][qlimit]") {
    RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) { o.maxValueBytes = 1024 * 1024; });
    RawClient c(s.port());
    REQUIRE(c.hello().value("ok", false));
    std::string script = "SET @a = 'xxxxxxxxxxxxxxxx';";
    for (int i = 0; i < 40; ++i) script += "SET @a = CONCAT(@a, @a);";
    Json reply = c.query(script);
    const auto& results = reply["results"];
    CHECK(results[10]["error"] == "");  // 16 KB is fine
    CHECK(results[20]["error"].get<std::string>().find("Text value bahut badi") != std::string::npos);  // 16 MB is not
    CHECK(c.query("SET @b = 'still working'")["results"][0]["error"] == "");
}

TEST_CASE("REPLACE cannot multiply a text past the value limit", "[server][qlimit]") {
    RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) { o.maxValueBytes = 1024 * 1024; });
    RawClient c(s.port());
    REQUIRE(c.hello().value("ok", false));
    const std::string big(100000, 'a');
    CHECK(firstError(c.query("DIKHAO REPLACE('" + big + "', 'a', '" + big + "')")).find("Text value bahut badi") != std::string::npos);
}

TEST_CASE("@variables cannot add up without end", "[server][qlimit]") {
    RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) { o.maxValueBytes = 1024 * 1024; });
    RawClient c(s.port());
    REQUIRE(c.hello().value("ok", false));
    const std::string value(1000 * 1000, 'v');
    std::string last;
    for (int i = 0; i < 6 && last.empty(); ++i) last = firstError(c.query("SET @v" + std::to_string(i) + " = '" + value + "'"));
    CHECK(last.find("@variables mein bahut zyada") != std::string::npos);
}

TEST_CASE("a result with too many rows is an error, one just under the limit is not", "[server][qlimit]") {
    RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) { o.maxResultRows = 10; });
    RawClient c(s.port());
    REQUIRE(c.hello().value("ok", false));
    makeNumbers(c, "a", 11);
    CHECK(firstError(c.query("DIKHAO * SE a")).find("bahut zyada rows") != std::string::npos);
    CHECK(firstError(c.query("DIKHAO * SE a SIRF 10")) == "");
}

TEST_CASE("a reply too big for the clients is an error, not a broken connection", "[server][qlimit]") {
    RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) { o.maxReplyBytes = 1024 * 1024; });
    ConnectOptions options;
    options.port = s.port();
    Connection db(options);
    db.execute("BANAO TABLE t (s TEXT); SET @big = 'x'");
    const std::string text(200000, 'x');
    for (int i = 0; i < 8; ++i) db.execute("DAALO MEIN t MAAN ('" + text + "')");
    CHECK(db.runScript("DIKHAO * SE t")[0].error.find("Result bahut bada hai") != std::string::npos);
    // Text that grows when it is written as JSON gets past the first estimate and is caught at the end.
    db.execute("MITAO SE t");
    for (int i = 0; i < 2; ++i) db.execute("DAALO MEIN t MAAN ('" + std::string(100000, '\t') + "')");
    CHECK(db.runScript("DIKHAO * SE t")[0].error == "");
    db.execute("DAALO MEIN t MAAN ('" + std::string(600000, '\t') + "')");
    CHECK(db.runScript("DIKHAO * SE t")[0].error.find("Result bahut bada hai") != std::string::npos);
    CHECK(db.runScript("DIKHAO COUNT(*) SE t")[0].error == "");
}

TEST_CASE("after a limit the open transaction is still there to finish", "[server][qlimit]") {
    RunningServer s("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) { o.maxQueryMemoryBytes = 2 * 1024 * 1024; });
    RawClient c(s.port());
    REQUIRE(c.hello().value("ok", false));
    makeNumbers(c, "a", 300);
    Json reply = c.query("SHURU; DAALO MEIN a MAAN (-1); DIKHAO * SE a p MILAO a q PAR 1 = 1");
    CHECK(reply["results"][2]["error"].get<std::string>().find("memory") != std::string::npos);
    CHECK(reply.value("in_transaction", false));
    CHECK(c.query("WAPAS").value("in_transaction", true) == false);
    CHECK(c.query("DIKHAO COUNT(*) SE a")["results"][0]["rows"][0][0] == 300);
}

TEST_CASE("a long query does not keep the server from stopping", "[server][qlimit]") {
    auto s = std::make_unique<RunningServer>("", false, protocol::kMaxMessageBytes, [](ServerOptions& o) {
        o.maxQuerySeconds = 0;
        o.maxQueryMemoryBytes = 0;
    });
    RawClient c(s->port());
    REQUIRE(c.hello().value("ok", false));
    makeNumbers(c, "a", 300);
    Json long_query = Json::object();
    long_query["type"] = "query";
    long_query["text"] = "DIKHAO COUNT(*) SE a p MILAO a q PAR p.x + q.x >= 0 MILAO a r PAR q.x + r.x >= 0";
    c.send(long_query);
    sleepMs(200);
    auto started = std::chrono::steady_clock::now();
    s->stop();
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() < 5.0);
}

TEST_CASE("an embedded engine has no query limits, even in a process that serves", "[server][qlimit]") {
    {
        limits::Limits small;
        small.valueBytes = 16;
        limits::Scope scope(small);  // the process now knows limits; this thread is armed only inside here
        CHECK_THROWS(limits::checkText(17));
    }
    CHECK_NOTHROW(limits::checkText(1u << 30));
    TempDir dir;
    Engine local(dir.str());
    std::string script = "SET @a = 'xxxxxxxxxxxxxxxx';";
    for (int i = 0; i < 16; ++i) script += "SET @a = CONCAT(@a, @a);";  // 1 MB, past the limit of the scope above
    for (const auto& r : local.runScript(script)) CHECK(r.error.empty());
}
