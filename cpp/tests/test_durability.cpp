// cpp/tests/test_durability.cpp
//
// Which syncs happen, and in what order. A kill test (crash_kill.py) cannot see a missing sync, because the operating
// system still holds the bytes of a killed process. And nothing here can pull the plug on a real disk: these tests
// prove the order of the writes that surviving a power cut depends on, not the surviving itself.
#include <catch2/catch_test_macros.hpp>
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "meradb/fs_util.h"
#include "meradb/storage.h"
#include "test_util.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace meradb;
using meradb_test::TempDir;
namespace fs = std::filesystem;

namespace {

std::vector<std::string> g_syncs;
std::vector<bool> g_commitMarkerSeen;
std::string g_markerPath;

void remember(const std::string& path) {
    g_syncs.push_back(pathOf(path).lexically_normal().generic_u8string());
    std::error_code ec;
    g_commitMarkerSeen.push_back(!g_markerPath.empty() && fs::exists(pathOf(g_markerPath), ec));
}

// Records every sync while it is alive.
struct SyncLog {
    SyncLog() {
        g_syncs.clear();
        g_commitMarkerSeen.clear();
        g_markerPath.clear();
        setSyncObserver(remember);
    }
    ~SyncLog() { setSyncObserver(nullptr); }
    void clear() {
        g_syncs.clear();
        g_commitMarkerSeen.clear();
    }
    static long indexOfEnding(const std::string& suffix) {
        for (size_t i = 0; i < g_syncs.size(); ++i)
            if (g_syncs[i].size() >= suffix.size() && g_syncs[i].compare(g_syncs[i].size() - suffix.size(), suffix.size(), suffix) == 0)
                return static_cast<long>(i);
        return -1;
    }
    static int countEnding(const std::string& suffix) {
        int n = 0;
        for (const auto& path : g_syncs)
            if (path.size() >= suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) ++n;
        return n;
    }
};

}  // namespace

TEST_CASE("A folder can really be synced on this file system", "[durability]") {
    TempDir dir("meradb_durable_");
    CHECK(syncFolder(dir.str()));
}

TEST_CASE("An autocommit statement has its table file synced before it returns", "[durability]") {
    TempDir dir("meradb_durable_");
    Engine e(dir.str());
    e.execute("BANAO TABLE t (id INT, v INT);");
    SyncLog log;
    e.execute("DAALO MEIN t MAAN (1, 1);");
    CHECK(SyncLog::countEnding("/main/t.tbl") == 1);
    log.clear();
    e.execute("MITAO SE t JAHAN id = 1;");
    CHECK(SyncLog::countEnding("/main/t.tbl") == 1);
    log.clear();
    e.execute("DIKHAO * SE t;");
    CHECK(g_syncs.empty());  // a read writes nothing
}

TEST_CASE("One statement over many rows syncs its file once", "[durability]") {
    TempDir dir("meradb_durable_");
    Engine e(dir.str());
    e.execute("BANAO TABLE t (id INT, v INT);");
    e.execute("DAALO MEIN t MAAN (1, 1), (2, 2), (3, 3), (4, 4);");
    SyncLog log;
    e.execute("BADLO t RAKHO v = 9;");  // tombstones and appends: two writes, one sync
    CHECK(SyncLog::countEnding("/main/t.tbl") == 1);
}

TEST_CASE("SHURU has the whole snapshot on the disk before it gets its name", "[durability]") {
    TempDir dir("meradb_durable_");
    Engine e(dir.str());
    e.execute("BANAO TABLE t (id INT, v INT); BANAO TABLE u (id INT);");
    SyncLog log;
    e.execute("SHURU;");
    const long table = SyncLog::indexOfEnding("/.wapas/main.tmp/t.tbl");
    const long other = SyncLog::indexOfEnding("/.wapas/main.tmp/u.tbl");
    const long catalog = SyncLog::indexOfEnding("/.wapas/main.tmp/catalog.json");
    const long folder = SyncLog::indexOfEnding("/.wapas");  // the rename to the real name, last
    REQUIRE(table >= 0);
    REQUIRE(other >= 0);
    REQUIRE(catalog >= 0);
    REQUIRE(folder >= 0);
    CHECK(table < folder);
    CHECK(other < folder);
    CHECK(catalog < folder);
    e.execute("WAPAS;");
}

TEST_CASE("Inside a transaction nothing is synced until PAKKA, and then the data comes before the commit marker", "[durability]") {
    TempDir dir("meradb_durable_");
    Engine e(dir.str());
    e.execute("BANAO TABLE t (id INT, v INT); BANAO TABLE u (id INT);");
    e.execute("SHURU;");
    SyncLog log;
    for (int i = 0; i < 50; ++i) e.execute("DAALO MEIN t MAAN (" + std::to_string(i) + ", 0);");
    e.execute("DAALO MEIN u MAAN (1);");
    CHECK(g_syncs.empty());

    g_markerPath = (dir.path() / ".wapas" / "main.done").u8string();
    e.execute("PAKKA;");
    const long table = SyncLog::indexOfEnding("/main/t.tbl");
    const long other = SyncLog::indexOfEnding("/main/u.tbl");
    const long marker = SyncLog::indexOfEnding("/.wapas");
    REQUIRE(table >= 0);
    REQUIRE(other >= 0);
    REQUIRE(marker >= 0);
    CHECK(SyncLog::countEnding("/main/t.tbl") == 1);
    CHECK(table < marker);
    CHECK(other < marker);
    CHECK_FALSE(g_commitMarkerSeen[static_cast<size_t>(table)]);  // the data went out while rollback was still possible
    CHECK(g_commitMarkerSeen[static_cast<size_t>(marker)]);       // and the sync of the folder is the sync of the marker
}

