// cpp/tests/test_wb_e2e.cpp -- the whole workbench without a terminal: the real engine (LocalBackend), the real Session
// with its worker thread, and the real component tree, driven by synthetic key events and read back from a rendered
// FTXUI Screen. Complements the scripted-backend tests (wbsession, wbui) and the comparison with the Python workbench.
#include "server_fixture.h"
#include "test_util.h"
#include "wb_screen_util.h"
#include "wb_test_util.h"
#include "meradb/backend.h"
#include "meradb/wb_keys.h"
#include "meradb/wb_session.h"
#include "meradb/wb_ui.h"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace meradb;
using namespace meradb::wb;
using namespace ftxui;
using namespace wbtest;

namespace {

constexpr int kW = 220, kH = 60;

std::string dump(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& l : lines) out += l + '\n';
    return out;
}

bool has(const std::vector<std::string>& lines, const std::string& needle) { return findRow(lines, needle) >= 0; }

std::string slurp(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Cells from the start of the row to the end of the first occurrence of `needle` (-1 if absent).
int endColumn(const std::string& row, const std::string& needle) {
    const int start = cellColumn(row, needle);
    if (start < 0) return -1;
    int cells = 0;
    for (std::size_t i = 0; i < needle.size(); ++i)
        if ((static_cast<unsigned char>(needle[i]) & 0xC0) != 0x80) ++cells;
    return start + cells;
}

// The real engine on a temp folder, the real session and window, a manual UI-thread poster.
struct E2e {
    meradb_test::TempDir data, exportDir;
    ManualPoster poster;
    std::unique_ptr<Session> session;
    std::unique_ptr<WorkbenchUi> ui;
    int exits = 0;

    explicit E2e(SessionOptions options = {}) {
        options.dataDir = data.str();
        options.exportBaseDir = exportDir.str();
        session = std::make_unique<Session>(std::make_unique<LocalBackend>(data.str()), std::move(options),
                                            poster.poster());
        session->setOnExit([this] { ++exits; });
        ui = std::make_unique<WorkbenchUi>(*session);
        settle();
    }
    ~E2e() {
        ui.reset();
        session.reset();   // joins the worker; closes the engine
    }

    Session& s() { return *session; }
    bool press(const Event& e) { return ui->onEvent(e); }
    void type(const std::string& text) {
        for (std::size_t i = 0; i < text.size();) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            std::size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            n = std::min(n, text.size() - i);
            const std::string piece = text.substr(i, n);
            if (piece == "\n") press(Event::Return);
            else press(Event::Character(piece));
            i += n;
        }
    }
    void settle() { REQUIRE(poster.pumpIdle(*session)); }
    std::vector<std::string> screen(int w = kW, int h = kH, Screen* keep = nullptr) { return renderLines(ui->render(), w, h, keep); }
    // Ctrl+A, type the text over it, F5, wait.
    void run(const std::string& text) {
        press(keys::ctrl('A'));
        type(text);
        press(Event::F5);
        settle();
    }
    std::string logText() const {
        std::string out;
        for (const auto& entry : session->log().entries())
            for (const auto& line : entry.lines) out += plainText(line) + "\n";
        return out;
    }
    int treeRow(const std::string& labelPrefix) {
        const auto& rows = s().tree().rows();
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (plainText(rows[i].label).rfind(labelPrefix, 0) == 0) return static_cast<int>(i);
        return -1;
    }
    // The tree must have the focus: moves the selection to the row with this label prefix.
    void goToRow(const std::string& labelPrefix) {
        const int target = treeRow(labelPrefix);
        REQUIRE(target >= 0);
        for (int guard = 0; s().tree().selected() != target && guard < 200; ++guard)
            press(s().tree().selected() < target ? Event::ArrowDown : Event::ArrowUp);
        REQUIRE(s().tree().selected() == target);
    }
};

