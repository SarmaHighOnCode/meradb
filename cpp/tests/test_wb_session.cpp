// cpp/tests/test_wb_session.cpp
#include "wb_test_util.h"
#include "test_util.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <regex>

using namespace meradb;
using namespace meradb::wb;
using wbtest::FakeBackend;
using wbtest::ManualPoster;
using wbtest::tableResult;

namespace {

nlohmann::ordered_json oneTableSchema() {
    return nlohmann::ordered_json::parse(R"([
      {"name":"main","current":true,"tables":[
         {"name":"t","columns":[
            {"name":"id","type_name":"INT","primary_key":true,"unique":false,"not_null":true},
            {"name":"naam","type_name":"TEXT","primary_key":false,"unique":false,"not_null":false}]}]},
      {"name":"college","current":false,"tables":[]}])");
}

struct Rig {
    FakeBackend* fake;
    ManualPoster poster;
    std::unique_ptr<Session> session;
    explicit Rig(const std::function<void(FakeBackend&)>& setup = {}, SessionOptions opts = {}) {
        auto owned = std::make_unique<FakeBackend>();
        fake = owned.get();
        if (setup) setup(*fake);
        session = std::make_unique<Session>(std::move(owned), std::move(opts), poster.poster());
        REQUIRE(poster.pumpIdle(*session));
    }
    Session& s() { return *session; }
};

std::string textOf(const LogEntry& e) {
    std::string out;
    for (std::size_t i = 0; i < e.lines.size(); ++i) {
        if (i) out += "\n";
        out += plainText(e.lines[i]);
    }
    return out;
}

// (kind, text) of every log entry after the two start-up lines.
std::vector<std::pair<LogKind, std::string>> logAfterStartup(const Session& s) {
    std::vector<std::pair<LogKind, std::string>> out;
    const auto& entries = s.log().entries();
    for (std::size_t i = 2; i < entries.size(); ++i) out.emplace_back(entries[i].kind, textOf(entries[i]));
    return out;
}

Result messageResult(const std::string& m) {
    Result r;
    r.message = m;
    return r;
}

Result errorResult(const std::string& e) {
    Result r;
    r.error = e;
    return r;
}

}  // namespace

TEST_CASE("wbsession start-up logs two lines, sets the header and loads the tree", "[wbsession]") {
    Rig rig([](FakeBackend& f) { f.tree = oneTableSchema(); });
    Session& s = rig.s();
    const auto& e = s.log().entries();
    REQUIRE(e.size() == 2);
    CHECK(e[0].kind == LogKind::Bold);
    CHECK(textOf(e[0]) == "Namaste! Connected: fake:1");
    CHECK(e[1].kind == LogKind::Dim);
    CHECK(textOf(e[1]) == "F1 dabao madad ke liye.");
    CHECK(s.subtitle() == "fake:1  |  db: main");
    CHECK(s.title() == "MeraDB Workbench");
    CHECK(s.tree().rows().size() > 1);
    CHECK(s.table() == nullptr);
}

TEST_CASE("wbsession a select shows the table and logs echo and timing", "[wbsession]") {
    Rig rig([](FakeBackend& f) {
        f.replies["DIKHAO * SE t;"] = {tableResult({"id", "naam"}, {{Value(int64_t(1)), Value(std::string("A"))}})};
    });
    Session& s = rig.s();
    s.runText("  DIKHAO * SE t;  ");
    REQUIRE(rig.poster.pumpIdle(s));
    CHECK(s.history().items() == std::vector<std::string>({"DIKHAO * SE t;"}));
    const auto log = logAfterStartup(s);
    REQUIRE(log.size() == 2);
    CHECK(log[0].first == LogKind::Echo);
    CHECK(log[0].second == "main> DIKHAO * SE t;");
    CHECK(log[1].first == LogKind::Dim);
    CHECK(std::regex_match(log[1].second, std::regex(R"(\(\d+\.\d ms\))")));
    REQUIRE(s.table() != nullptr);
    CHECK(s.table()->rows.size() == 1);
    CHECK(s.table()->columns == std::vector<std::string>({"id", "naam"}));
    CHECK(resultsTitle(s.table()) == "Results -- 1 row(s)");
    CHECK(s.resultVersion() == 1);
}

