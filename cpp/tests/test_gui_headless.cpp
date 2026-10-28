// cpp/tests/test_gui_headless.cpp -- real frames of the window's layout against a real session, with a Dear ImGui
// context but no window and no renderer. A failed IM_ASSERT throws (gui_test_util.h).
#include <catch2/catch_test_macros.hpp>
#include "gui_test_util.h"
#include "test_util.h"
#include "imgui_internal.h"
#include "meradb/backend.h"
#include "meradb/gui_poster.h"
#include "meradb/gui_ui.h"
#include "meradb/wb_session.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using namespace meradb;
using namespace meradb::gui;

namespace {

// A Session on a LocalBackend in a temporary data folder, delivering its closures into a UiPostQueue that the test
// drains (the main loop's job).
struct Rig {
    meradb_test::TempDir data{"meradb_gui_test_"};
    meradb_test::TempDir exports{"meradb_gui_export_"};   // CSV files go below here, never into the working directory
    UiPostQueue queue;
    std::unique_ptr<wb::Session> session;
    bool exited = false;

    Rig() {
        wb::SessionOptions options;
        options.dataDir = data.str();
        options.exportBaseDir = exports.str();
        session = std::make_unique<wb::Session>(std::make_unique<LocalBackend>(data.str()), options,
                                                [this](std::function<void()> fn) { queue.post(std::move(fn)); });
        session->setOnExit([this] { exited = true; });
        settle();
    }

    template <typename Done>
    bool pumpUntil(Done done, std::chrono::seconds timeout = std::chrono::seconds(20)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done()) {
            queue.drain();
            if (done()) return true;
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }
    bool settle() { return pumpUntil([this] { return !session->busy(); }); }
};

ImGuiWindow* panel(const char* name) { return ImGui::FindWindowByName(name); }

void pressChord(guitest::HeadlessImGui& gui, GuiUi& ui, ImGuiKey key) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(key, true);
    gui.frame(ui);
    io.AddKeyEvent(key, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    gui.frame(ui);
}

}  // namespace

TEST_CASE("gui headless: the empty layout draws all its panels", "[gui][headless]") {
    guitest::HeadlessImGui gui;
    Rig rig;
    GuiUi ui(*rig.session, gui.fonts(), ThemeMode::Dark);
    for (int i = 0; i < 4; ++i) gui.frame(ui);
    CHECK(gui.vertexCount() > 200);
    for (const char* name : {"##topbar", "##schema", "##query", "##results", "##status"}) {
        INFO(name);
        REQUIRE(panel(name) != nullptr);
        CHECK(panel(name)->WasActive);
    }
    // geometry: top bar 48px, status bar 24px, schema column 280px, everything inside the display
    CHECK(panel("##topbar")->Pos.y == 0.0f);
    CHECK(panel("##topbar")->Size.y == GuiUi::kTopBarHeight);
    CHECK(panel("##topbar")->Size.x == 1280.0f);
    CHECK(panel("##status")->Size.y == GuiUi::kStatusBarHeight);
    CHECK(panel("##status")->Pos.y == 800.0f - GuiUi::kStatusBarHeight);
    CHECK(panel("##schema")->Size.x == GuiUi::kSchemaWidth);
    CHECK(panel("##schema")->Pos.y == GuiUi::kTopBarHeight);
    CHECK(panel("##query")->Pos.x == GuiUi::kSchemaWidth);
    CHECK(panel("##results")->Pos.y == panel("##query")->Pos.y + panel("##query")->Size.y);
    CHECK(panel("##results")->Pos.y + panel("##results")->Size.y == panel("##status")->Pos.y);
}

TEST_CASE("gui headless: light and dark both draw, the switch applies at the next frame", "[gui][headless]") {
    guitest::HeadlessImGui gui;
    Rig rig;
    GuiUi ui(*rig.session, gui.fonts(), ThemeMode::Dark);
    gui.frame(ui);
    CHECK(ImGui::GetStyle().Colors[ImGuiCol_WindowBg].x == 0.0f);   // #000000
    ui.setMode(ThemeMode::Light);
    gui.frame(ui);
    CHECK(ui.mode() == ThemeMode::Light);
    CHECK(ImGui::GetStyle().Colors[ImGuiCol_WindowBg].x == 1.0f);   // #FFFFFF
    CHECK(gui.vertexCount() > 200);
}

TEST_CASE("gui headless: a clicked theme button toggles the mode", "[gui][headless]") {
    guitest::HeadlessImGui gui;
    Rig rig;
    GuiUi ui(*rig.session, gui.fonts(), ThemeMode::Dark);
    gui.frame(ui);
    gui.frame(ui);
    // the theme button is the right-most 32px square of the top bar
    const ImVec2 target(1280.0f - 16.0f - 16.0f, GuiUi::kTopBarHeight / 2.0f);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(target.x, target.y);
    gui.frame(ui);
    io.AddMouseButtonEvent(0, true);
    gui.frame(ui);
    io.AddMouseButtonEvent(0, false);
    gui.frame(ui);
    CHECK(ui.mode() == ThemeMode::Light);
}

