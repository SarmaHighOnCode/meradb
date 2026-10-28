// cpp/src/wb_width.cpp -- see wb_width.h.
#include "meradb/wb_width.h"
#include <cstddef>

namespace meradb::wb {

namespace {

struct CodepointInterval {
    std::uint32_t first, last;
};

#include "wb_width_tables.inc"

template <std::size_t N>
bool inTable(std::uint32_t ucs, const CodepointInterval (&table)[N]) {
    if (ucs < table[0].first || ucs > table[N - 1].last) return false;
    std::size_t lo = 0, hi = N;   // [lo, hi)
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (ucs > table[mid].last) lo = mid + 1;
        else if (ucs < table[mid].first) hi = mid;
        else return true;
    }
    return false;
}

bool isControl(std::uint32_t ucs) {
    if (ucs == 0) return true;
    if (ucs < 32) return ucs != 10;   // a line feed is not a control character here
    return ucs >= 0x7f && ucs < 0xa0;
}

// One code point from `s` at `start`: the length is the one the lead byte announces; a continuation byte, a missing
// tail or a lead byte beyond F7 is one invalid byte (false). The continuation bytes themselves are not checked.
bool eatCodepoint(const std::string& s, std::size_t start, std::size_t* end, std::uint32_t* ucs) {
    const unsigned char c0 = static_cast<unsigned char>(s[start]);
    auto cont = [&](std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(s[i]) & 0x3F); };
    if ((c0 & 0x80) == 0) {
        *ucs = c0;
        *end = start + 1;
        return true;
    }
    if ((c0 & 0xE0) == 0xC0 && start + 1 < s.size()) {
        *ucs = (static_cast<std::uint32_t>(c0 & 0x1F) << 6) + cont(start + 1);
        *end = start + 2;
        return true;
    }
    if ((c0 & 0xF0) == 0xE0 && start + 2 < s.size()) {
        *ucs = (static_cast<std::uint32_t>(c0 & 0x0F) << 12) + (cont(start + 1) << 6) + cont(start + 2);
        *end = start + 3;
        return true;
    }
    if ((c0 & 0xF8) == 0xF0 && start + 3 < s.size()) {
        *ucs = (static_cast<std::uint32_t>(c0 & 0x07) << 18) + (cont(start + 1) << 12) + (cont(start + 2) << 6) + cont(start + 3);
        *end = start + 4;
        return true;
    }
    *end = start + 1;
    return false;
}

}  // namespace

int codepointWidth(std::uint32_t ucs) {
    if (isControl(ucs)) return 0;
    if (inTable(ucs, kZeroWidthIntervals)) return 0;
    if (ucs >= 0x0300 && inTable(ucs, kWideIntervals)) return 2;
    return 1;
}

int displayWidth(const std::string& utf8) {
    int width = 0;
    std::size_t at = 0;
    while (at < utf8.size()) {
        std::uint32_t ucs = 0;
        if (!eatCodepoint(utf8, at, &at, &ucs)) continue;
        width += codepointWidth(ucs);
    }
    return width;
}

}  // namespace meradb::wb
