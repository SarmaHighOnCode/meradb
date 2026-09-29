// cpp/include/meradb/cli_format.h
//
// Renders one statement's Result the way the Python shell / `meradb run` does
// (meradb/repl.py: format_table + print_result), minus colour.
#pragma once
#include "meradb/engine.h"
#include <string>

namespace meradb {

// Error -> the error text. Rows -> an ASCII table ("+---+" borders), followed
// by the message on its own line if there is one. Otherwise the bare message.
// No trailing newline.
std::string formatResult(const Result& r);

}  // namespace meradb
