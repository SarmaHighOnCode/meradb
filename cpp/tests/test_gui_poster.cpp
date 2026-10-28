// cpp/tests/test_gui_poster.cpp -- the queue that carries the session worker's closures to the UI thread.
#include <catch2/catch_test_macros.hpp>
#include "meradb/gui_poster.h"
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using meradb::gui::UiPostQueue;

TEST_CASE("gui poster runs closures in order on drain", "[gui][poster]") {
    UiPostQueue queue;
    std::vector<int> seen;
    queue.post([&] { seen.push_back(1); });
    queue.post([&] { seen.push_back(2); });
    queue.post([&] { seen.push_back(3); });
    CHECK(queue.pending() == 3);
    CHECK(seen.empty());   // nothing runs until the UI thread drains
    CHECK(queue.drain() == 3);
    CHECK(seen == std::vector<int>{1, 2, 3});
    CHECK(queue.pending() == 0);
    CHECK(queue.drain() == 0);
}

TEST_CASE("gui poster wakes the loop after each post", "[gui][poster]") {
    UiPostQueue queue;
    int wakes = 0;
    queue.setWake([&] { ++wakes; });
    queue.post([] {});
    queue.post([] {});
    CHECK(wakes == 2);
    queue.drain();
    CHECK(wakes == 2);   // draining does not wake
}

TEST_CASE("gui poster: a closure posted while draining waits for the next drain", "[gui][poster]") {
    UiPostQueue queue;
    std::vector<std::string> seen;
    queue.post([&] {
        seen.push_back("outer");
        queue.post([&] { seen.push_back("inner"); });
    });
    CHECK(queue.drain() == 1);
    CHECK(seen == std::vector<std::string>{"outer"});
    CHECK(queue.drain() == 1);
    CHECK(seen == std::vector<std::string>{"outer", "inner"});
}

TEST_CASE("gui poster accepts posts from many threads", "[gui][poster]") {
    UiPostQueue queue;
    std::atomic<int> wakes{0};
    queue.setWake([&] { ++wakes; });
    constexpr int kThreads = 4, kPerThread = 250;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) queue.post([] {});
        });
    for (auto& thread : threads) thread.join();
    CHECK(wakes == kThreads * kPerThread);
    std::size_t total = 0;
    while (std::size_t n = queue.drain()) total += n;
    CHECK(total == static_cast<std::size_t>(kThreads * kPerThread));
}

TEST_CASE("gui poster close drops what is queued and refuses more", "[gui][poster]") {
    UiPostQueue queue;
    int ran = 0, wakes = 0;
    queue.setWake([&] { ++wakes; });
    queue.post([&] { ++ran; });
    queue.close();
    CHECK(queue.pending() == 0);
    queue.post([&] { ++ran; });
    CHECK(queue.drain() == 0);
    CHECK(ran == 0);
    CHECK(wakes == 1);    // the wake function is forgotten
    queue.close();        // idempotent
}
