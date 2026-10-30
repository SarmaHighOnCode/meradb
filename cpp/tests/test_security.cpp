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

// ---- requests that are not statements: nothing in checkPrivileges looks at them ----

namespace {
protocol::Json requestOf(const char* type) {
    protocol::Json message = protocol::Json::object();
    message["type"] = type;
    return message;
}
}  // namespace

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
