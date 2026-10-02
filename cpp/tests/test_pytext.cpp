// cpp/tests/test_pytext.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/pytext.h"
#include "golden_lower.h"
#include <cstdint>
#include <iterator>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

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

TEST_CASE("pytext lower matches Python's str.lower() on hand-picked cases", "[pytext]") {
    CHECK(pytext::lower(".Help JOIN") == ".help join");
    CHECK(pytext::lower("") == "");
    // final sigma, as CPython's handle_capital_sigma decides it (expected values come from Python 3.12)
    CHECK(pytext::lower("\xCE\x91\xCE\xA3") == "\xCE\xB1\xCF\x82");
    CHECK(pytext::lower("\xCE\x91\xCE\xA3\xCE\x91") == "\xCE\xB1\xCF\x83\xCE\xB1");
    CHECK(pytext::lower("\xCE\x91\xCE\xA3.") == "\xCE\xB1\xCF\x82.");
    CHECK(pytext::lower("\xCE\xA3") == "\xCF\x83");
    CHECK(pytext::lower("\xCE\x91\xCE\xA3 \xCE\x91") == "\xCE\xB1\xCF\x82 \xCE\xB1");
    CHECK(pytext::lower("\xCE\x91.\xCE\xA3") == "\xCE\xB1.\xCF\x82");
    CHECK(pytext::lower("\xCE\x91\xCE\xA3.\xCE\x91") == "\xCE\xB1\xCF\x83.\xCE\xB1");
    CHECK(pytext::lower("'\xCE\x91\xCE\xA3'") == "'\xCE\xB1\xCF\x82'");
    CHECK(pytext::lower("\xCE\x91\xCE\xA3\xCE\xA3") == "\xCE\xB1\xCF\x83\xCF\x82");
    CHECK(pytext::lower("\xCE\x91\xCE\xA3\xCC\x81") == "\xCE\xB1\xCF\x82\xCC\x81");
    CHECK(pytext::lower("\xCE\xA3\xCC\x81") == "\xCF\x83\xCC\x81");
    CHECK(pytext::lower("\xCE\xA3\xCE\x91\xCE\xA3") == "\xCF\x83\xCE\xB1\xCF\x82");
    CHECK(pytext::lower("\xCE\x91\xC2\xAD\xCE\xA3") == "\xCE\xB1\xC2\xAD\xCF\x82");
    CHECK(pytext::lower("\xCE\x91\xCE\xA3\xC2\xAD") == "\xCE\xB1\xCF\x82\xC2\xAD");
    CHECK(pytext::lower("1\xCE\xA3") == "1\xCF\x83");
    CHECK(pytext::lower("\xCE\x91\xCE\xA3" "1") == "\xCE\xB1\xCF\x82" "1");
    CHECK(pytext::lower("\xCE\x91\xC2\xAD\xCE\xA3\xC2\xAD" "B") == "\xCE\xB1\xC2\xAD\xCF\x83\xC2\xAD" "b");
    // U+0130, Kelvin sign, other special letters
    CHECK(pytext::lower("\xC4\xB0") == "i\xCC\x87");
    CHECK(pytext::lower("\xC4\xB0STANBUL") == "i\xCC\x87stanbul");
    CHECK(pytext::lower("\xE2\x84\xAAunji") == "kunji");
    CHECK(pytext::lower("\xC3\x89" "COLE") == "\xC3\xA9" "cole");
    CHECK(pytext::lower("\xC7\x85") == "\xC7\x86");
    CHECK(pytext::lower("\xE1\xBA\x9E") == "\xC3\x9F");
    CHECK(pytext::lower("\xE2\x85\xA7") == "\xE2\x85\xB7");
    CHECK(pytext::lower("\xF0\x90\x90\x80") == "\xF0\x90\x90\xA8");
    CHECK(pytext::lower("MIXED Case 123") == "mixed case 123");
}

