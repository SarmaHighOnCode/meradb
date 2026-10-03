// cpp/include/meradb/backend_closer.h
//
// Python's `finally: backend.close()`: the backend is closed (an open transaction rolled back) on every way
// out of a command, including an exception that is not a MeraDBError. On a normal return the caller closes
// explicitly with close(), so a failure of that close is reported; on the unwinding path the destructor
// closes and swallows any error, because the exception already in flight is the one that matters.
#pragma once
#include "meradb/backend.h"

namespace meradb {

class BackendCloser {
public:
    explicit BackendCloser(Backend& backend) : backend_(backend) {}
    ~BackendCloser() {
        if (!done_) {
            try {
                backend_.close();
            } catch (...) {
            }
        }
    }
    void close() {
        done_ = true;
        backend_.close();
    }
    BackendCloser(const BackendCloser&) = delete;
    BackendCloser& operator=(const BackendCloser&) = delete;

private:
    Backend& backend_;
    bool done_ = false;
};

}  // namespace meradb