TEST_CASE("gui headless: content scale doubles sizes and fonts", "[gui][headless]") {
    guitest::HeadlessImGui gui(2560.0f, 1600.0f);
    Rig rig;
    GuiUi ui(*rig.session, gui.fonts(), ThemeMode::Dark);
    gui.frame(ui, 1.0f);
    gui.frame(ui, 2.0f);
    gui.frame(ui, 2.0f);
    CHECK(ui.scale() == 2.0f);
    CHECK(ImGui::GetStyle().FontScaleDpi == 2.0f);
    CHECK(panel("##topbar")->Size.y == 2.0f * GuiUi::kTopBarHeight);
    CHECK(panel("##schema")->Size.x == 2.0f * GuiUi::kSchemaWidth);
    // the style's font size is the unscaled base: the scale is applied on top of it
    CHECK(ImGui::GetStyle().FontSizeBase == kUiFontSize);
    gui.frame(ui, 1.0f);
    gui.frame(ui, 1.0f);
    CHECK(panel("##topbar")->Size.y == GuiUi::kTopBarHeight);
}

TEST_CASE("gui headless: tiny and huge windows do not break the layout", "[gui][headless]") {
    guitest::HeadlessImGui gui;
    Rig rig;
    GuiUi ui(*rig.session, gui.fonts(), ThemeMode::Light);
    for (const ImVec2 size : {ImVec2(100.0f, 60.0f), ImVec2(640.0f, 400.0f), ImVec2(30.0f, 30.0f), ImVec2(7680.0f, 4320.0f)}) {
        gui.setDisplaySize(size.x, size.y);
        CHECK_NOTHROW(gui.frame(ui));
        CHECK_NOTHROW(gui.frame(ui));
    }
}

TEST_CASE("gui headless: the status bar and breadcrumb follow the session", "[gui][headless]") {
    guitest::HeadlessImGui gui;
    Rig rig;
    GuiUi ui(*rig.session, gui.fonts(), ThemeMode::Dark);
    CHECK(rig.session->description().rfind("local (", 0) == 0);
    CHECK_FALSE(rig.session->inTransaction());
    rig.session->runText("SHURU;");
    REQUIRE(rig.settle());
    CHECK(rig.session->inTransaction());
    CHECK_NOTHROW(gui.frame(ui));   // draws the TRANSACTION badge
    CHECK_NOTHROW(gui.frame(ui));
    rig.session->runText("WAPAS;");
    REQUIRE(rig.settle());
    CHECK_FALSE(rig.session->inTransaction());
    CHECK_NOTHROW(gui.frame(ui));
}

TEST_CASE("gui headless: busy frames and a result do not assert", "[gui][headless]") {
    guitest::HeadlessImGui gui;
    Rig rig;
    GuiUi ui(*rig.session, gui.fonts(), ThemeMode::Dark);
    rig.session->runText("BANAO TABLE t (a ANK); DAALO MEIN t MAAN (1), (2); DIKHAO * SE t;");
    CHECK(rig.session->busy());
    CHECK_NOTHROW(gui.frame(ui));   // "chal raha hai..."
    REQUIRE(rig.settle());
    REQUIRE(rig.session->table() != nullptr);
    CHECK(rig.session->table()->rows.size() == 2);
    CHECK_NOTHROW(gui.frame(ui));
    CHECK_NOTHROW(gui.frame(ui));
    rig.session->clearLog();
    CHECK_NOTHROW(gui.frame(ui));   // "Log khaali hai."
}

TEST_CASE("gui headless: Ctrl+Q runs the quit protocol and rolls an open transaction back", "[gui][headless]") {
    guitest::HeadlessImGui gui;
    Rig rig;
    GuiUi ui(*rig.session, gui.fonts(), ThemeMode::Dark);
    rig.session->runText("BANAO TABLE t (a ANK); SHURU; DAALO MEIN t MAAN (1);");
    REQUIRE(rig.settle());
    REQUIRE(rig.session->inTransaction());
    gui.frame(ui);
    gui.frame(ui);
    CHECK_FALSE(rig.session->quitting());

    pressChord(gui, ui, ImGuiKey_Q);
    CHECK(rig.session->quitting());
    CHECK_NOTHROW(gui.frame(ui));   // "band ho raha hai..."
    REQUIRE(rig.pumpUntil([&] { return rig.exited; }));   // the loop would leave here
    rig.session->shutdown();
    rig.queue.close();
    rig.session.reset();

    // the open transaction was rolled back: its row is not in the folder
    LocalBackend reopened(rig.data.str());
    CHECK_FALSE(reopened.inTransaction());
    const std::vector<Result> rows = reopened.runScript("DIKHAO * SE t;");
    REQUIRE(rows.size() == 1);
    if (rows[0].error.empty()) CHECK(rows[0].rows.empty());
    reopened.close();
}

TEST_CASE("gui headless: Q without Ctrl does not quit", "[gui][headless]") {
    guitest::HeadlessImGui gui;
    Rig rig;
    GuiUi ui(*rig.session, gui.fonts(), ThemeMode::Dark);
    ImGuiIO& io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiKey_Q, true);   // Q without Ctrl is a plain key
    gui.frame(ui);
    io.AddKeyEvent(ImGuiKey_Q, false);
    gui.frame(ui);
    CHECK_FALSE(rig.session->quitting());
}


// ---------------------------------------------------------------- the panels, driven like a user would

