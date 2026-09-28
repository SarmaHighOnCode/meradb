// cpp/tests/test_aggregates.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/aggregates.h"
#include "meradb/errors.h"
#include <functional>

using namespace meradb;
using namespace meradb::ast;

namespace {
FuncCall makeCall(const std::string& name, std::unique_ptr<Expr> arg) {
    FuncCall f; f.name = name; f.arg = std::move(arg); return f;
}

// s.x as the planner's bind() leaves it
FuncCall onX(const std::string& name) {
    auto ref = std::make_unique<ColumnRef>("x", "s");
    ref->bound = true;
    return makeCall(name, std::move(ref));
}

std::vector<Row> rowsOf(std::vector<Value> xs) {
    std::vector<Row> rows;
    for (auto& v : xs) rows.push_back({{"s.x", v}});
    return rows;
}

std::string messageOf(const std::function<void()>& fn) {
    try { fn(); } catch (const MeraDBError& e) { return e.message(); }
    return "<no error>";
}

Value I(int64_t v) { return Value(v); }
Value F(double v) { return Value(v); }
Value S(const char* v) { return Value(std::string(v)); }
}  // namespace

TEST_CASE("canonicalName resolves every alias", "[aggregates]") {
    REQUIRE(canonicalName(makeCall("COUNT", std::make_unique<Star>())) == "GINO");
    REQUIRE(canonicalName(makeCall("GINO", std::make_unique<Star>())) == "GINO");
    REQUIRE(canonicalName(makeCall("SUM", std::make_unique<ColumnRef>("cgpa"))) == "KUL");
    REQUIRE(canonicalName(makeCall("AVG", std::make_unique<ColumnRef>("cgpa"))) == "AUSAT");
    REQUIRE(canonicalName(makeCall("MIN", std::make_unique<ColumnRef>("cgpa"))) == "NYUNTAM");
    REQUIRE(canonicalName(makeCall("MAX", std::make_unique<ColumnRef>("cgpa"))) == "ADHIKTAM");
    REQUIRE(canonicalName(makeCall("ADHIKTAM", std::make_unique<ColumnRef>("cgpa"))) == "ADHIKTAM");
}

TEST_CASE("canonicalName throws for unknown function name", "[aggregates]") {
    REQUIRE_THROWS_AS(canonicalName(makeCall("NONSENSE", std::make_unique<Star>())), ExecutionError);
    REQUIRE(messageOf([] { canonicalName(makeCall("NONSENSE", std::make_unique<Star>())); }) ==
            "Function 'NONSENSE' nahi pata. Ye chalte hain: GINO, KUL, AUSAT, NYUNTAM, ADHIKTAM");
}

TEST_CASE("canonicalName rejects X(*) for anything but GINO", "[aggregates]") {
    REQUIRE(messageOf([] { canonicalName(makeCall("MAX", std::make_unique<Star>())); }) ==
            "MAX(*) nahi chalta -- '*' sirf GINO(*) mein");
}

TEST_CASE("GINO(*) counts all rows including NULL columns", "[aggregates]") {
    auto call = makeCall("GINO", std::make_unique<Star>());
    REQUIRE(std::get<int64_t>(computeAggregate(call, rowsOf({Value(), I(1)})).data) == 2);
    REQUIRE(std::get<int64_t>(computeAggregate(call, {}).data) == 0);
}

TEST_CASE("GINO(x) counts only non-KHALI values", "[aggregates]") {
    REQUIRE(std::get<int64_t>(computeAggregate(onX("GINO"), rowsOf({Value(), I(1)})).data) == 1);
    REQUIRE(std::get<int64_t>(computeAggregate(onX("COUNT"), rowsOf({Value()})).data) == 0);
}

TEST_CASE("KUL/AUSAT ignore KHALI and return KHALI for an all-KHALI group", "[aggregates]") {
    auto sum = onX("KUL");
    REQUIRE(computeAggregate(sum, rowsOf({Value()})).isNull());
    REQUIRE(computeAggregate(onX("AUSAT"), {}).isNull());

    auto total = computeAggregate(sum, rowsOf({I(2), Value(), I(3)}));
    REQUIRE(std::get<int64_t>(total.data) == 5);
    REQUIRE(std::get<double>(computeAggregate(sum, rowsOf({I(2), F(1.5)})).data) == 3.5);
    REQUIRE(std::get<double>(computeAggregate(onX("AUSAT"), rowsOf({I(2), I(3)})).data) == 2.5);
}

