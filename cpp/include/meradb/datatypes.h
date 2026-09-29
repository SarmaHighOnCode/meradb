// cpp/include/meradb/datatypes.h
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace meradb {

struct Date {
    int32_t ordinal;  // proleptic Gregorian ordinal, matches Python date.toordinal()
    int32_t toOrdinal() const { return ordinal; }
    std::string isoFormat() const;  // "YYYY-MM-DD"
    static Date fromOrdinal(int32_t ord) { return Date{ord}; }
    bool operator==(const Date& other) const { return ordinal == other.ordinal; }
};

struct Value {
    using Storage = std::variant<std::monostate, int64_t, double, std::string, bool, Date>;
    Storage data;

    Value() : data(std::monostate{}) {}
    explicit Value(int64_t v) : data(v) {}
    explicit Value(double v) : data(v) {}
    explicit Value(std::string v) : data(std::move(v)) {}
    explicit Value(bool v) : data(v) {}
    explicit Value(Date v) : data(v) {}

    bool isNull() const { return std::holds_alternative<std::monostate>(data); }
};

constexpr int64_t INT_MIN_VALUE = INT64_MIN;
constexpr int64_t INT_MAX_VALUE = INT64_MAX;

std::optional<std::string> normalizeType(const std::string& name);
Date parseDate(const std::string& text, const std::string& column = "");
Value coerce(const Value& value, const std::string& typeName, const std::string& column);
std::string formatValue(const Value& value);

// Python's repr() of a str (quote choice + escapes), used where Python error
// messages and labels write `{text!r}`.
std::string pyRepr(const std::string& s);

// Python's repr() of a float: shortest round-tripping digits, fixed notation
// for 1e-4 <= |x| < 1e16, exponent form otherwise (inf / nan spelled like Python).
std::string pyReprFloat(double d);

}  // namespace meradb
