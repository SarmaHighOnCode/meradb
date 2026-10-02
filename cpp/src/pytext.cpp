// cpp/src/pytext.cpp -- see pytext.h.
#include "meradb/pytext.h"

namespace meradb::pytext {

bool isSpace(char32_t c) {
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

std::size_t decode(const std::string& text, std::size_t at, char32_t& codePoint) {
    const auto byteAt = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byteAt(at);
    std::size_t extra = 0;
    char32_t value = 0;
    if (lead < 0x80) {
        codePoint = lead;
        return 1;
    } else if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        value = lead & 0x1Fu;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        value = lead & 0x0Fu;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        value = lead & 0x07u;
    } else {
        codePoint = 0xFFFD;
        return 1;
    }
    if (at + extra >= text.size()) {
        codePoint = 0xFFFD;
        return 1;
    }
    for (std::size_t k = 1; k <= extra; ++k) {
        const unsigned char next = byteAt(at + k);
        if ((next & 0xC0) != 0x80) {
            codePoint = 0xFFFD;
            return 1;
        }
        value = (value << 6) | (next & 0x3Fu);
    }
    codePoint = value;
    return extra + 1;
}

std::string lstrip(const std::string& text) {
    std::size_t start = 0;
    while (start < text.size()) {
        char32_t cp = 0;
        const std::size_t length = decode(text, start, cp);
        if (!isSpace(cp)) break;
        start += length;
    }
    return text.substr(start);
}

std::string rstrip(const std::string& text) {
    std::size_t end = text.size();
    while (end > 0) {
        // Find where the last character starts: step back over at most three continuation bytes.
        std::size_t start = end - 1;
        while (start > 0 && end - start < 4 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) --start;
        char32_t cp = 0;
        std::size_t length = decode(text, start, cp);
        if (start + length != end) {  // not one whole character: treat the last byte on its own
            start = end - 1;
            cp = 0xFFFD;
        }
        if (!isSpace(cp)) break;
        end = start;
    }
    return text.substr(0, end);
}

std::string strip(const std::string& text) { return lstrip(rstrip(text)); }

std::vector<std::string> split(const std::string& text) {
    std::vector<std::string> words;
    std::string word;
    std::size_t at = 0;
    while (at < text.size()) {
        char32_t cp = 0;
        const std::size_t length = decode(text, at, cp);
        if (isSpace(cp)) {
            if (!word.empty()) words.push_back(word);
            word.clear();
        } else {
            word.append(text, at, length);
        }
        at += length;
    }
    if (!word.empty()) words.push_back(word);
    return words;
}

std::string lowerAscii(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

std::size_t length(const std::string& text) {
    std::size_t n = 0;
    for (unsigned char c : text)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

std::string ljust(const std::string& text, std::size_t width) {
    const std::size_t have = length(text);
    return have >= width ? text : text + std::string(width - have, ' ');
}

bool startsWith(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace meradb::pytext
