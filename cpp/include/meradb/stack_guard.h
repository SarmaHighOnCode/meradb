// cpp/include/meradb/stack_guard.h
//
// A byte-based guard against stack overflow from hostile (deeply nested) input.
// Counting nesting levels is not a bound: the stack cost of one level differs
// by shape, build type and platform. Instead the outermost entry point on a
// thread records the address of a local (StackBase); every recursive function
// then compares its own stack position against that and refuses once more than
// the budget has been used. Works on every platform whose stack grows downward
// (all supported ones) and for every build type.
#pragma once
#include "meradb/errors.h"
#include <cstddef>
#include <string>

namespace meradb {

// Upper limit of the budget, measured from the outermost StackBase on the
// thread. The budget actually used is min(this, half of the stack the thread
// really has left below that entry point), asked from the OS once per thread
// (Windows GetCurrentThreadStackLimits, Linux pthread_getattr_np, macOS
// pthread_get_stack*_np); it is this value itself when the OS cannot say. The
// other half is slack for the frames above the entry point and for the
// (non-recursive) work between two checks. So a 512 KB macOS thread or a
// 128 KB musl thread gets a smaller budget instead of overflowing first.
constexpr std::size_t kDefaultStackBudget = 512u * 1024u;

// Put one at the top of every entry point (parse, execute, protocol decode).
// Only the outermost one on a thread records the base; nested ones do nothing.
class StackBase {
public:
    StackBase();
    ~StackBase();
    StackBase(const StackBase&) = delete;
    StackBase& operator=(const StackBase&) = delete;

private:
    bool owner_;
};

std::size_t stackUsedBytes();  // since the outermost StackBase on this thread (0 if none)
bool stackExhausted();         // stackUsedBytes() > budget
std::size_t stackBudget();  // the override if set, else this thread's derived budget
// Process wide override; tests lower it to measure frame costs. 0 removes the override.
void setStackBudget(std::size_t bytes);
std::string stackLimitMessage();         // "Query bahut gehri (nested) hai (stack limit 512 KB)"

// Throws Err (a MeraDBError subclass taking a message) when the budget is spent.
template <class Err = ExecutionError>
inline void requireStack() {
    if (stackExhausted()) throw Err(stackLimitMessage());
}

}  // namespace meradb
