// cpp/src/aggregates.cpp -- mirrors meradb/aggregates.py
#include "meradb/aggregates.h"
#include "meradb/errors.h"
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace meradb {
using namespace ast;

namespace {

constexpr double TWO_POW_63 = 9223372036854775808.0;

bool isInt(const Value& v) { return std::holds_alternative<int64_t>(v.data); }
bool isDouble(const Value& v) { return std::holds_alternative<double>(v.data); }
bool isBool(const Value& v) { return std::holds_alternative<bool>(v.data); }

// Exact sign of (i - d) for a non-NaN double, like Python's int/float compare.
int cmpIntDouble(int64_t i, double d) {
    if (d >= TWO_POW_63) return -1;
    if (d < -TWO_POW_63) return 1;
    double t = std::trunc(d);
    auto ti = static_cast<int64_t>(t);
    if (i != ti) return i < ti ? -1 : 1;
    double frac = d - t;
    return frac > 0 ? -1 : (frac < 0 ? 1 : 0);
}

// Python's `a < b` as min()/max() use it. bool counts as an int there (a
// SACH/JHOOTH column compares JHOOTH < SACH); numbers compare exactly
// across int/float; NaN compares false. Python raises TypeError for
// unorderable pairs (text vs number...); here that is an ExecutionError.
bool pyLess(const Value& a, const Value& b) {
    auto numeric = [](const Value& v) { return isInt(v) || isDouble(v) || isBool(v); };
    if (numeric(a) && numeric(b)) {
        auto asInt = [](const Value& v) {
            return isBool(v) ? int64_t{std::get<bool>(v.data) ? 1 : 0} : std::get<int64_t>(v.data);
        };
        if (!isDouble(a) && !isDouble(b)) return asInt(a) < asInt(b);
        if (isDouble(a) && isDouble(b)) return std::get<double>(a.data) < std::get<double>(b.data);
        double d = isDouble(a) ? std::get<double>(a.data) : std::get<double>(b.data);
        if (std::isnan(d)) return false;
        return isDouble(b) ? cmpIntDouble(asInt(a), d) < 0 : cmpIntDouble(asInt(b), d) > 0;
    }
    if (std::holds_alternative<std::string>(a.data) && std::holds_alternative<std::string>(b.data))
        return std::get<std::string>(a.data) < std::get<std::string>(b.data);
    if (std::holds_alternative<Date>(a.data) && std::holds_alternative<Date>(b.data))
        return std::get<Date>(a.data).ordinal < std::get<Date>(b.data).ordinal;
    throw ExecutionError(pyRepr(formatValue(a)) + " aur " + pyRepr(formatValue(b)) +
                         " ko compare nahi kar sakte (alag types)");
}

// Python's built-in sum() over ints/floats (CPython 3.12+): an exact integer
// phase, then -- from the first float on -- a float phase with Neumaier
// compensated summation for float items (ints are added plainly).
Value pySum(const std::vector<Value>& values, const std::string& label) {
    size_t i = 0;
    int64_t intTotal = 0;
    for (; i < values.size() && isInt(values[i]); ++i) {
        int64_t y = std::get<int64_t>(values[i].data);
        if ((y > 0 && intTotal > std::numeric_limits<int64_t>::max() - y) ||
            (y < 0 && intTotal < std::numeric_limits<int64_t>::min() - y))
            throw ExecutionError(label + ": KUL ka result INT ke liye bahut bada hai (8-byte limit)");
        intTotal += y;
    }
    if (i == values.size()) return Value(intTotal);

    // first float: int + float -> float, then compensated float summation
    double total = static_cast<double>(intTotal) + std::get<double>(values[i].data);
    double c = 0.0;
    for (++i; i < values.size(); ++i) {
        if (isDouble(values[i])) {
            double x = std::get<double>(values[i].data);
            double t = total + x;
            if (std::fabs(total) >= std::fabs(x))
                c += (total - t) + x;
            else
                c += (x - t) + total;
            total = t;
        } else {
            total += static_cast<double>(std::get<int64_t>(values[i].data));
        }
    }
    if (c != 0.0 && std::isfinite(c)) total += c;
    return Value(total);
}

}  // namespace

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
        double sum = isInt(total) ? static_cast<double>(std::get<int64_t>(total.data)) : std::get<double>(total.data);
        return Value(sum / static_cast<double>(values.size()));
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
