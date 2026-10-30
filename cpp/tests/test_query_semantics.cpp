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
