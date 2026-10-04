// cpp/tests/test_wb_window.cpp -- the whole window: layout, header, footer, key routing and focus.
#include "wb_ui_rig.h"
#include <filesystem>
#include <future>
#include <thread>

using namespace meradb;
using namespace meradb::wb;
using namespace ftxui;
using namespace wbtest;

namespace {

std::string dumpLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& l : lines) out += l + '\n';
    return out;
}

// A backend whose statement "slow;" waits until the test opens the gate.
struct Gate {
    std::promise<void> opened;
    std::shared_future<void> future = opened.get_future().share();
    void install(FakeBackend& f) {
        f.beforeRun = [this](const std::string& text) {
            if (text == "slow;") future.wait();
        };
    }
    bool done = false;
    void open() {
        if (done) return;
        done = true;
        opened.set_value();
    }
};
// Declared after the rig: opens the gate on the way out so a failed check cannot leave the worker blocked.
struct GateRelease {
    Gate& gate;
    ~GateRelease() { gate.open(); }
};

}  // namespace

TEST_CASE("wbui window layout at 120 x 40", "[wbui]") {
    UiRig rig;
    Screen screen(1, 1);
    auto lines = rig.screen(120, 40, &screen);
    INFO(dumpLines(lines));
    CHECK(lines[0].find(" MeraDB Workbench \xE2\x80\x94 fake:1  |  db: main") == 0);
    CHECK(lines[39].find("F5 Chalao") != std::string::npos);
    CHECK(lines[39].find("F6 Samjhao") != std::string::npos);
    CHECK(lines[39].find("^Q Bahar") != std::string::npos);
    CHECK(lines[39].find("^\xE2\x86\x91 Pichli") != std::string::npos);
    CHECK(lines[39].find("^S CSV") != std::string::npos);
    CHECK(lines[39].find("F1 Madad") != std::string::npos);
    CHECK(findRow(lines, " Schema ") == 1);
    CHECK(findRow(lines, " Results ") == 1);
    const int logRow = findRow(lines, " Log ");
    const int editorRow = findRow(lines, "Query  [F5 = chalao, F6 = samjhao]");
    CHECK(logRow == 20);
    CHECK(editorRow == 30);               // log frame: rows 20..29 (10 tall); editor frame: rows 30..38 (9 tall)
    CHECK(editorRow - logRow == 10);
    CHECK(39 - 1 - editorRow == 8);       // the editor's last row is 38
    // The schema column is 32 wide: its right border is at x = 31, the results frame starts at x = 32.
    CHECK(screen.PixelAt(31, 5).character == "\xE2\x94\x82");   // │ (not focused)
    CHECK(screen.PixelAt(32, 1).character == "\xE2\x95\xAD");   // ╭
    CHECK(screen.PixelAt(0, 38).character == "\xE2\x95\xB0");   // ╰ of the schema frame
    CHECK(bgOf(screen, 5, 0) == rgbBg(0x3b3e52));
    CHECK(lines[2].find("Databases") != std::string::npos);
}

TEST_CASE("wbui window F5 and Ctrl+R run the editor text", "[wbui]") {
    UiRig rig;
    rig.type("DIKHAO * SE t;");
    Screen screen(1, 1);
    CHECK(rig.press(Event::F5));
    rig.settle();
    auto lines = rig.screen(120, 40, &screen);
    INFO(dumpLines(lines));
    const int echo = findRow(lines, "main> DIKHAO * SE t;");
    REQUIRE(echo > 0);
    CHECK(findRow(lines, "ok") > echo);
    CHECK(rig.s().editor().text() == "DIKHAO * SE t;");
    CHECK(rig.fake->ranList().back() == "DIKHAO * SE t;");
    const int dikhao = cellColumn(lines[static_cast<std::size_t>(echo)], "DIKHAO");
    CHECK(fgOf(screen, dikhao, echo) == rgbFg(palette::kPink));

    CHECK(rig.press(keys::ctrl('R')));
    rig.settle();
    CHECK(rig.fake->ranList().size() == 2);
}

TEST_CASE("wbui window F6 explains one statement and warns about more", "[wbui]") {
    UiRig rig;
    rig.type("DIKHAO * SE t;");
    CHECK(rig.press(Event::F6));
    rig.settle();
    CHECK(rig.logText().find("main> SAMJHAO DIKHAO * SE t;") != std::string::npos);
    CHECK(rig.fake->ranList().back() == "SAMJHAO DIKHAO * SE t;");

    rig.s().editor().setText("DIKHAO * SE t; DIKHAO * SE u;");
    CHECK(rig.press(Event::F6));
    rig.settle();
    CHECK(rig.s().log().entries().back().kind == LogKind::Warn);
    CHECK(rig.logText().find("SAMJHAO ek hi query par chalta hai -- ek query select karke F6 dabao") != std::string::npos);
}

