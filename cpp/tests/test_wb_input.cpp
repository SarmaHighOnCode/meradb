// cpp/tests/test_wb_input.cpp -- the surrogate-pair joiner used by the patched Windows input loop (see cmake/patch_ftxui.cmake).
#include <catch2/catch_test_macros.hpp>
#include "ftxui_win_input.h"

using ftxui_patch::SurrogateJoiner;

TEST_CASE("wbinput a surrogate pair becomes one UTF-8 character", "[wbinput]") {
    SurrogateJoiner j;
    CHECK(j.push(0xD83D).empty());
    CHECK(j.pending());
    CHECK(j.push(0xDE00) == "\xF0\x9F\x98\x80");   // U+1F600
    CHECK_FALSE(j.pending());
}

TEST_CASE("wbinput BMP units pass through as UTF-8", "[wbinput]") {
    SurrogateJoiner j;
    CHECK(j.push('a') == "a");
    CHECK(j.push(0x00E9) == "\xC3\xA9");
    CHECK(j.push(0x0928) == "\xE0\xA4\xA8");
    CHECK(j.push(0x65E5) == "\xE6\x97\xA5");
    CHECK(j.push(0) == std::string(1, '\0'));
    CHECK(j.push(0xE000) == "\xEE\x80\x80");   // just past the surrogates
}

TEST_CASE("wbinput orphan halves are dropped", "[wbinput]") {
    SurrogateJoiner j;
    CHECK(j.push(0xDE00).empty());                 // low half alone
    CHECK(j.push('b') == "b");
    CHECK(j.push(0xD83D).empty());                 // high half, then an ordinary character
    CHECK(j.push('c') == "c");
    CHECK_FALSE(j.pending());
    CHECK(j.push(0xDE00).empty());                 // that high half is gone: this low half is an orphan
    CHECK(j.push(0xD83D).empty());                 // two high halves in a row: the second one wins
    CHECK(j.push(0xD83E).empty());
    CHECK(j.push(0xDD70) == "\xF0\x9F\xA5\xB0");   // U+1F970
}

TEST_CASE("wbinput the pseudo console's Alt+numpad form", "[wbinput]") {
    // What ConPTY sends for U+1F600: Alt down, numpad digits (no character), then the halves on the Alt key-up.
    SurrogateJoiner j;
    CHECK_FALSE(SurrogateJoiner::wantsKeyEvent(false, 0x41, 'a'));          // an ordinary key-up is still ignored
    CHECK_FALSE(SurrogateJoiner::wantsKeyEvent(false, 0x12, 0));            // Alt key-up without a character
    CHECK_FALSE(SurrogateJoiner::wantsKeyEvent(false, 0x12, 'a'));
    CHECK_FALSE(SurrogateJoiner::wantsKeyEvent(false, 0x41, 0xD83D));       // a surrogate on another key-up: not this form
    CHECK(SurrogateJoiner::wantsKeyEvent(true, 0x41, 'a'));
    CHECK(SurrogateJoiner::wantsKeyEvent(true, 0, 0xD83D));                 // key-downs always count
    REQUIRE(SurrogateJoiner::wantsKeyEvent(false, 0x12, 0xD83D));
    CHECK(j.push(0xD83D).empty());
    CHECK(j.push(0) == std::string(1, '\0'));                               // Alt / numpad key-downs in between
    CHECK(j.push(0) == std::string(1, '\0'));
    REQUIRE(SurrogateJoiner::wantsKeyEvent(false, 0x12, 0xDE00));
    CHECK(j.push(0xDE00) == "\xF0\x9F\x98\x80");
}

TEST_CASE("wbinput the highest and lowest pairs", "[wbinput]") {
    SurrogateJoiner j;
    j.push(0xD800);
    CHECK(j.push(0xDC00) == "\xF0\x90\x80\x80");   // U+10000
    j.push(0xDBFF);
    CHECK(j.push(0xDFFF) == "\xF4\x8F\xBF\xBF");   // U+10FFFF
    j.push(0xD83D);
    j.reset();
    CHECK_FALSE(j.pending());
}
