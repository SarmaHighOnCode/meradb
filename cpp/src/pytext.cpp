// cpp/src/pytext.cpp -- see pytext.h.
#include "meradb/pytext.h"
#include <algorithm>
#include <cstdint>
#include <limits>

namespace meradb::pytext {

bool isSpace(char32_t c) {
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

std::size_t decode(const std::string& text, std::size_t at, char32_t& codePoint) {
    // The well-formed UTF-8 sequences Python's strict decoder accepts: no overlong forms (C0, C1, E0 80..9F,
    // F0 80..8F), no surrogates (ED A0..BF), nothing above U+10FFFF (F4 90.., F5..FF). Anything else, or a
    // sequence cut short by the end of the text, is ONE invalid byte: it decodes as U+FFFD and consumes one byte.
    const auto byteAt = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byteAt(at);
    if (lead < 0x80) {
        codePoint = lead;
        return 1;
    }
    std::size_t extra = 0;
    unsigned secondLow = 0x80, secondHigh = 0xBF;
    char32_t value = 0;
    if (lead >= 0xC2 && lead <= 0xDF) {
        extra = 1;
        value = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        extra = 2;
        value = lead & 0x0Fu;
        if (lead == 0xE0) secondLow = 0xA0;
        if (lead == 0xED) secondHigh = 0x9F;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        extra = 3;
        value = lead & 0x07u;
        if (lead == 0xF0) secondLow = 0x90;
        if (lead == 0xF4) secondHigh = 0x8F;
    } else {
        codePoint = 0xFFFD;
        return 1;
    }
    if (text.size() - at <= extra) {  // not enough bytes left (this also keeps every read in range)
        codePoint = 0xFFFD;
        return 1;
    }
    for (std::size_t k = 1; k <= extra; ++k) {
        const unsigned next = byteAt(at + k);
        const unsigned low = k == 1 ? secondLow : 0x80u;
        const unsigned high = k == 1 ? secondHigh : 0xBFu;
        if (next < low || next > high) {
            codePoint = 0xFFFD;
            return 1;
        }
        value = (value << 6) | (next & 0x3Fu);
    }
    codePoint = value;
    return extra + 1;
}

std::string lstrip(const std::string& text) {
    std::size_t start = 0;
    while (start < text.size()) {
        char32_t cp = 0;
        const std::size_t length = decode(text, start, cp);
        if (!isSpace(cp)) break;
        start += length;
    }
    return text.substr(start);
}

std::string rstrip(const std::string& text) {
    std::size_t end = text.size();
    while (end > 0) {
        // Find where the last character starts: step back over at most three continuation bytes.
        std::size_t start = end - 1;
        while (start > 0 && end - start < 4 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) --start;
        char32_t cp = 0;
        std::size_t length = decode(text, start, cp);
        if (start + length != end) {  // not one whole character: treat the last byte on its own
            start = end - 1;
            cp = 0xFFFD;
        }
        if (!isSpace(cp)) break;
        end = start;
    }
    return text.substr(0, end);
}

std::string strip(const std::string& text) { return lstrip(rstrip(text)); }

std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> words;
    std::string word;
    std::size_t at = 0;
    while (at < text.size()) {
        char32_t cp = 0;
        const std::size_t length = decode(text, at, cp);
        if (isSpace(cp)) {
            if (!word.empty()) words.push_back(word);
            word.clear();
        } else {
            word.append(text, at, length);
        }
        at += length;
    }
    if (!word.empty()) words.push_back(word);
    return words;
}

namespace {

struct LowerRange {
    std::uint32_t lo, hi;
    std::uint32_t step;
    std::int32_t delta;
};
struct CaseClassRange {
    std::uint32_t lo, hi;
    int kind;  // 1 = case-ignorable, 2 = cased
};

// kLowerRanges and kCaseClasses: generated, see cpp/tests/gen_lower_table.py.
#include "lower_table.inc"

struct WordRange {
    std::uint32_t lo, hi;
};
// kWordRanges: generated, see cpp/tests/gen_word_table.py.
#include "word_table.inc"

constexpr int kCaseIgnorable = 1;
constexpr int kCased = 2;

int caseClass(char32_t cp) {
    const auto* end = kCaseClasses + sizeof(kCaseClasses) / sizeof(kCaseClasses[0]);
    const auto* it = std::upper_bound(kCaseClasses, end, static_cast<std::uint32_t>(cp),
                                      [](std::uint32_t value, const CaseClassRange& r) { return value < r.lo; });
    if (it == kCaseClasses) return 0;
    --it;
    return cp <= it->hi ? it->kind : 0;
}

// The single-character lowercase of `cp` (itself when it has none).
char32_t lowerOne(char32_t cp) {
    const auto* end = kLowerRanges + sizeof(kLowerRanges) / sizeof(kLowerRanges[0]);
    const auto* it = std::upper_bound(kLowerRanges, end, static_cast<std::uint32_t>(cp),
                                      [](std::uint32_t value, const LowerRange& r) { return value < r.lo; });
    if (it == kLowerRanges) return cp;
    --it;
    if (cp > it->hi || (cp - it->lo) % it->step != 0) return cp;
    return static_cast<char32_t>(static_cast<std::int64_t>(cp) + it->delta);
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

struct Unit {
    char32_t cp;
    std::size_t at, size;
    bool valid;
};

}  // namespace

// CPython's unicode_lower: each character through its single-character lowercase mapping, U+0130 becomes
// "i" + U+0307, and capital sigma becomes final sigma at the end of a word (handle_capital_sigma): the nearest
// character before it that is not case-ignorable is cased, and the nearest one after it that is not
// case-ignorable is not cased (or there is none). Malformed bytes are copied through untouched.
std::string lower(const std::string& text) {
    std::vector<Unit> units;
    for (std::size_t at = 0; at < text.size();) {
        char32_t cp = 0;
        const std::size_t size = decode(text, at, cp);
        units.push_back({cp, at, size, !(cp == 0xFFFD && size == 1)});
        at += size;
    }
    const auto classOf = [&](std::size_t i) { return units[i].valid ? caseClass(units[i].cp) : 0; };

    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < units.size(); ++i) {
        const Unit& u = units[i];
        if (!u.valid) {
            out.append(text, u.at, u.size);
        } else if (u.cp == 0x3A3) {
            bool isFinal = false;
            std::size_t before = i;
            while (before > 0 && classOf(before - 1) == kCaseIgnorable) --before;
            if (before > 0 && classOf(before - 1) == kCased) {
                std::size_t after = i + 1;
                while (after < units.size() && classOf(after) == kCaseIgnorable) ++after;
                isFinal = after == units.size() || classOf(after) != kCased;
            }
            appendUtf8(out, isFinal ? 0x3C2 : 0x3C3);
        } else if (u.cp == 0x130) {
            out += "i\xCC\x87";  // i + COMBINING DOT ABOVE
        } else {
            const char32_t low = lowerOne(u.cp);
            if (low == u.cp) out.append(text, u.at, u.size);
            else appendUtf8(out, low);
        }
    }
    return out;
}

std::size_t length(const std::string& text) {
    std::size_t n = 0;
    for (std::size_t at = 0; at < text.size(); ++n) {
        char32_t cp = 0;
        at += decode(text, at, cp);
    }
    return n;
}

std::string ljust(const std::string& text, std::size_t width) {
    const std::size_t have = length(text);
    return have >= width ? text : text + std::string(width - have, ' ');
}

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool isWordChar(char32_t codePoint) {
    const auto* end = kWordRanges + sizeof(kWordRanges) / sizeof(kWordRanges[0]);
    const auto* it = std::upper_bound(kWordRanges, end, static_cast<std::uint32_t>(codePoint),
                                      [](std::uint32_t value, const WordRange& r) { return value < r.lo; });
    if (it == kWordRanges) return false;
    --it;
    return codePoint <= it->hi;
}

namespace {
// First code point ("zero") of every run of ten decimal digits (Unicode category Nd, Unicode 15.0 like the other
// tables). Python's int() accepts any of them; the value is the offset within the run.
const std::uint32_t kDigitZeros[] = {
    0x30, 0x660, 0x6F0, 0x7C0, 0x966, 0x9E6, 0xA66, 0xAE6, 0xB66, 0xBE6, 0xC66, 0xCE6, 0xD66, 0xDE6, 0xE50, 0xED0,
    0xF20, 0x1040, 0x1090, 0x17E0, 0x1810, 0x1946, 0x19D0, 0x1A80, 0x1A90, 0x1B50, 0x1BB0, 0x1C40, 0x1C50, 0xA620,
    0xA8D0, 0xA900, 0xA9D0, 0xA9F0, 0xAA50, 0xABF0, 0xFF10, 0x104A0, 0x10D30, 0x11066, 0x110F0, 0x11136, 0x111D0,
    0x112F0, 0x11450, 0x114D0, 0x11650, 0x116C0, 0x11730, 0x118E0, 0x11950, 0x11C50, 0x11D50, 0x11DA0, 0x11F50,
    0x16A60, 0x16AC0, 0x16B50, 0x1D7CE, 0x1D7D8, 0x1D7E2, 0x1D7EC, 0x1D7F6, 0x1E140, 0x1E2F0, 0x1E4F0, 0x1E950,
    0x1FBF0};
}  // namespace

int decimalDigit(char32_t codePoint) {
    const auto* end = kDigitZeros + sizeof(kDigitZeros) / sizeof(kDigitZeros[0]);
    const auto* it = std::upper_bound(kDigitZeros, end, static_cast<std::uint32_t>(codePoint));
    if (it == kDigitZeros) return -1;
    --it;
    const std::uint32_t offset = static_cast<std::uint32_t>(codePoint) - *it;
    return offset < 10 ? static_cast<int>(offset) : -1;
}

std::optional<long long> parseInt(const std::string& raw) {
    // int() strips str.isspace() characters except U+001C..U+001F (checked against CPython for every code point).
    struct Ch {
        char32_t cp;
        std::size_t length;
    };
    std::vector<Ch> chars;
    for (std::size_t at = 0; at < raw.size();) {
        char32_t cp = 0;
        const std::size_t length = decode(raw, at, cp);
        chars.push_back(Ch{cp, length});
        at += length;
    }
    const auto stripped = [](const Ch& c) { return isSpace(c.cp) && !(c.cp >= 0x1C && c.cp <= 0x1F); };
    std::size_t first = 0, last = chars.size();
    while (first < last && stripped(chars[first])) ++first;
    while (last > first && stripped(chars[last - 1])) --last;
    if (first == last) return std::nullopt;
    bool negative = false;
    if (chars[first].cp == '+' || chars[first].cp == '-') {
        negative = chars[first].cp == '-';
        ++first;
    }
    constexpr long long kMax = std::numeric_limits<long long>::max();
    long long value = 0;
    bool lastWasDigit = false;
    for (std::size_t i = first; i < last; ++i) {
        const int d = decimalDigit(chars[i].cp);
        if (d >= 0) {
            value = value > (kMax - d) / 10 ? kMax : value * 10 + d;  // saturates
            lastWasDigit = true;
        } else if (chars[i].cp == '_' && lastWasDigit && i + 1 < last) {
            lastWasDigit = false;
        } else {
            return std::nullopt;
        }
    }
    if (!lastWasDigit) return std::nullopt;
    return negative ? -value : value;
}

}  // namespace meradb::pytext
