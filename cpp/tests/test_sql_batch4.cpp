// cpp/tests/test_sql_batch4.cpp -- transactions (autocommit, SAVEPOINT), trigger rules (BEFORE/AFTER, SIGNAL, RAKHO NAYA.col),
// stored procedures with variables / IF / loops / OUT parameters, stored functions, cursors, and PREPARE / EXECUTE.
//
// MeraDB-C++ only (the Python engine does not have them), so every expectation is written out here.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "golden_runner.h"
#include "meradb/catalog.h"
#include "meradb/client.h"
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "meradb/highlight.h"
#include "meradb/parser.h"
#include "server_fixture.h"
#include "test_util.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

using namespace meradb;
using Catch::Matchers::ContainsSubstring;
using meradb_test::RunningServer;
using meradb_test::TempDir;

namespace {

Result run(Engine& e, const std::string& sql) { return meradb_test::runLast(e, sql); }

// "a|b|c" per row, rows joined by ";" -- so one REQUIRE shows a whole result.
std::string flat(const Result& r) {
    std::string out;
    for (size_t i = 0; i < r.rows.size(); ++i) {
        if (i) out += ";";
        for (size_t j = 0; j < r.rows[i].size(); ++j) out += (j ? "|" : "") + formatValue(r.rows[i][j]);
    }
    return out;
}

std::string q(Engine& e, const std::string& sql) {
    Result r = run(e, sql);
    INFO(sql);
    INFO(r.error);
    REQUIRE(r.error.empty());
    return flat(r);
}

// The error message of a failing statement, without the "[... Galti] " stage prefix ("" if it ran fine).
std::string errorOf(Engine& e, const std::string& sql) {
    std::string error = run(e, sql).error;
    size_t close = error.find("Galti] ");
    return close == std::string::npos ? error : error.substr(close + 7);
}

void ok(Engine& e, const std::string& sql) {
    Result r = run(e, sql);
    INFO(sql);
    INFO(r.error);
    REQUIRE(r.error.empty());
}

std::string slurp(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

size_t entriesIn(const std::string& dir) {
    namespace fs = std::filesystem;
    if (!fs::is_directory(fs::u8path(dir))) return 0;
    size_t n = 0;
    for (const auto& entry : fs::directory_iterator(fs::u8path(dir))) {
        (void)entry;
        ++n;
    }
    return n;
}

}  // namespace

// ------------------------------------------------------------ autocommit ----

TEST_CASE("SET autocommit = 0 opens a transaction at the first write and keeps it until PAKKA / WAPAS", "[sql4][txn]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT MUKHYA KUNJI, v INT);");
    CHECK(e.autocommit());
    CHECK(q(e, "SHOW VARIABLES LIKE 'autocommit';") == "autocommit|ON");
    ok(e, "SET autocommit = 0;");
    CHECK_FALSE(e.autocommit());
    CHECK(q(e, "SHOW VARIABLES LIKE 'auto%';") == "autocommit|OFF");
    CHECK(q(e, "SHOW VARIABLES LIKE 'zzz';").empty());
    CHECK_FALSE(e.inTransaction());  // a read starts nothing
    ok(e, "DIKHAO * SE t;");
    CHECK_FALSE(e.inTransaction());
    ok(e, "DAALO MEIN t MAAN (1, 10);");
    CHECK(e.inTransaction());
    ok(e, "DAALO MEIN t MAAN (2, 20);");
    ok(e, "WAPAS;");
    CHECK_FALSE(e.inTransaction());
    CHECK(q(e, "DIKHAO * SE t;").empty());
    // the next write opens the next transaction; PAKKA keeps it
    ok(e, "DAALO MEIN t MAAN (3, 30);");
    CHECK(e.inTransaction());
    ok(e, "PAKKA;");
    CHECK(q(e, "DIKHAO * SE t;") == "3|30");
    // DDL takes part in the transaction too (it always did: WAPAS undoes a BANAO TABLE)
    ok(e, "BANAO TABLE u (x INT);");
    CHECK(e.inTransaction());
    ok(e, "WAPAS;");
    CHECK_THAT(errorOf(e, "DIKHAO * SE u;"), ContainsSubstring("exist nahi karta"));
    // SHURU closes (commits) an autocommit transaction and starts a real one
    ok(e, "DAALO MEIN t MAAN (4, 40);");
    ok(e, "SHURU;");
    ok(e, "DAALO MEIN t MAAN (5, 50);");
    ok(e, "WAPAS;");
    CHECK(q(e, "DIKHAO id SE t KRAM id;") == "3;4");
    // a SHURU while an explicit SHURU is open is still refused
    ok(e, "SHURU;");
    CHECK_THAT(errorOf(e, "SHURU;"), ContainsSubstring("pehle se chal raha hai"));
    ok(e, "WAPAS;");
    // SET autocommit = 1 commits the open transaction
    ok(e, "DAALO MEIN t MAAN (6, 60);");
    CHECK(e.inTransaction());
    ok(e, "SET autocommit = 1;");
    CHECK_FALSE(e.inTransaction());
    CHECK(e.autocommit());
    ok(e, "DAALO MEIN t MAAN (7, 70);");
    CHECK_FALSE(e.inTransaction());
    CHECK(q(e, "DIKHAO id SE t KRAM id;") == "3;4;6;7");
    // ON / OFF spell the same thing
    ok(e, "SET autocommit = OFF;");
    CHECK_FALSE(e.autocommit());
    ok(e, "set AUTOCOMMIT=on;");
    CHECK(e.autocommit());
    CHECK_THAT(errorOf(e, "SET autocommit = 'maybe';"), ContainsSubstring("0, 1, ON ya OFF"));
    CHECK_THAT(errorOf(e, "SET nosuch = 1;"), ContainsSubstring("koi variable nahi hai"));
}

TEST_CASE("an autocommit = 0 transaction holds the lock like SHURU does; closing the session rolls it back", "[sql4][txn]") {
    TempDir dir;
    auto inst = std::make_shared<Instance>(dir.str());
    inst->lockTimeoutSeconds = 0.2;
    {
        Engine a(inst), b(inst);
        ok(a, "BANAO TABLE t (id INT);");
        ok(a, "SET autocommit = 0;");
        ok(a, "DAALO MEIN t MAAN (1);");
        // the lock is per thread (recursive), so the other session must run on its own thread
        std::string busy;
        std::thread other([&] { busy = errorOf(b, "DAALO MEIN t MAAN (2);"); });
        other.join();
        CHECK_THAT(busy, ContainsSubstring("Database busy"));
        a.close();  // the unfinished transaction is rolled back
        CHECK_FALSE(a.inTransaction());
        CHECK(q(b, "DIKHAO * SE t;").empty());
    }
}

// ------------------------------------------------------------ savepoints ----

TEST_CASE("SAVEPOINT, ROLLBACK TO and RELEASE SAVEPOINT", "[sql4][txn][savepoint]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT MUKHYA KUNJI);");
    CHECK_THAT(errorOf(e, "SAVEPOINT a;"), ContainsSubstring("transaction chahiye"));
    CHECK_THAT(errorOf(e, "ROLLBACK TO a;"), ContainsSubstring("Koi transaction nahi chal raha"));
    CHECK_THAT(errorOf(e, "RELEASE SAVEPOINT a;"), ContainsSubstring("Koi transaction nahi chal raha"));
    ok(e, "SHURU; DAALO MEIN t MAAN (1);");
    ok(e, "SAVEPOINT a;");
    ok(e, "DAALO MEIN t MAAN (2);");
    ok(e, "SAVEPOINT b;");
    ok(e, "DAALO MEIN t MAAN (3);");
    CHECK(q(e, "DIKHAO id SE t KRAM id;") == "1;2;3");
    // back to a: the data is as it was at the savepoint, b is gone, a and the transaction stay
    CHECK_THAT(run(e, "ROLLBACK TO SAVEPOINT a;").message, ContainsSubstring("Savepoint 'a' tak WAPAS"));
    CHECK(e.inTransaction());
    CHECK(q(e, "DIKHAO id SE t KRAM id;") == "1");
    CHECK_THAT(errorOf(e, "ROLLBACK TO b;"), ContainsSubstring("Savepoint 'b' exist nahi karta"));
    ok(e, "DAALO MEIN t MAAN (4);");
    ok(e, "WAPAS TAK a;");  // the Hinglish spelling; a is still there, so it can be used again
    CHECK(q(e, "DIKHAO id SE t KRAM id;") == "1");
    ok(e, "DAALO MEIN t MAAN (5);");
    ok(e, "WAPAS TO SAVEPOINT a;");
    CHECK(q(e, "DIKHAO id SE t KRAM id;") == "1");
    // RELEASE forgets the savepoint, not the changes
    ok(e, "DAALO MEIN t MAAN (6);");
    ok(e, "RELEASE SAVEPOINT a;");
    CHECK(q(e, "DIKHAO id SE t KRAM id;") == "1;6");
    CHECK_THAT(errorOf(e, "ROLLBACK TO a;"), ContainsSubstring("exist nahi karta"));
    CHECK_THAT(errorOf(e, "RELEASE SAVEPOINT a;"), ContainsSubstring("exist nahi karta"));
    ok(e, "PAKKA;");
    CHECK(q(e, "DIKHAO id SE t KRAM id;") == "1;6");
    CHECK(entriesIn(dir.file(".wapas")) == 0);  // every copy is cleaned up
    CHECK_THAT(errorOf(e, "SAVEPOINT a;"), ContainsSubstring("transaction chahiye"));
}

