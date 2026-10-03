// cpp/tests/test_wb_panels.cpp -- the four FTXUI panels, rendered into a Screen.
#include "wb_screen_util.h"
#include "wb_test_util.h"
#include <catch2/catch_test_macros.hpp>
#include "meradb/wb_keys.h"
#include "meradb/wb_panels.h"
#include <chrono>

using namespace meradb;
using namespace meradb::wb;
using namespace ftxui;
using wbtest::FakeBackend;
using wbtest::ManualPoster;
using wbtest::tableResult;

namespace {

nlohmann::ordered_json sampleTree() {
    return nlohmann::ordered_json::parse(R"([
      {"name":"college","current":false,"tables":[]},
      {"name":"main","current":true,"tables":[
         {"name":"students","columns":[
            {"name":"id","type_name":"INT","primary_key":true,"unique":false,"not_null":true},
            {"name":"naam","type_name":"TEXT","primary_key":false,"unique":false,"not_null":true}]},
         {"name":"marks","columns":[{"name":"score","type_name":"FLOAT","primary_key":false,"unique":false,"not_null":false}]}]}])");
}

struct Rig {
    FakeBackend* fake;
    ManualPoster poster;
    std::unique_ptr<Session> session;
    explicit Rig(const std::function<void(FakeBackend&)>& setup = {}) {
        auto owned = std::make_unique<FakeBackend>();
        fake = owned.get();
        owned->tree = sampleTree();
        if (setup) setup(*owned);
        session = std::make_unique<Session>(std::move(owned), SessionOptions(), poster.poster());
        REQUIRE(poster.pumpIdle(*session));
    }
    Session& s() { return *session; }
    void run(const std::string& text) {
        session->runText(text);
        REQUIRE(poster.pumpIdle(*session));
    }
};

std::vector<std::string> draw(PanelView& panel, int w, int h, Screen* keep = nullptr) {
    Element e = panel.Render();
    e = e | size(WIDTH, EQUAL, w);
    e = e | size(HEIGHT, EQUAL, h);
    return wbtest::renderLines(e, w, h, keep);
}

std::string dump(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& l : lines) out += l + '\n';
    return out;
}

Result resultsOf3() {
    return tableResult({"id", "price", "name", "flag"},
                       {{Value(int64_t(1)), Value(8.5), Value(std::string("alpha")), Value(true)},
                        {Value(int64_t(22)), Value(), Value(std::string("beta")), Value(false)},
                        {Value(int64_t(333)), Value(0.25), Value(std::string("gamma")), Value()}});
}

}  // namespace

TEST_CASE("wbui keys are table driven and exact", "[wbui]") {
    CHECK(keys::isRun(Event::F5));
    CHECK(keys::isRun(keys::ctrl('R')));
    CHECK(keys::isHistoryPrev(Event::ArrowUpCtrl));
    CHECK(keys::isHistoryPrev(keys::ctrl('P')));
    CHECK(keys::isHistoryNext(Event::ArrowDownCtrl));
    CHECK(keys::isHistoryNext(keys::ctrl('N')));
    CHECK(keys::ctrl('S').input() == "\x13");
    CHECK(keys::ctrl('Q').input() == "\x11");
    CHECK(keys::isExport(keys::ctrl('S')));
    CHECK_FALSE(keys::isQuit(keys::ctrl('S')));
    CHECK_FALSE(keys::isExplain(Event::F5));
    CHECK(keys::isExplain(Event::F6));
    CHECK(keys::isHelp(Event::F1));
    CHECK(keys::isConnect(keys::ctrl('O')));
    CHECK(keys::isClearLog(keys::ctrl('L')));
    CHECK(keys::isInterrupt(keys::ctrl('C')));
    CHECK(keys::isSelectAll(keys::ctrl('A')));
    REQUIRE(keys::findEditorKey(Event::Special(keys::kShiftRight)) != nullptr);
    CHECK(keys::findEditorKey(Event::Special(keys::kShiftRight))->extendSelection);
    CHECK(keys::findEditorKey(Event::ArrowRight)->move == Move::Right);
    CHECK(keys::findEditorKey(Event::F5) == nullptr);
    // Every global key is distinct from every editor cursor key.
    for (const keys::Binding& b : keys::bindings())
        for (const Event& e : b.events) CHECK(keys::findEditorKey(e) == nullptr);
}

