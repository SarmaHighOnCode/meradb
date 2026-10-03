// cpp/src/highlight.cpp
#include "meradb/highlight.h"
#include "meradb/aggregates.h"
#include "meradb/datatypes.h"
#include "meradb/pytext.h"
#include "meradb/tokenizer.h"

namespace meradb::highlight {

namespace {

constexpr std::size_t kNone = static_cast<std::size_t>(-1);

bool isDigit(unsigned char c) { return c >= '0' && c <= '9'; }
bool isAsciiAlpha(unsigned char c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isAsciiWord(unsigned char c) { return isAsciiAlpha(c) || isDigit(c); }

// Python's : is the character starting at s[at] a regex word character (letter/digit of any script, or '_')?
bool wordAt(const std::string& s, std::size_t at) {
    const unsigned char c = static_cast<unsigned char>(s[at]);
    if (c < 0x80) return isAsciiWord(c);
    char32_t cp = 0;
    pytext::decode(s, at, cp);
    return pytext::isWordChar(cp);
}

// Is the character that ends just before s[end] a word character?
bool wordBefore(const std::string& s, std::size_t end) {
    std::size_t start = end - 1;
    while (start > 0 && end - start < 4 && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) --start;
    char32_t cp = 0;
    if (pytext::decode(s, start, cp) != end - start) return false;  // malformed: not a word character
    return pytext::isWordChar(cp);
}

bool boundaryAfter(const std::string& s, std::size_t end) {
    return end >= s.size() || !wordAt(s, end);
}

std::size_t numberEnd(const std::string& s, std::size_t i) {
    if (i > 0 && wordBefore(s, i)) return kNone;  // no \b before the first digit
    std::size_t j = i;
    while (j < s.size() && isDigit(static_cast<unsigned char>(s[j]))) ++j;
    if (j + 1 < s.size() && s[j] == '.' && isDigit(static_cast<unsigned char>(s[j + 1]))) {
        std::size_t k = j + 1;
        while (k < s.size() && isDigit(static_cast<unsigned char>(s[k]))) ++k;
        if (boundaryAfter(s, k)) return k;
    }
    return boundaryAfter(s, j) ? j : kNone;
}

std::string upperAscii(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

bool classifyWord(const std::string& line, std::size_t start, std::size_t end, Kind& kind) {
    const std::string upper = upperAscii(line.substr(start, end - start));
    if (upper == "SACH" || upper == "JHOOTH") kind = Kind::Boolean;
    else if (upper == "KHALI") kind = Kind::ConstantBuiltin;
    else if (isKeyword(upper)) kind = Kind::Keyword;
    else if (normalizeType(upper).has_value()) kind = Kind::Type;
    else if (isAggregateName(upper)) {
        const std::string rest = pytext::lstrip(line.substr(end));
        if (rest.empty() || rest[0] != '(') return false;
        kind = Kind::Function;
    } else return false;
    return true;
}

}  // namespace

const char* kindName(Kind kind) {
    switch (kind) {
        case Kind::Keyword: return "keyword";
        case Kind::String: return "string";
        case Kind::Number: return "number";
        case Kind::Comment: return "comment";
        case Kind::Operator: return "operator";
        case Kind::Type: return "type";
        case Kind::Boolean: return "boolean";
        case Kind::ConstantBuiltin: return "constant.builtin";
        case Kind::Function: return "function";
        case Kind::Bracket: return "punctuation.bracket";
    }
    return "";
}

std::vector<Span> spans(const std::string& s) {
    std::vector<Span> out;
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '-' && i + 1 < n && s[i + 1] == '-') {
            std::size_t e = s.find('\n', i);
            if (e == std::string::npos) e = n;
            out.push_back({i, e, Kind::Comment});
            i = e;
            continue;
        }
        if (c == '\'') {
            std::size_t j = i + 1;
            while (j < n) {
                if (s[j] == '\'') {
                    if (j + 1 < n && s[j + 1] == '\'') { j += 2; continue; }
                    ++j;  // the closing quote
                    break;
                }
                ++j;
            }
            out.push_back({i, j, Kind::String});
            i = j;
            continue;
        }
        if (isDigit(c)) {
            const std::size_t e = numberEnd(s, i);
            if (e != kNone) {
                out.push_back({i, e, Kind::Number});
                i = e;
            } else {
                ++i;
            }
            continue;
        }
        if (isAsciiAlpha(c)) {
            std::size_t j = i + 1;
            while (j < n && isAsciiWord(static_cast<unsigned char>(s[j]))) ++j;
            Kind kind = Kind::Keyword;
            if (classifyWord(s, i, j, kind)) out.push_back({i, j, kind});
            i = j;
            continue;
        }
        if (i + 1 < n && ((c == '<' && (s[i + 1] == '=' || s[i + 1] == '>')) || ((c == '>' || c == '!') && s[i + 1] == '='))) {
            out.push_back({i, i + 2, Kind::Operator});
            i += 2;
            continue;
        }
        if (c == '=' || c == '<' || c == '>' || c == '+' || c == '-' || c == '*' || c == '/' || c == '%') {
            out.push_back({i, i + 1, Kind::Operator});
            ++i;
            continue;
        }
        if (c == '(' || c == ')') {
            out.push_back({i, i + 1, Kind::Bracket});
            ++i;
            continue;
        }
        ++i;
    }
    return out;
}

RichStyle richStyle(Kind kind) {
    switch (kind) {
        case Kind::Keyword: return {0xff79c6, true, false};
        case Kind::String: return {0xf1fa8c, false, false};
        case Kind::Number: return {0xbd93f9, false, false};
        case Kind::Comment: return {0x6272a4, false, true};
        case Kind::Operator: return {0xff79c6, false, false};
        case Kind::Type: return {0x8be9fd, false, false};
        case Kind::Boolean: return {0xbd93f9, false, false};
        case Kind::ConstantBuiltin: return {0xbd93f9, false, true};
        case Kind::Function: return {0x50fa7b, false, false};
        case Kind::Bracket: return {-1, false, false};
    }
    return {-1, false, false};
}

}  // namespace meradb::highlight