// A statement list that inserts `count` rows (0..count-1) into table `name(x)`.
std::string bulkInsert(const std::string& name, int count) {
    std::string text = "DAALO MEIN " + name + " MAAN ";
    for (int i = 0; i < count; ++i) text += std::string(i ? ", (" : "(") + std::to_string(i) + ")";
    return text + ";";
}

}  // namespace

TEST_CASE("wbe2e start: header, start-up log, tree and the focused editor", "[wbe2e]") {
    E2e rig;
    Screen screen(1, 1);
    auto lines = rig.screen(kW, kH, &screen);
    INFO(dump(lines));
    CHECK(lines[0].find("MeraDB Workbench") != std::string::npos);
    CHECK(lines[0].find("local (" + rig.data.str() + ")  |  db: main") != std::string::npos);
    CHECK(lines[0].find("TRANSACTION") == std::string::npos);
    CHECK(has(lines, "Namaste! Connected: local (" + rig.data.str() + ")"));
    CHECK(has(lines, "F1 dabao madad ke liye."));
    CHECK(findRow(lines, "Databases") >= 0);
    CHECK(findRow(lines, "main") >= 0);
    CHECK(rig.ui->focus() == Panel::Editor);
    // The cursor cell of the editor is inverted.
    const int editorRow = findRow(lines, "Query  [F5 = chalao, F6 = samjhao]");
    REQUIRE(editorRow > 0);
    bool inverted = false;
    for (int x = 0; x < kW; ++x) inverted = inverted || screen.PixelAt(x, editorRow + 1).inverted;
    CHECK(inverted);
}

TEST_CASE("wbe2e DDL, DML and SELECT through the keyboard", "[wbe2e]") {
    E2e rig;
    rig.type("BANAO TABLE students (id ANK MUKHYA KUNJI, naam TEXT ZAROORI, marks DASHAMLAV, pass BOOLEAN);");
    CHECK(rig.press(Event::F5));
    rig.settle();
    Screen screen(1, 1);
    auto lines = rig.screen(kW, kH, &screen);
    INFO(dump(lines));
    const int echo = findRow(lines, "main> BANAO TABLE students");
    REQUIRE(echo > 0);
    CHECK(fgOf(screen, cellColumn(lines[static_cast<std::size_t>(echo)], "BANAO"), echo) == rgbFg(palette::kPink));
    const int message = findRow(lines, "Table 'students' ban gaya");
    REQUIRE(message > echo);
    CHECK(fgOf(screen, cellColumn(lines[static_cast<std::size_t>(message)], "Table"), message) == rgbFg(palette::kGreen));
    CHECK(rig.treeRow("students") > 0);
    for (const auto& row : rig.s().tree().rows()) {
        if (plainText(row.label) == "students") CHECK_FALSE(row.expanded);
    }
    CHECK(rig.s().editor().text().find("BANAO TABLE students") == 0);   // F5 leaves the text in the editor

    rig.run("DAALO MEIN students MAAN (1, 'Asha', 91.5, SACH), (2, 'Ravi', 40, JHOOTH), (3, 'Meena', KHALI, KHALI);");
    CHECK(rig.logText().find("3 row(s) daal di") != std::string::npos);
    rig.run("DIKHAO * SE students;");
    lines = rig.screen(kW, kH, &screen);
    INFO(dump(lines));
    CHECK(has(lines, "Results -- 3 row(s)"));
    const int header = findRow(lines, "naam");
    REQUIRE(header > 0);
    CHECK(lines[static_cast<std::size_t>(header)].find("id") != std::string::npos);
    CHECK(lines[static_cast<std::size_t>(header)].find("marks") != std::string::npos);
    CHECK(lines[static_cast<std::size_t>(header)].find("pass") != std::string::npos);

    const int asha = findRow(lines, "Asha");
    const int ravi = findRow(lines, "Ravi");
    const int meena = findRow(lines, "Meena");
    REQUIRE(asha > header);
    REQUIRE(ravi > asha);
    REQUIRE(meena > ravi);
    const std::string& ashaRow = lines[static_cast<std::size_t>(asha)];
    const std::string& raviRow = lines[static_cast<std::size_t>(ravi)];
    const std::string& meenaRow = lines[static_cast<std::size_t>(meena)];
    // Booleans: SACH green, JHOOTH red.
    CHECK(fgOf(screen, cellColumn(ashaRow, "SACH"), asha) == rgbFg(palette::kGreen));
    CHECK(fgOf(screen, cellColumn(raviRow, "JHOOTH"), ravi) == rgbFg(palette::kRed));
    // KHALI is dim (this terminal library has no italic).
    const int khali = cellColumn(meenaRow, "KHALI");
    REQUIRE(khali >= 0);
    CHECK(screen.PixelAt(khali, meena).dim);
    // Numbers are purple and right-aligned: 91.5 and 40.0 end in the same column.
    CHECK(fgOf(screen, cellColumn(ashaRow, "91.5"), asha) == rgbFg(palette::kPurple));
    CHECK(endColumn(ashaRow, "91.5") == endColumn(raviRow, "40.0"));
    CHECK(endColumn(ashaRow, "91.5") > 0);
    // A timing line sits in the log.
    const int timing = findRow(lines, " ms)");
    CHECK(timing > 0);
}

