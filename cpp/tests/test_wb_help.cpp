// cpp/tests/test_wb_help.cpp
#include "meradb/wb_help.h"
#include "meradb/wb_markdown.h"
#include "test_util.h"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>

using namespace meradb::wb;

namespace {

std::vector<std::string> plain(const std::vector<Line>& lines) {
    std::vector<std::string> out;
    for (const Line& l : lines) out.push_back(plainText(l));
    return out;
}

using V = std::vector<std::string>;

}  // namespace

TEST_CASE("wbhelp heading and paragraph wrap", "[wbhelp]") {
    const auto lines = renderMarkdown("# Title\n\nalpha beta gamma delta", 12);
    CHECK(plain(lines) == V({"Title", "", "alpha beta", "gamma delta"}));
    REQUIRE_FALSE(lines[0].empty());
    CHECK(lines[0][0].style.bold);
    CHECK(lines[0][0].style.fg == palette::kPink);
}

TEST_CASE("wbhelp a heading is a block", "[wbhelp]") {
    const auto lines = renderMarkdown("## Sub\ntext", 40);
    CHECK(plain(lines) == V({"Sub", "", "text"}));
    CHECK(lines[0][0].style.fg == palette::kCyan);
    const auto h3 = renderMarkdown("### Plain", 40);
    CHECK(h3[0][0].style.bold);
    CHECK(h3[0][0].style.fg == -1);
}

TEST_CASE("wbhelp bullets with inline bold and code", "[wbhelp]") {
    const auto lines = renderMarkdown("- **x** and `y`\n- second", 40);
    CHECK(plain(lines) == V({"\xE2\x80\xA2 x and y", "\xE2\x80\xA2 second"}));
    bool boldX = false, cyanY = false;
    for (const Segment& s : lines[0]) {
        if (s.text == "x" && s.style.bold) boldX = true;
        if (s.text == "y" && s.style.fg == palette::kCyan) cyanY = true;
    }
    CHECK(boldX);
    CHECK(cyanY);
}

TEST_CASE("wbhelp bullets use a hanging indent", "[wbhelp]") {
    CHECK(plain(renderMarkdown("- aaa bbb ccc", 8)) == V({"\xE2\x80\xA2 aaa", "  bbb", "  ccc"}));
}

TEST_CASE("wbhelp numbered items keep their numbers", "[wbhelp]") {
    CHECK(plain(renderMarkdown("1. one\n2. two", 40)) == V({"1. one", "2. two"}));
}

TEST_CASE("wbhelp tables align and keep header and cell styles", "[wbhelp]") {
    const auto lines = renderMarkdown("| Key | Kaam |\n|-----|------|\n| **F5** | chalao |", 80);
    CHECK(plain(lines) == V({"Key \xE2\x94\x82 Kaam",
                             "\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\xBC\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80",
                             "F5  \xE2\x94\x82 chalao"}));
    for (const Segment& s : lines[0])
        if (s.text == "Key" || s.text == "Kaam") CHECK(s.style.bold);
    bool boldF5 = false;
    for (const Segment& s : lines[2])
        if (s.text == "F5" && s.style.bold) boldF5 = true;
    CHECK(boldF5);
}

TEST_CASE("wbhelp fences are verbatim, unwrapped and on the line background", "[wbhelp]") {
    const auto lines = renderMarkdown("```\nSELECT 1;\n  x\n```", 40);
    CHECK(plain(lines) == V({"SELECT 1;", "  x"}));
    for (const Line& l : lines)
        for (const Segment& s : l) CHECK(s.style.bg == palette::kCurrentLine);
    const auto longLine = renderMarkdown("```\n0123456789ABCDEF\n```", 5);
    CHECK(plain(longLine) == V({"0123456789ABCDEF"}));
}

TEST_CASE("wbhelp quotes and rules", "[wbhelp]") {
    const auto q = renderMarkdown("> hi", 40);
    CHECK(plain(q) == V({"\xE2\x96\x8C hi"}));
    for (const Segment& s : q[0]) CHECK(s.style.dim);
    CHECK(plain(renderMarkdown("---", 5)) == V({"\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80\xE2\x94\x80"}));
}

TEST_CASE("wbhelp CRLF equals LF and degenerate input is safe", "[wbhelp]") {
    const std::string lf = "# T\n\npara one\ntwo\n\n- a\n- b\n";
    std::string crlf;
    for (char c : lf) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    CHECK(plain(renderMarkdown(crlf, 20)) == plain(renderMarkdown(lf, 20)));
    CHECK(renderMarkdown("", 10).empty());
    CHECK_NOTHROW(renderMarkdown("abc def\n- x y z\n| a | b |\n|--|--|\n| 1 | 2 |", 1));
    CHECK_NOTHROW(renderMarkdown("abc", 0));
    CHECK_NOTHROW(renderMarkdown("**unclosed `code and *star", 7));
    CHECK(plain(renderMarkdown("**unclosed", 40)) == V({"**unclosed"}));
}

TEST_CASE("wbhelp key table is verbatim plus the extra section", "[wbhelp]") {
    const std::string keys = keysHelpMarkdown();
    CHECK(keys.rfind("# MeraDB Workbench: Madad", 0) == 0);
    CHECK(keys.find("| **F5** / **Ctrl+R** | Query chalao (selected text only, if something is selected) |") != std::string::npos);
    CHECK(keys.find("| **F6** | SAMJHAO: query plan dikhao (index / scan / join), bina chalaye |") != std::string::npos);
    CHECK(keys.find("| **Ctrl+Up / Ctrl+Down** | Pichli / agli query (history) |") != std::string::npos);
    CHECK(keys.find("| **Ctrl+Q** | Bahar niklo |") != std::string::npos);
    CHECK(keys.find("## Extra keys (C++ version)") != std::string::npos);
}

TEST_CASE("wbhelp the language text is appended and the fallback renders", "[wbhelp]") {
    const std::string md = helpMarkdownWith("X");
    CHECK(md.size() >= 2);
    CHECK(md.substr(md.size() - 2) == "\nX");
    const auto lines = plain(renderMarkdown(helpMarkdownWith("*(docs/LANGUAGE.md nahi mila)*"), 80));
    CHECK(lines.back() == "(docs/LANGUAGE.md nahi mila)");
}

TEST_CASE("wbhelp the embedded language reference renders cleanly", "[wbhelp]") {
    REQUIRE(languageDocEmbedded());
    std::string expected = meradb_test::readText(MERADB_LANGUAGE_MD);
    expected.erase(std::remove(expected.begin(), expected.end(), '\r'), expected.end());
    CHECK(languageDoc() == expected);

    const auto lines = plain(renderHelp(80));
    CHECK(std::find(lines.begin(), lines.end(), "MeraDB Query Language (MQL): Reference") != lines.end());
    bool cheat = false, banao = false;
    for (const std::string& l : lines) {
        if (l.find("Keyword \xE2\x86\x94 SQL cheat sheet") != std::string::npos) cheat = true;
        if (l.find("BANAO") != std::string::npos && l.find("CREATE") != std::string::npos) banao = true;
        CHECK(l.find("**") == std::string::npos);
        CHECK(l.rfind("|", 0) != 0);
    }
    CHECK(cheat);
    CHECK(banao);
}
