// cpp/include/meradb/pytext.h
//
// The few Python `str` behaviours the shell depends on, for UTF-8 text: strip() / rstrip() / split()
// with Python's Unicode idea of whitespace (so a trailing no-break space still ends a statement, as it
// does in the Python shell), Unicode lower(), and length / ljust counted in characters.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace meradb::pytext {

// str.isspace() for one code point (the 29 characters Python 3 treats as whitespace).
bool isSpace(char32_t codePoint);

// Decodes the UTF-8 character starting at text[at] (at < text.size()) and returns its length in
// bytes (at least 1). Valid means what Python's strict UTF-8 decoder accepts: overlong forms, surrogates
// (ED A0..BF), anything above U+10FFFF and stray continuation bytes are malformed. A malformed byte decodes
// as U+FFFD and consumes ONE byte (a genuine U+FFFD is told apart by its length of 3). Never reads outside
// the text. length(), strip(), split() and lower() all go through this function.
std::size_t decode(const std::string& text, std::size_t at, char32_t& codePoint);

std::string lstrip(const std::string& text);
std::string rstrip(const std::string& text);
std::string strip(const std::string& text);

// str.split() with no argument: runs of whitespace separate words, no empty words.
std::vector<std::string> split(const std::string& text);

// str.lower(): full Unicode lowercasing as CPython does it (generated tables, Unicode 15.0), including
// U+0130 -> "i" + U+0307 and the context-sensitive final sigma. Malformed bytes are copied through unchanged.
std::string lower(const std::string& text);

// len(str): the number of characters (each malformed byte counts as one).
std::size_t length(const std::string& text);

// str.ljust(width): pads with spaces up to `width` CHARACTERS.
std::string ljust(const std::string& text, std::size_t width);

bool startsWith(const std::string& text, const std::string& prefix);
bool endsWith(const std::string& text, const std::string& suffix);

}  // namespace meradb::pytext
