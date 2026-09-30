// cpp/tests/test_engine_procedures.cpp
#include <catch2/catch_test_macros.hpp>
#include "golden_runner.h"
#include "meradb/engine.h"
#include "test_util.h"

using namespace meradb;
using meradb_test::TempDir;
using meradb_test::runLast;

namespace {
struct Db {
    TempDir dir;
    Engine e{dir.str()};
    Db() { e.execute("BANAO TABLE accounts (id INT MUKHYA KUNJI, balance INT);"); }
    Result q(const std::string& sql) { return runLast(e, sql); }
};
}  // namespace

TEST_CASE("engine_procedures create and drop use Python's messages", "[engine][procedures]") {
    Db d;
    CHECK(d.q("BANAO PROCEDURE p(a INT, b TEXT) SHURU DIKHAO * SE t; KHATAM;").message == "Procedure 'p' ban gaya (2 parameter(s))");
    CHECK(d.q("BANAO PROCEDURE p() SHURU DIKHAO * SE t; KHATAM;").error == "[Execution Galti] Procedure 'p' pehle se hai");
    CHECK(d.q("BANAO PROCEDURE q(a INT, a TEXT) SHURU DIKHAO * SE t; KHATAM;").error ==
          "[Execution Galti] Procedure 'q': ek parameter naam do baar diya hai");
    CHECK(d.q("HATAO PROCEDURE p;").message == "Procedure 'p' hata diya");
    CHECK(d.q("HATAO PROCEDURE p;").error == "[Execution Galti] Procedure 'p' exist nahi karta");
}

TEST_CASE("engine_procedures CHALAO substitutes parameters and reports every message", "[engine][procedures]") {
    Db d;
    d.q("BANAO PROCEDURE add_acct(pid INT, bal INT) SHURU DAALO MEIN accounts MAAN (pid, bal); "
        "BADLO accounts RAKHO balance = balance + bal JAHAN id = pid; KHATAM;");
    Result r = d.q("CHALAO add_acct(5, 10);");
    CHECK(r.message == "Procedure 'add_acct' chal gaya (2 statement(s)): 1 row(s) daal di; 1 row(s) badal di");
    Result rows = d.q("DIKHAO id, balance SE accounts;");
    REQUIRE(rows.rows.size() == 1);
    CHECK(formatValue(rows.rows[0][0]) == "5");
    CHECK(formatValue(rows.rows[0][1]) == "20");
}

TEST_CASE("engine_procedures arguments are constant expressions coerced to the parameter type", "[engine][procedures]") {
    Db d;
    d.q("BANAO TABLE prices (id INT, amount FLOAT);");
    d.q("BANAO PROCEDURE put(pid INT, amt FLOAT) SHURU DAALO MEIN prices MAAN (pid, amt); KHATAM;");
    CHECK(d.q("CHALAO put(1 + 1, 3);").error.empty());               // 3 (INT) widens to 3.0 for a FLOAT parameter
    Result r = d.q("DIKHAO id, amount SE prices;");
    CHECK(formatValue(r.rows.at(0).at(0)) == "2");
    CHECK(formatValue(r.rows.at(0).at(1)) == "3.0");
    CHECK_FALSE(d.q("CHALAO put('x', 1);").error.empty());            // TEXT does not fit an INT parameter
    CHECK_FALSE(d.q("CHALAO put(id, 1);").error.empty());             // no row here: a column reference is an error
}

TEST_CASE("engine_procedures argument count and unknown name errors", "[engine][procedures]") {
    Db d;
    d.q("BANAO PROCEDURE p(a INT, b INT) SHURU DIKHAO * SE t; KHATAM;");
    CHECK(d.q("CHALAO p(1);").error == "[Execution Galti] Procedure 'p' ko 2 argument(s) chahiye, 1 mile");
    CHECK(d.q("CHALAO p(1, 2, 3);").error == "[Execution Galti] Procedure 'p' ko 2 argument(s) chahiye, 3 mile");
    CHECK(d.q("CHALAO nope();").error == "[Execution Galti] Procedure 'nope' exist nahi karta");
}

TEST_CASE("engine_procedures a failing statement keeps the earlier ones applied", "[engine][procedures]") {
    Db d;
    d.q("BANAO PROCEDURE two(pid INT) SHURU DAALO MEIN accounts MAAN (pid, 1); DAALO MEIN nosuch MAAN (1); KHATAM;");
    CHECK(d.q("CHALAO two(7);").error == "[Execution Galti] Table 'nosuch' exist nahi karta");
    CHECK(d.q("DIKHAO * SE accounts;").rows.size() == 1);   // mirrored: no implicit transaction
}

TEST_CASE("engine_procedures can call other procedures and be used by triggers", "[engine][procedures]") {
    Db d;
    d.q("BANAO TABLE audit (id INT, note TEXT);");
    d.q("BANAO PROCEDURE note(i INT, t TEXT) SHURU DAALO MEIN audit MAAN (i, t); KHATAM;");
    d.q("BANAO PROCEDURE outer_p(i INT) SHURU CHALAO note(i, 'from outer'); KHATAM;");
    CHECK(d.q("CHALAO outer_p(3);").message ==
          "Procedure 'outer_p' chal gaya (1 statement(s)): Procedure 'note' chal gaya (1 statement(s)): 1 row(s) daal di");
    d.q("BANAO TRIGGER tr BAAD DAALO PAR accounts SHURU CHALAO note(NAYA.id, 'trigger'); KHATAM;");
    d.q("DAALO MEIN accounts MAAN (9, 0);");
    CHECK(d.q("DIKHAO * SE audit;").rows.size() == 2);
}

TEST_CASE("engine_procedures recursion hits the nesting cap", "[engine][procedures]") {
    Db d;
    d.q("BANAO PROCEDURE r() SHURU CHALAO r(); KHATAM;");
    CHECK(d.q("CHALAO r();").error.find("bahut gehra") != std::string::npos);
}

TEST_CASE("engine_procedures are superuser-only for restricted sessions", "[engine][procedures]") {
    TempDir dir;
    auto inst = std::make_shared<Instance>(dir.str());
    Engine admin(inst), ravi(inst);
    ravi.user = "ravi";
    admin.execute("BANAO USER ravi GUPT 'pw'; BANAO PROCEDURE p() SHURU DIKHAO * SE t; KHATAM;");
    CHECK(runLast(ravi, "CHALAO p();").error ==
          "[Execution Galti] 'ravi' superuser nahi hai -- 'CallProcedure' jaisa DDL/admin command sirf superuser "
          "(bina username connect kiya session) chala sakta hai");
    CHECK(runLast(ravi, "BANAO PROCEDURE z() SHURU DIKHAO * SE t; KHATAM;").error.find("'CreateProcedure'") != std::string::npos);
}