TEST_CASE("wbsession several results log in order and the last table wins", "[wbsession]") {
    Result t1 = tableResult({"a"}, {{Value(int64_t(1))}});
    Result t2 = tableResult({"b"}, {{Value(int64_t(2))}, {Value(int64_t(3))}});
    Result bad = errorResult("[Execution Galti] x");
    bad.columns = {"zzz"};  // an errored result never becomes the table
    Rig rig([&](FakeBackend& f) {
        f.replies["x;"] = {messageResult("a"), bad, t1, messageResult("b"), t2};
    });
    Session& s = rig.s();
    s.runText("x;");
    REQUIRE(rig.poster.pumpIdle(s));
    const auto log = logAfterStartup(s);
    REQUIRE(log.size() == 5);
    CHECK(log[0].first == LogKind::Echo);
    CHECK(log[1] == std::make_pair(LogKind::Message, std::string("a")));
    CHECK(log[2] == std::make_pair(LogKind::Error, std::string("[Execution Galti] x")));
    CHECK(log[3] == std::make_pair(LogKind::Message, std::string("b")));
    CHECK(log[4].first == LogKind::Dim);
    REQUIRE(s.table() != nullptr);
    CHECK(s.table()->columns == std::vector<std::string>({"b"}));
    CHECK(s.table()->rows.size() == 2);
}

TEST_CASE("wbsession empty text does nothing at all", "[wbsession]") {
    Rig rig;
    Session& s = rig.s();
    s.runText("   ");
    CHECK_FALSE(s.busy());
    CHECK(s.history().items().empty());
    CHECK(s.log().entries().size() == 2);
    CHECK(rig.fake->ranList().empty());
}

TEST_CASE("wbsession the same text twice is stored once in history", "[wbsession]") {
    Rig rig;
    Session& s = rig.s();
    s.runText("a;");
    s.runText("a;");
    REQUIRE(rig.poster.pumpIdle(s));
    CHECK(s.history().items() == std::vector<std::string>({"a;"}));
}

TEST_CASE("wbsession a dropped connection logs the error and a hint, nothing else", "[wbsession]") {
    Rig rig([](FakeBackend& f) {
        f.replies["DIKHAO * SE t;"] = {tableResult({"id"}, {{Value(int64_t(1))}})};
    });
    Session& s = rig.s();
    s.runText("DIKHAO * SE t;");
    REQUIRE(rig.poster.pumpIdle(s));
    const int version = s.resultVersion();
    const std::string subtitle = s.subtitle();
    const std::size_t before = s.log().entries().size();
    rig.fake->dropped = true;
    s.runText("x;");
    REQUIRE(rig.poster.pumpIdle(s));
    const auto& e = s.log().entries();
    REQUIRE(e.size() == before + 3);
    CHECK(e[before].kind == LogKind::Echo);
    CHECK(e[before + 1].kind == LogKind::Error);
    CHECK(textOf(e[before + 1]) == "[Connection Galti] Server se connection toot gaya: fake");
    CHECK(e[before + 2].kind == LogKind::Dim);
    CHECK(textOf(e[before + 2]) == "Ctrl+O se dobara connect karo.");
    CHECK(s.resultVersion() == version);
    CHECK(s.subtitle() == subtitle);
    CHECK(s.table()->columns == std::vector<std::string>({"id"}));
}

TEST_CASE("wbsession the transaction indicator follows the backend", "[wbsession]") {
    Rig rig;
    Session& s = rig.s();
    s.runText("SHURU;");
    REQUIRE(rig.poster.pumpIdle(s));
    CHECK(s.inTransaction());
    const std::string suffix = "  |  TRANSACTION (PAKKA / WAPAS)";
    CHECK(s.subtitle().size() > suffix.size());
    CHECK(s.subtitle().substr(s.subtitle().size() - suffix.size()) == suffix);
    s.runText("WAPAS;");
    REQUIRE(rig.poster.pumpIdle(s));
    CHECK_FALSE(s.inTransaction());
    CHECK(s.subtitle() == "fake:1  |  db: main");
}

