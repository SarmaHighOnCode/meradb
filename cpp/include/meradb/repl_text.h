// cpp/include/meradb/repl_text.h
//
// The words of the interactive shell (mirrors meradb/repl.py): the `.help` reference and examples, the
// banner and the prompts. Pure text: nothing here reads input or touches a backend.
#pragma once
#include "meradb/term_style.h"
#include <ostream>
#include <string>
#include <vector>

namespace meradb::repl {

// One row of the keyword reference: MeraDB syntax, the SQL it corresponds to, what it does.
struct HelpRow {
    std::string keyword;
    std::string sql;
    std::string description;
};

struct HelpCategory {
    std::string name;
    std::vector<HelpRow> rows;
};

// Python's HELP_REFERENCE, generated from repl.py by cpp/tests/gen_shell_help.py.
const std::vector<HelpCategory>& helpReference();
// Python's EXAMPLES_HELP (same generator).
const char* examplesHelp();

// Python's SHELL_COMMANDS_HELP: the list of dot-commands, its heading bold magenta.
std::string shellCommandsHelp(const term::Style& style);

// Python's _render_reference(query): the keyword reference, optionally cut down to the rows whose syntax,
// SQL name or description contain `query` (stripped, lower-cased). No trailing newline.
std::string renderReference(const term::Style& style, const std::string& query = "");

// Everything bare `.help` prints, without the final newline: the shell commands, a blank line, the
// reference, a blank line, the examples.
std::string fullHelp(const term::Style& style);

}  // namespace meradb::repl