TEST_CASE("wbe2e errors, several statements and the table that stays", "[wbe2e]") {
    E2e rig;
    rig.run("BANAO TABLE t (a ANK); DAALO MEIN t MAAN (1), (2);");
    rig.run("DIKHAO * SE nahi_hai; DIKHAO * SE t KRAM a ULTA;");
    Screen screen(1, 1);
    auto lines = rig.screen(kW, kH, &screen);
    INFO(dump(lines));
    const int error = findRow(lines, "exist nahi karta");
    REQUIRE(error > 0);
    CHECK(fgOf(screen, cellColumn(lines[static_cast<std::size_t>(error)], "[Execution"), error) == rgbFg(palette::kRed));
    CHECK(screen.PixelAt(cellColumn(lines[static_cast<std::size_t>(error)], "[Execution"), error).bold);
    CHECK(has(lines, "Results -- 2 row(s)"));
    const int two = findRow(lines, " 2 ");
    CHECK(two >= 0);

    // A later statement with no table leaves the table of the last result in place.
    rig.run("DAALO MEIN t MAAN (3);");
    lines = rig.screen();
    CHECK(has(lines, "Results -- 2 row(s)"));
    CHECK(has(lines, "1 row(s) daal di"));
    // A statement that fails to parse shows its error and also leaves the table alone.
    rig.run("DIKHAO FROM;");
    lines = rig.screen();
    CHECK(has(lines, "[Parser Galti]"));
    CHECK(has(lines, "Results -- 2 row(s)"));
}

TEST_CASE("wbe2e selection runs alone and F6 explains", "[wbe2e]") {
    E2e rig;
    rig.run("BANAO TABLE t (a ANK);");
    rig.press(keys::ctrl('A'));
    rig.type("DAALO MEIN t MAAN (1);\nDIKHAO * SE t;");
    rig.press(Event::Special("\x1b[1;5H"));   // Ctrl+Home
    rig.press(Event::Special(keys::kShiftEnd));
    CHECK(rig.s().editor().selectedText() == "DAALO MEIN t MAAN (1);");
    rig.press(Event::F5);
    rig.settle();
    auto lines = rig.screen();
    INFO(dump(lines));
    CHECK(has(lines, "main> DAALO MEIN t MAAN (1);"));
    CHECK(rig.logText().find("main> DIKHAO * SE t;") == std::string::npos);   // the second line did not run
    CHECK(rig.logText().find("1 row(s) daal di") != std::string::npos);

    // F6 on the second line: one statement.
    rig.press(Event::ArrowDown);
    rig.press(Event::Home);
    rig.press(Event::Special(keys::kShiftEnd));
    CHECK(rig.s().editor().selectedText() == "DIKHAO * SE t;");
    rig.press(Event::F6);
    rig.settle();
    CHECK(rig.logText().find("main> SAMJHAO DIKHAO * SE t;") != std::string::npos);

    // F6 on two statements warns.
    rig.press(keys::ctrl('A'));
    CHECK(rig.press(Event::F6));
    rig.settle();
    CHECK(rig.s().log().entries().back().kind == LogKind::Warn);
    CHECK(rig.logText().find("SAMJHAO ek hi query par chalta hai -- ek query select karke F6 dabao") != std::string::npos);
}