TEST_CASE("wbsession a failing schema refresh is logged and the tree is left alone", "[wbsession]") {
    Rig rig([](FakeBackend& f) { f.tree = oneTableSchema(); });
    Session& s = rig.s();
    const std::size_t rows = s.tree().rows().size();
    rig.fake->schemaFails = true;
    s.runText("x;");
    REQUIRE(rig.poster.pumpIdle(s));
    const auto log = logAfterStartup(s);
    REQUIRE(log.size() == 4);
    CHECK(log[3] == std::make_pair(LogKind::Dim,
                                   std::string("(schema refresh nahi hua: [Storage Galti] schema nahi mila)")));
    CHECK(s.tree().rows().size() == rows);
}

TEST_CASE("wbsession every database call runs on one worker thread", "[wbsession]") {
    Rig rig;
    Session& s = rig.s();
    s.runText("a;");
    s.runText("b;");
    s.runText("c;");
    REQUIRE(rig.poster.pumpIdle(s));
    const auto ids = rig.fake->threadList();
    REQUIRE(ids.size() == 3);
    CHECK(ids[0] == ids[1]);
    CHECK(ids[1] == ids[2]);
    CHECK(ids[0] != std::this_thread::get_id());
}

TEST_CASE("wbsession busy label counts queued statements and order is kept", "[wbsession]") {
    std::promise<void> release;
    std::shared_future<void> gate = release.get_future().share();
    std::promise<void> entered;
    std::atomic<bool> first{true};
    Rig rig([&](FakeBackend& f) {
        f.beforeRun = [&](const std::string&) {
            if (first.exchange(false)) {
                entered.set_value();
                gate.wait();
            }
        };
    });
    Session& s = rig.s();
    s.runText("a;");
    s.runText("b;");
    CHECK(s.busy());
    CHECK(s.pendingJobs() == 2);
    CHECK(s.busyLabel() == "[chal raha hai +1]");
    const auto log = logAfterStartup(s);
    REQUIRE(log.size() == 2);  // both echo lines are in already
    CHECK(log[0].second == "main> a;");
    CHECK(log[1].second == "main> b;");
    entered.get_future().wait();
    release.set_value();
    REQUIRE(rig.poster.pumpIdle(s));
    CHECK(rig.fake->ranList() == std::vector<std::string>({"a;", "b;"}));
    CHECK(s.busyLabel() == "");
}

TEST_CASE("wbsession F5 runs the selection if there is one, else everything", "[wbsession]") {
    Rig rig;
    Session& s = rig.s();
    s.editor().setText("one;\ntwo;");
    s.editor().selectRange({0, 0}, {0, 4});
    s.runEditorText();
    REQUIRE(rig.poster.pumpIdle(s));
    s.editor().setText("one;\ntwo;");
    s.runEditorText();
    REQUIRE(rig.poster.pumpIdle(s));
    CHECK(rig.fake->ranList() == std::vector<std::string>({"one;", "one;\ntwo;"}));
}

TEST_CASE("wbsession clearLog empties the log", "[wbsession]") {
    Rig rig;
    Session& s = rig.s();
    REQUIRE_FALSE(s.log().entries().empty());
    s.clearLog();
    CHECK(s.log().entries().empty());
}

// ---------------------------------------------------------------------------------------------------------------
// Task 7: explain, history, export, connect, tree activation, quit and shutdown

TEST_CASE("wbsession explain runs SAMJHAO for one statement", "[wbsession]") {
    Rig rig;
    Session& s = rig.s();
    s.editor().setText("DIKHAO * SE t;");
    s.explainEditorText();
    REQUIRE(rig.poster.pumpIdle(s));
    CHECK(rig.fake->ranList() == std::vector<std::string>({"SAMJHAO DIKHAO * SE t;"}));
    const auto log = logAfterStartup(s);
    REQUIRE_FALSE(log.empty());
    CHECK(log[0] == std::make_pair(LogKind::Echo, std::string("main> SAMJHAO DIKHAO * SE t;")));
    CHECK(s.history().items().back() == "SAMJHAO DIKHAO * SE t;");
}

TEST_CASE("wbsession explain refuses two statements", "[wbsession]") {
    Rig rig;
    Session& s = rig.s();
    s.editor().setText("DIKHAO * SE a; DIKHAO * SE b;");
    s.explainEditorText();
    REQUIRE(rig.poster.pumpIdle(s));
    const auto log = logAfterStartup(s);
    REQUIRE(log.size() == 1);
    CHECK(log[0] == std::make_pair(LogKind::Warn, std::string("SAMJHAO ek hi query par chalta hai -- ek query select karke F6 dabao")));
    CHECK(rig.fake->ranList().empty());
    CHECK(s.history().items().empty());
}