TEST_CASE("wbui lineToElement keeps styles and survives a coloured frame", "[wbui]") {
    Line line;
    appendSegment(line, "DIKHAO", fgStyle(palette::kPink, true));
    appendSegment(line, " x", Style());
    Screen screen = Screen::Create(Dimension::Fixed(20), Dimension::Fixed(3));
    auto lines = wbtest::renderLines(panelFrame("T", lineToElement(line), false, palette::kCyan), 20, 3, &screen);
    CHECK(lines[1].find("DIKHAO x") != std::string::npos);
    CHECK(wbtest::fgOf(screen, 1, 1) == wbtest::rgbFg(palette::kPink));
    CHECK(screen.PixelAt(1, 1).bold);
    CHECK(wbtest::fgOf(screen, 8, 1) == wbtest::defaultFg());   // the frame's colour does not leak into the content
    CHECK_FALSE(screen.PixelAt(8, 1).bold);
}

TEST_CASE("wbui panelFrame draws a titled border in the accent colour", "[wbui]") {
    Screen screen = Screen::Create(Dimension::Fixed(20), Dimension::Fixed(4));
    auto lines = wbtest::renderLines(panelFrame("Log", text("hi"), false, palette::kPink), 20, 4, &screen);
    CHECK(lines[0].find(" Log ") != std::string::npos);
    CHECK(lines[0].rfind("╭", 0) == 0);
    CHECK(wbtest::fgOf(screen, 0, 0) == wbtest::rgbFg(palette::kPink));
    CHECK(wbtest::fgOf(screen, 2, 0) == wbtest::rgbFg(palette::kPink));   // the title
    CHECK(wbtest::fgOf(screen, 0, 1) == wbtest::rgbFg(palette::kPink));
    Screen focused = Screen::Create(Dimension::Fixed(20), Dimension::Fixed(4));
    lines = wbtest::renderLines(panelFrame("Log", text("hi"), true, palette::kPink), 20, 4, &focused);
    CHECK(lines[0].rfind("┏", 0) == 0);   // heavy
    CHECK(wbtest::fgOf(focused, 0, 0) == wbtest::rgbFg(palette::kYellow));
    CHECK(wbtest::fgOf(focused, 19, 3) == wbtest::rgbFg(palette::kYellow));
}

TEST_CASE("wbui tree panel draws, navigates and activates", "[wbui]") {
    Rig rig;
    auto panel = makeTreePanel(rig.s());
    panel->setFocused(true);
    Screen screen(1, 1);
    auto lines = draw(*panel, 40, 12, &screen);
    CHECK(lines[0].find(" Schema ") != std::string::npos);
    CHECK(lines[1] == "┃▼ Databases                           ┃");
    CHECK(lines[2] == "┃  ▶ college                           ┃");
    CHECK(lines[3] == "┃  ▼ main                              ┃");
    CHECK(lines[4] == "┃    ▶ students                        ┃");
    CHECK(lines[5] == "┃    ▶ marks                           ┃");
    CHECK(wbtest::bgOf(screen, 5, 1) == wbtest::rgbBg(palette::kCurrentLine));   // the selected row, focused
    CHECK(wbtest::bgOf(screen, 38, 1) == wbtest::rgbBg(palette::kCurrentLine));  // ... across the whole width
    CHECK(wbtest::bgOf(screen, 5, 2) == wbtest::defaultBg());

    panel->setFocused(false);
    draw(*panel, 40, 12, &screen);
    CHECK(wbtest::bgOf(screen, 5, 1) == wbtest::rgbBg(palette::kStripe));
    panel->setFocused(true);

    CHECK(panel->OnEvent(Event::ArrowDown));
    draw(*panel, 40, 12, &screen);
    CHECK(wbtest::bgOf(screen, 5, 2) == wbtest::rgbBg(palette::kCurrentLine));
    CHECK(rig.s().tree().selected() == 1);

    // Right on a collapsed table expands it, Left collapses it again.
    rig.s().tree().select(3);   // students
    CHECK(panel->OnEvent(Event::ArrowRight));
    lines = draw(*panel, 40, 12);
    INFO(dump(lines));
    CHECK(lines[5].find("id int PK NN") != std::string::npos);
    CHECK(lines[5].find("┃        id") == 0);   // columns: indent 6 + two spaces instead of a marker
    rig.s().tree().select(3);
    CHECK(panel->OnEvent(Event::ArrowLeft));
    lines = draw(*panel, 40, 12);
    CHECK(lines[5].find("▶ marks") != std::string::npos);

    // Space toggles.
    rig.s().tree().select(3);
    CHECK(panel->OnEvent(Event::Character(" ")));
    lines = draw(*panel, 40, 12);
    CHECK(lines[4].find("▼ students") != std::string::npos);
    CHECK(panel->OnEvent(Event::Character(" ")));
    lines = draw(*panel, 40, 12);
    CHECK(lines[4].find("▶ students") != std::string::npos);

    // Return on a table expands it and queues the SELECT.
    CHECK(panel->OnEvent(Event::Return));
    CHECK(rig.s().pendingJobs() == 1);
    REQUIRE(rig.poster.pumpIdle(rig.s()));
    CHECK(rig.fake->ranList().back() == "DIKHAO * SE students SIRF 100;");
    lines = draw(*panel, 40, 12);
    CHECK(lines[4].find("▼ students") != std::string::npos);

    // Return on a column inserts its name into the editor.
    rig.s().tree().select(4);   // id (students is expanded)
    CHECK(panel->OnEvent(Event::Return));
    CHECK(rig.s().editor().text() == "id");
    CHECK(rig.s().takeFocusRequest() == Panel::Editor);
    CHECK_FALSE(panel->OnEvent(Event::F5));
}

