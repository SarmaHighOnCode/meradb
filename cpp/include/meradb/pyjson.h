// cpp/include/meradb/pyjson.h
//
// JSON exactly the way Python's `json` module writes and reads it, so a
// Python peer and a C++ peer understand each other on the wire (nlohmann
// alone can do neither of the two things below):
//
//   dump   json.dumps(obj): separators ", " and ": ", ensure_ascii (every
//          non-ASCII character as a lowercase \uXXXX escape, astral ones as
//          surrogate pairs), floats spelled like float.__repr__, and the bare
//          tokens Infinity / -Infinity / NaN for non-finite floats.
//   parse  json.loads(text): additionally accepts those bare tokens.
#pragma once
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace meradb::pyjson {

using Json = nlohmann::ordered_json;

// Thrown by parse(); what() is a short reason (the exact wording differs from
// Python's JSONDecodeError -- a documented, accepted difference).
class ParseFailure : public std::runtime_error {
public:
    explicit ParseFailure(const std::string& message) : std::runtime_error(message) {}
};

// More nested arrays/objects than this are refused (Python fails around 1000
// with a RecursionError, which the server would not survive gracefully).
constexpr int kMaxParseDepth = 512;

std::string dump(const Json& value);
Json parse(const std::string& text);

// Strict UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF).
bool isValidUtf8(const std::string& text);

}  // namespace meradb::pyjson
