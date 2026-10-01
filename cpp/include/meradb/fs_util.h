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

namespace meradb {

inline std::filesystem::path pathOf(const std::string& utf8) { return std::filesystem::u8path(utf8); }
inline std::string textOf(const std::filesystem::path& path) { return path.u8string(); }

}  // namespace meradb