TEST_CASE("wbui tree panel scrolls with the selection", "[wbui]") {
    std::string json = R"([{"name":"main","current":true,"tables":[)";
    for (int i = 0; i < 30; ++i) {
        if (i) json += ",";
        json += R"({"name":"table)" + std::to_string(i) + R"(","columns":[]})";
    }
    json += "]}]";
    Rig rig([&](FakeBackend& f) { f.tree = nlohmann::ordered_json::parse(json); });
    auto panel = makeTreePanel(rig.s());
    panel->setFocused(true);
    auto lines = draw(*panel, 40, 12);
    CHECK(wbtest::findRow(lines, "table0") > 0);
    CHECK(wbtest::findRow(lines, "table29") < 0);
    panel->OnEvent(Event::End);
    lines = draw(*panel, 40, 12);
    CHECK(wbtest::findRow(lines, "table29") == 10);   // the last visible row
    CHECK(wbtest::findRow(lines, "table0 ") < 0);
    panel->OnEvent(Event::Home);
    lines = draw(*panel, 40, 12);
    CHECK(wbtest::findRow(lines, "Databases") == 1);
    panel->OnEvent(Event::PageDown);
    CHECK(rig.s().tree().selected() == 9);
}

TEST_CASE("wbui results panel draws a table", "[wbui]") {
    Rig rig([](FakeBackend& f) { f.replies["DIKHAO * SE t;"] = {resultsOf3()}; });
    auto panel = makeResultsPanel(rig.s());
    auto lines = draw(*panel, 50, 8);
    CHECK(lines[0].find(" Results ") != std::string::npos);
    CHECK(lines[0].find("--") == std::string::npos);   // no table yet
    CHECK(lines[1] == "│" + std::string(48, ' ') + "│");

    rig.run("DIKHAO * SE t;");
    panel->setFocused(true);
    Screen screen(1, 1);
    lines = draw(*panel, 50, 8, &screen);
    CHECK(lines[0].find("Results -- 3 row(s)") != std::string::npos);
    const int header = wbtest::findRow(lines, "id");
    REQUIRE(header == 1);
    CHECK(lines[1].find("price") != std::string::npos);
    CHECK(lines[1].find("flag") != std::string::npos);
    // Numbers are right-aligned: the last digits of 1, 22 and 333 share a column.
    const int d1 = wbtest::cellColumn(lines[2], "1 ") ;
    const int d22 = wbtest::cellColumn(lines[3], "22");
    const int d333 = wbtest::cellColumn(lines[4], "333");
    CHECK(d1 + 0 == d22 + 1);
    CHECK(d22 + 1 == d333 + 2);
    // KHALI is dim (FTXUI 5.0.0 cannot do italic), SACH green, JHOOTH red.
    const int khali = wbtest::cellColumn(lines[3], "KHALI");
    REQUIRE(khali > 0);
    CHECK(screen.PixelAt(khali, 3).dim);
    const int sach = wbtest::cellColumn(lines[2], "SACH");
    CHECK(wbtest::fgOf(screen, sach, 2) == wbtest::rgbFg(palette::kGreen));
    const int jhooth = wbtest::cellColumn(lines[3], "JHOOTH");
    CHECK(wbtest::fgOf(screen, jhooth, 3) == wbtest::rgbFg(palette::kRed));
    CHECK(wbtest::fgOf(screen, d22, 3) == wbtest::rgbFg(palette::kPurple));   // numbers
    // Row cursor on the first data row while focused; odd data rows are striped.
    CHECK(wbtest::bgOf(screen, 20, 2) == wbtest::rgbBg(palette::kCurrentLine));
    CHECK(wbtest::bgOf(screen, 20, 3) == wbtest::rgbBg(palette::kStripe));
    CHECK(wbtest::bgOf(screen, 20, 4) == wbtest::defaultBg());
    CHECK(wbtest::bgOf(screen, 20, 1) == wbtest::rgbBg(0x3b3e52));   // header band

    CHECK(panel->OnEvent(Event::ArrowDown));
    draw(*panel, 50, 8, &screen);
    CHECK(wbtest::bgOf(screen, 20, 3) == wbtest::rgbBg(palette::kCurrentLine));
    CHECK(panel->OnEvent(Event::End));
    draw(*panel, 50, 8, &screen);
    CHECK(wbtest::bgOf(screen, 20, 4) == wbtest::rgbBg(palette::kCurrentLine));
    CHECK(panel->OnEvent(Event::PageUp));
    draw(*panel, 50, 8, &screen);
    CHECK(wbtest::bgOf(screen, 20, 2) == wbtest::rgbBg(palette::kCurrentLine));
    panel->setFocused(false);
    draw(*panel, 50, 8, &screen);
    CHECK(wbtest::bgOf(screen, 20, 2) == wbtest::defaultBg());
}

