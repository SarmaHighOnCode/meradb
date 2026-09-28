// cpp/tests/test_datatypes.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/datatypes.h"
#include "meradb/errors.h"
#include <cmath>
#include <cstdint>

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
    REQUIRE(d.toOrdinal() == 731713);  // Python: date(2004, 5, 12).toordinal()
    REQUIRE(d.isoFormat() == "2004-05-12");
    REQUIRE_THROWS_AS(parseDate("not-a-date"), ExecutionError);
}

TEST_CASE("Date ordinal round-trips at the calendar edges", "[datatypes]") {
    REQUIRE(parseDate("0001-01-01").toOrdinal() == 1);
    REQUIRE(parseDate("9999-12-31").toOrdinal() == 3652059);
    for (const char* text : {"0001-01-01", "1900-02-28", "1900-03-01", "2000-02-29", "2000-12-31",
                             "2004-12-31", "2023-03-01", "2024-02-29", "9999-12-31"}) {
        REQUIRE(parseDate(text).isoFormat() == text);
    }
}

TEST_CASE("parseDate validates day-of-month including leap years", "[datatypes]") {
    REQUIRE_THROWS_AS(parseDate("2023-02-29"), ExecutionError);
    REQUIRE(parseDate("2024-02-29").isoFormat() == "2024-02-29");
    REQUIRE_THROWS_AS(parseDate("2023-02-31"), ExecutionError);
    REQUIRE_THROWS_AS(parseDate("2023-04-31"), ExecutionError);
    REQUIRE_THROWS_AS(parseDate("1900-02-29"), ExecutionError);
    REQUIRE_THROWS_AS(parseDate("2023-13-01"), ExecutionError);
    REQUIRE_THROWS_AS(parseDate("2023-00-10"), ExecutionError);
    REQUIRE_THROWS_AS(parseDate("2023-01-00"), ExecutionError);
}

TEST_CASE("parseDate is as strict as date.fromisoformat about shape", "[datatypes]") {
    for (const char* bad : {" 2004-05-12", "2004-05-12 ", "+2004-05-12", "-2004-05-12",
                            "2004-5-12", "2004-05-1", "04-05-12", "0000-01-01", "2004-05-12x",
                            "2004-0512", "200405-12", "2004/05/12", "2004-05-12T00", ""}) {
        INFO(bad);
        REQUIRE_THROWS_AS(parseDate(bad), ExecutionError);
    }
}

TEST_CASE("parseDate accepts the other forms Python 3.11+ fromisoformat takes", "[datatypes]") {
    // Values checked against CPython 3.12's date.fromisoformat.
    REQUIRE(parseDate("20040512").isoFormat() == "2004-05-12");
    REQUIRE(parseDate("2004-W19-3").isoFormat() == "2004-05-05");
    REQUIRE(parseDate("2004W193").isoFormat() == "2004-05-05");
    REQUIRE(parseDate("2004-W19").isoFormat() == "2004-05-03");
    REQUIRE(parseDate("2004W19").isoFormat() == "2004-05-03");
    REQUIRE(parseDate("2004-W53-7").isoFormat() == "2005-01-02");
    REQUIRE(parseDate("0001-W01-1").isoFormat() == "0001-01-01");
    for (const char* bad : {"20040230", "2003-W53", "2004-W00", "2004-W19-0", "2004-W19-8",
                            "2004W19-3", "2004-W193", "2004-W1-3", "2004-w19", "9999-W52-7"}) {
        INFO(bad);
        REQUIRE_THROWS_AS(parseDate(bad), ExecutionError);
    }
}

TEST_CASE("parseDate error text matches Python", "[datatypes]") {
    try {
        parseDate("2023-02-31", "dob");
        FAIL("expected ExecutionError");
    } catch (const ExecutionError& e) {
        REQUIRE(e.message() == "Column 'dob': '2023-02-31' valid DATE nahi hai -- 'YYYY-MM-DD' format chahiye");
    }
}

TEST_CASE("formatValue renders floats like Python's format_value", "[datatypes]") {
    REQUIRE(formatValue(Value(5.0)) == "5.0");
    REQUIRE(formatValue(Value(8.166666666666666)) == "8.166666667");
    REQUIRE(formatValue(Value(1e20)) == "1e+20");
    REQUIRE(formatValue(Value(0.5)) == "0.5");
    REQUIRE(formatValue(Value(-3.0)) == "-3.0");
    REQUIRE(formatValue(Value(1e-5)) == "1e-05");
    REQUIRE(formatValue(Value(HUGE_VAL)) == "inf");
    REQUIRE(formatValue(Value(-HUGE_VAL)) == "-inf");
    REQUIRE(formatValue(Value(std::nan(""))) == "nan");
}

TEST_CASE("coerce FLOAT to INT only when integral and in range", "[datatypes]") {
    REQUIRE(std::get<int64_t>(coerce(Value(3.0), "INT", "id").data) == 3);
    REQUIRE(std::get<int64_t>(coerce(Value(-9223372036854775808.0), "INT", "id").data) == INT64_MIN);
    try {
        coerce(Value(1e20), "INT", "id");
        FAIL("expected ExecutionError");
    } catch (const ExecutionError& e) {
        REQUIRE(e.message() == "Column 'id': 100000000000000000000 INT ke liye bahut bada hai (8-byte limit)");
    }
    REQUIRE_THROWS_AS(coerce(Value(9223372036854775808.0), "INT", "id"), ExecutionError);
    REQUIRE_THROWS_AS(coerce(Value(HUGE_VAL), "INT", "id"), ExecutionError);
    REQUIRE_THROWS_AS(coerce(Value(std::nan("")), "INT", "id"), ExecutionError);
    REQUIRE_THROWS_AS(coerce(Value(3.5), "INT", "id"), ExecutionError);
}

TEST_CASE("coerce type-mismatch text matches Python", "[datatypes]") {
    try {
        coerce(Value(3.5), "INT", "id");
        FAIL("expected ExecutionError");
    } catch (const ExecutionError& e) {
        REQUIRE(e.message() == "Column 'id' INT type ka hai, par value 3.5 mili");
    }
}
