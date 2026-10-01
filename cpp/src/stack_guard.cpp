// cpp/src/stack_guard.cpp
#include "meradb/stack_guard.h"
#include <atomic>
#include <cstdint>

namespace meradb {

namespace {

thread_local std::uintptr_t g_base = 0;
std::atomic<std::size_t> g_budget{kDefaultStackBudget};

// The address of a local, taken in a function that is never inlined, so it
// reflects the caller's depth rather than a register-promoted value.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
std::uintptr_t stackPosition() {
    volatile char probe = 0;
    return reinterpret_cast<std::uintptr_t>(&probe);
}

}  // namespace

StackBase::StackBase() : owner_(g_base == 0) {
    if (owner_) g_base = stackPosition();
}

StackBase::~StackBase() {
    if (owner_) g_base = 0;
}

std::size_t stackUsedBytes() {
    if (g_base == 0) return 0;
    std::uintptr_t here = stackPosition();
    return here < g_base ? static_cast<std::size_t>(g_base - here) : 0;
}

bool stackExhausted() { return stackUsedBytes() > g_budget.load(std::memory_order_relaxed); }

std::size_t stackBudget() { return g_budget.load(std::memory_order_relaxed); }

void setStackBudget(std::size_t bytes) { g_budget.store(bytes, std::memory_order_relaxed); }

std::string stackLimitMessage() {
    return "Query bahut gehri (nested) hai (stack limit " + std::to_string(stackBudget() / 1024) + " KB)";
}

}  // namespace meradb