TEST_CASE("wbui results panel scrolls sideways and resets on a new result", "[wbui]") {
    Rig rig([](FakeBackend& f) {
        f.replies["DIKHAO * SE t;"] = {resultsOf3()};
        f.replies["DIKHAO 1;"] = {tableResult({"one"}, {{Value(int64_t(1))}})};
    });
    auto panel = makeResultsPanel(rig.s());
    panel->setFocused(true);
    rig.run("DIKHAO * SE t;");
    auto lines = draw(*panel, 24, 8);   // 18 inner cells, the table is wider
    const int before = wbtest::cellColumn(lines[1], "price");
    REQUIRE(before > 0);
    CHECK(panel->OnEvent(Event::ArrowRight));
    lines = draw(*panel, 24, 8);
    CHECK(wbtest::cellColumn(lines[1], "price") == before - 4);
    for (int i = 0; i < 20; ++i) panel->OnEvent(Event::ArrowRight);
    lines = draw(*panel, 24, 8);
    CHECK(lines[1].find("flag") != std::string::npos);   // never past the last column
    const int pastEnd = wbtest::findRow(lines, "flag");
    CHECK(pastEnd == 1);
    CHECK(panel->OnEvent(Event::ArrowLeft));
    panel->OnEvent(Event::ArrowDown);

    rig.run("DIKHAO 1;");
    lines = draw(*panel, 24, 8);
    INFO(dump(lines));
    CHECK(lines[0].find("Results -- 1 row(s)") != std::string::npos);
    CHECK(wbtest::cellColumn(lines[1], "one") == 2);   // offset reset
    CHECK_FALSE(panel->OnEvent(Event::F5));
}

TEST_CASE("wbui results panel renders only the visible rows of a huge table", "[wbui]") {
    std::vector<std::vector<Value>> rows;
    rows.reserve(100000);
    for (int i = 0; i < 100000; ++i) rows.push_back({Value(int64_t(i)), Value(std::string("row ") + std::to_string(i))});
    Rig rig([&](FakeBackend& f) { f.replies["DIKHAO * SE big;"] = {tableResult({"n", "s"}, rows)}; });
    auto panel = makeResultsPanel(rig.s());
    rig.run("DIKHAO * SE big;");
    draw(*panel, 40, 12);   // first frame computes the column widths
    panel->OnEvent(Event::PageDown);
    const auto t0 = std::chrono::steady_clock::now();
    auto lines = draw(*panel, 40, 12);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 50.0);
    CHECK(lines[0].find("Results -- 100000 row(s)") != std::string::npos);
    panel->OnEvent(Event::End);
    lines = draw(*panel, 40, 12);
    CHECK(wbtest::findRow(lines, "99999") > 0);
}

