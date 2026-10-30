// cpp/include/meradb/fs_util.h
//
// Every path inside MeraDB is a UTF-8 std::string. std::filesystem on Windows
// reads a plain narrow string in the ANSI code page (MSVC) -- and a stream
// opened from a narrow string does too -- so a data folder or script name with
// an accented letter would turn into garbage. Going through these two helpers
// makes the encoding explicit everywhere.
#pragma once
#include <filesystem>
#include <string>
#include <system_error>

namespace meradb {

inline std::filesystem::path pathOf(const std::string& utf8) { return std::filesystem::u8path(utf8); }
inline std::string textOf(const std::filesystem::path& path) { return path.u8string(); }

// A database or table name becomes one file or folder name inside the data folder. The SQL tokenizer only produces
// safe names, but a name can also come from the wire ("database" in hello) or a hand-edited catalog.json, so it is
// checked where it turns into a path: no separators or "..", no NTFS stream (':'), no hidden folder such as the
// snapshot folder, nothing Windows would silently change (trailing dot or space).
inline bool isSafeFileName(const std::string& name) {
    if (name.empty() || name.front() == '.' || name.back() == '.' || name.back() == ' ') return false;
    for (char c : name) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) return false;
        switch (c) {
            case '/': case '\\': case ':': case '*': case '?': case '"': case '<': case '>': case '|': return false;
            default: break;
        }
    }
    return true;
}

// CON, NUL, COM1, ... are devices on Windows in every folder and with any extension: writing "nul.tbl" stores nothing
// and reports success. Refused when something is created, on every platform so that a data folder stays portable.
// (Not in isSafeFileName: an old database that already has such a name must still open.)
inline bool isReservedDeviceName(const std::string& name) {
    std::string upper;
    for (char c : name) upper += static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL") return true;
    return upper.size() == 4 && (upper.compare(0, 3, "COM") == 0 || upper.compare(0, 3, "LPT") == 0) && upper[3] >= '1' &&
           upper[3] <= '9';
}

// os.path.abspath: relative to the working directory, "" and "." mean the working directory itself,
// "." and ".." parts and doubled separators are folded away and a trailing separator is dropped
// (except on a root). Returns the input unchanged if the working directory cannot be read.
inline std::string absolutePathOf(const std::string& utf8) {
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(pathOf(utf8.empty() ? "." : utf8), ec);
    if (ec) return utf8;
    absolute = absolute.lexically_normal();
    if (!absolute.has_filename() && absolute.has_relative_path()) absolute = absolute.parent_path();
    return textOf(absolute);
}

}  // namespace meradb