TEST_CASE("KUL sums floats with compensation like Python's sum()", "[aggregates]") {
    std::vector<Value> tenths(10, F(0.1));
    REQUIRE(std::get<double>(computeAggregate(onX("KUL"), rowsOf(tenths)).data) == 1.0);
    REQUIRE(std::get<double>(computeAggregate(onX("SUM"), rowsOf({F(1e100), F(1.0), F(-1e100)})).data) == 1.0);
}

TEST_CASE("KUL follows the Windows CPython sum() paths", "[aggregates]") {
    // expected values printed by the reference Python (MSC build, 32-bit C long)
    auto sum = [](std::vector<Value> xs) { return std::get<double>(computeAggregate(onX("KUL"), rowsOf(xs)).data); };
    REQUIRE(sum({I(int64_t{1} << 40), F(1e100), F(1.0), F(-1e100)}) == 0.0);  // big int: generic path, no compensation
    REQUIRE(sum({I(int64_t{1} << 20), F(1e100), F(1.0), F(-1e100)}) == 1.0);
    REQUIRE(sum({F(1e100), F(1.0), I(int64_t{1} << 40), F(-1e100), F(3.0)}) == 3.0);
    REQUIRE(sum({F(1e100), F(1.0), I(int64_t{1} << 20), F(-1e100), F(3.0)}) == 4.0);
    REQUIRE(sum({I(3000000000), F(0.1), F(0.2)}) == 3000000000.2999997);
    REQUIRE(sum({F(0.1), I(3000000000), F(0.2), F(0.3)}) == 3000000000.6);
    REQUIRE(sum({F(0.1), I(5), F(0.2), F(0.3)}) == 5.6);
    // plain ints beyond 32 bits stay exact
    REQUIRE(std::get<int64_t>(computeAggregate(onX("KUL"), rowsOf({I(3000000000), I(3000000000)})).data) == 6000000000);
    REQUIRE_THROWS_AS(computeAggregate(onX("KUL"), rowsOf({I(INT64_MAX), I(1)})), ExecutionError);
}

TEST_CASE("AUSAT of an int total uses correctly-rounded division", "[aggregates]") {
    // Python: sum([9007199254740993, 0, 0]) / 3 -> 3002399751580331.0
    auto avg = computeAggregate(onX("AUSAT"), rowsOf({I(9007199254740993), I(0), I(0)}));
    REQUIRE(std::get<double>(avg.data) == 3002399751580331.0);
}

TEST_CASE("NYUNTAM/ADHIKTAM compute min/max ignoring KHALI", "[aggregates]") {
    REQUIRE(std::get<int64_t>(computeAggregate(onX("NYUNTAM"), rowsOf({I(5), Value(), I(2)})).data) == 2);
    REQUIRE(std::get<int64_t>(computeAggregate(onX("ADHIKTAM"), rowsOf({I(5), Value(), I(2)})).data) == 5);
    REQUIRE(std::get<std::string>(computeAggregate(onX("MIN"), rowsOf({S("b"), S("a")})).data) == "a");
    REQUIRE(computeAggregate(onX("MAX"), rowsOf({Value()})).isNull());
}

TEST_CASE("NYUNTAM/ADHIKTAM keep the first of equal values, like Python", "[aggregates]") {
    // max([1, 2.0, 2]) -> 2.0 ; min([2, 2.0]) -> 2
    REQUIRE(std::holds_alternative<double>(computeAggregate(onX("ADHIKTAM"), rowsOf({I(1), F(2.0), I(2)})).data));
    REQUIRE(std::holds_alternative<int64_t>(computeAggregate(onX("NYUNTAM"), rowsOf({I(2), F(2.0)})).data));
}

TEST_CASE("NYUNTAM/ADHIKTAM of unorderable values is an error", "[aggregates]") {
    REQUIRE_THROWS_AS(computeAggregate(onX("ADHIKTAM"), rowsOf({I(1), S("a")})), ExecutionError);
}

TEST_CASE("KUL rejects a non-numeric argument", "[aggregates]") {
    REQUIRE_THROWS_AS(computeAggregate(onX("KUL"), rowsOf({S("nope")})), ExecutionError);
    REQUIRE(messageOf([] { computeAggregate(onX("KUL"), rowsOf({S("nope")})); }) ==
            "KUL(s.x): KUL sirf numbers ke saath chalta hai");
    REQUIRE(messageOf([] { computeAggregate(onX("SUM"), rowsOf({I(1), Value(true)})); }) ==
            "SUM(s.x): KUL sirf numbers ke saath chalta hai");
}
