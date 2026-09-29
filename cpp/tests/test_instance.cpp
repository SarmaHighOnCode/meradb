// cpp/tests/test_instance.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/engine.h"
#include "test_util.h"
#include <filesystem>
#include <fstream>

using namespace meradb;
using meradb_test::TempDir;
using meradb_test::readText;
namespace fs = std::filesystem;

namespace {
void writeFile(const fs::path& p, const std::string& text) {
    std::ofstream out(p);
    out << text;
}
}  // namespace

TEST_CASE("Instance creates the default database directory on startup", "[instance]") {
    TempDir dir("meradb_instance_");
    Instance inst(dir.str());
    REQUIRE(fs::is_directory(dir.path() / "main"));
    inst.catalog("main");
    REQUIRE(inst.databases() == std::vector<std::string>{"main"});
}

TEST_CASE("Instance databases() is sorted and hides the snapshot folder", "[instance]") {
    TempDir dir("meradb_instance_");
    fs::create_directories(dir.path() / "zeta");
    fs::create_directories(dir.path() / "alpha");
    fs::create_directories(dir.path() / ".wapas");
    Instance inst(dir.str());
    REQUIRE(inst.databases() == std::vector<std::string>{"alpha", "main", "zeta"});
}

TEST_CASE("Instance snapshot/restore round-trips a database directory", "[instance]") {
    TempDir dir("meradb_instance_");
    Instance inst(dir.str());
    writeFile(dir.path() / "main" / "marker.txt", "before");
    inst.takeSnapshot("main");
    REQUIRE(fs::is_directory(dir.path() / ".wapas" / "main"));
    REQUIRE_FALSE(fs::exists(dir.path() / ".wapas" / "main.tmp"));
    writeFile(dir.path() / "main" / "marker.txt", "after-corrupted");
    writeFile(dir.path() / "main" / "extra.txt", "new file");
    inst.restoreSnapshot("main");
    REQUIRE(readText((dir.path() / "main" / "marker.txt").string()) == "before");
    REQUIRE_FALSE(fs::exists(dir.path() / "main" / "extra.txt"));
    REQUIRE_FALSE(fs::exists(dir.path() / ".wapas" / "main"));
}

TEST_CASE("Instance discardSnapshot leaves the live database untouched", "[instance]") {
    TempDir dir("meradb_instance_");
    Instance inst(dir.str());
    inst.takeSnapshot("main");
    writeFile(dir.path() / "main" / "marker.txt", "committed-value");
    inst.discardSnapshot("main");
    REQUIRE_FALSE(fs::exists(dir.path() / ".wapas" / "main"));
    REQUIRE_FALSE(fs::exists(dir.path() / ".wapas" / "main.done"));
    REQUIRE(readText((dir.path() / "main" / "marker.txt").string()) == "committed-value");
}

TEST_CASE("Instance recovers an incomplete transaction on startup", "[instance]") {
    TempDir dir("meradb_instance_");
    {
        Instance inst(dir.str());
        writeFile(dir.path() / "main" / "marker.txt", "original");
        inst.takeSnapshot("main");
        writeFile(dir.path() / "main" / "marker.txt", "mid-transaction");
        // simulated crash: neither discardSnapshot nor restoreSnapshot
    }
    Instance inst2(dir.str());
    REQUIRE(inst2.recovered() == std::vector<std::string>{"main"});
    REQUIRE(readText((dir.path() / "main" / "marker.txt").string()) == "original");
}

TEST_CASE("Instance recovery deletes .tmp and .done leftovers without rolling back", "[instance]") {
    TempDir dir("meradb_instance_");
    fs::create_directories(dir.path() / "main");
    writeFile(dir.path() / "main" / "marker.txt", "live");
    fs::create_directories(dir.path() / ".wapas" / "main.tmp");
    fs::create_directories(dir.path() / ".wapas" / "other.done");
    Instance inst(dir.str());
    REQUIRE(inst.recovered().empty());
    REQUIRE_FALSE(fs::exists(dir.path() / ".wapas" / "main.tmp"));
    REQUIRE_FALSE(fs::exists(dir.path() / ".wapas" / "other.done"));
    REQUIRE(readText((dir.path() / "main" / "marker.txt").string()) == "live");
}

TEST_CASE("Instance forget() drops the cached catalog and index slots", "[instance]") {
    TempDir dir("meradb_instance_");
    Instance inst(dir.str());
    Catalog* first = &inst.catalog("main");
    REQUIRE(&inst.catalog("main") == first);
    auto slot = inst.indexCache("main", "t");
    REQUIRE(inst.indexCache("main", "t") == slot);
    inst.forget("main");
    REQUIRE(inst.indexCache("main", "t") != slot);
}
