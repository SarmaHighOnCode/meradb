// cpp/src/datatypes.cpp
#include "meradb/datatypes.h"
#include "meradb/errors.h"
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// Python's repr() of a str, for error messages: single quotes unless the
// text contains a ' and no ", then double quotes; backslash, the chosen
// quote and common control characters are escaped.
std::string pyRepr(const std::string& s) {
    char quote = (s.find('\'') != std::string::npos && s.find('"') == std::string::npos) ? '"' : '\'';
    std::string out(1, quote);
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == quote) { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else out += c;
    }
    out += quote;
    return out;
}

namespace {

// ---- proleptic Gregorian calendar helpers (same rules as CPython's datetime) ----
constexpr int32_t MAX_ORDINAL = 3652059;  // 9999-12-31
const int DAYS_IN_MONTH[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
const int DAYS_BEFORE_MONTH[] = {0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

bool isLeap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

int daysInMonth(int y, int m) { return (m == 2 && isLeap(y)) ? 29 : DAYS_IN_MONTH[m]; }

int64_t ymdToOrdinal(int y, int m, int d) {
    int64_t py = y - 1;
    int64_t daysBeforeYear = 365 * py + py / 4 - py / 100 + py / 400;
    return daysBeforeYear + DAYS_BEFORE_MONTH[m] + ((m > 2 && isLeap(y)) ? 1 : 0) + d;
}

bool digitsAt(const std::string& s, size_t pos, size_t n) {
    if (pos + n > s.size()) return false;
    for (size_t i = pos; i < pos + n; ++i)
        if (s[i] < '0' || s[i] > '9') return false;  // ASCII only, like fromisoformat
    return true;
}

int numberAt(const std::string& s, size_t pos, size_t n) {
    int v = 0;
    for (size_t i = pos; i < pos + n; ++i) v = v * 10 + (s[i] - '0');
    return v;
}

// Mirrors date.fromisoformat (Python 3.11+): YYYY-MM-DD, YYYYMMDD,
// YYYY-Www, YYYYWww, YYYY-Www-D, YYYYWwwD. Returns false on any bad input.
bool isoToOrdinal(const std::string& s, int64_t& ordinal) {
    const size_t n = s.size();
    if (!digitsAt(s, 0, 4)) return false;
    int year = numberAt(s, 0, 4);
    if (year < 1) return false;  // 0001..9999; 4 digits caps the top end
    const bool extended = n > 4 && s[4] == '-';
    size_t p = extended ? 5 : 4;

    if (p < n && s[p] == 'W') {
        ++p;
        if (!digitsAt(s, p, 2)) return false;
        int week = numberAt(s, p, 2);
        p += 2;
        int day = 1;
        if (p != n) {
            if (extended) {
                if (s[p] != '-') return false;
                ++p;
            }
            if (!digitsAt(s, p, 1) || p + 1 != n) return false;
            day = s[p] - '0';
        }
        int64_t jan1 = ymdToOrdinal(year, 1, 1);
        int64_t jan1Weekday = (jan1 + 6) % 7;  // Monday == 0
        bool has53 = jan1Weekday == 3 || (jan1Weekday == 2 && isLeap(year));
        if (week < 1 || week > 53 || (week == 53 && !has53)) return false;
        if (day < 1 || day > 7) return false;
        int64_t week1Monday = jan1 - jan1Weekday + (jan1Weekday > 3 ? 7 : 0);
        ordinal = week1Monday + (week - 1) * 7 + (day - 1);
        return ordinal >= 1 && ordinal <= MAX_ORDINAL;
    }

    int month, day;
    if (extended) {
        if (n != 10 || !digitsAt(s, 5, 2) || s[7] != '-' || !digitsAt(s, 8, 2)) return false;
        month = numberAt(s, 5, 2);
        day = numberAt(s, 8, 2);
    } else {
        if (n != 8 || !digitsAt(s, 4, 4)) return false;
        month = numberAt(s, 4, 2);
        day = numberAt(s, 6, 2);
    }
    if (month < 1 || month > 12) return false;
    if (day < 1 || day > daysInMonth(year, month)) return false;
    ordinal = ymdToOrdinal(year, month, day);
    return true;
}

// Floor division/modulo, so ordinal -> y/m/d never misbehaves for odd inputs.
void floorDivMod(int64_t a, int64_t b, int64_t& q, int64_t& r) {
    q = a / b;
    r = a % b;
    if (r < 0) { r += b; --q; }
}

std::string formatDouble(double d) {
    // Python: f"{v:.10g}", plus ".0" unless the text has '.', 'e' or 'n'.
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
    char buf[32];
    snprintf(buf, sizeof(buf), "%.10g", d);
    std::string text = buf;
    if (text.find_first_of(".en") == std::string::npos) text += ".0";
    return text;
}

std::string typeMismatch(const std::string& column, const std::string& typeName, const Value& value) {
    return "Column '" + column + "' " + typeName + " type ka hai, par value " + formatValue(value) + " mili";
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
    int64_t ordinal = 0;
    if (!isoToOrdinal(text, ordinal)) {
        std::string where = column.empty() ? "" : "Column '" + column + "': ";
        throw ExecutionError(where + pyRepr(text) + " valid DATE nahi hai -- 'YYYY-MM-DD' format chahiye");
    }
    return Date{static_cast<int32_t>(ordinal)};
}

Value coerce(const Value& value, const std::string& typeName, const std::string& column) {
    if (value.isNull()) return value;

    if (typeName == "INT") {
        // Value keeps bool and int64_t as distinct alternatives, so a bool
        // never matches here — the same result as Python's explicit
        // "not isinstance(v, bool)" guard against its int-subclass quirk.
        if (std::holds_alternative<int64_t>(value.data)) return value;
        if (std::holds_alternative<double>(value.data)) {
            double d = std::get<double>(value.data);
            if (std::isfinite(d) && d == std::floor(d)) {
                // INT is 8 bytes on disk; casting an out-of-range double is UB,
                // so range-check first. 2^63 is exactly representable.
                if (d < -9223372036854775808.0 || d >= 9223372036854775808.0) {
                    char buf[400];
                    snprintf(buf, sizeof(buf), "%.0f", d);  // exact integer text, like Python int(d)
                    throw ExecutionError("Column '" + column + "': " + buf +
                                         " INT ke liye bahut bada hai (8-byte limit)");
                }
                return Value(static_cast<int64_t>(d));
            }
        }
    } else if (typeName == "FLOAT") {
        if (std::holds_alternative<double>(value.data)) return value;
        if (std::holds_alternative<int64_t>(value.data))
            return Value(static_cast<double>(std::get<int64_t>(value.data)));
    } else if (typeName == "TEXT") {
        if (std::holds_alternative<std::string>(value.data)) return value;
    } else if (typeName == "BOOL") {
        if (std::holds_alternative<bool>(value.data)) return value;
    } else if (typeName == "DATE") {
        if (std::holds_alternative<Date>(value.data)) return value;
        if (std::holds_alternative<std::string>(value.data))
            return Value(parseDate(std::get<std::string>(value.data), column));
    }
    throw ExecutionError(typeMismatch(column, typeName, value));
}

std::string formatValue(const Value& value) {
    if (value.isNull()) return "KHALI";
    if (std::holds_alternative<bool>(value.data)) return std::get<bool>(value.data) ? "SACH" : "JHOOTH";
    if (std::holds_alternative<int64_t>(value.data)) return std::to_string(std::get<int64_t>(value.data));
    if (std::holds_alternative<double>(value.data)) return formatDouble(std::get<double>(value.data));
    if (std::holds_alternative<std::string>(value.data)) return std::get<std::string>(value.data);
    if (std::holds_alternative<Date>(value.data)) return std::get<Date>(value.data).isoFormat();
    return "";
}

std::string Date::isoFormat() const {
    // O(1) inverse of ymdToOrdinal, a port of CPython's _ord2ymd.
    int64_t n400, n100, n4, n1, n;
    floorDivMod(static_cast<int64_t>(ordinal) - 1, 146097, n400, n);
    int64_t year = n400 * 400 + 1;
    floorDivMod(n, 36524, n100, n);
    floorDivMod(n, 1461, n4, n);
    floorDivMod(n, 365, n1, n);
    year += n100 * 100 + n4 * 4 + n1;
    int month, day;
    if (n1 == 4 || n100 == 4) {
        year -= 1;
        month = 12;
        day = 31;
    } else {
        bool leap = n1 == 3 && (n4 != 24 || n100 == 3);
        month = static_cast<int>((n + 50) >> 5);
        int64_t preceding = DAYS_BEFORE_MONTH[month] + ((month > 2 && leap) ? 1 : 0);
        if (preceding > n) {
            month -= 1;
            preceding -= DAYS_IN_MONTH[month] + ((month == 2 && leap) ? 1 : 0);
        }
        day = static_cast<int>(n - preceding + 1);
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%04lld-%02d-%02d", static_cast<long long>(year), month, day);
    return buf;
}

// Python's repr(float): shortest round-tripping digits, fixed notation for
// 1e-4 <= |x| < 1e16, exponent form otherwise.
std::string pyReprFloat(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
    if (d == 0) return std::signbit(d) ? "-0.0" : "0.0";
    char buf[48];
    for (int prec = 1; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*e", prec - 1, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    std::string s = buf;
    bool neg = s[0] == '-';
    if (neg) s.erase(0, 1);
    size_t e = s.find('e');
    int exp10 = std::atoi(s.c_str() + e + 1);
    std::string digits;
    for (size_t i = 0; i < e; ++i)
        if (s[i] != '.') digits += s[i];
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    std::string out;
    if (exp10 >= -4 && exp10 < 16) {
        int decpt = exp10 + 1;
        if (decpt <= 0) out = "0." + std::string(static_cast<size_t>(-decpt), '0') + digits;
        else if (static_cast<size_t>(decpt) >= digits.size())
            out = digits + std::string(static_cast<size_t>(decpt) - digits.size(), '0') + ".0";
        else out = digits.substr(0, static_cast<size_t>(decpt)) + "." + digits.substr(static_cast<size_t>(decpt));
    } else {
        out = digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        std::string ex = std::to_string(std::abs(exp10));
        if (ex.size() < 2) ex = "0" + ex;
        out += std::string("e") + (exp10 < 0 ? "-" : "+") + ex;
    }
    return neg ? "-" + out : out;
}

}  // namespace meradb