TEST_CASE("wbe2e history with Ctrl+Up and Ctrl+Down", "[wbe2e]") {
    E2e rig;
    rig.run("BANAO TABLE t (a ANK);");
    rig.run("DAALO MEIN t MAAN (1);");
    rig.run("DAALO MEIN t MAAN (1);");   // equal to the last entry: not added again
    rig.run("DIKHAO * SE t;");
    CHECK(rig.s().history().size() == 3);

    rig.ui->setFocus(Panel::Log);
    CHECK(rig.press(Event::ArrowUpCtrl));
    CHECK(rig.s().editor().text() == "DIKHAO * SE t;");
    CHECK(rig.ui->focus() == Panel::Editor);
    CHECK(rig.press(Event::ArrowUpCtrl));
    CHECK(rig.s().editor().text() == "DAALO MEIN t MAAN (1);");
    CHECK(rig.press(Event::ArrowUpCtrl));
    CHECK(rig.s().editor().text() == "BANAO TABLE t (a ANK);");
    CHECK(rig.press(Event::ArrowUpCtrl));   // stays at the oldest
    CHECK(rig.s().editor().text() == "BANAO TABLE t (a ANK);");
    CHECK(rig.press(Event::ArrowDownCtrl));
    CHECK(rig.press(Event::ArrowDownCtrl));
    CHECK(rig.s().editor().text() == "DIKHAO * SE t;");
    CHECK(rig.press(Event::ArrowDownCtrl));   // past the end: empty
    CHECK(rig.s().editor().text().empty());
    CHECK(rig.press(Event::ArrowDownCtrl));
    CHECK(rig.s().editor().text().empty());
    // The cursor goes to the end of the recalled text.
    CHECK(rig.press(Event::ArrowUpCtrl));
    rig.type(" --x");
    CHECK(rig.s().editor().text() == "DIKHAO * SE t; --x");
}

TEST_CASE("wbe2e the schema tree: expand, insert a column, run a table, switch database", "[wbe2e]") {
    E2e rig;
    rig.run("BANAO TABLE students (id ANK MUKHYA KUNJI, naam TEXT ZAROORI);");
    rig.run("BANAO DATABASE college;");
    CHECK(rig.treeRow("college") > 0);

    CHECK(rig.press(Event::Tab));
    CHECK(rig.ui->focus() == Panel::Tree);
    rig.goToRow("students");
    CHECK(rig.press(Event::ArrowRight));   // expand
    CHECK(rig.treeRow("naam ") > 0);
    // Expansion survives a refresh caused by a statement.
    rig.ui->setFocus(Panel::Editor);
    rig.run("DIKHAO * SE students;");
    CHECK(rig.treeRow("naam ") > 0);

    // Enter on a column inserts its name at the editor cursor and focuses the editor.
    rig.press(Event::Tab);
    rig.goToRow("naam ");
    const std::string before = rig.s().editor().text();
    CHECK(rig.press(Event::Return));
    CHECK(rig.s().editor().text() == before + "naam");
    CHECK(rig.ui->focus() == Panel::Editor);

    // Enter on the table runs DIKHAO * SE students SIRF 100;
    rig.press(Event::Tab);
    rig.goToRow("students");
    CHECK(rig.press(Event::Return));
    rig.settle();
    CHECK(rig.logText().find("main> DIKHAO * SE students SIRF 100;") != std::string::npos);

    // Enter on another database switches to it.
    rig.goToRow("college");
    CHECK(rig.press(Event::Return));
    rig.settle();
    CHECK(rig.logText().find("main> ISTEMAL college;") != std::string::npos);
    auto lines = rig.screen();
    CHECK(lines[0].find("|  db: college") != std::string::npos);
    CHECK(rig.s().currentDb() == "college");
    // The database that was open stays open.
    bool mainOpen = false;
    for (const auto& row : rig.s().tree().rows())
        if (plainText(row.label) == "main") mainOpen = row.expanded;
    CHECK(mainOpen);
}

