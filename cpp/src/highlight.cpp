// cpp/src/highlight.cpp
#include "meradb/highlight.h"
#include "meradb/aggregates.h"
#include "meradb/datatypes.h"
#include "meradb/pytext.h"
#include "meradb/scalar_funcs.h"
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

// Words that are only keywords in one spot of one statement (so the tokenizer keeps them plain identifiers and a
// column may still be called `first` or `limit`); the highlighter marks them wherever they appear, like OFFSET / USING.
bool isSoftKeyword(const std::string& upper) {
    static const char* const kWords[] = {
        "AUTO_INCREMENT", "AUTO_ANK",                                   // column attributes
        "LIMIT", "DIV",                                                 // English SIRF, integer division
        "REPLACE", "INTO", "BADAL",                                     // REPLACE INTO / BADAL DAALO MEIN
        "AS", "LIKE",                                                   // BANAO TABLE t AS DIKHAO ... / LIKE other
        "FOREIGN", "KEY", "CONSTRAINT", "REFERENCES",                   // table-level foreign keys
        "ON", "DELETE", "UPDATE", "CASCADE", "RESTRICT", "NO", "ACTION", "SET", "NULL",  // ON DELETE ... / ON UPDATE ...
        "CHANGE", "MODIFY", "FIRST", "AFTER",                           // SUDHARO TABLE t CHANGE / MODIFY / JODO ... AFTER
        "OR", "WITH", "CHECK", "OPTION", "LOCAL", "CASCADED",           // BANAO OR REPLACE VIEW ... WITH CHECK OPTION
        "ROLE", "GRANT", "PRIVILEGES", "SELECT", "INSERT", "ALL", "NONE",  // roles, WITH GRANT OPTION, English privileges, SET ROLE
        "SHOW", "GRANTS", "DATABASES", "TRIGGERS", "PROCEDURES", "STATUS",  // SHOW family / DIKHAO GRANTS ...
        "COLUMNS", "FIELDS", "CREATE", "FOR", "FROM",                   // SHOW COLUMNS FROM, SHOW CREATE TABLE, SHOW GRANTS FOR
        "SAVEPOINT", "RELEASE", "ROLLBACK", "TO", "TAK", "AUTOCOMMIT", "VARIABLES",  // SAVEPOINT / ROLLBACK TO / WAPAS TAK, SET autocommit
        "PREPARE", "EXECUTE", "DEALLOCATE",                             // PREPARE s FROM '..?..' / EXECUTE s USING @a / DEALLOCATE PREPARE s
        "FUNCTION", "RETURNS", "RETURN", "DETERMINISTIC", "IF", "NOT",  // BANAO FUNCTION f(..) RETURNS INT, HATAO .. IF EXISTS
        "DECLARE", "DEFAULT", "THEN", "ELSEIF", "ELSE", "END",          // procedural bodies: DECLARE, IF .. THEN .. ELSEIF .. ELSE .. END IF
        "WHILE", "JAB", "KARO", "LOOP", "REPEAT", "UNTIL", "LEAVE", "ITERATE",  // WHILE .. DO, JAB TAK .. KARO, LOOP, REPEAT .. UNTIL
        "SIGNAL", "CURSOR", "OPEN", "FETCH", "CLOSE", "HANDLER", "CONTINUE", "FOUND",  // SIGNAL 'msg', cursors, DECLARE CONTINUE HANDLER FOR NOT FOUND
        "IN", "OUT", "INOUT",                                           // procedure parameter modes
    };
    for (const char* w : kWords)
        if (upper == w) return true;
    return false;
}

bool classifyWord(const std::string& line, std::size_t start, std::size_t end, Kind& kind) {
    const std::string upper = upperAscii(line.substr(start, end - start));
    // line[end:].lstrip().startswith("("), without copying the rest of the line
    auto parenNext = [&]() {
        std::size_t at = end;
        while (at < line.size()) {
            char32_t cp = 0;
            const std::size_t len = pytext::decode(line, at, cp);
            if (!pytext::isSpace(cp)) break;
            at += len;
        }
        return at < line.size() && line[at] == '(';
    };
    if (upper == "SACH" || upper == "JHOOTH") kind = Kind::Boolean;
    else if (upper == "KHALI") kind = Kind::ConstantBuiltin;
    else if (isKeyword(upper)) kind = Kind::Keyword;
    else if (normalizeType(upper).has_value()) kind = Kind::Type;
    else if (upper == "OFFSET" || upper == "USING") kind = Kind::Keyword;  // SIRF n OFFSET m, MILAO t USING (c)
    else if (upper == "ENUM") kind = Kind::Type;                           // ENUM('a', 'b')
    else if (isSoftKeyword(upper) && !(upper == "REPLACE" && parenNext())) kind = Kind::Keyword;  // REPLACE(..) is the function
    else if (isAggregateName(upper) || findScalarFunc(upper) != nullptr || upper == "IFNULL" || upper == "CAST" ||
             upper == "EXISTS" || upper == "ANY" || upper == "SOME" || upper == "ALL") {
        if (!parenNext()) return false;
        // EXISTS / ANY / SOME / ALL (...) read as keywords, the functions as functions
        kind = (upper == "EXISTS" || upper == "ANY" || upper == "SOME" || upper == "ALL") ? Kind::Keyword : Kind::Function;
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

std::vector<Span> spans(const std::string& s) { return spans(s, 0, s.size()); }

std::vector<Span> spans(const std::string& s, std::size_t from, std::size_t to) { return spans(s, 0, from, to); }

std::vector<Span> spans(const std::string& s, std::size_t start, std::size_t from, std::size_t to) {
    std::vector<Span> out;
    const std::size_t n = s.size();
    std::size_t i = start < from ? start : from;
    while (i < n && i < to) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '-' && i + 1 < n && s[i + 1] == '-') {
            std::size_t e = s.find('\n', i);
            if (e == std::string::npos) e = n;
            if (e > from) out.push_back({i, e, Kind::Comment});
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
            if (j > from) out.push_back({i, j, Kind::String});
            i = j;
            continue;
        }
        if (isDigit(c)) {
            const std::size_t e = numberEnd(s, i);
            if (e != kNone) {
                if (e > from) out.push_back({i, e, Kind::Number});
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
            if (j > from && classifyWord(s, i, j, kind)) out.push_back({i, j, kind});
            i = j;
            continue;
        }
        if (i + 1 < n && ((c == '<' && (s[i + 1] == '=' || s[i + 1] == '>')) || ((c == '>' || c == '!') && s[i + 1] == '=') ||
                          (c == '|' && s[i + 1] == '|'))) {  // || is string concatenation
            if (i + 2 > from) out.push_back({i, i + 2, Kind::Operator});
            i += 2;
            continue;
        }
        if (c == '=' || c == '<' || c == '>' || c == '+' || c == '-' || c == '*' || c == '/' || c == '%') {
            if (i + 1 > from) out.push_back({i, i + 1, Kind::Operator});
            ++i;
            continue;
        }
        if (c == '(' || c == ')') {
            if (i + 1 > from) out.push_back({i, i + 1, Kind::Bracket});
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
