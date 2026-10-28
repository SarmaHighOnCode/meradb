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
#include <thread>

using namespace meradb;
using namespace meradb::gui;

namespace {

// A Session on a LocalBackend in a temporary data folder, delivering its closures into a UiPostQueue that the test
// drains (the main loop's job).
struct Rig {
    meradb_test::TempDir data{"meradb_gui_test_"};
    UiPostQueue queue;
    std::unique_ptr<wb::Session> session;
    bool exited = false;

    Rig() {
        wb::SessionOptions options;
        options.dataDir = data.str();
        options.exportBaseDir = data.str();
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