TEST_CASE("a SAVEPOINT of the same name moves; RELEASE drops the later ones; WAPAS drops them all", "[sql4][txn][savepoint]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT);");
    ok(e, "SHURU;");
    ok(e, "SAVEPOINT a; DAALO MEIN t MAAN (1); SAVEPOINT b; DAALO MEIN t MAAN (2); SAVEPOINT a;");  // a again: now after row 2
    ok(e, "DAALO MEIN t MAAN (3);");
    ok(e, "ROLLBACK TO a;");
    CHECK(q(e, "DIKHAO id SE t KRAM id;") == "1;2");
    ok(e, "ROLLBACK TO b;");  // b was set before the new a, so it survived the first ROLLBACK TO
    CHECK(q(e, "DIKHAO id SE t KRAM id;") == "1");
    CHECK_THAT(errorOf(e, "ROLLBACK TO a;"), ContainsSubstring("exist nahi karta"));  // ... and took the new a with it
    ok(e, "SAVEPOINT x; SAVEPOINT y; SAVEPOINT z; RELEASE SAVEPOINT y;");
    CHECK_THAT(errorOf(e, "ROLLBACK TO z;"), ContainsSubstring("exist nahi karta"));
    ok(e, "ROLLBACK TO x;");  // x is older than y, so it is still there
    ok(e, "WAPAS;");
    CHECK(q(e, "DIKHAO GINO(*) SE t;") == "0");
    CHECK(entriesIn(dir.file(".wapas")) == 0);
}

TEST_CASE("ROLLBACK TO a savepoint also undoes views, triggers, procedures and functions made after it", "[sql4][txn][savepoint]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT, v INT);");
    ok(e, "SHURU; SAVEPOINT s;");
    ok(e, "BANAO VIEW vw KAHO DIKHAO id SE t;");
    ok(e, "BANAO TRIGGER tr BAAD DAALO PAR t SHURU DAALO MEIN t MAAN (0, 0); KHATAM;");
    ok(e, "BANAO PROCEDURE p() SHURU DIKHAO 1; KHATAM;");
    ok(e, "BANAO FUNCTION f() RETURNS INT SHURU RETURN 1; KHATAM;");
    ok(e, "BANAO TABLE other (x INT);");
    CHECK(q(e, "DIKHAO f();") == "1");
    ok(e, "ROLLBACK TO s;");
    CHECK(e.inTransaction());
    CHECK_THAT(errorOf(e, "DIKHAO * SE vw;"), ContainsSubstring("exist nahi karta"));
    CHECK_THAT(errorOf(e, "DIKHAO * SE other;"), ContainsSubstring("exist nahi karta"));
    CHECK_THAT(errorOf(e, "CHALAO p();"), ContainsSubstring("exist nahi karta"));
    CHECK_THAT(errorOf(e, "DIKHAO f();"), ContainsSubstring("nahi pata"));
    CHECK(q(e, "SHOW TRIGGERS;").empty());
    ok(e, "DAALO MEIN t MAAN (1, 1);");  // the trigger is gone: no extra row
    CHECK(q(e, "DIKHAO GINO(*) SE t;") == "1");
    ok(e, "PAKKA;");
}

TEST_CASE("a savepoint copy left behind by a crash is removed at the next start", "[sql4][txn][savepoint]") {
    TempDir dir;
    {
        Engine e(dir.str());
        ok(e, "BANAO TABLE t (id INT);");
    }
    namespace fs = std::filesystem;
    fs::create_directories(fs::u8path(dir.file(".wapas/main.sv1.sp")));
    {
        std::ofstream f(dir.file(".wapas/main.sv1.sp/catalog.json"));
        f << "{}";
    }
    Instance again(dir.str());
    CHECK(again.recovered().empty());  // a copy is not a transaction to roll back
    CHECK(entriesIn(dir.file(".wapas")) == 0);
}

// ------------------------------------------------------------ triggers ----

namespace {
void auditSetup(Engine& e) {
    ok(e, "BANAO TABLE t (id INT MUKHYA KUNJI, v INT);");
    ok(e, "BANAO TABLE log (ev TEXT, nv INT, ov INT);");
    ok(e, "BANAO TRIGGER bi PEHLE DAALO PAR t SHURU DAALO MEIN log MAAN ('bi', NAYA.v, KHALI); KHATAM;");
    ok(e, "BANAO TRIGGER ai BAAD DAALO PAR t SHURU DAALO MEIN log MAAN ('ai', NAYA.v, KHALI); KHATAM;");
    ok(e, "BANAO TRIGGER bu PEHLE BADLO PAR t SHURU DAALO MEIN log MAAN ('bu', NAYA.v, PURANA.v); KHATAM;");
    ok(e, "BANAO TRIGGER au BAAD BADLO PAR t SHURU DAALO MEIN log MAAN ('au', NAYA.v, PURANA.v); KHATAM;");
    ok(e, "BANAO TRIGGER bd PEHLE MITAO PAR t SHURU DAALO MEIN log MAAN ('bd', KHALI, PURANA.v); KHATAM;");
    ok(e, "BANAO TRIGGER ad BAAD MITAO PAR t SHURU DAALO MEIN log MAAN ('ad', KHALI, PURANA.v); KHATAM;");
}
}  // namespace

TEST_CASE("BEFORE and AFTER x INSERT / UPDATE / DELETE all fire, with the right NAYA and PURANA", "[sql4][trigger]") {
    TempDir dir;
    Engine e(dir.str());
    auditSetup(e);
    ok(e, "DAALO MEIN t MAAN (1, 10);");
    CHECK(q(e, "DIKHAO * SE log;") == "bi|10|KHALI;ai|10|KHALI");
    ok(e, "BADLO t RAKHO v = 15 JAHAN id = 1;");
    CHECK(q(e, "DIKHAO * SE log;") == "bi|10|KHALI;ai|10|KHALI;bu|15|10;au|15|10");
    ok(e, "MITAO SE t JAHAN id = 1;");
    CHECK(q(e, "DIKHAO * SE log;") == "bi|10|KHALI;ai|10|KHALI;bu|15|10;au|15|10;bd|KHALI|15;ad|KHALI|15");
    // one firing per affected row
    ok(e, "MITAO SE log;");
    ok(e, "DAALO MEIN t MAAN (1, 1), (2, 2);");
    CHECK(q(e, "DIKHAO ev, nv SE log KRAM ev, nv;") == "ai|1;ai|2;bi|1;bi|2");
    CHECK(q(e, "SHOW TRIGGERS;").find("bi|PEHLE|DAALO|t|") == 0);
    CHECK(q(e, "SHOW TRIGGERS;").find("ad|BAAD|MITAO|t|") != std::string::npos);
}

TEST_CASE("several triggers on one event run in creation order", "[sql4][trigger]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT);");
    ok(e, "BANAO TABLE log (n INT);");
    ok(e, "BANAO TRIGGER z_first PEHLE DAALO PAR t SHURU DAALO MEIN log MAAN (1); KHATAM;");
    ok(e, "BANAO TRIGGER a_second PEHLE DAALO PAR t SHURU DAALO MEIN log MAAN (2); KHATAM;");
    ok(e, "BANAO TRIGGER m_third PEHLE DAALO PAR t SHURU DAALO MEIN log MAAN (3); KHATAM;");
    ok(e, "DAALO MEIN t MAAN (1);");
    CHECK(q(e, "DIKHAO n SE log;") == "1;2;3");  // creation order, not alphabetical
    // ... and the order survives a reload
    Engine again(dir.str());
    ok(again, "DAALO MEIN t MAAN (2);");
    CHECK(q(again, "DIKHAO n SE log;") == "1;2;3;1;2;3");
}

TEST_CASE("BANAO TRIGGER IF NOT EXISTS and HATAO TRIGGER IF EXISTS", "[sql4][trigger]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT);");
    ok(e, "BANAO TABLE log (n INT);");
    ok(e, "BANAO TRIGGER tr BAAD DAALO PAR t SHURU DAALO MEIN log MAAN (1); KHATAM;");
    CHECK_THAT(errorOf(e, "BANAO TRIGGER tr BAAD DAALO PAR t SHURU DAALO MEIN log MAAN (2); KHATAM;"), ContainsSubstring("pehle se hai"));
    CHECK_THAT(run(e, "BANAO TRIGGER IF NOT EXISTS tr BAAD DAALO PAR t SHURU DAALO MEIN log MAAN (2); KHATAM;").message,
               ContainsSubstring("kuch nahi badla"));
    ok(e, "DAALO MEIN t MAAN (1);");
    CHECK(q(e, "DIKHAO n SE log;") == "1");  // the original body is still the one that runs
    ok(e, "BANAO TRIGGER IF NOT EXISTS other BAAD DAALO PAR t SHURU DAALO MEIN log MAAN (9); KHATAM;");
    ok(e, "HATAO TRIGGER IF EXISTS tr;");
    ok(e, "HATAO TRIGGER IF EXISTS tr;");
    CHECK_THAT(errorOf(e, "HATAO TRIGGER tr;"), ContainsSubstring("exist nahi karta"));
    ok(e, "HATAO TRIGGER other;");
    CHECK(q(e, "SHOW TRIGGERS;").empty());
}

TEST_CASE("SIGNAL in a trigger aborts the statement and undoes all of it", "[sql4][trigger][signal]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT MUKHYA KUNJI, v INT);");
    ok(e, "BANAO TABLE log (ev TEXT);");
    ok(e, "DAALO MEIN t MAAN (100, 1);");
    ok(e, "BANAO TRIGGER neg PEHLE DAALO PAR t SHURU IF NAYA.v < 0 THEN SIGNAL 'v negative nahi ho sakta'; END IF; KHATAM;");
    CHECK(errorOf(e, "DAALO MEIN t MAAN (1, -5);") == "v negative nahi ho sakta");
    CHECK(q(e, "DIKHAO id SE t;") == "100");
    ok(e, "DAALO MEIN t MAAN (1, 5);");
    // an AFTER trigger signals when the rows are already written (and an earlier trigger logged): all of it goes
    ok(e, "BANAO TRIGGER lg BAAD DAALO PAR t SHURU DAALO MEIN log MAAN ('inserted'); KHATAM;");
    ok(e, "BANAO TRIGGER seven BAAD DAALO PAR t SHURU AGAR NAYA.v = 7 TAB SIGNAL 'saat mana hai'; KHATAM AGAR; KHATAM;");
    CHECK(errorOf(e, "DAALO MEIN t MAAN (2, 6), (3, 7), (4, 8);") == "saat mana hai");
    CHECK(q(e, "DIKHAO id SE t KRAM id;") == "1;100");
    CHECK(q(e, "DIKHAO GINO(*) SE log;") == "0");  // lg logged the first rows of the failed statement, and that went too
    // the same through UPDATE and DELETE
    ok(e, "BANAO TRIGGER nodel BAAD MITAO PAR t SHURU AGAR PURANA.id = 100 TAB SIGNAL 'ye row nahi hat sakti'; KHATAM AGAR; KHATAM;");
    CHECK(errorOf(e, "MITAO SE t;") == "ye row nahi hat sakti");
    CHECK(q(e, "DIKHAO id SE t KRAM id;") == "1;100");
    ok(e, "BANAO TRIGGER noup PEHLE BADLO PAR t SHURU SIGNAL 'update band hai'; KHATAM;");
    CHECK(errorOf(e, "BADLO t RAKHO v = 0;") == "update band hai");
    CHECK(q(e, "DIKHAO v SE t KRAM id;") == "5;1");
    // inside a transaction only the signalling statement is undone
    ok(e, "SHURU; DAALO MEIN t MAAN (50, 50);");
    CHECK(errorOf(e, "BADLO t RAKHO v = 0;") == "update band hai");
    CHECK(q(e, "DIKHAO id SE t KRAM id;") == "1;50;100");
    ok(e, "WAPAS;");
    CHECK(q(e, "DIKHAO id SE t KRAM id;") == "1;100");
    CHECK(entriesIn(dir.file(".wapas")) == 0);
    // SIGNAL takes an expression, and is a plain execution error elsewhere
    ok(e, "HATAO TRIGGER noup; HATAO TRIGGER neg;");
    ok(e, "BANAO TRIGGER msg PEHLE DAALO PAR t SHURU SIGNAL CONCAT('id ', NAYA.id, ' mana hai'); KHATAM;");
    CHECK(errorOf(e, "DAALO MEIN t MAAN (9, 9);") == "id 9 mana hai");
}

