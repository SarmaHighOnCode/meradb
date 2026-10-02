// cpp/include/meradb/cli_format.h
//
// Renders one statement's Result the way the Python shell / `meradb run` does
// (meradb/repl.py: format_table + print_result), with colour when asked.
#pragma once
#include "meradb/engine.h"
#include "meradb/term_style.h"
#include <string>

namespace meradb {

// Error -> the error text (red). Rows -> an ASCII table ("+---+" borders, dim; header row bold),
// followed by the message (green) on its own line if there is one. Otherwise the bare message.
// No trailing newline. With term::Style::none() the text is plain.
std::string formatResult(const Result& r, const term::Style& style);

// The plain rendering: formatResult(r, term::Style::none()).
inline std::string formatResult(const Result& r) { return formatResult(r, term::Style::none()); }

}  // namespace meradb
