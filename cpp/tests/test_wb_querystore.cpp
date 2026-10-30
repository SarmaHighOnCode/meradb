// cpp/tests/test_wb_querystore.cpp -- the saved queries (QueryStore and Session::savedQueries)
#include "wb_test_util.h"
#include <catch2/catch_test_macros.hpp>
#include "meradb/wb_querystore.h"
#include "meradb/wb_session.h"
#include <filesystem>
#include <fstream>

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
