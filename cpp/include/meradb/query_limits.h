// cpp/include/meradb/query_limits.h
//
// What one request may cost a server. A Scope on the thread that runs the request arms the limits; the engine calls
// tick() from every expression it evaluates and charge() for the rows it holds, and past a limit they throw an
// ExecutionError, which ends the statement like any other error (the same cleanup, the same rollback of a statement
// with triggers).
//
// Without a Scope (every embedded or --local Engine) nothing is limited. Until the first Scope in the process these
// calls are one relaxed load of a global, so a program that never serves pays nothing that can be measured.
//
// The memory figure is an estimate of what the rows hold, not the allocator's count: enough to stop a cross join long
// before it hurts. Marks give it back when a statement or a sub-select ends, so a correlated sub-select that runs once
// per row is not counted once per row.
#pragma once
#include "meradb/datatypes.h"
#include <atomic>
#include <cstddef>
#include <string>
#include <variant>

namespace meradb::limits {

struct Limits {
    double seconds = 0;           // of one request; 0 = no limit
    std::size_t memoryBytes = 0;  // estimated, for the rows of one request; 0 = no limit
    std::size_t valueBytes = 0;   // of one text value, and of a session's @variables four times that; 0 = no limit
    const std::atomic<bool>* stop = nullptr;  // once true, the request ends (the server is going down)
};

class Scope {
public:
    explicit Scope(const Limits& limits);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

extern std::atomic<bool> g_inUse;  // has any Scope existed in this process?

void tickSlow();
void chargeSlow(std::size_t bytes);
void checkTextSlow(std::size_t bytes);
void checkVariablesSlow(std::size_t bytes);
bool armed();                  // does this thread have a Scope?
std::size_t usedBytes();       // what is charged on this thread right now
void restoreBytes(std::size_t bytes);

// Call often: reads the clock once in a while. After a limit has passed it throws on every call, so nothing can catch
// the error once and carry on.
inline void tick() {
    if (g_inUse.load(std::memory_order_relaxed)) tickSlow();
}
inline void charge(std::size_t bytes) {
    if (g_inUse.load(std::memory_order_relaxed)) chargeSlow(bytes);
}
// A text value of this size is about to exist.
inline void checkText(std::size_t bytes) {
    if (g_inUse.load(std::memory_order_relaxed)) checkTextSlow(bytes);
}

inline std::size_t costOfValue(const Value& value) {
    constexpr std::size_t kCell = sizeof(Value) + 16;
    if (const auto* text = std::get_if<std::string>(&value.data)) return kCell + text->size();
    return kCell;
}
// A row: {key: value} (Row) or a plain list of cells.
template <typename Cells>
std::size_t costOfRow(const Cells& row) {
    std::size_t total = 64;
    for (const auto& cell : row) {
        if constexpr (std::is_same_v<std::decay_t<decltype(cell)>, Value>) {
            total += costOfValue(cell);
        } else {
            total += costOfValue(cell.second) + cell.first.size() + 48;  // the map's node and its key
        }
    }
    return total;
}
template <typename Cells>
void chargeRow(const Cells& row) {
    if (g_inUse.load(std::memory_order_relaxed) && armed()) chargeSlow(costOfRow(row));
}
template <typename Rows>
void chargeRows(const Rows& rows) {
    if (!g_inUse.load(std::memory_order_relaxed) || !armed()) return;
    for (const auto& row : rows) chargeSlow(costOfRow(row));
}
// @variables are kept between requests, so they count as a whole. `Map` is {name: Value}.
template <typename Map>
void checkVariables(const Map& variables) {
    if (!g_inUse.load(std::memory_order_relaxed) || !armed()) return;
    std::size_t total = 0;
    for (const auto& entry : variables) total += costOfValue(entry.second) + entry.first.size();
    checkVariablesSlow(total);
}

// Everything charged after this point is given back at the end of the scope.
class MemoryMark {
public:
    MemoryMark() : armed_(g_inUse.load(std::memory_order_relaxed) && armed()), at_(armed_ ? usedBytes() : 0) {}
    ~MemoryMark() {
        if (armed_) restoreBytes(at_);
    }
    MemoryMark(const MemoryMark&) = delete;
    MemoryMark& operator=(const MemoryMark&) = delete;

private:
    bool armed_;
    std::size_t at_;
};

}  // namespace meradb::limits