TEST_CASE("wbsession explain: empty text, comment only and bad syntax", "[wbsession]") {
    Rig rig;
    Session& s = rig.s();
    const auto warn = std::make_pair(LogKind::Warn, std::string("SAMJHAO ek hi query par chalta hai -- ek query select karke F6 dabao"));
    s.editor().setText("");
    s.explainEditorText();
    REQUIRE(rig.poster.pumpIdle(s));
    s.editor().setText("-- sirf comment");
    s.explainEditorText();
    REQUIRE(rig.poster.pumpIdle(s));
    auto log = logAfterStartup(s);
    REQUIRE(log.size() == 2);
    CHECK(log[0] == warn);
    CHECK(log[1] == warn);
    s.editor().setText("DIKHAO FROM;");
    s.explainEditorText();
    REQUIRE(rig.poster.pumpIdle(s));
    log = logAfterStartup(s);
    REQUIRE(log.size() == 3);
    CHECK(log[2].first == LogKind::Error);
    CHECK(log[2].second.rfind("[", 0) == 0);
    CHECK(rig.fake->ranList().empty());
    CHECK(s.history().items().empty());
}

TEST_CASE("wbsession explain is selection-aware", "[wbsession]") {
    Rig rig;
    Session& s = rig.s();
    s.editor().setText("DIKHAO * SE a; DIKHAO * SE b;");
    s.editor().selectRange({0, 0}, {0, 14});
    s.explainEditorText();
    REQUIRE(rig.poster.pumpIdle(s));
    CHECK(rig.fake->ranList() == std::vector<std::string>({"SAMJHAO DIKHAO * SE a;"}));
}

TEST_CASE("wbsession history steps put text in the editor and ask for its focus", "[wbsession]") {
    Rig rig;
    Session& s = rig.s();
    s.historyStep(-1);
    CHECK_FALSE(s.takeFocusRequest().has_value());
    CHECK(s.editor().text() == "");
    s.runText("a;");
    s.runText("b;");
    REQUIRE(rig.poster.pumpIdle(s));
    s.historyStep(-1);
    CHECK(s.editor().text() == "b;");
    CHECK(s.takeFocusRequest() == std::optional<Panel>(Panel::Editor));
    CHECK_FALSE(s.takeFocusRequest().has_value());
    s.historyStep(-1);
    CHECK(s.editor().text() == "a;");
    s.historyStep(1);
    CHECK(s.editor().text() == "b;");
    s.historyStep(1);
    CHECK(s.editor().text() == "");
}

TEST_CASE("wbsession export writes the CSV and reports it", "[wbsession]") {
    meradb_test::TempDir tmp;
    SessionOptions opts;
    opts.exportBaseDir = tmp.str();
    opts.stamp = [] { return std::string("20260101-000000"); };
    Rig rig(
        [](FakeBackend& f) {
            f.replies["DIKHAO * SE t;"] = {tableResult(
                {"id", "naam"}, {{Value(int64_t(1)), Value(std::string("A"))}, {Value(int64_t(2)), Value()}})};
        },
        opts);
    Session& s = rig.s();
    s.exportCsv();
    {
        const auto log = logAfterStartup(s);
        REQUIRE(log.size() == 1);
        CHECK(log[0] == std::make_pair(LogKind::Warn, std::string("Pehle koi DIKHAO query chalao, phir Ctrl+S")));
        CHECK_FALSE(std::filesystem::exists(tmp.path() / "exports"));
    }
    s.runText("DIKHAO * SE t;");
    REQUIRE(rig.poster.pumpIdle(s));
    s.exportCsv();
    const std::filesystem::path file = tmp.path() / "exports" / "meradb-20260101-000000.csv";
    REQUIRE(std::filesystem::exists(file));
    std::ifstream in(file, std::ios::binary);
    std::stringstream bytes;
    bytes << in.rdbuf();
    CHECK(bytes.str() == "id,naam\r\n1,A\r\n2,\r\n");
    const auto& last = s.log().entries().back();
    CHECK(last.kind == LogKind::Message);
    CHECK(textOf(last) == "2 row(s) CSV mein save: " + file.string());
}

