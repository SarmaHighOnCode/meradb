// cpp/tests/test_wb_form.cpp -- LineEdit and ConnectForm (no terminal involved).
#include <catch2/catch_test_macros.hpp>
#include "meradb/wb_form.h"

using namespace meradb::wb;

TEST_CASE("wbform LineEdit edits by code points", "[wbform]") {
    LineEdit e;
    e.insert("h\xC3\xA9llo");   // héllo
    CHECK(e.text() == "h\xC3\xA9llo");
    CHECK(e.length() == 5);
    CHECK(e.cursor() == 5);
    e.left();
    e.left();
    e.backspace();   // the cursor is at 3: removes the first l
    CHECK(e.text() == "h\xC3\xA9lo");
    e.home();
    CHECK(e.cursor() == 0);
    e.backspace();   // nothing before the start
    CHECK(e.text() == "h\xC3\xA9lo");
    e.right();
    e.del();         // removes the é
    CHECK(e.text() == "hlo");
    e.end();
    CHECK(e.cursor() == 3);
    e.del();         // nothing after the end
    CHECK(e.text() == "hlo");
    for (int i = 0; i < 10; ++i) e.left();
    CHECK(e.cursor() == 0);
    for (int i = 0; i < 10; ++i) e.right();
    CHECK(e.cursor() == 3);
}

TEST_CASE("wbform LineEdit backspace removes a whole multi-byte character", "[wbform]") {
    LineEdit e;
    e.insert("a\xF0\x9F\x98\x80" "b");
    e.left();
    e.backspace();
    CHECK(e.text() == "ab");
    CHECK(e.cursor() == 1);
}

TEST_CASE("wbform LineEdit digitsOnly, passwords and control characters", "[wbform]") {
    LineEdit port;
    port.digitsOnly = true;
    port.insert("a");
    CHECK(port.text().empty());
    port.insert("12ab3");
    CHECK(port.text() == "123");
    CHECK(port.cursor() == 3);
    port.setText("6x3y7");
    CHECK(port.text() == "637");

    LineEdit pw("h\xC3\xA9llo", true);
    CHECK(pw.shown() == "*****");
    CHECK(pw.text() == "h\xC3\xA9llo");
    CHECK(pw.isPassword());

    LineEdit plain;
    plain.insert("a\tb\nc\x1b" "d");
    CHECK(plain.text() == "abcd");
    plain.insert("");
    CHECK(plain.text() == "abcd");
}

TEST_CASE("wbform ConnectForm navigation and actions", "[wbform]") {
    ConnectForm form("127.0.0.1", "6372");
    CHECK(form.active() == 0);
    CHECK(form.field(0).text() == "127.0.0.1");
    CHECK(form.field(1).text() == "6372");
    CHECK(form.field(1).digitsOnly);
    CHECK(form.field(2).isPassword());
    for (int i = 0; i < 7; ++i) form.next();
    CHECK(form.active() == 0);
    form.previous();
    CHECK(form.active() == 6);
    form.setActive(2);
    CHECK(form.enter() == FormAction::Connect);   // any field submits
    form.setActive(3);
    CHECK(form.enter() == FormAction::Connect);
    form.setActive(4);
    CHECK(form.enter() == FormAction::Connect);
    form.setActive(5);
    CHECK(form.enter() == FormAction::Local);
    form.setActive(6);
    CHECK(form.enter() == FormAction::Cancel);
    CHECK(form.escape() == FormAction::Cancel);
    form.setActive(99);
    CHECK(form.active() == 6);
    form.setActive(-4);
    CHECK(form.active() == 0);
}

TEST_CASE("wbform ConnectForm builds the request like tui.py _choice", "[wbform]") {
    ConnectForm form("127.0.0.1", "6372");
    form.field(2).insert("pw");
    form.field(3).insert(" db ");
    ConnectRequest r = form.request(false);
    CHECK_FALSE(r.local);
    CHECK(r.host == "127.0.0.1");
    CHECK(r.port == "6372");
    REQUIRE(r.password.has_value());
    CHECK(*r.password == "pw");
    REQUIRE(r.database.has_value());
    CHECK(*r.database == "db");

    form.field(0).setText("");
    form.field(1).setText("");
    r = form.request(true);
    CHECK(r.local);
    CHECK(r.host == "127.0.0.1");   // defaults when empty
    CHECK(r.port == "6372");

    ConnectForm blank("example.org", "7000");
    r = blank.request(false);
    CHECK(r.host == "example.org");
    CHECK(r.port == "7000");
    CHECK_FALSE(r.password.has_value());
    CHECK_FALSE(r.database.has_value());
}

TEST_CASE("wbform a field that gets the focus selects its text; typing replaces it", "[wbform]") {
    ConnectForm form("127.0.0.1", "6372");
    CHECK(form.field(0).selected());          // the first field starts with the focus
    CHECK_FALSE(form.field(1).selected());
    form.next();
    REQUIRE(form.active() == 1);
    CHECK(form.field(1).selected());
    form.field(1).insert("47831");
    CHECK(form.field(1).text() == "47831");   // 6372 replaced, not appended to
    CHECK_FALSE(form.field(1).selected());
    form.field(1).insert("0");
    CHECK(form.field(1).text() == "478310");  // the selection is gone after the first character
    // Empty fields have nothing to select; the password field too.
    form.next();
    CHECK_FALSE(form.field(2).selected());
    form.field(2).insert("pw");
    CHECK(form.field(2).text() == "pw");
    // Coming back selects again (the cursor was somewhere in the middle).
    form.previous();
    form.previous();
    REQUIRE(form.active() == 0);
    CHECK(form.field(0).selected());
    form.field(0).left();                     // a cursor key ends the selection: Left goes to the start
    CHECK_FALSE(form.field(0).selected());
    CHECK(form.field(0).cursor() == 0);
    form.field(0).insert("x");
    CHECK(form.field(0).text() == "x127.0.0.1");
    form.next();
    form.previous();
    CHECK(form.field(0).selected());
    form.field(0).right();                    // Right goes to the end
    CHECK(form.field(0).cursor() == form.field(0).length());
    CHECK_FALSE(form.field(0).selected());
    form.next();
    form.previous();
    form.field(0).home();
    CHECK_FALSE(form.field(0).selected());
    CHECK(form.field(0).cursor() == 0);
    form.next();
    form.previous();
    form.field(0).end();
    CHECK_FALSE(form.field(0).selected());
    CHECK(form.field(0).cursor() == form.field(0).length());
}

TEST_CASE("wbform Backspace and Delete remove a selection; a rejected character keeps it", "[wbform]") {
    ConnectForm form("example.org", "7000");
    form.field(0).backspace();
    CHECK(form.field(0).text().empty());
    form.previous();   // buttons wrap
    form.setActive(0);
    form.field(0).insert("abc");
    form.setActive(1);
    form.setActive(0);
    CHECK(form.field(0).selected());
    form.field(0).del();
    CHECK(form.field(0).text().empty());
    form.setActive(1);
    CHECK(form.field(1).selected());
    form.field(1).insert("xyz");              // nothing a digits-only field accepts: the selected text stays
    CHECK(form.field(1).text() == "7000");
    CHECK(form.field(1).selected());
    form.field(1).insert("\t");
    CHECK(form.field(1).text() == "7000");
    form.setActive(1);                        // no change of focus: the selection is not renewed or lost
    CHECK(form.field(1).selected());
    form.field(1).setText("12");
    CHECK_FALSE(form.field(1).selected());
}