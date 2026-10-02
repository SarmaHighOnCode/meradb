// cpp/include/meradb/pytext.h
//
// The few Python `str` behaviours the shell depends on, for UTF-8 text: strip() / rstrip() / split()
// with Python's Unicode idea of whitespace (so a trailing no-break space still ends a statement, as it
// does in the Python shell), an ASCII-only lower(), and length / ljust counted in characters.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace meradb::pytext {

// str.isspace() for one code point (the 29 characters Python 3 treats as whitespace).
bool isSpace(char32_t codePoint);

// Decodes the UTF-8 character starting at text[at] (at < text.size()) and returns its length in
// bytes (at least 1). A malformed sequence decodes as U+FFFD and consumes ONE byte.
std::size_t decode(const std::string& text, std::size_t at, char32_t& codePoint);

std::string lstrip(const std::string& text);
std::string rstrip(const std::string& text);
std::string strip(const std::string& text);

// str.split() with no argument: runs of whitespace separate words, no empty words.
std::vector<std::string> split(const std::string& text);

// str.lower() for ASCII letters only; every other byte is left alone.
std::string lowerAscii(std::string text);

// len(str): the number of characters (UTF-8 lead bytes).
std::size_t length(const std::string& text);

// str.ljust(width): pads with spaces up to `width` CHARACTERS.
std::string ljust(const std::string& text, std::size_t width);

bool startsWith(const std::string& text, const std::string& prefix);
bool endsWith(const std::string& text, const std::string& suffix);

}  // namespace meradb::pytext
