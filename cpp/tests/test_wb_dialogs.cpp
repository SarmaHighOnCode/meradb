// cpp/tests/test_wb_dialogs.cpp -- the connect and help dialogs, and the mouse, through the whole window.
#include "wb_ui_rig.h"
#include "meradb/errors.h"
#include <future>
#include <mutex>

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

void serverDefaults(FakeBackend& f) { f.desc = "127.0.0.1:6372"; }

bool screenHas(const std::vector<std::string>& lines, const std::string& needle) { return findRow(lines, needle) >= 0; }

// Records the request of the connect dialog; optionally blocks, optionally fails; otherwise builds another fake.
struct FactoryProbe {
    std::mutex m;
    std::vector<ConnectRequest> requests;
    std::shared_future<void> gate;
    bool useGate = false;
    bool fail = false;
    BackendFactory factory() {
        return [this](const ConnectRequest& r, const std::string&) -> std::unique_ptr<Backend> {
            {
                std::lock_guard<std::mutex> l(m);
                requests.push_back(r);
            }
            if (useGate) gate.wait();
            if (fail) throw ConnectionFailed("nope");
            auto next = std::make_unique<FakeBackend>();
            next->desc = r.local ? "local (fake)" : r.host + ":" + r.port;
            next->db = "other";
            return next;
        };
    }
    std::size_t count() {
        std::lock_guard<std::mutex> l(m);
        return requests.size();
    }
};

Event mouseEvent(Mouse::Button button, int x, int y) {
    Mouse mouse;
    mouse.button = button;
    mouse.motion = Mouse::Pressed;
    mouse.x = x;
    mouse.y = y;
    return Event::Mouse("", mouse);
}

}  // namespace

TEST_CASE("wbui dialog Ctrl+O opens the connect form and blocks other keys", "[wbui]") {
    UiRig rig(serverDefaults);
    rig.type("DIKHAO 1;");
    CHECK(rig.press(keys::ctrl('O')));
    CHECK(rig.s().modal() == Modal::Connect);
    auto lines = rig.screen(120, 40);
    INFO(dumpLines(lines));
    CHECK(screenHas(lines, "MeraDB server se connect karo"));
    CHECK(screenHas(lines, "Host"));
    CHECK(screenHas(lines, "127.0.0.1"));
    CHECK(screenHas(lines, "6372"));
    CHECK(screenHas(lines, "Password (agar server par hai)"));
    CHECK(screenHas(lines, "Database (optional)"));
    CHECK(screenHas(lines, "[ Connect ]"));
    CHECK(screenHas(lines, "[ Local mode ]"));
    CHECK(screenHas(lines, "[ Cancel ]"));

    const std::size_t ran = rig.fake->ranList().size();
    const std::size_t logged = rig.s().log().entries().size();
    CHECK(rig.press(Event::F5));
    CHECK(rig.press(keys::ctrl('S')));
    CHECK(rig.press(keys::ctrl('C')));
    CHECK(rig.press(keys::ctrl('L')));
    CHECK(rig.press(Event::F6));
    CHECK(rig.press(keys::ctrl('P')));
    CHECK(rig.press(keys::ctrl('O')));
    CHECK(rig.press(Event::F1));
    rig.settle();
    CHECK(rig.fake->ranList().size() == ran);
    CHECK(rig.s().log().entries().size() == logged);
    CHECK(rig.s().modal() == Modal::Connect);
    CHECK(rig.s().editor().text() == "DIKHAO 1;");

    CHECK(rig.press(keys::ctrl('Q')));   // quitting still works
    CHECK(rig.s().quitting());
    REQUIRE(rig.poster.pumpUntil([&] { return rig.exits > 0; }));
}

TEST_CASE("wbui dialog typing, hidden password, Tab and Escape", "[wbui]") {
    FactoryProbe probe;
    SessionOptions options;
    options.factory = probe.factory();
    UiRig rig(serverDefaults, options);
    rig.press(keys::ctrl('O'));
    rig.press(Event::End);
    rig.type("9");
    auto lines = rig.screen(120, 40);
    CHECK(screenHas(lines, "127.0.0.19"));
    rig.press(Event::Tab);   // port
    rig.press(Event::Tab);   // password
    rig.type("secret");
    lines = rig.screen(120, 40);
    INFO(dumpLines(lines));
    CHECK(screenHas(lines, "******"));
    CHECK_FALSE(screenHas(lines, "secret"));
    rig.press(Event::TabReverse);
    rig.type("ab5");   // the port takes digits only
    lines = rig.screen(120, 40);
    CHECK(screenHas(lines, "63725"));
    CHECK_FALSE(screenHas(lines, "ab5"));
    CHECK(rig.press(Event::Escape));
    CHECK(rig.s().modal() == Modal::None);
    CHECK(probe.count() == 0);
    lines = rig.screen(120, 40);
    CHECK_FALSE(screenHas(lines, "MeraDB server se connect karo"));
    // The next Ctrl+O starts from a fresh form.
    rig.press(keys::ctrl('O'));
    lines = rig.screen(120, 40);
    CHECK(screenHas(lines, "127.0.0.1 "));
    CHECK_FALSE(screenHas(lines, "127.0.0.19"));
}

