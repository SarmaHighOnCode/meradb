// cpp/tests/test_wb_bridge.cpp -- the bridge between the worker thread and the screen's loop.
#include "wb_test_util.h"
#include "meradb/wb_bridge.h"
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

using namespace meradb::wb;
using namespace wbtest;

TEST_CASE("wbbridge queues posts until the loop runs, then forwards them in order", "[wbbridge]") {
    std::vector<int> seen;
    UiBridge bridge([&seen](UiBridge::Task task) { task(); });
    bridge.post([&seen] { seen.push_back(1); });
    bridge.post([&seen] { seen.push_back(2); });
    CHECK(seen.empty());
    CHECK_FALSE(bridge.running());
    bridge.start();
    CHECK(seen == std::vector<int>{1, 2});
    bridge.post([&seen] { seen.push_back(3); });
    CHECK(seen == std::vector<int>{1, 2, 3});
    bridge.start();   // a second start changes nothing
    CHECK(seen.size() == 3);
}

TEST_CASE("wbbridge drops everything after close", "[wbbridge]") {
    int ran = 0;
    UiBridge bridge([&ran](UiBridge::Task task) { task(); });
    bridge.post([&ran] { ++ran; });
    bridge.close();
    bridge.start();
    bridge.post([&ran] { ++ran; });
    CHECK(ran == 0);
}

TEST_CASE("wbbridge a session started before the loop still loads its tree and clears the busy label", "[wbbridge]") {
    // The Session's worker posts its first snapshot as soon as the Session exists: before the loop is running.
    ManualPoster ui;
    UiBridge bridge([&ui](UiBridge::Task task) { ui.poster()(std::move(task)); });
    auto owned = std::make_unique<FakeBackend>();
    owned->tree = nlohmann::ordered_json::parse(R"([{"name":"main","current":true,"tables":[]}])");
    Session session(std::move(owned), SessionOptions{}, [&bridge](std::function<void()> f) { bridge.post(std::move(f)); });
    // Wait until the worker has loaded the schema (it has then posted its closure into the queue).
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(session.busy());                 // the closure has not reached the UI: nothing was lost, nothing was shown yet
    CHECK(session.tree().rows().size() == 1);   // only the root

    bridge.start();                        // the first frame of the loop
    REQUIRE(ui.pumpIdle(session));
    CHECK_FALSE(session.busy());
    CHECK(session.tree().rows().size() == 2);   // Databases + main
    session.shutdown();
}
