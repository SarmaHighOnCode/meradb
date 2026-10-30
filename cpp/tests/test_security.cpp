// cpp/tests/test_security.cpp -- proofs of concept for security flaws found in review. Each case failed before its fix.
#include <catch2/catch_test_macros.hpp>
#include "golden_runner.h"
#include "meradb/crypto.h"
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "meradb/fs_util.h"
#include "server_fixture.h"
#include "test_util.h"
#include <filesystem>
#include <fstream>

using namespace meradb;
using namespace meradb_test;

namespace {

struct SecFixture {
    TempDir dir;
    std::shared_ptr<Instance> inst = std::make_shared<Instance>(dir.str());
    Engine admin{inst};
    Engine ravi{inst};
    SecFixture() {
        ravi.user = "ravi";
        admin.execute("BANAO USER ravi GUPT 'pw';"
                      "BANAO TABLE pub (id INT MUKHYA KUNJI, name TEXT, secret INT);"
                      "BANAO TABLE hidden (id INT);"
                      "DAALO MEIN pub MAAN (1, 'a', 30), (2, 'b', 10), (3, 'c', 20);"
                      "DAALO MEIN hidden MAAN (777);");
    }
    std::string nameOf(int id) {
        return formatValue(runLast(admin, "DIKHAO name SE pub JAHAN id = " + std::to_string(id) + ";").rows.at(0).at(0));
    }
};

protocol::Json requestOf(const char* type) {
    protocol::Json message = protocol::Json::object();
    message["type"] = type;
    return message;
}

}  // namespace

// ---- privileges: statements that read or write more than their check looked at ----

TEST_CASE("security statements that re-enter the engine stay checked", "[security][privileges]") {
    SecFixture f;
    CHECK_FALSE(runLast(f.ravi, "SET @x = (DIKHAO id SE hidden);").error.empty());
    runLast(f.ravi, "PREPARE p FROM 'DIKHAO ?';");
    CHECK_FALSE(runLast(f.ravi, "EXECUTE p USING (DIKHAO id SE hidden);").error.empty());
    CHECK_FALSE(runLast(f.ravi, "PREPARE q FROM (DIKHAO 'DIKHAO 1' SE hidden);").error.empty());
    f.admin.execute("BANAO FUNCTION peek() RETURNS INT SHURU RETURN (DIKHAO id SE hidden); KHATAM;");
    CHECK_FALSE(runLast(f.ravi, "DIKHAO peek();").error.empty());
}

TEST_CASE("security EXPLAIN of a set operation needs DIKHAO on both sides", "[security][privileges]") {
    SecFixture f;
    CHECK(runLast(f.ravi, "SAMJHAO DIKHAO id SE hidden SANYUKT DIKHAO id SE hidden;").error ==
          "[Execution Galti] 'ravi' ko table 'hidden' par DIKHAO ka adhikar nahi hai");
    f.admin.execute("ADHIKAR DO DIKHAO PAR hidden KO ravi;");
    CHECK(runLast(f.ravi, "SAMJHAO DIKHAO id SE hidden SANYUKT DIKHAO id SE hidden;").error.empty());
}

TEST_CASE("security TAKRAAV PAR BADLO needs BADLO, on the assigned columns", "[security][privileges]") {
    SecFixture f;
    f.admin.execute("ADHIKAR DO DAALO PAR pub KO ravi;");
    CHECK(runLast(f.ravi, "DAALO MEIN pub MAAN (1, 'x', 0) TAKRAAV PAR BADLO name = 'hacked';").error ==
          "[Execution Galti] 'ravi' ko table 'pub' par BADLO ka adhikar nahi hai");
    CHECK(f.nameOf(1) == "a");
    f.admin.execute("ADHIKAR DO BADLO (secret) PAR pub KO ravi;");
    CHECK(runLast(f.ravi, "DAALO MEIN pub MAAN (1, 'x', 0) TAKRAAV PAR BADLO name = 'hacked';").error ==
          "[Execution Galti] 'ravi' ko table 'pub' ke column 'name' par BADLO ka adhikar nahi hai");
    CHECK(f.nameOf(1) == "a");
    CHECK(runLast(f.ravi, "DAALO MEIN pub MAAN (1, 'x', 0) TAKRAAV PAR BADLO secret = 5;").error.empty());
    CHECK(runLast(f.ravi, "DAALO MEIN pub MAAN (9, 'new', 0);").error.empty());  // a plain insert still only needs DAALO
}

