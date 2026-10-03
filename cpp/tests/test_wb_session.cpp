// cpp/tests/test_wb_session.cpp
#include "wb_test_util.h"
#include "test_util.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <future>
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
