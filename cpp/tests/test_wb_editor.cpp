#include <catch2/catch_test_macros.hpp>
#include "meradb/wb_editor.h"

using namespace meradb::wb;

TEST_CASE("wbeditor setText normalises newlines and puts the cursor at the end", "[wbeditor]") {
    TextBuffer b;
    b.setText("a\r\nbc\rd");
    CHECK(b.lineCount() == 3);
    CHECK(b.text() == "a\nbc\nd");
    CHECK(b.cursor() == Pos{2, 1});
    b.setText("");
    CHECK(b.empty());
    CHECK(b.cursor() == Pos{0, 0});
}

TEST_CASE("wbeditor insert, newline, backspace and delete", "[wbeditor]") {
    TextBuffer b;
    b.insert("abc");
    b.move(Move::Left, false);
    b.newline();                       // ab | c
    CHECK(b.text() == "ab\nc");
    CHECK(b.cursor() == Pos{1, 0});
    b.backspace();                     // joins the lines
    CHECK(b.text() == "abc");
    CHECK(b.cursor() == Pos{0, 2});
    b.del();                           // removes 'c'
    CHECK(b.text() == "ab");
    b.move(Move::DocStart, false);
    b.backspace();                     // nothing before the start
    CHECK(b.text() == "ab");
    b.insert("x\ny\tz\x01");           // tab -> 4 spaces, control dropped, newline splits
    CHECK(b.text() == "x\ny    zab");
}

TEST_CASE("wbeditor code points are columns", "[wbeditor]") {
    TextBuffer b;
    b.insert("\xC3\xA9\xF0\x9F\x98\x80x");   // é 😀 x
    CHECK(b.cursor() == Pos{0, 3});
    b.backspace();
    b.backspace();
    CHECK(b.text() == "\xC3\xA9");
    CHECK(displayColumn("\xC3\xA9\xE6\x97\xA5x", 2) == 3);   // é(1) 日(2) before x
}

TEST_CASE("wbeditor selection drives runnableText", "[wbeditor]") {
    TextBuffer b;
    b.setText("one;\ntwo;\nthree;");
    CHECK(b.runnableText() == "one;\ntwo;\nthree;");
    b.selectRange(Pos{0, 0}, Pos{1, 4});
    CHECK(b.hasSelection());
    CHECK(b.selectedText() == "one;\ntwo;");
    CHECK(b.runnableText() == "one;\ntwo;");
    b.insert("X");                           // typing replaces the selection
    CHECK(b.text() == "X\nthree;");
    CHECK_FALSE(b.hasSelection());
    b.selectAll();
    CHECK(b.selectedText() == "X\nthree;");
    b.move(Move::Left, false);               // collapses to the start of the selection
    CHECK(b.cursor() == Pos{0, 0});
    CHECK_FALSE(b.hasSelection());
}

TEST_CASE("wbeditor shift-moves extend, plain moves clear", "[wbeditor]") {
    TextBuffer b;
    b.setText("abcd\nefgh");
    b.moveTo(Pos{0, 1}, false);
    b.move(Move::Right, true);
    b.move(Move::Right, true);
    CHECK(b.selectedText() == "bc");
    b.move(Move::Down, true);                // the cursor was at column 3, so it lands on column 3 of the next row
    CHECK(b.selectedText() == "bcd\nefg");
    b.move(Move::Right, false);
    CHECK_FALSE(b.hasSelection());
    b.move(Move::End, true);
    CHECK(b.cursor() == Pos{1, 4});
}

TEST_CASE("wbeditor Up and Down remember the wanted column", "[wbeditor]") {
    TextBuffer b;
    b.setText("abcdef\nab\nabcdef");
    b.moveTo(Pos{0, 5}, false);
    b.move(Move::Down, false);
    CHECK(b.cursor() == Pos{1, 2});
    b.move(Move::Down, false);
    CHECK(b.cursor() == Pos{2, 5});
    b.move(Move::Left, false);     // a horizontal move resets the wanted column
    b.move(Move::Up, false);
    CHECK(b.cursor() == Pos{1, 2});
}

TEST_CASE("wbeditor word moves", "[wbeditor]") {
    TextBuffer b;
    b.setText("foo bar_1  baz");
    b.moveTo(Pos{0, 0}, false);
    b.move(Move::WordRight, false);
    CHECK(b.cursor() == Pos{0, 3});
    b.move(Move::WordRight, false);
    CHECK(b.cursor() == Pos{0, 9});
    b.move(Move::WordLeft, false);
    CHECK(b.cursor() == Pos{0, 4});
}

TEST_CASE("wbeditor row rendering: gutter, selection and cursor", "[wbeditor]") {
    TextBuffer b;
    b.setText("DIKHAO x");
    CHECK(gutterWidth(1) == 3);
    CHECK(gutterWidth(120) == 4);
    CHECK(gutterText(0, 1) == " 1 ");
    b.selectRange(Pos{0, 0}, Pos{0, 3});
    Line row = editorRowLine(b, 0, true);              // the cursor is at column 3 (the selection's end)
    CHECK(plainText(row) == "DIKHAO x");
    CHECK(row[0].text == "DIK");
    CHECK(row[0].style.bg == palette::kCurrentLine);
    CHECK(row[1].text == "H");
    CHECK(row[1].style.inverse);
    b.moveTo(Pos{0, 8}, false);                        // cursor past the end: one extra inverse cell
    Line end = editorRowLine(b, 0, true);
    CHECK(plainText(end) == "DIKHAO x ");
    CHECK(end.back().style.inverse);
    for (const Segment& s : editorRowLine(b, 0, false)) CHECK_FALSE(s.style.inverse);   // unfocused: no cursor
}