TEST_CASE("wbui dialog Return connects and the header follows", "[wbui]") {
    FactoryProbe probe;
    std::promise<void> opened;
    probe.gate = opened.get_future().share();
    probe.useGate = true;
    SessionOptions options;
    options.factory = probe.factory();
    UiRig rig(serverDefaults, options);
    struct Release {
        std::promise<void>& p;
        bool done = false;
        void open() { if (!done) { done = true; p.set_value(); } }
        ~Release() { open(); }
    } release{opened};

    rig.press(keys::ctrl('O'));
    rig.press(Event::Tab);
    rig.press(Event::Tab);
    rig.type("pw");
    rig.press(Event::Tab);
    rig.type(" db ");
    CHECK(rig.press(Event::Return));
    CHECK(rig.s().modal() == Modal::None);
    REQUIRE(rig.poster.pumpUntil([&] { return probe.count() == 1; }));
    {
        std::lock_guard<std::mutex> l(probe.m);
        REQUIRE(probe.requests.size() == 1);
        const ConnectRequest& r = probe.requests[0];
        CHECK_FALSE(r.local);
        CHECK(r.host == "127.0.0.1");
        CHECK(r.port == "6372");
        REQUIRE(r.password.has_value());
        CHECK(*r.password == "pw");
        REQUIRE(r.database.has_value());
        CHECK(*r.database == "db");
    }
    CHECK(rig.screen(120, 40)[0].find("[chal raha hai]") != std::string::npos);
    release.open();
    rig.settle();
    auto lines = rig.screen(120, 40);
    CHECK(lines[0].find("127.0.0.1:6372  |  db: other") != std::string::npos);
    CHECK(rig.s().currentDb() == "other");
    CHECK(rig.logText().find("Connected: 127.0.0.1:6372") != std::string::npos);
}

TEST_CASE("wbui dialog Local mode button passes local", "[wbui]") {
    FactoryProbe probe;
    SessionOptions options;
    options.factory = probe.factory();
    UiRig rig(serverDefaults, options);
    rig.press(keys::ctrl('O'));
    for (int i = 0; i < 5; ++i) rig.press(Event::Tab);   // Host, Port, Password, Database, [Connect], [Local mode]
    auto lines = rig.screen(120, 40);
    CHECK(screenHas(lines, "[ Local mode ]"));
    CHECK(rig.press(Event::Return));
    rig.settle();
    REQUIRE(probe.count() == 1);
    CHECK(probe.requests[0].local);
    CHECK(rig.s().modal() == Modal::None);

    // [ Cancel ] closes without calling the factory.
    rig.press(keys::ctrl('O'));
    rig.press(Event::TabReverse);   // wraps to [Cancel]
    CHECK(rig.press(Event::Return));
    CHECK(rig.s().modal() == Modal::None);
    CHECK(probe.count() == 1);
}

TEST_CASE("wbui dialog a failing connect is logged and nothing changes", "[wbui]") {
    FactoryProbe probe;
    probe.fail = true;
    SessionOptions options;
    options.factory = probe.factory();
    UiRig rig(serverDefaults, options);
    rig.press(keys::ctrl('O'));
    rig.press(Event::Return);
    rig.settle();
    CHECK(rig.logText().find("Connect nahi hua: ") != std::string::npos);
    CHECK(rig.logText().find("nope") != std::string::npos);
    CHECK(rig.s().currentDb() == "main");
    CHECK(rig.screen(120, 40)[0].find("127.0.0.1:6372  |  db: main") != std::string::npos);
    rig.runStatement("DIKHAO 1;");   // the old backend still works
    CHECK(rig.fake->ranList().back() == "DIKHAO 1;");
}