TEST_CASE("wbe2e the transaction marker and WAPAS", "[wbe2e]") {
    E2e rig;
    rig.run("BANAO TABLE t (a ANK);");
    rig.run("SHURU;");
    CHECK(rig.screen()[0].find("TRANSACTION (PAKKA / WAPAS)") != std::string::npos);
    rig.run("DAALO MEIN t MAAN (1);");
    CHECK(rig.screen()[0].find("TRANSACTION (PAKKA / WAPAS)") != std::string::npos);
    rig.run("WAPAS;");
    CHECK(rig.screen()[0].find("TRANSACTION") == std::string::npos);
    rig.run("DIKHAO GINO(*) SE t;");
    auto lines = rig.screen();
    INFO(dump(lines));
    CHECK(has(lines, "Results -- 1 row(s)"));
    REQUIRE(rig.s().table() != nullptr);
    CHECK(rig.s().table()->rows.at(0).at(0).text == "0");
}

TEST_CASE("wbe2e Ctrl+Q rolls an open transaction back and leaves no snapshot behind", "[wbe2e]") {
    E2e rig;
    rig.run("BANAO TABLE t (a ANK);");
    rig.run("SHURU;");
    rig.run("DAALO MEIN t MAAN (42);");
    CHECK(rig.press(keys::ctrl('Q')));
    REQUIRE(rig.poster.pumpUntil([&] { return rig.exits > 0; }));
    CHECK(rig.exits == 1);
    rig.ui.reset();
    rig.session.reset();

    LocalBackend again(rig.data.str());
    const auto results = again.runScript("DIKHAO GINO(*) SE t;");
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].rows.size() == 1);
    CHECK(formatValue(results[0].rows[0][0]) == "0");
    again.close();
    const std::filesystem::path snapshots = rig.data.path() / SNAPSHOT_DIR;
    if (std::filesystem::exists(snapshots)) {
        int leftovers = 0;
        for (const auto& entry : std::filesystem::directory_iterator(snapshots)) {
            (void)entry;
            ++leftovers;
        }
        CHECK(leftovers == 0);
    }
}

TEST_CASE("wbe2e Ctrl+Q waits for a slow statement and Ctrl+C only logs a hint", "[wbe2e]") {
    E2e rig;
    rig.run("BANAO TABLE a (x ANK); BANAO TABLE b (x ANK); BANAO TABLE marker (m ANK);");
    rig.s().runText(bulkInsert("a", 1500));
    rig.s().runText(bulkInsert("b", 1500));
    rig.settle();
    // A cross join of two 1,500-row tables, then a write that only happens if the whole script ran.
    const std::string slow = "DIKHAO GINO(*) SE a MILAO b PAR a.x >= 0; DAALO MEIN marker MAAN (1);";
    rig.s().editor().setText(slow);
    rig.press(Event::F5);
    // The statement has started once its echo is in the log.
    REQUIRE(rig.poster.pumpUntil([&] { return rig.logText().find("main> DIKHAO GINO(*) SE a MILAO b") != std::string::npos; }));
    CHECK(rig.screen()[0].find("[chal raha hai") != std::string::npos);

    const std::size_t entries = rig.s().log().entries().size();
    CHECK(rig.press(keys::ctrl('C')));
    CHECK(rig.s().log().entries().size() == entries + 1);
    CHECK(rig.s().log().entries().back().kind == LogKind::Warn);
    CHECK(rig.logText().find("Bahar niklne ke liye Ctrl+Q dabao.") != std::string::npos);
    CHECK_FALSE(rig.s().quitting());
    CHECK(rig.exits == 0);

    CHECK(rig.press(keys::ctrl('Q')));
    CHECK(rig.s().quitting());
    CHECK(rig.screen()[0].find("[band ho raha hai ...]") != std::string::npos);
    CHECK(rig.exits == 0);
    REQUIRE(rig.poster.pumpUntil([&] { return rig.exits > 0; }, std::chrono::seconds(120)));
    CHECK(rig.exits == 1);
    rig.ui.reset();
    rig.session.reset();

    // The running statement was not abandoned half-way: its second statement happened.
    LocalBackend again(rig.data.str());
    const auto results = again.runScript("DIKHAO GINO(*) SE marker;");
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].rows.size() == 1);
    CHECK(formatValue(results[0].rows[0][0]) == "1");
    again.close();
}

