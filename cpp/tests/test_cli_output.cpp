#include <catch2/catch_test_macros.hpp>
#include "meradb/cli_format.h"

using namespace meradb;

TEST_CASE("formatResult renders an ASCII table for rows", "[cli]") {
    Result r;
    r.columns = {"naam", "cgpa"};
    r.rows = {{Value(std::string("Ravi")), Value(9.5)}, {Value(std::string("Priya")), Value(6.0)}};
    REQUIRE(formatResult(r) ==
            "+-------+------+\n"
            "| naam  | cgpa |\n"
            "+-------+------+\n"
            "| Ravi  | 9.5  |\n"
            "| Priya | 6.0  |\n"
            "+-------+------+");
}

TEST_CASE("formatResult renders a bare message for DDL/DML confirmations", "[cli]") {
    Result r;
    r.message = "Table 'students' ban gaya";
    REQUIRE(formatResult(r) == "Table 'students' ban gaya");
}

TEST_CASE("formatResult renders errors as-is", "[cli]") {
    Result r;
    r.error = "[Execution Galti] Table 'x' nahi mila";
    REQUIRE(formatResult(r) == "[Execution Galti] Table 'x' nahi mila");
}

TEST_CASE("formatResult renders KHALI for null cells", "[cli]") {
    Result r;
    r.columns = {"x"};
    r.rows = {{Value()}};
    REQUIRE(formatResult(r).find("KHALI") != std::string::npos);
}

TEST_CASE("formatResult pads by characters, not bytes", "[cli]") {
    Result r;
    r.columns = {"n"};
    r.rows = {{Value(std::string("caf\xC3\xA9"))}, {Value(std::string("ab"))}};
    REQUIRE(formatResult(r) ==
            "+------+\n| n    |\n+------+\n| caf\xC3\xA9 |\n| ab   |\n+------+");
}

TEST_CASE("formatResult puts the message under a table", "[cli]") {
    Result r;
    r.columns = {"a"};
    r.message = "done";
    REQUIRE(formatResult(r) == "+---+\n| a |\n+---+\n+---+\ndone");
}
