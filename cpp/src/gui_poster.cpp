// cpp/src/gui_poster.cpp -- see gui_poster.h.
#include "meradb/gui_poster.h"
#include <utility>

namespace meradb::gui {

void UiPostQueue::post(std::function<void()> fn) {
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return;
        queue_.push_back(std::move(fn));
        wake = wake_;
    }
    if (wake) wake();
}

std::size_t UiPostQueue::drain() {
    std::vector<std::function<void()>> batch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        batch.swap(queue_);
    }
    for (auto& fn : batch)
        if (fn) fn();
    return batch.size();
}

void UiPostQueue::setWake(std::function<void()> wake) {
    std::lock_guard<std::mutex> lock(mutex_);
    wake_ = std::move(wake);
}

void UiPostQueue::close() {
    std::vector<std::function<void()>> dropped;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        wake_ = nullptr;
        dropped.swap(queue_);
    }
}

std::size_t UiPostQueue::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

}  // namespace meradb::gui