TEST_CASE("wbe2e CSV export matches csv.writer byte for byte", "[wbe2e]") {
    E2e rig;
    rig.run("BANAO TABLE t (a TEXT, b TEXT);");
    rig.run("DAALO MEIN t MAAN ('x,y', 'say \"hi\"'), (KHALI, 'plain'), ('', '');");
    rig.run("DIKHAO * SE t;");
    CHECK(rig.press(keys::ctrl('S')));
    const std::filesystem::path folder = rig.exportDir.path() / "exports";
    REQUIRE(std::filesystem::is_directory(folder));
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) files.push_back(entry.path());
    REQUIRE(files.size() == 1);
    CHECK(files[0].filename().string().rfind("meradb-", 0) == 0);
    CHECK(files[0].extension() == ".csv");
    CHECK(slurp(files[0]) == "a,b\r\n\"x,y\",\"say \"\"hi\"\"\"\r\n,plain\r\n,\r\n");
    const auto& entries = rig.s().log().entries();
    REQUIRE_FALSE(entries.empty());
    const std::string last = plainText(entries.back().lines.back());
    CHECK(last.rfind("3 row(s) CSV mein save: ", 0) == 0);
    CHECK(last.find("exports") != std::string::npos);
    CHECK(last.find(files[0].filename().string()) != std::string::npos);
}

TEST_CASE("wbe2e the connect dialog against a real server and back to local mode", "[wbe2e]") {
    meradb_test::RunningServer server;
    E2e rig;
    rig.run("BANAO TABLE only_local (a ANK);");
    CHECK(rig.treeRow("only_local") > 0);

    // Ctrl+O, the Port field: clear it, type the server's port, Enter.
    CHECK(rig.press(keys::ctrl('O')));
    CHECK(rig.s().modal() == Modal::Connect);
    rig.press(Event::Tab);
    rig.press(Event::Home);
    for (int i = 0; i < 4; ++i) rig.press(Event::Delete);
    rig.type(std::to_string(server.port()));
    CHECK(rig.press(Event::Return));
    rig.settle();
    const std::string where = "127.0.0.1:" + std::to_string(server.port());
    auto lines = rig.screen();
    INFO(dump(lines));
    CHECK(lines[0].find(where + "  |  db: main") != std::string::npos);
    CHECK(rig.logText().find("Connected: " + where) != std::string::npos);
    Screen screen(1, 1);
    lines = rig.screen(kW, kH, &screen);
    const int connected = findRow(lines, "Connected: " + where);
    REQUIRE(connected > 0);
    CHECK(fgOf(screen, cellColumn(lines[static_cast<std::size_t>(connected)], "Connected:"), connected) == rgbFg(palette::kGreen));
    CHECK(screen.PixelAt(cellColumn(lines[static_cast<std::size_t>(connected)], "Connected:"), connected).bold);
    // The local table is not on the server: statements really go over the wire.
    CHECK(rig.treeRow("only_local") < 0);
    rig.run("BANAO TABLE remote_t (a ANK);");
    CHECK(rig.treeRow("remote_t") > 0);

    // Ctrl+O again: the Host field is active; five Tabs reach [ Local mode ].
    CHECK(rig.press(keys::ctrl('O')));
    lines = rig.screen();
    CHECK(has(lines, "127.0.0.1"));
    CHECK(has(lines, "[ Local mode ]"));
    for (int i = 0; i < 5; ++i) rig.press(Event::Tab);
    CHECK(rig.press(Event::Return));
    rig.settle();
    lines = rig.screen();
    CHECK(lines[0].find("local (" + rig.data.str() + ")") != std::string::npos);
    CHECK(rig.treeRow("only_local") > 0);
    CHECK(rig.treeRow("remote_t") < 0);
    CHECK(rig.exits == 0);
}