TEST_CASE("wbsession export failure is logged in red", "[wbsession]") {
    meradb_test::TempDir tmp;
    { std::ofstream(tmp.path() / "exports") << "not a folder"; }
    SessionOptions opts;
    opts.exportBaseDir = tmp.str();
    opts.stamp = [] { return std::string("s"); };
    Rig rig(
        [](FakeBackend& f) { f.replies["q;"] = {tableResult({"a"}, {{Value(int64_t(1))}})}; }, opts);
    Session& s = rig.s();
    s.runText("q;");
    REQUIRE(rig.poster.pumpIdle(s));
    s.exportCsv();
    const auto& last = s.log().entries().back();
    CHECK(last.kind == LogKind::Error);
    CHECK(textOf(last).rfind("CSV save nahi hua: ", 0) == 0);
}

TEST_CASE("wbsession connect dialog helpers", "[wbsession]") {
    const ConnectRequest a = makeConnectRequest(false, " h ", "", "", " db ");
    CHECK_FALSE(a.local);
    CHECK(a.host == "h");
    CHECK(a.port == "6372");
    CHECK_FALSE(a.password.has_value());
    CHECK(a.database == std::optional<std::string>("db"));
    const ConnectRequest b = makeConnectRequest(true, "", "", "pw", "");
    CHECK(b.local);
    CHECK(b.host == "127.0.0.1");
    CHECK(b.password == std::optional<std::string>("pw"));
    CHECK_FALSE(b.database.has_value());

    auto d = connectDefaults("10.0.0.5:6400");
    CHECK(d.host == "10.0.0.5");
    CHECK(d.port == "6400");
    d = connectDefaults("local (C:\\data)");
    CHECK(d.host == "127.0.0.1");
    CHECK(d.port == "6372");
    d = connectDefaults("::1:6372");
    CHECK(d.host == "::1");
    CHECK(d.port == "6372");
    d = connectDefaults("nonsense");
    CHECK(d.port == "6372");

    ConnectRequest bad = makeConnectRequest(false, "127.0.0.1", "abc", "", "");
    try {
        defaultBackendFactory(bad, "");
        FAIL("expected invalid_argument");
    } catch (const std::invalid_argument& e) {
        CHECK(std::string(e.what()) == "invalid literal for int() with base 10: 'abc'");
    }

    Rig rig;
    CHECK(rig.s().connectDefaults().host == "fake");
    CHECK(rig.s().connectDefaults().port == "1");
    rig.s().openConnectDialog();
    CHECK(rig.s().modal() == Modal::Connect);
    rig.s().closeModal();
    CHECK(rig.s().modal() == Modal::None);
    rig.s().showHelp();
    CHECK(rig.s().modal() == Modal::Help);
}

TEST_CASE("wbsession connect success swaps the backend", "[wbsession]") {
    FakeBackend* second = nullptr;
    SessionOptions opts;
    opts.factory = [&second](const ConnectRequest&, const std::string&) {
        auto b = std::make_unique<FakeBackend>();
        b->desc = "other:2";
        b->db = "college";
        b->tree = nlohmann::ordered_json::parse(R"([{"name":"college","current":true,"tables":[]}])");
        second = b.get();
        return std::unique_ptr<Backend>(std::move(b));
    };
    Rig rig([](FakeBackend& f) { f.tree = oneTableSchema(); }, opts);
    Session& s = rig.s();
    s.runText("SHURU;");
    REQUIRE(rig.poster.pumpIdle(s));
    auto oldSeen = rig.fake->seen;
    s.openConnectDialog();
    s.connect(makeConnectRequest(false, "other", "2", "", ""));
    CHECK(s.modal() == Modal::None);
    REQUIRE(rig.poster.pumpIdle(s));
    REQUIRE(second != nullptr);
    CHECK(s.subtitle() == "other:2  |  db: college");
    const auto& last = s.log().entries().back();
    CHECK(last.kind == LogKind::Connected);
    CHECK(textOf(last) == "Connected: other:2");
    CHECK(oldSeen->closed);
    CHECK(oldSeen->rolledBackOnClose);
    bool sawCollege = false;
    for (const auto& r : s.tree().rows()) sawCollege = sawCollege || plainText(r.label) == "college";
    CHECK(sawCollege);
    s.runText("x;");
    s.runText("y;");
    REQUIRE(rig.poster.pumpIdle(s));
    CHECK(second->ranList() == std::vector<std::string>({"x;", "y;"}));
    const auto ids = second->threadList();
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == ids[1]);
}

