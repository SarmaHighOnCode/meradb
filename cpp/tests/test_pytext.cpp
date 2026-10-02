// cpp/tests/test_pytext.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/pytext.h"

using namespace meradb;

TEST_CASE("pytext isSpace matches Python's str.isspace", "[pytext]") {
    for (char32_t c : {0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x85, 0xA0, 0x1680, 0x2000, 0x2005, 0x200A,
                       0x2028, 0x2029, 0x202F, 0x205F, 0x3000})
        CHECK(pytext::isSpace(c));
    for (char32_t c : {0x00, 0x08, 0x0E, 0x1B, 0x21, 0x41, 0x84, 0x86, 0x9F, 0xA1, 0x180E, 0x1FFF, 0x200B, 0x2027, 0x202A,
                       0x2060, 0x3001, 0xFEFF})
        CHECK_FALSE(pytext::isSpace(c));
}

TEST_CASE("pytext strip handles Unicode whitespace at both ends", "[pytext]") {
    CHECK(pytext::strip("  a b \t\n") == "a b");
    CHECK(pytext::strip("\xC2\xA0 a;\xE3\x80\x80\xE2\x80\xA8") == "a;");  // NBSP, ideographic space, line separator
    CHECK(pytext::strip("   ") == "");
    CHECK(pytext::strip("") == "");
    CHECK(pytext::rstrip("x; \xC2\xA0") == "x;");
    CHECK(pytext::lstrip(" \xC2\xA0x ") == "x ");
    CHECK(pytext::strip("caf\xC3\xA9 ") == "caf\xC3\xA9");  // a non-space two-byte character stays whole
}

TEST_CASE("pytext strip leaves malformed bytes alone", "[pytext]") {
    CHECK(pytext::rstrip("a\xC2") == "a\xC2");   // truncated sequence
    CHECK(pytext::rstrip("a\x80 ") == "a\x80");  // stray continuation byte, then a space
    CHECK(pytext::lstrip("\xFF a") == "\xFF a");
}

TEST_CASE("pytext split works on runs of whitespace", "[pytext]") {
    CHECK(pytext::split(".HELP  join \t x") == std::vector<std::string>{".HELP", "join", "x"});
    CHECK(pytext::split("a\xC2\xA0" "b\xE2\x80\x83" "c") == std::vector<std::string>{"a", "b", "c"});
    CHECK(pytext::split("   ").empty());
    CHECK(pytext::split("").empty());
    CHECK(pytext::split("solo") == std::vector<std::string>{"solo"});
}

TEST_CASE("pytext lowerAscii touches ASCII letters only", "[pytext]") {
    CHECK(pytext::lowerAscii(".Help JOIN \xC3\x89") == ".help join \xC3\x89");
}

TEST_CASE("pytext length and ljust count characters", "[pytext]") {
    CHECK(pytext::length("caf\xC3\xA9") == 4);
    CHECK(pytext::length("\xF0\x9F\x98\x80") == 1);
    CHECK(pytext::ljust("ab", 5) == "ab   ");
    CHECK(pytext::ljust("caf\xC3\xA9", 6) == "caf\xC3\xA9  ");
    CHECK(pytext::ljust("long", 2) == "long");
}

TEST_CASE("pytext startsWith and endsWith", "[pytext]") {
    CHECK(pytext::startsWith(".tables", "."));
    CHECK_FALSE(pytext::startsWith("", "."));
    CHECK(pytext::endsWith("a;", ";"));
    CHECK_FALSE(pytext::endsWith(";a", ";"));
    CHECK_FALSE(pytext::endsWith("", ";"));
}
