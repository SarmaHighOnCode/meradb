// cpp/tests/test_server_limits.cpp -- who may log in, and how much one client may take from the others.
#include <catch2/catch_test_macros.hpp>
#include "meradb/client.h"
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "server_fixture.h"
#include <string>

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
