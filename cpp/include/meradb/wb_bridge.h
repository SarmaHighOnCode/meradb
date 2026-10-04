// cpp/include/meradb/wb_bridge.h -- hands closures from the worker thread to the screen's event loop.
//
// ScreenInteractive::Post drops a task silently while the loop is not running (its task channel only exists between
// the start of Loop() and its end), and the session's worker posts its first closure as soon as the Session is built,
// i.e. before Loop(). So the bridge queues closures until start() is called from inside the loop (the first frame
// drawn), then forwards them in order; after close() nothing is forwarded any more (the worker must not post into a
// finished screen). Thread safe.
#pragma once
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace meradb::wb {

class UiBridge {
public:
    using Task = std::function<void()>;
    using Sink = std::function<void(Task)>;   // thread safe; runs the task on the UI thread (and redraws)

    explicit UiBridge(Sink sink) : sink_(std::move(sink)) {}

    // Any thread. Before start(): kept in order. After close(): dropped.
    void post(Task task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        if (!running_) {
            queued_.push_back(std::move(task));
            return;
        }
        sink_(std::move(task));
    }
    // UI thread, from inside the running loop: forwards what was queued, in order, then everything that follows.
    void start() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_ || closed_) return;
        running_ = true;
        std::vector<Task> queued;
        queued.swap(queued_);
        for (Task& task : queued) sink_(std::move(task));
    }
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        queued_.clear();
    }
    bool running() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

private:
    mutable std::mutex mutex_;
    Sink sink_;
    std::vector<Task> queued_;
    bool running_ = false;
    bool closed_ = false;
};

}  // namespace meradb::wb
