// cpp/tests/test_engine_select.cpp -- DIKHAO: scan/index, joins, GROUP BY,
// subqueries, set operations.
//
// The golden scripts in golden_engine.h were produced by the Python reference
// engine (tests/golden/gen_golden.py): same statements, same messages, same
// error wording, same columns and rows.
#include <catch2/catch_test_macros.hpp>
#include "golden_runner.h"
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "test_util.h"

using namespace meradb;
using meradb_test::TempDir;

namespace {

Result run(Engine& e, const std::string& sql) { return meradb_test::runLast(e, sql); }

void seedSchoolData(Engine& e) {
    run(e, "BANAO TABLE courses (id INT MUKHYA KUNJI, title TEXT);");
    run(e, "BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT, cgpa FLOAT, cid INT SANDARBH courses(id));");
    run(e, "DAALO MEIN courses (id, title) MAAN (10, 'DBMS'), (20, 'OS');");
    run(e, "DAALO MEIN students (id, naam, cgpa, cid) MAAN "
           "(1, 'Ravi', 8.4, 10), (2, 'Priya', 9.1, 10), (3, 'Aman', 7.0, KHALI);");
}

}  // namespace

TEST_CASE("golden SELECT scripts match the Python engine", "[engine][select][golden]") {
    meradb_test::replayGolden("select");
}

TEST_CASE("golden parse-error wording matches the Python engine", "[engine][parser][golden]") {
    meradb_test::replayGolden("parse");
}

TEST_CASE("SELECT filters, orders, and limits", "[engine][select]") {
    TempDir dir;
    Engine e(dir.str());
    seedSchoolData(e);
    auto r = run(e, "DIKHAO naam SE students JAHAN cgpa > 8 KRAM cgpa ULTA SIRF 1;");
    REQUIRE(r.rows.size() == 1);
    REQUIRE(std::get<std::string>(r.rows[0][0].data) == "Priya");
}

TEST_CASE("LEFT JOIN pads non-matching rows with NULL", "[engine][select]") {
    TempDir dir;
    Engine e(dir.str());
    seedSchoolData(e);
    auto r = run(e, "DIKHAO s.naam, c.title SE students s BAAYAN MILAO courses c PAR s.cid = c.id "
                    "JAHAN s.naam = 'Aman';");
    REQUIRE(r.rows.size() == 1);
    REQUIRE(r.rows[0][1].isNull());
}

TEST_CASE("GROUP BY with HAVING computes per-group aggregates", "[engine][select]") {
    TempDir dir;
    Engine e(dir.str());
    seedSchoolData(e);
    auto r = run(e, "DIKHAO cid, GINO(*) KAHO total SE students SAMOOH cid JINKA GINO(*) > 1;");
    REQUIRE(r.rows.size() == 1);
    REQUIRE(std::get<int64_t>(r.rows[0][1].data) == 2);
}

TEST_CASE("Uncorrelated subquery in WHERE is computed once", "[engine][select]") {
    TempDir dir;
    Engine e(dir.str());
    seedSchoolData(e);
    auto r = run(e, "DIKHAO naam SE students JAHAN cgpa > (DIKHAO AUSAT(cgpa) SE students);");
    REQUIRE(r.rows.size() == 2);  // average is 8.17: Ravi (8.4) and Priya (9.1)
    REQUIRE(std::get<std::string>(r.rows[0][0].data) == "Ravi");
    REQUIRE(std::get<std::string>(r.rows[1][0].data) == "Priya");
}

TEST_CASE("Correlated subquery re-executes per outer row", "[engine][select]") {
    TempDir dir;
    Engine e(dir.str());
    seedSchoolData(e);
    auto r = run(e, "DIKHAO c.title SE courses c "
                    "JAHAN (DIKHAO GINO(*) SE students s JAHAN s.cid = c.id) > 1;");
    REQUIRE(r.rows.size() == 1);
    REQUIRE(std::get<std::string>(r.rows[0][0].data) == "DBMS");
}

TEST_CASE("SetOp UNION dedupes combined rows", "[engine][select]") {
    TempDir dir;
    Engine e(dir.str());
    run(e, "BANAO TABLE a (id INT);");
    run(e, "BANAO TABLE b (id INT);");
    run(e, "DAALO MEIN a (id) MAAN (1), (2);");
    run(e, "DAALO MEIN b (id) MAAN (2), (3);");
    auto r = run(e, "DIKHAO id SE a SANYUKT DIKHAO id SE b;");
    REQUIRE(r.rows.size() == 3);  // 1,2,3 -- 2 deduped
}

TEST_CASE("SetOp CHHODKAR (EXCEPT) returns rows only in the left side", "[engine][select]") {
    TempDir dir;
    Engine e(dir.str());
    run(e, "BANAO TABLE a (id INT);");
    run(e, "BANAO TABLE b (id INT);");
    run(e, "DAALO MEIN a (id) MAAN (1), (2);");
    run(e, "DAALO MEIN b (id) MAAN (2);");
    auto r = run(e, "DIKHAO id SE a CHHODKAR DIKHAO id SE b;");
    REQUIRE(r.rows.size() == 1);
    REQUIRE(std::get<int64_t>(r.rows[0][0].data) == 1);
}

TEST_CASE("Index lookup and full scan give the same rows", "[engine][select]") {
    TempDir dir;
    Engine e(dir.str());
    seedSchoolData(e);
    auto viaIndex = run(e, "DIKHAO naam SE students JAHAN id = 2;");
    auto viaScan = run(e, "DIKHAO naam SE students JAHAN id + 0 = 2;");
    REQUIRE(viaIndex.rows.size() == 1);
    REQUIRE(viaIndex.rows[0][0].data == viaScan.rows[0][0].data);
}
