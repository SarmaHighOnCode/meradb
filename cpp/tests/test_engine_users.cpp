// cpp/tests/test_engine_users.cpp
#include <catch2/catch_test_macros.hpp>
#include "golden_runner.h"   // runLast
#include "meradb/engine.h"
#include "test_util.h"

using namespace meradb;
using meradb_test::TempDir;
using meradb_test::runLast;

namespace {
std::string notSuper(const std::string& user, const std::string& cls) {
    return "[Execution Galti] '" + user + "' superuser nahi hai -- '" + cls +
           "' jaisa DDL/admin command sirf superuser (bina username connect kiya session) chala sakta hai";
}
std::string noPriv(const std::string& user, const std::string& table, const std::string& priv) {
    return "[Execution Galti] '" + user + "' ko table '" + table + "' par " + priv + " ka adhikar nahi hai";
}
struct Fixture {
    TempDir dir;
    std::shared_ptr<Instance> inst = std::make_shared<Instance>(dir.str());
    Engine admin{inst};
    Engine ravi{inst};
    Fixture() {
        ravi.user = "ravi";
        admin.execute("BANAO USER ravi GUPT 'pw';"
                      "BANAO TABLE students (id INT MUKHYA KUNJI, name TEXT);"
                      "BANAO TABLE other (id INT);"
                      "DAALO MEIN students MAAN (1, 'a');"
                      "DAALO MEIN other MAAN (7);");
    }
};
}  // namespace

TEST_CASE("engine_users user and grant statements return Python's messages", "[engine][users]") {
    TempDir dir;
    Engine e(dir.str());
    CHECK(runLast(e, "BANAO USER ravi GUPT 'pw';").message == "User 'ravi' ban gaya");
    CHECK(runLast(e, "BANAO USER ravi GUPT 'pw';").error == "[Execution Galti] User 'ravi' pehle se hai");
    CHECK(runLast(e, "ADHIKAR DO DIKHAO, DAALO PAR students KO ravi;").message ==
          "'ravi' ko 'main.students' par DIKHAO, DAALO ka adhikar mil gaya");
    CHECK(runLast(e, "ADHIKAR WAPAS DAALO PAR students SE ravi;").message ==
          "'ravi' se 'main.students' par DAALO ka adhikar wapas le liya");
    CHECK(runLast(e, "ADHIKAR DO SAB PAR students KO ghost;").error == "[Execution Galti] User 'ghost' exist nahi karta");
    CHECK(runLast(e, "HATAO USER ravi;").message == "User 'ravi' hata diya");
    CHECK(runLast(e, "HATAO USER ravi;").error == "[Execution Galti] User 'ravi' exist nahi karta");
}

TEST_CASE("engine_users grants are recorded against the CURRENT database", "[engine][users]") {
    TempDir dir;
    Engine e(dir.str());
    e.execute("BANAO USER ravi GUPT 'pw'; BANAO DATABASE school; ISTEMAL school;");
    CHECK(runLast(e, "ADHIKAR DO DIKHAO PAR t KO ravi;").message == "'ravi' ko 'school.t' par DIKHAO ka adhikar mil gaya");
    CHECK(e.instance().users().hasPrivilege("ravi", "school", "t", "DIKHAO"));
    CHECK_FALSE(e.instance().users().hasPrivilege("ravi", "main", "t", "DIKHAO"));
}

TEST_CASE("engine_users superuser sessions are never checked", "[engine][users]") {
    Fixture f;
    CHECK(runLast(f.admin, "DIKHAO * SE students;").error.empty());
    CHECK(runLast(f.admin, "HATAO TABLE other;").error.empty());
}

TEST_CASE("engine_users SELECT needs DIKHAO on the FROM table and every joined table", "[engine][users]") {
    Fixture f;
    CHECK(runLast(f.ravi, "DIKHAO * SE students;").error == noPriv("ravi", "students", "DIKHAO"));
    f.admin.execute("ADHIKAR DO DIKHAO PAR students KO ravi;");
    CHECK(runLast(f.ravi, "DIKHAO * SE students;").error.empty());
    CHECK(runLast(f.ravi, "DIKHAO * SE students s MILAO other o PAR s.id = o.id;").error == noPriv("ravi", "other", "DIKHAO"));
    f.admin.execute("ADHIKAR DO DIKHAO PAR other KO ravi;");
    CHECK(runLast(f.ravi, "DIKHAO * SE students s MILAO other o PAR s.id = o.id;").error.empty());
}

