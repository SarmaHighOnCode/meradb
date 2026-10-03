#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
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

namespace {
bool sameRow(const Line& a, const Line& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].text != b[i].text || !(a[i].style == b[i].style)) return false;
    return true;
}

std::string inverseText(const Line& line) {
    std::string out;
    for (const Segment& s : line)
        if (s.style.inverse) out += s.text;
    return out;
}
}  // namespace



TEST_CASE("wbeditor the cursor covers a whole character, base letter and combining marks", "[wbeditor]") {
    TextBuffer b;
    b.setText("ae\xCC\x81" "b");  // a, e + U+0301, b: four code points
    b.moveTo(Pos{0, 1}, false);
    CHECK(inverseText(editorRowLine(b, 0, true)) == "e\xCC\x81");
    b.moveTo(Pos{0, 2}, false);  // between the letter and its mark: still the whole character
    Line row = editorRowLine(b, 0, true);
    CHECK(inverseText(row) == "e\xCC\x81");
    CHECK(plainText(row) == "ae\xCC\x81" "b");
    REQUIRE(row.size() == 3);
    CHECK(row[1].text == "e\xCC\x81");
    b.moveTo(Pos{0, 3}, false);
    CHECK(inverseText(editorRowLine(b, 0, true)) == "b");
    b.moveTo(Pos{0, 0}, false);
    CHECK(inverseText(editorRowLine(b, 0, true)) == "a");
    // a selection ending inside the character covers it whole too
    b.selectRange(Pos{0, 0}, Pos{0, 2});
    Line sel = editorRowLine(b, 0, false);
    REQUIRE(sel.size() == 2);
    CHECK(sel[0].text == "ae\xCC\x81");
    CHECK(sel[0].style.bg == palette::kCurrentLine);
    CHECK(sel[1].text == "b");
}

TEST_CASE("wbeditor a selection continuing to the next row marks the line break", "[wbeditor]") {
    TextBuffer b;
    b.setText("ab\n\ncd");
    b.selectRange(Pos{0, 1}, Pos{2, 1});
    Line first = editorRowLine(b, 0, false);
    CHECK(plainText(first) == "ab ");
    CHECK(first.back().style.bg == palette::kCurrentLine);
    Line empty = editorRowLine(b, 1, false);  // an empty selected line is visible too
    CHECK(plainText(empty) == " ");
    CHECK(empty[0].style.bg == palette::kCurrentLine);
    CHECK(plainText(editorRowLine(b, 2, false)) == "cd");  // the last row has no break after the selection
    b.selectRange(Pos{0, 1}, Pos{0, 2});
    CHECK(plainText(editorRowLine(b, 0, false)) == "ab");
}

TEST_CASE("wbeditor rows narrower than their text: the window equals the clipped row", "[wbeditor]") {
    TextBuffer b;
    b.setText("DIKHAO \xE6\x97\xA5\xE6\x9C\xAC x = 'it''s a string' -- c\xC3\xA9 and 12.5 count (y)");
    unsigned seed = 12345;
    auto next = [&]() { seed = seed * 1103515245u + 12345u; return static_cast<int>((seed >> 16) & 0x7fff); };
    for (int pass = 0; pass < 3; ++pass) {
        if (pass == 1) b.selectRange(Pos{0, 4}, Pos{0, 25});
        if (pass == 2) b.moveTo(Pos{0, 10}, false);
        for (bool focused : {true, false}) {
            for (int i = 0; i < 300; ++i) {
                const int skip = next() % 90, take = 1 + next() % 30;
                INFO("pass " << pass << " focused " << focused << " skip " << skip << " take " << take);
                CHECK(sameRow(editorRowWindow(b, 0, focused, skip, take), clipLine(editorRowLine(b, 0, focused), skip, take)));
            }
        }
    }
    b.moveTo(Pos{0, 1000}, false);  // cursor past the end: the extra cell is in the window that reaches it
    const int width = static_cast<int>(plainText(editorRowLine(b, 0, false)).size());
    CHECK(plainText(editorRowWindow(b, 0, true, 0, 1000)) == plainText(editorRowLine(b, 0, true)));
    CHECK(width > 0);
}

TEST_CASE("wbeditor edits keep the cached row data in step with the text", "[wbeditor]") {
    TextBuffer b;
    unsigned seed = 7;
    auto next = [&]() { seed = seed * 1103515245u + 12345u; return static_cast<int>((seed >> 16) & 0x7fff); };
    const char* pieces[] = {"a", "b c", "\xC3\xA9", "\xE6\x97\xA5", "\xF0\x9F\x98\x80", "e\xCC\x81", "\n", "x\ny", "12", "\xE2\x82", "\x80"};  // the last two merge into one character
    for (int step = 0; step < 4000; ++step) {
        switch (next() % 9) {
            case 0: case 1: case 2: b.insert(pieces[next() % 11]); break;
            case 3: b.backspace(); break;
            case 4: b.del(); break;
            case 5: b.move(static_cast<Move>(next() % 12), next() % 3 == 0); break;
            case 6: b.moveTo(Pos{next() % 5, next() % 12}, next() % 2 == 0); break;
            case 7: b.selectRange(Pos{next() % 5, next() % 12}, Pos{next() % 5, next() % 12}); break;
            default: if (next() % 6 == 0) b.setText(std::string(pieces[next() % 11]) + "\n" + pieces[next() % 11]); break;
        }
        TextBuffer fresh;
        fresh.setText(b.text());
        for (int r = 0; r < b.lineCount(); ++r) {
            INFO("step " << step << " row " << r);
            REQUIRE(b.lineLength(r) == fresh.lineLength(r));
            for (int c = 12; c >= 0; --c) REQUIRE(b.byteOffset(r, c) == fresh.byteOffset(r, c));
        }
    }
}

TEST_CASE("wbeditor typing and moving on a two megabyte line stays fast", "[wbeditor]") {
    std::string big;
    while (big.size() < 2000000) big += "x = 12 'ab' ";
    TextBuffer b;
    b.setText(big);
    const int mid = b.lineLength(0) / 2;
    b.moveTo(Pos{0, mid}, false);
    const auto t0 = std::chrono::steady_clock::now();
    int inserted = 0;
    for (int i = 0; i < 600; ++i) {
        b.insert("q");
        ++inserted;
        if (i % 3 == 1) { b.backspace(); --inserted; }
        if (i % 5 == 2) b.move(Move::Left, false);
        if (i % 7 == 3) b.move(Move::WordRight, false);
        if (i % 50 == 0) {  // a render costs a scan of the row (tens of ms on 2 MB), so not after every key
            const Line row = editorRowWindow(b, 0, true, displayColumn(b.line(0), b.cursor().col) - 40, 100);
            CHECK(plainText(row).size() == 100);
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(b.line(0).size() == big.size() + static_cast<std::size_t>(inserted));
    CHECK(ms < 5000);  // about a tenth of a second where this was written; the old per-edit rescans took 24 s
    INFO("600 edits and 12 rendered windows: " << ms << " ms");
}
