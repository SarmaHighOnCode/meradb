#include <catch2/catch_test_macros.hpp>
#include "meradb/cli_format.h"
#include "golden_shell.h"

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

namespace {

Result sampleTable() {
    Result r;
    r.columns = {"id", "naam"};
    r.rows = {{Value(int64_t(1)), Value(std::string("Ravi"))}, {Value(int64_t(2)), Value()}};
    r.message = "2 row(s)";
    return r;
}

Result sampleMessage() {
    Result r;
    r.message = "Table 't' ban gaya (1 columns)";
    return r;
}

Result sampleError() {
    Result r;
    r.error = "[Execution Galti] Table 'x' exist nahi karta";
    return r;
}

Result sampleEmpty() {
    Result r;
    r.columns = {"a"};
    r.message = "0 row(s)";
    return r;
}

Result sampleUnicode() {
    Result r;
    r.columns = {"n"};
    r.rows = {{Value(std::string("\xC3\xA9\xF0\x9F\x98\x80"))}};
    r.message = "1 row(s)";
    return r;
}

// Python's print_result prints formatResult's text plus a newline (nothing at all for an empty result).
void checkAgainstPython(const std::string& sample, const Result& r) {
    for (bool color : {false, true}) {
        INFO(sample << (color ? " (colour)" : " (plain)"));
        CHECK(formatResult(r, term::Style(color)) + "\n" == golden_shell::get(sample, color));
    }
}

}  // namespace

TEST_CASE("formatResult matches Python's print_result, plain and coloured", "[cli]") {
    checkAgainstPython("result_table", sampleTable());
    checkAgainstPython("result_message", sampleMessage());
    checkAgainstPython("result_error", sampleError());
    checkAgainstPython("result_empty", sampleEmpty());
    checkAgainstPython("result_unicode", sampleUnicode());
}

TEST_CASE("formatResult colours: dim borders, bold header, green message, red error", "[cli]") {
    const term::Style on = term::Style::colored();
    CHECK(formatResult(sampleMessage(), on) == "\x1b[32mTable 't' ban gaya (1 columns)\x1b[0m");
    CHECK(formatResult(sampleError(), on) == "\x1b[31m[Execution Galti] Table 'x' exist nahi karta\x1b[0m");
    CHECK(formatResult(sampleEmpty(), on) ==
          "\x1b[2m+---+\x1b[0m\n\x1b[1m| a |\x1b[0m\n\x1b[2m+---+\x1b[0m\n\x1b[2m+---+\x1b[0m\n\x1b[32m0 row(s)\x1b[0m");
    CHECK(formatResult(sampleEmpty()) == formatResult(sampleEmpty(), term::Style::none()));
}