TEST_CASE("security a column-limited DIKHAO cannot be widened through BADLO, MITAO or KRAM", "[security][privileges]") {
    SecFixture f;
    const std::string denied = "[Execution Galti] 'ravi' ko table 'pub' ke column 'secret' par DIKHAO ka adhikar nahi hai";
    f.admin.execute("ADHIKAR DO DIKHAO (id, name) PAR pub KO ravi;"
                    "ADHIKAR DO DAALO, BADLO (name), MITAO PAR pub KO ravi;");
    // copying a hidden value into a readable column
    CHECK(runLast(f.ravi, "BADLO pub RAKHO name = CONCAT('', secret) JAHAN id = 1;").error == denied);
    CHECK(f.nameOf(1) == "a");
    CHECK(runLast(f.ravi, "DAALO MEIN pub MAAN (1, 'x', 0) TAKRAAV PAR BADLO name = CONCAT('', secret);").error == denied);
    CHECK(f.nameOf(1) == "a");
    // telling hidden values apart by the rows touched
    CHECK(runLast(f.ravi, "BADLO pub RAKHO name = 'z' JAHAN secret > 25;").error == denied);
    CHECK(runLast(f.ravi, "MITAO SE pub JAHAN secret > 25;").error == denied);
    CHECK(runLast(f.ravi, "MITAO SE pub JAHAN pub.secret > 25;").error == denied);
    // the same through a correlated sub-select
    f.admin.execute("ADHIKAR DO DIKHAO PAR hidden KO ravi;");
    CHECK(runLast(f.ravi, "MITAO SE pub JAHAN EXISTS (DIKHAO 1 SE hidden JAHAN secret > 25);").error == denied);
    CHECK(runLast(f.ravi, "DIKHAO id SE pub JAHAN EXISTS (DIKHAO 1 SE hidden JAHAN secret > 25);").error == denied);
    // an output alias does not excuse sorting by the real column of that name
    CHECK(runLast(f.ravi, "DIKHAO id KAHO secret SE pub KRAM secret;").error == denied);
    // what the grants do cover keeps working
    CHECK(runLast(f.ravi, "BADLO pub RAKHO name = CONCAT(name, '!') JAHAN id = 1;").error.empty());
    CHECK(f.nameOf(1) == "a!");
    CHECK(runLast(f.ravi, "DIKHAO id KAHO n SE pub KRAM n;").error.empty());
    CHECK(runLast(f.ravi, "MITAO SE pub JAHAN id = 3;").error.empty());
}

// ---- file access ----

TEST_CASE("security a database name can never leave the data folder", "[security][paths]") {
    TempDir outer;
    const std::filesystem::path data = outer.path() / "data";
    std::filesystem::create_directories(outer.path() / "outside");
    std::filesystem::create_directories(data / ".wapas");
    Engine e(data.string());
    for (const char* name : {"../outside", "..\\outside", "..", ".", ".wapas", "main/../../outside", "main:stream", "main.", "main ",
                             "a\nb", "C:\\Windows", "/etc"}) {
        INFO(name);
        ast::UseDatabase use;
        use.name = name;
        CHECK_THROWS_AS(e.executeStatement(use), ExecutionError);
        CHECK(e.currentDb == DEFAULT_DATABASE);
        CHECK_THROWS_AS(e.instance().dbDir(name), ExecutionError);
    }
    CHECK_FALSE(isSafeFileName(""));
    CHECK(isSafeFileName("palantir_2"));
    CHECK(isSafeFileName("my-db"));
}

