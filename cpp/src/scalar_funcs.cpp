// cpp/src/scalar_funcs.cpp -- UPPER, LOWER, LENGTH, TRIM, SUBSTRING, CONCAT, ROUND, YEAR, DATEDIFF, CAST, ...
#include "meradb/scalar_funcs.h"
#include "meradb/errors.h"
#include "meradb/pytext.h"
#include "meradb/pyvalue.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>

namespace meradb {

namespace {

const ScalarSpec kSpecs[] = {
    {"UPPER", "UPPER", 1, 1},
    {"LOWER", "LOWER", 1, 1},
    {"LENGTH", "LENGTH", 1, 1},
    {"CHAR_LENGTH", "LENGTH", 1, 1},
    {"TRIM", "TRIM", 1, 1},
    {"LTRIM", "LTRIM", 1, 1},
    {"RTRIM", "RTRIM", 1, 1},
    {"SUBSTRING", "SUBSTRING", 2, 3},
    {"SUBSTR", "SUBSTRING", 2, 3},
    {"CONCAT", "CONCAT", 1, -1},
    {"REPLACE", "REPLACE", 3, 3},
    {"LEFT", "LEFT", 2, 2},
    {"RIGHT", "RIGHT", 2, 2},
    {"ROUND", "ROUND", 1, 2},
    {"FLOOR", "FLOOR", 1, 1},
    {"CEIL", "CEIL", 1, 1},
    {"CEILING", "CEIL", 1, 1},
    {"ABS", "ABS", 1, 1},
    {"MOD", "MOD", 2, 2},
    {"POWER", "POWER", 2, 2},
    {"POW", "POWER", 2, 2},
    {"SQRT", "SQRT", 1, 1},
    {"NOW", "NOW", 0, 0},
    {"CURDATE", "CURDATE", 0, 0},
    {"CURRENT_DATE", "CURDATE", 0, 0},
    {"YEAR", "YEAR", 1, 1},
    {"MONTH", "MONTH", 1, 1},
    {"DAY", "DAY", 1, 1},
    {"DATEDIFF", "DATEDIFF", 2, 2},
    {"DATE_FORMAT", "DATE_FORMAT", 2, 2},
    {"NULLIF", "NULLIF", 2, 2},
};

constexpr int64_t I64_MAX = std::numeric_limits<int64_t>::max();
constexpr int64_t I64_MIN = std::numeric_limits<int64_t>::min();

std::string shown(const Value& v) { return pyRepr(formatValue(v)); }

// ---- argument helpers ----

bool isNumber(const Value& v) { return isIntValue(v) || isDoubleValue(v); }

double asDouble(const Value& v) {
    return isIntValue(v) ? static_cast<double>(std::get<int64_t>(v.data)) : std::get<double>(v.data);
}

void needNumber(const Value& v, const char* fn) {
    if (!isNumber(v)) throw ExecutionError(std::string(fn) + " ko number chahiye, " + shown(v) + " mila");
}

int64_t needInt(const Value& v, const char* fn) {
    if (!isIntValue(v)) throw ExecutionError(std::string(fn) + " ko whole number chahiye, " + shown(v) + " mila");
    return std::get<int64_t>(v.data);
}

// ---- UTF-8 characters ----

// Byte offset of every character, plus the end of the text as a last entry.
std::vector<size_t> charStarts(const std::string& s) {
    std::vector<size_t> starts;
    size_t at = 0;
    while (at < s.size()) {
        starts.push_back(at);
        char32_t cp = 0;
        at += pytext::decode(s, at, cp);
    }
    starts.push_back(s.size());
    return starts;
}

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Upper-casing for ASCII, Latin-1, Greek and Cyrillic letters (the scripts with one-to-one simple mappings).
char32_t upperOf(char32_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c < 0x80) return c;
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 32;
    if (c == 0xFF) return 0x178;
    if (c == 0x3C2) return 0x3A3;
    if (c >= 0x3B1 && c <= 0x3C9) return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c >= 0x450 && c <= 0x45F) return c - 80;
    return c;
}

