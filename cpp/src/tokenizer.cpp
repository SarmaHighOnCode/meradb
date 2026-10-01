// cpp/src/tokenizer.cpp
#include "meradb/tokenizer.h"
#include "meradb/errors.h"
#include <cctype>
#include <cstdlib>
#include <stdexcept>
#include <set>
#include <unordered_set>

namespace meradb {

namespace {
const std::unordered_set<std::string>& keywordSet() {
    static const std::unordered_set<std::string> kw = {
        "BANAO", "HATAO", "SUDHARO", "JODO", "SAAF", "SIKODO", "TABLE", "TABLES",
        "DATABASE", "COLUMN", "ISTEMAL", "BATAO", "VIEW", "VIEWS",
        "MUKHYA", "KUNJI", "ZAROORI", "ANOKHA", "WARNA", "SANDARBH", "SHART", "NAYA_NAAM",
        "DAALO", "MEIN", "MAAN", "DIKHAO", "SE", "JAHAN", "BADLO", "RAKHO", "MITAO",
        "KRAM", "SEEDHA", "ULTA", "SIRF", "ALAG", "SAMOOH", "JINKA", "MILAO",
        "BAAYAN", "DAHINA", "DONO", "SAMAAN", "PAR", "KAHO", "SAMJHAO",
        "SHURU", "PAKKA", "WAPAS",
        "JAISA", "BEECH", "AUR", "YA", "NAHI", "HAI", "KHALI", "SACH", "JHOOTH",
        "SANYUKT", "SAAJHA", "CHHODKAR", "AGAR", "TAB", "KHATAM", "TAKRAAV",
        // Phase 2 words (users/triggers/procedures): reserved now, exactly like
        // Python, so a name valid here is valid there. No grammar uses them yet.
        "USER", "GUPT", "ADHIKAR", "DO", "KO", "SAB", "TRIGGER", "PEHLE", "BAAD", "PROCEDURE", "CHALAO",
    };
    return kw;
}

std::string toUpper(std::string s) {
    for (auto& c : s) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string toLower(std::string s) {
    for (auto& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

Tokenizer::Tokenizer(std::string text) : text_(std::move(text)) {}

char Tokenizer::peek(int offset) const {
    size_t p = pos_ + static_cast<size_t>(offset);
    return p < text_.size() ? text_[p] : '\0';
}

char Tokenizer::advance() {
    char c = text_[pos_++];
    if (c == '\n') { ++line_; col_ = 1; }
    else if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++col_;  // count characters, not UTF-8 bytes (Python's str)
    return c;
}

void Tokenizer::skipWhitespaceAndComments() {
    while (pos_ < text_.size()) {
        char c = peek();
        if (c == '\xEF' && peek(1) == '\xBB' && peek(2) == '\xBF') {
            // UTF-8 BOM, 3 bytes — skip as raw bytes (not through advance's
            // line/col tracking, since it's not a real character).
            // Python sees the BOM as one character and counts it as a column.
            pos_ += 3;
            ++col_;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) { advance(); continue; }
        if (c == '-' && peek(1) == '-') {
            while (pos_ < text_.size() && peek() != '\n') advance();
            continue;
        }
        break;
    }
}

[[noreturn]] void Tokenizer::error(const std::string& msg) const {
    throw TokenizerError(msg + " (line " + std::to_string(line_) + ", col " + std::to_string(col_) + ")");
}

Token Tokenizer::readWord() {
    int startLine = line_, startCol = col_;
    size_t start = pos_;
    while (pos_ < text_.size() && (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) advance();
    std::string word = text_.substr(start, pos_ - start);
    std::string upper = toUpper(word);
    Token t;
    t.line = startLine; t.col = startCol;
    t.start = static_cast<int>(start); t.end = static_cast<int>(pos_);
    if (keywordSet().count(upper)) {
        t.type = TokenType::Keyword;
        t.textValue = upper;
    } else {
        t.type = TokenType::Ident;
        t.textValue = toLower(word);
    }
    return t;
}

Token Tokenizer::readNumber() {
    int startLine = line_, startCol = col_;
    size_t start = pos_;
    while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
    bool isFloat = false;
    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
        isFloat = true;
        advance();  // consume '.'
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(peek()))) advance();
    }
    std::string text = text_.substr(start, pos_ - start);
    Token t;
    t.type = TokenType::Number;
    t.textValue = text;
    t.isFloat = isFloat;
    t.line = startLine; t.col = startCol;
    t.start = static_cast<int>(start); t.end = static_cast<int>(pos_);
    if (isFloat) {
        // strtod (not stod): an over-long literal rounds to inf / 0.0 the
        // way Python's float(text) does, instead of throwing out_of_range.
        t.doubleValue = std::strtod(text.c_str(), nullptr);
    } else {
        // Python ints are unbounded and the 8-byte INT limit is only hit
        // later in coerce; int64_t cannot hold the literal at all, so the
        // same limit is reported here as a MeraDB error.
        try {
            t.intValue = std::stoll(text);
        } catch (const std::out_of_range&) {
            throw TokenizerError("Number " + text + " INT ke liye bahut bada hai (8-byte limit) (line " +
                                 std::to_string(startLine) + ", col " + std::to_string(startCol) + ")");
        }
    }
    return t;
}

Token Tokenizer::readString() {
    int startLine = line_, startCol = col_;
    size_t start = pos_;
    advance();  // opening '
    std::string value;
    while (true) {
        if (pos_ >= text_.size()) error("String band nahi hui -- closing ' missing hai");
        char c = advance();
        if (c == '\'') {
            if (peek() == '\'') { value += '\''; advance(); continue; }  // '' -> '
            break;
        }
        value += c;
    }
    Token t;
    t.type = TokenType::String;
    t.textValue = value;
    t.line = startLine; t.col = startCol;
    t.start = static_cast<int>(start); t.end = static_cast<int>(pos_);
    return t;
}

std::vector<Token> Tokenizer::tokenize() {
    std::vector<Token> tokens;
    while (true) {
        skipWhitespaceAndComments();
        if (pos_ >= text_.size()) {
            Token eof;
            eof.type = TokenType::Eof;
            eof.line = line_; eof.col = col_;
            eof.start = static_cast<int>(pos_); eof.end = static_cast<int>(pos_);
            tokens.push_back(eof);
            break;
        }
        char c = peek();
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            tokens.push_back(readWord());
        } else if (std::isdigit(static_cast<unsigned char>(c))) {
            tokens.push_back(readNumber());
        } else if (c == '\'') {
            tokens.push_back(readString());
        } else {
            int startLine = line_, startCol = col_;
            size_t start = pos_;
            std::string two = text_.substr(pos_, 2);
            static const std::set<std::string> twoChar = {"<=", ">=", "!=", "<>"};
            std::string symbol;
            if (twoChar.count(two)) {
                advance(); advance();
                symbol = (two == "<>") ? "!=" : two;
            } else {
                static const std::string oneChar = "(),;*=<>+-/%.";
                if (oneChar.find(c) == std::string::npos) {
                    // Python formats the character with repr(): '@', "'" or '\\'
                    // A non-ASCII character is shown whole (all of its UTF-8 bytes), as Python's repr does for
                    // printable characters; escaping of unprintable ones (e.g. '\xa0') is not reproduced.
                    unsigned char lead = static_cast<unsigned char>(c);
                    size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
                    std::string character = text_.substr(pos_, length);
                    std::string shown = (c == '\\') ? std::string("'\\\\'") : "'" + character + "'";
                    error("Ye character samajh nahi aaya: " + shown);
                }
                advance();
                symbol = std::string(1, c);
            }
            Token t;
            t.type = TokenType::Symbol;
            t.textValue = symbol;
            t.line = startLine; t.col = startCol;
            t.start = static_cast<int>(start); t.end = static_cast<int>(pos_);
            tokens.push_back(t);
        }
    }
    return tokens;
}

std::vector<Token> tokenize(const std::string& text) {
    return Tokenizer(text).tokenize();
}

}  // namespace meradb
