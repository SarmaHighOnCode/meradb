// cpp/src/datatypes.cpp
#include "meradb/datatypes.h"
#include "meradb/errors.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <unordered_map>

namespace meradb {

namespace {
const std::unordered_map<std::string, std::string>& aliasTable() {
    static const std::unordered_map<std::string, std::string> table = {
        {"INT", "INT"}, {"INTEGER", "INT"}, {"ANK", "INT"},
        {"FLOAT", "FLOAT"}, {"REAL", "FLOAT"}, {"DASHAMLAV", "FLOAT"},
        {"NUMBER", "FLOAT"}, {"NUMERIC", "FLOAT"}, {"DECIMAL", "FLOAT"},
        {"TEXT", "TEXT"}, {"STRING", "TEXT"}, {"VARCHAR", "TEXT"},
        {"VARCHAR2", "TEXT"}, {"CHAR", "TEXT"}, {"SHABD", "TEXT"},
        {"BOOL", "BOOL"}, {"BOOLEAN", "BOOL"}, {"HAAN_NA", "BOOL"},
        {"DATE", "DATE"}, {"TAREEKH", "DATE"},
    };
    return table;
}

std::string toUpper(std::string s) {
    for (auto& c : s) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

std::optional<std::string> normalizeType(const std::string& name) {
    // NOTE: real "VARCHAR(20)"-style names arrive here with the length
    // already stripped by the parser (same split Python's parser does
    // before calling datatypes.normalize_type) — this function only
    // resolves the bare type-name alias.
    auto it = aliasTable().find(toUpper(name));
    if (it == aliasTable().end()) return std::nullopt;
    return it->second;
}

Date parseDate(const std::string& text, const std::string& column) {
    std::tm tm{};
    int consumed = 0;
    if (sscanf(text.c_str(), "%d-%d-%d%n", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &consumed) != 3 ||
        static_cast<size_t>(consumed) != text.size()) {
        throw ExecutionError("'" + text + "' ek theek DATE nahi hai" +
                              (column.empty() ? "" : " (column: " + column + ")"));
    }
    // Convert Gregorian y-m-d to a proleptic ordinal (days since 0001-01-01,
    // matching Python's date.toordinal()). Days-before-year uses the
    // standard leap-year-aware formula.
    auto isLeap = [](int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; };
    static const int cumDays[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    int y = tm.tm_year, m = tm.tm_mon, d = tm.tm_mday;
    if (m < 1 || m > 12 || d < 1 || d > 31) {
        throw ExecutionError("'" + text + "' ek theek DATE nahi hai");
    }
    int64_t daysBeforeYear = 365LL * (y - 1) + (y - 1) / 4 - (y - 1) / 100 + (y - 1) / 400;
    int64_t dayOfYear = cumDays[m - 1] + d + (m > 2 && isLeap(y) ? 1 : 0);
    return Date{static_cast<int32_t>(daysBeforeYear + dayOfYear)};
}

Value coerce(const Value& value, const std::string& typeName, const std::string& column) {
    if (value.isNull()) return value;

    if (typeName == "INT") {
        // bool checked BEFORE int: Value never stores a bool as an int
        // itself (they're distinct variant alternatives), so this is just
        // an explicit rejection, matching Python's "isinstance(v, bool)"
        // guard against its int-subclass quirk.
        if (std::holds_alternative<bool>(value.data))
            throw ExecutionError("'" + column + "' INT hai, BOOL nahi milna chahiye");
        if (std::holds_alternative<int64_t>(value.data)) return value;
        if (std::holds_alternative<double>(value.data)) {
            double d = std::get<double>(value.data);
            if (d == std::floor(d)) return Value(static_cast<int64_t>(d));
        }
        throw ExecutionError("'" + column + "' ke liye ANK/INT chahiye");
    }
    if (typeName == "FLOAT") {
        if (std::holds_alternative<double>(value.data)) return value;
        if (std::holds_alternative<int64_t>(value.data))
            return Value(static_cast<double>(std::get<int64_t>(value.data)));
        throw ExecutionError("'" + column + "' ke liye DASHAMLAV/FLOAT chahiye");
    }
    if (typeName == "TEXT") {
        if (std::holds_alternative<std::string>(value.data)) return value;
        throw ExecutionError("'" + column + "' ke liye TEXT chahiye");
    }
    if (typeName == "BOOL") {
        if (std::holds_alternative<bool>(value.data)) return value;
        throw ExecutionError("'" + column + "' ke liye BOOL chahiye");
    }
    if (typeName == "DATE") {
        if (std::holds_alternative<Date>(value.data)) return value;
        if (std::holds_alternative<std::string>(value.data))
            return Value(parseDate(std::get<std::string>(value.data), column));
        throw ExecutionError("'" + column + "' ke liye TAREEKH/DATE chahiye");
    }
    throw ExecutionError("Anjaan type: " + typeName);
}

std::string formatValue(const Value& value) {
    if (value.isNull()) return "KHALI";
    if (std::holds_alternative<bool>(value.data)) return std::get<bool>(value.data) ? "SACH" : "JHOOTH";
    if (std::holds_alternative<int64_t>(value.data)) return std::to_string(std::get<int64_t>(value.data));
    if (std::holds_alternative<double>(value.data)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.10g", std::get<double>(value.data));
        return buf;
    }
    if (std::holds_alternative<std::string>(value.data)) return std::get<std::string>(value.data);
    if (std::holds_alternative<Date>(value.data)) return std::get<Date>(value.data).isoFormat();
    return "";
}

std::string Date::isoFormat() const {
    // Inverse of parseDate's ordinal math: walk the ordinal back to y-m-d.
    // Implemented with a simple day-counting loop for clarity, since dates
    // in this project's test data are always in a small, sane range.
    int64_t remaining = ordinal;
    int year = 1;
    auto isLeap = [](int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; };
    while (true) {
        int64_t daysInYear = isLeap(year) ? 366 : 365;
        if (remaining <= daysInYear) break;
        remaining -= daysInYear;
        ++year;
    }
    static const int monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int month = 1;
    for (int i = 0; i < 12; ++i) {
        int days = monthDays[i] + (i == 1 && isLeap(year) ? 1 : 0);
        if (remaining <= days) { month = i + 1; break; }
        remaining -= days;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, static_cast<int>(remaining));
    return buf;
}

}  // namespace meradb