namespace {

// A window with a session, driven through Dear ImGui's input events.
struct Screen {
    guitest::HeadlessImGui gui;
    Rig rig;
    GuiUi ui;

    Screen() : gui(), rig(), ui(*rig.session, gui.fonts(), ThemeMode::Dark) { frames(3); }

    void frames(int n = 1) {
        for (int i = 0; i < n; ++i) gui.frame(ui);
    }
    bool run(const std::string& script) {
        rig.session->runText(script);
        const bool done = rig.settle();
        frames(4);
        return done;
    }

    // a key, with modifiers held around it
    void press(ImGuiKey key, bool ctrl = false, bool shift = false, bool alt = false) {
        ImGuiIO& io = ImGui::GetIO();
        if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
        if (alt) io.AddKeyEvent(ImGuiMod_Alt, true);
        if (ctrl || shift || alt) frames();
        io.AddKeyEvent(key, true);
        frames();
        io.AddKeyEvent(key, false);
        frames();
        if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
        if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
        if (alt) io.AddKeyEvent(ImGuiMod_Alt, false);
        if (ctrl || shift || alt) frames();
    }
    void type(const std::string& utf8) {
        ImGui::GetIO().AddInputCharactersUTF8(utf8.c_str());
        frames(2);
    }
    void moveTo(ImVec2 p) {
        ImGui::GetIO().AddMousePosEvent(p.x, p.y);
        frames();
    }
    // Clicks `times` times in a row (a double click is 2). A pause comes first, so that this never counts as the
    // continuation of the click before it.
    void click(ImVec2 p, int times = 1, ImGuiMouseButton button = 0, bool shift = false) {
        ImGuiIO& io = ImGui::GetIO();
        frames(25);
        moveTo(p);
        if (shift) {
            io.AddKeyEvent(ImGuiMod_Shift, true);
            frames();
        }
        for (int i = 0; i < times; ++i) {
            io.AddMouseButtonEvent(button, true);
            frames();
            io.AddMouseButtonEvent(button, false);
            frames();
        }
        if (shift) {
            io.AddKeyEvent(ImGuiMod_Shift, false);
            frames();
        }
    }
    void drag(ImVec2 from, ImVec2 to) {
        ImGuiIO& io = ImGui::GetIO();
        frames(25);
        moveTo(from);
        io.AddMouseButtonEvent(0, true);
        frames();
        for (int i = 1; i <= 4; ++i)
            moveTo(ImVec2(from.x + (to.x - from.x) * static_cast<float>(i) / 4.0f, from.y + (to.y - from.y) * static_cast<float>(i) / 4.0f));
        io.AddMouseButtonEvent(0, false);
        frames();
    }
    // a point just right of the left edge of character (row, col) of the editor: a click there puts the caret at col
    ImVec2 at(int row, int col) const {
        const EditorWidget::Layout& l = ui.editorLayout();
        return ImVec2(l.origin.x + (static_cast<float>(col) + 0.25f) * l.charWidth, l.origin.y + (static_cast<float>(row) + 0.5f) * l.lineHeight);
    }
    wb::TextBuffer& buffer() { return rig.session->editor(); }
    std::string text() { return buffer().text(); }
    ImVec2 treeRow(wb::NodeKey::Kind kind, const std::string& name) {
        for (const SchemaTree::RowHit& hit : ui.schemaTree().rowHits())
            if (hit.key.kind == kind && nodeName(hit.key) == name) return ImVec2((hit.min.x + hit.max.x) * 0.5f, (hit.min.y + hit.max.y) * 0.5f);
        return ImVec2(-1, -1);
    }
    int logCount(const std::string& needle) {
        int count = 0;
        for (const wb::LogEntry& e : rig.session->log().entries())
            for (const wb::Line& l : e.lines)
                if (wb::plainText(l).find(needle) != std::string::npos) ++count;
        return count;
    }
    bool logHas(const std::string& needle) { return logCount(needle) > 0; }
};

}  // namespace

TEST_CASE("gui panels: typing a script and Ctrl+Enter runs it and the grid shows the rows", "[gui][headless][panels]") {
    Screen s;
    s.click(s.at(0, 0));
    REQUIRE(s.ui.editor().focused());
    s.type("BANAO TABLE t (a ANK, b TEXT);");
    s.press(ImGuiKey_Enter);
    s.type("DAALO MEIN t MAAN (1, 'x'), (2, 'y');");
    s.press(ImGuiKey_Enter);
    s.type("DIKHAO * SE t;");
    CHECK(s.text() == "BANAO TABLE t (a ANK, b TEXT);\nDAALO MEIN t MAAN (1, 'x'), (2, 'y');\nDIKHAO * SE t;");
    s.press(ImGuiKey_Enter, true);
    CHECK(s.rig.session->busy());
    CHECK_NOTHROW(s.frames(2));                          // "chal raha hai..." with its spinner
    REQUIRE(s.rig.settle());
    s.frames(4);
    REQUIRE(s.rig.session->table() != nullptr);
    CHECK(s.rig.session->table()->rows.size() == 2);
    CHECK(s.rig.session->table()->columns == std::vector<std::string>{"a", "b"});
    CHECK(s.ui.tab() == GuiUi::Tab::Results);
    CHECK(s.ui.lastRunMs() >= 0.0);
    CHECK(s.ui.grid().cellHits().size() == 4);           // two rows of two cells, all in view
    CHECK(s.rig.session->history().size() == 1);
    CHECK(s.text().find("DIKHAO") != std::string::npos);   // the editor keeps its text
}

