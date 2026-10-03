// cpp/tests/test_wb_sanity.cpp
#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

using namespace ftxui;

TEST_CASE("wbsanity FTXUI renders into an in-memory screen", "[wbsanity]") {
    auto screen = Screen::Create(Dimension::Fixed(20), Dimension::Fixed(3));
    Element row = hbox({text("ab"), text(" "), text("\xE6\x97\xA5\xE6\x9C\xAC") | bold});  // "ab 日本"
    Render(screen, row);
    CHECK(screen.PixelAt(0, 0).character == "a");
    CHECK(screen.PixelAt(3, 0).character == "\xE6\x97\xA5");
    CHECK(screen.PixelAt(3, 0).bold);
    CHECK_FALSE(screen.PixelAt(0, 0).bold);
}

TEST_CASE("wbsanity string_width counts terminal cells", "[wbsanity]") {
    CHECK(string_width("a") == 1);
    CHECK(string_width("\xC3\xA9") == 1);                 // é
    CHECK(string_width("\xE6\x97\xA5\xE6\x9C\xAC") == 4);  // 日本: two cells each
    CHECK(string_width("") == 0);
}

TEST_CASE("wbsanity events compare by their raw text", "[wbsanity]") {
    CHECK(Event::Special(std::string(1, '\x13')).input() == std::string(1, '\x13'));  // Ctrl+S
    CHECK(Event::F5 != Event::F6);
    CHECK(Event::ArrowUpCtrl != Event::ArrowUp);
    CHECK(Event::Character("x") == Event::Character('x'));
}
