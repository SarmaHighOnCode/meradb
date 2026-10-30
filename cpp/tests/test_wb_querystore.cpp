// cpp/tests/test_wb_querystore.cpp -- the saved queries (QueryStore and Session::savedQueries)
#include "wb_test_util.h"
#include "test_util.h"
#include <catch2/catch_test_macros.hpp>
#include "meradb/wb_querystore.h"
#include "meradb/wb_session.h"
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace meradb::wb;
using wbtest::FakeBackend;
using wbtest::ManualPoster;
namespace fs = std::filesystem;

namespace {
struct TempDir {
    fs::path path;
    TempDir() {
        path = fs::temp_directory_path() / ("meradb_qs_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};
}  // namespace

TEST_CASE("QueryStore keeps queries per database, oldest first", "[wb][querystore]") {
    TempDir dir;
    QueryStore store(dir.path.string());
    REQUIRE(store.load("palantir").empty());
    store.append("palantir", {"DIKHAO employees;", "2026-10-29 10:00:00"});
    store.append("palantir", {"line1\nline2 \"quoted\" \xE0\xA4\xA8", "2026-10-29 10:01:00"});
    store.append("main", {"DIKHAO t;", ""});
    const auto p = store.load("palantir");
    REQUIRE(p.size() == 2);
    CHECK(p[0].text == "DIKHAO employees;");
    CHECK(p[1].text == "line1\nline2 \"quoted\" \xE0\xA4\xA8");
    CHECK(p[1].when == "2026-10-29 10:01:00");
    CHECK(store.load("main").size() == 1);
    store.clear("palantir");
    CHECK(store.load("palantir").empty());
    CHECK(store.load("main").size() == 1);
}

TEST_CASE("QueryStore skips damaged lines and never throws", "[wb][querystore]") {
    TempDir dir;
    QueryStore store(dir.path.string());
    store.append("d", {"ek", "t"});
    { std::ofstream(store.fileFor("d"), std::ios::app) << "not json\n{\"x\":1}\n\n"; }
    store.append("d", {"do", "t"});
    CHECK(store.load("d").size() == 2);
    CHECK(QueryStore("").load("d").empty());
    QueryStore("").append("d", {"x", ""});   // disabled: no-op
    CHECK(QueryStore::nowStamp().size() == 19);
}

TEST_CASE("A query holding a password is not kept on disk", "[wb][querystore]") {
    TempDir dir;
    QueryStore store(dir.path.string());
    store.append("d", {"BANAO USER ravi GUPT 'hunter2';", "t"});
    store.append("d", {"banao user ravi gupt\n'hunter2';", "t"});
    store.append("d", {"CREATE USER ravi IDENTIFIED BY 'hunter2';", "t"});
    store.append("d", {"DIKHAO user SE logins;", "t"});   // a column called user holds no password
    const auto kept = store.load("d");
    REQUIRE(kept.size() == 1);
    CHECK(kept[0].text == "DIKHAO user SE logins;");
    std::ifstream in(store.fileFor("d"), std::ios::binary);
    const std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(all.find("hunter2") == std::string::npos);
}
TEST_CASE("Session saves every run per database and reloads them", "[wb][querystore]") {
    TempDir dir;
    auto make = [&](ManualPoster& poster, FakeBackend*& fake) {
        auto owned = std::make_unique<FakeBackend>();
        fake = owned.get();
        fake->desc = "local (" + dir.path.string() + ")";
        fake->db = "palantir";
        SessionOptions opts;
        opts.dataDir = dir.path.string();
        return std::make_unique<Session>(std::move(owned), std::move(opts), poster.poster());
    };
    {
        ManualPoster poster;
        FakeBackend* fake = nullptr;
        auto s = make(poster, fake);
        REQUIRE(poster.pumpIdle(*s));
        CHECK(s->savedQueriesEnabled());
        CHECK(s->savedQueries().empty());
        s->runText("DIKHAO employees;");
        REQUIRE(poster.pumpIdle(*s));
        REQUIRE(s->savedQueries().size() == 1);
        CHECK(s->savedQueries()[0].text == "DIKHAO employees;");
        CHECK(s->savedQueries()[0].when.size() == 19);
    }
    {   // a new start: the list is still there
        ManualPoster poster;
        FakeBackend* fake = nullptr;
        auto s = make(poster, fake);
        REQUIRE(poster.pumpIdle(*s));
        REQUIRE(s->savedQueries().size() == 1);
        s->clearSavedQueries();
        CHECK(s->savedQueries().empty());
    }
    CHECK(QueryStore(dir.path.string()).load("palantir").empty());
}

TEST_CASE("Session without a local data folder keeps no saved queries", "[wb][querystore]") {
    ManualPoster poster;
    auto owned = std::make_unique<FakeBackend>();   // description is not "local (...)"
    Session s(std::move(owned), SessionOptions{}, poster.poster());
    REQUIRE(poster.pumpIdle(s));
    s.runText("DIKHAO t;");
    REQUIRE(poster.pumpIdle(s));
    CHECK_FALSE(s.savedQueriesEnabled());
    CHECK(s.savedQueries().empty());
}

TEST_CASE("Session keeps the writing area per database and loads it again", "[wb][querystore]") {
    TempDir dir;
    auto make = [&](ManualPoster& poster) {
        auto owned = std::make_unique<FakeBackend>();
        owned->desc = "local (" + dir.path.string() + ")";
        owned->db = "main";
        SessionOptions opts;
        opts.dataDir = dir.path.string();
        return std::make_unique<Session>(std::move(owned), std::move(opts), poster.poster());
    };
    {
        ManualPoster poster;
        auto s = make(poster);
        REQUIRE(poster.pumpIdle(*s));
        CHECK(s->editor().text().empty());
        s->editor().setText("DIKHAO * SE t;\n-- baad mein");
        s->persistEditor(true);
    }
    {   // a new start brings the text back, without anything having been run
        ManualPoster poster;
        auto s = make(poster);
        REQUIRE(poster.pumpIdle(*s));
        CHECK(s->editor().text() == "DIKHAO * SE t;\n-- baad mein");
        CHECK(s->savedQueries().empty());
    }
}

TEST_CASE("Pichli and Agli walk the saved queries and never lose the writing area", "[wb][querystore]") {
    TempDir dir;
    QueryStore(dir.path.string()).append("main", {"DIKHAO * SE a;", ""});
    QueryStore(dir.path.string()).append("main", {"DIKHAO * SE b;", ""});
    ManualPoster poster;
    auto owned = std::make_unique<FakeBackend>();
    owned->desc = "local (" + dir.path.string() + ")";
    SessionOptions opts;
    opts.dataDir = dir.path.string();
    Session s(std::move(owned), std::move(opts), poster.poster());
    REQUIRE(poster.pumpIdle(s));
    s.editor().setText("-- mera lamba script\nDIKHAO 1;");
    s.historyStep(-1);
    CHECK(s.editor().text() == "DIKHAO * SE b;");
    s.historyStep(-1);
    CHECK(s.editor().text() == "DIKHAO * SE a;");
    s.historyStep(1);
    s.historyStep(1);   // past the newest entry: the script is back, not an empty area
    CHECK(s.editor().text() == "-- mera lamba script\nDIKHAO 1;");
}

TEST_CASE("Opening a database from the cards adds no query to the list", "[wb][querystore]") {
    TempDir dir;
    ManualPoster poster;
    auto owned = std::make_unique<FakeBackend>();
    owned->desc = "local (" + dir.path.string() + ")";
    SessionOptions opts;
    opts.dataDir = dir.path.string();
    Session s(std::move(owned), std::move(opts), poster.poster());
    REQUIRE(poster.pumpIdle(s));
    s.openDatabase("college");
    REQUIRE(poster.pumpIdle(s));
    CHECK(s.savedQueries().empty());
    CHECK(s.history().size() == 0);
}

TEST_CASE("A database switch is not saved as a query, in any letter case", "[wb][querystore]") {
    TempDir dir;
    ManualPoster poster;
    auto owned = std::make_unique<FakeBackend>();
    owned->desc = "local (" + dir.path.string() + ")";
    SessionOptions opts;
    opts.dataDir = dir.path.string();
    Session s(std::move(owned), std::move(opts), poster.poster());
    REQUIRE(poster.pumpIdle(s));
    s.runText("istemal main;");
    s.runText("ISTEMAL main;");
    s.runText("DIKHAO * SE t;");
    REQUIRE(poster.pumpIdle(s));
    REQUIRE(s.savedQueries().size() == 1);
    CHECK(s.savedQueries()[0].text == "DIKHAO * SE t;");
}

TEST_CASE("The writing area is saved before a card swaps it, even inside the save delay", "[wb][querystore]") {
    TempDir dir;
    QueryStore(dir.path.string()).saveScript("college", "-- college ka script");
    ManualPoster poster;
    auto owned = std::make_unique<FakeBackend>();
    FakeBackend* fake = owned.get();
    fake->beforeRun = [fake](const std::string& text) {   // on the worker, like a real ISTEMAL
        if (text == "ISTEMAL college;") fake->db = "college";
    };
    owned->desc = "local (" + dir.path.string() + ")";
    SessionOptions opts;
    opts.dataDir = dir.path.string();
    Session s(std::move(owned), std::move(opts), poster.poster());
    REQUIRE(poster.pumpIdle(s));
    s.editor().setText("-- main ka script");
    s.persistEditor();        // inside the delay right after start? either way the swap below must keep it
    s.editor().setText("-- main ka script, aakhri badlav");
    s.openDatabase("college");
    REQUIRE(poster.pumpIdle(s));
    CHECK(s.editor().text() == "-- college ka script");
    CHECK(QueryStore(dir.path.string()).loadScript("main") == "-- main ka script, aakhri badlav");
}

TEST_CASE("Looking at an old query with Pichli never overwrites the saved script", "[wb][querystore]") {
    TempDir dir;
    QueryStore(dir.path.string()).append("main", {"DIKHAO * SE a;", ""});
    QueryStore(dir.path.string()).saveScript("main", "-- mera poora script");
    ManualPoster poster;
    auto owned = std::make_unique<FakeBackend>();
    owned->desc = "local (" + dir.path.string() + ")";
    SessionOptions opts;
    opts.dataDir = dir.path.string();
    {
        Session s(std::move(owned), std::move(opts), poster.poster());
        REQUIRE(poster.pumpIdle(s));
        CHECK(s.editor().text() == "-- mera poora script");
        s.historyStep(-1);
        CHECK(s.editor().text() == "DIKHAO * SE a;");
        s.persistEditor(true);   // what a quit or the autosave does while the old query is on screen
        CHECK(QueryStore(dir.path.string()).loadScript("main") == "-- mera poora script");
        s.editor().setText("DIKHAO * SE a; -- badla");   // an edit of it is real work: saved
        s.persistEditor(true);
        CHECK(QueryStore(dir.path.string()).loadScript("main") == "DIKHAO * SE a; -- badla");
    }
}

// ---- Pichli / Agli as the user meets them ----

namespace {
// A local session on a fake backend: "ISTEMAL x;" changes the database on the worker, like the real one.
struct WalkRig {
    TempDir dir;
    ManualPoster poster;
    FakeBackend* fake = nullptr;
    std::unique_ptr<Session> session;
    explicit WalkRig(const std::vector<std::string>& savedInMain = {}, const std::string& script = "") {
        QueryStore store(dir.path.string());
        for (const std::string& q : savedInMain) store.append("main", {q, ""});
        if (!script.empty()) store.saveScript("main", script);
        auto owned = std::make_unique<FakeBackend>();
        fake = owned.get();
        fake->desc = "local (" + dir.path.string() + ")";
        FakeBackend* f = fake;
        fake->beforeRun = [f](const std::string& text) {
            const std::string prefix = "ISTEMAL ";
            if (text.rfind(prefix, 0) == 0 && text.find(';') == text.size() - 1) f->db = text.substr(prefix.size(), text.size() - prefix.size() - 1);
        };
        SessionOptions opts;
        opts.dataDir = dir.path.string();
        session = std::make_unique<Session>(std::move(owned), std::move(opts), poster.poster());
        REQUIRE(poster.pumpIdle(*session));
    }
    Session& s() { return *session; }
    void run(const std::string& text) {
        s().runText(text);
        REQUIRE(poster.pumpIdle(s()));
    }
    std::string text() { return s().editor().text(); }
    std::string savedScript(const std::string& db = "main") { return QueryStore(dir.path.string()).loadScript(db); }
};
}  // namespace

TEST_CASE("Pichli after running the editor's own text shows the query before it, the first press already", "[wb][querystore][walk]") {
    WalkRig rig;
    rig.run("a;");
    rig.s().editor().setText("b;");
    rig.s().runEditorText();
    REQUIRE(rig.poster.pumpIdle(rig.s()));
    rig.s().historyStep(-1);
    CHECK(rig.text() == "a;");   // not "b;" again: that press would change nothing on screen
}

TEST_CASE("Pichli compares without the blanks a run strips off", "[wb][querystore][walk]") {
    WalkRig rig;
    rig.run("a;");
    rig.s().editor().setText("b;\n");
    rig.s().runEditorText();
    REQUIRE(rig.poster.pumpIdle(rig.s()));
    rig.s().historyStep(-1);
    CHECK(rig.text() == "a;");
}

TEST_CASE("Agli goes back down the list and then brings the draft back", "[wb][querystore][walk]") {
    WalkRig rig({"a;", "b;"});
    rig.s().editor().setText("mera script");
    rig.s().historyStep(-1);
    rig.s().historyStep(-1);
    REQUIRE(rig.text() == "a;");
    rig.s().historyStep(1);
    CHECK(rig.text() == "b;");
    rig.s().historyStep(1);
    CHECK(rig.text() == "mera script");
}

TEST_CASE("The walk tells where it is, so a press at the end of the list is not a dead button", "[wb][querystore][walk]") {
    WalkRig rig({"a;", "b;"});
    CHECK(rig.s().historyLabel().empty());
    rig.s().historyStep(-1);
    CHECK(rig.s().historyLabel() == "2/2");
    rig.s().historyStep(-1);
    CHECK(rig.s().historyLabel() == "1/2");
    rig.s().historyStep(-1);      // the oldest: the text stays, the label says why
    CHECK(rig.s().historyLabel() == "1/2");
    CHECK(rig.text() == "a;");
    rig.s().historyStep(1);
    rig.s().historyStep(1);
    CHECK(rig.s().historyLabel().empty());
}

TEST_CASE("Running an old query from Pichli does not cost the script", "[wb][querystore][walk]") {
    WalkRig rig({"a;", "b;"}, "-- mera script");
    rig.s().historyStep(-1);
    rig.s().historyStep(-1);
    REQUIRE(rig.text() == "a;");
    rig.s().runEditorText();
    REQUIRE(rig.poster.pumpIdle(rig.s()));
    rig.s().persistEditor(true);                  // the autosave of the next frame
    CHECK(rig.savedScript() == "-- mera script");
    rig.s().historyStep(1);                       // the run moved the walk to the newest end
    CHECK(rig.text() == "-- mera script");
}

TEST_CASE("Agli with no walk going on never touches the writing area", "[wb][querystore][walk]") {
    SECTION("local") {
        WalkRig rig({"a;"});
        rig.s().editor().setText("likh raha hoon");
        rig.s().historyStep(1);
        CHECK(rig.text() == "likh raha hoon");
    }
    SECTION("server: no store, the text is still not wiped") {
        ManualPoster poster;
        Session s(std::make_unique<FakeBackend>(), SessionOptions{}, poster.poster());
        REQUIRE(poster.pumpIdle(s));
        s.runText("a;");
        REQUIRE(poster.pumpIdle(s));
        s.editor().setText("likh raha hoon");
        s.historyStep(1);
        CHECK(s.editor().text() == "likh raha hoon");
    }
}

TEST_CASE("On a server connection Agli past the newest entry brings the draft back too", "[wb][querystore][walk]") {
    ManualPoster poster;
    Session s(std::make_unique<FakeBackend>(), SessionOptions{}, poster.poster());
    REQUIRE(poster.pumpIdle(s));
    s.runText("a;");
    REQUIRE(poster.pumpIdle(s));
    s.editor().setText("mera draft");
    s.historyStep(-1);
    REQUIRE(s.editor().text() == "a;");
    s.historyStep(1);
    CHECK(s.editor().text() == "mera draft");
}

TEST_CASE("An edit of a shown entry is the new draft, on a server connection too", "[wb][querystore][walk]") {
    ManualPoster poster;
    Session s(std::make_unique<FakeBackend>(), SessionOptions{}, poster.poster());
    REQUIRE(poster.pumpIdle(s));
    s.runText("a;");
    s.runText("b;");
    REQUIRE(poster.pumpIdle(s));
    s.editor().setText("purana draft");
    s.historyStep(-1);
    s.editor().setText("b; -- badla");
    s.historyStep(1);    // no autosave ended the walk here: the edit must still not be lost
    CHECK(s.editor().text() == "b; -- badla");
}

TEST_CASE("Stepping on after editing a shown entry does not save the next entry as the script", "[wb][querystore][walk]") {
    WalkRig rig({"a;", "b;"}, "-- script");
    rig.s().historyStep(-1);
    rig.s().historyStep(-1);
    REQUIRE(rig.text() == "a;");
    rig.s().editor().setText("a; -- badla");
    rig.s().persistEditor(true);          // real work: saved, and the walk is over
    REQUIRE(rig.savedScript() == "a; -- badla");
    rig.s().historyStep(1);               // goes on through the list
    CHECK(rig.text() == "b;");
    rig.s().persistEditor(true);
    CHECK(rig.savedScript() == "a; -- badla");
    rig.s().historyStep(1);
    CHECK(rig.text() == "a; -- badla");
}

TEST_CASE("The draft of one database never lands in another", "[wb][querystore][walk]") {
    WalkRig rig({"a;"}, "-- main ka script");
    QueryStore(rig.dir.path.string()).saveScript("college", "-- college ka script");
    QueryStore(rig.dir.path.string()).append("college", {"c;", ""});
    rig.s().historyStep(-1);              // walking in main when the card opens college
    REQUIRE(rig.text() == "a;");
    rig.s().openDatabase("college");
    REQUIRE(rig.poster.pumpIdle(rig.s()));
    REQUIRE(rig.text() == "-- college ka script");
    rig.s().historyStep(-1);
    REQUIRE(rig.text() == "c;");
    rig.s().historyStep(1);
    CHECK(rig.text() == "-- college ka script");
    CHECK(rig.savedScript("main") == "-- main ka script");
}

TEST_CASE("A text typed just before Pichli is saved, even inside the save delay", "[wb][querystore][walk]") {
    WalkRig rig({"a;"});
    rig.s().editor().setText("-- pehle");
    rig.s().persistEditor(true);
    rig.s().editor().setText("-- abhi likha");
    rig.s().persistEditor();                     // inside the delay: not written yet
    rig.s().historyStep(-1);
    rig.s().persistEditor(true);
    CHECK(rig.savedScript() == "-- abhi likha");
}

// ---- the history follows the database, however it was opened ----

TEST_CASE("A typed ISTEMAL brings the history of that database, not a mix", "[wb][querystore][history]") {
    WalkRig rig({"main1;"});
    QueryStore(rig.dir.path.string()).append("college", {"college1;", ""});
    QueryStore(rig.dir.path.string()).append("college", {"college2;", ""});
    rig.run("ISTEMAL college;");
    CHECK(rig.s().history().items() == std::vector<std::string>({"college1;", "college2;"}));
    REQUIRE(rig.s().savedQueries().size() == 2);       // the Queries tab and Pichli agree
    rig.s().historyStep(-1);
    CHECK(rig.text() == "college2;");
}

TEST_CASE("A table opened from the tree of another database stays in the history", "[wb][querystore][history]") {
    WalkRig rig({"main1;"});
    QueryStore(rig.dir.path.string()).append("college", {"college1;", ""});
    rig.s().runText("ISTEMAL college;");   // what activating the row queues: the switch, then the statement
    rig.s().runText("DIKHAO * SE t SIRF 100;");
    REQUIRE(rig.poster.pumpIdle(rig.s()));
    CHECK(rig.s().history().items() == std::vector<std::string>({"college1;", "DIKHAO * SE t SIRF 100;"}));
}

TEST_CASE("A script that switches the database stays reachable with Pichli", "[wb][querystore][history]") {
    WalkRig rig;
    rig.fake->beforeRun = [f = rig.fake](const std::string&) { f->db = "college"; };
    rig.run("BANAO DATABASE college; ISTEMAL college; DIKHAO * SE t;");
    CHECK(rig.s().currentDb() == "college");
    CHECK(rig.s().history().items() == std::vector<std::string>({"BANAO DATABASE college; ISTEMAL college; DIKHAO * SE t;"}));
}

TEST_CASE("The queries of a database come back with Pichli after a restart", "[wb][querystore][history]") {
    TempDir dir;
    auto make = [&](ManualPoster& poster) {
        auto owned = std::make_unique<FakeBackend>();
        owned->desc = "local (" + dir.path.string() + ")";
        SessionOptions opts;
        opts.dataDir = dir.path.string();
        return std::make_unique<Session>(std::move(owned), std::move(opts), poster.poster());
    };
    {
        ManualPoster poster;
        auto s = make(poster);
        REQUIRE(poster.pumpIdle(*s));
        s->runText("a;");
        s->runText("b;");
        REQUIRE(poster.pumpIdle(*s));
    }
    ManualPoster poster;
    auto s = make(poster);
    REQUIRE(poster.pumpIdle(*s));
    s->historyStep(-1);
    CHECK(s->editor().text() == "b;");
    s->historyStep(-1);
    CHECK(s->editor().text() == "a;");
}

TEST_CASE("A one-line script that starts with ISTEMAL is a query, only a bare switch is not", "[wb][querystore][history]") {
    WalkRig rig;
    rig.run("ISTEMAL main; DIKHAO * SE t;");
    REQUIRE(rig.s().savedQueries().size() == 1);
    CHECK(rig.s().savedQueries()[0].text == "ISTEMAL main; DIKHAO * SE t;");
    rig.run("ISTEMAL main;");
    CHECK(rig.s().savedQueries().size() == 1);
}

TEST_CASE("Queries saaf karo empties Pichli too and gives the script back", "[wb][querystore][history]") {
    WalkRig rig({"a;", "b;"}, "-- script");
    rig.s().historyStep(-1);
    REQUIRE(rig.text() == "b;");
    rig.s().clearSavedQueries();
    CHECK(rig.s().history().size() == 0);
    CHECK(rig.text() == "-- script");
    rig.s().persistEditor(true);
    CHECK(rig.savedScript() == "-- script");
}

// ---- every table of a script ----

TEST_CASE("Every table of a script is kept, the last one is shown, and any of them can be chosen", "[wb][resultsets]") {
    meradb_test::TempDir tmp;
    ManualPoster poster;
    auto owned = std::make_unique<FakeBackend>();
    owned->replies["s;"] = {wbtest::tableResult({"a"}, {{meradb::Value(int64_t(1))}}), meradb::Result{},
                            wbtest::tableResult({"b"}, {{meradb::Value(int64_t(2))}, {meradb::Value(int64_t(3))}})};
    owned->replies["s;"][1].error = "galti";
    owned->replies["m;"] = {meradb::Result{}};
    owned->replies["m;"][0].message = "ho gaya";
    SessionOptions opts;
    opts.exportBaseDir = tmp.str();
    Session s(std::move(owned), std::move(opts), poster.poster());
    REQUIRE(poster.pumpIdle(s));
    CHECK(s.table() == nullptr);
    s.runText("s;");
    REQUIRE(poster.pumpIdle(s));
    REQUIRE(s.resultSets().size() == 2);
    CHECK(s.resultSets()[0].statement == 1);
    CHECK(s.resultSets()[1].statement == 3);
    CHECK(s.table()->columns == std::vector<std::string>{"b"});   // like before: the last table
    REQUIRE(s.lastRunErrors().size() == 1);
    CHECK(s.lastRunErrors()[0].statement == 2);
    const int version = s.resultVersion();
    s.selectResultSet(0);
    CHECK(s.resultVersion() == version + 1);
    CHECK(s.table()->columns == std::vector<std::string>{"a"});
    s.exportCsv();                                                // the CSV is the table on screen
    CHECK(s.lastExport().message.rfind("1 row(s)", 0) == 0);
    s.selectResultSet(0);
    s.selectResultSet(7);
    CHECK(s.resultVersion() == version + 1);                      // nothing changed
    s.runText("m;");                                              // no table: the tables stay, the failure is gone
    REQUIRE(poster.pumpIdle(s));
    CHECK(s.resultSets().size() == 2);
    CHECK(s.lastRunErrors().empty());
}

TEST_CASE("A syntax error or a lost connection is one failure without a statement number", "[wb][resultsets]") {
    ManualPoster poster;
    auto owned = std::make_unique<FakeBackend>();
    FakeBackend* fake = owned.get();
    owned->replies["bad;"] = {meradb::Result{}};
    owned->replies["bad;"][0].error = "syntax";
    Session s(std::move(owned), SessionOptions{}, poster.poster());
    REQUIRE(poster.pumpIdle(s));
    s.runText("bad;");
    REQUIRE(poster.pumpIdle(s));
    REQUIRE(s.lastRunErrors().size() == 1);
    CHECK(s.lastRunErrors()[0].statement == 0);
    fake->dropped = true;
    s.runText("x;");
    REQUIRE(poster.pumpIdle(s));
    REQUIRE(s.lastRunErrors().size() == 1);
    CHECK(s.lastRunErrors()[0].text.find("toot gaya") != std::string::npos);
}

// ---- one query out of a script ----

TEST_CASE("statementAt finds the statement around the caret", "[wb][statement]") {
    const std::string script = "DIKHAO 'a;b' SE t; -- yeh; bhi\n\nDIKHAO * SE u;\nBADLO t RAKHO a = 1";
    const std::string first = "DIKHAO 'a;b' SE t;", second = "DIKHAO * SE u;", third = "BADLO t RAKHO a = 1";
    CHECK(statementAt(script, 0) == first);
    CHECK(statementAt(script, 9) == first);                   // inside the string
    CHECK(statementAt(script, first.size()) == first);        // right after the semicolon
    CHECK(statementAt(script, script.find("yeh")) == second); // the comment belongs to what comes next
    CHECK(statementAt(script, script.find(second) + 3) == second);
    CHECK(statementAt(script, script.size()) == third);       // no semicolon at the end
    CHECK(statementAt("  ", 1).empty());
    CHECK(statementAt("-- sirf comment", 3).empty());
    CHECK(statementAt("DIKHAO 1;  \n", 11) == "DIKHAO 1;");    // past the last: the last
    CHECK(statementAt("DIKHAO 'khula", 2) == "DIKHAO 'khula"); // cannot be split: whole, for its error
}

TEST_CASE("statementAt keeps a procedure body together and stops at a broken statement", "[wb][statement]") {
    const std::string proc = "BANAO PROCEDURE p () SHURU DIKHAO 1; DIKHAO 2; KHATAM;";
    const std::string script = "DIKHAO 0;\n" + proc + "\nDIKHAO 3;";
    INFO(script);
    CHECK(statementAt(script, script.find("DIKHAO 2")) == proc);
    CHECK(statementAt(script, script.find("DIKHAO 3")) == "DIKHAO 3;");
    const std::string broken = "DIKHAO SE SE;\nDIKHAO 4;";
    CHECK(statementAt(broken, 2) == "DIKHAO SE SE;");
    CHECK(statementAt(broken, broken.size()) == "DIKHAO 4;");
}

TEST_CASE("One query of a script runs, with a broken one elsewhere in it", "[wb][statement]") {
    WalkRig rig;
    rig.s().editor().setText("BANAO BANAO;\nDIKHAO * SE t;\nDAALO MEIN t MAAN (1);");
    rig.s().editor().moveTo(Pos{1, 3}, false);
    rig.s().runEditorStatement();
    REQUIRE(rig.poster.pumpIdle(rig.s()));
    CHECK(rig.fake->ranList() == std::vector<std::string>({"DIKHAO * SE t;"}));
    CHECK(rig.s().history().items().back() == "DIKHAO * SE t;");
    rig.s().editor().selectRange(Pos{2, 0}, Pos{2, 5});      // a selection wins
    rig.s().runEditorStatement();
    REQUIRE(rig.poster.pumpIdle(rig.s()));
    CHECK(rig.fake->ranList().back() == "DAALO");
}

TEST_CASE("A query from the list goes below the script, selected, and only it runs", "[wb][statement]") {
    WalkRig rig({"a;"}, "-- script");
    rig.s().addQueryToEditor("x;\ny;");
    CHECK(rig.text() == "-- script\n\nx;\ny;");
    CHECK(rig.s().editor().selectedText() == "x;\ny;");
    rig.s().runEditorText();
    REQUIRE(rig.poster.pumpIdle(rig.s()));
    CHECK(rig.fake->ranList() == std::vector<std::string>({"x;\ny;"}));
    rig.s().editor().setText("  ");
    rig.s().addQueryToEditor("z;");                          // into an empty area
    CHECK(rig.text() == "z;");
    CHECK(rig.s().editor().selectedText() == "z;");
}

TEST_CASE("A query from the list does not stack on a history entry being looked at", "[wb][statement]") {
    WalkRig rig({"a;"}, "-- script");
    rig.s().historyStep(-1);
    REQUIRE(rig.text() == "a;");
    rig.s().addQueryToEditor("q;");
    CHECK(rig.text() == "-- script\n\nq;");
}

// ---- Connect: the kept work follows the connection ----

TEST_CASE("The kept work follows a Connect in both directions", "[wb][querystore][connect]") {
    TempDir dir;
    QueryStore(dir.path.string()).append("main", {"local1;", ""});
    QueryStore(dir.path.string()).saveScript("main", "-- local script");
    ManualPoster poster;
    auto local = std::make_unique<FakeBackend>();
    local->desc = "local (" + dir.path.string() + ")";
    SessionOptions opts;
    opts.dataDir = dir.path.string();
    opts.factory = [&dir](const ConnectRequest& request, const std::string&) -> std::unique_ptr<meradb::Backend> {
        auto fresh = std::make_unique<FakeBackend>();
        fresh->desc = request.local ? "local (" + dir.path.string() + ")" : request.host + ":" + request.port;
        return fresh;
    };
    Session s(std::move(local), std::move(opts), poster.poster());
    REQUIRE(poster.pumpIdle(s));
    REQUIRE(s.savedQueriesEnabled());

    s.connect(makeConnectRequest(false, "server", "6372", "", ""));   // to a server: nothing is written locally any more
    REQUIRE(poster.pumpIdle(s));
    CHECK_FALSE(s.savedQueriesEnabled());
    CHECK(s.savedQueries().empty());
    s.runText("server_query;");
    REQUIRE(poster.pumpIdle(s));
    CHECK(QueryStore(dir.path.string()).load("main").size() == 1);
    s.editor().setText("-- server par likha");
    s.persistEditor(true);
    CHECK(QueryStore(dir.path.string()).loadScript("main") == "-- local script");

    s.connect(makeConnectRequest(true, "", "", "", ""));              // back: the list and Pichli are there again
    REQUIRE(poster.pumpIdle(s));
    CHECK(s.savedQueriesEnabled());
    REQUIRE(s.savedQueries().size() == 1);
    CHECK(s.history().items() == std::vector<std::string>({"local1;"}));
    CHECK(s.editor().text() == "-- server par likha");                // what is on screen is never replaced
    CHECK(QueryStore(dir.path.string()).loadScript("main") == "-- local script");
    s.connect(makeConnectRequest(false, "server", "6372", "", ""));
    REQUIRE(poster.pumpIdle(s));
    s.editor().setText("");
    s.connect(makeConnectRequest(true, "", "", "", ""));              // an empty area takes the kept script
    REQUIRE(poster.pumpIdle(s));
    CHECK(s.editor().text() == "-- local script");
}
