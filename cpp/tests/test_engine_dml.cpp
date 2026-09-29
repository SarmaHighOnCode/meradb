// cpp/tests/test_engine_dml.cpp -- INSERT / UPDATE / DELETE.
//
// The golden scripts in golden_engine.h were produced by the Python reference
// engine (tests/golden/gen_golden.py): same statements, same messages, same
// error wording, same rows.
#include <catch2/catch_test_macros.hpp>
#include "golden_engine.h"
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "meradb/parser.h"
#include "test_util.h"
#include <regex>

using namespace meradb;
using meradb_test::TempDir;

namespace {

std::string canonical(const Result& r) {
    std::string s = "C:";
    for (size_t i = 0; i < r.columns.size(); ++i) s += (i ? "," : "") + r.columns[i];
    s += " R:";
    for (size_t i = 0; i < r.rows.size(); ++i) {
        s += i ? ";" : "";
        for (size_t j = 0; j < r.rows[i].size(); ++j) s += (j ? "|" : "") + formatValue(r.rows[i][j]);
    }
    return s + " M:" + r.message + " E:" + r.error;
}

Result run(Engine& e, const std::string& sql) {
    Result last;
    for (auto& r : e.runScript(sql)) last = r;
    return last;
}

// Until DIKHAO exists, `DIKHAO * SE t;` steps read the heap file directly (same
// columns, file order, and row-count message as the real thing).
Result stepResult(Engine& e, const std::string& sql) {
    static const std::regex star(R"(^DIKHAO \* SE (\w+);$)");
    std::smatch m;
    if (!std::regex_match(sql, m, star)) return run(e, sql);
    Table t(e.catalog().get(m[1]), e.catalog().tablePath(m[1]));
    Result r;
    r.columns = t.schema().columnNames();
    for (auto& [id, values] : t.rows()) r.rows.push_back(values);
    r.message = std::to_string(r.rows.size()) + " row(s)";
    return r;
}

}  // namespace

TEST_CASE("golden DML scripts match the Python engine", "[engine][dml][golden]") {
    for (const auto& script : golden::scripts()) {
        if (std::string(script.group) != "dml") continue;
        TempDir dir;
        Engine e(dir.str());
        int n = 0;
        for (const auto& step : script.steps) {
            ++n;
            INFO("script " << script.name << ", step " << n << ": " << step.sql);
            Result got = stepResult(e, step.sql);
            CHECK(canonical(got) == step.expect);
        }
    }
}

TEST_CASE("INSERT validates all rows before writing any", "[engine][dml]") {
    TempDir dir;
    Engine e(dir.str());
    run(e, "BANAO TABLE t (id INT MUKHYA KUNJI, naam TEXT ZAROORI);");
    auto result = run(e, "DAALO MEIN t (id, naam) MAAN (1, 'a'), (2, 'b'), (3, KHALI);");
    REQUIRE(result.error == "[Execution Galti] Column 'naam' ZAROORI hai, KHALI nahi ho sakta");
    REQUIRE(e.catalog().find("t") != nullptr);
    Table t(e.catalog().get("t"), e.catalog().tablePath("t"));
    REQUIRE(t.rows().empty());  // rows 1 and 2 were NOT written
}

TEST_CASE("DML against a VIEW is refused with Python's wording", "[engine][dml]") {
    TempDir dir;
    Engine e(dir.str());
    run(e, "BANAO TABLE t (id INT);");
    // a view definition can be stored straight in the catalog: SE/BANAO VIEW is Task 19
    e.catalog().addView("v", "DIKHAO * SE t");
    const std::string msg = "[Execution Galti] 'v' ek VIEW hai, table nahi -- isme DAALO/BADLO/MITAO nahi kar sakte";
    REQUIRE(run(e, "DAALO MEIN v MAAN (1);").error == msg);
    REQUIRE(run(e, "BADLO v RAKHO id = 1;").error == msg);
    REQUIRE(run(e, "MITAO SE v;").error == msg);
}

TEST_CASE("UPDATE that does not re-match its own output (no Halloween problem)", "[engine][dml]") {
    TempDir dir;
    Engine e(dir.str());
    run(e, "BANAO TABLE t (id INT MUKHYA KUNJI, n INT);");
    run(e, "DAALO MEIN t MAAN (1, 1), (2, 2), (3, 3);");
    auto r = run(e, "BADLO t RAKHO n = n + 10 JAHAN n > 0;");
    REQUIRE(r.message == "3 row(s) badal di");
    Table t(e.catalog().get("t"), e.catalog().tablePath("t"));
    int64_t sum = 0;
    for (auto& [id, values] : t.rows()) sum += std::get<int64_t>(values[1].data);
    REQUIRE(sum == 36);  // each row updated exactly once
}

TEST_CASE("Transactions roll back DML", "[engine][dml]") {
    TempDir dir;
    Engine e(dir.str());
    run(e, "BANAO TABLE t (id INT MUKHYA KUNJI);");
    run(e, "SHURU;");
    run(e, "DAALO MEIN t (id) MAAN (1);");
    run(e, "WAPAS;");
    Table t(e.catalog().get("t"), e.catalog().tablePath("t"));
    REQUIRE(t.rows().empty());
    // the index cache was dropped with the rollback: the key is free again
    REQUIRE(run(e, "DAALO MEIN t (id) MAAN (1);").message == "1 row(s) daal di");
}