TEST_CASE("gui panels: keys without the editor in focus do not type", "[gui][headless][panels]") {
    Screen s;
    s.type("abc");
    CHECK(s.text().empty());
    s.click(s.at(0, 0));
    s.type("abc");
    CHECK(s.text() == "abc");
    s.click(ImVec2(800.0f, 700.0f));                     // the results panel
    CHECK_FALSE(s.ui.editor().focused());
    s.type("def");
    CHECK(s.text() == "abc");
}

TEST_CASE("gui panels: F5, F6 and Ctrl+Shift+Enter run and explain", "[gui][headless][panels]") {
    Screen s;
    REQUIRE(s.run("BANAO TABLE t (a ANK); DAALO MEIN t MAAN (1);"));
    s.buffer().setText("DIKHAO * SE t;");
    s.press(ImGuiKey_F5);
    REQUIRE(s.rig.settle());
    s.frames(4);
    CHECK(s.rig.session->table() != nullptr);
    CHECK(s.logCount("SAMJHAO") == 0);
    s.press(ImGuiKey_Enter, true, true);                 // Ctrl+Shift+Enter: Samjhao
    REQUIRE(s.rig.settle());
    s.frames(4);
    CHECK(s.logCount("SAMJHAO DIKHAO * SE t;") == 1);
    s.press(ImGuiKey_F6);
    REQUIRE(s.rig.settle());
    s.frames(2);
    CHECK(s.logCount("SAMJHAO DIKHAO * SE t;") == 2);
}

TEST_CASE("gui panels: Pichli and Agli walk the history with the keys", "[gui][headless][panels]") {
    Screen s;
    s.press(ImGuiKey_UpArrow, false, false, true);       // empty history: nothing happens
    CHECK(s.text().empty());
    s.buffer().setText("BANAO TABLE h (a ANK);");
    s.press(ImGuiKey_Enter, true);
    REQUIRE(s.rig.settle());
    s.buffer().setText("DIKHAO * SE h;");
    s.press(ImGuiKey_F5);
    REQUIRE(s.rig.settle());
    s.frames(3);
    s.press(ImGuiKey_UpArrow, false, false, true);       // Alt+Up
    CHECK(s.text() == "DIKHAO * SE h;");
    s.press(ImGuiKey_UpArrow, true);                     // Ctrl+Up
    CHECK(s.text() == "BANAO TABLE h (a ANK);");
    s.press(ImGuiKey_DownArrow, false, false, true);     // Alt+Down
    CHECK(s.text() == "DIKHAO * SE h;");
    s.press(ImGuiKey_DownArrow, true);                   // Ctrl+Down
    CHECK(s.text().empty());                             // past the newest entry: empty, like the terminal workbench
    s.press(ImGuiKey_Z, true);                           // a history step is one undo step
    CHECK(s.text() == "DIKHAO * SE h;");
}

TEST_CASE("gui panels: a double click on a table runs its rows, on a column inserts its name", "[gui][headless][panels]") {
    Screen s;
    REQUIRE(s.run("BANAO DATABASE d2; ISTEMAL d2; BANAO TABLE t (a ANK, b TEXT); DAALO MEIN t MAAN (7, 'q');"));
    const ImVec2 row = s.treeRow(wb::NodeKey::Kind::Table, "t");
    REQUIRE(row.x > 0.0f);
    s.click(row);                                        // a click only selects
    CHECK(s.ui.schemaTree().selected().table == "t");
    CHECK(s.rig.session->history().size() == 1);
    const int version = s.rig.session->resultVersion();
    s.click(row, 2);                                     // a double click activates
    REQUIRE(s.rig.settle());
    s.frames(4);
    REQUIRE(s.rig.session->table() != nullptr);
    CHECK(s.rig.session->resultVersion() > version);
    CHECK(s.rig.session->table()->rows.size() == 1);
    CHECK(s.rig.session->table()->rows[0][0].text == "7");
    CHECK(s.rig.session->history().items().back() == "DIKHAO * SE t SIRF 100;");
    // activating a table also opened it: its columns are listed
    const ImVec2 column = s.treeRow(wb::NodeKey::Kind::Column, "b");
    REQUIRE(column.x > 0.0f);
    s.click(column, 2);
    CHECK(s.text() == "b");                              // inserted at the caret
    CHECK(s.ui.editor().focused());
    s.type("x");                                         // and typing goes on there
    CHECK(s.text() == "bx");
    s.press(ImGuiKey_Z, true);                           // typing is one step, the insertion another
    CHECK(s.text() == "b");
}

TEST_CASE("gui panels: Enter on the selected tree row activates it, the arrows move", "[gui][headless][panels]") {
    Screen s;
    REQUIRE(s.run("BANAO TABLE t (a ANK); BANAO TABLE u (b ANK); DAALO MEIN u MAAN (3);"));
    s.click(s.treeRow(wb::NodeKey::Kind::Table, "t"));
    CHECK(s.ui.schemaTree().focused());
    s.press(ImGuiKey_DownArrow);                         // t is closed: the next row is u
    CHECK(s.ui.schemaTree().selected().table == "u");
    s.press(ImGuiKey_Enter);
    REQUIRE(s.rig.settle());
    s.frames(3);
    REQUIRE(s.rig.session->table() != nullptr);
    CHECK(s.rig.session->table()->rows[0][0].text == "3");
    s.press(ImGuiKey_UpArrow);
    CHECK(s.ui.schemaTree().selected().table == "t");
}