TEST_CASE("security Windows device names cannot be used for new databases and tables", "[security][paths]") {
    TempDir dir;
    Engine e(dir.str());
    runLast(e, "BANAO TABLE ok_table (id INT);");
    for (const char* name : {"nul", "NUL", "con", "prn", "aux", "com1", "Com9", "lpt1", "LPT9"}) {
        CAPTURE(name);
        for (const std::string& sql : {std::string("BANAO DATABASE ") + name + ";", std::string("BANAO TABLE ") + name + " (id INT);",
                                       std::string("SUDHARO TABLE ok_table NAYA_NAAM ") + name + ";"})
            CHECK(runLast(e, sql).error.find("reserved") != std::string::npos);
    }
    // only the exact names: these are ordinary
    for (const char* name : {"console", "auxiliary", "com", "com10", "lpt0", "nullable"})
        CHECK(runLast(e, std::string("BANAO TABLE ") + name + " (id INT);").error.empty());
    CHECK_FALSE(isReservedDeviceName("com0"));
}

TEST_CASE("security a table name in catalog.json can never leave the database folder", "[security][paths]") {
    TempDir dir;
    std::filesystem::create_directories(dir.path() / "main");
    std::ofstream(dir.file("main/catalog.json")) << R"({"tables":{"../../evil":{"name":"../../evil","columns":[{"name":"a","type_name":"INT"}]}}})";
    Engine e(dir.str());
    CHECK_THROWS_AS(e.catalog().tablePath("../../evil"), StorageError);
    CHECK_THROWS_AS(e.catalog().tablePath("t:stream"), StorageError);
    CHECK_FALSE(runLast(e, "DIKHAO * SE t;").error.empty());
    CHECK_FALSE(std::filesystem::exists(dir.path().parent_path() / "evil.tbl"));
}

TEST_CASE("security the hello message cannot choose a database outside the data folder", "[security][paths][server]") {
    RunningServer server;
    const auto outside = std::filesystem::path(server.dataDir()).parent_path() / ("outside_" + std::to_string(server.port()));
    std::filesystem::create_directories(outside);
    {
        RawClient client(server.port());
        auto reply = client.hello(nullptr, "../" + outside.filename().string());
        CHECK_FALSE(reply.value("ok", true));
        if (reply.value("ok", false)) client.query("BANAO TABLE planted (a INT);");
    }
    CHECK_FALSE(std::filesystem::exists(outside / "catalog.json"));
    std::error_code ignored;
    std::filesystem::remove_all(outside, ignored);
}

// ---- passwords ----

TEST_CASE("security a user cannot be created with an empty password", "[security][passwords]") {
    SecFixture f;
    CHECK(runLast(f.admin, "BANAO USER blank GUPT '';").error ==
          "[Execution Galti] User 'blank' ka password khali nahi ho sakta");
    CHECK_FALSE(f.inst->users().exists("blank"));
}

namespace {
// A users.json entry the way the old code wrote it: 100,000 iterations and no "iterations" key.
nlohmann::ordered_json legacyEntry(const std::string& password) {
    const crypto::Bytes salt(16, 0x5A);
    nlohmann::ordered_json entry = nlohmann::ordered_json::object();
    entry["salt"] = crypto::toHex(salt);
    entry["hash"] = crypto::toHex(crypto::pbkdf2HmacSha256(password, salt, PBKDF2_LEGACY_ITERATIONS, 32));
    entry["grants"] = nlohmann::ordered_json::object();
    entry["grants"]["main.t"] = {"DIKHAO"};
    return entry;
}
nlohmann::ordered_json readUsersFile(const TempDir& dir) {
    std::ifstream in(dir.file("users.json"));
    return nlohmann::ordered_json::parse(in);
}
}  // namespace

