// cpp/tests/test_datatypes.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/datatypes.h"
#include "meradb/errors.h"

using namespace meradb;

TEST_CASE("normalizeType resolves aliases", "[datatypes]") {
    REQUIRE(normalizeType("ank") == "INT");
    REQUIRE(normalizeType("INTEGER") == "INT");
    REQUIRE(normalizeType("dashamlav") == "FLOAT");
    REQUIRE(normalizeType("varchar2") == "TEXT");
    REQUIRE(normalizeType("haan_na") == "BOOL");
    REQUIRE(normalizeType("tareekh") == "DATE");
    REQUIRE_FALSE(normalizeType("nonsense").has_value());
}

TEST_CASE("coerce validates and widens", "[datatypes]") {
    Value v = coerce(Value(int64_t{5}), "FLOAT", "cgpa");
    REQUIRE(std::get<double>(v.data) == 5.0);

    REQUIRE_THROWS_AS(coerce(Value(std::string("x")), "INT", "id"), ExecutionError);
}

TEST_CASE("coerce rejects bool for INT columns", "[datatypes]") {
    // Python bool is an int subclass, so coerce must check bool BEFORE int.
    REQUIRE_THROWS_AS(coerce(Value(true), "INT", "id"), ExecutionError);
}

TEST_CASE("coerce passes NULL through untouched", "[datatypes]") {
    Value v = coerce(Value(), "INT", "id");
    REQUIRE(v.isNull());
}

TEST_CASE("formatValue renders Hinglish literals", "[datatypes]") {
    REQUIRE(formatValue(Value(true)) == "SACH");
    REQUIRE(formatValue(Value(false)) == "JHOOTH");
    REQUIRE(formatValue(Value()) == "KHALI");
}

TEST_CASE("parseDate parses ISO dates", "[datatypes]") {
    Date d = parseDate("2004-05-12");
    REQUIRE(d.toOrdinal() > 0);
    REQUIRE_THROWS_AS(parseDate("not-a-date"), ExecutionError);
}