TEST_CASE("gui panels: the filter hides what does not match and keeps the parents", "[gui][headless][panels]") {
    Screen s;
    REQUIRE(s.run("BANAO TABLE students (id INT, naam TEXT); BANAO TABLE courses (id INT, title TEXT);"));
    const std::size_t all = s.ui.schemaTree().entries().size();
    CHECK(all >= 3);
    s.ui.schemaTree().setFilter("TITLE");
    s.frames(3);
    std::vector<std::string> names;
    for (const TreeEntry& e : s.ui.schemaTree().entries()) names.push_back(e.name);
    REQUIRE(names.size() == 3);                          // the database, courses, title
    CHECK(names.back() == "title");
    s.ui.schemaTree().setFilter("zzz");
    CHECK_NOTHROW(s.frames(3));                          // "Kuch nahi mila."
    CHECK(s.ui.schemaTree().entries().empty());
    s.ui.schemaTree().setFilter("");
    s.frames(2);
    CHECK(s.ui.schemaTree().entries().size() == all);
}

TEST_CASE("gui panels: a right click on a table opens the menu", "[gui][headless][panels]") {
    Screen s;
    REQUIRE(s.run("BANAO TABLE t (a ANK);"));
    const ImVec2 row = s.treeRow(wb::NodeKey::Kind::Table, "t");
    REQUIRE(row.x > 0.0f);
    s.click(row, 1, 1);
    CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == 1);
    CHECK(s.ui.schemaTree().selected().table == "t");
    CHECK_NOTHROW(s.frames(3));
    s.press(ImGuiKey_Escape);
    CHECK(ImGui::GetCurrentContext()->OpenPopupStack.Size == 0);
}

TEST_CASE("gui panels: Ctrl+S writes the CSV below the export folder and the tab stays", "[gui][headless][panels]") {
    Screen s;
    s.press(ImGuiKey_S, true);                           // nothing to save yet: a hint in the log
    CHECK(s.logHas("Pehle koi DIKHAO query chalao"));
    REQUIRE(s.run("BANAO TABLE t (a ANK, b TEXT); DAALO MEIN t MAAN (1, 'x,y'), (2, 'z'); DIKHAO * SE t;"));
    CHECK(s.ui.tab() == GuiUi::Tab::Results);
    s.press(ImGuiKey_S, true);
    s.frames(3);
    CHECK(s.ui.tab() == GuiUi::Tab::Results);            // saving is not the end of a run: no switch to the log
    CHECK(s.logHas("2 row(s) CSV mein save"));
    const std::filesystem::path folder = std::filesystem::path(s.rig.exports.str()) / "exports";
    REQUIRE(std::filesystem::exists(folder));
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) files.push_back(entry.path());
    REQUIRE(files.size() == 1);
    CHECK(files[0].extension() == ".csv");
    std::ifstream in(files[0], std::ios::binary);
    std::stringstream body;
    body << in.rdbuf();
    CHECK(body.str() == "a,b\r\n1,\"x,y\"\r\n2,z\r\n");
    CHECK_FALSE(std::filesystem::exists(std::filesystem::path(s.rig.data.str()) / "exports"));
}

TEST_CASE("gui panels: results go to Results, messages and errors without a table go to Log", "[gui][headless][panels]") {
    Screen s;
    CHECK(s.ui.tab() == GuiUi::Tab::Results);
    REQUIRE(s.run("DIKHAO * SE nahi_hai;"));             // an error
    CHECK(s.ui.tab() == GuiUi::Tab::Log);
    REQUIRE(s.run("BANAO TABLE t (a ANK); DAALO MEIN t MAAN (1); DIKHAO * SE t;"));   // messages and a table
    CHECK(s.ui.tab() == GuiUi::Tab::Results);
    REQUIRE(s.run("DAALO MEIN t MAAN (2);"));            // only a message
    CHECK(s.ui.tab() == GuiUi::Tab::Log);
    REQUIRE(s.run("DIKHAO * SE t;"));
    CHECK(s.ui.tab() == GuiUi::Tab::Results);
    CHECK(s.ui.lastRunMs() >= 0.0);                      // the header time is the one of the run that made the table
    // Ctrl+L clears the log; both tabs draw in every state
    s.press(ImGuiKey_L, true);
    CHECK(s.rig.session->log().entries().empty());
    s.ui.showTab(GuiUi::Tab::Log);
    CHECK_NOTHROW(s.frames(3));                          // "Log khaali hai."
    CHECK(s.ui.tab() == GuiUi::Tab::Log);
    s.ui.showTab(GuiUi::Tab::Results);
    CHECK_NOTHROW(s.frames(3));
    CHECK(s.ui.tab() == GuiUi::Tab::Results);
}

