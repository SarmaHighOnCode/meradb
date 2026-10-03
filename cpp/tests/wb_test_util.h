// cpp/tests/wb_test_util.h -- a scripted backend and a stand-in for ScreenInteractive::Post.
#pragma once
#include "meradb/backend.h"
#include "meradb/errors.h"
#include "meradb/wb_session.h"
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wbtest {

// Stands in for ScreenInteractive::Post: closures queue up and the TEST thread (the "UI thread") runs them.
class ManualPoster {
public:
    meradb::wb::UiPoster poster() {
        return [this](std::function<void()> f) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.push_back(std::move(f));
            }
            wake_.notify_all();
        };
    }
    // Runs posted closures here until done() is true; false on timeout.
    bool pumpUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (done()) return true;
            std::function<void()> f;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (queue_.empty() && wake_.wait_until(lock, deadline) == std::cv_status::timeout && queue_.empty())
                    return done();
                if (queue_.empty()) continue;
                f = std::move(queue_.front());
                queue_.pop_front();
            }
            f();
        }
    }
    bool pumpIdle(meradb::wb::Session& s) { return pumpUntil([&s] { return !s.busy(); }); }

private:
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
};

// A Backend whose answers the test scripts. All calls are recorded with the thread they ran on.
class FakeBackend : public meradb::Backend {
public:
    std::map<std::string, std::vector<meradb::Result>> replies;  // by exact statement text; default: message "ok"
    std::function<void(const std::string&)> beforeRun;           // may block (runs on the worker)
    bool dropped = false;      // runScript throws "Server se connection toot gaya"
    bool schemaFails = false;  // schemaTree throws
    bool txn = false;          // set by "SHURU;", cleared by "PAKKA;" / "WAPAS;"
    // What the backend saw, shared so a test can still read it after the session has destroyed the backend.
    struct Shared {
        std::mutex m;
        std::vector<std::string> ran;
        std::vector<std::thread::id> threads;
        bool closed = false, rolledBackOnClose = false;
        int closeCalls = 0;
    };
    std::shared_ptr<Shared> seen = std::make_shared<Shared>();
    std::string db = "main", desc = "fake:1";
    nlohmann::ordered_json tree = nlohmann::ordered_json::array();

    std::vector<std::string> ranList() const { std::lock_guard<std::mutex> l(seen->m); return seen->ran; }
    std::vector<std::thread::id> threadList() const { std::lock_guard<std::mutex> l(seen->m); return seen->threads; }
    bool closed() const { std::lock_guard<std::mutex> l(seen->m); return seen->closed; }

    std::vector<meradb::Result> runScript(const std::string& text) override {
        { std::lock_guard<std::mutex> l(seen->m); seen->threads.push_back(std::this_thread::get_id()); }
        if (beforeRun) beforeRun(text);
        if (dropped) throw meradb::ConnectionFailed("Server se connection toot gaya: fake");
        { std::lock_guard<std::mutex> l(seen->m); seen->ran.push_back(text); }
        if (text == "SHURU;") txn = true;
        if (text == "PAKKA;" || text == "WAPAS;") txn = false;
        auto it = replies.find(text);
        if (it != replies.end()) return it->second;
        meradb::Result r;
        r.message = "ok";
        return {r};
    }
    std::vector<meradb::Result> execute(const std::string& text) override { return runScript(text); }
    std::string currentDb() override { return db; }
    bool inTransaction() override { return txn; }
    nlohmann::ordered_json schemaTree() override {
        if (schemaFails) throw meradb::StorageError("schema nahi mila");
        return tree;
    }
    std::string description() override { return desc; }
    void close() override {
        std::lock_guard<std::mutex> l(seen->m);
        seen->closed = true;
        seen->rolledBackOnClose = txn;
        ++seen->closeCalls;
        txn = false;
    }
};

inline meradb::Result tableResult(std::vector<std::string> columns, std::vector<std::vector<meradb::Value>> rows) {
    meradb::Result r;
    r.columns = std::move(columns);
    r.rows = std::move(rows);
    return r;
}

}  // namespace wbtest