TEST_CASE("wbui log panel wraps, colours and follows", "[wbui]") {
    Rig rig;
    auto panel = makeLogPanel(rig.s());
    auto lines = draw(*panel, 40, 8);
    CHECK(lines[0].find(" Log ") != std::string::npos);
    CHECK(lines[1].find("Namaste! Connected: fake:1") != std::string::npos);
    CHECK(lines[2].find("F1 dabao madad ke liye.") != std::string::npos);

    rig.s().logLine(LogKind::Error, "boom");
    rig.s().logLine(LogKind::Message, "all fine");
    Screen screen(1, 1);
    lines = draw(*panel, 40, 8, &screen);
    const int err = wbtest::findRow(lines, "boom");
    const int msg = wbtest::findRow(lines, "all fine");
    REQUIRE(err > 0);
    REQUIRE(msg > 0);
    CHECK(wbtest::fgOf(screen, 1, err) == wbtest::rgbFg(palette::kRed));
    CHECK(wbtest::fgOf(screen, 1, msg) == wbtest::rgbFg(palette::kGreen));

    // A long line wraps by characters.
    rig.s().logLine(LogKind::Plain, std::string(50, 'a') + std::string(10, 'b'));
    lines = draw(*panel, 40, 8);
    const int first = wbtest::findRow(lines, std::string(38, 'a'));
    REQUIRE(first > 0);
    CHECK(lines[static_cast<std::size_t>(first) + 1].find(std::string(12, 'a') + std::string(10, 'b')) != std::string::npos);

    // New entries scroll the panel while it follows; Up stops following, End resumes.
    for (int i = 0; i < 30; ++i) rig.s().logLine(LogKind::Plain, "line " + std::to_string(i));
    lines = draw(*panel, 40, 8);
    CHECK(wbtest::findRow(lines, "line 29") == 6);
    CHECK(panel->OnEvent(Event::ArrowUp));
    rig.s().logLine(LogKind::Plain, "late");
    lines = draw(*panel, 40, 8);
    CHECK(wbtest::findRow(lines, "late") < 0);
    CHECK(wbtest::findRow(lines, "line 28") > 0);
    CHECK(panel->OnEvent(Event::End));
    lines = draw(*panel, 40, 8);
    CHECK(wbtest::findRow(lines, "late") == 6);
    panel->OnEvent(Event::Home);
    lines = draw(*panel, 40, 8);
    CHECK(wbtest::findRow(lines, "Namaste") == 1);

    rig.s().clearLog();
    lines = draw(*panel, 40, 8);
    for (std::size_t i = 1; i + 1 < lines.size(); ++i) CHECK(lines[i] == "│" + std::string(38, ' ') + "│");
}

TEST_CASE("wbui log panel handles 20000 lines quickly", "[wbui]") {
    Rig rig;
    for (int i = 0; i < 20000; ++i) rig.s().logLine(LogKind::Plain, "row " + std::to_string(i));
    auto panel = makeLogPanel(rig.s());
    draw(*panel, 80, 12);
    const auto t0 = std::chrono::steady_clock::now();
    auto lines = draw(*panel, 80, 12);
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 50.0);
    CHECK(wbtest::findRow(lines, "row 19999") > 0);
}