TEST_CASE("wbe2e a wrong port logs a red error and keeps the old connection", "[wbe2e]") {
    int closedPort = 0;
    {
        meradb_test::RunningServer gone;
        closedPort = gone.port();
    }   // stopped: nothing listens there any more
    E2e rig;
    rig.run("BANAO TABLE kept (a ANK);");
    rig.press(keys::ctrl('O'));
    rig.press(Event::Tab);
    rig.press(Event::Home);
    for (int i = 0; i < 4; ++i) rig.press(Event::Delete);
    rig.type(std::to_string(closedPort));
    rig.press(Event::Return);
    rig.settle();
    Screen screen(1, 1);
    auto lines = rig.screen(kW, kH, &screen);
    INFO(dump(lines));
    const int failure = findRow(lines, "Connect nahi hua: [Connection Galti]");
    REQUIRE(failure > 0);
    const int x = cellColumn(lines[static_cast<std::size_t>(failure)], "Connect nahi hua");
    CHECK(fgOf(screen, x, failure) == rgbFg(palette::kRed));
    CHECK(screen.PixelAt(x, failure).bold);
    CHECK(lines[0].find("local (" + rig.data.str() + ")") != std::string::npos);
    rig.run("DIKHAO * SE kept;");   // the old connection still works
    CHECK(has(rig.screen(), "Results"));
    CHECK(rig.treeRow("kept") > 0);
}

TEST_CASE("wbe2e F1 shows the help, PageDown scrolls it, Esc closes it", "[wbe2e]") {
    E2e rig;
    rig.type("DIKHAO 1;");
    CHECK(rig.press(Event::F1));
    CHECK(rig.s().modal() == Modal::Help);
    auto lines = rig.screen();
    INFO(dump(lines));
    CHECK(has(lines, "Madad"));
    CHECK(has(lines, "Query chalao"));
    CHECK(has(lines, "Ctrl+Q"));

    // F5 does nothing while the help is open.
    const std::size_t entries = rig.s().log().entries().size();
    CHECK(rig.press(Event::F5));
    rig.settle();
    CHECK(rig.s().log().entries().size() == entries);

    bool reachedReference = false;
    for (int i = 0; i < 40 && !reachedReference; ++i) {
        rig.press(Event::PageDown);
        reachedReference = has(rig.screen(), "DDL: Data Definition Language");
    }
    CHECK(reachedReference);
    CHECK_FALSE(has(rig.screen(), "Query chalao"));

    CHECK(rig.press(Event::Escape));
    CHECK(rig.s().modal() == Modal::None);
    lines = rig.screen();
    CHECK_FALSE(has(lines, "DDL: Data Definition Language"));
    CHECK(has(lines, "Query  [F5 = chalao, F6 = samjhao]"));
    CHECK(rig.s().editor().text() == "DIKHAO 1;");
}

