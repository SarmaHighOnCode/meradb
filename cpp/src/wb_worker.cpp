// cpp/src/wb_worker.cpp
#include "meradb/wb_worker.h"
#include <utility>

namespace meradb::wb {

Worker::Worker() : thread_([this] { loop(); }) {}

Worker::~Worker() { stopAndJoin(); }

bool Worker::post(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return false;
        queue_.push_back(std::move(job));
    }
    wake_.notify_one();
    return true;
}

std::size_t Worker::cancelPending() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t dropped = queue_.size();
    queue_.clear();
    return dropped;
}

void Worker::stopAndJoin() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable() && std::this_thread::get_id() != thread_.get_id()) thread_.join();
}

void Worker::loop() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return;  // stopping and drained
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        try {
            job();
        } catch (...) {  // a job reports its own failures; nothing may end the thread
        }
    }
}

}  // namespace meradb::wb
