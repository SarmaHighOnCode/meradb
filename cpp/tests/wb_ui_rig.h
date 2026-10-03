// cpp/tests/wb_ui_rig.h -- a real Session over a FakeBackend plus the WorkbenchUi, driven with synthetic events.
#pragma once
#include "test_util.h"
#include "wb_screen_util.h"
#include "wb_test_util.h"
#include "meradb/wb_keys.h"
#include "meradb/wb_ui.h"
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace wbtest {

inline nlohmann::ordered_json uiSampleTree() {
    return nlohmann::ordered_json::parse(R"([
      {"name":"college","current":false,"tables":[]},
      {"name":"main","current":true,"tables":[
         {"name":"students","columns":[
            {"name":"id","type_name":"INT","primary_key":true,"unique":false,"not_null":true},
            {"name":"naam","type_name":"TEXT","primary_key":false,"unique":false,"not_null":true}]},
         {"name":"marks","columns":[{"name":"score","type_name":"FLOAT","primary_key":false,"unique":false,"not_null":false}]}]}])");
}

struct UiRig {
    FakeBackend* fake = nullptr;                 // valid until the session closes the backend (quit / connect)
    std::shared_ptr<FakeBackend::Shared> seen;   // what the backend saw; stays valid after that
    ManualPoster poster;
    meradb_test::TempDir exportDir;
    std::unique_ptr<meradb::wb::Session> session;
    std::unique_ptr<meradb::wb::WorkbenchUi> ui;
    int exits = 0;

    explicit UiRig(const std::function<void(FakeBackend&)>& setup = {}, meradb::wb::SessionOptions options = {}) {
        auto owned = std::make_unique<FakeBackend>();
        fake = owned.get();
        seen = owned->seen;
        owned->tree = uiSampleTree();
        if (setup) setup(*owned);
        if (options.exportBaseDir.empty()) options.exportBaseDir = exportDir.str();
        session = std::make_unique<meradb::wb::Session>(std::move(owned), std::move(options), poster.poster());
        session->setOnExit([this] { ++exits; });
        ui = std::make_unique<meradb::wb::WorkbenchUi>(*session);
        settle();
    }
    ~UiRig() {
        ui.reset();
        session.reset();
    }

    meradb::wb::Session& s() { return *session; }
    bool press(const ftxui::Event& e) { return ui->onEvent(e); }
    void type(const std::string& text) {
        for (std::size_t i = 0; i < text.size();) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            std::size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            n = std::min(n, text.size() - i);
            const std::string piece = text.substr(i, n);
            if (piece == "\n") press(ftxui::Event::Return);
            else press(ftxui::Event::Character(piece));
            i += n;
        }
    }
    void settle() { REQUIRE(poster.pumpIdle(*session)); }
    std::vector<std::string> screen(int w, int h, ftxui::Screen* keep = nullptr) {
        return renderLines(ui->render(), w, h, keep);
    }
    // Types the statement into an empty editor, runs it with F5 and waits.
    void runStatement(const std::string& text) {
        session->editor().setText("");
        type(text);
        press(ftxui::Event::F5);
        settle();
    }
    std::string logText() const {
        std::string out;
        for (const auto& entry : session->log().entries())
            for (const auto& line : entry.lines) out += meradb::wb::plainText(line) + "\n";
        return out;
    }
};

}  // namespace wbtest
