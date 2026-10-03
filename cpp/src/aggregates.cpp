// cpp/src/aggregates.cpp -- mirrors meradb/aggregates.py
#include "meradb/aggregates.h"
#include "meradb/errors.h"
#include "meradb/pyvalue.h"
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace meradb {
using namespace ast;

namespace {

bool isInt(const Value& v) { return isIntValue(v); }
bool isDouble(const Value& v) { return isDoubleValue(v); }

// Does an int fit a C `long`? The reference CPython is the Windows (MSC)
// build, where long is 32-bit, and sum()'s fast paths depend on it.
constexpr int64_t C_LONG_MAX = 2147483647;
constexpr int64_t C_LONG_MIN = -2147483647 - 1;
bool fitsCLong(int64_t v) { return v >= C_LONG_MIN && v <= C_LONG_MAX; }

int64_t checkedAdd(int64_t x, int64_t y, const std::string& label) {
    if ((y > 0 && x > std::numeric_limits<int64_t>::max() - y) ||
        (y < 0 && x < std::numeric_limits<int64_t>::min() - y))
        throw ExecutionError(label + ": KUL ka result INT ke liye bahut bada hai (8-byte limit)");
    return x + y;
}

// Python's built-in sum() over ints/floats, step for step as CPython 3.12
// (Windows build) runs it, starting from the int 0:
//   1. int fast path: while every item and the running total fit a C long;
//   2. float fast path (entered when a float arrives during step 1):
//      Neumaier-compensated for float items, plain `+=` for C-long ints;
//      the compensation is added once at the end if it is finite;
//   3. generic path (an int that does not fit a C long): plain Python `+`
//      for every remaining item, never returning to a fast path.
// Python ints are unbounded; an int64 overflow here is an ExecutionError.
Value pySum(const std::vector<Value>& values, const std::string& label) {
    enum class Phase { IntFast, FloatFast, Generic } phase = Phase::IntFast;
    int64_t intTotal = 0;       // IntFast, and Generic while the total is an int
    double floatTotal = 0.0;    // FloatFast, and Generic once the total is a float
    double compensation = 0.0;  // FloatFast
    bool genericIsFloat = false;

    auto genericAdd = [&](const Value& item) {
        if (!genericIsFloat && isInt(item)) {
            intTotal = checkedAdd(intTotal, std::get<int64_t>(item.data), label);
        } else if (!genericIsFloat) {
            floatTotal = static_cast<double>(intTotal) + std::get<double>(item.data);
            genericIsFloat = true;
        } else {
            floatTotal += isInt(item) ? static_cast<double>(std::get<int64_t>(item.data)) : std::get<double>(item.data);
        }
    };

    for (const auto& item : values) {
        switch (phase) {
            case Phase::IntFast:
                if (isInt(item)) {
                    int64_t b = std::get<int64_t>(item.data);
                    // both within 32 bits, so the int64 sum cannot overflow
                    if (fitsCLong(b) && fitsCLong(intTotal + b)) {
                        intTotal += b;
                        break;
                    }
                    phase = Phase::Generic;  // int + int stays an int
                    genericAdd(item);
                } else {
                    floatTotal = static_cast<double>(intTotal) + std::get<double>(item.data);
                    compensation = 0.0;
                    phase = Phase::FloatFast;
                }
                break;
            case Phase::FloatFast:
                if (isDouble(item)) {
                    double x = std::get<double>(item.data);
                    double t = floatTotal + x;
                    if (std::fabs(floatTotal) >= std::fabs(x))
                        compensation += (floatTotal - t) + x;
                    else
                        compensation += (x - t) + floatTotal;
                    floatTotal = t;
                } else if (fitsCLong(std::get<int64_t>(item.data))) {
                    floatTotal += static_cast<double>(std::get<int64_t>(item.data));
                } else {
                    if (compensation != 0.0 && std::isfinite(compensation)) floatTotal += compensation;
                    genericIsFloat = true;
                    phase = Phase::Generic;
                    genericAdd(item);
                }
                break;
            case Phase::Generic:
                genericAdd(item);
                break;
        }
    }
    if (phase == Phase::IntFast) return Value(intTotal);
    if (phase == Phase::FloatFast) {
        if (compensation != 0.0 && std::isfinite(compensation)) floatTotal += compensation;
        return Value(floatTotal);
    }
    return genericIsFloat ? Value(floatTotal) : Value(intTotal);
}

}  // namespace

bool isAggregateName(const std::string& upperWord) {
    // The keys of Python's aggregates.ALIASES.
    static const char* const names[] = {"GINO", "COUNT", "KUL", "SUM", "AUSAT", "AVG", "NYUNTAM", "MIN", "ADHIKTAM", "MAX"};
    for (const char* n : names)
        if (upperWord == n) return true;
    return false;
}

std::string canonicalName(const FuncCall& func) {
    static const std::unordered_map<std::string, std::string> aliases = {
        {"GINO", "GINO"},       {"COUNT", "GINO"},   {"KUL", "KUL"},         {"SUM", "KUL"},
        {"AUSAT", "AUSAT"},     {"AVG", "AUSAT"},    {"NYUNTAM", "NYUNTAM"}, {"MIN", "NYUNTAM"},
        {"ADHIKTAM", "ADHIKTAM"}, {"MAX", "ADHIKTAM"},
    };
    auto it = aliases.find(func.name);
    if (it == aliases.end())
        throw ExecutionError("Function '" + func.name +
                             "' nahi pata. Ye chalte hain: GINO, KUL, AUSAT, NYUNTAM, ADHIKTAM");
    if (dynamic_cast<const Star*>(func.arg.get()) && it->second != "GINO")
        throw ExecutionError(func.name + "(*) nahi chalta -- '*' sirf GINO(*) mein");
    return it->second;
}

Value computeAggregate(const FuncCall& func, const std::vector<Row>& groupRows) {
    std::string name = canonicalName(func);
    if (dynamic_cast<const Star*>(func.arg.get())) return Value(static_cast<int64_t>(groupRows.size()));

    std::vector<Value> values;
    for (const auto& row : groupRows) {
        Value v = evaluate(*func.arg, row);
        if (!v.isNull()) values.push_back(std::move(v));
    }

    if (name == "GINO") return Value(static_cast<int64_t>(values.size()));
    if (values.empty()) return Value();

    if (name == "KUL" || name == "AUSAT") {
        for (const auto& v : values)
            if (!isInt(v) && !isDouble(v))
                throw ExecutionError(exprLabel(func) + ": " + name + " sirf numbers ke saath chalta hai");
        Value total = pySum(values, exprLabel(func));
        if (name == "KUL") return total;
        auto n = static_cast<int64_t>(values.size());
        // Python: total / len(values) -- int / int is correctly-rounded true division
        if (isInt(total)) return Value(pyTrueDivide(std::get<int64_t>(total.data), n));
        return Value(std::get<double>(total.data) / static_cast<double>(n));
    }

    // NYUNTAM / ADHIKTAM: Python min()/max() keep the FIRST extreme value
    const Value* best = &values[0];
    for (size_t i = 1; i < values.size(); ++i) {
        bool better = name == "NYUNTAM" ? pyLess(values[i], *best) : pyLess(*best, values[i]);
        if (better) best = &values[i];
    }
    return *best;
}

}  // namespace meradb