TEST_CASE("RAKHO NAYA.col = expr in a BEFORE trigger changes the row about to be written", "[sql4][trigger]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT MUKHYA KUNJI, v INT, note TEXT);");
    ok(e, "BANAO TABLE log (ev TEXT, nv INT);");
    ok(e, "BANAO TRIGGER clamp PEHLE DAALO PAR t SHURU AGAR NAYA.v > 100 TAB RAKHO NAYA.v = 100; KHATAM AGAR; KHATAM;");
    ok(e, "BANAO TRIGGER stamp PEHLE DAALO PAR t SHURU SET NAYA.note = 'new:' || NAYA.id; KHATAM;");
    ok(e, "BANAO TRIGGER seen BAAD DAALO PAR t SHURU DAALO MEIN log MAAN ('ai', NAYA.v); KHATAM;");
    ok(e, "DAALO MEIN t MAAN (1, 500, 'x'), (2, 7, 'y');");
    CHECK(q(e, "DIKHAO * SE t KRAM id;") == "1|100|new:1;2|7|new:2");
    CHECK(q(e, "DIKHAO * SE log KRAM nv;") == "ai|7;ai|100");  // AFTER sees what was written
    // BEFORE UPDATE: the values the UPDATE computed can be corrected, PURANA is read-only
    ok(e, "BANAO TRIGGER keepsum PEHLE BADLO PAR t SHURU AGAR NAYA.v < PURANA.v TAB RAKHO NAYA.v = PURANA.v; KHATAM AGAR; KHATAM;");
    ok(e, "BADLO t RAKHO v = v - 50;");
    CHECK(q(e, "DIKHAO id, v SE t KRAM id;") == "1|100;2|7");  // never lowered
    ok(e, "BADLO t RAKHO v = v + 1;");
    CHECK(q(e, "DIKHAO id, v SE t KRAM id;") == "1|101;2|8");
    // a changed value is checked like any other: type, NOT NULL, unique
    ok(e, "BANAO TRIGGER dup PEHLE DAALO PAR t SHURU AGAR NAYA.id = 77 TAB RAKHO NAYA.id = 1; KHATAM AGAR; KHATAM;");
    CHECK_THAT(errorOf(e, "DAALO MEIN t MAAN (77, 1, 'z');"), ContainsSubstring("Duplicate value"));
    CHECK(q(e, "DIKHAO GINO(*) SE t;") == "2");
    ok(e, "HATAO TRIGGER dup;");
    ok(e, "BANAO TRIGGER bad PEHLE DAALO PAR t SHURU RAKHO NAYA.v = 'abc'; KHATAM;");
    CHECK_THAT(errorOf(e, "DAALO MEIN t MAAN (5, 1, 'z');"), ContainsSubstring("INT"));
    CHECK(q(e, "DIKHAO GINO(*) SE t;") == "2");
    ok(e, "HATAO TRIGGER bad;");
    ok(e, "BANAO TRIGGER nocol PEHLE DAALO PAR t SHURU RAKHO NAYA.nope = 1; KHATAM;");
    CHECK_THAT(errorOf(e, "DAALO MEIN t MAAN (5, 1, 'z');"), ContainsSubstring("column 'nope' nahi hai"));
}

TEST_CASE("NAYA.col can only be assigned where it makes sense", "[sql4][trigger]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT, v INT);");
    CHECK_THAT(errorOf(e, "BANAO TRIGGER x BAAD DAALO PAR t SHURU RAKHO NAYA.v = 1; KHATAM;"), ContainsSubstring("sirf PEHLE DAALO / PEHLE BADLO"));
    CHECK_THAT(errorOf(e, "BANAO TRIGGER x PEHLE MITAO PAR t SHURU RAKHO NAYA.v = 1; KHATAM;"), ContainsSubstring("sirf PEHLE DAALO / PEHLE BADLO"));
    CHECK_THAT(errorOf(e, "BANAO TRIGGER x PEHLE BADLO PAR t SHURU RAKHO PURANA.v = 1; KHATAM;"), ContainsSubstring("PURANA ki value badli nahi"));
    CHECK_THAT(errorOf(e, "BANAO TRIGGER x PEHLE BADLO PAR t SHURU RETURN 1; KHATAM;"), ContainsSubstring("RETURN sirf function"));
    CHECK_THAT(errorOf(e, "BANAO PROCEDURE p() SHURU RAKHO NAYA.v = 1; KHATAM;"), ContainsSubstring("sirf PEHLE DAALO / PEHLE BADLO"));
    ok(e, "BANAO TRIGGER ok PEHLE BADLO PAR t SHURU RAKHO NAYA.v = 1; KHATAM;");
    // REPLACE and the upsert write their rows another way: a BEFORE INSERT trigger there can read NAYA but not change it
    ok(e, "HATAO TRIGGER ok;");
    ok(e, "BANAO TRIGGER ok PEHLE DAALO PAR t SHURU RAKHO NAYA.v = 1; KHATAM;");
    CHECK_THAT(errorOf(e, "BADAL DAALO MEIN t MAAN (1, 1);"), ContainsSubstring("REPLACE / TAKRAAV"));
}

TEST_CASE("triggers still run old-style bodies, and now accept control flow and variables", "[sql4][trigger]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT, v INT);");
    ok(e, "BANAO TABLE log (n INT);");
    ok(e, R"(BANAO TRIGGER big BAAD DAALO PAR t SHURU
        DECLARE i INT DEFAULT 0;
        WHILE i < NAYA.v DO
            SET i = i + 1;
            DAALO MEIN log MAAN (i);
        END WHILE;
    KHATAM;)");
    ok(e, "DAALO MEIN t MAAN (1, 3);");
    CHECK(q(e, "DIKHAO n SE log;") == "1;2;3");
    CHECK(q(e, "SHOW TRIGGERS;").find("big|BAAD|DAALO|t|") == 0);
}

// ------------------------------------------------------------ procedures: variables, IF, loops ----

TEST_CASE("existing procedures (plain statement lists) work unchanged", "[sql4][proc]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT MUKHYA KUNJI, v INT);");
    ok(e, "DAALO MEIN t MAAN (1, 10);");
    ok(e, "BANAO PROCEDURE badhao (p_id INT, p_kitna INT) SHURU BADLO t RAKHO v = v + p_kitna JAHAN id = p_id; DIKHAO v SE t; KHATAM;");
    CHECK(run(e, "CHALAO badhao(1, 5);").message == "Procedure 'badhao' chal gaya (2 statement(s)): 1 row(s) badal di; 1 row(s)");
    CHECK(q(e, "DIKHAO v SE t;") == "15");
    CHECK_THAT(errorOf(e, "CHALAO badhao(1);"), ContainsSubstring("2 argument(s) chahiye, 1 mile"));
    CHECK_THAT(errorOf(e, "CHALAO nope();"), ContainsSubstring("exist nahi karta"));
    // a parameter is visible inside a sub-select now too
    ok(e, "BANAO PROCEDURE has (p INT) SHURU DIKHAO id SE t JAHAN v MEIN (DIKHAO v SE t JAHAN id = p); KHATAM;");
    CHECK(run(e, "CHALAO has(1);").message.find("chal gaya (1 statement(s))") != std::string::npos);
    // HATAO PROCEDURE [IF EXISTS]
    ok(e, "HATAO PROCEDURE IF EXISTS nope;");
    ok(e, "HATAO PROCEDURE has;");
    CHECK_THAT(errorOf(e, "HATAO PROCEDURE has;"), ContainsSubstring("exist nahi karta"));
    ok(e, "HATAO PROCEDURE IF EXISTS has;");
}