TEST_CASE("wbui window Tab cycles the focus and the border shows it", "[wbui]") {
    UiRig rig;
    CHECK(rig.ui->focus() == Panel::Editor);
    Screen screen(1, 1);
    auto lines = rig.screen(120, 40, &screen);
    CHECK(fgOf(screen, 32, 30) == rgbFg(palette::kFocus));   // the editor's corner
    CHECK(fgOf(screen, 32, 20) == rgbFg(palette::kPink));     // the log's
    CHECK(fgOf(screen, 32, 1) == rgbFg(palette::kCyan));      // the results'
    CHECK(fgOf(screen, 0, 1) == rgbFg(palette::kPurple));     // the schema's

    CHECK(rig.press(Event::Tab));
    CHECK(rig.ui->focus() == Panel::Tree);
    rig.screen(120, 40, &screen);
    CHECK(fgOf(screen, 0, 1) == rgbFg(palette::kFocus));
    CHECK(fgOf(screen, 32, 30) == rgbFg(palette::kGreen));
    CHECK(rig.press(Event::Tab));
    CHECK(rig.ui->focus() == Panel::Results);
    CHECK(rig.press(Event::Tab));
    CHECK(rig.ui->focus() == Panel::Log);
    CHECK(rig.press(Event::Tab));
    CHECK(rig.ui->focus() == Panel::Editor);
    CHECK(rig.press(Event::TabReverse));
    CHECK(rig.ui->focus() == Panel::Log);
    CHECK(rig.press(Event::TabReverse));
    CHECK(rig.press(Event::TabReverse));
    CHECK(rig.press(Event::TabReverse));
    CHECK(rig.ui->focus() == Panel::Editor);
    // The Tab key does not reach the editor's text.
    CHECK(rig.s().editor().text().empty());
}

TEST_CASE("wbui window keys reach the focused panel", "[wbui]") {
    UiRig rig;
    rig.press(Event::Tab);   // tree
    rig.press(Event::ArrowDown);
    CHECK(rig.s().tree().selected() == 1);
    CHECK(rig.s().editor().text().empty());
    rig.ui->setFocus(Panel::Editor);
    rig.type("abc");
    CHECK(rig.s().editor().text() == "abc");
}

TEST_CASE("wbui window history keys work from any panel and focus the editor", "[wbui]") {
    UiRig rig;
    rig.runStatement("DIKHAO 1;");
    rig.runStatement("DIKHAO 2;");
    rig.ui->setFocus(Panel::Tree);
    CHECK(rig.press(Event::ArrowUpCtrl));
    CHECK(rig.s().editor().text() == "DIKHAO 2;");
    CHECK(rig.ui->focus() == Panel::Editor);
    rig.ui->setFocus(Panel::Log);
    CHECK(rig.press(keys::ctrl('P')));
    CHECK(rig.s().editor().text() == "DIKHAO 1;");
    CHECK(rig.ui->focus() == Panel::Editor);
    rig.ui->setFocus(Panel::Results);
    CHECK(rig.press(Event::ArrowDownCtrl));
    CHECK(rig.s().editor().text() == "DIKHAO 2;");
    CHECK(rig.press(keys::ctrl('N')));
    CHECK(rig.s().editor().text().empty());
}

TEST_CASE("wbui window Ctrl+S exports only after a result", "[wbui]") {
    UiRig rig([](FakeBackend& f) {
        f.replies["DIKHAO * SE t;"] = {tableResult({"id"}, {{Value(int64_t(1))}, {Value(int64_t(2))}})};
    });
    CHECK(rig.press(keys::ctrl('S')));
    CHECK(rig.s().log().entries().back().kind == LogKind::Warn);
    CHECK(rig.logText().find("Pehle koi DIKHAO query chalao, phir Ctrl+S") != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(rig.exportDir.path() / "exports"));
    rig.runStatement("DIKHAO * SE t;");
    CHECK(rig.press(keys::ctrl('S')));
    CHECK(rig.logText().find("2 row(s) CSV mein save: ") != std::string::npos);
    int files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(rig.exportDir.path() / "exports")) {
        (void)entry;
        ++files;
    }
    CHECK(files == 1);
}

TEST_CASE("wbui window Ctrl+L clears the log and Ctrl+C only hints", "[wbui]") {
    UiRig rig;
    CHECK_FALSE(rig.s().log().entries().empty());
    CHECK(rig.press(keys::ctrl('L')));
    CHECK(rig.s().log().entries().empty());
    CHECK(rig.press(keys::ctrl('C')));
    REQUIRE(rig.s().log().entries().size() == 1);
    CHECK(rig.s().log().entries().back().kind == LogKind::Warn);
    CHECK(rig.logText() == "Bahar niklne ke liye Ctrl+Q dabao.\n");
    CHECK(rig.exits == 0);
    CHECK_FALSE(rig.s().quitting());
}