std::string upperText(const std::string& s) {
    std::string out;
    size_t at = 0;
    while (at < s.size()) {
        char32_t cp = 0;
        size_t len = pytext::decode(s, at, cp);
        if (cp == 0xFFFD && len == 1) out += s[at];  // malformed byte: copy through
        else appendUtf8(out, upperOf(cp));
        at += len;
    }
    return out;
}

// ---- dates ----

struct DateTime {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    Date date{0};
};

bool allDigits(const std::string& s, size_t pos, size_t n) {
    if (pos + n > s.size()) return false;
    for (size_t i = pos; i < pos + n; ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}

// A DATE, or 'YYYY-MM-DD' text, or 'YYYY-MM-DD HH:MM:SS' text (what NOW() returns).
DateTime dateTimeOf(const Value& v, const char* fn) {
    DateTime dt;
    if (std::holds_alternative<Date>(v.data)) {
        dt.date = std::get<Date>(v.data);
    } else if (isTextValue(v)) {
        const std::string& s = std::get<std::string>(v.data);
        if (s.size() > 10) {  // a date and a time: all of it must be valid, parseDateTime gives 'YYYY-MM-DD HH:MM:SS'
            const std::string full = parseDateTime(s);
            dt.hour = std::atoi(full.substr(11, 2).c_str());
            dt.minute = std::atoi(full.substr(14, 2).c_str());
            dt.second = std::atoi(full.substr(17, 2).c_str());
            dt.date = parseDate(full.substr(0, 10));
        } else {
            dt.date = parseDate(s);
        }
    } else {
        throw ExecutionError(std::string(fn) + " ko DATE (ya 'YYYY-MM-DD' text) chahiye, " + shown(v) + " mila");
    }
    std::string iso = dt.date.isoFormat();
    dt.year = std::atoi(iso.substr(0, 4).c_str());
    dt.month = std::atoi(iso.substr(5, 2).c_str());
    dt.day = std::atoi(iso.substr(8, 2).c_str());
    return dt;
}

const char* const kMonthNames[] = {"January", "February", "March",     "April",   "May",      "June",
                                   "July",    "August",   "September", "October", "November", "December"};
const char* const kWeekdayNames[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};

std::string padded(int n, const char* format) {
    char buf[32];
    std::snprintf(buf, sizeof buf, format, n);
    return buf;
}

std::string dateFormat(const DateTime& dt, const std::string& fmt) {
    int weekday = (dt.date.ordinal + 6) % 7;  // Monday == 0
    const int hour12 = dt.hour % 12 == 0 ? 12 : dt.hour % 12;
    std::string out;
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] != '%' || i + 1 >= fmt.size()) {
            out += fmt[i];
            continue;
        }
        char c = fmt[++i];
        switch (c) {
            case 'Y': out += padded(dt.year, "%04d"); break;
            case 'y': out += padded(dt.year % 100, "%02d"); break;
            case 'm': out += padded(dt.month, "%02d"); break;
            case 'd': out += padded(dt.day, "%02d"); break;
            case 'e': out += std::to_string(dt.day); break;
            case 'H': out += padded(dt.hour, "%02d"); break;
            case 'i': out += padded(dt.minute, "%02d"); break;
            case 's': case 'S': out += padded(dt.second, "%02d"); break;
            case 'c': out += std::to_string(dt.month); break;
            case 'k': out += std::to_string(dt.hour); break;
            case 'h': case 'I': out += padded(hour12, "%02d"); break;
            case 'l': out += std::to_string(hour12); break;
            case 'p': out += dt.hour < 12 ? "AM" : "PM"; break;
            case 'T': out += padded(dt.hour, "%02d") + ":" + padded(dt.minute, "%02d") + ":" + padded(dt.second, "%02d"); break;
            case 'r': out += padded(hour12, "%02d") + ":" + padded(dt.minute, "%02d") + ":" + padded(dt.second, "%02d") + (dt.hour < 12 ? " AM" : " PM"); break;
            case 'M': out += kMonthNames[dt.month - 1]; break;
            case 'b': out += std::string(kMonthNames[dt.month - 1]).substr(0, 3); break;
            case 'W': out += kWeekdayNames[weekday]; break;
            case 'a': out += std::string(kWeekdayNames[weekday]).substr(0, 3); break;
            case 'j': {
                Date jan1 = parseDate(padded(dt.year, "%04d") + "-01-01");
                out += padded(dt.date.ordinal - jan1.ordinal + 1, "%03d");
                break;
            }
            case '%': out += '%'; break;
            default: out += c; break;  // unknown specifier: the character itself, like MySQL
        }
    }
    return out;
}