TEST_CASE("DECLARE, SET and IF / ELSEIF / ELSE, in English and in AGAR / TAB / WARNA", "[sql4][proc]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, R"(BANAO PROCEDURE grade_en (IN m INT, OUT g TEXT) SHURU
        IF m >= 90 THEN SET g = 'A';
        ELSEIF m >= 75 THEN SET g = 'B';
        ELSE SET g = 'C';
        END IF;
    KHATAM;)");
    ok(e, R"(BANAO PROCEDURE grade_hi (m INT, OUT g TEXT) SHURU
        AGAR m >= 90 TAB RAKHO g = 'A';
        WARNA AGAR m >= 75 TAB RAKHO g = 'B';
        WARNA RAKHO g = 'C';
        KHATAM AGAR;
    KHATAM;)");
    for (const char* proc : {"grade_en", "grade_hi"}) {
        INFO(proc);
        CHECK(q(e, std::string("CHALAO ") + proc + "(95, @g);") == "A");
        CHECK(q(e, "DIKHAO @g;") == "A");
        CHECK(q(e, std::string("CHALAO ") + proc + "(80, @g);") == "B");
        CHECK(q(e, std::string("CHALAO ") + proc + "(10, @g);") == "C");
        CHECK(q(e, "DIKHAO @g KAHO grade;") == "C");
    }
    // CHALAO reports the OUT values as a one-row table named after the parameters
    Result r = run(e, "CHALAO grade_en(80, @g);");
    CHECK(r.columns == std::vector<std::string>{"g"});
    CHECK(r.message == "Procedure 'grade_en' chal gaya (0 statement(s)): ");
    // the AGAR ... KHATAM expression is still an expression where an expression is expected, also inside a body
    ok(e, "BANAO PROCEDURE pf (m INT, OUT s TEXT) SHURU SET s = AGAR m >= 50 TAB 'pass' WARNA 'fail' KHATAM; KHATAM;");
    CHECK(q(e, "CHALAO pf(60, @s);") == "pass");
    CHECK(q(e, "CHALAO pf(40, @s);") == "fail");
    CHECK(q(e, "DIKHAO AGAR 1 = 1 TAB 'ek' WARNA 'do' KHATAM;") == "ek");
    // nested IFs and conditions that are KHALI count as false
    ok(e, R"(BANAO PROCEDURE nest (a INT, b INT, OUT r TEXT) SHURU
        SET r = 'none';
        IF a > 0 THEN
            IF b > 0 THEN SET r = 'both'; ELSE SET r = 'a only'; END IF;
        END IF;
        IF a = KHALI THEN SET r = 'never'; END IF;
    KHATAM;)");
    CHECK(q(e, "CHALAO nest(1, 1, @r);") == "both");
    CHECK(q(e, "CHALAO nest(1, -1, @r);") == "a only");
    CHECK(q(e, "CHALAO nest(-1, 1, @r);") == "none");
    // DECLARE with DEFAULT (or WARNA), several names at once, and the type is enforced
    ok(e, R"(BANAO PROCEDURE decl (OUT r TEXT) SHURU
        DECLARE a, b INT DEFAULT 4;
        DECLARE c INT WARNA 10;
        DECLARE d TEXT;
        SET d = 'x';
        SET a = a + c;
        SET r = d || a || b;
    KHATAM;)");
    CHECK(q(e, "CHALAO decl(@r);") == "x144");
    ok(e, "BANAO PROCEDURE typed (OUT r INT) SHURU DECLARE a INT; SET a = 'abc'; SET r = a; KHATAM;");
    CHECK_THAT(errorOf(e, "CHALAO typed(@r);"), ContainsSubstring("INT"));
    ok(e, "BANAO PROCEDURE undeclared () SHURU SET nope = 1; KHATAM;");
    CHECK_THAT(errorOf(e, "CHALAO undeclared();"), ContainsSubstring("Variable 'nope' declare nahi hui"));
}

TEST_CASE("WHILE, JAB TAK, LOOP with LEAVE / ITERATE, REPEAT", "[sql4][proc][loop]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, R"(BANAO PROCEDURE sum_en (IN n INT, OUT total INT) SHURU
        DECLARE i INT DEFAULT 1;
        SET total = 0;
        WHILE i <= n DO
            SET total = total + i;
            SET i = i + 1;
        END WHILE;
    KHATAM;)");
    ok(e, R"(BANAO PROCEDURE sum_hi (IN n INT, OUT total INT) SHURU
        DECLARE i INT DEFAULT 1;
        SET total = 0;
        JAB TAK i <= n KARO
            SET total = total + i;
            SET i = i + 1;
        KHATAM JAB TAK;
    KHATAM;)");
    CHECK(q(e, "CHALAO sum_en(10, @t);") == "55");
    CHECK(q(e, "CHALAO sum_hi(10, @t);") == "55");
    CHECK(q(e, "CHALAO sum_en(0, @t);") == "0");
    ok(e, R"(BANAO PROCEDURE lp (OUT total INT) SHURU
        DECLARE i INT DEFAULT 0;
        SET total = 0;
        lp: LOOP
            SET i = i + 1;
            IF i > 5 THEN LEAVE lp; END IF;
            IF i = 3 THEN ITERATE lp; END IF;
            SET total = total + i;
        END LOOP lp;
    KHATAM;)");
    CHECK(q(e, "CHALAO lp(@t);") == "12");  // 1 + 2 + 4 + 5
    ok(e, R"(BANAO PROCEDURE rp (OUT i INT) SHURU
        SET i = 0;
        REPEAT
            SET i = i + 1;
        UNTIL i >= 4 END REPEAT;
    KHATAM;)");
    CHECK(q(e, "CHALAO rp(@i);") == "4");
    // REPEAT runs the body once even when the condition is already true; ITERATE goes to the UNTIL test
    ok(e, R"(BANAO PROCEDURE rp2 (OUT i INT) SHURU
        SET i = 100;
        r: REPEAT
            SET i = i + 1;
            ITERATE r;
            SET i = 0;
        UNTIL i > 0 END REPEAT r;
    KHATAM;)");
    CHECK(q(e, "CHALAO rp2(@i);") == "101");
    // LEAVE of an outer label from inside an inner loop
    ok(e, R"(BANAO PROCEDURE two (OUT hits INT) SHURU
        DECLARE a INT DEFAULT 0;
        DECLARE b INT DEFAULT 0;
        SET hits = 0;
        outer_l: WHILE a < 5 DO
            SET a = a + 1;
            SET b = 0;
            inner_l: WHILE b < 5 DO
                SET b = b + 1;
                IF b = 3 THEN ITERATE outer_l; END IF;
                IF a = 4 THEN LEAVE outer_l; END IF;
                SET hits = hits + 1;
            END WHILE inner_l;
        END WHILE outer_l;
    KHATAM;)");
    CHECK(q(e, "CHALAO two(@h);") == "6");  // a = 1..3 give 2 hits each, a = 4 leaves at once
    // a label nobody owns
    ok(e, "BANAO PROCEDURE bad_leave () SHURU LEAVE nowhere; KHATAM;");
    CHECK_THAT(errorOf(e, "CHALAO bad_leave();"), ContainsSubstring("label 'nowhere' wala koi loop nahi mila"));
    ok(e, "BANAO PROCEDURE unlabeled () SHURU DECLARE i INT DEFAULT 0; LOOP SET i = i + 1; IF i = 3 THEN LEAVE; END IF; END LOOP; KHATAM;");
    ok(e, "CHALAO unlabeled();");
}

TEST_CASE("a loop that never ends stops at the safety limit", "[sql4][proc][loop]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO PROCEDURE forever () SHURU DECLARE i INT DEFAULT 0; LOOP SET i = i + 1; END LOOP; KHATAM;");
    CHECK_THAT(errorOf(e, "CHALAO forever();"), ContainsSubstring("100000 baar se zyada chal gaya"));
    ok(e, "BANAO PROCEDURE forever2 () SHURU WHILE 1 = 1 DO DIKHAO 1; END WHILE; KHATAM;");
    CHECK_THAT(errorOf(e, "CHALAO forever2();"), ContainsSubstring("safety limit"));
    ok(e, "BANAO PROCEDURE forever3 () SHURU REPEAT DECLARE x INT; UNTIL 1 = 2 END REPEAT; KHATAM;");
    CHECK_THAT(errorOf(e, "CHALAO forever3();"), ContainsSubstring("safety limit"));
}

TEST_CASE("SELECT ... INTO, rows into variables", "[sql4][proc][into]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT MUKHYA KUNJI, naam TEXT, v INT);");
    ok(e, "DAALO MEIN t MAAN (1, 'a', 10), (2, 'b', 20), (3, 'c', 30);");
    ok(e, "BANAO PROCEDURE cnt (OUT c INT, OUT s INT) SHURU DIKHAO GINO(*), KUL(v) INTO c, s SE t; KHATAM;");
    CHECK(q(e, "CHALAO cnt(@c, @s);") == "3|60");
    ok(e, "BANAO PROCEDURE nm (IN p INT, OUT n TEXT) SHURU DIKHAO naam SE t JAHAN id = p INTO n; KHATAM;");
    CHECK(q(e, "CHALAO nm(2, @n);") == "b");
    ok(e, "BANAO PROCEDURE nm2 (IN p INT, OUT n TEXT, OUT x INT) SHURU DIKHAO naam, v INTO n, x SE t JAHAN id = p; KHATAM;");
    CHECK(q(e, "CHALAO nm2(3, @n, @x);") == "c|30");
    ok(e, "BANAO PROCEDURE nm3 (IN p INT, OUT n TEXT) SHURU DIKHAO naam SE t JAHAN id = p INTO n; KHATAM;");
    CHECK(q(e, "CHALAO nm3(1, @n);") == "a");
    // the Hinglish word order is the ordinary one
    ok(e, "BANAO PROCEDURE hi (OUT n INT) SHURU DIKHAO ADHIKTAM(v) INTO n SE t; KHATAM;");
    CHECK(q(e, "CHALAO hi(@n);") == "30");
    // no rows, too many rows, wrong column count: errors like MySQL's
    CHECK_THAT(errorOf(e, "CHALAO nm(99, @n);"), ContainsSubstring("koi row nahi mili"));
    ok(e, "BANAO PROCEDURE many (OUT n TEXT) SHURU DIKHAO naam INTO n SE t; KHATAM;");
    CHECK_THAT(errorOf(e, "CHALAO many(@n);"), ContainsSubstring("ek se zyada rows mili (3)"));
    ok(e, "BANAO PROCEDURE cols (OUT n TEXT) SHURU DIKHAO naam, v INTO n SE t JAHAN id = 1; KHATAM;");
    CHECK_THAT(errorOf(e, "CHALAO cols(@n);"), ContainsSubstring("1 variable hain par query 2 column deti hai"));
    // a plain DIKHAO in a body (no INTO) still runs; a local variable can be used in the query
    ok(e, "BANAO PROCEDURE plain (IN p INT) SHURU DECLARE x INT; DIKHAO v INTO x SE t JAHAN id = p; DIKHAO x + 1; KHATAM;");
    CHECK(run(e, "CHALAO plain(2);").message.find("(2 statement(s))") != std::string::npos);
}