TEST_CASE("pytext lower copies malformed bytes through and never reads out of range", "[pytext]") {
    CHECK(pytext::lower("A\x80" "B\xFF" "C\xC3") == "a\x80" "b\xFF" "c\xC3");
    CHECK(pytext::lower("\xCE\x91\xFF\xCE\xA3") == "\xCE\xB1\xFF\xCF\x83");  // an invalid byte is not "cased"
    CHECK(pytext::lower("\xED\xA0\x80Z") == "\xED\xA0\x80z");               // a surrogate stays three raw bytes
}

TEST_CASE("pytext lower agrees with Python on every code point", "[pytext]") {
    const auto encode = [](std::uint32_t cp) {
        std::string s;
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
        return s;
    };
    std::map<std::uint32_t, std::uint32_t> lowerOf;
    for (const auto& pair : kGoldenLower) lowerOf[pair[0]] = pair[1];
    const std::set<std::uint32_t> afterC(std::begin(kGoldenFinalAfterC), std::end(kGoldenFinalAfterC));
    const std::set<std::uint32_t> afterAC(std::begin(kGoldenFinalAfterAC), std::end(kGoldenFinalAfterAC));
    const std::string finalSigma = "\xCF\x82";

    std::size_t wrongLower = 0, wrongC = 0, wrongAC = 0;
    for (std::uint32_t cp = 0; cp < 0x110000; ++cp) {
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;
        const std::string text = encode(cp);
        const auto found = lowerOf.find(cp);
        const std::string expected = cp == 0x130 ? "i\xCC\x87" : encode(found == lowerOf.end() ? cp : found->second);
        if (pytext::lower(text) != expected) ++wrongLower;
        const std::string withC = pytext::lower(text + "\xCE\xA3");
        if (pytext::endsWith(withC, finalSigma) != (afterC.count(cp) != 0)) ++wrongC;
        const std::string withAC = pytext::lower("A" + text + "\xCE\xA3");
        if (pytext::endsWith(withAC, finalSigma) != (afterAC.count(cp) != 0)) ++wrongAC;
    }
    CHECK(wrongLower == 0);
    CHECK(wrongC == 0);
    CHECK(wrongAC == 0);
}

TEST_CASE("pytext decode follows Python's UTF-8 rules", "[pytext]") {
    char32_t cp = 0;
    const auto decodeAt = [&](const std::string& s, std::size_t at) { return pytext::decode(s, at, cp); };
    CHECK(decodeAt("\xC3\xA9", 0) == 2);
    CHECK(cp == 0xE9);
    CHECK(decodeAt("\xE2\x82\xAC", 0) == 3);
    CHECK(cp == 0x20AC);
    CHECK(decodeAt("\xF0\x9F\x98\x80", 0) == 4);
    CHECK(cp == 0x1F600);
    CHECK(decodeAt("\xF4\x8F\xBF\xBF", 0) == 4);  // U+10FFFF, the last one
    CHECK(cp == 0x10FFFF);
    CHECK(decodeAt("\xEF\xBF\xBD", 0) == 3);  // a genuine U+FFFD is three bytes
    CHECK(cp == 0xFFFD);
    // every one of these is malformed: ONE byte, U+FFFD
    for (const char* bad : {"\xC0\x80", "\xC1\xBF", "\xE0\x80\xA0", "\xE0\x9F\xBF", "\xF0\x80\x80\x80", "\xF0\x8F\xBF\xBF",
                            "\xED\xA0\x80", "\xED\xBF\xBF", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xF8\x88\x80\x80\x80",
                            "\xFF", "\x80", "\xBF", "\xC3", "\xE2\x82", "\xF0\x9F\x98", "\xC3\x28", "\xE2\x28\xA1"}) {
        cp = 0;
        CHECK(decodeAt(bad, 0) == 1);
        CHECK(cp == 0xFFFD);
    }
    // a lone lead byte at the very end is cut short, never read past
    CHECK(decodeAt("a\xE2", 1) == 1);
    CHECK(decodeAt("a\xE2\x82", 1) == 1);
}

