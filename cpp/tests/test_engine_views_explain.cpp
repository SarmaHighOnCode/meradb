// cpp/tests/test_engine_views_explain.cpp -- BANAO/HATAO VIEW, DIKHAO VIEWS,
// SAMJHAO (EXPLAIN), and the top-level execute()/runScript() entry points.
//
// The golden scripts in golden_engine.h were produced by the Python reference
// engine (tests/golden/gen_golden.py).
#include <catch2/catch_test_macros.hpp>
#include "golden_runner.h"
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "test_util.h"

using namespace meradb;
using meradb_test::TempDir;

namespace {
Result run(Engine& e, const std::string& sql) { return meradb_test::runLast(e, sql); }
}  // namespace

TEST_CASE("golden VIEW scripts match the Python engine", "[engine][views][golden]") {
    meradb_test::replayGolden("view");
}

TEST_CASE("A view is re-run on every use, so it sees new rows", "[engine][views]") {
    TempDir dir;
    Engine e(dir.str());
    run(e, "BANAO TABLE t (id INT, n INT);");
    run(e, "DAALO MEIN t MAAN (1, 10);");
    REQUIRE(run(e, "BANAO VIEW big KAHO DIKHAO id SE t JAHAN n > 5;").message == "View 'big' ban gaya");
    REQUIRE(run(e, "DIKHAO * SE big;").rows.size() == 1);
    run(e, "DAALO MEIN t MAAN (2, 20);");
    REQUIRE(run(e, "DIKHAO * SE big;").rows.size() == 2);
    // the definition is stored as raw text in the catalog
    REQUIRE(e.catalog().views.at("big") == "DIKHAO id SE t JAHAN n > 5");
}

TEST_CASE("EXPLAIN describes the plan without running the statement", "[engine][explain]") {
    TempDir dir;
    Engine e(dir.str());
    run(e, "BANAO TABLE t (id INT MUKHYA KUNJI, n INT);");
    run(e, "DAALO MEIN t MAAN (1, 10);");
    auto r = run(e, "SAMJHAO MITAO SE t JAHAN id = 1;");
    REQUIRE(r.columns == std::vector<std::string>{"plan"});
    REQUIRE(r.message == "Query plan (query chalayi nahi gayi)");
    REQUIRE(formatValue(r.rows.at(0).at(0)) == "1. INDEX LOOKUP t PAR id = 1  [hash index, MUKHYA KUNJI]");
    REQUIRE(run(e, "DIKHAO * SE t;").rows.size() == 1);  // nothing was deleted
}

TEST_CASE("execute() throws on the first error, runScript() keeps going", "[engine][entry]") {
    TempDir dir;
    Engine e(dir.str());
    REQUIRE_THROWS_AS(e.execute("BANAO TABLE t (id INT); DIKHAO nope SE t;"), ExecutionError);
    REQUIRE(e.catalog().find("t") != nullptr);  // the first statement had already run
    REQUIRE_THROWS_AS(e.execute("BANAO BANAO;"), ParseError);
    auto results = e.runScript("DIKHAO nope SE t; DIKHAO id SE t;");
    REQUIRE(results.size() == 2);
    REQUIRE_FALSE(results[0].error.empty());
    REQUIRE(results[1].error.empty());
}
