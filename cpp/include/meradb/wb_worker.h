// cpp/include/meradb/wb_worker.h
#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace meradb::wb {

// ONE thread and a FIFO queue. Every database call of a workbench session runs here: a transaction's statements
// must all run on the thread that ran SHURU, so this is deliberately not a pool.
class Worker {
public:
    Worker();
    ~Worker();  // stopAndJoin()
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    bool post(std::function<void()> job);   // false once stopAndJoin() has begun
    std::size_t cancelPending();            // drops jobs that have not started; returns how many
    void stopAndJoin();                     // finishes the running job and the queued ones, then joins; idempotent
    std::thread::id threadId() const { return thread_.get_id(); }

private:
    void loop();
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
    bool stopping_ = false;
    std::thread thread_;  // declared last: it starts running in the constructor and uses the members above
};

}  // namespace meradb::wb