TEST_CASE("OUT and INOUT parameters, @user variables", "[sql4][proc][out]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO PROCEDURE dbl (INOUT x INT) SHURU SET x = x * 2; KHATAM;");
    ok(e, "SET @y = 5;");
    CHECK(q(e, "CHALAO dbl(@y);") == "10");
    CHECK(q(e, "DIKHAO @y;") == "10");
    CHECK(q(e, "CHALAO dbl(@y);") == "20");
    // IN, OUT and INOUT together; OUT starts as KHALI
    ok(e, "BANAO PROCEDURE mix (IN a INT, OUT b INT, INOUT c INT) SHURU SET b = a + c; SET c = c + 1; KHATAM;");
    ok(e, "SET @c = 100;");
    Result r = run(e, "CHALAO mix(1, @b, @c);");
    CHECK(r.columns == std::vector<std::string>{"b", "c"});
    CHECK(flat(r) == "101|101");
    CHECK(q(e, "DIKHAO @b, @c;") == "101|101");
    ok(e, "BANAO PROCEDURE only_out (OUT o INT) SHURU SET o = o; KHATAM;");
    CHECK(q(e, "CHALAO only_out(@o);") == "KHALI");
    // an expression is fine for IN, but OUT / INOUT need a variable
    CHECK(q(e, "SET @base = 10; CHALAO mix(@base * 2, @b, @c);") == "121|102");
    CHECK_THAT(errorOf(e, "CHALAO dbl(5);"), ContainsSubstring("INOUT parameter 'x' ke liye @variable"));
    CHECK_THAT(errorOf(e, "CHALAO only_out(nosuch);"), ContainsSubstring("OUT parameter 'o' ke liye @variable"));
    // a parameter called `in` is not mistaken for a mode word
    ok(e, "BANAO PROCEDURE odd (in INT, out INT) SHURU DIKHAO in + out; KHATAM;");
    CHECK(run(e, "CHALAO odd(1, 2);").message.find("chal gaya (1 statement(s))") != std::string::npos);
    // a procedure calling a procedure hands a local variable to an INOUT parameter
    ok(e, "BANAO PROCEDURE outer_p (OUT r INT) SHURU DECLARE tmp INT DEFAULT 3; CHALAO dbl(tmp); CHALAO dbl(tmp); SET r = tmp; KHATAM;");
    CHECK(q(e, "CHALAO outer_p(@r);") == "12");
    // user variables: expressions, WHERE, VALUES, KHALI when unset, strings
    ok(e, "BANAO TABLE t (id INT, naam TEXT);");
    ok(e, "SET @id = 7, @naam = 'sat';");
    ok(e, "DAALO MEIN t MAAN (@id, @naam), (@id + 1, @naam || '+1');");
    CHECK(q(e, "DIKHAO * SE t JAHAN id = @id;") == "7|sat");
    CHECK(q(e, "DIKHAO naam SE t JAHAN id > @id;") == "sat+1");
    CHECK(q(e, "DIKHAO @unset;") == "KHALI");
    CHECK(q(e, "DIKHAO @unset HAI KHALI KAHO n;") == "SACH");
    ok(e, "BADLO t RAKHO naam = @naam || '!' JAHAN id = @id;");
    ok(e, "SET @cnt = (DIKHAO GINO(*) SE t);");
    CHECK(q(e, "DIKHAO @cnt;") == "2");
    CHECK(q(e, "DIKHAO id SE t JAHAN id = (DIKHAO ADHIKTAM(id) SE t) AUR id > @id - 1;") == "8");
    CHECK(q(e, "DIKHAO id SE t JAHAN id MEIN (@id, 99) KRAM id;") == "7");
}

TEST_CASE("procedures survive a restart with their parameter modes; old catalogs load", "[sql4][proc][persist]") {
    TempDir dir;
    {
        Engine e(dir.str());
        ok(e, "BANAO PROCEDURE m (IN a INT, OUT b INT, INOUT c INT) SHURU SET b = a; SET c = c + 1; KHATAM;");
        ok(e, "BANAO PROCEDURE plain (a INT) SHURU DIKHAO a; KHATAM;");
    }
    std::string catalog = slurp(dir.file("main/catalog.json"));
    CHECK_THAT(catalog, ContainsSubstring("\"OUT\""));
    CHECK_THAT(catalog, ContainsSubstring("\"INOUT\""));
    {
        Engine again(dir.str());
        CHECK(q(again, "SET @c = 1; CHALAO m(5, @b, @c);") == "5|2");
        CHECK(q(again, "DIKHAO @b + @c;") == "7");
    }
    // a procedure written by an older build / the Python engine has two-element parameters and no modes
    std::string old = R"({"tables": {}, "views": {}, "triggers": {}, "procedures": {"p": {"params": [["a", "INT"]], "body_text": "DIKHAO a + 1;"}}})";
    TempDir dir2;
    std::filesystem::create_directories(std::filesystem::u8path(dir2.file("main")));
    {
        std::ofstream f(dir2.file("main/catalog.json"), std::ios::binary);
        f << old;
    }
    Engine legacy(dir2.str());
    CHECK(run(legacy, "CHALAO p(1);").message == "Procedure 'p' chal gaya (1 statement(s)): 1 row(s)");
    CHECK(slurp(dir2.file("main/catalog.json")).find("functions") == std::string::npos);  // nothing new written unless used
    ok(legacy, "BANAO FUNCTION f() RETURNS INT SHURU RETURN 1; KHATAM;");
    CHECK_THAT(slurp(dir2.file("main/catalog.json")), ContainsSubstring("\"functions\""));
}

// ------------------------------------------------------------ functions ----

TEST_CASE("stored functions: BANAO FUNCTION, use in expressions, SHOW FUNCTION STATUS, HATAO FUNCTION", "[sql4][function]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT MUKHYA KUNJI, salary INT);");
    ok(e, "DAALO MEIN t MAAN (1, 1000), (2, 2000), (3, 3000);");
    CHECK(run(e, "BANAO FUNCTION bonus (s INT, pct INT) RETURNS INT SHURU RETURN s * pct DIV 100; KHATAM;").message ==
          "Function 'bonus' ban gaya (2 parameter(s), RETURNS INT)");
    CHECK(q(e, "DIKHAO bonus(salary, 10) KAHO b SE t KRAM id;") == "100;200;300");
    CHECK(q(e, "DIKHAO id SE t JAHAN bonus(salary, 10) >= 200 KRAM id;") == "2;3");
    CHECK(q(e, "DIKHAO KUL(bonus(salary, 5)) SE t;") == "300");
    CHECK(q(e, "DIKHAO bonus(1000, 10) + bonus(500, 10);") == "150");
    CHECK(q(e, "DIKHAO id, bonus(salary, 10) SE t KRAM bonus(salary, 10) ULTA;").find("3|300") == 0);
    ok(e, "BADLO t RAKHO salary = salary + bonus(salary, 10) JAHAN id = 1;");
    CHECK(q(e, "DIKHAO salary SE t JAHAN id = 1;") == "1100");
    ok(e, "DAALO MEIN t MAAN (4, bonus(1000, 50));");
    CHECK(q(e, "DIKHAO salary SE t JAHAN id = 4;") == "500");
    // header text for an expression column
    CHECK(run(e, "DIKHAO bonus(salary, 10) SE t;").columns == std::vector<std::string>{"bonus(salary, 10)"});
    // arguments are coerced to the parameter types, arity is checked, a NULL goes through
    CHECK(q(e, "DIKHAO bonus('1000', 10);") == "100");
    CHECK_THAT(errorOf(e, "DIKHAO bonus(1);"), ContainsSubstring("2 argument(s) chahiye, 1 mile"));
    CHECK(q(e, "DIKHAO bonus(KHALI, 10);") == "KHALI");
    // listing and dropping
    Result status = run(e, "SHOW FUNCTION STATUS;");
    CHECK(status.columns == std::vector<std::string>{"name", "database", "returns", "created"});
    REQUIRE(status.rows.size() == 1);
    CHECK(flat(status).find("bonus|main|INT|") == 0);
    CHECK_THAT(errorOf(e, "BANAO FUNCTION bonus () RETURNS INT SHURU RETURN 1; KHATAM;"), ContainsSubstring("pehle se hai"));
    CHECK_THAT(errorOf(e, "BANAO FUNCTION upper (x TEXT) RETURNS TEXT SHURU RETURN x; KHATAM;"), ContainsSubstring("built-in function"));
    ok(e, "HATAO FUNCTION bonus;");
    CHECK_THAT(errorOf(e, "HATAO FUNCTION bonus;"), ContainsSubstring("exist nahi karta"));
    ok(e, "HATAO FUNCTION IF EXISTS bonus;");
    CHECK_THAT(errorOf(e, "DIKHAO bonus(1, 2);"), ContainsSubstring("Function 'BONUS' nahi pata. Ye chalte hain:"));
    CHECK(q(e, "SHOW FUNCTION STATUS;").empty());
}

TEST_CASE("an unknown function is still the old error, built-ins win, and a stored function can use them", "[sql4][function]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT);");
    // exactly the old text, even with no rows to evaluate it on
    CHECK_THAT(errorOf(e, "DIKHAO FOO(id) SE t;"), ContainsSubstring("Function 'FOO' nahi pata. Ye chalte hain: GINO, KUL, AUSAT"));
    CHECK_THAT(errorOf(e, "DIKHAO FOO(1, 2);"), ContainsSubstring("Function 'FOO' nahi pata"));
    CHECK_THAT(errorOf(e, "DIKHAO FOO();"), ContainsSubstring("Function 'FOO' nahi pata"));
    ok(e, "BANAO FUNCTION shout (s TEXT) RETURNS TEXT SHURU RETURN UPPER(s) || '!'; KHATAM;");
    CHECK(q(e, "DIKHAO shout('hi');") == "HI!");
    CHECK(q(e, "DIKHAO SHOUT('hi');") == "HI!");  // identifiers are case-insensitive
    ok(e, "BANAO FUNCTION twice (s TEXT) RETURNS TEXT SHURU RETURN shout(shout(s)); KHATAM;");  // functions call functions
    CHECK(q(e, "DIKHAO twice('a');") == "A!!");
}

