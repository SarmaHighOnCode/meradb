// cpp/tests/test_query_semantics.cpp -- SQL semantics audit: every case here was a wrong answer or a corrupted table
// before it was fixed. MeraDB-C++ only, so each expectation is written out by hand (from standard SQL / MySQL).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "golden_runner.h"
#include "test_util.h"

using namespace meradb;
using Catch::Matchers::ContainsSubstring;
using meradb_test::TempDir;

namespace {

Result run(Engine& e, const std::string& sql) { return meradb_test::runLast(e, sql); }

// "1|a;2|b": the rows of a result as one comparable string
std::string flat(const Result& r) {
    std::string out;
    for (size_t i = 0; i < r.rows.size(); ++i) {
        if (i) out += ";";
        for (size_t j = 0; j < r.rows[i].size(); ++j) out += (j ? "|" : "") + formatValue(r.rows[i][j]);
    }
    return out;
}

std::string q(Engine& e, const std::string& sql) { return flat(run(e, sql)); }

void seed(Engine& e) {
    run(e, "BANAO TABLE t (id INT MUKHYA KUNJI, a INT, b TEXT);");
    run(e, "DAALO MEIN t MAAN (1, 10, 'x'), (2, KHALI, 'y'), (3, 30, KHALI), (4, 10, 'x'), (5, 20, 'z');");
    run(e, "BANAO TABLE n (v INT);");
    run(e, "DAALO MEIN n MAAN (10), (KHALI);");
}

}  // namespace

TEST_CASE("NOT IN over a subquery holding KHALI is never true", "[semantics][null]") {
    TempDir dir;
    Engine e(dir.str());
    seed(e);
    REQUIRE(q(e, "DIKHAO id SE t JAHAN a MEIN (DIKHAO v SE n) KRAM id;") == "1;4");
    // 30 NOT IN (10, NULL) is unknown, 10 NOT IN (10, NULL) is false: no row qualifies
    REQUIRE(q(e, "DIKHAO id SE t JAHAN a NAHI MEIN (DIKHAO v SE n) KRAM id;") == "");
    // the same list written out gave the right answer all along
    REQUIRE(q(e, "DIKHAO id SE t JAHAN a NAHI MEIN (10, KHALI) KRAM id;") == "");
    // NOT (x IN (10, NULL)) must agree with x NOT IN (10, NULL)
    REQUIRE(q(e, "DIKHAO id SE t JAHAN NAHI (a MEIN (DIKHAO v SE n)) KRAM id;") == "");
}

TEST_CASE("KHALI NOT IN an empty subquery is true", "[semantics][null]") {
    TempDir dir;
    Engine e(dir.str());
    seed(e);
    // the empty set contains nothing, not even an unknown: every row qualifies, also the one with a KHALI
    REQUIRE(q(e, "DIKHAO id SE t JAHAN a NAHI MEIN (DIKHAO v SE n JAHAN v = 12345) KRAM id;") == "1;2;3;4;5");
    REQUIRE(q(e, "DIKHAO id SE t JAHAN a MEIN (DIKHAO v SE n JAHAN v = 12345) KRAM id;") == "");
}

TEST_CASE("IN over a large subquery keeps its NULL rules", "[semantics][null]") {
    TempDir dir;
    Engine e(dir.str());
    run(e, "BANAO TABLE big (v INT);");
    std::string rows = "(KHALI)";
    for (int i = 0; i < 200; ++i) rows += ",(" + std::to_string(i) + ")";
    run(e, "DAALO MEIN big MAAN " + rows + ";");
    REQUIRE(q(e, "DIKHAO 5 MEIN (DIKHAO v SE big), 500 MEIN (DIKHAO v SE big), 5.0 MEIN (DIKHAO v SE big), 5 NAHI MEIN (DIKHAO v SE big), 500 NAHI MEIN (DIKHAO v SE big);") ==
            "SACH|KHALI|SACH|JHOOTH|KHALI");
}

