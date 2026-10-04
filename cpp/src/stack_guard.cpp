// cpp/src/stack_guard.cpp
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1  // pthread_getattr_np (glibc and musl); must precede every system header
#endif
#include "meradb/stack_guard.h"
#include <algorithm>
#include <atomic>
#include <cstdint>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace meradb {

namespace {

thread_local std::uintptr_t g_base = 0;
// Budget derived for this thread by its outermost StackBase (see deriveBudget()).
thread_local std::size_t g_threadBudget = kDefaultStackBudget;
// Lowest address of this thread's stack, looked up once per thread (0 = unknown).
thread_local bool g_lowKnown = false;
thread_local std::uintptr_t g_low = 0;
// A process-wide override (tests); 0 = none, use the per-thread derived budget.
std::atomic<std::size_t> g_override{0};

// The address of a local, taken in a function that is never inlined, so it
// reflects the caller's depth rather than a register-promoted value.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
std::uintptr_t stackPosition() {
#if defined(__GNUC__) || defined(__clang__)
    return reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
#else
    volatile char probe = 0;
    return reinterpret_cast<std::uintptr_t>(&probe);
#endif
}

// Lowest address of the current thread's stack from the OS, or 0 when it cannot be asked.
std::uintptr_t queryStackLow() {
#if defined(_WIN32)
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    return static_cast<std::uintptr_t>(low);
#elif defined(__APPLE__)
    pthread_t self = pthread_self();
    void* high = pthread_get_stackaddr_np(self);  // macOS: the HIGH end of the stack
    std::size_t size = pthread_get_stacksize_np(self);
    if (high == nullptr || size == 0) return 0;
    return reinterpret_cast<std::uintptr_t>(high) - size;
#elif defined(__linux__)
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) != 0) return 0;
    void* low = nullptr;
    std::size_t size = 0;
    int rc = pthread_attr_getstack(&attr, &low, &size);
    pthread_attr_destroy(&attr);
    if (rc != 0 || size == 0) return 0;
    return reinterpret_cast<std::uintptr_t>(low);  // pthread_attr_getstack: the LOWEST address
#else
    return 0;
#endif
}

// min(default, half of the stack that is really left below `base`); the default
// when the OS cannot say. Half stays as slack for the frames above the entry
// point and for the unguarded stretch between two checks.
std::size_t deriveBudget(std::uintptr_t base) {
    if (!g_lowKnown) {
        g_low = queryStackLow();
        g_lowKnown = true;
    }
    if (g_low == 0 || base <= g_low) return kDefaultStackBudget;
    return std::min<std::size_t>(kDefaultStackBudget, static_cast<std::size_t>(base - g_low) / 2);
}

}  // namespace

StackBase::StackBase() : owner_(g_base == 0) {
    if (owner_) {
        g_base = stackPosition();
        g_threadBudget = deriveBudget(g_base);
    }
}

StackBase::~StackBase() {
    if (owner_) g_base = 0;
}

std::size_t stackUsedBytes() {
    if (g_base == 0) return 0;
    std::uintptr_t here = stackPosition();
    return here < g_base ? static_cast<std::size_t>(g_base - here) : 0;
}

bool stackExhausted() { return stackUsedBytes() > stackBudget(); }

std::size_t stackBudget() {
    std::size_t forced = g_override.load(std::memory_order_relaxed);
    return forced != 0 ? forced : g_threadBudget;
}

void setStackBudget(std::size_t bytes) { g_override.store(bytes, std::memory_order_relaxed); }

std::string stackLimitMessage() {
    return "Query bahut gehri (nested) hai (stack limit " + std::to_string(stackBudget() / 1024) + " KB)";
}

}  // namespace meradb
