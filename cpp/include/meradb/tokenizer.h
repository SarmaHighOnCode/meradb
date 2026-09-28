// cpp/include/meradb/tokenizer.h
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace meradb {

enum class TokenType { Keyword, Ident, Number, String, Symbol, Eof };

struct Token {
    TokenType type = TokenType::Eof;
    std::string textValue;   // upper-cased for Keyword/Symbol, lower-cased for Ident, raw for String
    int64_t intValue = 0;
    double doubleValue = 0.0;
    bool isFloat = false;
    int line = 1;
    int col = 1;
    int start = -1;  // byte offset into source text
    int end = -1;
};

class Tokenizer {
public:
    explicit Tokenizer(std::string text);
    std::vector<Token> tokenize();

private:
    std::string text_;
    size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;

    char peek(int offset = 0) const;
    char advance();
    void skipWhitespaceAndComments();
    Token readWord();
    Token readNumber();
    Token readString();
    [[noreturn]] void error(const std::string& msg) const;
};

std::vector<Token> tokenize(const std::string& text);

}  // namespace meradb