TEST_CASE("function bodies: variables, IF, loops, SELECT INTO, cursors, recursion", "[sql4][function]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT MUKHYA KUNJI, v INT);");
    ok(e, "DAALO MEIN t MAAN (1, 10), (2, 20), (3, 30);");
    ok(e, R"(BANAO FUNCTION fact (n INT) RETURNS INT SHURU
        IF n <= 1 THEN RETURN 1; END IF;
        RETURN n * fact(n - 1);
    KHATAM;)");
    CHECK(q(e, "DIKHAO fact(5), fact(10);") == "120|3628800");
    ok(e, R"(BANAO FUNCTION sum_to (n INT) RETURNS INT NOT DETERMINISTIC SHURU
        DECLARE i INT DEFAULT 0;
        DECLARE s INT DEFAULT 0;
        JAB TAK i < n KARO SET i = i + 1; SET s = s + i; KHATAM JAB TAK;
        RETURN s;
    KHATAM;)");
    CHECK(q(e, "DIKHAO sum_to(100);") == "5050");
    ok(e, "BANAO FUNCTION total () RETURNS INT DETERMINISTIC SHURU DECLARE s INT; DIKHAO KUL(v) INTO s SE t; RETURN s; KHATAM;");
    CHECK(q(e, "DIKHAO total();") == "60");
    ok(e, "DAALO MEIN t MAAN (4, 40);");
    CHECK(q(e, "DIKHAO total();") == "100");  // it reads the table each time
    ok(e, "BANAO FUNCTION v_of (p INT) RETURNS INT SHURU RETURN (DIKHAO v SE t JAHAN id = p); KHATAM;");
    CHECK(q(e, "DIKHAO id, v_of(id) SE t KRAM id;") == "1|10;2|20;3|30;4|40");
    ok(e, R"(BANAO FUNCTION cur_sum () RETURNS INT SHURU
        DECLARE done INT DEFAULT 0;
        DECLARE x INT;
        DECLARE s INT DEFAULT 0;
        DECLARE c CURSOR FOR DIKHAO v SE t;
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;
        OPEN c;
        l: LOOP
            FETCH c INTO x;
            IF done = 1 THEN LEAVE l; END IF;
            SET s = s + x;
        END LOOP l;
        CLOSE c;
        RETURN s;
    KHATAM;)");
    CHECK(q(e, "DIKHAO cur_sum();") == "100");
    // the return value is coerced to the declared type; running off the end is an error
    ok(e, "BANAO FUNCTION as_text () RETURNS TEXT SHURU RETURN 5; KHATAM;");
    CHECK(q(e, "DIKHAO as_text() || 'x';") == "5x");
    ok(e, "BANAO FUNCTION no_return () RETURNS INT SHURU DECLARE a INT; SET a = 1; KHATAM;");
    CHECK_THAT(errorOf(e, "DIKHAO no_return();"), ContainsSubstring("ne RETURN nahi kiya"));
    // recursion has a limit
    ok(e, "BANAO FUNCTION forever (n INT) RETURNS INT SHURU RETURN forever(n + 1); KHATAM;");
    CHECK_THAT(errorOf(e, "DIKHAO forever(1);"), ContainsSubstring("limit"));
    CHECK(q(e, "DIKHAO fact(5);") == "120");  // the session is still fine afterwards
    // a function inside a procedure body
    ok(e, "BANAO PROCEDURE pf (OUT r INT) SHURU SET r = fact(4) + total(); KHATAM;");
    CHECK(q(e, "CHALAO pf(@r);") == "124");
}

TEST_CASE("a function may read but not change anything", "[sql4][function]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT);");
    CHECK_THAT(errorOf(e, "BANAO FUNCTION f1 () RETURNS INT SHURU DAALO MEIN t MAAN (1); RETURN 1; KHATAM;"),
               ContainsSubstring("sirf padhne wali statements"));
    CHECK_THAT(errorOf(e, "BANAO FUNCTION f2 () RETURNS INT SHURU BANAO TABLE x (a INT); RETURN 1; KHATAM;"),
               ContainsSubstring("sirf padhne wali statements"));
    CHECK_THAT(errorOf(e, "BANAO FUNCTION f3 () RETURNS INT SHURU CHALAO p(); RETURN 1; KHATAM;"), ContainsSubstring("sirf padhne wali statements"));
    CHECK_THAT(errorOf(e, "BANAO FUNCTION f4 () RETURNS INT SHURU SET @x = 1; RETURN 1; KHATAM;"), ContainsSubstring("@variable"));
    CHECK_THAT(errorOf(e, "BANAO FUNCTION f5 () RETURNS INT SHURU SHURU; RETURN 1; KHATAM;"), ContainsSubstring("sirf padhne wali statements"));
    ok(e, "BANAO FUNCTION reads () RETURNS INT SHURU DIKHAO * SE t; RETURN 1; KHATAM;");
    CHECK(q(e, "DIKHAO reads();") == "1");
    // a procedure may do anything, but a function it is used by may not
    ok(e, "BANAO PROCEDURE writer () SHURU DAALO MEIN t MAAN (9); KHATAM;");
    CHECK(q(e, "DIKHAO GINO(*) SE t;") == "0");
    CHECK(q(e, "DIKHAO reads() KAHO x;") == "1");
    // a trigger fired by a function cannot happen: no function can write. RETURN is only for functions (refused when created, like MySQL)
    CHECK_THAT(errorOf(e, "BANAO PROCEDURE bad_ret () SHURU RETURN 1; KHATAM;"), ContainsSubstring("RETURN sirf function"));
}

TEST_CASE("functions are part of the catalog: saved, loaded, undone by WAPAS", "[sql4][function][persist]") {
    TempDir dir;
    {
        Engine e(dir.str());
        ok(e, "BANAO FUNCTION f (a INT) RETURNS INT SHURU RETURN a + 1; KHATAM;");
        ok(e, "SHURU; BANAO FUNCTION g () RETURNS INT SHURU RETURN 2; KHATAM;");
        CHECK(q(e, "DIKHAO g();") == "2");
        ok(e, "WAPAS;");
        CHECK_THAT(errorOf(e, "DIKHAO g();"), ContainsSubstring("nahi pata"));
    }
    Engine again(dir.str());
    CHECK(q(again, "DIKHAO f(1);") == "2");
    CHECK_THAT(slurp(dir.file("main/catalog.json")), ContainsSubstring("\"functions\""));
    // another database has its own functions
    ok(again, "BANAO DATABASE other; ISTEMAL other;");
    CHECK_THAT(errorOf(again, "DIKHAO f(1);"), ContainsSubstring("nahi pata"));
    ok(again, "BANAO FUNCTION f (a INT) RETURNS INT SHURU RETURN a + 100; KHATAM;");
    CHECK(q(again, "DIKHAO f(1);") == "101");
    CHECK(run(again, "SHOW FUNCTION STATUS;").rows.size() == 2);  // every database, like SHOW PROCEDURE STATUS
}

// ------------------------------------------------------------ cursors ----

TEST_CASE("cursors: DECLARE, OPEN, FETCH, CLOSE and the NOT FOUND handler", "[sql4][cursor]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT MUKHYA KUNJI, naam TEXT, v INT);");
    ok(e, "DAALO MEIN t MAAN (1, 'a', 10), (2, 'b', 20), (3, 'c', 30);");
    ok(e, "BANAO TABLE out (naam TEXT, v INT);");
    ok(e, R"(BANAO PROCEDURE copy_big (IN minv INT, OUT n INT) SHURU
        DECLARE done INT DEFAULT 0;
        DECLARE nm TEXT;
        DECLARE x INT;
        DECLARE c CURSOR FOR DIKHAO naam, v SE t JAHAN v >= minv KRAM id;
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;
        SET n = 0;
        OPEN c;
        rd: LOOP
            FETCH c INTO nm, x;
            IF done = 1 THEN LEAVE rd; END IF;
            DAALO MEIN out MAAN (nm, x * 2);
            SET n = n + 1;
        END LOOP rd;
        CLOSE c;
    KHATAM;)");
    CHECK(q(e, "CHALAO copy_big(15, @n);") == "2");
    CHECK(q(e, "DIKHAO * SE out KRAM v;") == "b|40;c|60");
    // the cursor holds the rows as they were at OPEN
    ok(e, "BANAO PROCEDURE snap (OUT s INT) SHURU DECLARE c CURSOR FOR DIKHAO v SE t; DECLARE x INT; SET s = 0; OPEN c; MITAO SE t; FETCH c INTO x; SET s = x; CLOSE c; KHATAM;");
    CHECK(q(e, "CHALAO snap(@s);") != "KHALI");
    CHECK(q(e, "DIKHAO GINO(*) SE t;") == "0");
    // errors: not declared, not open, opened twice, past the end with no handler, wrong number of variables
    ok(e, "DAALO MEIN t MAAN (1, 'a', 10);");
    ok(e, "BANAO PROCEDURE e1 () SHURU OPEN nope; KHATAM;");
    CHECK_THAT(errorOf(e, "CHALAO e1();"), ContainsSubstring("Cursor 'nope' declare nahi hua"));
    ok(e, "BANAO PROCEDURE e2 () SHURU DECLARE c CURSOR FOR DIKHAO v SE t; DECLARE x INT; FETCH c INTO x; KHATAM;");
    CHECK_THAT(errorOf(e, "CHALAO e2();"), ContainsSubstring("khula nahi hai"));
    ok(e, "BANAO PROCEDURE e3 () SHURU DECLARE c CURSOR FOR DIKHAO v SE t; OPEN c; OPEN c; KHATAM;");
    CHECK_THAT(errorOf(e, "CHALAO e3();"), ContainsSubstring("pehle se khula hai"));
    ok(e, "BANAO PROCEDURE e4 () SHURU DECLARE c CURSOR FOR DIKHAO v SE t; DECLARE x INT; OPEN c; FETCH c INTO x; FETCH c INTO x; KHATAM;");
    CHECK_THAT(errorOf(e, "CHALAO e4();"), ContainsSubstring("aur rows nahi hain (NOT FOUND)"));
    ok(e, "BANAO PROCEDURE e5 () SHURU DECLARE c CURSOR FOR DIKHAO v SE t; DECLARE x INT; DECLARE y INT; OPEN c; FETCH c INTO x, y; KHATAM;");
    CHECK_THAT(errorOf(e, "CHALAO e5();"), ContainsSubstring("2 variable hain par cursor 1 column deta hai"));
    ok(e, "BANAO PROCEDURE e6 () SHURU DECLARE c CURSOR FOR DIKHAO v SE t; CLOSE c; KHATAM;");
    CHECK_THAT(errorOf(e, "CHALAO e6();"), ContainsSubstring("khula hi nahi hai"));
    // a cursor can be reopened after CLOSE and sees the current rows; the handler also covers SELECT .. INTO
    ok(e, R"(BANAO PROCEDURE twice (OUT s INT) SHURU
        DECLARE c CURSOR FOR DIKHAO v SE t;
        DECLARE x INT;
        SET s = 0;
        OPEN c; FETCH c INTO x; SET s = s + x; CLOSE c;
        OPEN c; FETCH c INTO x; SET s = s + x; CLOSE c;
    KHATAM;)");
    CHECK(q(e, "CHALAO twice(@s);") == "20");
    ok(e, R"(BANAO PROCEDURE none_ok (OUT r TEXT) SHURU
        DECLARE found INT DEFAULT 1;
        DECLARE x INT;
        DECLARE CONTINUE HANDLER FOR NOT FOUND SET found = 0;
        DIKHAO v INTO x SE t JAHAN id = 999;
        SET r = AGAR found = 0 TAB 'koi nahi' WARNA 'mila' KHATAM;
    KHATAM;)");
    CHECK(q(e, "CHALAO none_ok(@r);") == "koi nahi");
}