TEST_CASE("gui panels: a clicked cell is copied with Ctrl+C, the row with Ctrl+Shift+C", "[gui][headless][panels]") {
    Screen s;
    REQUIRE(s.run("BANAO TABLE t (a ANK, b TEXT); DAALO MEIN t MAAN (1, 'x'), (2, 'y'), (3, KHALI); DIKHAO * SE t;"));
    ImVec2 target(-1, -1);
    for (const ResultsGrid::CellHit& hit : s.ui.grid().cellHits())
        if (hit.row == 1 && hit.col == 1) target = ImVec2((hit.min.x + hit.max.x) * 0.5f, (hit.min.y + hit.max.y) * 0.5f);
    REQUIRE(target.x > 0.0f);
    s.click(target);
    CHECK(s.ui.grid().selectedRow() == 1);
    CHECK(s.ui.grid().selectedColumn() == 1);
    s.press(ImGuiKey_C, true);
    CHECK(guitest::fakeClipboard() == "y");
    s.press(ImGuiKey_C, true, true);
    CHECK(guitest::fakeClipboard() == "2\ty");
    REQUIRE(s.run("DIKHAO * SE t JAHAN a = 3;"));        // a new table starts without a selection
    CHECK_FALSE(s.ui.grid().hasSelection());
}

TEST_CASE("gui panels: the TRANSACTION badge follows SHURU and WAPAS", "[gui][headless][panels]") {
    Screen s;
    CHECK_FALSE(s.ui.transactionBadgeShown());
    REQUIRE(s.run("BANAO TABLE t (a ANK); SHURU;"));
    CHECK(s.ui.transactionBadgeShown());
    REQUIRE(s.run("WAPAS;"));
    CHECK_FALSE(s.ui.transactionBadgeShown());
    REQUIRE(s.run("SHURU; DAALO MEIN t MAAN (1); PAKKA;"));
    CHECK_FALSE(s.ui.transactionBadgeShown());
}

TEST_CASE("gui panels: typing SHURU and a row, then Ctrl+Q, closes the backend and rolls back", "[gui][headless][panels]") {
    Screen s;
    REQUIRE(s.run("BANAO TABLE t (a ANK);"));
    s.click(s.at(0, 0));
    s.type("SHURU; DAALO MEIN t MAAN (5);");
    s.press(ImGuiKey_Enter, true);
    REQUIRE(s.rig.settle());
    s.frames(3);
    REQUIRE(s.ui.transactionBadgeShown());
    s.press(ImGuiKey_Q, true);
    CHECK(s.rig.session->quitting());
    CHECK_NOTHROW(s.frames(2));
    REQUIRE(s.rig.pumpUntil([&] { return s.rig.exited; }));
    s.rig.session->shutdown();
    s.rig.queue.close();
    s.rig.session.reset();
    LocalBackend reopened(s.rig.data.str());
    CHECK_FALSE(reopened.inTransaction());
    const std::vector<Result> rows = reopened.runScript("DIKHAO * SE t;");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].error.empty());
    CHECK(rows[0].rows.empty());
    reopened.close();
}

TEST_CASE("gui panels: the splitters move the sidebar and the editor edge", "[gui][headless][panels]") {
    Screen s;
    CHECK(s.ui.schemaWidth() == GuiUi::kSchemaWidth);
    const float body = 800.0f - GuiUi::kTopBarHeight - GuiUi::kStatusBarHeight;
    s.drag(ImVec2(GuiUi::kSchemaWidth, 400.0f), ImVec2(GuiUi::kSchemaWidth + 100.0f, 400.0f));
    CHECK(s.ui.schemaWidth() == GuiUi::kSchemaWidth + 100.0f);
    CHECK(panel("##schema")->Size.x == GuiUi::kSchemaWidth + 100.0f);
    CHECK(panel("##query")->Pos.x == GuiUi::kSchemaWidth + 100.0f);
    const float edge = panel("##query")->Pos.y + panel("##query")->Size.y;
    const float before = s.ui.queryFraction();
    s.drag(ImVec2(900.0f, edge), ImVec2(900.0f, edge + 80.0f));
    CHECK(s.ui.queryFraction() > before + 80.0f / body - 0.01f);
    CHECK(panel("##results")->Pos.y == panel("##query")->Pos.y + panel("##query")->Size.y);
    // pushed to the ends the parts keep a minimum
    s.drag(ImVec2(900.0f, panel("##results")->Pos.y), ImVec2(900.0f, 790.0f));
    CHECK(panel("##results")->Size.y >= 120.0f - 1.0f);
    s.drag(ImVec2(s.ui.schemaWidth(), 400.0f), ImVec2(1270.0f, 400.0f));
    CHECK(s.ui.schemaWidth() <= 640.0f);
    CHECK_NOTHROW(s.frames(3));
}

TEST_CASE("gui panels: busy shows the spinner text, Pichli and Agli draw disabled with no history", "[gui][headless][panels]") {
    Screen s;
    CHECK(s.rig.session->history().size() == 0);
    CHECK_NOTHROW(s.frames(2));                          // disabled buttons
    s.rig.session->runText("BANAO TABLE t (a ANK); DAALO MEIN t MAAN (1); DIKHAO * SE t;");
    CHECK(s.rig.session->busy());
    CHECK_NOTHROW(s.frames(3));
    REQUIRE(s.rig.settle());
    CHECK_NOTHROW(s.frames(3));
    CHECK(s.rig.session->history().size() == 1);         // enabled now
}