TEST_CASE("PAKKA syncs a table under its new name after RENAME, and after a savepoint rollback swapped every file", "[durability]") {
    TempDir dir("meradb_durable_");
    Engine e(dir.str());
    e.execute("BANAO TABLE t (id INT);");
    e.execute("SHURU; DAALO MEIN t MAAN (1); SUDHARO TABLE t NAYA_NAAM naya;");
    {
        SyncLog log;
        e.execute("PAKKA;");
        CHECK(SyncLog::countEnding("/main/naya.tbl") == 1);
    }
    e.execute("SHURU; SAVEPOINT a; DAALO MEIN naya MAAN (2); ROLLBACK TO a;");
    SyncLog log;
    e.execute("PAKKA;");
    CHECK(SyncLog::countEnding("/main/naya.tbl") >= 1);
    CHECK(SyncLog::countEnding("/main/catalog.json") == 1);
}

TEST_CASE("A heap file used without any statement syncs every write itself", "[durability]") {
    TempDir dir("meradb_durable_");
    const std::string path = (dir.path() / "x.tbl").u8string();
    HeapFile heap(path);
    heap.create();
    SyncLog log;
    const auto ids = heap.insertMany({{1, 2, 3}});
    CHECK(SyncLog::countEnding("/x.tbl") == 1);
    heap.deleteMany(ids);
    CHECK(SyncLog::countEnding("/x.tbl") == 2);
}

TEST_CASE("The undo journal is written before the data, wiped after the sync, and gone after a clean close", "[durability]") {
    TempDir dir("meradb_durable_");
    const fs::path log = dir.path() / ".wapas" / "journal.tmp" / "log";
    {
        Engine e(dir.str());
        e.execute("BANAO TABLE t (id INT); DAALO MEIN t MAAN (1);");
        REQUIRE(fs::exists(log));
        std::ifstream in(log, std::ios::binary);
        char header[8] = {};
        in.read(header, sizeof header);
        CHECK(std::string(header, sizeof header) == std::string(8, '\0'));  // nothing in flight between statements
    }
    CHECK_FALSE(fs::exists(log.parent_path()));
}

TEST_CASE("An unreadable or wiped journal undoes nothing", "[durability]") {
    TempDir dir("meradb_durable_");
    { Engine e(dir.str()); e.execute("BANAO TABLE t (id INT); DAALO MEIN t MAAN (1);"); }
    const fs::path folder = dir.path() / ".wapas" / "journal.tmp";
    for (const std::string& junk : {std::string("garbage that is no journal at all"), std::string("MDBUNDO1"),
                                    std::string("MDBUNDO1") + std::string(8, 'x') + std::string(40, 'y'), std::string()}) {
        fs::create_directories(folder);
        { std::ofstream out(folder / "log", std::ios::binary); out << junk; }
        Instance inst(dir.str());
        CHECK(inst.recovered().empty());
        CHECK_FALSE(fs::exists(folder));
    }
    Engine e(dir.str());
    CHECK(formatValue(e.execute("DIKHAO GINO(*) SE t;")[0].rows[0][0]) == "1");
}

TEST_CASE("A statement that fails is synced like one that works: its partial writes are what the files now hold", "[durability]") {
    TempDir dir("meradb_durable_");
    Engine e(dir.str());
    e.execute("BANAO TABLE t (id INT MUKHYA KUNJI);");
    e.execute("DAALO MEIN t MAAN (1);");
    SyncLog log;
    CHECK_THROWS(e.execute("DAALO MEIN t MAAN (1);"));
    log.clear();
    e.execute("DAALO MEIN t MAAN (2);");  // and the next statement starts clean
    CHECK(SyncLog::countEnding("/main/t.tbl") == 1);
    CHECK(formatValue(e.execute("DIKHAO GINO(*) SE t;")[0].rows[0][0]) == "2");
}

TEST_CASE("HATAO DATABASE leaves no half database behind and TRUNCATE goes through a swapped file", "[durability]") {
    TempDir dir("meradb_durable_");
    Engine e(dir.str());
    e.execute("BANAO DATABASE d; ISTEMAL d; BANAO TABLE t (id INT); DAALO MEIN t MAAN (1); SAAF TABLE t;");
    CHECK(formatValue(e.execute("DIKHAO GINO(*) SE t;")[0].rows[0][0]) == "0");
    e.execute("ISTEMAL main; HATAO DATABASE d;");
    CHECK_FALSE(fs::exists(dir.path() / "d"));
    CHECK_FALSE(fs::exists(dir.path() / ".wapas" / "d.done"));
}
