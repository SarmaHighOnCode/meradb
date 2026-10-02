// cpp/include/meradb/repl.h
//
// The interactive shell (mirrors meradb/repl.py): read lines, collect a statement until `;`, run it on a
// Backend, print the results. Everything prints to a `std::ostream&` and reads from a `LineSource`, so the
// whole loop runs against scripted input in tests; only ConsoleLineSource touches a real terminal.
#pragma once
#include "meradb/backend.h"
#include "meradb/sys_compat.h"
#include "meradb/term_style.h"
#include <istream>
#include <ostream>
#include <string>

namespace meradb::repl {

using sys::ReadStatus;

// Where the shell's lines come from. A source writes the prompt itself (Python's input(prompt) does too), to
// the same stream the shell prints to.
class LineSource {
public:
    virtual ~LineSource() = default;

    // Line: `line` holds one line without its terminator. Eof: no more input. Interrupted: Ctrl+C at the prompt.
    virtual ReadStatus read(const std::string& prompt, std::string& line) = 0;

    // True once if Ctrl+C arrived while the shell was busy running something (not waiting at a prompt).
    virtual bool takePendingInterrupt() { return false; }
};

// Lines from any std::istream (a pipe, a file, a test's istringstream). Like Python's sys.stdin with
// universal newlines: "\n", "\r\n" and a lone "\r" each end a line, and a last line without a terminator
// still counts. Bytes are passed through untouched (no BOM stripping, 0x1A is an ordinary character).
class StreamLineSource : public LineSource {
public:
    StreamLineSource(std::istream& in, std::ostream& out) : in_(in), out_(out) {}
    ReadStatus read(const std::string& prompt, std::string& line) override;

private:
    std::istream& in_;
    std::ostream& out_;
};

// A real terminal: the operating system's own line editing (see Design decision D1), via
// sys::readTerminalLine. Needs a sys::InterruptGuard to be alive for Ctrl+C to be reported.
class ConsoleLineSource : public LineSource {
public:
    explicit ConsoleLineSource(std::ostream& out) : out_(out) {}
    ReadStatus read(const std::string& prompt, std::string& line) override;
    bool takePendingInterrupt() override { return sys::InterruptGuard::consume(); }

private:
    std::ostream& out_;
};

// Python's run_text: run `text` on the backend and print every result followed by a blank line. A statement
// that fails prints its error and the next ones still run. A MeraDBError escaping the backend (the server
// connection dropped) is printed (`str(e)`) and the shell goes on. False if anything went wrong.
bool runText(Backend& backend, const std::string& text, std::ostream& out, const term::Style& style);

// Python's run_file: read a script (UTF-8, an optional byte-order mark, any newline style) and runText it.
// An unreadable file prints "File nahi khuli: <reason>" and returns false.
bool runFile(Backend& backend, const std::string& path, std::ostream& out, const term::Style& style);

// Whether the text typed so far ends a statement: Python's buffer.rstrip().endswith(";"). It does not look
// inside strings or comments (a line that ends in `;` inside a string literal ends it too).
bool endsStatement(const std::string& buffer);

// Python's handle_dot_command(backend, line); `line` is already stripped and starts with ".". False when the
// user asked to leave (.exit, .quit, .nikal).
bool handleDotCommand(Backend& backend, const std::string& line, std::ostream& out, const term::Style& style);

}  // namespace meradb::repl