TEST_CASE("wbsession connect failure changes nothing", "[wbsession]") {
    SessionOptions opts;
    opts.factory = [](const ConnectRequest&, const std::string&) -> std::unique_ptr<Backend> {
        throw ConnectionFailed("kaun hai");
    };
    Rig rig({}, opts);
    Session& s = rig.s();
    s.connect(makeConnectRequest(false, "x", "1", "", ""));
    REQUIRE(rig.poster.pumpIdle(s));
    const auto& last = s.log().entries().back();
    CHECK(last.kind == LogKind::Error);
    CHECK(textOf(last) == "Connect nahi hua: [Connection Galti] kaun hai");
    CHECK_FALSE(rig.fake->closed());
    CHECK(s.subtitle() == "fake:1  |  db: main");
    s.runText("z;");
    REQUIRE(rig.poster.pumpIdle(s));
    CHECK(rig.fake->ranList() == std::vector<std::string>({"z;"}));
}

TEST_CASE("wbsession tree activation runs scripts or inserts the column", "[wbsession]") {
    Rig rig([](FakeBackend& f) { f.tree = oneTableSchema(); });
    Session& s = rig.s();
    auto rowOf = [&s](const std::string& label) {
        const auto& rows = s.tree().rows();
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].key.kind != NodeKey::Kind::Root && plainText(rows[i].label).rfind(label, 0) == 0)
                return static_cast<int>(i);
        return -1;
    };
    const int tableRow = rowOf("t");
    REQUIRE(tableRow >= 0);
    s.activateTreeRow(tableRow);  // also toggles the table open
    REQUIRE(rig.poster.pumpIdle(s));
    CHECK(rig.fake->ranList() == std::vector<std::string>({"DIKHAO * SE t SIRF 100;"}));

    const int columnRow = rowOf("naam");
    REQUIRE(columnRow >= 0);
    s.editor().setText("x ");
    s.activateTreeRow(columnRow);
    CHECK(s.editor().text() == "x naam");
    CHECK(s.takeFocusRequest() == std::optional<Panel>(Panel::Editor));

    s.activateTreeRow(rowOf("college"));
    REQUIRE(rig.poster.pumpIdle(s));
    CHECK(rig.fake->ranList().back() == "ISTEMAL college;");
}

TEST_CASE("wbsession quit when idle closes the backend and calls onExit once", "[wbsession]") {
    Rig rig;
    Session& s = rig.s();
    s.runText("SHURU;");
    REQUIRE(rig.poster.pumpIdle(s));
    auto seen = rig.fake->seen;
    int exits = 0;
    s.setOnExit([&exits] { ++exits; });
    s.requestQuit();
    CHECK(s.quitting());
    CHECK(s.busyLabel() == "[band ho raha hai ...]");
    s.requestQuit();  // does nothing
    REQUIRE(rig.poster.pumpUntil([&exits] { return exits > 0; }));
    CHECK(exits == 1);
    CHECK(seen->closed);
    CHECK(seen->rolledBackOnClose);
    CHECK(seen->closeCalls == 1);
    const std::size_t entries = s.log().entries().size();
    s.runText("late;");
    CHECK(s.log().entries().size() == entries);
}

TEST_CASE("wbsession quit while busy drops queued jobs and waits for the running one", "[wbsession]") {
    std::promise<void> release, entered;
    std::shared_future<void> gate = release.get_future().share();
    std::atomic<bool> first{true};
    Rig rig([&](FakeBackend& f) {
        f.beforeRun = [&](const std::string&) {
            if (first.exchange(false)) {
                entered.set_value();
                gate.wait();
            }
        };
    });
    Session& s = rig.s();
    auto seen = rig.fake->seen;
    int exits = 0;
    s.setOnExit([&exits] { ++exits; });
    s.runText("first;");
    s.runText("second;");
    entered.get_future().wait();
    s.requestQuit();
    CHECK(exits == 0);
    release.set_value();
    REQUIRE(rig.poster.pumpUntil([&exits] { return exits > 0; }));
    CHECK(exits == 1);
    {
        std::lock_guard<std::mutex> l(seen->m);
        CHECK(seen->ran == std::vector<std::string>({"first;"}));
        CHECK(seen->closed);
    }
}

