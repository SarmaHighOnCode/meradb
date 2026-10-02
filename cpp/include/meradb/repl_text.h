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

// Python's print_banner(version, where). With pauseMs > 0 each logo and wordmark line is flushed and
// followed by that pause (the "reveal" animation, which Python plays only when colour is on).
void printBanner(std::ostream& out, const term::Style& style, const std::string& version, const std::string& where,
                 int pauseMs);

// "meradb:<db>> ", or "meradb:<db>*> " while a transaction is open.
std::string promptFor(const term::Style& style, const std::string& db, bool inTransaction);

// Shown while a statement is still being typed.
inline constexpr const char* kContinuationPrompt = "      ...> ";

}  // namespace meradb::repl
