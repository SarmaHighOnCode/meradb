// cpp/tests/test_wb_width.cpp -- wb::displayWidth (own tables) gives the same answers as the terminal library's string_width.
#include <catch2/catch_test_macros.hpp>
#include <ftxui/screen/string.hpp>
#include "meradb/wb_width.h"
#include <cstdint>
#include <random>
#include <string>

namespace {

std::string utf8(std::uint32_t cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

}  // namespace

TEST_CASE("wb width: every code point up to U+10FFFF matches ftxui::string_width", "[wb][width]") {
    int mismatches = 0;
    std::uint32_t first = 0;
    for (std::uint32_t cp = 0; cp <= 0x10FFFF; ++cp) {
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;   // surrogates are not characters
        const std::string s = utf8(cp);
        if (meradb::wb::displayWidth(s) != ftxui::string_width(s)) {
            if (mismatches++ == 0) first = cp;
        }
    }
    INFO("first mismatch: U+" << std::hex << first);
    CHECK(mismatches == 0);
}

TEST_CASE("wb width: strings, invalid bytes and mixes match too", "[wb][width]") {
    for (const char* text : {"", "abc", "namaste", "\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87",
                             "\xE6\xBC\xA2\xE5\xAD\x97", "\xEF\xBC\xA1\xEF\xBC\xA2", "\xF0\x9F\x98\x80 x", "e\xCC\x81", "a\tb\nc\r",
                             "\xC3", "\x80\x80", "\xE2\x82", "\xF0\x9F", "\xFF\xFE", "\xF9\x80\x80\x80\x80"}) {
        INFO(text);
        CHECK(meradb::wb::displayWidth(text) == ftxui::string_width(text));
    }
    std::mt19937 rng(12345);
    for (int i = 0; i < 2000; ++i) {
        std::string s;
        const int n = static_cast<int>(rng() % 12);
        for (int k = 0; k < n; ++k) s += static_cast<char>(rng() & 0xFF);
        CAPTURE(i);
        CHECK(meradb::wb::displayWidth(s) == ftxui::string_width(s));
    }
}