TEST_CASE("wbui window Ctrl+Q rolls back and exits, swallowing keys meanwhile", "[wbui]") {
    Gate gate;
    UiRig rig([&](FakeBackend& f) { gate.install(f); });
    GateRelease release{gate};
    rig.runStatement("SHURU;");
    CHECK(rig.s().inTransaction());
    CHECK(rig.screen(120, 40)[0].find("TRANSACTION") != std::string::npos);

    rig.s().editor().setText("slow;");
    rig.press(Event::F5);   // blocks on the worker
    CHECK(rig.screen(120, 40)[0].find("[chal raha hai]") != std::string::npos);
    CHECK(rig.press(keys::ctrl('Q')));
    CHECK(rig.s().quitting());
    CHECK(rig.screen(120, 40)[0].find("[band ho raha hai ...]") != std::string::npos);
    const int pending = rig.s().pendingJobs();
    CHECK(rig.press(Event::F5));            // swallowed
    CHECK(rig.press(Event::Character("x")));
    CHECK(rig.press(keys::ctrl('Q')));      // a second Ctrl+Q does nothing
    CHECK(rig.s().pendingJobs() == pending);
    CHECK(rig.s().editor().text() == "slow;");
    CHECK(rig.exits == 0);

    gate.open();
    REQUIRE(rig.poster.pumpUntil([&] { return rig.exits > 0; }));
    CHECK(rig.exits == 1);
    CHECK(rig.seen->rolledBackOnClose);
}

TEST_CASE("wbui window handles every terminal size", "[wbui]") {
    UiRig rig;
    auto lines = rig.screen(60, 24);
    CHECK(findRow(lines, " Schema ") == 1);
    CHECK(findRow(lines, "Terminal bahut chhota") < 0);
    CHECK(findRow(lines, " Log ") >= 0);
    CHECK(findRow(lines, "Query  [F5") >= 0);
    CHECK(lines[23].find("F5 Chalao") != std::string::npos);

    lines = rig.screen(59, 24);
    CHECK(findRow(lines, "Terminal bahut chhota hai (kam se kam 60 x 24)") >= 0);
    CHECK(findRow(lines, " Schema ") < 0);
    lines = rig.screen(80, 23);
    CHECK(findRow(lines, "Terminal bahut chhota hai (kam se kam 60 x 24)") >= 0);

    CHECK_NOTHROW(rig.screen(200, 60));
    CHECK_NOTHROW(rig.screen(1000, 3));
    CHECK_NOTHROW(rig.screen(1, 1));
    CHECK_NOTHROW(rig.screen(300, 24));
    // Keys still work when the window is too small.
    rig.screen(40, 10);
    rig.type("q");
    CHECK(rig.s().editor().text() == "q");
}

TEST_CASE("wbui window busy label shows while a statement runs", "[wbui]") {
    Gate gate;
    UiRig rig([&](FakeBackend& f) { gate.install(f); });
    GateRelease release{gate};
    CHECK(rig.screen(120, 40)[0].find("[chal raha hai") == std::string::npos);
    rig.s().editor().setText("slow;");
    rig.press(Event::F5);
    auto head = rig.screen(120, 40)[0];
    CHECK(head.find("[chal raha hai]") != std::string::npos);
    CHECK(head.rfind("[chal raha hai]") > 100);   // right-aligned
    rig.s().editor().setText("DIKHAO 1;");
    rig.press(Event::F5);
    CHECK(rig.screen(120, 40)[0].find("[chal raha hai +1]") != std::string::npos);
    gate.open();
    rig.settle();
    CHECK(rig.screen(120, 40)[0].find("[chal raha hai") == std::string::npos);
}

TEST_CASE("wbui window runs only the selection", "[wbui]") {
    UiRig rig;
    rig.type("one;\ntwo;");
    CHECK(rig.s().editor().text() == "one;\ntwo;");
    rig.s().editor().moveTo(Pos{0, 0}, false);
    for (int i = 0; i < 4; ++i) rig.press(Event::Special(keys::kShiftRight));
    CHECK(rig.s().editor().selectedText() == "one;");
    rig.press(Event::F5);
    rig.settle();
    CHECK(rig.fake->ranList().back() == "one;");
    // Ctrl+A selects everything, and the whole text runs.
    rig.press(keys::ctrl('A'));
    rig.press(Event::F5);
    rig.settle();
    CHECK(rig.fake->ranList().back() == "one;\ntwo;");
}
