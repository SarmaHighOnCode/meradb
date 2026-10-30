// cpp/src/query_limits.cpp -- see query_limits.h.
#include "meradb/query_limits.h"
#include "meradb/errors.h"
#include <chrono>
#include <cstdint>

namespace meradb::limits {

std::atomic<bool> g_inUse{false};

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kTicksBetweenClockReads = 256;

struct State {
    Limits limits;
    Clock::time_point deadline;
    std::size_t used = 0;
    std::uint32_t ticks = 0;
    const char* over = nullptr;  // once a time limit has passed: its message, for every later tick
};

thread_local State* t_state = nullptr;

std::string megabytes(std::size_t bytes) { return std::to_string(bytes / (1024 * 1024)) + " MB"; }

[[noreturn]] void refuseTime(const State& state) {
    if (state.over != nullptr && state.over[0] == 's') throw ExecutionError("Server band ho raha hai -- query rok di gayi");
    throw ExecutionError("Query ka time khatam (limit " + std::to_string(static_cast<long long>(state.limits.seconds)) +
                         " second) -- query ko chhota karo (JAHAN, SIRF), ya server par MERADB_MAX_QUERY_SECONDS badhao");
}

}  // namespace

Scope::Scope(const Limits& limits) {
    g_inUse.store(true, std::memory_order_relaxed);
    auto* state = new State;
    state->limits = limits;
    if (limits.seconds > 0)
        state->deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(limits.seconds));
    delete t_state;
    t_state = state;
}

Scope::~Scope() {
    delete t_state;
    t_state = nullptr;
}

bool armed() { return t_state != nullptr; }
std::size_t usedBytes() { return t_state != nullptr ? t_state->used : 0; }
void restoreBytes(std::size_t bytes) {
    if (t_state != nullptr) t_state->used = bytes;
}

void tickSlow() {
    State* state = t_state;
    if (state == nullptr) return;
    if (state->over != nullptr) refuseTime(*state);
    if (++state->ticks < kTicksBetweenClockReads) return;
    state->ticks = 0;
    if (state->limits.stop != nullptr && state->limits.stop->load(std::memory_order_relaxed)) {
        state->over = "stop";
        refuseTime(*state);
    }
    if (state->limits.seconds > 0 && Clock::now() >= state->deadline) {
        state->over = "time";
        refuseTime(*state);
    }
}

void chargeSlow(std::size_t bytes) {
    State* state = t_state;
    if (state == nullptr || state->limits.memoryBytes == 0) return;
    state->used += bytes;
    if (state->used > state->limits.memoryBytes)
        throw ExecutionError("Query bahut zyada memory maang rahi hai (limit " + megabytes(state->limits.memoryBytes) +
                             ") -- JAHAN ya SIRF lagao, ya server par MERADB_MAX_QUERY_MEMORY_MB badhao");
}

void checkTextSlow(std::size_t bytes) {
    const State* state = t_state;
    if (state == nullptr || state->limits.valueBytes == 0 || bytes <= state->limits.valueBytes) return;
    throw ExecutionError("Text value bahut badi ho rahi hai (limit " + megabytes(state->limits.valueBytes) + ")");
}

void checkVariablesSlow(std::size_t bytes) {
    const State* state = t_state;
    if (state == nullptr || state->limits.valueBytes == 0 || bytes <= state->limits.valueBytes * 4) return;
    throw ExecutionError("@variables mein bahut zyada data ho gaya (limit " + megabytes(state->limits.valueBytes * 4) + ")");
}

}  // namespace meradb::limits
