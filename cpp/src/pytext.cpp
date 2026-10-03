// cpp/src/pytext.cpp -- see pytext.h.
#include "meradb/pytext.h"
#include <algorithm>
#include <cstdint>

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

}  // namespace meradb::pytext
