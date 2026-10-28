// cpp/include/meradb/gui_poster.h -- how the session's worker thread reaches the UI thread of the desktop window.
//
// The workbench Session (wb_session.h) hands every UI update to a UiPoster. Here that poster pushes the closure into
// this queue and calls the wake function (glfwPostEmptyEvent in the program), which makes the idle main loop leave
// glfwWaitEvents; the loop then calls drain() on the UI thread, once per frame.
#pragma once
#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>

namespace meradb::gui {

class UiPostQueue {
public:
    // Any thread. Dropped silently after close(). The wake function (if set) is called after the closure is queued,
    // outside the queue's lock.
    void post(std::function<void()> fn);
    // UI thread. Runs every closure queued so far, in order, and returns how many ran. A closure posted while
    // draining waits for the next drain (so one that posts again cannot keep a frame busy forever).
    std::size_t drain();
    // Called from any thread after each post. Set it before the first post, or never.
    void setWake(std::function<void()> wake);
    // Stops accepting closures, forgets the wake function and drops what is still queued. Idempotent; call it before
    // the thing the wake function uses (the GLFW library) goes away.
    void close();
    std::size_t pending() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::function<void()>> queue_;
    std::function<void()> wake_;
    bool closed_ = false;
};

}  // namespace meradb::gui