// ------------------------------------------------------------ prepared statements ----

TEST_CASE("PREPARE, EXECUTE ... USING, DEALLOCATE PREPARE", "[sql4][prepare]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT MUKHYA KUNJI, naam TEXT, v FLOAT);");
    ok(e, "DAALO MEIN t MAAN (1, 'a', 1.5), (2, 'b', 2.5), (3, 'c', 3.5);");
    CHECK_THAT(run(e, "PREPARE s1 FROM 'DIKHAO naam SE t JAHAN id = ?';").message, ContainsSubstring("1 ? placeholder"));
    ok(e, "SET @id = 2;");
    CHECK(q(e, "EXECUTE s1 USING @id;") == "b");
    CHECK(q(e, "EXECUTE s1 USING 3;") == "c");
    CHECK(q(e, "EXECUTE s1 USING @id + 1;") == "c");
    ok(e, "PREPARE s2 FROM 'DIKHAO id SE t JAHAN id >= ? AUR v < ? KRAM id';");
    CHECK(q(e, "EXECUTE s2 USING 2, 3.0;") == "2");
    CHECK(q(e, "EXECUTE s2 USING 1, 99.5;") == "1;2;3");
    CHECK(q(e, "EXECUTE s2 USING -5, 2;") == "1");
    // wrong number of values / unknown name
    CHECK_THAT(errorOf(e, "EXECUTE s2 USING 1;"), ContainsSubstring("2 ? hain par USING mein 1 value(s)"));
    CHECK_THAT(errorOf(e, "EXECUTE s1;"), ContainsSubstring("1 ? hain par USING mein 0 value(s)"));
    CHECK_THAT(errorOf(e, "EXECUTE nope;"), ContainsSubstring("exist nahi karta"));
    // text goes in as a literal: quotes and SQL in a value do nothing special
    ok(e, "PREPARE ins FROM 'DAALO MEIN t MAAN (?, ?, ?)';");
    ok(e, "SET @n = 'O''Brien';");
    ok(e, "EXECUTE ins USING 10, @n, 4.25;");
    ok(e, "SET @evil = 'x'');MITAO SE t;--';");
    ok(e, "EXECUTE ins USING 11, @evil, KHALI;");
    CHECK(q(e, "DIKHAO naam, v SE t JAHAN id >= 10 KRAM id;") == "O'Brien|4.25;x');MITAO SE t;--|KHALI");
    CHECK(q(e, "DIKHAO GINO(*) SE t;") == "5");
    // a ? inside a string is not a placeholder
    ok(e, "PREPARE q FROM 'DIKHAO ''what?'' KAHO w';");
    CHECK(q(e, "EXECUTE q;") == "what?");
    // PREPARE FROM a variable; PREPARE again replaces; the SQL is checked at PREPARE time
    ok(e, "SET @sql = 'DIKHAO GINO(*) SE t JAHAN id > ?';");
    ok(e, "PREPARE c FROM @sql;");
    CHECK(q(e, "EXECUTE c USING 2;") == "3");
    ok(e, "PREPARE c FROM 'DIKHAO 42 KAHO answer';");
    CHECK(q(e, "EXECUTE c;") == "42");
    CHECK_THAT(errorOf(e, "PREPARE bad FROM 'DIKHAO SE';"), ContainsSubstring("expected tha"));
    CHECK_THAT(errorOf(e, "PREPARE bad FROM 'DIKHAO 1; DIKHAO 2';"), ContainsSubstring("bilkul ek statement"));
    CHECK_THAT(errorOf(e, "PREPARE bad FROM 5;"), ContainsSubstring("SQL text"));
    CHECK_THAT(errorOf(e, "EXECUTE bad;"), ContainsSubstring("exist nahi karta"));
    // a number for a number position
    ok(e, "PREPARE lim FROM 'DIKHAO id SE t KRAM id SIRF ?';");
    CHECK(q(e, "EXECUTE lim USING 2;") == "1;2");
    // DDL / DML through EXECUTE; DEALLOCATE and its other spelling
    ok(e, "PREPARE mk FROM 'BANAO TABLE made (a INT)';");
    ok(e, "EXECUTE mk;");
    CHECK(q(e, "DIKHAO GINO(*) SE made;") == "0");
    ok(e, "DEALLOCATE PREPARE mk;");
    CHECK_THAT(errorOf(e, "EXECUTE mk;"), ContainsSubstring("exist nahi karta"));
    CHECK_THAT(errorOf(e, "DEALLOCATE PREPARE mk;"), ContainsSubstring("exist nahi karta"));
    ok(e, "HATAO PREPARE c;");
    CHECK_THAT(errorOf(e, "EXECUTE c;"), ContainsSubstring("exist nahi karta"));
    // a session of its own: a second Engine does not see it
    Engine other(dir.str());
    CHECK_THAT(errorOf(other, "EXECUTE s1 USING 1;"), ContainsSubstring("exist nahi karta"));
}

TEST_CASE("prepared statements and variables inside procedure bodies", "[sql4][prepare][proc]") {
    TempDir dir;
    Engine e(dir.str());
    ok(e, "BANAO TABLE t (id INT, naam TEXT);");
    ok(e, R"(BANAO PROCEDURE addrow (IN i INT, IN n TEXT) SHURU
        PREPARE p FROM 'DAALO MEIN t MAAN (?, ?)';
        EXECUTE p USING i, n;
        EXECUTE p USING i + 1, n || '2';
        DEALLOCATE PREPARE p;
    KHATAM;)");
    ok(e, "CHALAO addrow(1, 'x');");
    CHECK(q(e, "DIKHAO * SE t KRAM id;") == "1|x;2|x2");
    CHECK_THAT(errorOf(e, "EXECUTE p USING 1, 'a';"), ContainsSubstring("exist nahi karta"));
    // dynamic table name: the SQL text is built in the body
    ok(e, R"(BANAO PROCEDURE count_in (IN tbl TEXT, OUT n INT) SHURU
        DECLARE sql_text TEXT;
        SET sql_text = 'DIKHAO GINO(*) SE ' || tbl;
        PREPARE cnt FROM sql_text;
        EXECUTE cnt;
        DEALLOCATE PREPARE cnt;
        SET n = 0;
    KHATAM;)");
    CHECK_THAT(run(e, "CHALAO count_in('t', @n);").message, ContainsSubstring("chal gaya (3 statement(s))"));
    // @variables are the session's: a procedure sets one, the caller reads it
    ok(e, "BANAO PROCEDURE setter () SHURU SET @from_proc = 'hello'; SET @n = @n + 1; KHATAM;");
    ok(e, "SET @n = 41;");
    ok(e, "CHALAO setter();");
    CHECK(q(e, "DIKHAO @from_proc, @n;") == "hello|42");
}

// ------------------------------------------------------------ privileges ----

TEST_CASE("bodies run with the caller's own privileges; functions need DIKHAO on what they read", "[sql4][privileges]") {
    TempDir dir;
    auto inst = std::make_shared<Instance>(dir.str());
    Engine admin(inst), ravi(inst);
    ravi.user = "ravi";
    ok(admin, "BANAO USER ravi GUPT 'pw';");
    ok(admin, "BANAO TABLE t (id INT, v INT); BANAO TABLE secret (x INT);");
    ok(admin, "DAALO MEIN t MAAN (1, 10); DAALO MEIN secret MAAN (42);");
    ok(admin, "BANAO FUNCTION peek () RETURNS INT SHURU DECLARE r INT; DIKHAO x INTO r SE secret; RETURN r; KHATAM;");
    ok(admin, "BANAO FUNCTION plain (a INT) RETURNS INT SHURU RETURN a + 1; KHATAM;");
    ok(admin, "ADHIKAR DO DIKHAO PAR t KO ravi;");
    CHECK(q(ravi, "DIKHAO plain(v) SE t;") == "11");
    CHECK_THAT(errorOf(ravi, "DIKHAO peek() SE t;"), ContainsSubstring("adhikar nahi hai"));  // no DIKHAO on secret
    ok(admin, "ADHIKAR DO DIKHAO PAR secret KO ravi;");
    CHECK(q(ravi, "DIKHAO peek() SE t;") == "42");
    // a restricted user cannot make functions, procedures or savepoints (admin commands / transactions)
    CHECK_THAT(errorOf(ravi, "BANAO FUNCTION z () RETURNS INT SHURU RETURN 1; KHATAM;"), ContainsSubstring("superuser nahi hai"));
    CHECK_THAT(errorOf(ravi, "SAVEPOINT a;"), ContainsSubstring("superuser nahi hai"));
    CHECK_THAT(errorOf(ravi, "SET autocommit = 0;"), ContainsSubstring("superuser nahi hai"));
    // but session variables and prepared statements (which check the statement they run) are open
    ok(ravi, "SET @a = 1;");
    ok(ravi, "PREPARE s FROM 'DIKHAO v SE t JAHAN id = ?';");
    CHECK(q(ravi, "EXECUTE s USING @a;") == "10");
    ok(ravi, "PREPARE w FROM 'DAALO MEIN t MAAN (?, ?)';");
    CHECK_THAT(errorOf(ravi, "EXECUTE w USING 5, 5;"), ContainsSubstring("DAALO ka adhikar nahi hai"));
}

