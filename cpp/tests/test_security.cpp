// cpp/tests/test_security.cpp -- proofs of concept for security flaws found in review. Each case failed before its fix.
#include <catch2/catch_test_macros.hpp>
#include "golden_runner.h"
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
