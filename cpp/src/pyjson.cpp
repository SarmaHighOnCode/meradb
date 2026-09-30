// cpp/src/pyjson.cpp
#include "meradb/pyjson.h"
#include "meradb/datatypes.h"
#include <cmath>
#include <cstdint>
#include <limits>

namespace meradb::pyjson {

namespace {

// Decodes one UTF-8 sequence starting at s[i]. Returns its length, or 0 when
// the bytes are not strictly valid UTF-8.
std::size_t decodeUtf8(const std::string& s, std::size_t i, std::uint32_t& cp) {
    const auto byteAt = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    unsigned char b0 = byteAt(i);
    std::size_t len;
    std::uint32_t minimum;
    if (b0 < 0x80) {
        cp = b0;
        return 1;
    } else if ((b0 & 0xE0) == 0xC0) {
        len = 2;
        cp = b0 & 0x1F;
        minimum = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3;
        cp = b0 & 0x0F;
        minimum = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4;
        cp = b0 & 0x07;
        minimum = 0x10000;
    } else {
        return 0;
    }
    if (i + len > s.size()) return 0;
    for (std::size_t k = 1; k < len; ++k) {
        unsigned char b = byteAt(i + k);
        if ((b & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return len;
}

void appendEscape(std::string& out, std::uint32_t unit) {
    static const char* hex = "0123456789abcdef";
    out += "\\u";
    out += hex[(unit >> 12) & 0xF];
    out += hex[(unit >> 8) & 0xF];
    out += hex[(unit >> 4) & 0xF];
    out += hex[unit & 0xF];
}

void writeString(std::string& out, const std::string& s) {
    out += '"';
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20 || c == 0x7f) appendEscape(out, c);  // Python escapes DEL too
                    else out += static_cast<char>(c);
            }
            ++i;
            continue;
        }
        std::uint32_t cp = 0;
        std::size_t len = decodeUtf8(s, i, cp);
        if (len == 0) {  // not valid UTF-8 (cannot come from a Python str): U+FFFD
            cp = 0xFFFD;
            len = 1;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            appendEscape(out, 0xD800 + (cp >> 10));
            appendEscape(out, 0xDC00 + (cp & 0x3FF));
        } else {
            appendEscape(out, cp);
        }
        i += len;
    }
    out += '"';
}

void writeValue(std::string& out, const Json& j) {
    if (j.is_null()) {
        out += "null";
    } else if (j.is_boolean()) {
        out += j.get<bool>() ? "true" : "false";
    } else if (j.is_number_unsigned()) {
        out += std::to_string(j.get<std::uint64_t>());
    } else if (j.is_number_integer()) {
        out += std::to_string(j.get<std::int64_t>());
    } else if (j.is_number_float()) {
        double d = j.get<double>();
        if (std::isnan(d)) out += "NaN";
        else if (std::isinf(d)) out += d > 0 ? "Infinity" : "-Infinity";
        else out += pyReprFloat(d);
    } else if (j.is_string()) {
        writeString(out, j.get_ref<const std::string&>());
    } else if (j.is_array()) {
        out += '[';
        bool first = true;
        for (const auto& item : j) {
            if (!first) out += ", ";
            first = false;
            writeValue(out, item);
        }
        out += ']';
    } else if (j.is_object()) {
        out += '{';
        bool first = true;
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!first) out += ", ";
            first = false;
            writeString(out, it.key());
            out += ": ";
            writeValue(out, it.value());
        }
        out += '}';
    } else {
        out += "null";  // binary blobs never occur in MeraDB messages
    }
}

// Python writes non-finite floats as bare words nlohmann cannot read. Outside
// strings they are swapped for {"$float": "..."} markers (undone after
// parsing); the same pass counts nesting so a hostile message cannot be deep.
std::string markBareConstants(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool inString = false;
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (inString) {
            out += c;
            if (c == '\\' && i + 1 < text.size()) out += text[++i];
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '[' || c == '{') {
            if (++depth > kMaxParseDepth) throw ParseFailure("nesting bahut gehri hai");
        } else if (c == ']' || c == '}') {
            if (depth > 0) --depth;
        } else if (c == 'I' && text.compare(i, 8, "Infinity") == 0) {
            out += "{\"$float\": \"Infinity\"}";
            i += 7;
            continue;
        } else if (c == '-' && text.compare(i + 1, 8, "Infinity") == 0) {
            out += "{\"$float\": \"-Infinity\"}";
            i += 8;
            continue;
        } else if (c == 'N' && text.compare(i, 3, "NaN") == 0) {
            out += "{\"$float\": \"NaN\"}";
            i += 2;
            continue;
        }
        out += c;
    }
    return out;
}

void restoreConstants(Json& j) {
    if (j.is_object()) {
        if (j.size() == 1 && j.begin().key() == "$float" && j.begin().value().is_string()) {
            const std::string word = j.begin().value().get<std::string>();
            if (word == "Infinity") { j = std::numeric_limits<double>::infinity(); return; }
            if (word == "-Infinity") { j = -std::numeric_limits<double>::infinity(); return; }
            if (word == "NaN") { j = std::numeric_limits<double>::quiet_NaN(); return; }
        }
        for (auto it = j.begin(); it != j.end(); ++it) restoreConstants(it.value());
    } else if (j.is_array()) {
        for (auto& item : j) restoreConstants(item);
    }
}

}  // namespace

std::string dump(const Json& value) {
    std::string out;
    writeValue(out, value);
    return out;
}

Json parse(const std::string& text) {
    std::string marked = markBareConstants(text);
    Json result;
    try {
        result = Json::parse(marked);
    } catch (const nlohmann::json::exception& e) {
        throw ParseFailure(e.what());
    }
    restoreConstants(result);
    return result;
}

bool isValidUtf8(const std::string& text) {
    for (std::size_t i = 0; i < text.size();) {
        std::uint32_t cp = 0;
        std::size_t len = decodeUtf8(text, i, cp);
        if (len == 0) return false;
        i += len;
    }
    return true;
}

}  // namespace meradb::pyjson