// ---- the editor itself ----

TEST_CASE("gui editor: mouse selection, word and line selection, shift-click", "[gui][headless][editor]") {
    Screen s;
    s.buffer().setText("hello world\nsecond line");
    s.frames(2);
    s.click(s.at(0, 5));
    CHECK(s.buffer().cursor() == (wb::Pos{0, 5}));
    CHECK_FALSE(s.buffer().hasSelection());
    s.click(s.at(0, 8), 1, 0, true);                     // shift-click extends
    CHECK(s.buffer().selectedText() == " wo");
    s.click(s.at(0, 8), 2);                              // double click: the word
    CHECK(s.buffer().selectedText() == "world");
    s.click(s.at(1, 3), 3);                              // triple click: the line
    CHECK(s.buffer().selectedText() == "second line");
    s.drag(s.at(0, 2), s.at(0, 7));
    CHECK(s.buffer().selectedText() == "llo w");
    s.drag(s.at(0, 3), s.at(1, 4));                      // across lines
    CHECK(s.buffer().selectedText() == "lo world\nseco");
    s.click(ImVec2(s.at(0, 0).x, s.at(1, 0).y + 100.0f));   // below the text: the last line
    CHECK(s.buffer().cursor() == (wb::Pos{1, 0}));
}

TEST_CASE("gui editor: keyboard movement and selection", "[gui][headless][editor]") {
    Screen s;
    s.click(s.at(0, 0));
    s.type("one two three");
    s.press(ImGuiKey_LeftArrow, true);                   // Ctrl+Left: a word
    CHECK(s.buffer().cursor() == (wb::Pos{0, 8}));
    s.press(ImGuiKey_LeftArrow, true, true);             // Ctrl+Shift+Left: extend by a word
    CHECK(s.buffer().cursor() == (wb::Pos{0, 4}));
    CHECK(s.buffer().selectedText() == "two ");
    s.press(ImGuiKey_RightArrow);                        // collapses to the right end
    CHECK(s.buffer().cursor() == (wb::Pos{0, 8}));
    CHECK_FALSE(s.buffer().hasSelection());
    s.press(ImGuiKey_Home);
    CHECK(s.buffer().cursor() == (wb::Pos{0, 0}));
    s.press(ImGuiKey_End, false, true);                  // Shift+End selects to the end of the line
    CHECK(s.buffer().selectedText() == "one two three");
    s.press(ImGuiKey_Home, true);                        // Ctrl+Home
    CHECK(s.buffer().cursor() == (wb::Pos{0, 0}));
    CHECK_FALSE(s.buffer().hasSelection());
    s.press(ImGuiKey_End, true);                         // Ctrl+End
    s.press(ImGuiKey_Enter);
    s.type("second");
    s.press(ImGuiKey_UpArrow);
    CHECK(s.buffer().cursor().row == 0);
    s.press(ImGuiKey_End, true);
    CHECK(s.buffer().cursor() == (wb::Pos{1, 6}));
    s.press(ImGuiKey_A, true);
    CHECK(s.buffer().selectedText() == "one two three\nsecond");
}

TEST_CASE("gui editor: clipboard, delete keys and Tab", "[gui][headless][editor]") {
    Screen s;
    s.click(s.at(0, 0));
    s.type("select me");
    s.press(ImGuiKey_A, true);
    s.press(ImGuiKey_C, true);
    CHECK(guitest::fakeClipboard() == "select me");
    s.press(ImGuiKey_X, true);                           // cut
    CHECK(s.text().empty());
    s.press(ImGuiKey_V, true);                           // paste it twice
    s.press(ImGuiKey_V, true);
    CHECK(s.text() == "select meselect me");
    guitest::fakeClipboard() = "a\tb\nc\x01";             // a tab becomes 4 spaces, control characters are dropped
    s.press(ImGuiKey_A, true);
    s.press(ImGuiKey_V, true);
    CHECK(s.text() == "a    b\nc");
    s.press(ImGuiKey_Backspace);
    CHECK(s.text() == "a    b\n");
    s.press(ImGuiKey_Backspace);                         // joins the lines
    CHECK(s.text() == "a    b");
    s.press(ImGuiKey_Home);
    s.press(ImGuiKey_Delete);
    CHECK(s.text() == "    b");
    s.press(ImGuiKey_Tab);                               // Tab: the TextBuffer rule, 4 spaces
    CHECK(s.text() == "        b");
    s.press(ImGuiKey_Tab, false, true);                  // Shift+Tab does nothing
    CHECK(s.text() == "        b");
}

