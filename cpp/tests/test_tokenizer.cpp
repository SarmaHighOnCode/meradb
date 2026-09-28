// cpp/tests/test_tokenizer.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/tokenizer.h"
#include "meradb/errors.h"
#include <cmath>
#include <cstdint>
#include <string>

using namespace meradb;

TEST_CASE("tokenize recognizes keywords case-insensitively", "[tokenizer]") {
    auto tokens = tokenize("banao TABLE");
    REQUIRE(tokens[0].type == TokenType::Keyword);
    REQUIRE(tokens[0].textValue == "BANAO");
    REQUIRE(tokens[1].type == TokenType::Keyword);
    REQUIRE(tokens[1].textValue == "TABLE");
}

TEST_CASE("tokenize lower-cases identifiers", "[tokenizer]") {
    auto tokens = tokenize("Students");
    REQUIRE(tokens[0].type == TokenType::Ident);
    REQUIRE(tokens[0].textValue == "students");
}

TEST_CASE("tokenize distinguishes int and float numbers", "[tokenizer]") {
    auto tokens = tokenize("42 3.14");
    REQUIRE(tokens[0].type == TokenType::Number);
    REQUIRE_FALSE(tokens[0].isFloat);
    REQUIRE(tokens[0].intValue == 42);
    REQUIRE(tokens[1].type == TokenType::Number);
    REQUIRE(tokens[1].isFloat);
    REQUIRE(tokens[1].doubleValue == 3.14);
}

TEST_CASE("tokenize reads single-quoted strings with escaped quotes", "[tokenizer]") {
    auto tokens = tokenize("'Ravi''s book'");
    REQUIRE(tokens[0].type == TokenType::String);
    REQUIRE(tokens[0].textValue == "Ravi's book");
}

TEST_CASE("tokenize handles two-char symbols before one-char", "[tokenizer]") {
    auto tokens = tokenize("<= >= != <>");
    REQUIRE(tokens[0].textValue == "<=");
    REQUIRE(tokens[1].textValue == ">=");
    REQUIRE(tokens[2].textValue == "!=");
    REQUIRE(tokens[3].textValue == "!=");  // <> normalizes to !=
}

TEST_CASE("tokenize skips line comments", "[tokenizer]") {
    auto tokens = tokenize("DIKHAO -- this is a comment\n naam");
    REQUIRE(tokens[0].textValue == "DIKHAO");
    REQUIRE(tokens[1].textValue == "naam");
}

TEST_CASE("tokenize always ends with Eof", "[tokenizer]") {
    auto tokens = tokenize("");
    REQUIRE(tokens.back().type == TokenType::Eof);
}

TEST_CASE("tokenize records start/end char offsets for source slicing", "[tokenizer]") {
    // CHECK/VIEW/TRIGGER/PROCEDURE bodies are re-parsed later from raw
    // source text, so every token must know its byte offsets.
    auto tokens = tokenize("umar >= 0");
    REQUIRE(tokens[0].start == 0);
    REQUIRE(tokens[0].end == 4);
}

TEST_CASE("tokenize throws TokenizerError on unrecognized character", "[tokenizer]") {
    REQUIRE_THROWS_AS(tokenize("naam @ 5"), TokenizerError);
}

TEST_CASE("tokenize error text matches Python, with line and col", "[tokenizer]") {
    try {
        tokenize("naam @ 5");
        FAIL("expected TokenizerError");
    } catch (const TokenizerError& e) {
        REQUIRE(e.message() == "Ye character samajh nahi aaya: '@' (line 1, col 6)");
    }
    try {
        tokenize("'open");
        FAIL("expected TokenizerError");
    } catch (const TokenizerError& e) {
        REQUIRE(e.message() == "String band nahi hui -- closing ' missing hai (line 1, col 6)");
    }
}

TEST_CASE("tokenize reports an over-long integer literal as a MeraDB error", "[tokenizer]") {
    // 30 digits: does not fit int64_t. Must stay inside the MeraDBError
    // hierarchy instead of leaking std::out_of_range.
    REQUIRE_THROWS_AS(tokenize("123456789012345678901234567890"), MeraDBError);
    try {
        tokenize("DAALO 99999999999999999999");
        FAIL("expected TokenizerError");
    } catch (const TokenizerError& e) {
        REQUIRE(e.message() == "Number 99999999999999999999 INT ke liye bahut bada hai (8-byte limit) (line 1, col 7)");
    }
    auto tokens = tokenize("9223372036854775807");
    REQUIRE(tokens[0].intValue == INT64_MAX);
}

TEST_CASE("tokenize reads an over-long float literal like Python float()", "[tokenizer]") {
    std::string huge(400, '9');
    auto tokens = tokenize(huge + ".5");
    REQUIRE(tokens[0].isFloat);
    REQUIRE(tokens[0].doubleValue == HUGE_VAL);
}

TEST_CASE("tokenize: '3.' is an int followed by a dot symbol", "[tokenizer]") {
    auto tokens = tokenize("3.");
    REQUIRE(tokens[0].type == TokenType::Number);
    REQUIRE_FALSE(tokens[0].isFloat);
    REQUIRE(tokens[1].textValue == ".");
}

TEST_CASE("tokenize skips a UTF-8 BOM, counting it as one column", "[tokenizer]") {
    auto tokens = tokenize("\xEF\xBB\xBF" "DIKHAO");
    REQUIRE(tokens[0].textValue == "DIKHAO");
    REQUIRE(tokens[0].col == 2);
}
