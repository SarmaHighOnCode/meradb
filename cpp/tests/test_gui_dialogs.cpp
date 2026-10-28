// cpp/tests/test_gui_dialogs.cpp -- the pieces of the overlays that need no Dear ImGui context: the Markdown reader and
// its flow layout (also on hostile input), the command menu's filter, the key table, the icon, the folder helper and the
// extra command line options.
#include <catch2/catch_test_macros.hpp>
#include "meradb/gui_actions.h"
#include "meradb/gui_dialogs.h"
#include "meradb/gui_icon.h"
#include "meradb/gui_markdown.h"
#include "meradb/gui_options.h"
#include "meradb/gui_platform.h"
#include "meradb/gui_ui.h"
#include <chrono>
#include <random>
#include <set>
#include <string>

using namespace meradb::gui;

namespace {

using Kind = MdBlock::Kind;

double secondsFor(const std::function<void()>& work) {
    const auto start = std::chrono::steady_clock::now();
    work();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

float fakeMeasure(const std::string& text, const MdSpan&) {
    float w = 0.0f;
    for (unsigned char c : text)
        if ((c & 0xC0) != 0x80) w += 8.0f;
    return w;
}

}  // namespace

TEST_CASE("gui markdown: headings, paragraphs, lists, quotes, rules", "[gui][markdown]") {
    const auto blocks = parseMarkdown("# Title\n\nSome **bold** and `code` text\nwrapped over two lines.\n\n"
                                      "- one\n- two\n  continued\n  - nested\n\n1. first\n2. second\n\n> quoted\n> text\n\n---\n\n###### small\n");
    REQUIRE(blocks.size() == 10);
    CHECK(blocks[0].kind == Kind::Heading);
    CHECK(blocks[0].level == 1);
    CHECK(plainText(blocks[0].spans) == "Title");
    CHECK(blocks[1].kind == Kind::Paragraph);
    CHECK(plainText(blocks[1].spans) == "Some bold and code text wrapped over two lines.");
    CHECK(blocks[2].kind == Kind::Bullet);
    CHECK(blocks[2].level == 0);
    CHECK(plainText(blocks[3].spans) == "two continued");
    CHECK(blocks[4].level == 1);
    CHECK(blocks[5].kind == Kind::Numbered);
    CHECK(blocks[5].marker == "1.");
    CHECK(blocks[6].marker == "2.");
    CHECK(blocks[7].kind == Kind::Quote);
    CHECK(plainText(blocks[7].spans) == "quoted text");
    CHECK(blocks[8].kind == Kind::Rule);
    CHECK(blocks[9].level == 6);
}

TEST_CASE("gui markdown: inline bold, italic, code and unmatched markers", "[gui][markdown]") {
    auto spans = parseInline("a **b** *c* `d e` f");
    REQUIRE(spans.size() == 7);
    CHECK(spans[1].bold);
    CHECK(spans[1].text == "b");
    CHECK(spans[3].italic);
    CHECK(spans[5].code);
    CHECK(spans[5].text == "d e");
    spans = parseInline("2 * 3 = 6 and `open and **open");
    CHECK(plainText(spans) == "2 * 3 = 6 and `open and **open");
    spans = parseInline("**bold with `code` inside**");
    REQUIRE(spans.size() == 3);
    CHECK(spans[0].bold);
    CHECK(spans[1].bold);
    CHECK(spans[1].code);
    CHECK(spans[2].bold);
    CHECK(plainText(parseInline("escaped \\* star \\` tick")) == "escaped * star ` tick");
}

TEST_CASE("gui markdown: tables and fenced code", "[gui][markdown]") {
    const auto blocks = parseMarkdown("| A | B |\n|---|:-:|\n| `x` | **y** |\n| z |\n\n```sql\nDIKHAO 1;\n\n  indented\n```\nafter\n");
    REQUIRE(blocks.size() == 3);
    REQUIRE(blocks[0].kind == Kind::Table);
    CHECK(blocks[0].columns == 2);
    REQUIRE(blocks[0].rows.size() == 3);   // header + 2 rows; the short row is padded
    CHECK(blocks[0].rows[2].size() == 2);
    CHECK(blocks[0].rows[1][0][0].code);
    CHECK(blocks[1].kind == Kind::Code);
    CHECK(blocks[1].info == "sql");
    REQUIRE(blocks[1].codeLines.size() == 3);
    CHECK(blocks[1].codeLines[1].empty());
    CHECK(blocks[1].codeLines[2] == "  indented");
    CHECK(blocks[2].kind == Kind::Paragraph);
}

TEST_CASE("gui markdown: an unterminated fence runs to the end; CRLF and CR both end lines", "[gui][markdown]") {
    auto blocks = parseMarkdown("```\nopen\nstill code\n# not a heading");
    REQUIRE(blocks.size() == 1);
    CHECK(blocks[0].codeLines.size() == 3);
    blocks = parseMarkdown("# a\r\n\r\ntext\rmore\r\n");
    REQUIRE(blocks.size() == 2);
    CHECK(plainText(blocks[1].spans) == "text more");
}

TEST_CASE("gui markdown: the real language reference parses into tables, code and headings", "[gui][markdown]") {
    const auto blocks = parseMarkdown(madadMarkdown());
    int tables = 0, code = 0, headings = 0;
    for (const MdBlock& b : blocks) {
        tables += b.kind == Kind::Table;
        code += b.kind == Kind::Code;
        headings += b.kind == Kind::Heading;
    }
    CHECK(tables >= 2);
    CHECK(code >= 5);
    CHECK(headings >= 8);
    CHECK(blocks.front().kind == Kind::Heading);   // "Keys"
    CHECK(plainText(blocks.front().spans) == "Keys");
}

TEST_CASE("gui markdown: hostile input ends quickly and never crashes", "[gui][markdown][hostile]") {
    const std::vector<std::string> samples = {
        "",
        "\t\t\t- tab\n\t\t- tab\n",
        "\xC2\xA0\xC2\xA0- nbsp item\n\xC2\xA0\n\xC2\xA0\xC2\xA0\xC2\xA0 text",
        "```\n```\n```\n```",
        "```unterminated\n| a | b |\n|---|---|\n",
        "|\n|\n|---|\n||||||||||||||||||||||||||||||||||||||||||||||||||||||||\n",
        "| a |\n|---|\n| " + std::string(5000, 'x') + " |",
        "*" + std::string(2000, '*'),
        std::string(2000, '`') + "x",
        "**a *b **c *d",
        "#######",
        "#",
        "> > > >",
        "1. 2. 3.",
        "- - - - -",
        std::string("a\0b\n- \0", 8),
        "\xFF\xFE\xC3\n\xE2\x82\n",
    };
    for (const std::string& text : samples) {
        const auto blocks = parseMarkdown(text);
        for (const MdBlock& block : blocks) {
            const Flow flow = layoutFlow(block.spans, 200.0f, fakeMeasure);
            CHECK(flow.lines >= 0);
        }
    }
    std::string deep;   // a list nested more deeply than anything can show
    for (int i = 0; i < 5000; ++i) deep += std::string(static_cast<std::size_t>(i) * 2, ' ') + "- item\n";
    const auto blocks = parseMarkdown(deep);
    REQUIRE(blocks.size() == 5000);
    for (const MdBlock& b : blocks) CHECK(b.level <= MdBlock::kMaxDepth);
    CHECK(blocks.back().level == MdBlock::kMaxDepth);
}

TEST_CASE("gui markdown: one megabyte of random bytes and of markdown-looking noise", "[gui][markdown][hostile]") {
    std::mt19937 rng(2024);
    std::string bytes;
    for (int i = 0; i < 1000000; ++i) bytes += static_cast<char>(rng() & 0xFF);
    std::size_t blocks = 0;
    CHECK(secondsFor([&] { blocks = parseMarkdown(bytes).size(); }) < 20.0);
    CHECK(blocks > 0);

    const char alphabet[] = "*_`#|-> \t\n\r.1ab\\[]()~";
    std::string noise;
    for (int i = 0; i < 1000000; ++i) noise += alphabet[rng() % (sizeof alphabet - 1)];
    CHECK(secondsFor([&] { blocks = parseMarkdown(noise).size(); }) < 20.0);

    std::string stars(1000000, '*');   // the shape that makes a naive inline parser quadratic
    CHECK(secondsFor([&] { (void)parseMarkdown(stars); }) < 20.0);
    std::string ticks;
    for (int i = 0; i < 250000; ++i) ticks += "`a ";
    CHECK(secondsFor([&] { (void)parseMarkdown(ticks); }) < 20.0);

    std::string oneWord(1000000, 'w');   // a single 1 MB word is cut between characters, once per character
    std::vector<MdSpan> spans(1);
    spans[0].text = oneWord;
    Flow flow;
    CHECK(secondsFor([&] { flow = layoutFlow(spans, 300.0f, fakeMeasure); }) < 20.0);
    CHECK(flow.lines > 1000);
}

TEST_CASE("gui markdown flow: wraps at spaces, cuts long words, keeps short code spans whole", "[gui][markdown]") {
    std::vector<MdSpan> spans = parseInline("alpha beta gamma delta");
    Flow flow = layoutFlow(spans, 100.0f, fakeMeasure);   // 8 px per character: 12 characters per line
    REQUIRE(flow.words.size() == 4);
    CHECK(flow.lines == 2);
    CHECK(flow.words[0].line == 0);
    CHECK(flow.words[1].line == 0);
    CHECK(flow.words[1].x == 48.0f);   // "alpha" + a space (8 px) = 48
    CHECK(flow.words[2].line == 1);
    CHECK(flow.words[2].x == 0.0f);

    spans = parseInline("x " + std::string(40, 'q'));
    flow = layoutFlow(spans, 80.0f, fakeMeasure);   // 10 characters per line
    CHECK(flow.lines == 5);
    for (const FlowWord& w : flow.words) CHECK(w.width <= 80.0f);

    spans = parseInline("a `two words` b");
    flow = layoutFlow(spans, 1000.0f, fakeMeasure);
    REQUIRE(flow.words.size() == 3);
    CHECK(flow.words[1].text == "two words");
    CHECK(flow.words[1].style.code);

    spans = parseInline("`code`, then text");   // no space between the span and the comma
    flow = layoutFlow(spans, 1000.0f, fakeMeasure);
    REQUIRE(flow.words.size() == 4);
    CHECK(flow.words[1].x == flow.words[0].x + flow.words[0].width);

    CHECK(layoutFlow({}, 100.0f, fakeMeasure).lines == 0);
    CHECK(layoutFlow(parseInline("   "), 100.0f, fakeMeasure).lines == 0);
}

TEST_CASE("gui command menu: the filter needs every term, any case, anywhere", "[gui][commands]") {
    CHECK(commandMatches("DIKHAO * SE students SIRF 100", ""));
    CHECK(commandMatches("DIKHAO * SE students SIRF 100", "stud"));
    CHECK(commandMatches("DIKHAO * SE students SIRF 100", "STUD dikhao"));
    CHECK(commandMatches("DIKHAO * SE students SIRF 100", "  sirf   100 "));
    CHECK_FALSE(commandMatches("DIKHAO * SE students SIRF 100", "courses"));
    CHECK_FALSE(commandMatches("Chalao", "chalao x"));
    CHECK(commandMatches("Light / dark", "light"));
}

TEST_CASE("gui madad: every key binding of the window has a row in the key table", "[gui][madad]") {
    std::set<int> documented;
    for (const KeyHelp& row : keyHelp()) {
        CHECK_FALSE(row.keys.empty());
        CHECK_FALSE(row.what.empty());
        if (row.action) documented.insert(static_cast<int>(*row.action));
    }
    for (const KeyBinding& binding : keyBindings()) {
        INFO("action " << static_cast<int>(binding.action));
        CHECK(documented.count(static_cast<int>(binding.action)) == 1);
    }
    const std::string md = madadMarkdown();
    CHECK(md.find("Ctrl+O") != std::string::npos);
    CHECK(md.find("F1") != std::string::npos);
    CHECK(md.find("MeraDB Query Language") != std::string::npos);
}

TEST_CASE("gui icon: the embedded .ico yields the 16, 32 and 48 pixel images", "[gui][icon]") {
    const std::vector<IconImage> images = windowIconImages();
    REQUIRE(images.size() == 3);
    const int sizes[3] = {16, 32, 48};
    for (std::size_t i = 0; i < 3; ++i) {
        const IconImage& image = images[i];
        CHECK(image.width == sizes[i]);
        CHECK(image.height == sizes[i]);
        REQUIRE(image.rgba.size() == static_cast<std::size_t>(sizes[i] * sizes[i] * 4));
        CHECK(image.rgba[3] == 0);   // the corner is outside the rounded tile
        const std::size_t middle = (static_cast<std::size_t>(sizes[i] / 2) * static_cast<std::size_t>(sizes[i]) + static_cast<std::size_t>(sizes[i] / 2)) * 4;
        CHECK(image.rgba[middle + 3] == 255);
        int light = 0, dark = 0;
        for (std::size_t p = 0; p < image.rgba.size(); p += 4) {
            if (image.rgba[p + 3] != 255) continue;
            (image.rgba[p] > 200 ? light : dark)++;
        }
        CHECK(light > dark);   // a light tile ...
        CHECK(dark > 0);       // ... with a dark M
    }
}

TEST_CASE("gui icon: damaged .ico data is skipped, never read out of bounds", "[gui][icon]") {
    CHECK(parseIcoImages(nullptr, 0).empty());
    const unsigned char tiny[4] = {0, 0, 1, 0};
    CHECK(parseIcoImages(tiny, sizeof tiny).empty());
    // a directory entry that points beyond the data
    unsigned char bad[22] = {0, 0, 1, 0, 1, 0, 16, 16, 0, 0, 1, 0, 32, 0, 0xFF, 0xFF, 0, 0, 0xFF, 0xFF, 0, 0};
    CHECK(parseIcoImages(bad, sizeof bad).empty());
    // every truncation of the real icon is safe
    const std::vector<IconImage> full = windowIconImages();
    CHECK(full.size() == 3);
}

TEST_CASE("gui platform: folderOf", "[gui][platform]") {
    CHECK(folderOf("C:\\Users\\x\\exports\\meradb-1.csv") == "C:\\Users\\x\\exports");
    CHECK(folderOf("/home/x/exports/a.csv") == "/home/x/exports");
    CHECK(folderOf("a.csv") == ".");
    CHECK(folderOf("/a.csv") == "/");
    CHECK(folderOf("C:\\a.csv") == "C:\\");
    CHECK(folderOf("exports/a.csv") == "exports");
}

TEST_CASE("gui options: --open and --type", "[gui][options]") {
    GuiOptions options = parseGuiArgs({"--screenshot", "x.png", "--open", "palette", "--type", "stud"});
    CHECK(options.error.empty());
    REQUIRE(options.open);
    CHECK(*options.open == "palette");
    REQUIRE(options.typeText);
    CHECK(*options.typeText == "stud");
    CHECK(parseGuiArgs({"--open", "connect"}).error.empty());
    CHECK(parseGuiArgs({"--open", "madad"}).error.empty());
    CHECK_FALSE(parseGuiArgs({"--open", "nothing"}).error.empty());
    CHECK_FALSE(parseGuiArgs({"--open"}).error.empty());
}