std::string nowText() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

// ---- numbers ----

Value fromDoubleAsInt(double d) {
    if (std::isfinite(d) && d >= -9223372036854775808.0 && d < 9223372036854775808.0)
        return Value(static_cast<int64_t>(d));
    return Value(d);
}

Value roundValue(const Value& x, const Value* digitsArg) {
    needNumber(x, "ROUND");
    int64_t digits = digitsArg ? needInt(*digitsArg, "ROUND") : 0;
    if (isIntValue(x)) {
        int64_t v = std::get<int64_t>(x.data);
        if (digits >= 0) return x;
        if (digits < -18) return Value(int64_t{0});
        int64_t p = 1;
        for (int64_t i = 0; i < -digits; ++i) p *= 10;
        int64_t r = v % p;
        int64_t base = v - r;
        if ((r < 0 ? -r : r) * 2 >= p) {  // half away from zero
            int64_t step = v < 0 ? -p : p;
            if ((step > 0 && base > I64_MAX - step) || (step < 0 && base < I64_MIN - step))
                throw ExecutionError("ROUND ka result INT ke liye bahut bada hai (8-byte limit)");
            base += step;
        }
        return Value(base);
    }
    double d = std::get<double>(x.data);
    if (!std::isfinite(d) || digits > 300) return x;
    if (digits < -300) return Value(0.0);
    double scale = std::pow(10.0, static_cast<double>(digits));
    double y = std::round(d * scale) / scale;
    return Value(std::isfinite(y) ? y : d);
}

Value powerValue(const Value& a, const Value& b) {
    needNumber(a, "POWER");
    needNumber(b, "POWER");
    if (isIntValue(a) && isIntValue(b) && std::get<int64_t>(b.data) >= 0) {
        // exact integer power while it fits; the loop stops at once for the bases that cannot grow
        int64_t base = std::get<int64_t>(a.data), exp = std::get<int64_t>(b.data), result = 1;
        if (base == 0) return Value(int64_t{exp == 0 ? 1 : 0});
        if (base == 1) return Value(int64_t{1});
        if (base == -1) return Value(int64_t{exp % 2 == 0 ? 1 : -1});
        bool fits = true;
        for (int64_t i = 0; i < exp && fits; ++i) {
            if (base == I64_MIN || result > I64_MAX / (base < 0 ? -base : base)) fits = false;
            else result *= base;
        }
        if (fits) return Value(result);
    }
    double r = std::pow(asDouble(a), asDouble(b));
    if (std::isnan(r)) return Value();
    return Value(r);
}

bool parseFullDouble(const std::string& text, double& out) {
    std::string t = pytext::strip(text);
    if (t.empty()) return false;
    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);
    return end == t.c_str() + t.size() && std::isfinite(out);
}

[[noreturn]] void castFail(const Value& v, const char* type) {
    throw ExecutionError(shown(v) + " ko " + type + " mein cast nahi kar sakte");
}

std::string lowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

}  // namespace

const ScalarSpec* findScalarFunc(const std::string& upperName) {
    for (const auto& spec : kSpecs)
        if (upperName == spec.name) return &spec;
    return nullptr;
}

std::string scalarFunctionList() {
    std::string out;
    for (const auto& spec : kSpecs) out += (out.empty() ? "" : ", ") + std::string(spec.name);
    return out;
}

std::string valueAsText(const Value& value) { return formatValue(value); }

