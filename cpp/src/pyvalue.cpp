// cpp/src/pyvalue.cpp
#include "meradb/pyvalue.h"
#include "meradb/errors.h"
#include <cmath>
#include <functional>
#include <string>

namespace meradb {

namespace {

// A number as a Python dict key sees it: integral values (bool, int, or a
// float with no fraction inside the int64 range) fold to one int64.
bool integralKey(const Value& v, int64_t& out) {
    if (isBoolValue(v)) { out = std::get<bool>(v.data) ? 1 : 0; return true; }
    if (isIntValue(v)) { out = std::get<int64_t>(v.data); return true; }
    if (isDoubleValue(v)) {
        double d = std::get<double>(v.data);
        if (std::isfinite(d) && d == std::floor(d) && d >= -TWO_POW_63 && d < TWO_POW_63) {
            out = static_cast<int64_t>(d);
            return true;
        }
    }
    return false;
}

bool isNumeric(const Value& v) { return isIntValue(v) || isDoubleValue(v) || isBoolValue(v); }

int64_t asInt(const Value& v) {
    return isBoolValue(v) ? int64_t{std::get<bool>(v.data) ? 1 : 0} : std::get<int64_t>(v.data);
}

// Sign of (a - b) for two numbers (bool as int); `unordered` set for NaN.
int cmpNumbers(const Value& a, const Value& b, bool& unordered) {
    unordered = false;
    if (!isDoubleValue(a) && !isDoubleValue(b)) {
        int64_t x = asInt(a), y = asInt(b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (isDoubleValue(a) && isDoubleValue(b)) {
        double x = std::get<double>(a.data), y = std::get<double>(b.data);
        if (std::isnan(x) || std::isnan(y)) { unordered = true; return 0; }
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    double d = isDoubleValue(a) ? std::get<double>(a.data) : std::get<double>(b.data);
    if (std::isnan(d)) { unordered = true; return 0; }
    return isDoubleValue(b) ? cmpIntDouble(asInt(a), d) : -cmpIntDouble(asInt(b), d);
}

int bitLength(uint64_t x) {
    int n = 0;
    while (x) { ++n; x >>= 1; }
    return n;
}

}  // namespace

int cmpIntDouble(int64_t i, double d) {
    if (d >= TWO_POW_63) return -1;
    if (d < -TWO_POW_63) return 1;
    double t = std::trunc(d);
    auto ti = static_cast<int64_t>(t);  // exact: |t| < 2^63
    if (i != ti) return i < ti ? -1 : 1;
    double frac = d - t;
    return frac > 0 ? -1 : (frac < 0 ? 1 : 0);
}

double pyTrueDivide(int64_t a, int64_t b) {
    constexpr uint64_t LIMIT = uint64_t{1} << 53;
    const bool negative = (a < 0) != (b < 0);
    const uint64_t ua = a < 0 ? uint64_t{0} - static_cast<uint64_t>(a) : static_cast<uint64_t>(a);
    const uint64_t ub = b < 0 ? uint64_t{0} - static_cast<uint64_t>(b) : static_cast<uint64_t>(b);
    // both exactly representable: one IEEE division is already correctly rounded
    if (ua <= LIMIT && ub <= LIMIT) return static_cast<double>(a) / static_cast<double>(b);
    if (ua == 0) return negative ? -0.0 : 0.0;

    uint64_t q = ua / ub, r = ua % ub;
    uint64_t mantissa;
    int exponent;
    if (q >= LIMIT) {
        // too many integer bits: keep the top 53, round on the rest (+ r/ub)
        int shift = bitLength(q) - 53;
        mantissa = q >> shift;
        uint64_t low = q & ((uint64_t{1} << shift) - 1);
        uint64_t half = uint64_t{1} << (shift - 1);
        bool up = low > half || (low == half && (r != 0 || (mantissa & 1)));
        mantissa += up ? 1 : 0;
        exponent = shift;
    } else {
        // long division for fraction bits until the mantissa has 53 bits
        mantissa = q;
        exponent = 0;
        while (mantissa < (uint64_t{1} << 52)) {
            r <<= 1;  // r < ub <= 2^63, so this cannot overflow
            mantissa <<= 1;
            if (r >= ub) { r -= ub; mantissa |= 1; }
            --exponent;
        }
        r <<= 1;
        bool guard = r >= ub;
        if (guard) r -= ub;
        bool up = guard && (r != 0 || (mantissa & 1));
        mantissa += up ? 1 : 0;
    }
    double v = std::ldexp(static_cast<double>(mantissa), exponent);
    return negative ? -v : v;
}

bool pyEquals(const Value& a, const Value& b) {
    if (isNumeric(a) && isNumeric(b)) {
        bool unordered;
        int c = cmpNumbers(a, b, unordered);
        return !unordered && c == 0;
    }
    if (a.isNull() || b.isNull()) return a.isNull() && b.isNull();
    if (isTextValue(a) && isTextValue(b)) return std::get<std::string>(a.data) == std::get<std::string>(b.data);
    if (isDateValue(a) && isDateValue(b)) return std::get<Date>(a.data) == std::get<Date>(b.data);
    return false;
}

size_t PyValueHash::operator()(const Value& v) const {
    int64_t i;
    if (integralKey(v, i)) return std::hash<int64_t>{}(i);
    if (isDoubleValue(v)) return std::hash<double>{}(std::get<double>(v.data)) ^ static_cast<size_t>(0x9e3779b97f4a7c15ULL);
    if (isTextValue(v)) return std::hash<std::string>{}(std::get<std::string>(v.data));
    if (isDateValue(v)) return std::hash<int32_t>{}(std::get<Date>(v.data).ordinal) ^ static_cast<size_t>(0x51ed270b27a8c3d1ULL);
    return static_cast<size_t>(0x2545f4914f6cdd1dULL);  // KHALI
}

size_t PyValuesHash::operator()(const std::vector<Value>& vs) const {
    size_t h = vs.size();
    PyValueHash one;
    for (const auto& v : vs) h ^= one(v) + static_cast<size_t>(0x9e3779b97f4a7c15ULL) + (h << 6) + (h >> 2);
    return h;
}

bool PyValuesEq::operator()(const std::vector<Value>& a, const std::vector<Value>& b) const {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!pyEquals(a[i], b[i])) return false;
    return true;
}

bool pyLess(const Value& a, const Value& b) {
    if (isNumeric(a) && isNumeric(b)) {
        bool unordered;
        int c = cmpNumbers(a, b, unordered);
        return !unordered && c < 0;
    }
    if (isTextValue(a) && isTextValue(b)) return std::get<std::string>(a.data) < std::get<std::string>(b.data);
    if (isDateValue(a) && isDateValue(b)) return std::get<Date>(a.data).ordinal < std::get<Date>(b.data).ordinal;
    throw ExecutionError(pyRepr(formatValue(a)) + " aur " + pyRepr(formatValue(b)) +
                         " ko compare nahi kar sakte (alag types)");
}

}  // namespace meradb
