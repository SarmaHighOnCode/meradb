// cpp/tests/test_ws_text.cpp -- user text that Python strips or splits with str methods (Unicode whitespace):
// CHECK text, the server's verbose query log and int() on a port. Every expected value was produced by running
// Python (parse(), ' '.join(text.split())[:200], int()) on the same text.
#include <catch2/catch_test_macros.hpp>
#include "meradb/cli.h"
#include "meradb/parser.h"
#include "server_fixture.h"
#include <string>

using namespace meradb;
using namespace meradb::ast;
using namespace meradb_test;

namespace {

std::string checkText(const std::string& body) {
    auto stmts = parseScript("BANAO TABLE t (a INT SHART (" + body + "));");
    auto* ct = dynamic_cast<CreateTable*>(stmts.at(0).get());
    REQUIRE(ct != nullptr);
    REQUIRE(ct->columns.at(0).check.has_value());
    return *ct->columns[0].check;
}

}  // namespace

TEST_CASE("ws_text CHECK text loses trailing Unicode whitespace like Python's rstrip", "[parser][unicode]") {
    // NBSP, U+3000, U+2028, U+0085, U+001C, tab, VT, FF
    const char* const spaces[] = {"\xC2\xA0", "\xE3\x80\x80", "\xE2\x80\xA8", "\xC2\x85", "\x1c", "\t", "\x0b", "\x0c"};
    for (const char* space : spaces) {
        const std::string s = space;
        INFO("whitespace of " << s.size() << " byte(s), first byte " << static_cast<int>(static_cast<unsigned char>(s[0])));
        CHECK(checkText("a > 0" + s) == "a > 0");
        CHECK(checkText("a > 0" + s + s + s) == "a > 0");
        CHECK(checkText(s + "a > 0" + s + s) == "a > 0");  // the slice starts at the first token
        CHECK(checkText("a" + s + ">" + s + "0" + s) == "a" + s + ">" + s + "0");  // inside stays as typed
    }
    CHECK(checkText("a > 0 ") == "a > 0");
    CHECK(checkText("a > 0\r\n") == "a > 0");
}

TEST_CASE("ws_text U+FEFF is skipped by the tokenizer but is not whitespace to rstrip", "[parser][unicode]") {
    CHECK(checkText("a > 0\xEF\xBB\xBF\xEF\xBB\xBF") == "a > 0\xEF\xBB\xBF\xEF\xBB\xBF");
}

TEST_CASE("ws_text the verbose query log splits on Unicode whitespace like Python", "[server][unicode]") {
    RunningServer s("", /*verbose=*/true);
    RawClient c(s.port());
    c.hello();
    c.query("DIKHAO\xC2\xA0\xC2\xA0TABLES\xE2\x80\xA8;\xE3\x80\x80\xC2\x85x");  // an error is fine: it is logged first
    CHECK(s.logContains("[main]  DIKHAO TABLES ; x"));
    // ' '.join(text.split())[:200]: 150 e-acute, one space, then 49 more characters
    const std::string acute = "\xC3\xA9";
    std::string text, expected;
    for (int i = 0; i < 150; ++i) text += acute;
    text += "\xC2\xA0\xE3\x80\x80\x1c";
    for (int i = 0; i < 100; ++i) text += acute;
    c.query(text);
    for (int i = 0; i < 150; ++i) expected += acute;
    expected += ' ';
    for (int i = 0; i < 49; ++i) expected += acute;
    bool found = false;
    for (const auto& line : s.logLines()) {
        const std::string tail = "[main]  " + expected;
        if (line.size() >= tail.size() && line.compare(line.size() - tail.size(), tail.size(), tail) == 0) found = true;
    }
    CHECK(found);
}

TEST_CASE("ws_text --port accepts the whitespace int() strips and nothing else", "[cli][unicode]") {
    CHECK(parseCliArgs({"server", "--port", "80\xC2\xA0"}).port.value() == 80);
    CHECK(parseCliArgs({"server", "--port", "\xE3\x80\x80 81\xE2\x80\xA8"}).port.value() == 81);
    CHECK(parseCliArgs({"server", "--port", "\xC2\x85\t82\x0b\x0c"}).port.value() == 82);
    // U+001C..U+001F are whitespace to str.isspace() but int() does not strip them
    CHECK_FALSE(parseCliArgs({"server", "--port", "83\x1c"}).error.empty());
    CHECK_FALSE(parseCliArgs({"server", "--port", "\x1f" "84"}).error.empty());
    CHECK_FALSE(parseCliArgs({"server", "--port", "8\xC2\xA0" "5"}).error.empty());  // inside is not stripped
    CHECK_FALSE(parseCliArgs({"server", "--port", "\xC2\xA0"}).error.empty());
}