TEST_CASE("wbe2e every size renders, and Unicode text stays aligned", "[wbe2e]") {
    E2e rig;
    const std::pair<int, int> sizes[] = {{60, 24}, {80, 24}, {120, 40}, {200, 60}};
    for (const auto& size : sizes) {
        std::vector<std::string> lines;
        CHECK_NOTHROW(lines = rig.screen(size.first, size.second));
        INFO(size.first << "x" << size.second);
        CHECK(lines[0].find("MeraDB Workbench") != std::string::npos);
    }
    CHECK(has(rig.screen(59, 24), "Terminal bahut chhota hai (kam se kam 60 x 24)"));
    CHECK(has(rig.screen(80, 23), "Terminal bahut chhota hai (kam se kam 60 x 24)"));

    rig.run("BANAO TABLE u (id ANK, t TEXT);");
    rig.run("DAALO MEIN u MAAN (1, '\xC3\xA9\xF0\x9F\x98\x80\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87"
            "\xE6\x97\xA5\xE6\x9C\xAC'), (2, 'plain');");
    rig.run("DIKHAO * SE u;");
    const std::string text = "\xC3\xA9\xF0\x9F\x98\x80\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87"
                             "\xE6\x97\xA5\xE6\x9C\xAC";
    REQUIRE(rig.s().table() != nullptr);
    REQUIRE(rig.s().table()->rows.size() == 2);
    CHECK(rig.s().table()->rows[0][1].text == text);
    Screen screen(1, 1);
    const int width = 120;
    auto lines = rig.screen(width, 40, &screen);
    INFO(dump(lines));
    CHECK(has(lines, "Results -- 2 row(s)"));
    CHECK(has(lines, text));
    // The right border of the results panel stays in one column on every row of the panel.
    const int top = findRow(lines, "Results -- 2 row(s)");
    REQUIRE(top >= 0);
    for (int y = top + 1; y < top + 8; ++y)
        CHECK(screen.PixelAt(width - 1, y).character == "\xE2\x94\x82");   // vertical bar
}

TEST_CASE("wbe2e a local connect is refused while a transaction is open, and works after WAPAS", "[wbe2e]") {
    E2e rig;
    rig.run("BANAO TABLE t (a ANK);");
    rig.run("SHURU;");
    rig.run("DAALO MEIN t MAAN (5);");
    CHECK(rig.screen()[0].find("TRANSACTION") != std::string::npos);

    // Ctrl+O, Local mode: refused with a red line; nothing changes, the transaction stays open.
    auto chooseLocal = [&] {
        rig.press(keys::ctrl('O'));
        for (int i = 0; i < 5; ++i) rig.press(Event::Tab);
        rig.press(Event::Return);
        rig.settle();
    };
    chooseLocal();
    CHECK(rig.logText().find("Connect nahi hua: ") != std::string::npos);
    CHECK(rig.logText().find("PAKKA ya WAPAS") != std::string::npos);
    CHECK(rig.screen()[0].find("TRANSACTION") != std::string::npos);
    CHECK(rig.s().inTransaction());

    rig.run("WAPAS;");
    chooseLocal();
    CHECK(rig.logText().find("Connected: local (" + rig.data.str() + ")") != std::string::npos);
    CHECK(rig.screen()[0].find("TRANSACTION") == std::string::npos);
    rig.run("DIKHAO GINO(*) SE t;");
    REQUIRE(rig.s().table() != nullptr);
    CHECK(rig.s().table()->rows.at(0).at(0).text == "0");
}

TEST_CASE("wbe2e a Tab inside a bracketed paste is text, a typed Tab moves the focus", "[wbe2e]") {
    E2e rig;
    rig.press(Event::Special(keys::kPasteStart));
    rig.type("a");
    CHECK(rig.press(Event::Tab));
    rig.type("b\nc");
    rig.press(Event::Special(keys::kPasteEnd));
    CHECK(rig.ui->focus() == Panel::Editor);
    CHECK(rig.s().editor().text() == "a    b\nc");   // the editor stores a Tab as four spaces
    // Outside a paste the Tab goes to the next panel.
    CHECK(rig.press(Event::Tab));
    CHECK(rig.ui->focus() == Panel::Tree);
    // A paste into another panel is not typed anywhere, and its Tabs do not move the focus.
    rig.press(Event::Special(keys::kPasteStart));
    rig.press(Event::Tab);
    rig.press(Event::Special(keys::kPasteEnd));
    CHECK(rig.ui->focus() == Panel::Tree);
    CHECK(rig.s().editor().text() == "a    b\nc");
}