TEST_CASE("engine_users DML privileges are checked per statement kind", "[engine][users]") {
    Fixture f;
    CHECK(runLast(f.ravi, "DAALO MEIN students MAAN (2, 'b');").error == noPriv("ravi", "students", "DAALO"));
    CHECK(runLast(f.ravi, "BADLO students RAKHO name = 'z';").error == noPriv("ravi", "students", "BADLO"));
    CHECK(runLast(f.ravi, "MITAO SE students;").error == noPriv("ravi", "students", "MITAO"));
    f.admin.execute("ADHIKAR DO DAALO, BADLO PAR students KO ravi;");
    CHECK(runLast(f.ravi, "DAALO MEIN students MAAN (2, 'b');").message == "1 row(s) daal di");
    CHECK(runLast(f.ravi, "BADLO students RAKHO name = 'z' JAHAN id = 2;").message == "1 row(s) badal di");
    CHECK(runLast(f.ravi, "MITAO SE students;").error == noPriv("ravi", "students", "MITAO"));
}

TEST_CASE("engine_users INSERT ... DIKHAO also needs DIKHAO on the source", "[engine][users]") {
    Fixture f;
    f.admin.execute("ADHIKAR DO DAALO PAR other KO ravi;");
    CHECK(runLast(f.ravi, "DAALO MEIN other DIKHAO id SE students;").error == noPriv("ravi", "students", "DIKHAO"));
    f.admin.execute("ADHIKAR DO DIKHAO PAR students KO ravi;");
    CHECK(runLast(f.ravi, "DAALO MEIN other DIKHAO id SE students;").message == "1 row(s) daal di");
}

TEST_CASE("engine_users everything else is superuser-only and names the Python class", "[engine][users]") {
    Fixture f;
    f.admin.execute("ADHIKAR DO SAB PAR students KO ravi;");
    CHECK(runLast(f.ravi, "BANAO TABLE x (id INT);").error == notSuper("ravi", "CreateTable"));
    CHECK(runLast(f.ravi, "HATAO TABLE students;").error == notSuper("ravi", "DropTable"));
    CHECK(runLast(f.ravi, "SHURU;").error == notSuper("ravi", "Begin"));
    CHECK(runLast(f.ravi, "PAKKA;").error == notSuper("ravi", "Commit"));
    CHECK(runLast(f.ravi, "BANAO USER x GUPT 'y';").error == notSuper("ravi", "CreateUser"));
    CHECK(runLast(f.ravi, "ADHIKAR DO SAB PAR other KO ravi;").error == notSuper("ravi", "Grant"));
    CHECK(runLast(f.ravi, "ISTEMAL main;").error == notSuper("ravi", "UseDatabase"));
    CHECK(runLast(f.ravi, "nonsense;").error.find("Parser Galti") != std::string::npos);  // parse errors come first
}

TEST_CASE("engine_users SAMJHAO checks the inner statement", "[engine][users]") {
    Fixture f;
    CHECK(runLast(f.ravi, "SAMJHAO DIKHAO * SE students;").error == noPriv("ravi", "students", "DIKHAO"));
    CHECK(runLast(f.ravi, "SAMJHAO BANAO TABLE x (id INT);").error == notSuper("ravi", "CreateTable"));
    f.admin.execute("ADHIKAR DO DIKHAO PAR students KO ravi;");
    CHECK(runLast(f.ravi, "SAMJHAO DIKHAO * SE students;").error.empty());
}

TEST_CASE("engine_users set operations re-check every side", "[engine][users]") {
    Fixture f;
    f.admin.execute("ADHIKAR DO DIKHAO PAR students KO ravi;");
    CHECK(runLast(f.ravi, "DIKHAO id SE students SANYUKT DIKHAO id SE other;").error == noPriv("ravi", "other", "DIKHAO"));
    f.admin.execute("ADHIKAR DO DIKHAO PAR other KO ravi;");
    CHECK(runLast(f.ravi, "DIKHAO id SE students SANYUKT DIKHAO id SE other;").error.empty());
}

TEST_CASE("engine_users a view is granted by its own name", "[engine][users]") {
    Fixture f;
    f.admin.execute("BANAO VIEW v KAHO DIKHAO id SE students;");
    CHECK(runLast(f.ravi, "DIKHAO * SE v;").error == noPriv("ravi", "v", "DIKHAO"));
    f.admin.execute("ADHIKAR DO DIKHAO PAR v KO ravi;");
    // no DIKHAO on `students` itself is needed: the view's stored query is not re-checked
    CHECK(runLast(f.ravi, "DIKHAO * SE v;").error.empty());
    CHECK(runLast(f.ravi, "DIKHAO * SE students;").error == noPriv("ravi", "students", "DIKHAO"));
}

TEST_CASE("engine_users users survive a new Instance on the same folder", "[engine][users]") {
    TempDir dir;
    { Engine e(dir.str()); e.execute("BANAO USER ravi GUPT 'pw'; ADHIKAR DO DIKHAO PAR t KO ravi;"); }
    Engine again(dir.str());
    CHECK(again.instance().users().verify("ravi", "pw"));
    CHECK(again.instance().users().hasPrivilege("ravi", "main", "t", "DIKHAO"));
}

TEST_CASE("engine_users golden scripts match the Python engine", "[engine][users][golden]") {
    meradb_test::replayGolden("users");
}
