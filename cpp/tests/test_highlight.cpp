#include <catch2/catch_test_macros.hpp>
#include "golden_highlight.h"
#include "meradb/aggregates.h"
#include "meradb/highlight.h"
#include "meradb/tokenizer.h"

using namespace meradb;
using namespace meradb::highlight;

namespace {
std::string render(const std::string& line) {
    std::string out;
    for (const Span& s : spans(line)) {
        if (!out.empty()) out += ";";
        out += std::to_string(s.start) + "," + std::to_string(s.end) + "," + kindName(s.kind);
    }
    return out;
}
}  // namespace

TEST_CASE("highlight finds keywords, operators and skips plain names", "[highlight]") {
    CHECK(render("DIKHAO * SE t;") == "0,6,keyword;7,8,operator;9,11,keyword");
    CHECK(render("SACH jhooth khali") == "0,4,boolean;5,11,boolean;12,17,constant.builtin");
    CHECK(render("") == "");
}

TEST_CASE("highlight strings, comments and numbers", "[highlight]") {
    CHECK(render("x = 'it''s' -- note") == "2,3,operator;4,11,string;12,19,comment");
    CHECK(render("'abc") == "0,4,string");  // the closing quote is optional while typing
    CHECK(render("12 3.5 1abc 7.x") == "0,2,number;3,6,number;12,13,number");
}

TEST_CASE("highlight accessors share the tokenizer's word lists", "[highlight]") {
    CHECK(isKeyword("DIKHAO"));
    CHECK_FALSE(isKeyword("STUDENTS"));
    CHECK(isAggregateName("COUNT"));
    CHECK(isAggregateName("ADHIKTAM"));
    CHECK_FALSE(isAggregateName("DIKHAO"));
}

TEST_CASE("highlight matches Python's highlight.py on every golden line", "[highlight]") {
    for (const HighlightCase& c : kHighlightCases) {
        INFO("line: " << c.line);
        CHECK(render(c.line) == c.spans);
    }
}

TEST_CASE("highlight rich styles are the Dracula table of highlight.py", "[highlight]") {
    CHECK(richStyle(Kind::Keyword).rgb == 0xff79c6);
    CHECK(richStyle(Kind::Keyword).bold);
    CHECK(richStyle(Kind::Comment).italic);
    CHECK(richStyle(Kind::Bracket).rgb == -1);
    CHECK(richStyle(Kind::ConstantBuiltin).italic);
    CHECK(richStyle(Kind::Function).rgb == 0x50fa7b);
}