Value castValue(const Value& value, const std::string& type) {
    if (value.isNull()) return value;
    if (type == "INT") {
        if (isIntValue(value)) return value;
        if (isBoolValue(value)) return Value(int64_t{std::get<bool>(value.data) ? 1 : 0});
        if (isDoubleValue(value)) {
            double r = std::round(std::get<double>(value.data));
            if (!std::isfinite(r) || r < -9223372036854775808.0 || r >= 9223372036854775808.0)
                throw ExecutionError(shown(value) + " INT ke liye bahut bada hai (8-byte limit)");
            return Value(static_cast<int64_t>(r));
        }
        if (isTextValue(value)) {
            const std::string& s = std::get<std::string>(value.data);
            if (auto n = pytext::parseIntExact(s)) return Value(static_cast<int64_t>(*n));
            double d = 0;
            if (parseFullDouble(s, d) && std::fabs(d) < 9.2e18) return Value(static_cast<int64_t>(std::round(d)));
        }
        castFail(value, "INT");
    }
    if (type == "FLOAT") {
        if (isDoubleValue(value)) return value;
        if (isIntValue(value)) return Value(static_cast<double>(std::get<int64_t>(value.data)));
        if (isBoolValue(value)) return Value(std::get<bool>(value.data) ? 1.0 : 0.0);
        double d = 0;
        if (isTextValue(value) && parseFullDouble(std::get<std::string>(value.data), d)) return Value(d);
        castFail(value, "FLOAT");
    }
    if (type == "TEXT") return Value(valueAsText(value));
    if (type == "DATE") {
        if (std::holds_alternative<Date>(value.data)) return value;
        if (isTextValue(value)) return Value(dateTimeOf(value, "CAST").date);
        castFail(value, "DATE");
    }
    if (type == "DATETIME") {
        if (isTextValue(value)) {
            try {
                return Value(parseDateTime(std::get<std::string>(value.data)));
            } catch (const ExecutionError&) {
                castFail(value, "DATETIME");
            }
        }
        if (std::holds_alternative<Date>(value.data)) return Value(std::get<Date>(value.data).isoFormat() + " 00:00:00");
        castFail(value, "DATETIME");
    }
    if (type == "BOOL") {
        if (isBoolValue(value)) return value;
        if (isNumber(value)) return Value(asDouble(value) != 0);
        if (isTextValue(value)) {
            std::string s = lowerAscii(pytext::strip(std::get<std::string>(value.data)));
            if (s == "sach" || s == "true" || s == "1") return Value(true);
            if (s == "jhooth" || s == "false" || s == "0") return Value(false);
        }
        castFail(value, "BOOL");
    }
    throw ExecutionError("CAST ke liye type '" + type + "' nahi chalta (INT, FLOAT, TEXT, DATE, BOOL)");
}