TEST_CASE("security an old 100,000 iteration account still logs in and is upgraded by the login", "[security][passwords]") {
    TempDir dir;
    nlohmann::ordered_json file = nlohmann::ordered_json::object();
    file["old"] = legacyEntry("hunter2");
    std::ofstream(dir.file("users.json")) << file.dump(2);

    UserStore users(dir.str());
    CHECK_FALSE(users.verify("old", "wrong"));
    CHECK_FALSE(readUsersFile(dir)["old"].contains("iterations"));  // a failed login changes nothing

    CHECK(users.verify("old", "hunter2"));
    auto upgraded = readUsersFile(dir)["old"];
    CHECK(upgraded["iterations"] == PBKDF2_ITERATIONS);
    CHECK(upgraded["hash"] != file["old"]["hash"]);
    CHECK(upgraded["salt"] != file["old"]["salt"]);
    CHECK(upgraded["grants"] == file["old"]["grants"]);

    CHECK(users.verify("old", "hunter2"));  // the upgraded hash works, in memory and from the file
    UserStore reloaded(dir.str());
    CHECK(reloaded.verify("old", "hunter2"));
    CHECK_FALSE(reloaded.verify("old", "wrong"));
}

TEST_CASE("security an unknown user takes as long to refuse as a known one", "[security][passwords]") {
    TempDir dir;
    UserStore users(dir.str());
    users.create("ravi", "pw");
    auto timed = [&](const char* name) {
        auto start = std::chrono::steady_clock::now();
        CHECK_FALSE(users.verify(name, "wrong"));
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    };
    const double known = timed("ravi");
    const double unknown = timed("ghost");
    CHECK(unknown > known / 3);  // same work; the margin only absorbs scheduling noise
}

TEST_CASE("security damaged users.json entries never log in and never crash", "[security][passwords][corruption]") {
    for (const char* text : {R"({"r":{"salt":"zz","hash":"00"}})", R"({"r":{"salt":5,"hash":[]}})", R"({"r":7})",
                             R"({"r":{"role":true}})", R"({"r":{"salt":"00","hash":"00","iterations":-1}})",
                             R"({"r":{"salt":"00","hash":"00","iterations":0}})",
                             R"({"r":{"salt":"00","hash":"00","iterations":"many"}})",
                             R"({"r":{"salt":"00","hash":"00","iterations":99999999999}})"}) {
        INFO(text);
        TempDir dir;
        std::ofstream(dir.file("users.json")) << text;
        UserStore users(dir.str());
        CHECK_FALSE(users.verify("r", ""));
        CHECK_FALSE(users.verify("r", "x"));
    }
    for (const char* text : {"[1,2]", "{", "not json", ""}) {
        INFO(text);
        TempDir dir;
        std::ofstream(dir.file("users.json")) << text;
        CHECK_THROWS_AS(UserStore(dir.str()), StorageError);
    }
}

TEST_CASE("security passwords in queries never reach the server log", "[security][passwords][server]") {
    RunningServer server("", /*verbose=*/true);
    RawClient client(server.port());
    REQUIRE(client.hello().value("ok", false));
    for (const char* text : {"BANAO USER asha GUPT 'TopSecret99';",
                             "banao   user\n asha gupt 'TopSecret99';",
                             "BANAO USER asha 'TopSecret99';",  // a typo: the parse error must not echo it
                             "PREPARE p FROM 'BANAO USER asha GUPT ''TopSecret99''';"})
        client.query(text);
    CHECK_FALSE(server.logContains("TopSecret99"));
    client.query("DIKHAO 'still logged';");  // anything else is logged as before
    CHECK(server.logContains("still logged"));
    client.query("DIKHAO user_name SE nothing_here;");  // a word that only contains USER is not a false alarm
    CHECK(server.logContains("nothing_here"));
}

