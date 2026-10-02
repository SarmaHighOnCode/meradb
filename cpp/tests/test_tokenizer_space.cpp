// cpp/tests/test_tokenizer_space.cpp -- which characters separate tokens. Python's tokenizer skips whatever
// str.isspace() accepts (29 characters, ASCII and not) plus U+FEFF; the expected strings below were produced by
// running the Python tokenizer on the same text.
#include <catch2/catch_test_macros.hpp>
#include "meradb/errors.h"
#include "meradb/tokenizer.h"
#include <string>

using namespace meradb;

namespace {

// "KEYWORD:DIKHAO@1:1 ..." -- the token kind, its text and its line:column, or "ERR <message>".
std::string describe(const std::string& source) {
    try {
        std::string out;
        for (const Token& t : tokenize(source)) {
            if (!out.empty()) out += ' ';
            switch (t.type) {
                case TokenType::Keyword: out += "KEYWORD:"; break;
                case TokenType::Ident: out += "IDENT:"; break;
                case TokenType::Number: out += "NUMBER:"; break;
                case TokenType::String: out += "STRING:"; break;
                case TokenType::Symbol: out += "SYMBOL:"; break;
                case TokenType::Eof: out += "EOF:"; break;
            }
            if (t.type != TokenType::Eof) out += t.textValue;
            out += "@" + std::to_string(t.line) + ":" + std::to_string(t.col);
        }
        return out;
    } catch (const TokenizerError& e) {
        return "ERR " + e.message();
    }
}

const char* const kTables = "KEYWORD:DIKHAO@1:1 KEYWORD:TABLES@1:8 SYMBOL:;@1:14 EOF:@1:15";
const char* const kTwoIdents = "IDENT:a@1:1 IDENT:b@1:3 EOF:@1:4";

}  // namespace

TEST_CASE("tokenizer_space non-ASCII whitespace separates tokens like Python", "[tokenizer][unicode]") {
    CHECK(describe("DIKHAO\xC2\xA0TABLES;") == kTables);        // NBSP
    CHECK(describe("DIKHAO\xE3\x80\x80TABLES;") == kTables);    // U+3000 ideographic space
    CHECK(describe("DIKHAO\xE2\x80\xA8TABLES;") == kTables);    // U+2028 line separator (no line break counted)
    CHECK(describe("DIKHAO TABLES;\xC2\xA0") == "KEYWORD:DIKHAO@1:1 KEYWORD:TABLES@1:8 SYMBOL:;@1:14 EOF:@1:16");
    CHECK(describe("a\xE2\x80\xA9" "b") == kTwoIdents);         // U+2029
    CHECK(describe("a\xC2\x85" "b") == kTwoIdents);             // U+0085
    CHECK(describe("a\x1c" "b") == kTwoIdents);                 // U+001C .. U+001F
    CHECK(describe("a\x1d" "b") == kTwoIdents);
    CHECK(describe("a\x1e" "b") == kTwoIdents);
    CHECK(describe("a\x1f" "b") == kTwoIdents);
    CHECK(describe("a\xE1\x9A\x80" "b") == kTwoIdents);         // U+1680
    CHECK(describe("a\xE2\x80\xAF" "b") == kTwoIdents);         // U+202F
    CHECK(describe("a\xE2\x81\x9F" "b") == kTwoIdents);         // U+205F
    CHECK(describe("a\xE2\x80\x8A" "b") == kTwoIdents);         // U+200A
}

TEST_CASE("tokenizer_space the byte-order mark counts as a space, the zero width space does not", "[tokenizer][unicode]") {
    CHECK(describe("a\xEF\xBB\xBF" "b") == kTwoIdents);
    CHECK(describe("\xEF\xBB\xBF" "DIKHAO") == "KEYWORD:DIKHAO@1:2 EOF:@1:8");
    CHECK(describe("a\xE2\x80\x8B" "b") == "ERR Ye character samajh nahi aaya: '\\u200b' (line 1, col 2)");
    CHECK(describe("a\xE2\x80\x8C" "b") == "ERR Ye character samajh nahi aaya: '\\u200c' (line 1, col 2)");
    CHECK(describe("a\xE1\xA0\x8E" "b") == "ERR Ye character samajh nahi aaya: '\\u180e' (line 1, col 2)");  // not space since Unicode 6.3
    CHECK(describe("a\xE2\x81\xA0" "b") == "ERR Ye character samajh nahi aaya: '\\u2060' (line 1, col 2)");  // word joiner
}

TEST_CASE("tokenizer_space columns and lines with several kinds of space", "[tokenizer][unicode]") {
    // each space character is one column; only "\n" starts a new line
    CHECK(describe("a\xC2\xA0\xE3\x80\x80\xE2\x80\x83" "b\nc\xE2\x80\xA8" "d\re\x0b" "f\x0c" "g") ==
          "IDENT:a@1:1 IDENT:b@1:5 IDENT:c@2:1 IDENT:d@2:3 IDENT:e@2:5 IDENT:f@2:7 IDENT:g@2:9 EOF:@2:10");
    CHECK(describe("x\xE3\x80\x80\xE3\x80\x80@") == "ERR Ye character samajh nahi aaya: '@' (line 1, col 4)");
    CHECK(describe("x\xE2\x80\xA8\n\xC2\xA0@") == "ERR Ye character samajh nahi aaya: '@' (line 2, col 2)");
}
