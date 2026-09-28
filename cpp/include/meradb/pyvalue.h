// cpp/include/meradb/pyvalue.h
//
// Python value semantics that the reference engine gets for free from the
// Python runtime and that the C++ port has to spell out: exact int/float
// comparison, `==` and hashing as dict/set keys use them (1 == 1.0 == SACH),
// min()/max() ordering, and correctly-rounded int / int true division.
// Shared by the evaluator, the aggregates and the engine (GROUP BY buckets,
// hash joins, DISTINCT, set operations).
#pragma once
#include "meradb/datatypes.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace meradb {

constexpr double TWO_POW_63 = 9223372036854775808.0;

inline bool isIntValue(const Value& v) { return std::holds_alternative<int64_t>(v.data); }
inline bool isDoubleValue(const Value& v) { return std::holds_alternative<double>(v.data); }
inline bool isBoolValue(const Value& v) { return std::holds_alternative<bool>(v.data); }
inline bool isTextValue(const Value& v) { return std::holds_alternative<std::string>(v.data); }
inline bool isDateValue(const Value& v) { return std::holds_alternative<Date>(v.data); }

// Exact sign of (i - d) for a non-NaN double, the way Python compares an
// int with a float (the int is never rounded).
int cmpIntDouble(int64_t i, double d);

// Python's `a / b` for two ints: the correctly rounded (half-to-even)
// double nearest the exact quotient. Exact on every compiler (no reliance on
// long double). b must not be 0.
double pyTrueDivide(int64_t a, int64_t b);

// Python `a == b`: numbers compare by value across int/float/bool
// (1 == 1.0 == SACH); text, dates and KHALI (None == None) need the same
// kind; different kinds are simply unequal. NaN is unequal to everything.
bool pyEquals(const Value& a, const Value& b);

// A hash consistent with pyEquals (equal values hash equal), for
// unordered containers keyed by values the way Python dicts/sets are.
struct PyValueHash {
    size_t operator()(const Value& v) const;
};
struct PyValueEq {
    bool operator()(const Value& a, const Value& b) const { return pyEquals(a, b); }
};
// Tuple keys (GROUP BY on several expressions, DISTINCT rows, ...).
struct PyValuesHash {
    size_t operator()(const std::vector<Value>& vs) const;
};
struct PyValuesEq {
    bool operator()(const std::vector<Value>& a, const std::vector<Value>& b) const;
};

// Python `a < b` as min()/max() use it: bool orders as an int, numbers
// compare exactly across int/float, NaN compares false. Throws
// ExecutionError for an unorderable pair (Python raises TypeError).
bool pyLess(const Value& a, const Value& b);

}  // namespace meradb
