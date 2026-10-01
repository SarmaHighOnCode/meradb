// cpp/tests/test_engine_server_support.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "meradb/protocol.h"
#include "test_util.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace meradb;

namespace {

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace

TEST_CASE("engine_server schemaTree matches Python's schema_tree JSON", "[engine][server]") {
    meradb_test::TempDir dir;
    Engine e(dir.str());
    e.execute("BANAO TABLE zeta (id INT MUKHYA KUNJI, naam TEXT ZAROORI); "
              "BANAO TABLE alpha (a FLOAT WARNA 1.5, d DATE); BANAO DATABASE other");
    // json.dumps(Engine.schema_tree()) from the Python engine for the same statements.
    CHECK(pyjson::dump(e.schemaTree()) ==
          "[{\"name\": \"main\", \"current\": true, \"tables\": [{\"name\": \"alpha\", \"columns\": ["
          "{\"name\": \"a\", \"type_name\": \"FLOAT\", \"primary_key\": false, \"not_null\": false, \"unique\": false, "
          "\"default\": 1.5, \"max_length\": null, \"ref_table\": null, \"ref_column\": null, \"check\": null}, "
          "{\"name\": \"d\", \"type_name\": \"DATE\", \"primary_key\": false, \"not_null\": false, \"unique\": false, "
          "\"default\": null, \"max_length\": null, \"ref_table\": null, \"ref_column\": null, \"check\": null}]}, "
          "{\"name\": \"zeta\", \"columns\": ["
          "{\"name\": \"id\", \"type_name\": \"INT\", \"primary_key\": true, \"not_null\": false, \"unique\": false, "
          "\"default\": null, \"max_length\": null, \"ref_table\": null, \"ref_column\": null, \"check\": null}, "
          "{\"name\": \"naam\", \"type_name\": \"TEXT\", \"primary_key\": false, \"not_null\": true, \"unique\": false, "
          "\"default\": null, \"max_length\": null, \"ref_table\": null, \"ref_column\": null, \"check\": null}]}]}, "
          "{\"name\": \"other\", \"current\": false, \"tables\": []}]");
}

TEST_CASE("engine_server schemaTree marks the session's current database", "[engine][server]") {
    meradb_test::TempDir dir;
    Engine e(dir.str());
    e.execute("BANAO DATABASE college; ISTEMAL college");
    auto tree = e.schemaTree();
    REQUIRE(tree.size() == 2);
    CHECK(tree[0]["name"] == "college");
    CHECK(tree[0]["current"] == true);
    CHECK(tree[1]["name"] == "main");
    CHECK(tree[1]["current"] == false);
}

TEST_CASE("engine_server a statement waits for another session's transaction then reports busy", "[engine][server]") {
    meradb_test::TempDir dir;
    auto instance = std::make_shared<Instance>(dir.str());
    Engine main(instance);

    std::atomic<bool> holding{false};
    std::atomic<bool> release{false};
    std::thread holder([&] {  // SHURU and WAPAS must run on the SAME thread
        Engine other(instance);
        other.execute("SHURU");
        holding = true;
        while (!release) sleepMs(5);
        other.execute("WAPAS");
    });
    while (!holding) sleepMs(5);

    instance->lockTimeoutSeconds = 0.2;
    auto results = main.runScript("DIKHAO TABLES");
    REQUIRE(results.size() == 1);
    CHECK(results[0].error.find("Database busy hai") != std::string::npos);

    auto started = std::chrono::steady_clock::now();
    try {
        main.schemaTree();  // waits 2 s, not lockTimeoutSeconds
        FAIL("expected the busy error");
    } catch (const MeraDBError& e) {
        CHECK(e.message().find("Database busy hai") == 0);
    }
    auto waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(waited >= 1.5);

    release = true;
    holder.join();
    CHECK(main.runScript("DIKHAO TABLES")[0].error.empty());  // the lock is free again
    CHECK_NOTHROW(main.schemaTree());
}

TEST_CASE("engine_server an embedded engine refuses a folder a server is serving", "[engine][server]") {
    meradb_test::TempDir dir;
    net::Socket listener = net::listenOn("127.0.0.1", 0);
    pyjson::Json info = pyjson::Json::object();
    info["pid"] = 1;
    info["host"] = "127.0.0.1";
    info["port"] = listener.localPort();
    protocol::writePidFile(dir.str(), info);

    try {
        Engine e(dir.str());
        FAIL("expected the refusal");
    } catch (const MeraDBError& e) {
        CHECK(e.message() == "Is data folder par MeraDB server chal raha hai (port " + std::to_string(listener.localPort()) +
                                 "). Seedha files mat kholo -- `meradb shell` se server se connect karo.");
    }
    CHECK_NOTHROW(Instance(dir.str(), /*served=*/true));  // the server process itself opens it

    listener.close();
    CHECK_NOTHROW(Engine(dir.str()));  // stale pid file: nothing listens any more
}

TEST_CASE("engine_server databases() lists directories only", "[engine][server]") {
    meradb_test::TempDir dir;
    Instance instance(dir.str());
    std::filesystem::create_directories(dir.path() / "zoo");
    std::filesystem::create_directories(dir.path() / ".hidden");
    std::ofstream(dir.file("users.json")) << "{}";
    CHECK(instance.databases() == std::vector<std::string>{"main", "zoo"});
}

TEST_CASE("engine_server close() releases the shared lock even when the rollback itself fails", "[engine][server]") {
    meradb_test::TempDir dir;
    auto instance = std::make_shared<Instance>(dir.str());
    instance->lockTimeoutSeconds = 0.3;
    Engine a(instance);
    a.execute("BANAO TABLE t (x INT); SHURU");
    // Without its snapshot the restore throws a std::filesystem_error (not a MeraDBError).
    std::filesystem::remove_all(std::filesystem::path(dir.str()) / SNAPSHOT_DIR / "main");
    a.close();
    CHECK_FALSE(a.inTransaction());
    std::string error = "unset";
    std::thread other([&] {
        Engine b(instance);
        error = b.runScript("DIKHAO TABLES")[0].error;
    });
    other.join();
    CHECK(error.find("Database busy") == std::string::npos);
}