Value callScalarFunc(const std::string& upperName, const std::vector<Value>& args) {
    const ScalarSpec* spec = findScalarFunc(upperName);
    if (spec == nullptr) throw ExecutionError("Function '" + upperName + "' nahi pata");
    const std::string name = spec->canonical;

    if (name == "NOW") return Value(nowText());
    if (name == "CURDATE") return Value(parseDate(nowText().substr(0, 10)));
    if (name == "NULLIF") {
        if (args[0].isNull()) return Value();
        if (args[1].isNull()) return args[0];
        return pyEquals(args[0], args[1]) ? Value() : args[0];
    }
    for (const auto& a : args)
        if (a.isNull()) return Value();  // KHALI in => KHALI out

    if (name == "UPPER") return Value(upperText(valueAsText(args[0])));
    if (name == "LOWER") return Value(pytext::lower(valueAsText(args[0])));
    if (name == "LENGTH") return Value(static_cast<int64_t>(pytext::length(valueAsText(args[0]))));
    if (name == "TRIM") return Value(pytext::strip(valueAsText(args[0])));
    if (name == "LTRIM") return Value(pytext::lstrip(valueAsText(args[0])));
    if (name == "RTRIM") return Value(pytext::rstrip(valueAsText(args[0])));
    if (name == "CONCAT") {
        std::string out;
        for (const auto& a : args) out += valueAsText(a);
        return Value(out);
    }
    if (name == "SUBSTRING") {
        std::string s = valueAsText(args[0]);
        int64_t start = needInt(args[1], "SUBSTRING");
        auto starts = charStarts(s);
        int64_t n = static_cast<int64_t>(starts.size()) - 1;
        int64_t begin;  // 0-based index of the first character
        if (start > 0) begin = start - 1;
        else if (start < 0) begin = n + start;
        else return Value(std::string());  // position 0 is before the first character
        if (begin < 0 || begin >= n) return Value(std::string());
        int64_t count = n - begin;
        if (args.size() == 3) {
            int64_t len = needInt(args[2], "SUBSTRING");
            if (len <= 0) return Value(std::string());
            if (len < count) count = len;
        }
        size_t from = starts[static_cast<size_t>(begin)], to = starts[static_cast<size_t>(begin + count)];
        return Value(s.substr(from, to - from));
    }
    if (name == "REPLACE") {
        std::string s = valueAsText(args[0]), from = valueAsText(args[1]), to = valueAsText(args[2]);
        if (from.empty()) return Value(s);
        std::string out;
        size_t at = 0;
        while (true) {
            size_t hit = s.find(from, at);
            if (hit == std::string::npos) break;
            out += s.substr(at, hit - at) + to;
            at = hit + from.size();
        }
        return Value(out + s.substr(at));
    }
    if (name == "LEFT" || name == "RIGHT") {
        std::string s = valueAsText(args[0]);
        int64_t want = needInt(args[1], name.c_str());
        auto starts = charStarts(s);
        int64_t n = static_cast<int64_t>(starts.size()) - 1;
        if (want <= 0) return Value(std::string());
        if (want > n) want = n;
        if (name == "LEFT") return Value(s.substr(0, starts[static_cast<size_t>(want)]));
        return Value(s.substr(starts[static_cast<size_t>(n - want)]));
    }
    if (name == "ROUND") return roundValue(args[0], args.size() > 1 ? &args[1] : nullptr);
    if (name == "FLOOR" || name == "CEIL") {
        needNumber(args[0], name.c_str());
        if (isIntValue(args[0])) return args[0];
        double d = std::get<double>(args[0].data);
        return fromDoubleAsInt(name == "FLOOR" ? std::floor(d) : std::ceil(d));
    }
    if (name == "ABS") {
        needNumber(args[0], "ABS");
        if (isIntValue(args[0])) {
            int64_t v = std::get<int64_t>(args[0].data);
            if (v == I64_MIN) throw ExecutionError("ABS ka result INT ke liye bahut bada hai (8-byte limit)");
            return Value(v < 0 ? -v : v);
        }
        return Value(std::fabs(std::get<double>(args[0].data)));
    }
    if (name == "POWER") return powerValue(args[0], args[1]);
    if (name == "SQRT") {
        needNumber(args[0], "SQRT");
        double d = asDouble(args[0]);
        return d < 0 ? Value() : Value(std::sqrt(d));
    }
    if (name == "YEAR") return Value(static_cast<int64_t>(dateTimeOf(args[0], "YEAR").year));
    if (name == "MONTH") return Value(static_cast<int64_t>(dateTimeOf(args[0], "MONTH").month));
    if (name == "DAY") return Value(static_cast<int64_t>(dateTimeOf(args[0], "DAY").day));
    if (name == "DATEDIFF") {
        DateTime a = dateTimeOf(args[0], "DATEDIFF"), b = dateTimeOf(args[1], "DATEDIFF");
        return Value(static_cast<int64_t>(a.date.ordinal) - static_cast<int64_t>(b.date.ordinal));
    }
    if (name == "DATE_FORMAT") return Value(dateFormat(dateTimeOf(args[0], "DATE_FORMAT"), valueAsText(args[1])));
    throw ExecutionError("Function '" + upperName + "' nahi pata");
}

}  // namespace meradb