TEST_CASE("An error in a trigger undoes the whole statement, not only SIGNAL", "[semantics][trigger]") {
    TempDir dir;
    Engine e(dir.str());
    run(e, "BANAO TABLE t (id INT MUKHYA KUNJI, v INT);");
    run(e, "BANAO TABLE lg (id INT MUKHYA KUNJI);");
    run(e, "BANAO TRIGGER a1 BAAD DAALO PAR t SHURU DAALO MEIN lg MAAN (NAYA.v); KHATAM;");
    // the 3rd row's trigger hits a duplicate in lg
    REQUIRE_THROWS_WITH(e.execute("DAALO MEIN t MAAN (1, 1), (2, 2), (3, 1);"), ContainsSubstring("Duplicate"));
    REQUIRE(q(e, "DIKHAO GINO(*) SE t;") == "0");
    REQUIRE(q(e, "DIKHAO GINO(*) SE lg;") == "0");

    run(e, "HATAO TRIGGER a1;");
    run(e, "DAALO MEIN t MAAN (1, 1), (2, 2);");
    run(e, "BANAO TRIGGER a2 BAAD BADLO PAR t SHURU DIKHAO 1 / 0; KHATAM;");
    REQUIRE_THROWS_WITH(e.execute("BADLO t RAKHO v = 50;"), ContainsSubstring("Zero se divide"));
    REQUIRE(q(e, "DIKHAO v SE t KRAM id;") == "1;2");
    run(e, "HATAO TRIGGER a2;");
    run(e, "BANAO TRIGGER a3 PEHLE MITAO PAR t SHURU DAALO MEIN lg MAAN (77); DAALO MEIN lg MAAN (77); KHATAM;");
    REQUIRE_THROWS_WITH(e.execute("MITAO SE t;"), ContainsSubstring("Duplicate"));
    REQUIRE(q(e, "DIKHAO GINO(*) SE lg;") == "0");
    REQUIRE(q(e, "DIKHAO GINO(*) SE t;") == "2");
}

TEST_CASE("A runaway trigger leaves nothing behind", "[semantics][trigger]") {
    TempDir dir;
    Engine e(dir.str());
    run(e, "BANAO TABLE lg (msg TEXT);");
    run(e, "BANAO TRIGGER r1 BAAD DAALO PAR lg SHURU DAALO MEIN lg MAAN ('x'); KHATAM;");
    REQUIRE_THROWS(e.execute("DAALO MEIN lg MAAN ('start');"));
    REQUIRE(q(e, "DIKHAO GINO(*) SE lg;") == "0");
}

TEST_CASE("A failed statement inside a transaction undoes only itself", "[semantics][trigger][txn]") {
    TempDir dir;
    Engine e(dir.str());
    run(e, "BANAO TABLE t (id INT MUKHYA KUNJI, v INT);");
    run(e, "BANAO TABLE lg (id INT MUKHYA KUNJI);");
    run(e, "BANAO TRIGGER a1 BAAD DAALO PAR t SHURU DAALO MEIN lg MAAN (NAYA.v); KHATAM;");
    run(e, "SHURU;");
    run(e, "DAALO MEIN t MAAN (1, 1);");
    REQUIRE_THROWS(e.execute("DAALO MEIN t MAAN (2, 1);"));
    run(e, "PAKKA;");
    REQUIRE(q(e, "DIKHAO id SE t;") == "1");
    REQUIRE(q(e, "DIKHAO id SE lg;") == "1");
}

TEST_CASE("TAKRAAV PAR BADLO cannot create a duplicate key", "[semantics][upsert]") {
    TempDir dir;
    Engine e(dir.str());
    run(e, "BANAO TABLE u (id INT MUKHYA KUNJI, v INT);");
    run(e, "DAALO MEIN u MAAN (1, 0);");
    // row (1, 0) collides and is updated to id = 2, while row (2, 0) is inserted fresh
    REQUIRE_THROWS_WITH(e.execute("DAALO MEIN u MAAN (1, 0), (2, 0) TAKRAAV PAR BADLO id = 2;"), ContainsSubstring("Duplicate"));
    REQUIRE(q(e, "DIKHAO id SE u KRAM id;") == "1");
}
