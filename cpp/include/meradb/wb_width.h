// cpp/include/meradb/wb_width.h -- how many terminal cells a text takes, without any terminal library.
//
// Same rules as the width function the terminal workbench's screen library uses: control characters and combining /
// zero-width marks take no cell, East Asian Wide and Fullwidth characters (and emoji) take two, everything else one.
// Bytes that are not valid UTF-8 are skipped. The data is Unicode 13 (src/wb_width_tables.inc).
#pragma once
#include <cstdint>
#include <string>

namespace meradb::wb {

int displayWidth(const std::string& utf8);
int codepointWidth(std::uint32_t codepoint);   // 0, 1 or 2

}  // namespace meradb::wb