#ifndef _WIN32
TEST_CASE("security users.json is readable by its owner only", "[security][passwords]") {
    TempDir dir;
    UserStore users(dir.str());
    users.create("ravi", "pw");
    const auto perms = std::filesystem::status(dir.path() / "users.json").permissions();
    CHECK((perms & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
}
#endif

// ---- strangers: a peer that has not logged in must not be able to use the server up ----

namespace {
// True when the server closes the connection within `seconds` (a receive timeout means it is still open).
bool closedWithin(RawClient& client, double seconds) {
    client.socket().setReceiveTimeout(seconds);
    try {
        return !client.receive().has_value();
    } catch (const net::NetError&) {
        return false;
    }
}
}  // namespace

TEST_CASE("security a peer that never says hello is dropped", "[security][server][limits]") {
    RunningServer server("", false, protocol::kMaxMessageBytes,
                         [](ServerOptions& options) { options.handshakeTimeoutSeconds = 0.3; });
    RawClient silent(server.port());
    CHECK(closedWithin(silent, 3.0));
}

TEST_CASE("security sending a hello one byte at a time does not extend the deadline", "[security][server][limits]") {
    RunningServer server("", false, protocol::kMaxMessageBytes,
                         [](ServerOptions& options) { options.handshakeTimeoutSeconds = 0.4; });
    RawClient slow(server.port());
    bool closed = false;
    try {
        for (int i = 0; i < 40; ++i) {  // 2 s in all: five times the deadline
            slow.sendRaw(" ");
            sleepMs(50);
        }
    } catch (const net::NetError&) {  // the server hung up while we were still sending
        closed = true;
    }
    CHECK((closed || closedWithin(slow, 1.0)));
}

TEST_CASE("security a logged-in session has no handshake deadline", "[security][server][limits]") {
    RunningServer server("", false, protocol::kMaxMessageBytes,
                         [](ServerOptions& options) { options.handshakeTimeoutSeconds = 0.2; });
    RawClient client(server.port());
    REQUIRE(client.hello().value("ok", false));
    sleepMs(600);
    CHECK(client.request(requestOf("ping")).value("ok", false));
}

TEST_CASE("security a hello is limited in size, a logged-in session is not", "[security][server][limits]") {
    RunningServer server;
    {
        RawClient client(server.port());
        auto big = requestOf("hello");
        big["padding"] = std::string(protocol::kMaxHelloBytes, 'x');
        bool refused = true;
        try {
            client.send(big);
            refused = !client.receive().has_value();
        } catch (const net::NetError&) {  // closed with bytes still unread, which the OS reports as a reset
        }
        CHECK(refused);
    }
    RawClient client(server.port());
    REQUIRE(client.hello().value("ok", false));
    auto reply = client.query("DIKHAO '" + std::string(1024 * 1024, 'x') + "';");
    CHECK(reply.value("ok", false));
    CHECK(reply["results"][0]["error"] == "");
}

TEST_CASE("security connections beyond the limit are refused, and capacity comes back", "[security][server][limits]") {
    RunningServer server("", false, protocol::kMaxMessageBytes, [](ServerOptions& options) { options.maxConnections = 4; });
    std::vector<std::unique_ptr<RawClient>> held;
    for (int i = 0; i < 4; ++i) {
        held.push_back(std::make_unique<RawClient>(server.port()));
        REQUIRE(held.back()->hello().value("ok", false));
    }
    {
        RawClient extra(server.port());
        bool refused = true;
        try {
            auto reply = extra.hello();
            refused = !reply.value("ok", true);
            CHECK(reply.value("error", std::string()).find("connections") != std::string::npos);
        } catch (const std::exception&) {  // the refusal can lose the race with our hello: the OS then reports a reset
        }
        CHECK(refused);
        CHECK(server.logContains("connection limit"));
    }
    held.clear();
    CHECK(waitFor([&] {  // finished threads are reaped when the next connection arrives
        try {
            RawClient again(server.port());
            return again.hello().value("ok", false);
        } catch (const std::exception&) {
            return false;
        }
    }));
}

TEST_CASE("security password guessing from one address is locked out, then recovers", "[security][server][limits]") {
    RunningServer server("", false, protocol::kMaxMessageBytes, [](ServerOptions& options) {
        options.loginFailureLimit = 3;
        options.loginLockSeconds = 1.5;
    });
    server.instance().users().create("ravi", "pw");
    auto attempt = [&](const char* password) {
        RawClient client(server.port());
        return client.hello(password, nullptr, "ravi");
    };
    CHECK_FALSE(attempt("guess1").value("ok", true));
    CHECK(attempt("pw").value("ok", false));  // a success clears the count
    for (const char* guess : {"guess2", "guess3", "guess4"}) CHECK_FALSE(attempt(guess).value("ok", true));
    auto locked = attempt("pw");  // the right password, during the lock
    CHECK_FALSE(locked.value("ok", true));
    CHECK(locked.value("error", std::string()) == "User ya password galat hai");
    CHECK(server.logContains("login mana"));
    sleepMs(1600);
    CHECK(attempt("pw").value("ok", false));
}

TEST_CASE("security guessing the shared password is locked out too", "[security][server][limits]") {
    RunningServer server("s3cret", false, protocol::kMaxMessageBytes, [](ServerOptions& options) { options.loginFailureLimit = 2; });
    auto attempt = [&](const char* password) {
        RawClient client(server.port());
        return client.hello(password).value("ok", false);
    };
    CHECK_FALSE(attempt("a"));
    CHECK_FALSE(attempt("b"));
    CHECK_FALSE(attempt("s3cret"));
}

TEST_CASE("security a server without a password will not open to the network", "[security][server]") {
    // serve() refuses before binding, so nothing here ever listens on a non-loopback address
    TempDir dir;
    ServerOptions options;
    options.dataDir = dir.str();
    options.host = "0.0.0.0";
    options.port = 0;
    CHECK(serve(options) == 1);
    CHECK_FALSE(std::filesystem::exists(dir.path() / "meradb.pid"));
}

// ---- requests that are not statements: nothing in checkPrivileges looks at them ----

TEST_CASE("security a restricted user cannot shut the server down", "[security][server]") {
    RunningServer server;
    server.instance().users().create("ravi", "pw");
    RawClient ravi(server.port());
    REQUIRE(ravi.hello("pw", nullptr, "ravi").value("ok", false));
    CHECK_FALSE(ravi.request(requestOf("shutdown")).value("ok", true));
    sleepMs(300);
    CHECK_FALSE(server.stopped());
    RawClient admin(server.port());  // the superuser session `meradb stop` uses still can
    REQUIRE(admin.hello().value("ok", false));
    CHECK(admin.request(requestOf("shutdown")).value("ok", false));
    CHECK(waitFor([&] { return server.stopped(); }));
}

TEST_CASE("security a restricted user does not get the server's status details", "[security][server]") {
    RunningServer server;
    server.instance().users().create("ravi", "pw");
    RawClient ravi(server.port());
    REQUIRE(ravi.hello("pw", nullptr, "ravi").value("ok", false));
    auto reply = ravi.request(requestOf("status"));
    CHECK(reply.value("ok", false));
    for (const char* key : {"data_dir", "databases", "pid", "sessions", "started"}) CHECK_FALSE(reply.contains(key));
    RawClient admin(server.port());
    REQUIRE(admin.hello().value("ok", false));
    CHECK(admin.request(requestOf("status")).contains("databases"));
}

TEST_CASE("security the schema request shows a restricted user only what it was granted", "[security][server]") {
    RunningServer server;
    server.instance().users().create("ravi", "pw");
    RawClient admin(server.port());
    REQUIRE(admin.hello().value("ok", false));
    admin.query("BANAO TABLE topsecret (salary INT);"
                "BANAO TABLE mine (id INT, note TEXT);"
                "BANAO TABLE partly (id INT, hiddencol INT);"
                "ADHIKAR DO DAALO PAR mine KO ravi;"
                "ADHIKAR DO DIKHAO (id) PAR partly KO ravi;"
                "BANAO DATABASE otherdb;"
                "ISTEMAL otherdb;"
                "BANAO TABLE elsewhere (a INT);");
    RawClient ravi(server.port());
    REQUIRE(ravi.hello("pw", nullptr, "ravi").value("ok", false));
    const std::string tree = ravi.request(requestOf("schema")).dump();
    for (const char* hidden : {"topsecret", "salary", "otherdb", "elsewhere", "hiddencol"}) {
        INFO(hidden);
        CHECK(tree.find(hidden) == std::string::npos);
    }
    for (const char* shown : {"mine", "note", "partly"}) {
        INFO(shown);
        CHECK(tree.find(shown) != std::string::npos);
    }
    const std::string full = admin.request(requestOf("schema")).dump();
    CHECK(full.find("topsecret") != std::string::npos);
    CHECK(full.find("otherdb") != std::string::npos);
}
