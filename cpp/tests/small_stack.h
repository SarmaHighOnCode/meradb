// cpp/tests/small_stack.h -- run a function on a thread whose stack size is chosen by the test.
#pragma once
#include <cstddef>
#include <functional>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

namespace meradb_test {

namespace detail {
#ifdef _WIN32
inline unsigned __stdcall trampoline(void* arg) {
    (*static_cast<std::function<void()>*>(arg))();
    return 0;
}
#else
inline void* trampoline(void* arg) {
    (*static_cast<std::function<void()>*>(arg))();
    return nullptr;
}
#endif
}  // namespace detail

// Runs `body` on a new thread with a stack of `bytes` and waits for it. `body` must not throw.
// Returns false if the thread could not be created.
inline bool runOnStack(std::size_t bytes, std::function<void()> body) {
#ifdef _WIN32
    HANDLE h = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, static_cast<unsigned>(bytes), detail::trampoline, &body,
                       STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
    if (h == nullptr) return false;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    return true;
#else
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, bytes);
    pthread_t thread;
    int rc = pthread_create(&thread, &attr, detail::trampoline, &body);
    pthread_attr_destroy(&attr);
    if (rc != 0) return false;
    pthread_join(thread, nullptr);
    return true;
#endif
}

}  // namespace meradb_test