TEST_CASE("wbsession shutdown closes the backend even if the UI never pumps", "[wbsession]") {
    std::shared_ptr<FakeBackend::Shared> seen;
    {
        ManualPoster poster;
        auto owned = std::make_unique<FakeBackend>();
        seen = owned->seen;
        Session s(std::move(owned), SessionOptions{}, poster.poster());
        s.shutdown();
        s.shutdown();  // harmless
        CHECK(s.quitting());
    }
    CHECK(seen->closed);
    CHECK(seen->closeCalls == 1);
}

TEST_CASE("wbsession destroying the session while a statement runs waits for it", "[wbsession]") {
    std::shared_ptr<FakeBackend::Shared> seen;
    std::promise<void> release, entered;
    std::shared_future<void> gate = release.get_future().share();
    std::atomic<bool> first{true};
    {
        ManualPoster poster;
        auto owned = std::make_unique<FakeBackend>();
        seen = owned->seen;
        owned->beforeRun = [&](const std::string&) {
            if (first.exchange(false)) {
                entered.set_value();
                gate.wait();
            }
        };
        Session s(std::move(owned), SessionOptions{}, poster.poster());
        s.runText("slow;");
        entered.get_future().wait();
        std::thread releaser([&release] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            release.set_value();
        });
        s.shutdown();
        releaser.join();
    }
    CHECK(seen->closed);
}

TEST_CASE("wbsession closures delivered after the session is destroyed are no-ops", "[wbsession]") {
    ManualPoster poster;  // outlives the session, like the UI loop
    int exits = 0;
    {
        auto owned = std::make_unique<FakeBackend>();
        Session s(std::move(owned), SessionOptions{}, poster.poster());
        s.setOnExit([&exits] { ++exits; });
        s.runText("a;");
        s.runText("b;");
        s.explainEditorText();
        s.requestQuit();
        // nothing is pumped: the start-up snapshot, both statements and the exit closure are all still queued
    }
    // The session is gone; its queued closures run now (the pre-token code dereferenced freed memory here).
    REQUIRE(poster.pumpUntil([] { return false; }, std::chrono::milliseconds(100)) == false);
    CHECK(exits == 0);
}

TEST_CASE("wbsession closures delivered after shutdown change nothing", "[wbsession]") {
    ManualPoster poster;
    auto owned = std::make_unique<FakeBackend>();
    Session s(std::move(owned), SessionOptions{}, poster.poster());
    int exits = 0;
    s.setOnExit([&exits] { ++exits; });
    s.runText("a;");
    s.requestQuit();
    s.shutdown();
    const std::size_t entries = s.log().entries().size();
    const int pending = s.pendingJobs();
    poster.pumpUntil([] { return false; }, std::chrono::milliseconds(100));
    CHECK(exits == 0);
    CHECK(s.log().entries().size() == entries);
    CHECK(s.pendingJobs() == pending);
}

TEST_CASE("wbsession requestQuit during a slow statement: onExit after it ends, destructor then prompt", "[wbsession]") {
    std::promise<void> release, entered;
    std::shared_future<void> gate = release.get_future().share();
    std::atomic<bool> first{true};
    ManualPoster poster;
    auto owned = std::make_unique<FakeBackend>();
    auto seen = owned->seen;
    owned->beforeRun = [&](const std::string&) {
        if (first.exchange(false)) {
            entered.set_value();
            gate.wait();
        }
    };
    auto session = std::make_unique<Session>(std::move(owned), SessionOptions{}, poster.poster());
    int exits = 0;
    session->setOnExit([&exits] { ++exits; });
    session->runText("slow;");
    entered.get_future().wait();
    session->requestQuit();  // returns at once even though the statement is still running
    CHECK(session->quitting());
    CHECK_FALSE(poster.pumpUntil([&exits] { return exits > 0; }, std::chrono::milliseconds(150)));
    CHECK(exits == 0);
    release.set_value();  // the statement finishes
    REQUIRE(poster.pumpUntil([&exits] { return exits > 0; }));
    CHECK(exits == 1);
    CHECK(seen->closed);
    const auto before = std::chrono::steady_clock::now();
    session.reset();  // everything is already closed: nothing left to wait for
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(2));
    CHECK(seen->closeCalls == 1);
}
