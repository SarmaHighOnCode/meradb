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

// Default budget, measured from the outermost StackBase on the thread. The
// smallest default thread stack we run on is 1 MB (MSVC); half of it is the
// budget, the other half is slack for the frames below the entry point and
// for the (non-recursive) work between two checks.
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
std::size_t stackBudget();
void setStackBudget(std::size_t bytes);  // process wide; tests lower it to measure frame costs
std::string stackLimitMessage();         // "Query bahut gehri (nested) hai (stack limit 512 KB)"

// Throws Err (a MeraDBError subclass taking a message) when the budget is spent.
template <class Err = ExecutionError>
inline void requireStack() {
    if (stackExhausted()) throw Err(stackLimitMessage());
}

}  // namespace meradb