TEST_CASE("gui editor: undo and redo, typing merged until a pause or another key", "[gui][headless][editor]") {
    Screen s;
    s.click(s.at(0, 0));
    s.type("abc");
    s.press(ImGuiKey_Backspace);
    s.type("d");                                         // one step: abd
    CHECK(s.text() == "abd");
    s.press(ImGuiKey_Z, true);
    CHECK(s.text().empty());
    s.press(ImGuiKey_Y, true);                           // redo
    CHECK(s.text() == "abd");
    s.press(ImGuiKey_Z, true);
    s.press(ImGuiKey_Z, true, true);                     // Ctrl+Shift+Z is redo too
    CHECK(s.text() == "abd");
    s.press(ImGuiKey_Z, true);
    CHECK(s.text().empty());
    // Enter and the arrows end a step
    s.type("a");
    s.press(ImGuiKey_Enter);
    s.type("b");
    s.press(ImGuiKey_LeftArrow);
    s.press(ImGuiKey_RightArrow);
    s.type("c");
    CHECK(s.text() == "a\nbc");
    s.press(ImGuiKey_Z, true);
    CHECK(s.text() == "a\nb");
    s.press(ImGuiKey_Z, true);
    CHECK(s.text() == "a\n");
    s.press(ImGuiKey_Z, true);
    CHECK(s.text() == "a");
    s.press(ImGuiKey_Z, true);
    CHECK(s.text().empty());
    s.press(ImGuiKey_Z, true);                           // nothing left: no harm
    CHECK(s.text().empty());
    // after a pause typing starts a new step
    s.type("one");
    s.frames(90);                                        // 1.5 s
    s.type("two");
    CHECK(s.text() == "onetwo");
    s.press(ImGuiKey_Z, true);
    CHECK(s.text() == "one");
    // undo gives back what a selection replaced
    s.press(ImGuiKey_A, true);
    s.type("Z");
    CHECK(s.text() == "Z");
    s.press(ImGuiKey_Z, true);
    CHECK(s.text() == "one");
}

TEST_CASE("gui editor: UTF-8 input, including characters beyond the BMP", "[gui][headless][editor]") {
    Screen s;
    s.click(s.at(0, 0));
    s.type("caf\xC3\xA9 \xE0\xA4\xA8");                   // an accent and a Devanagari letter
    ImGui::GetIO().AddInputCharacter(0x1F600);            // an emoji, as one code point
    s.frames(2);
    CHECK(s.text() == "caf\xC3\xA9 \xE0\xA4\xA8\xF0\x9F\x98\x80");
    CHECK(s.buffer().cursor() == (wb::Pos{0, 7}));
    CHECK_NOTHROW(s.frames(3));                           // draws (a glyph the font lacks shows as its fallback)
    s.press(ImGuiKey_Backspace);                          // one code point at a time
    CHECK(s.text() == "caf\xC3\xA9 \xE0\xA4\xA8");
    ImGui::GetIO().AddInputCharacter(0x07);               // control characters never get in
    ImGui::GetIO().AddInputCharacter(0x7F);
    s.frames(2);
    CHECK(s.text() == "caf\xC3\xA9 \xE0\xA4\xA8");
}

TEST_CASE("gui editor: the caret stays in view and the IME is placed at it", "[gui][headless][editor]") {
    Screen s;
    std::string many;
    for (int i = 0; i < 120; ++i) many += "line " + std::to_string(i) + "\n";
    s.buffer().setText(many);
    s.click(s.at(0, 0));
    s.press(ImGuiKey_End, true);                          // the caret jumps to the last line
    s.frames(6);
    CHECK(s.ui.editor().scrollY() > 100.0f);
    const EditorWidget::Layout l = s.ui.editor().layout();
    const float caretY = l.origin.y + static_cast<float>(s.buffer().cursor().row) * l.lineHeight;
    CHECK(caretY >= l.viewMin.y - 1.0f);
    CHECK(caretY + l.lineHeight <= l.viewMax.y + 1.0f);
    const ImGuiPlatformImeData& ime = ImGui::GetCurrentContext()->PlatformImeData;
    CHECK(ime.WantVisible);
    CHECK(ime.InputPos.y >= l.viewMin.y - 1.0f);
    CHECK(ime.InputPos.y <= l.viewMax.y);
    CHECK(ime.InputLineHeight == l.lineHeight);
    s.press(ImGuiKey_Home, true);
    s.frames(6);
    CHECK(s.ui.editor().scrollY() == 0.0f);
    s.press(ImGuiKey_PageDown);                           // about a screenful, with the caret kept in view
    CHECK(s.buffer().cursor().row > 5);
    s.frames(6);
    const EditorWidget::Layout l2 = s.ui.editor().layout();
    const float y2 = l2.origin.y + static_cast<float>(s.buffer().cursor().row) * l2.lineHeight;
    CHECK(y2 + l2.lineHeight <= l2.viewMax.y + 1.0f);
    s.press(ImGuiKey_PageUp);
    CHECK(s.buffer().cursor().row == 0);
}

TEST_CASE("gui editor: a very long line draws only its visible columns", "[gui][headless][editor]") {
    Screen s;
    s.buffer().setText(std::string(1000000, 'x') + " SE students");
    s.click(s.at(0, 0));
    s.frames(3);
    CHECK(s.gui.vertexCount() < 60000);                   // a few hundred characters, not a million
    s.press(ImGuiKey_End);                                // scrolls to the far end
    s.frames(6);
    CHECK(s.ui.editor().scrollX() > 1000.0f);
    CHECK(s.gui.vertexCount() < 60000);
}

TEST_CASE("gui editor: the placeholder shows while the editor is empty", "[gui][headless][editor]") {
    Screen s;
    const int empty = s.gui.vertexCount();
    s.buffer().setText("x");
    s.frames(2);
    CHECK(s.gui.vertexCount() < empty);                   // the long hint is gone
}