TEST_CASE("pytext decodes every valid code point and no invalid one", "[pytext]") {
    std::size_t wrong = 0;
    for (std::uint32_t c = 0; c < 0x110000; ++c) {
        std::string s;
        if (c < 0x80) s = std::string(1, static_cast<char>(c));
        else if (c < 0x800) s = {static_cast<char>(0xC0 | (c >> 6)), static_cast<char>(0x80 | (c & 0x3F))};
        else if (c < 0x10000)
            s = {static_cast<char>(0xE0 | (c >> 12)), static_cast<char>(0x80 | ((c >> 6) & 0x3F)), static_cast<char>(0x80 | (c & 0x3F))};
        else
            s = {static_cast<char>(0xF0 | (c >> 18)), static_cast<char>(0x80 | ((c >> 12) & 0x3F)),
                 static_cast<char>(0x80 | ((c >> 6) & 0x3F)), static_cast<char>(0x80 | (c & 0x3F))};
        char32_t cp = 0;
        const std::size_t n = pytext::decode(s, 0, cp);
        const bool surrogate = c >= 0xD800 && c <= 0xDFFF;
        if (surrogate) {
            if (n != 1 || cp != 0xFFFD || pytext::length(s) != 3) ++wrong;  // three bad bytes: three units
        } else if (n != s.size() || cp != c || pytext::length(s) != 1) {
            ++wrong;
        }
    }
    CHECK(wrong == 0);
}

TEST_CASE("pytext length, ljust and strip agree on malformed input", "[pytext]") {
    CHECK(pytext::length("\x80") == 1);            // a lone continuation byte is one unit, as decode() says
    CHECK(pytext::length("\x80\x80\x80") == 3);
    CHECK(pytext::length("\xE0\x80\xA0") == 3);    // overlong: three bad bytes
    CHECK(pytext::length("\xED\xA0\x80") == 3);    // surrogate: three bad bytes
    CHECK(pytext::length("a\xC3") == 2);
    CHECK(pytext::length("\xF4\x90\x80\x80") == 4);
    CHECK(pytext::ljust("\x80", 3) == "\x80  ");
    CHECK(pytext::ljust("\xE0\x80\xA0", 4) == "\xE0\x80\xA0 ");
    CHECK(pytext::strip("\xE0\x80\xA0") == "\xE0\x80\xA0");  // the overlong NBSP-like bytes are not whitespace

    // random bytes: length() equals the number of decode() steps, and strip/split/lower equal a forward model
    std::mt19937 rng(12345);
    const unsigned char pool[] = {0x20, 0x41, 0x7F, 0x80, 0xA0, 0xBF, 0xC2, 0xC3, 0xE0, 0xE2, 0xED, 0xEF, 0xF0, 0xF4, 0xFF, 0x85, 0x09};
    for (int round = 0; round < 20000; ++round) {
        std::string text;
        const int n = static_cast<int>(rng() % 9);
        for (int i = 0; i < n; ++i) text += static_cast<char>(pool[rng() % sizeof(pool)]);
        std::vector<std::pair<std::string, bool>> units;  // piece, isSpace
        for (std::size_t at = 0; at < text.size();) {
            char32_t cp = 0;
            const std::size_t size = pytext::decode(text, at, cp);
            units.push_back({text.substr(at, size), pytext::isSpace(cp)});
            at += size;
        }
        std::size_t first = 0, last = units.size();
        while (first < last && units[first].second) ++first;
        while (last > first && units[last - 1].second) --last;
        std::string expectedStrip;
        for (std::size_t i = first; i < last; ++i) expectedStrip += units[i].first;
        REQUIRE(pytext::length(text) == units.size());
        REQUIRE(pytext::strip(text) == expectedStrip);
        REQUIRE(pytext::ljust(text, 12).size() == text.size() + (units.size() < 12 ? 12 - units.size() : 0));
        std::vector<std::string> words;
        std::string word;
        for (const auto& u : units) {
            if (u.second) {
                if (!word.empty()) words.push_back(word);
                word.clear();
            } else {
                word += u.first;
            }
        }
        if (!word.empty()) words.push_back(word);
        REQUIRE(pytext::split(text) == words);
        REQUIRE(pytext::lower(text).size() == text.size());  // none of these bytes change case or length
    }
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