TEST_CASE("wbui editor panel edits, highlights and scrolls", "[wbui]") {
    Rig rig;
    auto panel = makeEditorPanel(rig.s());
    panel->setFocused(true);
    Screen screen(1, 1);
    auto lines = draw(*panel, 90, 7, &screen);
    INFO(dump(lines));
    CHECK(lines[0].find("Query  [F5 = chalao, F6 = samjhao]") != std::string::npos);
    CHECK(lines[1].find(" 1  Yahan query likho, jaise:  DIKHAO * SE students;   (F5 se chalao)") != std::string::npos);
    CHECK(screen.PixelAt(4, 1).inverted);   // the cursor cell, focused
    CHECK(screen.PixelAt(6, 1).dim);        // the placeholder

    for (char c : std::string("DIKHAO x")) CHECK(panel->OnEvent(Event::Character(c)));
    lines = draw(*panel, 60, 7, &screen);
    CHECK(lines[1].find(" 1 DIKHAO x") != std::string::npos);
    CHECK(lines[1].find("Yahan") == std::string::npos);
    CHECK(wbtest::fgOf(screen, 4, 1) == wbtest::rgbFg(palette::kPink));
    CHECK(screen.PixelAt(4, 1).bold);
    CHECK(wbtest::fgOf(screen, 11, 1) == wbtest::defaultFg());
    CHECK(screen.PixelAt(12, 1).inverted);   // cursor past the end of the text

    panel->setFocused(false);
    draw(*panel, 60, 7, &screen);
    CHECK_FALSE(screen.PixelAt(12, 1).inverted);
    panel->setFocused(true);

    // Editing keys.
    CHECK(panel->OnEvent(Event::Backspace));
    CHECK(panel->OnEvent(Event::Return));
    CHECK(panel->OnEvent(Event::Character("é")));
    CHECK(panel->OnEvent(Event::Character("\xF0\x9F\x98\x80")));   // a 4-byte emoji, whole
    CHECK(rig.s().editor().text() == "DIKHAO \n\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(panel->OnEvent(Event::ArrowLeft));
    CHECK(panel->OnEvent(Event::Delete));
    CHECK(rig.s().editor().text() == "DIKHAO \n\xC3\xA9");
    CHECK(panel->OnEvent(Event::Home));
    CHECK(rig.s().editor().cursor() == Pos{1, 0});
    CHECK(panel->OnEvent(Event::End));
    CHECK(rig.s().editor().cursor() == Pos{1, 1});
    CHECK(panel->OnEvent(Event::ArrowUp));
    CHECK(rig.s().editor().cursor().row == 0);
    lines = draw(*panel, 60, 7);
    CHECK(lines[2].find(" 2 \xC3\xA9") != std::string::npos);

    // Selection: Shift+Right selects, the background shows it; Ctrl+A selects all.
    rig.s().editor().setText("select me");
    CHECK(panel->OnEvent(Event::Special(keys::kShiftHome)));
    draw(*panel, 60, 7, &screen);
    CHECK(wbtest::bgOf(screen, 4, 1) == wbtest::rgbBg(palette::kCurrentLine));
    CHECK(rig.s().editor().selectedText() == "select me");
    CHECK(panel->OnEvent(Event::ArrowLeft));
    CHECK_FALSE(rig.s().editor().hasSelection());
    CHECK(panel->OnEvent(keys::ctrl('A')));
    CHECK(rig.s().editor().selectedText() == "select me");
    CHECK_FALSE(panel->OnEvent(Event::F5));
    CHECK_FALSE(panel->OnEvent(Event::Tab));
    CHECK_FALSE(panel->OnEvent(keys::ctrl('S')));
}

TEST_CASE("wbui editor view follows the cursor vertically and horizontally", "[wbui]") {
    Rig rig;
    auto panel = makeEditorPanel(rig.s());
    panel->setFocused(true);
    std::string text;
    for (int i = 1; i <= 12; ++i) text += "row" + std::to_string(i) + (i < 12 ? "\n" : "");
    rig.s().editor().setText(text);   // cursor at the end
    auto lines = draw(*panel, 40, 7);   // 5 text rows
    CHECK(wbtest::findRow(lines, "row12") == 5);
    CHECK(wbtest::findRow(lines, "row8") == 1);
    CHECK(wbtest::findRow(lines, "row7") < 0);
    panel->OnEvent(Event::Special(keys::kShiftUp));   // extends upward, view stays with the cursor
    rig.s().editor().moveTo(Pos{0, 0}, false);
    lines = draw(*panel, 40, 7);
    CHECK(wbtest::findRow(lines, "row1 ") >= 0);
    CHECK(wbtest::findRow(lines, "row12") < 0);

    // A long line scrolls sideways.
    rig.s().editor().setText(std::string(100, 'x') + "END");
    lines = draw(*panel, 40, 7);
    CHECK(lines[1].find("END") != std::string::npos);
    CHECK(lines[1].find(" 1 ") != 0);
    rig.s().editor().moveTo(Pos{0, 0}, false);
    lines = draw(*panel, 40, 7);
    CHECK(lines[1].find(" 1 xxxx") != std::string::npos);
    CHECK(lines[1].find("END") == std::string::npos);
}

TEST_CASE("wbui panels never throw on tiny or huge sizes", "[wbui]") {
    Rig rig([](FakeBackend& f) { f.replies["DIKHAO * SE t;"] = {resultsOf3()}; });
    rig.run("DIKHAO * SE t;");
    std::vector<std::shared_ptr<PanelView>> panels = {makeTreePanel(rig.s()), makeResultsPanel(rig.s()),
                                                      makeLogPanel(rig.s()), makeEditorPanel(rig.s())};
    const int sizes[][2] = {{1, 1}, {2, 2}, {3, 3}, {1, 20}, {20, 1}, {5, 0}, {300, 3}, {400, 100}};
    for (auto& panel : panels)
        for (bool focus : {false, true}) {
            panel->setFocused(focus);
            for (const auto& sz : sizes) {
                CHECK_NOTHROW(draw(*panel, sz[0], std::max(1, sz[1])));
            }
        }
}
