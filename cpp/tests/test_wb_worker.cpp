// cpp/tests/test_wb_worker.cpp
#include "meradb/wb_worker.h"
#include <catch2/catch_test_macros.hpp>
#include <future>
#include <mutex>
#include <vector>

using meradb::wb::Worker;

TEST_CASE("wbworker runs jobs in order on another thread", "[wbworker]") {
    Worker w;
    std::mutex m;
    std::vector<int> order;
    std::vector<std::thread::id> ids;
    std::promise<void> done;
    for (int i = 0; i < 5; ++i) {
        REQUIRE(w.post([&, i] {
            std::lock_guard<std::mutex> l(m);
            order.push_back(i);
            ids.push_back(std::this_thread::get_id());
        }));
    }
    w.post([&] { done.set_value(); });
    done.get_future().wait();
    std::lock_guard<std::mutex> l(m);
    REQUIRE(order == std::vector<int>({0, 1, 2, 3, 4}));
    for (const auto& id : ids) {
        REQUIRE(id != std::this_thread::get_id());
        REQUIRE(id == w.threadId());
    }
}

TEST_CASE("wbworker cancelPending drops only queued jobs", "[wbworker]") {
    Worker w;
    std::promise<void> started, release;
    std::shared_future<void> gate = release.get_future().share();
    std::mutex m;
    std::vector<int> ran;
    w.post([&] {
        started.set_value();
        gate.wait();
        std::lock_guard<std::mutex> l(m);
        ran.push_back(1);
    });
    started.get_future().wait();
    w.post([&] { std::lock_guard<std::mutex> l(m); ran.push_back(2); });
    w.post([&] { std::lock_guard<std::mutex> l(m); ran.push_back(3); });
    REQUIRE(w.cancelPending() == 2);
    release.set_value();
    w.stopAndJoin();
    REQUIRE(ran == std::vector<int>({1}));
}

TEST_CASE("wbworker stopAndJoin runs queued jobs then refuses new ones", "[wbworker]") {
    Worker w;
    std::promise<void> started, release;
    std::shared_future<void> gate = release.get_future().share();
    int count = 0;
    w.post([&] {
        started.set_value();
        gate.wait();
        ++count;
    });
    started.get_future().wait();
    w.post([&] { ++count; });
    release.set_value();
    w.stopAndJoin();
    REQUIRE(count == 2);
    REQUIRE_FALSE(w.post([] {}));
    w.stopAndJoin();  // idempotent
}

TEST_CASE("wbworker a throwing job does not stop later jobs", "[wbworker]") {
    Worker w;
    int count = 0;
    w.post([] { throw std::runtime_error("boom"); });
    w.post([&] { ++count; });
    w.stopAndJoin();
    REQUIRE(count == 1);
}

TEST_CASE("wbworker destructor joins without a hang", "[wbworker]") {
    int count = 0;
    {
        Worker w;
        w.post([&] { ++count; });
    }
    REQUIRE(count == 1);
}