TEST_CASE("wbui dialog F1 shows the help, which scrolls and closes three ways", "[wbui]") {
    UiRig rig;
    CHECK(rig.press(Event::F1));
    CHECK(rig.s().modal() == Modal::Help);
    auto lines = rig.screen(120, 40);
    INFO(dumpLines(lines));
    CHECK(screenHas(lines, "MeraDB Workbench: Madad"));
    CHECK(screenHas(lines, "Ctrl+Q"));
    CHECK(screenHas(lines, "Bahar niklo"));
    CHECK_FALSE(screenHas(lines, "Transactions"));
    bool found = false;
    for (int i = 0; i < 200 && !found; ++i) {
        rig.press(Event::PageDown);
        found = screenHas(rig.screen(120, 40), "Transactions");
    }
    CHECK(found);
    rig.press(Event::Home);
    CHECK(screenHas(rig.screen(120, 40), "MeraDB Workbench: Madad"));
    rig.press(Event::ArrowDown);
    rig.press(Event::ArrowUp);
    // Other keys do nothing while it is open.
    const std::size_t ran = rig.fake->ranList().size();
    rig.press(Event::F5);
    rig.press(Event::Character("x"));
    CHECK(rig.fake->ranList().size() == ran);
    CHECK(rig.s().editor().text().empty());

    CHECK(rig.press(Event::Escape));
    CHECK(rig.s().modal() == Modal::None);
    rig.press(Event::F1);
    CHECK(rig.s().modal() == Modal::Help);
    CHECK(screenHas(rig.screen(120, 40), "MeraDB Workbench: Madad"));   // back at the top
    rig.press(Event::F1);
    CHECK(rig.s().modal() == Modal::None);
    rig.press(Event::F1);
    rig.press(Event::Character("q"));
    CHECK(rig.s().modal() == Modal::None);
    CHECK(rig.s().editor().text().empty());
}

TEST_CASE("wbui dialog both dialogs render at small and large sizes", "[wbui]") {
    UiRig rig(serverDefaults);
    rig.press(keys::ctrl('O'));
    auto lines = rig.screen(60, 24);
    CHECK(screenHas(lines, "MeraDB server se connect karo"));
    CHECK(screenHas(lines, "[ Cancel ]"));
    CHECK_NOTHROW(rig.screen(200, 60));
    CHECK_NOTHROW(rig.screen(20, 8));
    CHECK_NOTHROW(rig.screen(1, 1));
    rig.press(Event::Escape);
    rig.press(Event::F1);
    lines = rig.screen(60, 24);
    CHECK(screenHas(lines, "Madad"));
    CHECK_NOTHROW(rig.screen(200, 60));
    CHECK_NOTHROW(rig.screen(20, 8));
    CHECK_NOTHROW(rig.screen(1, 1));
    CHECK_NOTHROW(rig.screen(300, 3));
}

TEST_CASE("wbui mouse focuses panels and scrolls the log", "[wbui]") {
    UiRig rig;
    for (int i = 0; i < 30; ++i) rig.s().logLine(LogKind::Plain, "line " + std::to_string(i));
    auto lines = rig.screen(120, 40);   // layout: log frame rows 20..29, results 1..19, editor 30..38
    CHECK(rig.ui->focus() == Panel::Editor);
    CHECK(screenHas(lines, "line 29"));

    CHECK(rig.press(mouseEvent(Mouse::Left, 50, 5)));
    CHECK(rig.ui->focus() == Panel::Results);
    CHECK(rig.press(mouseEvent(Mouse::Left, 50, 22)));
    CHECK(rig.ui->focus() == Panel::Log);
    CHECK(rig.press(mouseEvent(Mouse::Left, 10, 3)));   // the tree: row 1 (y = 1 frame, 2 first row)
    CHECK(rig.ui->focus() == Panel::Tree);
    CHECK(rig.s().tree().selected() == 1);
    CHECK(rig.press(mouseEvent(Mouse::Left, 50, 33)));
    CHECK(rig.ui->focus() == Panel::Editor);

    // The wheel scrolls the panel under the pointer without moving the focus.
    CHECK(rig.press(mouseEvent(Mouse::WheelUp, 50, 25)));
    lines = rig.screen(120, 40);
    CHECK_FALSE(screenHas(lines, "line 29"));
    CHECK(screenHas(lines, "line 26"));
    CHECK(rig.ui->focus() == Panel::Editor);
    CHECK(rig.press(mouseEvent(Mouse::WheelDown, 50, 25)));
    lines = rig.screen(120, 40);
    CHECK(screenHas(lines, "line 29"));

    // Outside every panel (header, footer) the mouse is ignored.
    CHECK_FALSE(rig.press(mouseEvent(Mouse::Left, 50, 0)));
    CHECK_FALSE(rig.press(mouseEvent(Mouse::Left, 50, 39)));
    CHECK(rig.ui->focus() == Panel::Editor);
}

TEST_CASE("wbui mouse is ignored while a dialog is open", "[wbui]") {
    UiRig rig;
    rig.screen(120, 40);
    rig.press(Event::F1);
    CHECK(rig.press(mouseEvent(Mouse::Left, 50, 5)));
    CHECK(rig.ui->focus() == Panel::Editor);
    rig.press(Event::Escape);
    rig.press(keys::ctrl('O'));
    CHECK(rig.press(mouseEvent(Mouse::Left, 50, 5)));
    CHECK(rig.ui->focus() == Panel::Editor);
    CHECK(rig.s().modal() == Modal::Connect);
}