// ------------------------------------------------------------ parser ----

TEST_CASE("the batch 4 statements parse; bad ones are parse errors", "[sql4][parser]") {
    CHECK(parseScript("SET autocommit = 0; SET @a = 1, @b = 2; SAVEPOINT x; ROLLBACK TO x; ROLLBACK TO SAVEPOINT x; RELEASE SAVEPOINT x;").size() == 6);
    CHECK(parseScript("WAPAS TAK x; WAPAS TO x; WAPAS TAK SAVEPOINT x; WAPAS; PAKKA; SHURU;").size() == 6);
    CHECK(parseScript("PREPARE s FROM 'x ?'; EXECUTE s; EXECUTE s USING @a, 1; DEALLOCATE PREPARE s; HATAO PREPARE s;").size() == 5);
    CHECK(parseScript("BANAO FUNCTION f (a INT) RETURNS INT SHURU RETURN a; KHATAM; HATAO FUNCTION f; HATAO FUNCTION IF EXISTS f; SHOW FUNCTION STATUS;").size() == 4);
    CHECK(parseScript("BANAO TRIGGER IF NOT EXISTS t PEHLE DAALO PAR x SHURU DIKHAO 1; KHATAM; HATAO TRIGGER IF EXISTS t;").size() == 2);
    CHECK(parseScript("HATAO PROCEDURE IF EXISTS p; SHOW VARIABLES; SHOW VARIABLES LIKE 'a%';").size() == 3);
    CHECK_THROWS_AS(parseScript("SAVEPOINT;"), ParseError);
    CHECK_THROWS_AS(parseScript("RELEASE a;"), ParseError);
    CHECK_THROWS_AS(parseScript("DEALLOCATE s;"), ParseError);
    CHECK_THROWS_AS(parseScript("PREPARE s 'x';"), ParseError);
    CHECK_THROWS_AS(parseScript("SET;"), ParseError);
    CHECK_THROWS_AS(parseScript("SHOW FUNCTION;"), ParseError);
    CHECK_THROWS_AS(parseScript("BANAO FUNCTION f () SHURU RETURN 1; KHATAM;"), ParseError);  // RETURNS missing
    CHECK_THROWS_AS(parseScript("BANAO FUNCTION f () RETURNS INT SHURU KHATAM;"), ParseError);  // empty body
    // bodies: every construct must be closed
    const char* const broken[] = {
        "BANAO PROCEDURE p () SHURU IF 1 = 1 THEN DIKHAO 1; KHATAM;",
        "BANAO PROCEDURE p () SHURU IF 1 = 1 DIKHAO 1; END IF; KHATAM;",
        "BANAO PROCEDURE p () SHURU IF 1 = 1 THEN DIKHAO 1; KHATAM;",
        "BANAO PROCEDURE p () SHURU WHILE 1 = 1 DIKHAO 1; END WHILE; KHATAM;",
        "BANAO PROCEDURE p () SHURU WHILE 1 = 1 DO DIKHAO 1; END IF; KHATAM;",
        "BANAO PROCEDURE p () SHURU LOOP DIKHAO 1; KHATAM;",
        "BANAO PROCEDURE p () SHURU REPEAT DIKHAO 1; END REPEAT; KHATAM;",
        "BANAO PROCEDURE p () SHURU x: DIKHAO 1; KHATAM;",
        "BANAO PROCEDURE p () SHURU DECLARE a; KHATAM;",
        "BANAO PROCEDURE p () SHURU DECLARE a FOO; KHATAM;",
        "BANAO PROCEDURE p () SHURU DECLARE CONTINUE HANDLER FOR SQLEXCEPTION SET a = 1; KHATAM;",
        "BANAO PROCEDURE p () SHURU FETCH c; KHATAM;",
        "BANAO PROCEDURE p () SHURU DIKHAO 1 INTO; KHATAM;",
        "BANAO PROCEDURE p () SHURU ELSE DIKHAO 1; KHATAM;",
        "BANAO PROCEDURE p () SHURU DIKHAO 1 KHATAM;",
    };
    for (const char* sql : broken) {
        INFO(sql);
        CHECK_THROWS_AS(parseScript(sql), ParseError);
    }
    // nesting is capped like statements are
    std::string deep = "BANAO PROCEDURE p () SHURU ";
    for (int i = 0; i < 40; ++i) deep += "IF 1 = 1 THEN ";
    deep += "DIKHAO 1; ";
    for (int i = 0; i < 40; ++i) deep += "END IF; ";
    deep += "KHATAM;";
    CHECK_THROWS_AS(parseScript(deep), ParseError);
    // a user variable is one token; a lone @ is still an error
    CHECK_THROWS_AS(tokenize("naam @ 5"), TokenizerError);
    auto tokens = tokenize("@abc_1 @X");
    REQUIRE(tokens.size() == 3);
    CHECK(tokens[0].type == TokenType::Ident);
    CHECK(tokens[0].textValue == "@abc_1");
    CHECK(tokens[1].textValue == "@x");
}

// ------------------------------------------------------------ highlighter ----

TEST_CASE("the highlighter knows the batch 4 words", "[sql4][highlight]") {
    auto kinds = [](const std::string& line) {
        std::string out;
        for (const auto& span : highlight::spans(line)) {
            if (!out.empty()) out += ",";
            out += line.substr(span.start, span.end - span.start) + ":" + highlight::kindName(span.kind);
        }
        return out;
    };
    CHECK(kinds("SAVEPOINT a") == "SAVEPOINT:keyword");
    CHECK(kinds("WAPAS TAK a") == "WAPAS:keyword,TAK:keyword");
    CHECK(kinds("PREPARE s FROM 'x'") == "PREPARE:keyword,FROM:keyword,'x':string");
    CHECK(kinds("DECLARE v INT DEFAULT 0") == "DECLARE:keyword,INT:type,DEFAULT:keyword,0:number");
    CHECK(kinds("END IF") == "END:keyword,IF:keyword");
    CHECK(kinds("JAB TAK i < 3 KARO") == "JAB:keyword,TAK:keyword,<:operator,3:number,KARO:keyword");
    CHECK(kinds("SIGNAL 'x'") == "SIGNAL:keyword,'x':string");
    CHECK(kinds("FETCH c INTO a") == "FETCH:keyword,INTO:keyword");
    CHECK(kinds("BANAO FUNCTION f () RETURNS INT") == "BANAO:keyword,FUNCTION:keyword,(:punctuation.bracket,):punctuation.bracket,RETURNS:keyword,INT:type");
}

// ------------------------------------------------------------ over the server protocol ----

TEST_CASE("variables, savepoints, autocommit, procedures and functions work per connection over the wire", "[sql4][server]") {
    RunningServer s;
    ConnectOptions options;
    options.port = s.port();
    Connection a(options);
    a.execute("BANAO TABLE t (id INT MUKHYA KUNJI, v INT)");
    a.execute("BANAO PROCEDURE dbl (INOUT x INT) SHURU SET x = x * 2; KHATAM");
    a.execute("BANAO FUNCTION twice (n INT) RETURNS INT SHURU RETURN n * 2; KHATAM");
    a.execute("SET @x = 21");
    auto out = a.execute("CHALAO dbl(@x)")[0];
    REQUIRE(out.rows.size() == 1);
    CHECK(out.columns == std::vector<std::string>{"x"});
    CHECK(std::get<int64_t>(out.rows[0][0].data) == 42);
    CHECK(std::get<int64_t>(a.execute("DIKHAO @x, twice(@x)")[0].rows[0][1].data) == 84);
    // another connection has its own @x
    Connection b(options);
    CHECK(b.execute("DIKHAO @x")[0].rows[0][0].isNull());
    // prepared statements are per session
    a.execute("PREPARE p FROM 'DAALO MEIN t MAAN (?, ?)'");
    a.execute("EXECUTE p USING 1, @x");
    CHECK(b.runScript("EXECUTE p USING 2, 2")[0].error.find("exist nahi karta") != std::string::npos);
    // savepoints over the wire
    a.execute("SHURU");
    a.execute("DAALO MEIN t MAAN (2, 2)");
    a.execute("SAVEPOINT s");
    a.execute("DAALO MEIN t MAAN (3, 3)");
    a.execute("ROLLBACK TO s");
    CHECK(a.execute("DIKHAO GINO(*) SE t")[0].rows[0][0].data == Value(int64_t(2)).data);
    a.execute("PAKKA");
    // autocommit = 0 on a's connection holds the lock until PAKKA; b is refused meanwhile
    a.execute("SET autocommit = 0");
    a.execute("DAALO MEIN t MAAN (4, 4)");
    CHECK(b.runScript("DAALO MEIN t MAAN (5, 5)")[0].error.find("Database busy") != std::string::npos);
    a.execute("PAKKA");
    CHECK(b.runScript("DAALO MEIN t MAAN (5, 5)")[0].error.empty());
    // a connection that vanishes in the middle of an autocommit transaction rolls it back
    {
        Connection c(options);
        c.execute("SET autocommit = 0");
        c.execute("DAALO MEIN t MAAN (6, 6)");
    }
    meradb_test::waitFor([&] { return b.runScript("DAALO MEIN t MAAN (7, 7)")[0].error.empty(); });
    CHECK(std::get<int64_t>(b.execute("DIKHAO GINO(*) SE t JAHAN id = 6")[0].rows[0][0].data) == 0);
    // SIGNAL is an ordinary error on the wire, and the statement is undone
    a.execute("SET autocommit = 1");
    a.execute("BANAO TRIGGER nope PEHLE MITAO PAR t SHURU SIGNAL 'mat hatao'; KHATAM");
    auto err = a.runScript("MITAO SE t")[0];
    CHECK(err.error == "[Execution Galti] mat hatao");
    CHECK(std::get<int64_t>(a.execute("DIKHAO GINO(*) SE t")[0].rows[0][0].data) >= 4);
}
