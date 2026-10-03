// cpp/include/meradb/highlight.h
//
// Syntax highlighting for MeraDB queries (mirrors meradb/highlight.py). LENIENT on purpose: the strict tokenizer
// throws on `'unterminated`, which is exactly what the text looks like while you type. Spans are BYTE offsets
// into the line (Python's are characters; the golden generator converts), `kindName` gives the same names as
// Python's `kind` strings.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace meradb::highlight {

enum class Kind { Keyword, String, Number, Comment, Operator, Type, Boolean, ConstantBuiltin, Function, Bracket };

struct Span {
    std::size_t start;
    std::size_t end;  // exclusive
    Kind kind;
};

// "keyword" "string" "number" "comment" "operator" "type" "boolean" "constant.builtin" "function" "punctuation.bracket"
const char* kindName(Kind kind);

// Highlight spans for ONE line (no '\n'), in order, never overlapping.
std::vector<Span> spans(const std::string& line);
// Only the spans that overlap the byte range [from, to): the same spans spans(line) gives, filtered, but words
// outside the range are not classified and no span is built for them (for drawing a window of a very long line).
std::vector<Span> spans(const std::string& line, std::size_t from, std::size_t to);

// highlight.py's RICH_STYLES: rgb is 0xRRGGBB or -1 for "no colour".
struct RichStyle {
    int rgb;
    bool bold;
    bool italic;
};
RichStyle richStyle(Kind kind);

}  // namespace meradb::highlight
