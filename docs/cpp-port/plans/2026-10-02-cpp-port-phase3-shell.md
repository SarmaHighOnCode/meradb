# MeraDB C++ Port — Phase 3: Interactive Shell Implementation Plan

**Goal:** Give the C++ command line the interactive shell: `meradb_cli shell` (also what a bare
`meradb_cli` does) — banner, prompts, multi-line statements ended by `;`, the dot-commands
(`.help [word]`, `.tables`, `.schema <table>`, `.run <file>`, `.exit` / `.quit` / `.nikal`), colour,
Ctrl+C / Ctrl+D / end-of-input handling — behaving exactly like `python -m meradb shell`, in local mode
and in server mode, against a C++ server and against a Python server. Output is verified by driving
the Python shell and the C++ shell with identical piped scripts and diffing everything they print.

**Architecture:** Mirrors `meradb/repl.py` the way the earlier phases mirrored their modules. The shell
only ever talks to a `Backend` (Phase 2: `LocalBackend` / `Connection`), so local mode and server mode
share every line of it, exactly like Python's duck typing. All shell output goes through a
`std::ostream&` and all input through a small `LineSource` interface, so the whole read-eval-print loop
is unit-testable with scripted input; only three tiny platform primitives (read a line from a terminal,
catch Ctrl+C, switch on ANSI colour) touch the OS, and they live in `sys_compat`. Result rendering is the
Phase 1/2 `formatResult`, extended with colour; `runFile` / script output moves into the shell module so
`run` and `.run` share one code path (and `run` gains Python's "connection dropped" handling).
Phase 4's workbench reuses `term::Style`, `formatResult`, `repl::runText`, the help text and `pytext`.

**Tech Stack:** C++17, CMake, Catch2 v3 (all present from earlier phases). **No new third-party
dependency**: no line-editing library (Design decision D1). Python 3 (the reference shell) is used only
by the verification scripts and the two golden generators.

**Spec:** [docs/cpp-port/specs/2026-09-27-cpp-port-design.md](../specs/2026-09-27-cpp-port-design.md)
(Phase 3 row of "Phased delivery": banner with block logo, wordmark and reveal animation, ANSI colour
detection, `.help` reference, dot-commands, "feature-matched to `repl.py`").

**Starts from:** branch `worktree-cpp-port-phase1` at `08c35a6` (Phases 1 and 2 complete, merged into
local `main`; 503 ctest tests registered). Create the Phase 3 work on a new branch off that commit.

**Out of scope (deliberately left to the project owner):** the three prove-it exercises in
`docs/ROADMAP.md` — the `^` power operator, the `.hexdump <table>` shell command, and
`GINO(ALAG x)`. Do **not** add them. `.hexdump` therefore stays an *unknown* dot-command in the C++
shell, exactly as it is in the Python shell today (`Ye shell command nahi pata: .hexdump  (.help dekho)`).
Also out of scope: the workbench (Phase 4), the final test/doc polish (Phase 5), a statement history file,
tab completion and syntax highlighting (Python's shell has none; `highlight.py` belongs to the workbench).

## Global Constraints

Everything in the Phase 1 and Phase 2 plans' Global Constraints still applies. Additions and the
lessons that recurred in the earlier reviews:

- **Python is the oracle, never the plan text.** Prompts, wording, blank lines, escape codes and every
  quirk come from `meradb/repl.py` and from *running* it (`python -m meradb shell` fed through a pipe).
  Expected strings in tests come from the generators in Tasks 3-5 (`gen_shell_golden.py`,
  `gen_shell_help.py`), never retyped. When a unit test must hard-code a transcript, it was captured
  from Python and the task quotes where. Where this plan and Python disagree, Python wins: fix the
  plan's test, not the shell.
- **GCC evaluates call arguments right-to-left.** Never put two calls that can throw or have side
  effects in one argument list, one `<<` chain that mixes them with state changes, or one braced
  initializer. Bind into named locals in source order first.
- **Streams, not printf.** Every shell function that prints takes `std::ostream& out`; none touches
  `std::cout` / `std::cerr` directly except the CLI glue in `cli.cpp`. Write `"\n"`, never `std::endl`;
  flush only before a blocking read and at exit.
- **MSVC / portability rules** (only MinGW g++ is built on the dev machine, so review by reading): no
  `long double`, `__int128`, VLAs, `ssize_t`, GCC builtins, or POSIX-only headers outside `#ifdef`
  blocks; no `std::getenv` / `localtime` / `strerror` (use `sys::` wrappers); never include `<windows.h>`
  from a public header, and only `sys_compat.cpp` includes it (`WIN32_LEAN_AND_MEAN` and `NOMINMAX` are
  already defined by the build); avoid `ptrdiff_t` to `long` / `int` narrowing; any source containing
  UTF-8 relies on the `/utf-8` flag the build already sets.
- **Windows console specifics.** Output is always UTF-8 bytes (`main.cpp` already switches a console's
  code page via `sys::Utf8Console`). A console is read with `ReadConsoleW` (UTF-16, no code-page
  surprises) and converted; ANSI colour needs `ENABLE_VIRTUAL_TERMINAL_PROCESSING` on the classic
  console host, switched on and restored through `sys::AnsiConsole`. Piped stdin is read in *binary*
  mode (no CRLF translation, Ctrl+Z is not end-of-file), exactly as Python's `sys.stdin` behaves.
- **Tests never touch the real data folder or fixed ports.** Every test that opens a database uses a
  temp folder (`TempDir`, or `--data <tmp>` / `MERADB_DATA=<tmp>` in the Python scripts); every server
  binds port 0 / a free port the OS chose; every Python script removes `MERADB_*` from the child
  environment, passes `--local` where a developer's real server on 6372 could otherwise interfere,
  stops what it started, and leaves no process, pid file or log behind (`cli_lifecycle.py` pattern).
- **No terminal in CI.** The three terminal primitives (`readTerminalLine`, the Ctrl+C handler,
  `AnsiConsole`) are kept tiny and are the only code that cannot be unit tested; Task 14 lists the
  manual checks for them, and everything else sits behind the `LineSource` seam.
- **Windows MinGW builds need PATH exported in every shell** (Phase 1 ledger, "Build env"): CMake's `bin`
  and WinLibs' `mingw64\bin`, e.g. `export PATH="/c/Program Files/CMake/bin:<winlibs>/mingw64/bin:$PATH"`.
- **Hygiene:** no references to tool-generated attribution lines or tool names anywhere in
  code, comments, docs or commit messages; no personal, university or course details; commit messages
  are plain one-liners. Never modify `meradb/` or `examples/` (`git diff <phase-3-base> -- meradb examples`
  must be empty).
- **Deliberate divergences** from Python are allowed only when listed in "Design decisions" below; each
  one is copied into `docs/CPP.md` in Task 14.

---

## Design decisions

### D1. Line input: the terminal's own editing, no in-tree editor, no library

Python's shell calls `input(prompt)` and never imports `readline`, so it has **no history file, no
completion and no custom key handling**; what a user gets is whatever the operating system's line
discipline gives: on a Windows console the console host's editing (arrow keys, Home / End, insert,
the F7 history list of the session, Ctrl+Left / Right), on a POSIX terminal canonical mode (Backspace,
Ctrl+U, Ctrl+W, no arrow-key recall). The C++ shell reads the same way — `ReadConsoleW` on a Windows
console, `read(0)` on a POSIX tty, the stream otherwise — so it is *feature-matched*, with the same
editing available for free. We therefore do **not** add linenoise / replxx (which the Phase 2 note in
`docs/CPP.md` floated and which the spec does not name): a dependency, a vendored copy and a
raw-terminal mode to maintain, for behaviour Python does not have. If the owner later wants arrow-key
history in both shells, that is a Phase 5+ enhancement that must be done in Python too; the `LineSource`
interface (D3) is the one place it would plug in. Recorded in `docs/CPP.md` (Task 14).

### D2. What the Python shell does (read from `repl.py`, confirmed by running it piped)

The transcript rules the C++ shell reproduces to the byte (each is a test in Tasks 8-9 and 11):

1. **Banner**, always (also when piped), then one blank line, then the first prompt. Pieces: a blank
   line; the 7-row block logo `MERA` (bold cyan `1;36`) + `DB` (bold magenta `1;35`), each pixel two
   `█` wide; a blank line; the slim ASCII wordmark, dim (`2`), centred as one block under the logo; a
   blank line; the tagline `meraDB 1.0.0 -- apna database, apni bhasha.` centred, bold; the line
   `  connected: <where>`; and the hint line `  .help commands  ·  .exit bahar niklo  ·  statements ; se khatam hote hain`
   (`.help` and `.exit` bold yellow `1;33`, the dots dim, the `;` bold yellow). The reveal animation
   (40 ms pause after every logo and wordmark line) happens only when colour is on.
2. **Prompt** `meradb:<db>> ` (db bold cyan); inside a transaction `meradb:<db>*> ` (`*` bold yellow);
   continuation prompt `      ...> ` (6 spaces). Prompts are written to **stdout without a newline**, so
   a piped transcript contains them back to back with the output.
3. **A line is a dot-command** only when the buffer is empty and `line.strip()` starts with `.`;
   otherwise the line, plus `"\n"`, is appended to the buffer. **A blank line therefore makes the buffer
   non-empty** (`"\n"`): the next prompt is the continuation prompt, and a `.tables` typed after a blank
   line is *statement text*, not a command (the parser then reports `... par '.' mila`). This is a
   quirk; it is copied.
4. **A statement ends** when `buffer.rstrip().endswith(";")` — Python's `rstrip`, i.e. Unicode
   whitespace including NBSP and U+3000. It does not look inside strings or comments: `-- done;` ends
   the buffer, and a `;` that ends a physical line inside a string literal ends it too (the tokenizer then
   reports the unterminated string). The *whole buffer* then goes to `run_script`; a lone `;` runs an
   empty script and prints nothing, not even a blank line.
5. **Each result** prints (red `31` for an error; otherwise the table, then the message green `32`) and
   **then a blank line**, including after errors. A failing statement does not stop later statements in
   the same buffer.
6. **If the backend raises a `MeraDBError`** (connection dropped mid-session) the shell prints
   `str(e)` (`[Connection Galti] ...`) and goes on; every later statement prints it again.
7. **`.exit`, `.quit`, `.nikal`** (any case, extra words ignored) print `Phir milenge!` and end. **End of
   input or Ctrl+C at a prompt** (including the continuation prompt, which discards the buffer) print a
   newline, then `Phir milenge!` and end. Exit code 0 in every one of these cases.
8. **Dot-commands** (`parts = line.split()`, `cmd = parts[0].lower()`): `.help` with a topic
   (`line[len(parts[0]):].strip()`, so multi-word topics work) prints the filtered reference; bare `.help`
   prints `SHELL_COMMANDS_HELP`, a blank line, the reference, a blank line, `EXAMPLES_HELP`. `.tables` runs
   `DIKHAO TABLES;`. `.schema X` runs `BATAO X;` with `X = args[0]` only (so `.schema` alone is an
   *unknown command*, and `.schema a b` ignores `b`). `.run F` runs file `args[0]` (a path with spaces is
   cut at the first space) and `.run` alone is unknown. Anything else prints
   `Ye shell command nahi pata: <line>  (.help dekho)` where `<line>` is the stripped line.
9. **`.help <topic>`** filters `HELP_REFERENCE` rows whose `" ".join(row).lower()` contains
   `topic.strip().lower()`; no match prints `'<topic>' ke liye kuch nahi mila. Poora reference ke liye sirf .help likho.`
10. **Reading**: the script is `sys.stdin` text with universal newlines, so `\n`, `\r\n` and a lone `\r`
    all end a line; a final line without a newline still counts as a line; Ctrl+Z (0x1A) in *piped*
    input is an ordinary character.
11. **Colour** is decided once: off when `NO_COLOR` is non-empty or stdout is not a terminal; on for a
    terminal on Linux / macOS; on Windows on when `WT_SESSION`, `TERM` or `ConEmuANSI=ON` is set,
    otherwise only if the classic console host accepts `ENABLE_VIRTUAL_TERMINAL_PROCESSING`. `meradb run`
    colours its results by the same rule (Python's `run_file` uses the same `print_result`).
12. **Connecting** is `open_backend`, already ported in Phase 2 (`openBackend`): automatic local
    fallback with a note on stderr, `-W` (hidden prompt), `-U`, `-d`, `--local`. The shell calls it and
    does not copy it. On failure the error goes to stderr and the exit code is 1, before any banner.

### D3. Seams: `LineSource`, `std::ostream&`, `Backend`

```cpp
enum class sys::ReadStatus { Line, Eof, Interrupted };
class repl::LineSource {          // prompts are written by the source itself, to the same stream the shell prints to
public:
    virtual ReadStatus read(const std::string& prompt, std::string& line) = 0;
    virtual bool takePendingInterrupt() { return false; }   // Ctrl+C that arrived while a statement was running
};
```

`StreamLineSource` (any `std::istream`, used for pipes, files and every unit test) and
`ConsoleLineSource` (a real terminal, built on `sys::readTerminalLine`). The shell loop `repl::run`
takes `(Backend&, LineSource&, std::ostream&, term::Style, pauseMs)` and returns the exit code. A fake
`Backend` in the tests makes dropped connections, slow statements and transaction state easy to script.

### D4. Output glyphs are always UTF-8

Python probes `sys.stdout.encoding` and falls back to `*` and `#` when it cannot encode `·` / `█`
(a *piped* Python on Windows uses the legacy ANSI code page and so prints ASCII). The C++ program always
writes UTF-8 (a console is switched to the UTF-8 code page; redirected output is plain UTF-8), so it always
prints `·` and `█`. **Divergence**, recorded; the verification scripts run Python with
`PYTHONIOENCODING=utf-8` so both sides print the same glyphs.

### D5. Ctrl+C

At a prompt: prints `\nPhir milenge!` and exits 0, like Python. While a statement is running: Python's
`KeyboardInterrupt` unwinds out of the shell (no message, exit code 130, backend closed). A C++ statement
cannot be interrupted half-way (an engine call holds the engine lock; cancelling mid-write would corrupt
the very guarantees `WAPAS` exists for), so the flag is noted and the shell exits with **130 as soon as that
statement has finished**, silently. **Divergence**, recorded. The handler is installed only when stdin is a
terminal; piped runs keep the operating system's default (a signal kills the process, as Python's
default handler would end it too).

### D6. Errors that are not `MeraDBError`

Python lets any other exception (a bug, invalid UTF-8 on stdin, ...) print a traceback and exit 1 after
closing the backend. C++: `runText` catches only `MeraDBError` (as Python does); anything else propagates
to `cliMain`, which prints the message on stderr and returns 1, and the backend is closed by its
destructor on the way out. A `std::bad_alloc`-class failure is not reachable from a script. **Invalid UTF-8
on stdin:** Python's strict decoder dies with a traceback; C++ passes the bytes on (the tokenizer reports
an unexpected character). Recorded as a divergence; nothing depends on it.

### D7. `run` shares the shell's code path

Python's `run_file` and the shell's `.run` both go through `run_text`, which catches `MeraDBError`
(connection dropped), prints `str(e)` **to stdout** and returns False. The Phase 2 C++ `runFile` let that
exception escape to `cliMain` (stderr, exit 1, remaining files skipped). Task 7 moves the logic into
`repl::runText` / `repl::runFile`; `cli.cpp`'s `runFile` becomes a thin wrapper. This is a **bug fix to
match Python**, not a divergence: afterwards a dropped connection in `run` prints the error on stdout and
the next file still runs.

### D8. Module map (Phase 4 reuse in bold)

| Python | C++ |
|---|---|
| `repl._c`, `_supports_color`, `_enable_conhost_ansi` | **`term_style.h/.cpp`** (`term::Style`, `supportsColor`, `detectStyle`); `sys::isTerminal`, `sys::AnsiConsole` |
| `str.strip/split/lower/ljust` semantics | **`pytext.h/.cpp`** |
| `format_table`, `print_result` | **`cli_format.h/.cpp`** (`formatResult(result, style)`) |
| logo, banner, `HELP_REFERENCE`, `_render_reference`, prompts | **`repl_text.h/.cpp`** + generated `src/shell_help_data.inc` |
| `run_text`, `run_file`, `handle_dot_command`, `repl` | `repl.h/.cpp` (`repl::runText`, `runFile`, `handleDotCommand`, `run`, `LineSource`) |
| `input()`, `KeyboardInterrupt` | `sys::readTerminalLine`, `sys::InterruptGuard` |
| `cmd_shell` | `cli.cpp` (`runShellCommand`) |

### D9. Where extra review effort goes

1. **Transcript fidelity** (Tasks 8, 9, 11): prompt placement, the blank-line-makes-a-continuation quirk,
   what an empty script prints, the exact `Phir milenge!` newlines.
2. **Terminal primitives** (Tasks 2, 6): never run here; reviewed against the MSVC rules and the manual
   checklist.
3. **Colour goldens** (Tasks 3-5): every escape sequence compared with Python's own output.
4. **Server-mode behaviour** (Tasks 12-13): the same transcript from a C++ server and a Python server,
   mid-session disconnects, `-W` / `-U`, transaction marker through the wire.
5. **The `runFile` refactor** (Task 7): it changes a Phase 2 code path that `run`, the cross-engine tests
   and `.run` all depend on.

---

## Reference: the Python pieces being ported (`meradb/repl.py`)

```
print_banner(version, where)        logo (MERA bold cyan, DB bold magenta) / wordmark (dim) / tagline (bold)
prompt_for(backend)                 "meradb:" + c(db,1,36) + (c("*",1,33) if txn) + "> "
repl(backend)                       banner, blank line, then: read line -> dot-command | buffer -> run_text
run_text(backend, text) -> bool     run_script; print each result + blank line; MeraDBError -> print(e), False
run_file(backend, path) -> bool     utf-8-sig read; OSError -> "File nahi khuli: <errno text>"
handle_dot_command(backend, line)   .exit .quit .nikal | .help [topic] | .tables | .schema X | .run F | unknown
format_table(result) / print_result(result)
```

## Task index (details below; batches at the end)

| # | Title | Batch |
|---|---|---|
| 1 | `pytext`: Python string semantics for the shell | A |
| 2 | `term::Style`, colour detection, terminal primitives in `sys_compat` | A |
| 3 | Coloured result rendering and the Python golden generator | A |
| 4 | `.help` text: generated reference data and rendering | A |
| 5 | Banner and prompts | A |
| 6 | `LineSource`: stream and console readers | B |
| 7 | `repl::runText` / `runFile` and the `run` path fix | B |
| 8 | Dot-commands | B |
| 9 | The read-eval-print loop | B |
| 10 | CLI wiring: `meradb_cli shell` | B |
| 11 | Cross-engine shell diff: local mode | C |
| 12 | Cross-engine shell diff: C++ and Python servers | C |
| 13 | Shell session behaviours: logins, drops, fallback, hardening | C |
| 14 | Docs, manual terminal checklist, completion checklist, hand-off to Phases 4-5 | D |

---

## Command conventions used in every task

- Configure once (MinGW on Windows, from the repo root, PATH exported first):
  `cmake -S cpp -B cpp/build -G "MinGW Makefiles"` (Linux/macOS: omit `-G`). Re-run it whenever a task
  edits a `CMakeLists.txt`.
- Build: `cmake --build cpp/build`.
- Test everything: `ctest --test-dir cpp/build --output-on-failure`. One area:
  `ctest --test-dir cpp/build --output-on-failure -R "<name>"` where `<name>` is a fragment of the Catch2
  test-case names (each task names its fragment; names start with the area word on purpose: `pytext`,
  `term`, `shell`).
- "Expected: FAIL" steps mean the build breaks (missing header) or the test fails; either counts.
- Python oracle for the shell: `python -m meradb shell --local --data <tmpdir> < script.txt` from the repo
  root, with `PYTHONIOENCODING=utf-8` (the harness in Tasks 11-12 wraps this). Python on Windows writes
  `\r\n`; the harnesses normalise it to `\n`, as `cross_engine_diff.py` already does.
- Golden files are regenerated with `python cpp/tests/gen_shell_golden.py` and
  `python cpp/tests/gen_shell_help.py` (Tasks 3 and 4); never edit the generated files by hand.

# BATCH A — pure pieces: text helpers, colour, rendering, help, banner (no input, no backend)

### Task 1: `pytext`: Python string semantics for the shell

**Files:**
- Create: `cpp/include/meradb/pytext.h`
- Create: `cpp/src/pytext.cpp`
- Test: `cpp/tests/test_pytext.cpp`
- Modify: `cpp/CMakeLists.txt` (add `src/pytext.cpp` to `meradb_core`)
- Modify: `cpp/tests/CMakeLists.txt` (add `test_pytext.cpp`)

**Interfaces:**
- Produces, in `namespace meradb::pytext`: `isSpace(char32_t)`, `decode(text, at, cp)`, `lstrip`, `rstrip`,
  `strip`, `split`, `lower` (originally `lowerAscii`, replaced by a full Unicode `str.lower()`),
  `length`, `ljust`, `startsWith`, `endsWith`. The shell decides "is this a
  dot-command" with `strip(line)`, "does the statement end here" with `rstrip(buffer)`, cuts a command with
  `split`, and pads the help table with `ljust` — all with **Python's** notion of whitespace (29
  characters, including NBSP U+00A0, U+0085 and the ideographic space U+3000). `isspace()` from `<cctype>`
  is wrong for this (locale-dependent, one byte at a time).

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_pytext.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/pytext.h"

using namespace meradb;

TEST_CASE("pytext isSpace matches Python's str.isspace", "[pytext]") {
    for (char32_t c : {0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x85, 0xA0, 0x1680, 0x2000, 0x2005, 0x200A,
                       0x2028, 0x2029, 0x202F, 0x205F, 0x3000})
        CHECK(pytext::isSpace(c));
    for (char32_t c : {0x00, 0x08, 0x0E, 0x1B, 0x21, 0x41, 0x84, 0x86, 0x9F, 0xA1, 0x180E, 0x1FFF, 0x200B, 0x2027, 0x202A,
                       0x2060, 0x3001, 0xFEFF})
        CHECK_FALSE(pytext::isSpace(c));
}

TEST_CASE("pytext strip handles Unicode whitespace at both ends", "[pytext]") {
    CHECK(pytext::strip("  a b \t\n") == "a b");
    CHECK(pytext::strip("\xC2\xA0 a;\xE3\x80\x80\xE2\x80\xA8") == "a;");  // NBSP, ideographic space, line separator
    CHECK(pytext::strip("   ") == "");
    CHECK(pytext::strip("") == "");
    CHECK(pytext::rstrip("x; \xC2\xA0") == "x;");
    CHECK(pytext::lstrip(" \xC2\xA0x ") == "x ");
    CHECK(pytext::strip("caf\xC3\xA9 ") == "caf\xC3\xA9");  // a non-space two-byte character stays whole
}

TEST_CASE("pytext strip leaves malformed bytes alone", "[pytext]") {
    CHECK(pytext::rstrip("a\xC2") == "a\xC2");   // truncated sequence
    CHECK(pytext::rstrip("a\x80 ") == "a\x80");  // stray continuation byte, then a space
    CHECK(pytext::lstrip("\xFF a") == "\xFF a");
}

TEST_CASE("pytext split works on runs of whitespace", "[pytext]") {
    CHECK(pytext::split(".HELP  join \t x") == std::vector<std::string>{".HELP", "join", "x"});
    CHECK(pytext::split("a\xC2\xA0" "b\xE2\x80\x83" "c") == std::vector<std::string>{"a", "b", "c"});
    CHECK(pytext::split("   ").empty());
    CHECK(pytext::split("").empty());
    CHECK(pytext::split("solo") == std::vector<std::string>{"solo"});
}

TEST_CASE("pytext lowerAscii touches ASCII letters only", "[pytext]") {
    CHECK(pytext::lowerAscii(".Help JOIN \xC3\x89") == ".help join \xC3\x89");
}

TEST_CASE("pytext length and ljust count characters", "[pytext]") {
    CHECK(pytext::length("caf\xC3\xA9") == 4);
    CHECK(pytext::length("\xF0\x9F\x98\x80") == 1);
    CHECK(pytext::ljust("ab", 5) == "ab   ");
    CHECK(pytext::ljust("caf\xC3\xA9", 6) == "caf\xC3\xA9  ");
    CHECK(pytext::ljust("long", 2) == "long");
}

TEST_CASE("pytext startsWith and endsWith", "[pytext]") {
    CHECK(pytext::startsWith(".tables", "."));
    CHECK_FALSE(pytext::startsWith("", "."));
    CHECK(pytext::endsWith("a;", ";"));
    CHECK_FALSE(pytext::endsWith(";a", ";"));
    CHECK_FALSE(pytext::endsWith("", ";"));
}
```

- [ ] **Step 2: Add the test file to the build and verify it fails**

Add `test_pytext.cpp` to `cpp/tests/CMakeLists.txt` (before the
`# test files are appended here by later tasks` line). Run:
```bash
cmake --build cpp/build
```
Expected: FAIL — `meradb/pytext.h` does not exist.

- [ ] **Step 3: Write the header**

```cpp
// cpp/include/meradb/pytext.h
//
// The few Python `str` behaviours the shell depends on, for UTF-8 text: strip() / rstrip() / split()
// with Python's Unicode idea of whitespace (so a trailing no-break space still ends a statement, as it
// does in the Python shell), an ASCII-only lower(), and length / ljust counted in characters.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace meradb::pytext {

// str.isspace() for one code point (the 29 characters Python 3 treats as whitespace).
bool isSpace(char32_t codePoint);

// Decodes the UTF-8 character starting at text[at] (at < text.size()) and returns its length in
// bytes (at least 1). A malformed sequence decodes as U+FFFD and consumes ONE byte.
std::size_t decode(const std::string& text, std::size_t at, char32_t& codePoint);

std::string lstrip(const std::string& text);
std::string rstrip(const std::string& text);
std::string strip(const std::string& text);

// str.split() with no argument: runs of whitespace separate words, no empty words.
std::vector<std::string> split(const std::string& text);

// str.lower() for ASCII letters only; every other byte is left alone.
std::string lowerAscii(std::string text);

// len(str): the number of characters (UTF-8 lead bytes).
std::size_t length(const std::string& text);

// str.ljust(width): pads with spaces up to `width` CHARACTERS.
std::string ljust(const std::string& text, std::size_t width);

bool startsWith(const std::string& text, const std::string& prefix);
bool endsWith(const std::string& text, const std::string& suffix);

}  // namespace meradb::pytext
```

- [ ] **Step 4: Write the implementation**

```cpp
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
```

- [ ] **Step 5: Add the source to the library, build, run**

In `cpp/CMakeLists.txt`, inside `add_library(meradb_core STATIC ...)`, add `src/pytext.cpp` before the
`# source files are appended here by later tasks` line. Then:
```bash
cmake -S cpp -B cpp/build -G "MinGW Makefiles"
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "pytext"
```
Expected: 7 `pytext` test cases pass; no warnings.

- [ ] **Step 6: Commit**

```bash
git add cpp/include/meradb/pytext.h cpp/src/pytext.cpp cpp/tests/test_pytext.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add pytext: Python whitespace, strip, split and ljust for the shell"
```

**Completion checklist:**
- [ ] `git diff --stat` touches only the files listed above
- [ ] no `<cctype>` whitespace function is used on shell input anywhere in the new code
- [ ] suite green, zero warnings

---

### Task 2: `term::Style`, colour detection, terminal primitives in `sys_compat`

**Files:**
- Create: `cpp/include/meradb/term_style.h`, `cpp/src/term_style.cpp`
- Modify: `cpp/include/meradb/sys_compat.h` (append the terminal section), `cpp/src/sys_compat.cpp` (append its implementation)
- Test: `cpp/tests/test_term_style.cpp`
- Modify: `cpp/CMakeLists.txt` (add `src/term_style.cpp`), `cpp/tests/CMakeLists.txt` (add `test_term_style.cpp`)

**Interfaces:**
- Produces `term::Style` (`c(text, {codes...})`, `on()`, `none()`, `colored()`), `term::ColorEnv`,
  `term::supportsColor(env)` (Python's `_supports_color`, as a pure function), `term::currentColorEnv(fn)`,
  `term::detectStyle(fn)`.
- Produces in `sys`: `isTerminal(fd)`, `class AnsiConsole { bool enable(); }`, `class InterruptGuard`
  (`consume()`, `trigger()`), `enum class ReadStatus { Line, Eof, Interrupted }`, `readTerminalLine(line)`,
  `setStdinBinary()`. Task 6 uses `readTerminalLine`, `InterruptGuard` and `setStdinBinary`; Task 10 uses
  `AnsiConsole` and `isTerminal`. Everything here that cannot run without a terminal is the *smallest*
  possible piece of code (Task 14 lists how to check it by hand).

Python's rule (`_supports_color`, copied into `supportsColor`): off if `NO_COLOR` is non-empty or stdout
is not a terminal; on for any terminal on Linux / macOS; on Windows on if `WT_SESSION`, `TERM` or
`ConEmuANSI == "ON"` is set (modern terminal programs need nothing), otherwise ask the classic console host
to switch on `ENABLE_VIRTUAL_TERMINAL_PROCESSING` and use colour only if it agrees.

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_term_style.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/sys_compat.h"
#include "meradb/term_style.h"
#include <optional>
#include <vector>

using namespace meradb;

TEST_CASE("term Style wraps text in SGR codes only when colour is on", "[term]") {
    CHECK(term::Style::colored().c("hi", {1, 36}) == "\x1b[1;36mhi\x1b[0m");
    CHECK(term::Style::colored().c("hi", {2}) == "\x1b[2mhi\x1b[0m");
    CHECK(term::Style::colored().c("hi", {}) == "hi");
    CHECK(term::Style::none().c("hi", {1, 36}) == "hi");
    CHECK(term::Style().c("hi", {31}) == "hi");
    CHECK(term::Style::colored().on());
    CHECK_FALSE(term::Style::none().on());
}

TEST_CASE("term supportsColor follows the Python rule", "[term]") {
    term::ColorEnv env;
    CHECK_FALSE(term::supportsColor(env));  // not a terminal
    env.stdoutIsTerminal = true;
    CHECK(term::supportsColor(env));        // a Linux / macOS terminal
    env.noColor = true;
    CHECK_FALSE(term::supportsColor(env));  // NO_COLOR wins
    env.noColor = false;

    env.windows = true;  // from here on: Windows
    int asked = 0;
    env.enableConsoleAnsi = [&] {
        ++asked;
        return true;
    };
    env.windowsAnsiHint = true;
    CHECK(term::supportsColor(env));  // Windows Terminal, mintty, ConEmu: trusted, the console is not asked
    CHECK(asked == 0);
    env.windowsAnsiHint = false;
    CHECK(term::supportsColor(env));  // classic console host: asked, says yes
    CHECK(asked == 1);
    env.enableConsoleAnsi = [&] {
        ++asked;
        return false;
    };
    CHECK_FALSE(term::supportsColor(env));  // ... says no
    CHECK(asked == 2);
    env.enableConsoleAnsi = nullptr;
    CHECK_FALSE(term::supportsColor(env));
    env.stdoutIsTerminal = false;
    env.windowsAnsiHint = true;
    CHECK_FALSE(term::supportsColor(env));  // a pipe stays plain whatever the hint
}

namespace {
// Restores the environment variables a test sets.
class EnvRestore {
public:
    explicit EnvRestore(std::vector<std::string> names) : names_(std::move(names)) {
        for (const auto& name : names_) saved_.push_back(sys::getEnv(name));
    }
    ~EnvRestore() {
        for (std::size_t i = 0; i < names_.size(); ++i) sys::setEnv(names_[i], saved_[i].value_or(""));
    }

private:
    std::vector<std::string> names_;
    std::vector<std::optional<std::string>> saved_;
};
}  // namespace

TEST_CASE("term currentColorEnv reads NO_COLOR and the Windows hints", "[term]") {
    EnvRestore restore({"NO_COLOR", "WT_SESSION", "TERM", "ConEmuANSI"});
    sys::setEnv("NO_COLOR", "");
    sys::setEnv("WT_SESSION", "");
    sys::setEnv("TERM", "");
    sys::setEnv("ConEmuANSI", "");
    auto env = term::currentColorEnv(nullptr);
    CHECK_FALSE(env.noColor);
    CHECK_FALSE(env.windowsAnsiHint);

    sys::setEnv("NO_COLOR", "1");
    CHECK(term::currentColorEnv(nullptr).noColor);
    sys::setEnv("NO_COLOR", "");

    sys::setEnv("TERM", "xterm");
    CHECK(term::currentColorEnv(nullptr).windowsAnsiHint);
    sys::setEnv("TERM", "");
    sys::setEnv("WT_SESSION", "abc");
    CHECK(term::currentColorEnv(nullptr).windowsAnsiHint);
    sys::setEnv("WT_SESSION", "");
    sys::setEnv("ConEmuANSI", "OFF");
    CHECK_FALSE(term::currentColorEnv(nullptr).windowsAnsiHint);
    sys::setEnv("ConEmuANSI", "ON");
    CHECK(term::currentColorEnv(nullptr).windowsAnsiHint);
}

TEST_CASE("term detectStyle is plain when output is not a terminal", "[term]") {
    // ctest runs tests with stdout captured, so this is plain; run by hand in a terminal it may not be.
    if (sys::isTerminal(1)) SKIP("stdout is a terminal");
    CHECK_FALSE(term::detectStyle(nullptr).on());
}

TEST_CASE("term sys: unknown descriptors are not terminals, and the guards construct", "[term]") {
    CHECK_FALSE(sys::isTerminal(57));
    CHECK_FALSE(sys::isTerminal(-1));
    {
        sys::AnsiConsole ansi;
        (void)ansi.enable();  // must neither crash nor leave a console changed
    }
    {
        sys::InterruptGuard guard;
        CHECK_FALSE(sys::InterruptGuard::consume());
        sys::InterruptGuard::trigger();
        CHECK(sys::InterruptGuard::consume());
        CHECK_FALSE(sys::InterruptGuard::consume());
    }
}
```

- [ ] **Step 2: Add the test file to the build and verify it fails**

Add `test_term_style.cpp` to `cpp/tests/CMakeLists.txt`. Run `cmake --build cpp/build`.
Expected: FAIL — `meradb/term_style.h` does not exist.

- [ ] **Step 3: Write `term_style.h` and `term_style.cpp`**

```cpp
// cpp/include/meradb/term_style.h
//
// Colour for the command line (mirrors the colour helpers at the top of meradb/repl.py): plain ANSI
// escape codes, used only when stdout is a terminal that understands them. NO_COLOR (no-color.org) and
// redirected output both turn them off, so scripted output and test captures stay plain text.
#pragma once
#include <functional>
#include <initializer_list>
#include <string>

namespace meradb::term {

class Style {
public:
    Style() = default;
    explicit Style(bool on) : on_(on) {}

    static Style none() { return Style(false); }
    static Style colored() { return Style(true); }

    bool on() const { return on_; }

    // `text` wrapped in ANSI SGR codes (1 bold, 2 dim, 31 red, 32 green, 33 yellow, 35 magenta, 36 cyan),
    // or unchanged when colour is off or no code is given. Python: _c(text, *codes).
    std::string c(const std::string& text, std::initializer_list<int> codes) const;

private:
    bool on_ = false;
};

// Everything Python's _supports_color() looks at, as plain values so the rule can be tested.
struct ColorEnv {
    bool noColor = false;           // NO_COLOR is set to something non-empty
    bool stdoutIsTerminal = false;  // stdout is a terminal, not a pipe or a file
    bool windows = false;
    bool windowsAnsiHint = false;   // WT_SESSION or TERM non-empty, or ConEmuANSI == "ON"
    // Classic Windows console host only: ask it to render escape codes. Called at most once, last.
    std::function<bool()> enableConsoleAnsi;
};

bool supportsColor(const ColorEnv& env);

// The ColorEnv of this process.
ColorEnv currentColorEnv(std::function<bool()> enableConsoleAnsi);

// Style::colored() when supportsColor(currentColorEnv(...)), otherwise Style::none().
Style detectStyle(std::function<bool()> enableConsoleAnsi);

}  // namespace meradb::term
```

```cpp
// cpp/src/term_style.cpp -- see term_style.h.
#include "meradb/term_style.h"
#include "meradb/sys_compat.h"

namespace meradb::term {

std::string Style::c(const std::string& text, std::initializer_list<int> codes) const {
    if (!on_ || codes.size() == 0) return text;
    std::string joined;
    for (int code : codes) {
        if (!joined.empty()) joined += ';';
        joined += std::to_string(code);
    }
    return "\x1b[" + joined + "m" + text + "\x1b[0m";
}

bool supportsColor(const ColorEnv& env) {
    if (env.noColor || !env.stdoutIsTerminal) return false;
    if (!env.windows) return true;  // real terminals on Linux and macOS understand ANSI as standard
    // Modern terminal programs on Windows already understand ANSI; asking the classic console host
    // below could wrongly report "unsupported" on one of them.
    if (env.windowsAnsiHint) return true;
    return env.enableConsoleAnsi ? env.enableConsoleAnsi() : false;
}

ColorEnv currentColorEnv(std::function<bool()> enableConsoleAnsi) {
    const auto nonEmpty = [](const char* name) {
        const auto value = sys::getEnv(name);
        return value.has_value() && !value->empty();
    };
    ColorEnv env;
    env.noColor = nonEmpty("NO_COLOR");
    env.stdoutIsTerminal = sys::isTerminal(1);
#ifdef _WIN32
    env.windows = true;
#endif
    const auto conEmu = sys::getEnv("ConEmuANSI");
    env.windowsAnsiHint = nonEmpty("WT_SESSION") || nonEmpty("TERM") || (conEmu.has_value() && *conEmu == "ON");
    env.enableConsoleAnsi = std::move(enableConsoleAnsi);
    return env;
}

Style detectStyle(std::function<bool()> enableConsoleAnsi) {
    return supportsColor(currentColorEnv(std::move(enableConsoleAnsi))) ? Style::colored() : Style::none();
}

}  // namespace meradb::term
```

- [ ] **Step 4: Append the terminal section to `sys_compat.h`**

Add this at the end of `cpp/include/meradb/sys_compat.h`, just before the closing
`}  // namespace meradb::sys`:

```cpp
// ---------------------------------------------------------------------------
// The interactive terminal (used by the shell)
// ---------------------------------------------------------------------------

// True when standard stream `fd` (0 = stdin, 1 = stdout, 2 = stderr) is attached to a terminal (a console
// on Windows). Anything else, including a pipe or a file, is false.
bool isTerminal(int fd);

// Turns on ANSI escape-code processing for the console that stdout is attached to, on the classic Windows
// console host (ENABLE_VIRTUAL_TERMINAL_PROCESSING), and puts the old console mode back in the destructor.
// Does nothing elsewhere.
class AnsiConsole {
public:
    AnsiConsole();
    ~AnsiConsole();
    AnsiConsole(const AnsiConsole&) = delete;
    AnsiConsole& operator=(const AnsiConsole&) = delete;

    // True when escape codes will be understood after this call. False when stdout is not a console,
    // or the console refuses. Always true on non-Windows platforms.
    bool enable();

private:
    unsigned savedMode_ = 0;
    bool changed_ = false;
};

// While a guard exists, Ctrl+C does not end the process: it only sets a flag that the shell reads (and,
// on Windows, cancels a console read that is waiting for a line). One guard at a time.
class InterruptGuard {
public:
    InterruptGuard();
    ~InterruptGuard();
    InterruptGuard(const InterruptGuard&) = delete;
    InterruptGuard& operator=(const InterruptGuard&) = delete;

    // True once per Ctrl+C: reads the flag and clears it.
    static bool consume();
    // Does what the Ctrl+C handler does. For tests, which cannot press the key.
    static void trigger();
};

enum class ReadStatus { Line, Eof, Interrupted };

// Reads one line from the terminal (stdin MUST be one; see isTerminal) as UTF-8 without its line
// terminator. Eof: Ctrl+D (Ctrl+Z then Enter on Windows) or a closed input. Interrupted: Ctrl+C, which
// needs an InterruptGuard; the flag is cleared (consumed) by this return.
ReadStatus readTerminalLine(std::string& line);

// When stdin is NOT a terminal, Windows would translate CRLF and stop at Ctrl+Z (text mode); this puts
// stdin into binary mode so the bytes arrive untouched. Does nothing elsewhere.
void setStdinBinary();
```

- [ ] **Step 5: Append the implementation to `sys_compat.cpp`**

First add to the includes at the top of `cpp/src/sys_compat.cpp`: `#include <atomic>` with the other
standard headers; inside the existing `#ifdef _WIN32` include block add `#include <fcntl.h>` and
`#include <io.h>` (the `#else` block already has `<fcntl.h>`, `<signal.h>`, `<unistd.h>`, `<cerrno>`).
Then append this at the end of the file, **before** the closing `}  // namespace meradb::sys` (it uses the
file's existing `isConsole` and `narrow` helpers, which are visible at that point):

```cpp
// ---------------------------------------------------------------------------
// the interactive terminal
// ---------------------------------------------------------------------------

bool isTerminal(int fd) {
#ifdef _WIN32
    if (fd == 0) return isConsole(STD_INPUT_HANDLE);
    if (fd == 1) return isConsole(STD_OUTPUT_HANDLE);
    if (fd == 2) return isConsole(STD_ERROR_HANDLE);
    return false;
#else
    return ::isatty(fd) != 0;
#endif
}

#ifdef _WIN32
namespace {
constexpr DWORD kVirtualTerminalProcessing = 0x0004;  // ENABLE_VIRTUAL_TERMINAL_PROCESSING (older headers lack the name)
}

AnsiConsole::AnsiConsole() {}

bool AnsiConsole::enable() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (out == nullptr || out == INVALID_HANDLE_VALUE || !GetConsoleMode(out, &mode)) return false;
    if ((mode & kVirtualTerminalProcessing) != 0) return true;
    if (!SetConsoleMode(out, mode | kVirtualTerminalProcessing)) return false;
    savedMode_ = static_cast<unsigned>(mode);
    changed_ = true;
    return true;
}

AnsiConsole::~AnsiConsole() {
    if (!changed_) return;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE) SetConsoleMode(out, static_cast<DWORD>(savedMode_));
}
#else
AnsiConsole::AnsiConsole() {}
AnsiConsole::~AnsiConsole() {}
bool AnsiConsole::enable() { return true; }
#endif

namespace {

std::atomic<bool> g_interrupted{false};
static_assert(std::atomic<bool>::is_always_lock_free, "the Ctrl+C flag is set from a signal handler / handler thread");

#ifdef _WIN32
HANDLE g_readerThread = nullptr;  // a real handle to the thread that created the guard (the one that reads input)

BOOL WINAPI onConsoleControl(DWORD event) {
    if (event != CTRL_C_EVENT) return FALSE;  // Ctrl+Break and the rest keep their default meaning
    g_interrupted = true;
    if (g_readerThread != nullptr) CancelSynchronousIo(g_readerThread);  // wake a waiting ReadConsole
    return TRUE;
}
#else
struct sigaction g_previousAction;

void onSigint(int) { g_interrupted = true; }
#endif

}  // namespace

InterruptGuard::InterruptGuard() {
    g_interrupted = false;
#ifdef _WIN32
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_readerThread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    SetConsoleCtrlHandler(onConsoleControl, TRUE);
#else
    struct sigaction action {};
    action.sa_handler = onSigint;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;  // no SA_RESTART: a read() waiting for a line must return EINTR
    sigaction(SIGINT, &action, &g_previousAction);
#endif
}

InterruptGuard::~InterruptGuard() {
#ifdef _WIN32
    SetConsoleCtrlHandler(onConsoleControl, FALSE);
    if (g_readerThread != nullptr) CloseHandle(g_readerThread);
    g_readerThread = nullptr;
#else
    sigaction(SIGINT, &g_previousAction, nullptr);
#endif
}

bool InterruptGuard::consume() { return g_interrupted.exchange(false); }

void InterruptGuard::trigger() { g_interrupted = true; }

ReadStatus readTerminalLine(std::string& line) {
    line.clear();
#ifdef _WIN32
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    std::wstring text;
    for (;;) {
        wchar_t buffer[512];
        DWORD got = 0;
        const BOOL ok = ReadConsoleW(in, buffer, 512, &got, nullptr);
        const bool aborted = !ok && GetLastError() == ERROR_OPERATION_ABORTED;
        if (InterruptGuard::consume() || aborted) return ReadStatus::Interrupted;
        if (!ok || got == 0) {
            if (text.empty()) return ReadStatus::Eof;
            break;
        }
        text.append(buffer, got);
        if (text.back() == L'\n') break;  // a longer line arrives in several pieces
    }
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) text.pop_back();
    if (!text.empty() && text.front() == L'\x1a') return ReadStatus::Eof;  // Ctrl+Z, Enter
    line = narrow(text);
    return ReadStatus::Line;
#else
    for (;;) {
        char c = 0;
        const auto got = ::read(STDIN_FILENO, &c, 1);
        if (got < 0) {
            if (errno == EINTR) {
                if (InterruptGuard::consume()) return ReadStatus::Interrupted;
                continue;
            }
            return line.empty() ? ReadStatus::Eof : ReadStatus::Line;
        }
        if (got == 0) return line.empty() ? ReadStatus::Eof : ReadStatus::Line;
        if (c == '\n') return ReadStatus::Line;
        line.push_back(c);
    }
#endif
}

void setStdinBinary() {
#ifdef _WIN32
    if (!isConsole(STD_INPUT_HANDLE)) _setmode(_fileno(stdin), _O_BINARY);
#endif
}
```

- [ ] **Step 6: Add the source to the library, build, run**

Add `src/term_style.cpp` to `meradb_core` in `cpp/CMakeLists.txt`. Then:
```bash
cmake -S cpp -B cpp/build -G "MinGW Makefiles"
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "term"
```
Expected: 5 `term` test cases pass (the `detectStyle` one is skipped, not failed, if you run the test binary by
hand inside a terminal); no warnings.

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/term_style.h cpp/src/term_style.cpp cpp/include/meradb/sys_compat.h cpp/src/sys_compat.cpp cpp/tests/test_term_style.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add term::Style, colour detection and the terminal primitives"
```

**Completion checklist:**
- [ ] `<windows.h>` is still included only from `sys_compat.cpp` (`grep -rn "windows.h" cpp/include cpp/src`)
- [ ] the Ctrl+C flag is a lock-free atomic (the `static_assert` compiles)
- [ ] the POSIX handler is installed without `SA_RESTART`
- [ ] suite green, zero warnings

---

### Task 3: Coloured result rendering and the Python golden generator

**Files:**
- Modify: `cpp/include/meradb/cli_format.h`, `cpp/src/cli_format.cpp` (`formatResult` takes a `term::Style`)
- Create: `cpp/tests/gen_shell_golden.py`; generated and committed: `cpp/tests/golden_shell.h`
- Test: `cpp/tests/test_cli_output.cpp` (append)

**Interfaces:**
- Consumes: `term::Style` (Task 2).
- Produces: `std::string formatResult(const Result&, const term::Style&)` (the one-argument overload stays,
  plain). Colours exactly as `repl.py`: table borders dim (`2`), header row bold (`1`), message green
  (`32`), error red (`31`).
- Produces the **golden generator**: `gen_shell_golden.py` imports `meradb/repl.py`, runs its real
  `print_banner`, `handle_dot_command(None, ".help ...")`, `prompt_for`, `print_result` in two fresh
  interpreters (one with `NO_COLOR=1`, one pretending stdout is a terminal), and writes
  `golden_shell.h` with every sample twice. `golden_shell::get(name, colour)` returns a sample.
  Tasks 4 and 5 add their tests against the same file.

- [ ] **Step 1: Write the generator**

```python
# cpp/tests/gen_shell_golden.py
"""
Regenerates cpp/tests/golden_shell.h: exact strings the Python shell (meradb/repl.py) produces for the
pieces the C++ shell reimplements -- banner, .help output, prompts and result rendering -- once with colour
off and once with colour on (a pretend terminal), so the C++ unit tests can compare byte for byte.

    python cpp/tests/gen_shell_golden.py
"""
import argparse
import contextlib
import io
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUT = Path(__file__).with_name("golden_shell.h")


class FakeTerminal(io.TextIOWrapper):
    """A stdout that claims to be a terminal, so repl.py switches colour on."""

    def __init__(self):
        super().__init__(io.BytesIO(), encoding="utf-8")

    def isatty(self):
        return True


class FakeBackend:
    def __init__(self, db, txn):
        self.current_db, self.in_transaction = db, txn


def capture(fn, *args):
    buffer = io.StringIO()
    with contextlib.redirect_stdout(buffer):
        fn(*args)
    return buffer.getvalue()


def child(mode):
    """Runs in a fresh interpreter: prints {name: text} as JSON."""
    if mode == "color":
        sys.stdout = FakeTerminal()
    sys.path.insert(0, str(ROOT))
    from meradb import repl
    from meradb.engine import Result

    repl.time.sleep = lambda seconds: None  # the reveal animation pauses only when colour is on
    assert repl._COLOR == (mode == "color"), "colour mode was not what the generator asked for"
    samples = {
        "banner": capture(repl.print_banner, "1.0.0", "local (/data/x)"),
        "banner_server": capture(repl.print_banner, "1.0.0", "127.0.0.1:6372"),
        "help_full": capture(repl.handle_dot_command, None, ".help"),
        "help_join": capture(repl.handle_dot_command, None, ".help join"),
        "help_two_words": capture(repl.handle_dot_command, None, ".help foreign key"),
        "help_none": capture(repl.handle_dot_command, None, ".help zzzz"),
        "prompt_main": repl.prompt_for(FakeBackend("main", False)),
        "prompt_txn": repl.prompt_for(FakeBackend("college", True)),
        "result_table": capture(repl.print_result, Result(["id", "naam"], [[1, "Ravi"], [2, None]], "2 row(s)")),
        "result_message": capture(repl.print_result, Result(message="Table 't' ban gaya (1 columns)")),
        "result_error": capture(repl.print_result, Result(error="[Execution Galti] Table 'x' exist nahi karta")),
        "result_empty": capture(repl.print_result, Result(["a"], [], "0 row(s)")),
        "result_unicode": capture(repl.print_result, Result(["n"], [[chr(0xE9) + chr(0x1F600)]], "1 row(s)")),
    }
    sys.__stdout__.write(json.dumps(samples))


def run_child(mode):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_") and k != "NO_COLOR"}
    env.update(PYTHONIOENCODING="utf-8", PYTHONPATH=str(ROOT))
    if mode == "plain":
        env["NO_COLOR"] = "1"
    else:
        env["TERM"] = "xterm"  # makes repl.py trust ANSI on Windows too
    done = subprocess.run([sys.executable, __file__, "--child", mode], capture_output=True, env=env, cwd=str(ROOT), check=True)
    return json.loads(done.stdout.decode("utf-8"))


def literal(text):
    """A C++ string literal (one source line per text line, adjacent literals concatenate)."""
    pieces = []
    for line in text.split("\n"):
        out = []
        for ch in line:
            if ch == "\\":
                out.append("\\\\")
            elif ch == '"':
                out.append('\\"')
            elif ch == "\r":
                out.append("\\r")
            elif ch == "\x1b":
                out.append("\\x1b")
            elif ord(ch) < 32:
                raise SystemExit(f"unexpected control character {ch!r}")
            else:
                out.append(ch)
        pieces.append("".join(out))
    return "\n        ".join('"%s%s"' % (p, "\\n" if i < len(pieces) - 1 else "") for i, p in enumerate(pieces))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--child")
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    if args.child:
        child(args.child)
        return
    plain, color = run_child("plain"), run_child("color")
    assert plain.keys() == color.keys()
    lines = [
        "// cpp/tests/golden_shell.h -- GENERATED by gen_shell_golden.py from meradb/repl.py. Do not edit by hand.",
        "// Each sample exists twice: colour off (a pipe) and colour on (a terminal).",
        "#pragma once",
        "#include <stdexcept>",
        "#include <string>",
        "",
        "namespace golden_shell {",
        "",
        "struct Sample {",
        "    const char* name;",
        "    bool color;",
        "    const char* text;",
        "};",
        "",
        "inline const Sample* samples(int& count) {",
        "    static const Sample all[] = {",
    ]
    total = 0
    for name in plain:
        for flag, data in (("false", plain), ("true", color)):
            lines.append('        {"%s", %s,\n        %s},' % (name, flag, literal(data[name])))
            total += 1
    lines += [
        "    };",
        f"    count = {total};",
        "    return all;",
        "}",
        "",
        "// The sample called `name`, rendered with colour on or off.",
        "inline std::string get(const std::string& name, bool color) {",
        "    int count = 0;",
        "    const Sample* all = samples(count);",
        "    for (int i = 0; i < count; ++i)",
        "        if (name == all[i].name && all[i].color == color) return all[i].text;",
        '    throw std::runtime_error("no golden shell sample " + name);',
        "}",
        "",
        "}  // namespace golden_shell",
        "",
    ]
    args.out.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"wrote {args.out} ({total} samples)")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Generate the golden file**

```bash
python cpp/tests/gen_shell_golden.py
```
Expected: `wrote .../cpp/tests/golden_shell.h (26 samples)`. Open the file and look at the `"banner", true` sample: it
must contain `\x1b[1;36m` (MERA) and `\x1b[1;35m` (DB) sequences. The file is committed; regenerate it
whenever `repl.py` changes.

- [ ] **Step 3: Write the failing test** (append to `cpp/tests/test_cli_output.cpp`)

Add `#include "golden_shell.h"` next to the file's other includes, then append:

```cpp
namespace {

Result sampleTable() {
    Result r;
    r.columns = {"id", "naam"};
    r.rows = {{Value(int64_t(1)), Value(std::string("Ravi"))}, {Value(int64_t(2)), Value()}};
    r.message = "2 row(s)";
    return r;
}

Result sampleMessage() {
    Result r;
    r.message = "Table 't' ban gaya (1 columns)";
    return r;
}

Result sampleError() {
    Result r;
    r.error = "[Execution Galti] Table 'x' exist nahi karta";
    return r;
}

Result sampleEmpty() {
    Result r;
    r.columns = {"a"};
    r.message = "0 row(s)";
    return r;
}

Result sampleUnicode() {
    Result r;
    r.columns = {"n"};
    r.rows = {{Value(std::string("\xC3\xA9\xF0\x9F\x98\x80"))}};
    r.message = "1 row(s)";
    return r;
}

// Python's print_result prints formatResult's text plus a newline (nothing at all for an empty result).
void checkAgainstPython(const std::string& sample, const Result& r) {
    for (bool color : {false, true}) {
        INFO(sample << (color ? " (colour)" : " (plain)"));
        CHECK(formatResult(r, term::Style(color)) + "\n" == golden_shell::get(sample, color));
    }
}

}  // namespace

TEST_CASE("formatResult matches Python's print_result, plain and coloured", "[cli]") {
    checkAgainstPython("result_table", sampleTable());
    checkAgainstPython("result_message", sampleMessage());
    checkAgainstPython("result_error", sampleError());
    checkAgainstPython("result_empty", sampleEmpty());
    checkAgainstPython("result_unicode", sampleUnicode());
}

TEST_CASE("formatResult colours: dim borders, bold header, green message, red error", "[cli]") {
    const term::Style on = term::Style::colored();
    CHECK(formatResult(sampleMessage(), on) == "\x1b[32mTable 't' ban gaya (1 columns)\x1b[0m");
    CHECK(formatResult(sampleError(), on) == "\x1b[31m[Execution Galti] Table 'x' exist nahi karta\x1b[0m");
    CHECK(formatResult(sampleEmpty(), on) ==
          "\x1b[2m+---+\x1b[0m\n\x1b[1m| a |\x1b[0m\n\x1b[2m+---+\x1b[0m\n\x1b[2m+---+\x1b[0m\n\x1b[32m0 row(s)\x1b[0m");
    CHECK(formatResult(sampleEmpty()) == formatResult(sampleEmpty(), term::Style::none()));
}
```

Run `cmake --build cpp/build`. Expected: FAIL — `formatResult(const Result&, const term::Style&)` is not declared.

- [ ] **Step 4: Replace `cli_format.h`**

```cpp
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
```

- [ ] **Step 5: Replace `cli_format.cpp`**

```cpp
#include "meradb/cli_format.h"
#include "meradb/datatypes.h"
#include <algorithm>
#include <vector>

namespace meradb {

namespace {

// Python pads by code points (len()), not bytes: count UTF-8 lead bytes.
size_t displayLen(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

std::string padRight(const std::string& s, size_t width) {
    size_t len = displayLen(s);
    return len >= width ? s : s + std::string(width - len, ' ');
}

std::string formatTable(const Result& r, const term::Style& style) {
    std::vector<std::vector<std::string>> body;
    for (const auto& row : r.rows) {
        std::vector<std::string> cells;
        for (const auto& v : row) cells.push_back(formatValue(v));
        body.push_back(std::move(cells));
    }
    std::vector<size_t> widths;
    for (const auto& h : r.columns) widths.push_back(displayLen(h));
    for (const auto& row : body)
        for (size_t i = 0; i < row.size() && i < widths.size(); ++i) widths[i] = std::max(widths[i], displayLen(row[i]));

    std::string plainSep = "+";
    for (size_t w : widths) plainSep += std::string(w + 2, '-') + "+";
    const std::string sep = style.c(plainSep, {2});  // dim

    auto line = [&](const std::vector<std::string>& cells) {
        std::string out = "|";
        for (size_t i = 0; i < widths.size(); ++i) {
            out += " " + padRight(i < cells.size() ? cells[i] : "", widths[i]) + " |";
        }
        return out;
    };

    std::string out = sep + "\n" + style.c(line(r.columns), {1}) + "\n" + sep;  // the header row is bold
    for (const auto& row : body) out += "\n" + line(row);
    out += "\n" + sep;
    return out;
}

}  // namespace

std::string formatResult(const Result& r, const term::Style& style) {
    if (!r.error.empty()) return style.c(r.error, {31});  // red
    std::string out;
    if (!r.columns.empty()) out = formatTable(r, style);
    if (!r.message.empty()) out += (out.empty() ? "" : "\n") + style.c(r.message, {32});  // green
    return out;
}

}  // namespace meradb
```

- [ ] **Step 6: Build and run**

```bash
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "formatResult|cli"
```
Expected: the 6 older `formatResult` cases and the 2 new ones pass (every caller of the one-argument
`formatResult`, including `cli.cpp`, still compiles).

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/cli_format.h cpp/src/cli_format.cpp cpp/tests/gen_shell_golden.py cpp/tests/golden_shell.h cpp/tests/test_cli_output.cpp
git commit -m "Colour result rendering and add the Python golden generator for the shell"
```

**Completion checklist:**
- [ ] `golden_shell.h` was produced by the generator (diff it against a fresh run: no change)
- [ ] a colour sample really contains escape sequences; a plain sample contains none
- [ ] suite green, zero warnings

---

### Task 4: `.help` text: generated reference data and rendering

**Files:**
- Create: `cpp/tests/gen_shell_help.py`; generated and committed: `cpp/src/shell_help_data.inc`
- Create: `cpp/include/meradb/repl_text.h`, `cpp/src/repl_text.cpp`
- Test: `cpp/tests/test_repl_text.cpp`
- Modify: `cpp/CMakeLists.txt` (add `src/repl_text.cpp`), `cpp/tests/CMakeLists.txt` (add `test_repl_text.cpp`)

**Interfaces:**
- Consumes: `term::Style`, `pytext` (Tasks 1-2), `golden_shell.h` (Task 3).
- Produces in `namespace meradb::repl`: `struct HelpRow`, `struct HelpCategory`, `helpReference()`,
  `examplesHelp()`, `shellCommandsHelp(style)`, `renderReference(style, query = "")`, `fullHelp(style)`.
  The 80-odd rows of Hinglish help are **generated** from `HELP_REFERENCE` / `EXAMPLES_HELP` in `repl.py`
  and committed, so nobody retypes them and the help can never drift; `golden_shell.h` then proves the
  rendering (column widths, blank lines, colours, the "nothing found" message).

- [ ] **Step 1: Write the data generator**

```python
# cpp/tests/gen_shell_help.py
"""
Regenerates cpp/src/shell_help_data.inc: the `.help` keyword reference and the quick examples, copied from
meradb/repl.py (HELP_REFERENCE and EXAMPLES_HELP) so the C++ shell never drifts from the Python wording.

    python cpp/tests/gen_shell_help.py
"""
import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_OUT = ROOT / "cpp" / "src" / "shell_help_data.inc"


def literal(text):
    out = []
    for ch in text:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ord(ch) < 32 or ord(ch) > 126:
            raise SystemExit(f"non-ASCII or control character {ch!r} in the help text: extend the generator")
        else:
            out.append(ch)
    return '"' + "".join(out) + '"'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    sys.path.insert(0, str(ROOT))
    from meradb import repl

    lines = [
        "// cpp/src/shell_help_data.inc -- GENERATED by cpp/tests/gen_shell_help.py from meradb/repl.py. Do not edit by hand.",
        "// Included once, by repl_text.cpp, inside namespace meradb::repl.",
        "",
        "const std::vector<HelpCategory>& helpReference() {",
        "    static const std::vector<HelpCategory> data = {",
    ]
    for category, rows in repl.HELP_REFERENCE:
        lines.append(f"        {{{literal(category)},")
        lines.append("         {")
        for keyword, sql, desc in rows:
            lines.append(f"             {{{literal(keyword)}, {literal(sql)}, {literal(desc)}}},")
        lines.append("         }},")
    lines += ["    };", "    return data;", "}", "", "const char* examplesHelp() {",
              f"    return {literal(repl.EXAMPLES_HELP)};", "}", ""]
    args.out.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Generate the data**

```bash
python cpp/tests/gen_shell_help.py
```
Expected: `wrote .../cpp/src/shell_help_data.inc` (about 9 KB; it starts with
`const std::vector<HelpCategory>& helpReference() {` and the first category is `Databases`).

- [ ] **Step 3: Write the failing test**

```cpp
// cpp/tests/test_repl_text.cpp -- the shell's words, compared with what repl.py produces
// (golden_shell.h is generated by gen_shell_golden.py).
#include <catch2/catch_test_macros.hpp>
#include "golden_shell.h"
#include "meradb/repl_text.h"
#include <chrono>
#include <sstream>

using namespace meradb;

TEST_CASE("shell help: the full .help text matches Python, plain and coloured", "[shell]") {
    for (bool color : {false, true}) {
        INFO((color ? "colour" : "plain"));
        CHECK(repl::fullHelp(term::Style(color)) + "\n" == golden_shell::get("help_full", color));
    }
}

TEST_CASE("shell help: filtered references match Python", "[shell]") {
    for (bool color : {false, true}) {
        INFO((color ? "colour" : "plain"));
        const term::Style style(color);
        CHECK(repl::renderReference(style, "join") + "\n" == golden_shell::get("help_join", color));
        CHECK(repl::renderReference(style, "foreign key") + "\n" == golden_shell::get("help_two_words", color));
        CHECK(repl::renderReference(style, "zzzz") + "\n" == golden_shell::get("help_none", color));
    }
}

TEST_CASE("shell help: the query is stripped and lower-cased", "[shell]") {
    const term::Style plain;
    CHECK(repl::renderReference(plain, "  JOIN \t") == repl::renderReference(plain, "join"));
    CHECK(repl::renderReference(plain, "") == repl::renderReference(plain));
    CHECK(repl::renderReference(plain, "   ") == repl::renderReference(plain));  // blank means everything
    CHECK(repl::renderReference(plain, "Q?") == "'q?' ke liye kuch nahi mila. Poora reference ke liye sirf .help likho.");
}

TEST_CASE("shell help: the reference data has every category Python has", "[shell]") {
    const auto& data = repl::helpReference();
    REQUIRE_FALSE(data.empty());
    CHECK(data.front().name == "Databases");
    CHECK(data.back().name == "Stored procedures");
    std::size_t rows = 0;
    for (const auto& category : data) rows += category.rows.size();
    CHECK(rows > 60);
    CHECK(std::string(repl::examplesHelp()).rfind("Quick examples (full reference: docs/LANGUAGE.md):\n", 0) == 0);
}
```

Add `test_repl_text.cpp` to `cpp/tests/CMakeLists.txt`. Run `cmake --build cpp/build`.
Expected: FAIL — `meradb/repl_text.h` does not exist.

- [ ] **Step 4: Write the header** (Task 5 appends the banner and prompt declarations to it)

```cpp
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
```

- [ ] **Step 5: Write the implementation** (Task 5 appends the banner code to it)

```cpp
// cpp/src/repl_text.cpp -- see repl_text.h.
#include "meradb/repl_text.h"
#include "meradb/pytext.h"
#include <algorithm>

namespace meradb::repl {

// helpReference() and examplesHelp(): generated, see cpp/tests/gen_shell_help.py.
#include "shell_help_data.inc"

std::string shellCommandsHelp(const term::Style& style) {
    return style.c("Shell commands", {1, 35}) +
           "\n"
           "  .help [khoj]     ye reference dikhao (poora, ya sirf matching keywords -- jaise: .help join)\n"
           "  .tables          saari tables dikhao\n"
           "  .schema <table>  table ka structure\n"
           "  .run <file>      ek .mdb script file chalao\n"
           "  .exit / .nikal   bahar niklo";
}

std::string renderReference(const term::Style& style, const std::string& rawQuery) {
    const std::string query = pytext::lowerAscii(pytext::strip(rawQuery));

    struct Matched {
        const HelpCategory* category;
        std::vector<const HelpRow*> rows;
    };
    std::vector<Matched> matched;
    for (const auto& category : helpReference()) {
        Matched m{&category, {}};
        for (const auto& row : category.rows) {
            const std::string haystack = pytext::lowerAscii(row.keyword + " " + row.sql + " " + row.description);
            if (query.empty() || haystack.find(query) != std::string::npos) m.rows.push_back(&row);
        }
        if (!m.rows.empty()) matched.push_back(std::move(m));
    }

    if (!query.empty() && matched.empty())
        return "'" + query + "' ke liye kuch nahi mila. Poora reference ke liye sirf .help likho.";

    std::size_t keywordWidth = 0, sqlWidth = 0;
    for (const auto& m : matched)
        for (const HelpRow* row : m.rows) {
            keywordWidth = std::max(keywordWidth, pytext::length(row->keyword));
            sqlWidth = std::max(sqlWidth, pytext::length(row->sql));
        }

    std::string out;
    for (const auto& m : matched) {
        out += style.c(m.category->name, {1, 35}) + "\n";
        for (const HelpRow* row : m.rows)
            out += "  " + style.c(pytext::ljust(row->keyword, keywordWidth), {1, 36}) + "  " +
                   style.c(pytext::ljust(row->sql, sqlWidth), {2}) + "  " + row->description + "\n";
        out += "\n";
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();  // "\n".join(out).rstrip("\n")
    return out;
}

std::string fullHelp(const term::Style& style) {
    return shellCommandsHelp(style) + "\n\n" + renderReference(style) + "\n\n" + examplesHelp();
}

}  // namespace meradb::repl
```

- [ ] **Step 6: Add the source to the library, build, run**

Add `src/repl_text.cpp` to `meradb_core` in `cpp/CMakeLists.txt` (it picks up `shell_help_data.inc` from
its own directory). Then:
```bash
cmake -S cpp -B cpp/build -G "MinGW Makefiles"
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "shell help"
```
Expected: 4 `shell help` test cases pass; no warnings.

- [ ] **Step 7: Commit**

```bash
git add cpp/tests/gen_shell_help.py cpp/src/shell_help_data.inc cpp/include/meradb/repl_text.h cpp/src/repl_text.cpp cpp/tests/test_repl_text.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add the shell help text, generated from the Python reference"
```

**Completion checklist:**
- [ ] `python cpp/tests/gen_shell_help.py` produces no diff
- [ ] `.help` plain and coloured both match Python byte for byte (the golden test)
- [ ] suite green, zero warnings

---

### Task 5: Banner and prompts

**Files:**
- Modify: `cpp/include/meradb/repl_text.h`, `cpp/src/repl_text.cpp`
- Test: `cpp/tests/test_repl_text.cpp` (append)

**Interfaces:**
- Produces: `repl::printBanner(out, style, version, where, pauseMs)`, `repl::promptFor(style, db,
  inTransaction)`, `repl::kContinuationPrompt`. The version is passed in (`"1.0.0"`); Task 10 supplies it.
- Block glyph `█` (U+2588) and middle dot `·` (U+00B7) are always used (Design decision D4).

- [ ] **Step 1: Write the failing test** (append to `cpp/tests/test_repl_text.cpp`)

```cpp
TEST_CASE("shell banner matches Python, plain and coloured", "[shell]") {
    for (bool color : {false, true}) {
        INFO((color ? "colour" : "plain"));
        std::ostringstream out;
        repl::printBanner(out, term::Style(color), "1.0.0", "local (/data/x)", 0);
        CHECK(out.str() == golden_shell::get("banner", color));
        std::ostringstream server;
        repl::printBanner(server, term::Style(color), "1.0.0", "127.0.0.1:6372", 0);
        CHECK(server.str() == golden_shell::get("banner_server", color));
    }
}

TEST_CASE("shell banner reveal pauses only when asked to", "[shell]") {
    std::ostringstream out;
    const auto start = std::chrono::steady_clock::now();
    repl::printBanner(out, term::Style::none(), "1.0.0", "x", 5);  // 12 logo/wordmark lines x 5 ms
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed >= std::chrono::milliseconds(50));
    CHECK(out.str().find("connected: x") != std::string::npos);
}

TEST_CASE("shell prompts match Python", "[shell]") {
    for (bool color : {false, true}) {
        INFO((color ? "colour" : "plain"));
        const term::Style style(color);
        CHECK(repl::promptFor(style, "main", false) == golden_shell::get("prompt_main", color));
        CHECK(repl::promptFor(style, "college", true) == golden_shell::get("prompt_txn", color));
    }
    CHECK(std::string(repl::kContinuationPrompt) == "      ...> ");
}
```

Run `cmake --build cpp/build`. Expected: FAIL — `printBanner`, `promptFor`, `kContinuationPrompt` undeclared.

- [ ] **Step 2: Add the declarations to `repl_text.h`**

Append before the closing `}  // namespace meradb::repl`:

```cpp
// Python's print_banner(version, where). With pauseMs > 0 each logo and wordmark line is flushed and
// followed by that pause (the "reveal" animation, which Python plays only when colour is on).
void printBanner(std::ostream& out, const term::Style& style, const std::string& version, const std::string& where,
                 int pauseMs);

// "meradb:<db>> ", or "meradb:<db>*> " while a transaction is open.
std::string promptFor(const term::Style& style, const std::string& db, bool inTransaction);

// Shown while a statement is still being typed.
inline constexpr const char* kContinuationPrompt = "      ...> ";
```

- [ ] **Step 3: Add the implementation to `repl_text.cpp`**

Add `#include <chrono>`, `#include <functional>` and `#include <thread>` to the includes, and append this
before the closing `}  // namespace meradb::repl`:

```cpp
// ---------------------------------------------------------------------------
// banner
// ---------------------------------------------------------------------------

namespace {

const char* const kBlock = "\xE2\x96\x88";  // U+2588 FULL BLOCK
const char* const kDot = "\xC2\xB7";         // U+00B7 MIDDLE DOT

// A hand-authored 5x7 dot-matrix font, just for the letters M E R A D B. 1 = filled pixel.
struct Glyph {
    char letter;
    const char* rows[7];
};

const Glyph kFont[] = {
    {'M', {"1...1", "11.11", "1.1.1", "1.1.1", "1...1", "1...1", "1...1"}},
    {'E', {"11111", "1....", "1....", "1111.", "1....", "1....", "11111"}},
    {'R', {"1111.", "1...1", "1...1", "1111.", "1.1..", "1..1.", "1...1"}},
    {'A', {".111.", "1...1", "1...1", "11111", "1...1", "1...1", "1...1"}},
    {'D', {"1111.", "1...1", "1...1", "1...1", "1...1", "1...1", "1111."}},
    {'B', {"1111.", "1...1", "1...1", "1111.", "1...1", "1...1", "1111."}},
};

const Glyph& glyphFor(char letter) {
    for (const Glyph& glyph : kFont)
        if (glyph.letter == letter) return glyph;
    return kFont[0];  // unreachable: only the letters of "MERADB" are asked for
}

const std::vector<std::string> kWordmark = {
    "  __  __                 ____  ____",
    " |  \\/  | ___ _ __ __ _|  _ \\| __ )",
    " | |\\/| |/ _ \\ '__/ _` | | | |  _ \\",
    " | |  | |  __/ | | (_| | |_| | |_) |",
    " |_|  |_|\\___|_|  \\__,_|____/|____/",
};

struct Logo {
    std::vector<std::string> rows;
    int width;  // on-screen width: escape codes are invisible on screen but not to size()
};

// "MERADB" as 7 lines of block letters, split and coloured as MERA (bold cyan) + DB (bold magenta). Each
// pixel is two blocks wide, because a terminal cell is taller than it is wide.
Logo renderLogo(const term::Style& style) {
    const char* const halves[2] = {"MERA", "DB"};
    std::vector<std::string> rows(7);
    int width = 0;
    for (int half = 0; half < 2; ++half) {
        const std::string letters = halves[half];
        for (std::size_t i = 0; i < letters.size(); ++i) {
            const Glyph& glyph = glyphFor(letters[i]);
            for (int r = 0; r < 7; ++r) {
                std::string pixels;
                for (const char* px = glyph.rows[r]; *px != '\0'; ++px)
                    pixels += *px == '1' ? std::string(kBlock) + kBlock : std::string("  ");
                rows[static_cast<std::size_t>(r)] += half == 0 ? style.c(pixels, {1, 36}) : style.c(pixels, {1, 35});
            }
            width += 10;  // each glyph is 5 pixels, drawn 2 characters wide
            if (i + 1 != letters.size()) {
                for (auto& row : rows) row += " ";  // gap between letters of the same half
                width += 1;
            }
        }
        if (half == 0) {
            for (auto& row : rows) row += "   ";  // wider gap between MERA and DB
            width += 3;
        }
    }
    return {rows, width};
}

// Indents every line by the SAME amount, so the block is centred under `width` without disturbing its own
// internal alignment (centring each line separately would ruin the slanted wordmark).
std::vector<std::string> centerBlock(const std::vector<std::string>& lines, int width,
                                     const std::function<std::string(const std::string&)>& paint) {
    std::size_t inner = 0;
    for (const auto& line : lines) inner = std::max(inner, pytext::length(line));
    const int pad = std::max(0, (width - static_cast<int>(inner)) / 2);
    std::vector<std::string> out;
    for (const auto& line : lines) out.push_back(std::string(static_cast<std::size_t>(pad), ' ') + paint(line));
    return out;
}

}  // namespace

void printBanner(std::ostream& out, const term::Style& style, const std::string& version, const std::string& where,
                 int pauseMs) {
    const Logo logo = renderLogo(style);
    const auto wordmark = centerBlock(kWordmark, logo.width, [&](const std::string& s) { return style.c(s, {2}); });  // dim
    const auto taglineBlock = centerBlock({"meraDB " + version + " -- apna database, apni bhasha."}, logo.width,
                                          [&](const std::string& s) { return style.c(s, {1}); });
    const auto reveal = [&](const std::vector<std::string>& lines) {
        for (const auto& line : lines) {
            out << line << "\n";
            if (pauseMs > 0) {
                out.flush();
                std::this_thread::sleep_for(std::chrono::milliseconds(pauseMs));
            }
        }
    };

    out << "\n";
    reveal(logo.rows);
    out << "\n";
    reveal(wordmark);
    out << "\n";
    out << taglineBlock[0] << "\n";
    out << "  connected: " << where << "\n";
    const std::string hint = "  " + style.c(".help", {1, 33}) + " commands  " + style.c(kDot, {2}) + "  " +
                             style.c(".exit", {1, 33}) + " bahar niklo  " + style.c(kDot, {2}) + "  statements " +
                             style.c(";", {1, 33}) + " se khatam hote hain\n";
    out << hint;
}

std::string promptFor(const term::Style& style, const std::string& db, bool inTransaction) {
    const std::string marker = inTransaction ? style.c("*", {1, 33}) : std::string();
    return "meradb:" + style.c(db, {1, 36}) + marker + "> ";
}
```

- [ ] **Step 4: Build and run**

```bash
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "shell"
```
Expected: all 7 `shell` test cases (4 from Task 4, 3 new) pass.

- [ ] **Step 5: Commit**

```bash
git add cpp/include/meradb/repl_text.h cpp/src/repl_text.cpp cpp/tests/test_repl_text.cpp
git commit -m "Add the shell banner and prompts"
```

**Completion checklist:**
- [ ] banner, prompts, help match Python's own output byte for byte in both colour modes
- [ ] the reveal pause happens only when `pauseMs > 0` (the caller will pass 40 only when colour is on)
- [ ] suite green, zero warnings

---

# BATCH B — the shell itself: input, running text, dot-commands, the loop, the command

### Task 6: `LineSource`: stream and console readers

**Files:**
- Create: `cpp/include/meradb/repl.h`, `cpp/src/repl.cpp`
- Test: `cpp/tests/test_repl_input.cpp`
- Modify: `cpp/CMakeLists.txt` (add `src/repl.cpp` to `meradb_core`), `cpp/tests/CMakeLists.txt` (add `test_repl_input.cpp`)

**Interfaces:**
- Consumes: `sys::ReadStatus`, `sys::readTerminalLine`, `sys::InterruptGuard` (Task 2).
- Produces `repl::LineSource` (D3), `repl::StreamLineSource` (any `std::istream`) and `repl::ConsoleLineSource`
  (a real terminal). A source writes the prompt itself, to the stream it is given, as Python's
  `input(prompt)` does; so prompts and results interleave in a pipe exactly as Python's do.
- A stream source ends a line at `"\n"`, `"\r\n"` or a lone `"\r"` (Python's universal newlines), counts a
  last line with no terminator, and passes every byte through untouched: no BOM stripping (Python keeps the
  BOM of piped input too), `0x1A` is an ordinary character, a NUL is an ordinary character.

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_repl_input.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/repl.h"
#include <sstream>

using namespace meradb;

namespace {

// Reads everything the source has, as "line|line|...|EOF", and what it wrote as prompts.
std::string drain(const std::string& input, std::string* prompts = nullptr, const std::string& prompt = "> ") {
    std::istringstream in(input);
    std::ostringstream out;
    repl::StreamLineSource source(in, out);
    std::string result;
    for (;;) {
        std::string line;
        const auto status = source.read(prompt, line);
        if (status == sys::ReadStatus::Eof) {
            result += "EOF";
            break;
        }
        REQUIRE(status == sys::ReadStatus::Line);
        result += line + "|";
    }
    if (prompts) *prompts = out.str();
    return result;
}

}  // namespace

TEST_CASE("shell input: a line ends at LF, CRLF or a lone CR", "[shell]") {
    CHECK(drain("a\nb\r\nc\rd\n\ne") == "a|b|c|d||e|EOF");
    CHECK(drain("a\r\n\r\nb\r\n") == "a||b|EOF");
    CHECK(drain("a\r") == "a|EOF");
    CHECK(drain("\r\r\n\n") == "|||EOF");
}

TEST_CASE("shell input: a last line without a terminator still counts", "[shell]") {
    CHECK(drain("x") == "x|EOF");
    CHECK(drain("x\n") == "x|EOF");
    CHECK(drain("") == "EOF");
    CHECK(drain("\n") == "|EOF");
}

TEST_CASE("shell input: the prompt is written before every read, including the one that finds the end", "[shell]") {
    std::string prompts;
    drain("a\nb\n", &prompts, "P ");
    CHECK(prompts == "P P P ");  // a, b, then end of input
}

TEST_CASE("shell input: bytes pass through untouched", "[shell]") {
    CHECK(drain(std::string("caf\xC3\xA9 \xF0\x9F\x98\x80\n")) == "caf\xC3\xA9 \xF0\x9F\x98\x80|EOF");
    CHECK(drain(std::string("a\x1a" "b\n")) == "a\x1a" "b|EOF");  // Ctrl+Z is an ordinary character in a pipe
    CHECK(drain(std::string("\xEF\xBB\xBF" "x\n")) == "\xEF\xBB\xBF" "x|EOF");  // a BOM is not stripped (Python keeps it too)
    const std::string withNul("a\0b\n", 4);
    CHECK(drain(withNul) == std::string("a\0b|EOF", 7));
}

TEST_CASE("shell input: a stream source never reports a pending interrupt", "[shell]") {
    std::istringstream in("x\n");
    std::ostringstream out;
    repl::StreamLineSource source(in, out);
    CHECK_FALSE(source.takePendingInterrupt());
}

TEST_CASE("shell input: a console source reports Ctrl+C that arrived while it was not reading", "[shell]") {
    sys::InterruptGuard guard;
    std::ostringstream out;
    repl::ConsoleLineSource source(out);
    CHECK_FALSE(source.takePendingInterrupt());
    sys::InterruptGuard::trigger();
    CHECK(source.takePendingInterrupt());
    CHECK_FALSE(source.takePendingInterrupt());  // once per Ctrl+C
}
```

In `cpp/tests/CMakeLists.txt` add `test_repl_input.cpp` before the `# test files are appended here` line, and
in `cpp/CMakeLists.txt` add `src/repl.cpp` after `src/repl_text.cpp`, then re-run the configure command.

Run `cmake --build cpp/build`. Expected: FAIL — `meradb/repl.h` not found.

- [ ] **Step 2: Create `cpp/include/meradb/repl.h`**

```cpp
// cpp/include/meradb/repl.h
//
// The interactive shell (mirrors meradb/repl.py): read lines, collect a statement until `;`, run it on a
// Backend, print the results. Everything prints to a `std::ostream&` and reads from a `LineSource`, so the
// whole loop runs against scripted input in tests; only ConsoleLineSource touches a real terminal.
#pragma once
#include "meradb/sys_compat.h"
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

}  // namespace meradb::repl
```

- [ ] **Step 3: Create `cpp/src/repl.cpp`**

```cpp
// cpp/src/repl.cpp -- see repl.h.
#include "meradb/repl.h"

namespace meradb::repl {

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------

ReadStatus StreamLineSource::read(const std::string& prompt, std::string& line) {
    out_ << prompt << std::flush;
    line.clear();
    bool any = false;
    for (;;) {
        const int c = in_.get();
        if (c == std::istream::traits_type::eof()) break;
        any = true;
        if (c == '\n') return ReadStatus::Line;
        if (c == '\r') {
            if (in_.peek() == '\n') in_.get();
            return ReadStatus::Line;
        }
        line.push_back(static_cast<char>(c));
    }
    return any ? ReadStatus::Line : ReadStatus::Eof;
}

ReadStatus ConsoleLineSource::read(const std::string& prompt, std::string& line) {
    out_ << prompt << std::flush;
    return sys::readTerminalLine(line);
}

}  // namespace meradb::repl
```

- [ ] **Step 4: Build and run**

```bash
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "shell input"
```
Expected: 6 test cases pass (39 assertions in the Catch2 summary when run directly).

- [ ] **Step 5: Commit**

```bash
git add cpp/include/meradb/repl.h cpp/src/repl.cpp cpp/tests/test_repl_input.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add the shell's line sources"
```

**Completion checklist:**
- [ ] CR, LF, CRLF and a missing final newline behave as Python's text-mode stdin
- [ ] the prompt goes to the output stream before every read, also the one that meets end of input
- [ ] nothing in a source touches `std::cin` / `std::cout` (the caller passes the streams)
- [ ] suite green, zero warnings

---

### Task 7: `repl::runText` / `runFile` and the `run` path fix

**Files:**
- Modify: `cpp/include/meradb/repl.h`, `cpp/src/repl.cpp`
- Create: `cpp/tests/repl_test_util.h` (`FakeBackend`, used by Tasks 7-9)
- Create: `cpp/tests/test_repl.cpp`
- Modify: `cpp/tests/CMakeLists.txt` (add `test_repl.cpp`)

**Interfaces:**
- Produces `bool repl::runText(Backend&, text, std::ostream&, const term::Style&)` and
  `bool repl::runFile(Backend&, path, std::ostream&, const term::Style&)` — Python's `run_text` / `run_file`.
  Every result prints (`formatResult`) and is followed by a blank line, **including errors and results with
  nothing to print**; a `MeraDBError` escaping the backend (server connection dropped) prints `str(e)` on
  the output stream and returns false (Design decision D7). Task 10 makes `meradb run` use these.
- `runFile` reads UTF-8 with an optional BOM and any newline style and, for an unreadable file, prints
  `File nahi khuli: [Errno 2] No such file or directory: '<path>'` (or `[Errno 13] Permission denied`), the
  wording Phase 2's `runFile` already had.
- `FakeBackend`: records every script passed to `runScript`, answers with a scriptable `onRun` (default: one
  message result `ok`), and reports a settable current database, transaction state and description.

- [ ] **Step 1: Write the helper and the failing tests**

```cpp
// cpp/tests/repl_test_util.h -- a scriptable Backend for the shell tests.
#pragma once
#include "meradb/backend.h"
#include "meradb/errors.h"
#include <functional>
#include <string>
#include <vector>

namespace meradb_test {

class FakeBackend : public meradb::Backend {
public:
    std::vector<std::string> scripts;  // every text passed to runScript, in order
    // What runScript returns; the default is one message result "ok". May throw.
    std::function<std::vector<meradb::Result>(const std::string&)> onRun = [](const std::string&) {
        meradb::Result r;
        r.message = "ok";
        return std::vector<meradb::Result>{r};
    };
    std::string db = "main";
    bool txn = false;
    std::string where = "fake:1";
    bool closed = false;

    std::vector<meradb::Result> runScript(const std::string& text) override {
        scripts.push_back(text);
        return onRun(text);
    }
    std::vector<meradb::Result> execute(const std::string& text) override { return runScript(text); }
    std::string currentDb() override { return db; }
    bool inTransaction() override { return txn; }
    nlohmann::ordered_json schemaTree() override { return nlohmann::ordered_json::array(); }
    std::string description() override { return where; }
    void close() override { closed = true; }
};

}  // namespace meradb_test
```

```cpp
// cpp/tests/test_repl.cpp -- running text and files, dot-commands and the read-eval-print loop.
#include <catch2/catch_test_macros.hpp>
#include "meradb/repl.h"
#include "meradb/repl_text.h"
#include "repl_test_util.h"
#include "test_util.h"
#include <fstream>
#include <sstream>

using namespace meradb;
using meradb_test::FakeBackend;
using meradb_test::TempDir;

namespace {

void writeBinary(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

std::vector<Result> oneMessage(const std::string& text) {
    Result r;
    r.message = text;
    return {r};
}

const term::Style kPlain = term::Style::none();

}  // namespace

TEST_CASE("shell runText prints each result followed by a blank line", "[shell]") {
    FakeBackend backend;
    backend.onRun = [](const std::string&) {
        Result table;
        table.columns = {"a"};
        table.rows = {{Value(int64_t(1))}};
        table.message = "1 row(s)";
        Result error;
        error.error = "[Execution Galti] boom";
        Result silent;  // nothing to print, but Python still prints the blank line
        return std::vector<Result>{table, error, silent};
    };
    std::ostringstream out;
    CHECK_FALSE(repl::runText(backend, "x;", out, kPlain));
    CHECK(out.str() == "+---+\n| a |\n+---+\n| 1 |\n+---+\n1 row(s)\n\n[Execution Galti] boom\n\n\n");
    CHECK(backend.scripts == std::vector<std::string>{"x;"});
}

TEST_CASE("shell runText is true and silent for an empty script", "[shell]") {
    FakeBackend backend;
    backend.onRun = [](const std::string&) { return std::vector<Result>{}; };
    std::ostringstream out;
    CHECK(repl::runText(backend, ";\n", out, kPlain));
    CHECK(out.str().empty());
}

TEST_CASE("shell runText colours when the style says so", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    CHECK(repl::runText(backend, "x;", out, term::Style::colored()));
    CHECK(out.str() == "\x1b[32mok\x1b[0m\n\n");
}

TEST_CASE("shell runText prints a dropped connection and carries on", "[shell]") {
    FakeBackend backend;
    int calls = 0;
    backend.onRun = [&](const std::string&) -> std::vector<Result> {
        if (++calls == 1) throw ConnectionFailed("Server ne connection band kar diya");
        return oneMessage("back");
    };
    std::ostringstream out;
    CHECK_FALSE(repl::runText(backend, "a;", out, kPlain));
    CHECK(out.str() == "[Connection Galti] Server ne connection band kar diya\n");  // str(e), on stdout
    out.str("");
    CHECK(repl::runText(backend, "b;", out, kPlain));
    CHECK(out.str() == "back\n\n");
}

TEST_CASE("shell runFile reads UTF-8 with a BOM and any newline style", "[shell]") {
    TempDir dir;
    writeBinary(dir.file("s.mdb"), "\xEF\xBB\xBF" "A\r\nB;\rC;");
    FakeBackend backend;
    std::ostringstream out;
    CHECK(repl::runFile(backend, dir.file("s.mdb"), out, kPlain));
    CHECK(backend.scripts == std::vector<std::string>{"A\nB;\nC;"});
}

TEST_CASE("shell runFile reports a missing file with Python's wording", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    CHECK_FALSE(repl::runFile(backend, "definitely_missing_script.mdb", out, kPlain));
    CHECK(out.str() == "File nahi khuli: [Errno 2] No such file or directory: 'definitely_missing_script.mdb'\n");
    CHECK(backend.scripts.empty());
}

TEST_CASE("shell runFile survives a dropped connection and says so on stdout", "[shell]") {
    TempDir dir;
    writeBinary(dir.file("s.mdb"), "X;");
    FakeBackend backend;
    backend.onRun = [](const std::string&) -> std::vector<Result> { throw ConnectionFailed("Server ne connection band kar diya"); };
    std::ostringstream out;
    CHECK_FALSE(repl::runFile(backend, dir.file("s.mdb"), out, kPlain));
    CHECK(out.str() == "[Connection Galti] Server ne connection band kar diya\n");
}
```

Add `test_repl.cpp` to `cpp/tests/CMakeLists.txt` (after `test_repl_input.cpp`) and re-run configure.

Run `cmake --build cpp/build`. Expected: FAIL — `runText` / `runFile` are not members of `meradb::repl`.

- [ ] **Step 2: Extend `repl.h`**

Add `#include "meradb/backend.h"` and `#include "meradb/term_style.h"` to the includes (keep them sorted),
and append before the closing `}  // namespace meradb::repl`:

```cpp
// Python's run_text: run `text` on the backend and print every result followed by a blank line. A statement
// that fails prints its error and the next ones still run. A MeraDBError escaping the backend (the server
// connection dropped) is printed (`str(e)`) and the shell goes on. False if anything went wrong.
bool runText(Backend& backend, const std::string& text, std::ostream& out, const term::Style& style);

// Python's run_file: read a script (UTF-8, an optional byte-order mark, any newline style) and runText it.
// An unreadable file prints "File nahi khuli: <reason>" and returns false.
bool runFile(Backend& backend, const std::string& path, std::ostream& out, const term::Style& style);
```

- [ ] **Step 3: Extend `repl.cpp`**

Replace the include block with:

```cpp
#include "meradb/repl.h"
#include "meradb/cli_format.h"
#include "meradb/errors.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
```

and append before the closing `}  // namespace meradb::repl`:

```cpp
// ---------------------------------------------------------------------------
// running text and files
// ---------------------------------------------------------------------------

namespace {

// Python's text mode (universal newlines): "\r\n" and a lone "\r" both become "\n".
std::string normalizeNewlines(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\r') {
            out += '\n';
            if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
        } else {
            out += in[i];
        }
    }
    return out;
}

}  // namespace

bool runText(Backend& backend, const std::string& text, std::ostream& out, const term::Style& style) {
    std::vector<Result> results;
    try {
        results = backend.runScript(text);
    } catch (const MeraDBError& e) {  // e.g. the server connection dropped
        out << e.what() << "\n";
        return false;
    }
    bool ok = true;
    for (const auto& result : results) {
        const std::string rendered = formatResult(result, style);
        if (!rendered.empty()) out << rendered << "\n";
        out << "\n";
        if (!result.error.empty()) ok = false;
    }
    return ok;
}

bool runFile(Backend& backend, const std::string& path, std::ostream& out, const term::Style& style) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) {
        // Same wording as Python's OSError text: "[Errno 2] No such file or directory: 'x'"
        std::string quoted;
        for (char c : path) {
            if (c == '\\' || c == '\'') quoted += '\\';
            quoted += c;
        }
        std::error_code ec;
        const bool missing = !std::filesystem::exists(std::filesystem::u8path(path), ec);
        out << "File nahi khuli: " << (missing ? "[Errno 2] No such file or directory: '" : "[Errno 13] Permission denied: '")
            << quoted << "'\n";
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string text = buffer.str();
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);  // utf-8-sig, like Python
    return runText(backend, normalizeNewlines(text), out, style);
}
```

`MeraDBError::what()` already carries the `[Stage Galti]` prefix, which is what Python's `str(e)` prints.

- [ ] **Step 4: Build and run**

```bash
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "shell run"
```
Expected: 7 test cases pass (`shell runText` x4, `shell runFile` x3).

- [ ] **Step 5: Commit**

```bash
git add cpp/include/meradb/repl.h cpp/src/repl.cpp cpp/tests/repl_test_util.h cpp/tests/test_repl.cpp cpp/tests/CMakeLists.txt
git commit -m "Add runText and runFile with Python's dropped-connection handling"
```

**Completion checklist:**
- [ ] a result with nothing to print still produces its blank line (Python does)
- [ ] a failing statement does not stop the later ones in the same script (the backend returns them all)
- [ ] the dropped-connection message goes to the output stream, not stderr (Python prints it to stdout)
- [ ] `runFile` and the old Phase 2 `runFile` print the same text for a missing file (Task 10 deletes the old one)
- [ ] suite green, zero warnings

---

### Task 8: Dot-commands

**Files:**
- Modify: `cpp/include/meradb/repl.h`, `cpp/src/repl.cpp`
- Test: `cpp/tests/test_repl.cpp` (append; add `#include "golden_shell.h"` at the top)

**Interfaces:**
- Consumes: `pytext::split/strip/lowerAscii` (Task 1), `renderReference` / `fullHelp` (Task 4), `runText` /
  `runFile` (Task 7).
- Produces `bool repl::handleDotCommand(Backend&, const std::string& line, std::ostream&, const term::Style&)`
  (`line` is stripped and starts with `.`; false means "leave the shell") and
  `bool repl::endsStatement(const std::string& buffer)`, `buffer.rstrip().endswith(";")` with Python's
  whitespace (Task 9's loop uses it; it lives here because it is the other half of the dot-command rule).
- Rules (D2 items 8-9): the command word is `split()[0].lower()` (full Unicode lowering, `pytext::lower`, which replaced the original ASCII-only `lowerAscii`);
  `.exit` / `.quit` / `.nikal` leave, extra words ignored; `.help` takes everything after the command word,
  stripped, as its topic; `.tables` runs `DIKHAO TABLES;`; `.schema X` runs `BATAO X;` with the **first**
  argument only; `.run F` runs file `F` (first argument only, so a path with spaces is cut); `.schema` and
  `.run` with no argument fall into "unknown", as in Python; anything else prints
  `Ye shell command nahi pata: <line>  (.help dekho)`. `.hexdump` is **not** implemented (out of scope).

- [ ] **Step 1: Write the failing tests** (append to `cpp/tests/test_repl.cpp`; add `#include "golden_shell.h"` next to the other includes)

```cpp
TEST_CASE("shell dot-commands: .exit, .quit and .nikal leave, in any case, ignoring extra words", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    for (const char* line : {".exit", ".quit", ".nikal", ".EXIT", ".Quit now", ".nikal   please"})
        CHECK_FALSE(repl::handleDotCommand(backend, line, out, kPlain));
    CHECK(out.str().empty());
    CHECK(backend.scripts.empty());
}

TEST_CASE("shell dot-commands: .tables and .schema run the matching statements", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    CHECK(repl::handleDotCommand(backend, ".tables", out, kPlain));
    CHECK(repl::handleDotCommand(backend, ".schema students", out, kPlain));
    CHECK(repl::handleDotCommand(backend, ".SCHEMA a b c", out, kPlain));  // only the first word counts
    CHECK(backend.scripts == std::vector<std::string>{"DIKHAO TABLES;", "BATAO students;", "BATAO a;"});
    CHECK(out.str() == "ok\n\nok\n\nok\n\n");
}

TEST_CASE("shell dot-commands: .schema and .run need an argument, otherwise they are unknown", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    CHECK(repl::handleDotCommand(backend, ".schema", out, kPlain));
    CHECK(repl::handleDotCommand(backend, ".run", out, kPlain));
    CHECK(out.str() ==
          "Ye shell command nahi pata: .schema  (.help dekho)\n"
          "Ye shell command nahi pata: .run  (.help dekho)\n");
    CHECK(backend.scripts.empty());
}

TEST_CASE("shell dot-commands: .run runs a file, cut at the first space", "[shell]") {
    TempDir dir;
    writeBinary(dir.file("one.mdb"), "DIKHAO TABLES;");
    FakeBackend backend;
    std::ostringstream out;
    CHECK(repl::handleDotCommand(backend, ".run " + dir.file("one.mdb") + " ignored words", out, kPlain));
    CHECK(backend.scripts == std::vector<std::string>{"DIKHAO TABLES;"});
    out.str("");
    CHECK(repl::handleDotCommand(backend, ".run nodir/missing.mdb", out, kPlain));
    CHECK(out.str() == "File nahi khuli: [Errno 2] No such file or directory: 'nodir/missing.mdb'\n");
}

TEST_CASE("shell dot-commands: .help prints the whole reference or just the matches", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    CHECK(repl::handleDotCommand(backend, ".help", out, kPlain));
    CHECK(out.str() == golden_shell::get("help_full", false));
    out.str("");
    CHECK(repl::handleDotCommand(backend, ".HELP  join  ", out, kPlain));  // the topic is what follows the command, stripped
    CHECK(out.str() == golden_shell::get("help_join", false));
    out.str("");
    CHECK(repl::handleDotCommand(backend, ".help foreign key", out, kPlain));
    CHECK(out.str() == golden_shell::get("help_two_words", false));
    out.str("");
    CHECK(repl::handleDotCommand(backend, ".help zzzz", out, kPlain));
    CHECK(out.str() == golden_shell::get("help_none", false));
    CHECK(backend.scripts.empty());
}

TEST_CASE("shell dot-commands: anything else is unknown, including .hexdump", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    CHECK(repl::handleDotCommand(backend, ".bogus a b", out, kPlain));
    CHECK(repl::handleDotCommand(backend, ".hexdump students", out, kPlain));  // a ROADMAP exercise, not ours: unknown here too
    CHECK(repl::handleDotCommand(backend, ".", out, kPlain));
    CHECK(out.str() ==
          "Ye shell command nahi pata: .bogus a b  (.help dekho)\n"
          "Ye shell command nahi pata: .hexdump students  (.help dekho)\n"
          "Ye shell command nahi pata: .  (.help dekho)\n");
}
```

Run `cmake --build cpp/build`. Expected: FAIL — `handleDotCommand` is not a member of `meradb::repl`.

- [ ] **Step 2: Extend `repl.h`** (append before the closing namespace)

```cpp
// Whether the text typed so far ends a statement: Python's buffer.rstrip().endswith(";"). It does not look
// inside strings or comments (a line that ends in `;` inside a string literal ends it too).
bool endsStatement(const std::string& buffer);

// Python's handle_dot_command(backend, line); `line` is already stripped and starts with ".". False when the
// user asked to leave (.exit, .quit, .nikal).
bool handleDotCommand(Backend& backend, const std::string& line, std::ostream& out, const term::Style& style);
```

- [ ] **Step 3: Extend `repl.cpp`**

Add `#include "meradb/pytext.h"` and `#include "meradb/repl_text.h"` to the includes, and append before the
closing namespace:

```cpp
// ---------------------------------------------------------------------------
// the shell
// ---------------------------------------------------------------------------

bool endsStatement(const std::string& buffer) { return pytext::endsWith(pytext::rstrip(buffer), ";"); }

bool handleDotCommand(Backend& backend, const std::string& line, std::ostream& out, const term::Style& style) {
    const std::vector<std::string> parts = pytext::split(line);
    if (parts.empty()) return true;  // cannot happen: the caller passes a stripped line that starts with "."
    const std::string cmd = pytext::lowerAscii(parts[0]);
    if (cmd == ".exit" || cmd == ".quit" || cmd == ".nikal") return false;
    if (cmd == ".help") {
        const std::string topic = pytext::strip(line.substr(parts[0].size()));  // everything after ".help"
        if (!topic.empty())
            out << renderReference(style, topic) << "\n";
        else
            out << fullHelp(style) << "\n";
    } else if (cmd == ".tables") {
        runText(backend, "DIKHAO TABLES;", out, style);
    } else if (cmd == ".schema" && parts.size() > 1) {
        runText(backend, "BATAO " + parts[1] + ";", out, style);
    } else if (cmd == ".run" && parts.size() > 1) {
        runFile(backend, parts[1], out, style);
    } else {
        out << "Ye shell command nahi pata: " << line << "  (.help dekho)\n";
    }
    return true;
}
```

`renderReference` and `fullHelp` are the Task 4 functions (`.help <topic>` filter and the whole reference);
the `\n` after them reproduces `print(...)`.

- [ ] **Step 4: Build and run**

```bash
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "shell dot-commands"
```
Expected: 6 test cases pass.

- [ ] **Step 5: Commit**

```bash
git add cpp/include/meradb/repl.h cpp/src/repl.cpp cpp/tests/test_repl.cpp
git commit -m "Add the shell's dot-commands"
```

**Completion checklist:**
- [ ] `.schema` / `.run` without an argument are unknown commands, as in Python
- [ ] `.help` output equals the Python goldens byte for byte (plain and, via Task 4, coloured)
- [ ] `.hexdump` is unknown (nothing from the ROADMAP exercises crept in)
- [ ] suite green, zero warnings

---

### Task 9: The read-eval-print loop

**Files:**
- Modify: `cpp/include/meradb/repl.h`, `cpp/src/repl.cpp`
- Create: `cpp/tests/shell_scripts.py`, `cpp/tests/gen_shell_transcripts.py`; generated and committed:
  `cpp/tests/golden_transcripts.h`
- Test: `cpp/tests/test_repl.cpp` (append; add `#include "golden_transcripts.h"` at the top)

**Interfaces:**
- Consumes: everything above plus `printBanner` / `promptFor` (Task 5).
- Produces `int repl::run(Backend&, LineSource&, std::ostream&, const term::Style&, version, revealPauseMs)`
  — Python's `repl(backend)`. Returns the process exit code: 0 normally, 130 when Ctrl+C arrived while a
  statement was running (D5).
- Produces the **transcript goldens**: `shell_scripts.py` holds 31 named input scripts (statements,
  multi-line input, errors, dot-commands, the blank-line quirk, newline styles, end of input in every
  place); `gen_shell_transcripts.py` pipes each into Python's real `meradb shell --local` and records what
  it printed after the banner. Task 11 reuses `shell_scripts.py` to diff the two real programs.

- [ ] **Step 1: Write the scripts and the generator**

```python
# cpp/tests/shell_scripts.py
"""
The scripts typed into the shell by the shell tests: gen_shell_transcripts.py records what the Python shell
prints for each (golden_transcripts.h, replayed by the C++ unit tests) and shell_diff.py drives both real
shells with the same text and diffs everything they print.

Non-ASCII characters are built with chr() so this file stays plain ASCII.
"""

E_ACUTE = chr(0xE9)
SMILE = chr(0x1F600)
NAMASTE = "".join(chr(c) for c in (0x928, 0x92E, 0x938, 0x94D, 0x924, 0x947))
NBSP = chr(0xA0)
IDEOGRAPHIC_SPACE = chr(0x3000)

SCRIPTS = {
    # ---- statements, multi-line input, results -------------------------------------------------------
    "basic": (
        "BANAO TABLE t (id INT MUKHYA KUNJI, n TEXT);\n"
        "DAALO MEIN t (id, n) MAAN (1, 'Ravi'),\n"
        "  (2, 'Priya');\n"
        "DIKHAO * SE t;\n"
        ".tables\n"
        ".schema t\n"
    ),
    "unicode": (
        "BANAO TABLE u (id INT, n TEXT);\n"
        f"DAALO MEIN u MAAN (1, '{E_ACUTE}{SMILE}'), (2, '{NAMASTE}'), (3, 'plain');\n"
        "DIKHAO * SE u;\n"
        "DIKHAO n SE u JAHAN id = 2;\n"
    ),
    "long_multi_line": (
        "BANAO TABLE m (a INT, b TEXT);\n"
        "DAALO MEIN m MAAN (1, 'x'), (2, 'y'), (3, 'z');\n"
        "DIKHAO\n"
        "  a,\n"
        "  b\n"
        "SE m\n"
        "JAHAN a > 1\n"
        "KRAM a ULTA\n"
        ";\n"
    ),
    "several_statements_one_line": "DIKHAO TABLES; BANAO TABLE a (x INT); DIKHAO TABLES; BATAO nosuch;\n",
    "samjhao": "BANAO TABLE e (id INT);\nSAMJHAO DIKHAO * SE e JAHAN id = 1;\n",
    "use_database": (
        "BANAO DATABASE college;\nISTEMAL college;\nBANAO TABLE s (id INT);\nDIKHAO TABLES;\nISTEMAL main;\nDIKHAO TABLES;\n"
    ),
    "transaction": (
        "SHURU;\nBANAO TABLE x (a INT);\nDAALO MEIN x MAAN (1);\nDIKHAO * SE x;\nWAPAS;\nDIKHAO TABLES;\nSHURU;\nPAKKA;\n"
    ),
    "trigger_one_line": (
        "BANAO TABLE s (id INT, n TEXT);\n"
        "BANAO TABLE log (msg TEXT);\n"
        "BANAO TRIGGER t1 BAAD DAALO PAR s SHURU DAALO MEIN log MAAN ('naya'); KHATAM;\n"
        "DAALO MEIN s MAAN (1, 'a');\n"
        "DIKHAO * SE log;\n"
    ),
    # a body written over several lines is cut at its first `;` (the shell only looks at line ends)
    "trigger_multi_line": (
        "BANAO TABLE s (id INT);\nBANAO TABLE log (msg TEXT);\n"
        "BANAO TRIGGER t1 BAAD DAALO PAR s SHURU\n  DAALO MEIN log MAAN ('x');\nKHATAM;\n.tables\n"
    ),
    "users_local": "BANAO USER ravi GUPT 'pw';\nADHIKAR DO SAB PAR t KO ravi;\nDIKHAO TABLES;\n",
    # ---- errors ----------------------------------------------------------------------------------------
    "errors": (
        "DIKHAO * SE gayab;\n"
        "DIKHAO 1;\n"
        "DAALO MEIN\n"
        "BANAO TABLE a (x INT);\n"
        "BANAO TABLE a (x INT);\n"
        "BANAO TABLE a (x INT);\n"
        "DIKHAO * SE gayab; BANAO TABLE b (y INT); DIKHAO * SE b;\n"
    ),
    "string_semicolon": "DIKHAO 'a;\nb';\n",
    "unterminated_string": "DIKHAO 'bina band;\nDIKHAO TABLES;\n",
    # ---- dot-commands ------------------------------------------------------------------------------------
    "dot_commands": (
        ".help join\n.HELP  foreign key\n.help zzzz\n.tables\n.schema nosuch\n.schema\n.run\n"
        ".run no_such_file.mdb\n.run nodir/file.mdb\n.bogus arg\n.exit now\nDIKHAO TABLES;\n"
    ),
    "help_full": ".help\n.exit\n",
    "help_topics": ".HELP SANDARBH\n.help  Foreign   KEY \n.help   \n.exit\n",
    "quit": ".quit\n",
    "nikal": ".NIKAL\n",
    "indented_dot": "   .tables  \n\t.exit\n",
    "dot_inside_statement": "DIKHAO\n.tables\n;\n",
    # ---- the "a blank line starts a statement" quirk ---------------------------------------------------------
    "blank_line_quirk": "\n.tables\n;\n.tables\n",
    "semicolon_only": ";\n  ;  \n.tables\n",
    "comments": "-- hi;\nDIKHAO 1 SE x;\n-- trailing\n",
    "unicode_space": (
        f"BANAO TABLE w (id INT);{NBSP}\nDIKHAO * SE w ;{IDEOGRAPHIC_SPACE}\n.tables\n"
    ),
    # ---- end of input -------------------------------------------------------------------------------------
    "eof_empty": "",
    "eof_in_statement": "DIKHAO * SE",
    "eof_partial_last_line": "BANAO TABLE p (x INT);",
    "eof_after_newline": "BANAO TABLE p (x INT);\n",
    # ---- newline styles and stray characters ------------------------------------------------------------------
    "crlf": "BANAO TABLE c (id INT);\r\nDAALO MEIN c MAAN (1);\r\nDIKHAO * SE c;\r\n",
    "lone_cr": "BANAO TABLE c (id INT);\rDAALO MEIN c MAAN (1);\rDIKHAO * SE c;\r",
    "ctrl_z_in_pipe": "BANAO TABLE z (id INT);\n\x1a\n.tables\n;\n",
}
```

```python
# cpp/tests/gen_shell_transcripts.py
"""
Regenerates cpp/tests/golden_transcripts.h: for every script in shell_scripts.py, what `python -m meradb
shell --local` prints (after the banner) when the script is piped into it. The C++ unit tests replay the same
input through repl::run and compare.

    python cpp/tests/gen_shell_transcripts.py
"""
import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from gen_shell_golden import literal  # noqa: E402
from shell_scripts import SCRIPTS  # noqa: E402

ROOT = HERE.parents[1]
DEFAULT_OUT = HERE / "golden_transcripts.h"
BANNER_END = "se khatam hote hain\n\n"  # the last banner line, then the blank line the shell prints


def python_shell(script):
    """What the Python shell prints for `script` on stdin, with CRLF normalised and the banner cut off."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_") and k != "NO_COLOR"}
    env.update(PYTHONIOENCODING="utf-8", PYTHONPATH=str(ROOT), NO_COLOR="1")
    with tempfile.TemporaryDirectory() as data:
        done = subprocess.run([sys.executable, "-m", "meradb", "shell", "--local", "--data", data],
                              input=script.encode("utf-8"), capture_output=True, env=env, cwd=str(ROOT), timeout=120)
    assert done.returncode == 0, (done.returncode, done.stderr)
    text = done.stdout.decode("utf-8").replace("\r\n", "\n")
    head, marker, rest = text.partition(BANNER_END)
    assert marker, "banner end not found"
    return rest


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    lines = [
        "// cpp/tests/golden_transcripts.h -- GENERATED by gen_shell_transcripts.py: what the Python shell prints (after",
        "// its banner) for each script in shell_scripts.py. Do not edit by hand.",
        "#pragma once",
        "#include <stdexcept>",
        "#include <string>",
        "",
        "namespace golden_transcripts {",
        "",
        "struct Transcript {",
        "    const char* name;",
        "    std::string input;",
        "    std::string output;",
        "};",
        "",
        "inline const Transcript* all(int& count) {",
        "    static const Transcript data[] = {",
    ]
    for name, script in SCRIPTS.items():
        lines.append('        {"%s",\n        %s,\n        %s},' % (name, literal(script) if script else '""', literal(python_shell(script))))
    lines += [
        "    };",
        f"    count = {len(SCRIPTS)};",
        "    return data;",
        "}",
        "",
        "inline const Transcript& get(const std::string& name) {",
        "    int count = 0;",
        "    const Transcript* data = all(count);",
        "    for (int i = 0; i < count; ++i)",
        "        if (name == data[i].name) return data[i];",
        '    throw std::runtime_error("no golden transcript " + name);',
        "}",
        "",
        "}  // namespace golden_transcripts",
        "",
    ]
    args.out.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    print(f"wrote {args.out} ({len(SCRIPTS)} transcripts)")


if __name__ == "__main__":
    main()
```

`literal` is the C++ string-literal escaper Task 3 put in `gen_shell_golden.py` (it splits after every
hexadecimal escape so a following hex digit is not swallowed, and keeps lines short).

```bash
python cpp/tests/gen_shell_transcripts.py
```
Expected: `wrote .../cpp/tests/golden_transcripts.h (31 transcripts)`. Open the file and read three
transcripts (`blank_line_quirk`, `eof_in_statement`, `dot_commands`) against the rules in D2 before going
on: the generator, not the plan text, is the oracle, but a misread of the rules shows up here first.

- [ ] **Step 2: Write the failing tests** (append to `cpp/tests/test_repl.cpp`; add `#include "golden_transcripts.h"` next to the other includes)

```cpp
// A scripted terminal: hands out lines, then end of input (or Ctrl+C at the prompt).
namespace {

class ScriptedSource : public repl::LineSource {
public:
    ScriptedSource(std::vector<std::string> lines, std::ostream& out) : lines_(std::move(lines)), out_(out) {}

    sys::ReadStatus read(const std::string& prompt, std::string& line) override {
        out_ << prompt;
        ++reads;
        if (next_ >= lines_.size()) return interruptAtEnd ? sys::ReadStatus::Interrupted : sys::ReadStatus::Eof;
        line = lines_[next_++];
        return sys::ReadStatus::Line;
    }
    bool takePendingInterrupt() override {
        const bool was = pending;
        pending = false;
        return was;
    }

    bool pending = false;         // "Ctrl+C arrived while a statement was running"
    bool interruptAtEnd = false;  // the end of the script is a Ctrl+C at the prompt
    int reads = 0;

private:
    std::vector<std::string> lines_;
    std::size_t next_ = 0;
    std::ostream& out_;
};

std::string bannerFor(const std::string& where) {
    std::ostringstream out;
    repl::printBanner(out, kPlain, "1.0.0", where, 0);
    return out.str() + "\n";
}

}  // namespace

TEST_CASE("shell loop: banner, prompt, then end of input says goodbye", "[shell]") {
    FakeBackend backend;
    backend.where = "local (/data/x)";
    std::ostringstream out;
    ScriptedSource source({}, out);
    CHECK(repl::run(backend, source, out, kPlain, "1.0.0", 0) == 0);
    CHECK(out.str() == golden_shell::get("banner", false) + "\n" + "meradb:main> " + "\nPhir milenge!\n");
}

TEST_CASE("shell loop: the prompt shows the database and an open transaction", "[shell]") {
    FakeBackend backend;
    backend.db = "college";
    backend.txn = true;
    std::ostringstream out;
    ScriptedSource source({}, out);
    repl::run(backend, source, out, kPlain, "1.0.0", 0);
    CHECK(out.str() == bannerFor("fake:1") + "meradb:college*> \nPhir milenge!\n");
}

TEST_CASE("shell loop: a statement is collected until a line ends with a semicolon", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({"DIKHAO", "  1  ", "SE t ;  ", "DIKHAO 2;"}, out);
    repl::run(backend, source, out, kPlain, "1.0.0", 0);
    CHECK(backend.scripts == std::vector<std::string>{"DIKHAO\n  1  \nSE t ;  \n", "DIKHAO 2;\n"});
    CHECK(out.str() == bannerFor("fake:1") + "meradb:main>       ...>       ...> ok\n\nmeradb:main> ok\n\nmeradb:main> \nPhir milenge!\n");
}

TEST_CASE("shell loop: a line starting with a dot is a command only when no statement is pending", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({"  .tables  ", "DIKHAO", ".tables", ";"}, out);
    repl::run(backend, source, out, kPlain, "1.0.0", 0);
    CHECK(backend.scripts == std::vector<std::string>{"DIKHAO TABLES;", "DIKHAO\n.tables\n;\n"});
}

TEST_CASE("shell loop: a blank line starts a statement, so a later dot-command is statement text", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({"", ".tables", ";"}, out);
    repl::run(backend, source, out, kPlain, "1.0.0", 0);
    CHECK(backend.scripts == std::vector<std::string>{"\n.tables\n;\n"});
    CHECK(out.str() == bannerFor("fake:1") + "meradb:main>       ...>       ...> ok\n\nmeradb:main> \nPhir milenge!\n");
}

TEST_CASE("shell loop: Unicode whitespace after the semicolon still ends the statement", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({"A;\xC2\xA0", "B;\xE3\x80\x80"}, out);
    repl::run(backend, source, out, kPlain, "1.0.0", 0);
    CHECK(backend.scripts.size() == 2);
}

TEST_CASE("shell loop: a lone semicolon is sent to the backend, which decides what it means", "[shell]") {
    FakeBackend backend;
    backend.onRun = [](const std::string&) { return std::vector<Result>{}; };
    std::ostringstream out;
    ScriptedSource source({";"}, out);
    repl::run(backend, source, out, kPlain, "1.0.0", 0);
    CHECK(backend.scripts == std::vector<std::string>{";\n"});
    CHECK(out.str() == bannerFor("fake:1") + "meradb:main> meradb:main> \nPhir milenge!\n");
}

TEST_CASE("shell loop: .exit says goodbye without a leading newline and reads no more", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({".exit", "DIKHAO 1;"}, out);
    CHECK(repl::run(backend, source, out, kPlain, "1.0.0", 0) == 0);
    CHECK(source.reads == 1);
    CHECK(backend.scripts.empty());
    CHECK(out.str() == bannerFor("fake:1") + "meradb:main> Phir milenge!\n");
}

TEST_CASE("shell loop: end of input inside a statement drops the statement", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({"DIKHAO *"}, out);
    CHECK(repl::run(backend, source, out, kPlain, "1.0.0", 0) == 0);
    CHECK(backend.scripts.empty());
    CHECK(out.str() == bannerFor("fake:1") + "meradb:main>       ...> \nPhir milenge!\n");
}

TEST_CASE("shell loop: Ctrl+C at a prompt says goodbye like end of input", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({"DIKHAO"}, out);
    source.interruptAtEnd = true;
    CHECK(repl::run(backend, source, out, kPlain, "1.0.0", 0) == 0);
    CHECK(out.str() == bannerFor("fake:1") + "meradb:main>       ...> \nPhir milenge!\n");
}

TEST_CASE("shell loop: Ctrl+C while a statement runs ends the shell with 130, silently", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({"SLOW;", "DIKHAO 2;"}, out);
    backend.onRun = [&](const std::string&) {
        source.pending = true;  // Ctrl+C pressed during the statement
        return oneMessage("done");
    };
    CHECK(repl::run(backend, source, out, kPlain, "1.0.0", 0) == 130);
    CHECK(backend.scripts == std::vector<std::string>{"SLOW;\n"});  // the statement finished; the next line was never read
    CHECK(out.str() == bannerFor("fake:1") + "meradb:main> done\n\n");
}

TEST_CASE("shell loop: Ctrl+C during a dot-command also ends it with 130", "[shell]") {
    FakeBackend backend;
    std::ostringstream out;
    ScriptedSource source({".tables", "DIKHAO 2;"}, out);
    backend.onRun = [&](const std::string&) {
        source.pending = true;
        return oneMessage("done");
    };
    CHECK(repl::run(backend, source, out, kPlain, "1.0.0", 0) == 130);
}

TEST_CASE("shell loop: a dropped connection is reported and the shell keeps going", "[shell]") {
    FakeBackend backend;
    backend.onRun = [](const std::string&) -> std::vector<Result> { throw ConnectionFailed("Server ne connection band kar diya"); };
    std::ostringstream out;
    ScriptedSource source({"A;", "B;"}, out);
    CHECK(repl::run(backend, source, out, kPlain, "1.0.0", 0) == 0);
    CHECK(out.str() == bannerFor("fake:1") +
                           "meradb:main> [Connection Galti] Server ne connection band kar diya\n"
                           "meradb:main> [Connection Galti] Server ne connection band kar diya\n"
                           "meradb:main> \nPhir milenge!\n");
}

TEST_CASE("shell loop: the colour style reaches the banner, the prompts and the results", "[shell]") {
    FakeBackend backend;
    backend.where = "local (/data/x)";
    std::ostringstream out;
    ScriptedSource source({"X;"}, out);
    repl::run(backend, source, out, term::Style::colored(), "1.0.0", 0);
    CHECK(out.str() == golden_shell::get("banner", true) + "\n" + golden_shell::get("prompt_main", true) +
                           "\x1b[32mok\x1b[0m\n\n" + golden_shell::get("prompt_main", true) + "\nPhir milenge!\n");
}

// The real engine against what the Python shell printed for the same input.
TEST_CASE("shell loop: every recorded Python transcript is reproduced exactly", "[shell][transcript]") {
    int count = 0;
    const golden_transcripts::Transcript* all = golden_transcripts::all(count);
    REQUIRE(count > 20);
    for (int i = 0; i < count; ++i) {
        const auto& transcript = all[i];
        DYNAMIC_SECTION(transcript.name) {
            TempDir dir;
            LocalBackend backend(dir.file("data"));
            std::istringstream in(transcript.input);
            std::ostringstream out;
            repl::StreamLineSource source(in, out);
            CHECK(repl::run(backend, source, out, kPlain, "1.0.0", 0) == 0);
            const std::string marker = "se khatam hote hain\n\n";
            const std::string text = out.str();
            const auto at = text.find(marker);
            REQUIRE(at != std::string::npos);
            CHECK(text.substr(at + marker.size()) == transcript.output);
        }
    }
}
```

Run `cmake --build cpp/build`. Expected: FAIL — `run` is not a member of `meradb::repl`.

- [ ] **Step 3: Extend `repl.h`** (append before the closing namespace)

```cpp
// Python's repl(backend): the banner, then read-eval-print until .exit, end of input or Ctrl+C. Returns the
// process exit code: 0 normally, 130 if Ctrl+C arrived while a statement was running (Design decision D5).
// revealPauseMs is the banner animation's pause per line (0 = none).
int run(Backend& backend, LineSource& in, std::ostream& out, const term::Style& style, const std::string& version,
        int revealPauseMs);
```

- [ ] **Step 4: Extend `repl.cpp`** (append before the closing namespace)

```cpp
int run(Backend& backend, LineSource& in, std::ostream& out, const term::Style& style, const std::string& version,
        int revealPauseMs) {
    printBanner(out, style, version, backend.description(), revealPauseMs);
    out << "\n";
    std::string buffer;
    for (;;) {
        const std::string prompt =
            buffer.empty() ? promptFor(style, backend.currentDb(), backend.inTransaction()) : std::string(kContinuationPrompt);
        std::string line;
        if (in.read(prompt, line) != ReadStatus::Line) {  // end of input, or Ctrl+C at the prompt
            out << "\nPhir milenge!\n";
            out.flush();
            return 0;
        }

        const std::string stripped = pytext::strip(line);
        if (buffer.empty() && pytext::startsWith(stripped, ".")) {
            if (!handleDotCommand(backend, stripped, out, style)) {
                out << "Phir milenge!\n";
                out.flush();
                return 0;
            }
            if (in.takePendingInterrupt()) return 130;
            continue;
        }

        buffer += line + "\n";
        if (endsStatement(buffer)) {
            runText(backend, buffer, out, style);
            buffer.clear();
            if (in.takePendingInterrupt()) return 130;
        }
    }
}
```

`backend.description()`, `currentDb()` and `inTransaction()` are evaluated in source order into named
locals/arguments of different calls (no two in one argument list), as the Global Constraints require.

- [ ] **Step 5: Build and run**

```bash
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "shell loop"
```
Expected: 15 test cases pass, including the 31 replayed transcripts (every recorded one).
Then the whole suite: `ctest --test-dir cpp/build --output-on-failure`.

- [ ] **Step 6: Commit**

```bash
git add cpp/include/meradb/repl.h cpp/src/repl.cpp cpp/tests/test_repl.cpp cpp/tests/shell_scripts.py cpp/tests/gen_shell_transcripts.py cpp/tests/golden_transcripts.h
git commit -m "Add the read-eval-print loop and replay Python's shell transcripts"
```

**Completion checklist:**
- [ ] all 31 Python transcripts reproduce byte for byte through `repl::run` on a real `LocalBackend`
- [ ] the blank-line quirk, `.exit` vs end-of-input newline, and the `130` rule each have a test
- [ ] a dropped connection never ends the shell
- [ ] no transcript is skipped: `unicode_space` needs the tokenizer to treat non-ASCII whitespace as Python does, `ctrl_z_in_pipe` needs the `repr()`-style escape (`'\x1a'`) in the tokenizer error
- [ ] suite green, zero warnings

---
### Task 10: CLI wiring: `meradb_cli shell`

**Files:**
- Modify: `cpp/include/meradb/cli.h`, `cpp/src/cli.cpp`, `cpp/include/meradb/protocol.h`
- Test: `cpp/tests/test_cli.cpp` (replace the "not here yet" test, add shell tests)

**Interfaces:**
- Consumes: everything from Tasks 1-9, `openBackend` (Phase 2), `sys::isTerminal`, `sys::AnsiConsole`,
  `sys::InterruptGuard`, `sys::setStdinBinary` (Task 2).
- Produces: `meradb_cli shell` (and a bare `meradb_cli`, which the parser already turns into `shell`) running
  Python's `cmd_shell`: open the backend, run the shell, close the backend, exit with the shell's code.
  `meradb_cli run` now goes through `repl::runFile` with colour decided by the same rule as the shell
  (Python's `run_file` uses the same `print_result`), so a dropped connection in `run` prints the error on
  stdout and the next file still runs (D7). Phase 2's `runFile` and `normalizeNewlines` in `cli.cpp` are
  **deleted**, not wrapped: nothing else called them.
- Produces `protocol::kProgramVersion = "1.0.0"` (Python's `__version__`, the banner's version) and a test
  seam `ShellInputOverride` in `cli.h`: while one lives, the shell reads its lines from the given stream and
  prints without colour, so a test can drive `cliMain` even when the test binary itself runs in a terminal.
- Three input paths, chosen once: a **terminal** on stdin (`isTerminal(0)`) gets `ConsoleLineSource` inside an
  `InterruptGuard`, colour per `detectStyle`, and the 40 ms reveal animation when colour is on; **anything
  else** (a pipe, a file) gets `setStdinBinary()` and a `StreamLineSource` on `std::cin`, colour per
  `detectStyle` (so a piped run is plain unless stdout is still a terminal, as in Python), no animation unless
  colour is on; the **override** is test-only.

- [ ] **Step 1: Write the failing tests**

In `cpp/tests/test_cli.cpp`, replace the whole test case
`TEST_CASE("cli shell and workbench say they are not here yet", "[cli]") { ... }` with:

```cpp
TEST_CASE("cli the workbench says it is not here yet", "[cli]") {
    CleanEnv env;
    Capture capture;
    CHECK(cliMain({"workbench"}) == 1);
    CHECK(capture.err().find("abhi C++ version mein nahi hai") != std::string::npos);
}

TEST_CASE("cli the shell banner version is the one the server reports", "[cli][shell]") {
    CHECK(std::string(protocol::kServerName) == std::string("MeraDB ") + protocol::kProgramVersion);
}

TEST_CASE("cli shell --local runs a piped session: banner, statements, goodbye", "[cli][shell]") {
    CleanEnv env;
    TempDir dir;
    std::istringstream input("BANAO TABLE t (x INT);\nDAALO MEIN t MAAN (1);\nDIKHAO * SE t;\n.exit\n");
    ShellInputOverride feed(input);
    Capture capture;
    CHECK(cliMain({"shell", "--local", "--data", dir.file("d")}) == 0);
    const std::string out = capture.out();
    CHECK(out.find("connected: local (") != std::string::npos);
    CHECK(out.find("meradb:main> Table 't' ban gaya (1 columns)\n\n") != std::string::npos);
    CHECK(out.find("| 1 |") != std::string::npos);
    const std::string goodbye = "meradb:main> Phir milenge!\n";  // .exit: no newline before it
    REQUIRE(out.size() > goodbye.size());
    CHECK(out.substr(out.size() - goodbye.size()) == goodbye);
    CHECK(capture.err().empty());
    CHECK(std::filesystem::exists(dir.file("d")));  // it really opened the folder it was told to
}

TEST_CASE("cli no command at all is the shell, and end of input says goodbye", "[cli][shell]") {
    CleanEnv env;
    TempDir dir;
    std::istringstream input("");
    ShellInputOverride feed(input);
    Capture capture;
    CHECK(cliMain({"--local", "--data", dir.file("d")}) == 0);
    const std::string out = capture.out();
    const std::string tail = "meradb:main> \nPhir milenge!\n";
    REQUIRE(out.size() > tail.size());
    CHECK(out.substr(out.size() - tail.size()) == tail);
}

TEST_CASE("cli shell goes through a server when one answers", "[cli][shell]") {
    CleanEnv env;
    RunningServer s;
    std::istringstream input("BANAO TABLE via_shell (x INT);\nDIKHAO TABLES;\n");
    ShellInputOverride feed(input);
    Capture capture;
    CHECK(cliMain({"shell", "--port", std::to_string(s.port())}) == 0);
    const std::string out = capture.out();
    CHECK(out.find("connected: 127.0.0.1:" + std::to_string(s.port()) + "\n") != std::string::npos);
    CHECK(out.find("via_shell") != std::string::npos);
    CHECK(std::filesystem::exists(std::filesystem::path(s.dataDir()) / "main"));
}

TEST_CASE("cli shell reports a connection failure before any banner", "[cli][shell]") {
    CleanEnv env;
    RunningServer other;
    const int freePort = other.port();
    other.stop();
    std::istringstream input(".exit\n");
    ShellInputOverride feed(input);
    Capture capture;
    CHECK(cliMain({"shell", "--port", std::to_string(freePort)}) == 1);
    CHECK(capture.out().empty());
    CHECK(capture.err().find("par MeraDB server nahi mila") != std::string::npos);
}
```

Run `cmake --build cpp/build`. Expected: FAIL — `ShellInputOverride` and `protocol::kProgramVersion` are
undeclared.

- [ ] **Step 2: `protocol.h`**

Replace the `kServerName` line with:

```cpp
constexpr const char* kProgramVersion = "1.0.0";  // Python's __version__ (shown in the shell banner)
constexpr const char* kServerName = "MeraDB 1.0.0";  // "MeraDB " + kProgramVersion
```

- [ ] **Step 3: `cli.h`**

Change the header comment's last line to
`// means `shell` (the interactive shell, see repl.h). The workbench arrives in a later phase; here it says so.`,
add `#include <istream>` before `#include <memory>`, and replace the declaration of `runFile` (and its
comment) with:

```cpp
// A test seam: while one of these is alive, `meradb shell` reads its lines from `in` instead of the
// console or stdin, and prints without colour, so a test can drive the shell even from a terminal.
class ShellInputOverride {
public:
    explicit ShellInputOverride(std::istream& in);
    ~ShellInputOverride();
    ShellInputOverride(const ShellInputOverride&) = delete;
    ShellInputOverride& operator=(const ShellInputOverride&) = delete;
};
```

- [ ] **Step 4: `cli.cpp`**

1. Includes: remove `#include "meradb/cli_format.h"`; add `#include "meradb/repl.h"` after
   `#include "meradb/protocol.h"` and `#include "meradb/term_style.h"` after `#include "meradb/sys_compat.h"`.
2. Delete the function `normalizeNewlines` (the block from the comment `// Python's text mode (universal
   newlines)` down to its closing brace).
3. Delete the whole Phase 2 function `bool runFile(Backend& backend, const std::string& path) { ... }`
   (it sits just before the anonymous namespace that starts with `clientPassword`).
4. In the anonymous namespace that starts with `ControlOptions controlOptions(`, insert before it:

```cpp
// Python's _supports_color(): decided once, for the command that is about to print results.
term::Style detectStyle(sys::AnsiConsole& ansi) {
    return term::detectStyle([&ansi] { return ansi.enable(); });
}

// Test seam (see ShellInputOverride in cli.h): when set, the shell reads this stream, never the console.
std::istream* g_shellInput = nullptr;

// Python's cmd_shell: open the backend, run the shell on it, close the backend.
int runShellCommand(const CliArgs& args) {
    auto backend = openBackend(args);
    int code = 0;
    if (g_shellInput != nullptr) {
        repl::StreamLineSource source(*g_shellInput, std::cout);
        code = repl::run(*backend, source, std::cout, term::Style::none(), protocol::kProgramVersion, 0);
    } else if (sys::isTerminal(0)) {
        sys::AnsiConsole ansi;
        const term::Style style = detectStyle(ansi);
        sys::InterruptGuard interrupts;  // Ctrl+C is reported to the shell instead of killing the process
        repl::ConsoleLineSource source(std::cout);
        code = repl::run(*backend, source, std::cout, style, protocol::kProgramVersion, style.on() ? 40 : 0);
    } else {
        sys::setStdinBinary();  // bytes exactly as sent: no CRLF translation, Ctrl+Z is not end-of-file
        sys::AnsiConsole ansi;
        const term::Style style = detectStyle(ansi);
        repl::StreamLineSource source(std::cin, std::cout);
        code = repl::run(*backend, source, std::cout, style, protocol::kProgramVersion, style.on() ? 40 : 0);
    }
    backend->close();
    return code;
}
```

5. Immediately before `int cliMain(std::vector<std::string> argv) {` add:

```cpp
ShellInputOverride::ShellInputOverride(std::istream& in) { g_shellInput = &in; }
ShellInputOverride::~ShellInputOverride() { g_shellInput = nullptr; }
```

6. In `cliMain` replace the `run` branch and the `// shell / workbench` lines with:

```cpp
        if (args.command == "run") {
            auto backend = openBackend(args);
            sys::AnsiConsole ansi;
            const term::Style style = detectStyle(ansi);
            bool allOk = true;
            for (const auto& path : args.files)
                if (!repl::runFile(*backend, path, std::cout, style)) allOk = false;  // run ALL files, even after a failure
            backend->close();
            return allOk ? 0 : 1;
        }
        if (args.command == "shell") return runShellCommand(args);
        // workbench
        note("`meradb " + args.command + "` abhi C++ version mein nahi hai (aage ke phase mein aayega). "
             "Python version istemal karo, ya scripts ke liye:  meradb run FILE");
        return 1;
```

(`g_shellInput` is declared in the anonymous namespace *before* the two `ShellInputOverride` members, which
therefore see it; keep that order.)

- [ ] **Step 5: Build and run**

```bash
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "cli"
```
Expected: all `cli` test cases pass (the 3 new `[shell]` cases and the replaced workbench case included).
Smoke it by hand, then run everything:

```bash
printf 'BANAO TABLE t (x INT);\nDIKHAO * SE t;\n.help join\n' | cpp/build/meradb_cli shell --local --data /tmp/meradb_smoke
ctest --test-dir cpp/build --output-on-failure
```
Expected: the logo, wordmark, tagline and `  connected: local (...)`, the hint line, a blank line, then
`meradb:main> Table 't' ban gaya (1 columns)`, the empty table and `0 row(s)`, the `Joins` help block, and
`meradb:main>       ...> ` followed by a newline and `Phir milenge!` (the `.help join` output is followed
directly by the prompt: Python prints no blank line there either). The whole suite, including the
`cross_engine_*` and `cli_lifecycle` Python checks that exercise `run`, stays green. Delete `/tmp/meradb_smoke`
(or use a `mktemp -d`).

- [ ] **Step 6: Commit**

```bash
git add cpp/include/meradb/cli.h cpp/include/meradb/protocol.h cpp/src/cli.cpp cpp/tests/test_cli.cpp
git commit -m "Wire the interactive shell into meradb_cli and share its code path with run"
```

**Completion checklist:**
- [ ] `meradb_cli` with no arguments starts the shell; `shell -h` text is unchanged (Phase 2 golden test passes)
- [ ] a connection failure prints the error on stderr and exits 1 *before* any banner
- [ ] piped input is read in binary mode; a terminal gets the interrupt guard; the guard exists only then
- [ ] `run` colours by the same rule and survives a dropped connection (unit-tested through `repl::runFile`)
- [ ] no `std::cin` / `std::cout` use outside `cli.cpp`; `git diff <base> -- meradb examples` is empty
- [ ] suite green, zero warnings

---

# BATCH C — cross-engine verification: the same scripts through both real shells

### Task 11: Cross-engine shell diff: local mode

**Files:**
- Modify: `cpp/tests/shell_scripts.py`
- Create: `cpp/tests/shell_diff.py`
- Modify: `cpp/tests/CMakeLists.txt` (register `shell_diff_local`)

**Interfaces:**
- Consumes: the 31 scripts of Task 9, `meradb_cli shell` (Task 10), Python's `python -m meradb shell`.
- Produces `shell_diff.py --cli <meradb_cli> [script names...]`: for each script, run the Python shell and the
  C++ shell with `--local --data <same fresh folder>` and the script on stdin, and compare **stdout,
  stderr and the exit code** exactly (CRLF normalised, as `cross_engine_diff.py` does). The same folder
  path is used for both runs (wiped in between) so the banner's `connected: local (<path>)` line is
  identical. Colour is off (`NO_COLOR=1`, piped); colour is covered by the golden unit tests.
- Python runs with `PYTHONIOENCODING=utf-8` so it prints `·` and `█` like the C++ shell always does (D4).
- No script is skipped. (An earlier draft skipped `unicode_space` and `ctrl_z_in_pipe` as a "`repr()`
  divergence"; the real differences were that the C++ tokenizer rejected non-ASCII whitespace Python accepts and
  printed control characters raw in its error text. Both are fixed in the tokenizer, so both scripts are compared.)

- [ ] **Step 1: Append to `cpp/tests/shell_scripts.py`**

Nothing to append: every script in `SCRIPTS` is compared.

- [ ] **Step 2: Write `cpp/tests/shell_diff.py`**

```python
"""
Drives the Python shell (`python -m meradb shell`) and the C++ shell (`meradb_cli shell`) with the same
piped script and diffs everything they print -- stdout, stderr and the exit code.

Every script in shell_scripts.py runs with `--local` on a fresh data folder (server mode is added in the
next task).

Usage:
    python cpp/tests/shell_diff.py --cli path/to/meradb_cli                 # every script
    python cpp/tests/shell_diff.py --cli path/to/meradb_cli basic errors    # just these
Exit code 0 = everything matched.
"""
import argparse
import difflib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.stdout.reconfigure(encoding="utf-8")  # diffs may contain non-ASCII text; never die on a console code page

from shell_scripts import SCRIPTS  # noqa: E402


def child_env() -> dict:
    env = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_") and k != "NO_COLOR"}
    env.update(PYTHONIOENCODING="utf-8", PYTHONPATH=str(REPO_ROOT), NO_COLOR="1")
    return env


def run_shell(kind: str, cli: str, args: list[str], script: str) -> tuple[str, str, int]:
    """One shell session: `script` on stdin, CRLF normalised in what comes back."""
    base = [sys.executable, "-m", "meradb"] if kind == "python" else [cli]
    done = subprocess.run(base + ["shell"] + args, input=script.encode("utf-8"), cwd=REPO_ROOT, capture_output=True,
                          env=child_env(), timeout=120)
    return (done.stdout.decode("utf-8").replace("\r\n", "\n"), done.stderr.decode("utf-8").replace("\r\n", "\n"),
            done.returncode)


def show_diff(left: str, right: str, left_name: str, right_name: str) -> None:
    diff = difflib.unified_diff(left.splitlines(keepends=True), right.splitlines(keepends=True), left_name, right_name)
    print("".join(list(diff)[:80]) or f"(only whitespace differs) {left[-30:]!r} vs {right[-30:]!r}")


def selected(names: list[str], pool: dict) -> list[str]:
    """The scripts to run: all of `pool`, or the named ones."""
    for name in names:
        if name not in pool:
            raise SystemExit(f"unknown script {name!r}; known: {', '.join(pool)}")
    return names or list(pool)


# ---------------------------------------------------------------- local mode

def check_local(cli: str, names: list[str]) -> list[str]:
    failures = []
    for name in selected(names, SCRIPTS):
        label = f"local: {name}"
        script = SCRIPTS[name]
        with tempfile.TemporaryDirectory() as root:
            data = os.path.join(root, "data")  # the same path for both, so the banner's "connected:" line matches
            python = run_shell("python", cli, ["--local", "--data", data], script)
            shutil.rmtree(data, ignore_errors=True)
            cpp = run_shell("cpp", cli, ["--local", "--data", data], script)
        if python == cpp:
            print(f"PASS {label} ({len(python[0].splitlines())} lines, exit={python[2]})")
        else:
            print(f"FAIL {label} (exit python={python[2]}, cpp={cpp[2]})")
            failures.append(label)
            show_diff(python[0], cpp[0], "python stdout", "cpp stdout")
            if python[1] != cpp[1]:
                show_diff(python[1], cpp[1], "python stderr", "cpp stderr")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("names", nargs="*", help="script names from shell_scripts.py (default: all)")
    parser.add_argument("--cli", required=True)
    args = parser.parse_args()
    cli = str(Path(args.cli).resolve())  # children run with cwd=REPO_ROOT, so a relative path would break
    failures = check_local(cli, args.names)
    print("ALL MATCHED" if not failures else "FAILED: " + "; ".join(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 3: Run it**

```bash
python cpp/tests/shell_diff.py --cli cpp/build/meradb_cli
```
Expected: 31 lines `PASS local: <name> (<n> lines, exit=0)` and `ALL MATCHED`; exit code 0. Any `FAIL` prints a unified diff of the two transcripts:
fix the C++ shell (Python is the oracle), never the script.

- [ ] **Step 4: Prove the diff can fail** (a check that does not fail on a bug is worthless)

Change one word of the hint line in `cpp/src/repl_text.cpp` (`bahar niklo` to `bahar nikalo`), rebuild, and
run `python cpp/tests/shell_diff.py --cli cpp/build/meradb_cli basic`. Expected: `FAIL local: basic` with a
diff showing exactly that line, exit code 1. Revert with `git checkout -- cpp/src/repl_text.cpp`, rebuild,
and run it again: `ALL MATCHED`.

- [ ] **Step 5: Register it in `cpp/tests/CMakeLists.txt`**

Inside the `if(Python3_Interpreter_FOUND)` block, after the line
`set_tests_properties(interchange fuzz_triggers PROPERTIES TIMEOUT 900)`, add:

```cmake
  add_test(NAME shell_diff_local
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shell_diff.py --cli $<TARGET_FILE:meradb_cli>)
  set_tests_properties(shell_diff_local PROPERTIES TIMEOUT 600)
```

Re-run configure, then `ctest --test-dir cpp/build --output-on-failure -R shell_diff_local`. Expected: passes.

- [ ] **Step 6: Commit**

```bash
git add cpp/tests/shell_diff.py cpp/tests/shell_scripts.py cpp/tests/CMakeLists.txt
git commit -m "Diff the C++ shell against the Python shell on piped scripts"
```

**Completion checklist:**
- [ ] all 31 scripts match on stdout, stderr and exit code (none is skipped)
- [ ] the mutation check in Step 4 was seen failing, then reverted
- [ ] no temp folder, process or pid file is left behind; no `MERADB_*` variable leaks into a child
- [ ] suite green

---

### Task 12: Cross-engine shell diff: C++ and Python servers

**Files:**
- Modify: `cpp/tests/shell_scripts.py` (append `SERVER_SCRIPTS`)
- Replace: `cpp/tests/shell_diff.py` (adds server mode)
- Modify: `cpp/tests/CMakeLists.txt` (register `shell_diff_server`)

**Interfaces:**
- Consumes: `Server` from `interop_check.py` (a foreground server process of either kind on a free port,
  stopped through the protocol's `shutdown` message, leaving no process behind).
- Produces `--mode server`: 13 of the scripts, each run through all four client/server pairs — Python client
  or C++ client, against a Python server or a C++ server, each on a fresh data folder and a free port. The
  Python-client-to-Python-server transcript is the baseline; the other three must equal it byte for byte
  (the port number in the banner's `connected: 127.0.0.1:<port>` line is replaced by `PORT`; nothing else is
  normalised). This covers the banner through the wire, the transaction marker in the prompt
  (`meradb:main*>`), `ISTEMAL` changing the prompt's database, errors that cross the protocol, and a C++
  shell talking to a Python server (and the reverse).
- Run with `--mode local`, `--mode server` or (default) `--mode all`; script names narrow the run (names that
  are not server scripts are ignored in `all` mode and rejected in `server` mode).

- [ ] **Step 1: Append to `cpp/tests/shell_scripts.py`**

```python
# The scripts also run through a server (every client/server pair): the ones that do not depend on local-only
# behaviour. Each run starts two servers and two clients, so the list is kept to what adds coverage.
SERVER_SCRIPTS = [
    "basic", "unicode", "long_multi_line", "samjhao", "use_database", "transaction", "trigger_one_line", "errors",
    "dot_commands", "help_full", "blank_line_quirk", "eof_in_statement", "crlf",
]
```

- [ ] **Step 2: Replace `cpp/tests/shell_diff.py`** with the complete version (the helpers, `check_local` and
  `selected` are unchanged from Task 11; the docstring, imports, `check_servers` and `main` are new):

```python
"""
Drives the Python shell (`python -m meradb shell`) and the C++ shell (`meradb_cli shell`) with the same
piped script and diffs everything they print -- stdout, stderr and the exit code.

Modes:
  local     both shells open a fresh data folder (--local); every script in shell_scripts.py
  server    the same scripts through every client/server pair: a Python or a C++ client against a Python
            or a C++ server (the output of the three other pairs must equal the Python-to-Python baseline)

Usage:
    python cpp/tests/shell_diff.py --cli path/to/meradb_cli                 # both modes, every script
    python cpp/tests/shell_diff.py --cli path/to/meradb_cli --mode local basic errors
Exit code 0 = everything matched.
"""
import argparse
import difflib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.stdout.reconfigure(encoding="utf-8")  # diffs may contain non-ASCII text; never die on a console code page

from interop_check import Server  # noqa: E402  (a foreground server of either kind on a free port)
from shell_scripts import SCRIPTS, SERVER_SCRIPTS  # noqa: E402


def child_env() -> dict:
    env = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_") and k != "NO_COLOR"}
    env.update(PYTHONIOENCODING="utf-8", PYTHONPATH=str(REPO_ROOT), NO_COLOR="1")
    return env


def run_shell(kind: str, cli: str, args: list[str], script: str) -> tuple[str, str, int]:
    """One shell session: `script` on stdin, CRLF normalised in what comes back."""
    base = [sys.executable, "-m", "meradb"] if kind == "python" else [cli]
    done = subprocess.run(base + ["shell"] + args, input=script.encode("utf-8"), cwd=REPO_ROOT, capture_output=True,
                          env=child_env(), timeout=120)
    return (done.stdout.decode("utf-8").replace("\r\n", "\n"), done.stderr.decode("utf-8").replace("\r\n", "\n"),
            done.returncode)


def show_diff(left: str, right: str, left_name: str, right_name: str) -> None:
    diff = difflib.unified_diff(left.splitlines(keepends=True), right.splitlines(keepends=True), left_name, right_name)
    print("".join(list(diff)[:80]) or f"(only whitespace differs) {left[-30:]!r} vs {right[-30:]!r}")


def selected(names: list[str], pool: dict) -> list[str]:
    """The scripts to run: all of `pool`, or the named ones."""
    for name in names:
        if name not in pool:
            raise SystemExit(f"unknown script {name!r}; known: {', '.join(pool)}")
    return names or list(pool)


# ---------------------------------------------------------------- local mode

def check_local(cli: str, names: list[str]) -> list[str]:
    failures = []
    for name in selected(names, SCRIPTS):
        label = f"local: {name}"
        script = SCRIPTS[name]
        with tempfile.TemporaryDirectory() as root:
            data = os.path.join(root, "data")  # the same path for both, so the banner's "connected:" line matches
            python = run_shell("python", cli, ["--local", "--data", data], script)
            shutil.rmtree(data, ignore_errors=True)
            cpp = run_shell("cpp", cli, ["--local", "--data", data], script)
        if python == cpp:
            print(f"PASS {label} ({len(python[0].splitlines())} lines, exit={python[2]})")
        else:
            print(f"FAIL {label} (exit python={python[2]}, cpp={cpp[2]})")
            failures.append(label)
            show_diff(python[0], cpp[0], "python stdout", "cpp stdout")
            if python[1] != cpp[1]:
                show_diff(python[1], cpp[1], "python stderr", "cpp stderr")
    return failures


# ---------------------------------------------------------------- server mode

def check_servers(cli: str, names: list[str], strict: bool) -> list[str]:
    failures = []
    pool = {n: SCRIPTS[n] for n in SERVER_SCRIPTS}
    if names and not strict:  # both modes were asked for: take only the named scripts that also run through servers
        names = [n for n in names if n in pool]
        if not names:
            return failures
    for name in selected(names, pool):
        script = SCRIPTS[name]
        results = {}
        for server_kind in ("python", "cpp"):
            for client_kind in ("python", "cpp"):
                with tempfile.TemporaryDirectory() as data, Server(server_kind, cli, data) as server:
                    out, err, code = run_shell(client_kind, cli, ["--port", str(server.port)], script)
                    # the port differs per server; nothing else about the banner may
                    results[(client_kind, server_kind)] = (out.replace(f"127.0.0.1:{server.port}", "127.0.0.1:PORT"),
                                                           err.replace(str(server.port), "PORT"), code)
        baseline = results[("python", "python")]
        for pair, got in results.items():
            if pair == ("python", "python"):
                continue
            label = f"server: {name}: {pair[0]} client -> {pair[1]} server"
            if got == baseline:
                print(f"PASS {label} ({len(got[0].splitlines())} lines)")
            else:
                print(f"FAIL {label}")
                failures.append(label)
                show_diff(baseline[0], got[0], "python->python", f"{pair[0]}->{pair[1]}")
                if baseline[1] != got[1]:
                    show_diff(baseline[1], got[1], "python->python stderr", "stderr")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("names", nargs="*", help="script names from shell_scripts.py (default: all)")
    parser.add_argument("--cli", required=True)
    parser.add_argument("--mode", choices=["local", "server", "all"], default="all")
    args = parser.parse_args()
    cli = str(Path(args.cli).resolve())  # children run with cwd=REPO_ROOT, so a relative path would break
    failures = []
    if args.mode in ("local", "all"):
        failures += check_local(cli, args.names)
    if args.mode in ("server", "all"):
        failures += check_servers(cli, args.names, strict=args.mode == "server")
    print("ALL MATCHED" if not failures else "FAILED: " + "; ".join(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 3: Run it**

```bash
python cpp/tests/shell_diff.py --cli cpp/build/meradb_cli --mode server
```
Expected: 39 lines `PASS server: <script>: <client> client -> <server> server (<n> lines)` (13 scripts x
3 pairs), then `ALL MATCHED`; about a minute. A `FAIL` prints the diff against the Python-to-Python baseline:
if only the C++ *client* column fails the bug is in the shell or `Connection`; if only a C++ *server* column
fails it is in the server (Phase 2).

- [ ] **Step 4: Register it in `cpp/tests/CMakeLists.txt`**

Change the Task 11 registration to run local only, and add the server one:

```cmake
  add_test(NAME shell_diff_local
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shell_diff.py --cli $<TARGET_FILE:meradb_cli> --mode local)
  add_test(NAME shell_diff_server
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shell_diff.py --cli $<TARGET_FILE:meradb_cli> --mode server)
  set_tests_properties(shell_diff_local PROPERTIES TIMEOUT 600)
  set_tests_properties(shell_diff_server PROPERTIES TIMEOUT 900)
```

Re-run configure, then `ctest --test-dir cpp/build --output-on-failure -R shell_diff`. Expected: both pass.
Afterwards confirm nothing was left running (`tasklist | findstr meradb` on Windows,
`pgrep -f meradb` elsewhere: no output) and that the real per-user data folder has no new files.

- [ ] **Step 5: Commit**

```bash
git add cpp/tests/shell_diff.py cpp/tests/shell_scripts.py cpp/tests/CMakeLists.txt
git commit -m "Diff the shell through every client and server pairing"
```

**Completion checklist:**
- [ ] 39 server-mode comparisons pass; the baseline is Python client to Python server
- [ ] the C++ shell works against a Python server and the Python shell against a C++ server
- [ ] the transaction marker and the changing database name appear identically through the wire
- [ ] every server started is stopped; ports come from the OS (no fixed port); no stray processes
- [ ] suite green

---

### Task 13: Shell session behaviours: logins, drops, fallback, hardening

**Files:**
- Create: `cpp/tests/shell_session.py`
- Modify: `cpp/tests/CMakeLists.txt` (register `shell_session`)

**Interfaces:**
- Consumes: `Server` and `free_port` (`interop_check.py`), `child_env`, `run_shell`, `show_diff`
  (`shell_diff.py`), Python's `UserStore` / `Engine` to prepare data.
- Produces four groups of checks that a single piped script cannot express:
  1. **Logins** — a user made with Python's `UserStore` (so a C++ server also reads a Python-written
     `users.json`), `-U asha` with `MERADB_PASSWORD`, against a Python and a C++ server: the transcript from the
     C++ client equals the Python client's (grants enforced: `DIKHAO` on the granted table works, the other
     table and `BANAO` are refused); a wrong password and an unknown user print the same stderr and exit 1
     with **no banner** on stdout.
  2. **A dropped connection** — a session whose stdin stays open: connect, run a statement, stop the server
     while the shell is idle, run another statement. The shell prints `[Connection Galti] Server se
     connection toot gaya: ...` **on stdout**, prints it again for the next statement, and still ends cleanly
     with `Phir milenge!` and exit 0 (Python and C++ clients, each against both servers). The operating
     system's own wording after `toot gaya:` (`[WinError 10054] ...` from Python, a bare message from C++)
     is masked; everything before it must match. Recorded as a divergence in `docs/CPP.md`.
  3. **Fallback** — nothing listens on `MERADB_PORT`: both shells print the same note on stderr
     (`... par server nahi mila -- LOCAL mode ...`), then run a local shell.
  4. **Hardening** (C++ only; Python's behaviour on these is a traceback or a recursion error): a 300 000
     character line, 60 000 nested parentheses, a 30 000-term operator chain, 3 000 statements, invalid UTF-8,
     a NUL byte, and a lone `.` after a blank line each leave the shell alive: it answers the next statement
     (`1 table(s) in 'main'`), says goodbye and exits 0. (Invalid UTF-8 and the NUL byte are D6 divergences:
     Python dies, C++ carries on.)

- [ ] **Step 1: Write `cpp/tests/shell_session.py`**

```python
"""
Shell behaviours that a single piped script cannot show, checked against the Python shell where Python has
the behaviour and on their own where it does not:

  1. logins        -U / MERADB_PASSWORD against a Python and a C++ server (users made by the Python
                   UserStore): same transcript from both clients, grants enforced, a wrong password is
                   refused before any banner (stderr + exit code equal to Python's)
  2. dropped       the server goes away in the middle of a session: the shell prints the connection error
                   on stdout, keeps going, and still says goodbye (Python and C++ clients, both servers)
  3. fallback      no server is listening: the same note on stderr, then a local shell
  4. hardening     C++ only: a very long line, deeply nested input, invalid UTF-8 and a NUL byte on stdin
                   never crash the shell or stop it from answering the next statement

Usage:
    python cpp/tests/shell_session.py --cli path/to/meradb_cli
Exit code 0 = everything held.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO_ROOT))
sys.stdout.reconfigure(encoding="utf-8")

from interop_check import Server, free_port  # noqa: E402
from shell_diff import child_env, show_diff  # noqa: E402

PROMPT = "meradb:main> "


def command(kind: str, cli: str, *args: str) -> list[str]:
    return ([sys.executable, "-m", "meradb"] if kind == "python" else [cli]) + list(args)


def normalise(text: str, port: int) -> str:
    return text.replace(f"127.0.0.1:{port}", "127.0.0.1:PORT").replace(str(port), "PORT")


# ---------------------------------------------------------------- an interactive session (stdin kept open)

class Session:
    """A shell whose stdin we keep open, so a test can act (stop a server) between two lines."""

    def __init__(self, cmd: list[str], env: dict):
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     cwd=REPO_ROOT, env=env)
        self.out, self.err = bytearray(), bytearray()
        self.threads = [threading.Thread(target=self._pump, args=(self.proc.stdout, self.out), daemon=True),
                        threading.Thread(target=self._pump, args=(self.proc.stderr, self.err), daemon=True)]
        for thread in self.threads:
            thread.start()

    @staticmethod
    def _pump(stream, sink):
        while True:
            chunk = os.read(stream.fileno(), 4096)
            if not chunk:
                return
            sink.extend(chunk)

    def text(self) -> str:
        return bytes(self.out).decode("utf-8", "replace").replace("\r\n", "\n")

    def wait_for(self, needle: str, count: int = 1, seconds: float = 30.0) -> None:
        deadline = time.time() + seconds
        while time.time() < deadline:
            if self.text().count(needle) >= count:
                return
            if self.proc.poll() is not None and self.text().count(needle) < count:
                break
            time.sleep(0.05)
        raise SystemExit(f"timed out waiting for {needle!r} x{count}; got:\n{self.text()}")

    def send(self, text: str) -> None:
        self.proc.stdin.write(text.encode("utf-8"))
        self.proc.stdin.flush()

    def finish(self) -> tuple[str, str, int]:
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            code = self.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise SystemExit("the shell did not end after its input closed")
        for thread in self.threads:
            thread.join(timeout=5)
        return self.text(), bytes(self.err).decode("utf-8", "replace").replace("\r\n", "\n"), code


# ---------------------------------------------------------------- 1. logins

def check_logins(cli: str) -> list[str]:
    from meradb.engine import Engine
    from meradb.users import UserStore

    failures = []
    for server_kind in ("python", "cpp"):
        results = {}
        for client_kind in ("python", "cpp"):
            with tempfile.TemporaryDirectory() as data:
                engine = Engine(data)
                engine.execute("BANAO TABLE staff (id INT, name TEXT); DAALO MEIN staff MAAN (1, 'a'); "
                               "BANAO TABLE secret (x INT)")
                engine.close()
                users = UserStore(data)
                users.create("asha", "pw-é")
                users.grant("asha", "main", "staff", ["DIKHAO"])
                with Server(server_kind, cli, data) as server:
                    script = "DIKHAO * SE staff;\nDIKHAO * SE secret;\nBANAO TABLE z (x INT);\n"
                    good = run_user(client_kind, cli, server.port, "asha", {"MERADB_PASSWORD": "pw-é"}, script)
                    bad = run_user(client_kind, cli, server.port, "asha", {"MERADB_PASSWORD": "wrong"}, script)
                    ghost = run_user(client_kind, cli, server.port, "ghost", {"MERADB_PASSWORD": "x"}, script)
                    results[client_kind] = (good, bad, ghost)
        for index, what in enumerate(("login as asha", "wrong password", "unknown user")):
            label = f"logins: {what} on a {server_kind} server"
            python, cpp = results["python"][index], results["cpp"][index]
            if python == cpp:
                print(f"PASS {label} (exit={python[2]})")
            else:
                print(f"FAIL {label} (exit python={python[2]}, cpp={cpp[2]})")
                failures.append(label)
                show_diff(python[0], cpp[0], "python stdout", "cpp stdout")
                show_diff(python[1], cpp[1], "python stderr", "cpp stderr")
    return failures


def run_user(kind, cli, port, user, extra_env, script):
    env = child_env()
    env.update(extra_env)
    done = subprocess.run(command(kind, cli, "shell", "--port", str(port), "-U", user), input=script.encode("utf-8"),
                          cwd=REPO_ROOT, capture_output=True, env=env, timeout=120)
    return (normalise(done.stdout.decode("utf-8").replace("\r\n", "\n"), port),
            normalise(done.stderr.decode("utf-8").replace("\r\n", "\n"), port), done.returncode)


# ---------------------------------------------------------------- 2. the server goes away mid-session

def dropped_session(client_kind: str, server_kind: str, cli: str) -> tuple[str, str, int]:
    with tempfile.TemporaryDirectory() as data:
        server = Server(server_kind, cli, data)
        try:
            session = Session(command(client_kind, cli, "shell", "--port", str(server.port)), child_env())
            session.wait_for(PROMPT, 1)
            session.send("BANAO TABLE t (x INT);\n")
            session.wait_for(PROMPT, 2)
            server.stop()  # the server shuts down while the shell is connected and idle
            session.send("DIKHAO TABLES;\n")
            session.wait_for("[Connection Galti]", 1)
            session.wait_for(PROMPT, 3)
            session.send("DIKHAO TABLES;\n.exit\n")  # still alive: the same error again, then goodbye
            out, err, code = session.finish()
        finally:
            server.stop()
    # The operating system's own wording after "toot gaya:" differs by platform and by language runtime
    # ("[WinError 10054] ..." from Python, a bare message from C++): everything before it must match.
    out = re.sub(r"(toot gaya: ).*", r"\1<os error>", out)
    return normalise(out, server.port), normalise(err, server.port), code


def check_dropped(cli: str) -> list[str]:
    failures = []
    for server_kind in ("python", "cpp"):
        results = {client: dropped_session(client, server_kind, cli) for client in ("python", "cpp")}
        label = f"dropped: the {server_kind} server stops mid-session"
        python, cpp = results["python"], results["cpp"]
        errors = cpp[0].count("[Connection Galti]")
        if python == cpp and errors >= 2 and cpp[0].endswith("Phir milenge!\n") and cpp[2] == 0:
            print(f"PASS {label} ({errors} errors printed, exit={cpp[2]})")
        else:
            print(f"FAIL {label} (exit python={python[2]}, cpp={cpp[2]}, errors printed by cpp={errors})")
            failures.append(label)
            show_diff(python[0], cpp[0], "python stdout", "cpp stdout")
            show_diff(python[1], cpp[1], "python stderr", "cpp stderr")
    return failures


# ---------------------------------------------------------------- 3. no server: fall back to local

def check_fallback(cli: str) -> list[str]:
    script = "BANAO TABLE f (x INT);\n.tables\n"
    with tempfile.TemporaryDirectory() as root:
        data = os.path.join(root, "data")
        port = free_port()  # nothing listens here
        results = []
        for kind in ("python", "cpp"):
            env = child_env()
            env["MERADB_PORT"] = str(port)
            done = subprocess.run(command(kind, cli, "shell", "--data", data), input=script.encode("utf-8"), cwd=REPO_ROOT,
                                  capture_output=True, env=env, timeout=120)
            results.append((done.stdout.decode("utf-8").replace("\r\n", "\n"), done.stderr.decode("utf-8").replace("\r\n", "\n"),
                            done.returncode))
            shutil.rmtree(data, ignore_errors=True)
    python, cpp = results
    label = "fallback: no server, local shell with a note on stderr"
    if python == cpp and "LOCAL mode" in cpp[1] and "Phir milenge!" in cpp[0]:
        print(f"PASS {label}")
        return []
    print(f"FAIL {label}")
    show_diff(python[0], cpp[0], "python stdout", "cpp stdout")
    show_diff(python[1], cpp[1], "python stderr", "cpp stderr")
    return [label]


# ---------------------------------------------------------------- 4. hardening (C++ only)

def hardening_cases() -> list[tuple[str, bytes, str]]:
    """(name, stdin, something that must be in stdout). Every case ends with a statement that must still work."""
    head = b"BANAO TABLE alive (x INT);\n"
    tail = b"DIKHAO TABLES;\n"  # answers "1 table(s) in 'main'" only if the shell and the database are still sound
    alive = "1 table(s) in 'main'"
    return [
        ("a very long line", head + b"DIKHAO '" + b"x" * 300_000 + b"' SE alive;\n" + tail, alive),
        ("deeply nested parentheses", head + b"DIKHAO " + b"(" * 60_000 + b"1" + b")" * 60_000 + b" SE alive;\n" + tail, alive),
        ("a long chain of operators", head + b"DIKHAO " + b"1 + " * 30_000 + b"1 SE alive;\n" + tail, alive),
        ("many tiny statements", head + b"DIKHAO 1 SE alive;\n" * 3_000 + tail, alive),
        ("invalid UTF-8", head + b"DIKHAO '\xff\xfe' SE alive;\n" + tail, alive),
        ("a NUL byte", head + b"DIKHAO 'a\x00b' SE alive;\n" + tail, alive),
        # a blank line opens a statement, so the dot is statement text (the shell quirk) and ends in a parser error
        ("a blank line then a lone dot", head + b"   \n.\n" + tail, "par '.' mila"),
    ]


def check_hardening(cli: str) -> list[str]:
    failures = []
    for name, data_in, expect in hardening_cases():
        label = f"hardening: {name}"
        with tempfile.TemporaryDirectory() as root:
            done = subprocess.run([cli, "shell", "--local", "--data", os.path.join(root, "d")], input=data_in, cwd=REPO_ROOT,
                                  capture_output=True, env=child_env(), timeout=180)
        out = done.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
        if done.returncode == 0 and expect in out and out.endswith("Phir milenge!\n"):
            print(f"PASS {label} ({len(out)} bytes of output)")
        else:
            print(f"FAIL {label} (exit={done.returncode}, tail={out[-120:]!r}, stderr={done.stderr[-200:]!r})")
            failures.append(label)
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cli", required=True)
    args = parser.parse_args()
    cli = str(Path(args.cli).resolve())
    failures = check_logins(cli) + check_dropped(cli) + check_fallback(cli) + check_hardening(cli)
    print("ALL HELD" if not failures else "FAILED: " + "; ".join(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 2: Run it**

```bash
python cpp/tests/shell_session.py --cli cpp/build/meradb_cli
```
Expected: 6 `PASS logins: ...` lines (three scenarios on each server kind: exit 0, 1, 1), 2 `PASS dropped: ...`
(`2 errors printed, exit=0`), `PASS fallback: ...`, 7 `PASS hardening: ...`, then `ALL HELD`; about half a
minute. If the 60 000-parenthesis case fails, the stack guard of Phase 1 (`stack_guard`) is not covering the
shell's path: look at what the engine returned, not at the shell. Check nothing is left running.

- [ ] **Step 3: Register it in `cpp/tests/CMakeLists.txt`**

After the `shell_diff_*` registrations:

```cmake
  add_test(NAME shell_session
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/shell_session.py --cli $<TARGET_FILE:meradb_cli>)
  set_tests_properties(shell_session PROPERTIES TIMEOUT 900)
```

Re-run configure and `ctest --test-dir cpp/build --output-on-failure -R shell_session`. Expected: passes.

- [ ] **Step 4: Commit**

```bash
git add cpp/tests/shell_session.py cpp/tests/CMakeLists.txt
git commit -m "Check shell logins, dropped connections, fallback and hostile input"
```

**Completion checklist:**
- [ ] a wrong password or unknown user prints nothing on stdout (no banner) and the same stderr as Python, exit 1
- [ ] a stopped server never ends the shell: errors on stdout, then a clean goodbye with exit 0
- [ ] the fallback note and local session match Python's
- [ ] none of the hostile inputs crashes the shell; the temp data folders and servers are cleaned up
- [ ] the OS-wording difference after `toot gaya:` is noted for `docs/CPP.md`
- [ ] suite green

---

# BATCH D — documentation, manual checks, hand-off

### Task 14: Docs, manual terminal checklist, completion checklist, hand-off to Phases 4-5

**Files:**
- Modify: `docs/CPP.md`, `README.md`
- No code. (The last step is a read-through of the whole change.)

**Interfaces:**
- Consumes: the finished shell (Tasks 1-13).
- Produces: `docs/CPP.md` describing the shell (what it does, how input editing works, colour rules, the
  verification commands, the layout rows, every divergence) and a hand-off section for Phases 4-5; a one-line
  README update; the manual terminal checklist the owner runs once on real consoles.

- [ ] **Step 1: `docs/CPP.md` — title, coverage, "not yet"**

1. Title line: `# MeraDB in C++ (Phases 1-3: engine, server, client, shell)`.
2. After the Phase 2 list (before "Not yet") add:

```markdown
Phase 3 (the interactive shell):
- `meradb_cli shell` (and a bare `meradb_cli`): the banner with the block logo, the prompts
  (`meradb:<db>> `, `meradb:<db>*> ` inside a transaction, `      ...> ` while a statement is
  being typed), statements ended by `;`, the dot-commands (`.help [word]`, `.tables`,
  `.schema <table>`, `.run <file>`, `.exit` / `.quit` / `.nikal`), ANSI colour, and Ctrl+C /
  Ctrl+D / end-of-input handling, in local mode and through a server (C++ or Python). It behaves like
  `python -m meradb shell`: the same transcript for the same input, checked by driving both shells with
  identical piped scripts (below).
- `run` shares the shell's code path: coloured results on a terminal, and a dropped connection
  prints its error on stdout and the next file still runs, as in Python.
```

3. Replace the "Not yet" paragraph with:

```markdown
Not yet (Phases 4-5): the workbench and the polish pass. `workbench` prints a note and exits 1.
```

- [ ] **Step 2: `docs/CPP.md` — Command line**

In the code block add the line
`meradb_cli shell  [-D <dir>] [--local] [-H host] [-p port] [-U user] [-W] [-d database]   # also: no command at all`
and after the paragraph that ends with the data-folder default, add a section:

```markdown
### The shell

`meradb_cli shell` connects exactly like `run` (a server if one answers, otherwise a local data
folder with a note on stderr), prints the banner and reads statements until `.exit`, Ctrl+D (Ctrl+Z then
Enter on a Windows console) or the end of its input. A statement ends at a line whose last non-blank
character is `;`; a line that starts with `.` is a command only while no statement is being typed (so after
a blank line a `.tables` is statement text, as in Python). Piped input works the same way and prints the
same transcript, prompts included.

**Line editing.** Like Python's shell (which calls `input()` and never loads `readline`), the C++ shell
has no history file, completion or custom key handling of its own: what you get is the terminal's
editing. On a Windows console that is the console host's (arrow keys, Home / End, F7 history of the
session); on a POSIX terminal it is the kernel's line editing (Backspace, Ctrl+U, Ctrl+W). Input is read
with `ReadConsoleW` (so any Unicode text can be typed) or `read()`. No line-editing library is used. If
arrow-key history is ever wanted it has to be added to both shells; `repl::LineSource` is the one place
it would plug in.

**Colour** is decided once, as in Python: off when `NO_COLOR` is set to anything or stdout is not a
terminal; on for a terminal on Linux and macOS; on Windows when `WT_SESSION`, `TERM` or `ConEmuANSI=ON` is
set, otherwise only if the classic console host accepts virtual-terminal processing (switched on for the
run and restored afterwards). The banner's reveal animation (40 ms per logo line) plays only when colour is on.

**Ctrl+C** at a prompt prints `Phir milenge!` and exits 0. While a statement is running it makes the shell
exit with code 130 as soon as that statement has finished (see the divergences).
```

- [ ] **Step 3: `docs/CPP.md` — Verification against Python**

Add to the command list:

```
python cpp/tests/shell_diff.py      --cli cpp/build/meradb_cli    # the shell, piped scripts: local, and every client x server pair
python cpp/tests/shell_session.py   --cli cpp/build/meradb_cli    # logins, a server dropping mid-session, fallback, hostile input
python cpp/tests/gen_shell_golden.py       # regenerate golden_shell.h (banner, prompts, help, colour) from repl.py
python cpp/tests/gen_shell_help.py         # regenerate src/shell_help_data.inc from repl.py's help tables
python cpp/tests/gen_shell_transcripts.py  # regenerate golden_transcripts.h (what Python's shell prints per script)
```

and a paragraph after the `cross_engine_diff.py` one:

```markdown
`shell_diff.py` pipes each script of `cpp/tests/shell_scripts.py` (statements over several lines,
errors, every dot-command, the blank-line quirk, CRLF and lone-CR input, non-ASCII text, end of input in
every position) into the Python and the C++ shell and compares stdout, stderr and the exit code; in server
mode it does so for every client/server pairing. The generated `golden_*.h` / `.inc` files hold what Python
itself printed, so the unit tests compare the banner, prompts, `.help` text and colour codes byte for byte.
The terminal itself (line editing, Ctrl+C on a console, colour on a real window) cannot be driven from a
test; see the manual checklist in the Phase 3 plan.
```

- [ ] **Step 4: `docs/CPP.md` — Layout rows**

Replace the row `| \`repl.py\` (table output) | \`cli_format.h\` / \`cli_format.cpp\` |` and the `cli.py` row with:

```markdown
| `cli.py` (server, start, stop, status, run, shell) | `cli.h` / `cli.cpp`, `server_control.h` / `server_control.cpp`, `main.cpp` |
| `repl.py` (the shell)               | `repl.h` / `repl.cpp` (loop, dot-commands, `runText` / `runFile`, line sources), `repl_text.h` / `repl_text.cpp` + `shell_help_data.inc` (banner, prompts, help), `cli_format.h` / `cli_format.cpp` (results, colour) |
| `repl.py` colour helpers, `str` methods | `term_style.h` / `term_style.cpp` (`term::Style`, colour detection), `pytext.h` / `pytext.cpp` (Python's `strip` / `split` / `ljust` semantics) |
```

and extend the sentence about `sys_compat.h`: "(environment, time, random bytes, process spawning, and
the terminal: line reading, Ctrl+C, ANSI switch-on)".

- [ ] **Step 5: `docs/CPP.md` — divergences**

Append to "Known divergences from the Python engine" (before "Platform coverage"; also update that bullet to
say the terminal primitives — `ReadConsoleW`, the console control handler, `sigaction`, `read(0)` — have been
compiled on MinGW only and never run against a real terminal in a test):

```markdown
- **Shell: glyphs**: Python falls back to `*` and `#` for the logo and `·` when its stdout cannot encode
  them (a *piped* Python on Windows uses the ANSI code page). The C++ shell always writes UTF-8 and so
  always prints `█` and `·`. (The comparison scripts run Python with `PYTHONIOENCODING=utf-8`.)
- **Shell: Ctrl+C during a statement**: Python's `KeyboardInterrupt` unwinds at once (exit 130, no
  message). A C++ statement is never abandoned half-way (it holds the engine lock, and `WAPAS` exists
  because half-applied writes are not acceptable), so the shell notes the interrupt and exits 130 as soon
  as the statement has finished. Ctrl+C at a prompt behaves like Python (`Phir milenge!`, exit 0).
- **Shell: input that is not valid UTF-8**: Python's strict decoder stops with a traceback; the C++ shell
  passes the bytes on and the tokenizer reports an unexpected character. A NUL byte is likewise an
  ordinary character. Nothing depends on it.
- **Shell: `.help` topics and dot-command words** use `pytext::lower`, a full Unicode `str.lower()`
  (generated tables from Python 3.12 / Unicode 15.0 by `cpp/tests/gen_lower_table.py`, including `U+0130`
  and the final-sigma rule), so `.help ÉCOLE` echoes `école` and `.help <Kelvin sign>unji` finds the
  MUKHYA KUNJI rows exactly as Python does. Only text from a Unicode version other than 15.0 could differ.
  `cpp/tests/shell_lower_diff.py` compares both shells. `pytext` also follows Python's strict UTF-8
  rules, but input that is not valid UTF-8 is passed through (each bad byte is one unit) where Python
  stops with `UnicodeDecodeError`.
- **Shell: `-W` with piped input**: the password prompt reads its answer from the first line of the
  piped stdin; only a real console hides the typing. Not compared against Python.
- **Shell: operating-system wording** after `Server se connection toot gaya:` (a dropped connection) is
  the platform's own text, which differs from Python's (`[WinError 10054] ...` vs a bare message). The
  words before it match.
- **Shell: unprintable characters in a tokenizer error** — `Ye character samajh nahi aaya: '<c>'` escapes
  ASCII controls, U+007F..U+00AD and the common invisible format characters like Python's `repr()`; only
  other characters Python calls unprintable (unassigned, private use) are still shown raw. Non-ASCII
  whitespace is accepted exactly as Python does. No shell comparison script is skipped.
```

Also change the "Windows console" bullet's last sentence ("Not verifiable without an interactive console
here.") to: "Reading a console uses `ReadConsoleW`; piped input is read in binary mode, so Ctrl+Z is an
ordinary character and CRLF is not translated, as with Python's `sys.stdin`."

- [ ] **Step 6: `docs/CPP.md` — Next phases**

Replace the whole Phase 3 bullet with the hand-off below, and adjust the Phase 4 and 5 bullets as shown:

```markdown
- **Phase 3, shell**: done (see "The shell"). Pieces the later phases reuse:
  `term::Style` + `term::detectStyle` (colour decision), `sys::AnsiConsole` / `sys::Utf8Console` (console
  set-up), `formatResult(result, style)` (table output), `repl::runText` (run text on a `Backend` and print
  results), `repl_text.h` (`helpReference()`, `renderReference`, `fullHelp`, the prompt and banner text),
  `pytext` (Python's string semantics), `protocol::kProgramVersion`.
- **Phase 4, workbench**: `Backend::schemaTree()` already returns the JSON the sidebar needs
  (both local and remote); FTXUI panels; a query must run off the UI thread, but a
  transaction's statements must all run on ONE thread, so use a dedicated worker thread per
  session, never a pool. Reuse, do not copy: the help reference rows for a help pane, `runText` /
  `formatResult` for an output pane, `term::Style` where raw ANSI is needed (FTXUI draws its own colours),
  the tokenizer for syntax highlighting (`highlight.py`'s job). Do not use `ConsoleLineSource` or
  `sys::InterruptGuard` inside the full-screen UI: FTXUI owns the terminal and its keys.
- **Phase 5, polish**: `docs/REPORT.md`; grow the divergence list above; consider Unicode
  identifiers (ICU or a small generated table of letter ranges) if full parity is wanted, which would
  also let the tokenizer escape every unprintable character like Python's `repr()`;
  verify the POSIX and MSVC builds first thing and fix warnings (the code follows the
  portability rules but those toolchains have not been run yet). The terminal primitives in `sys_compat`
  (`readTerminalLine`, `InterruptGuard`, `AnsiConsole`) are the part most in need of a real run on Linux,
  macOS and a Visual Studio build; a pseudo-terminal test (Python's `pty` module driving both shells) is
  the natural next check on POSIX.
```

- [ ] **Step 7: `README.md`**

In "### C++ implementation" replace the end of the sentence, from `commands; the interactive shell and the
workbench are Python-only for now.` with `commands and the interactive shell (\`meradb_cli shell\`); the
workbench is Python-only for now.` Re-read the paragraph: it must still be one grammatical sentence.

- [ ] **Step 8: Manual terminal checklist** (run once, by the owner, on real terminals; record the result in the PR description, not in the repo)

The three terminal primitives cannot run under `ctest`. On each terminal below, build `cpp/build`, then run
`cpp/build/meradb_cli shell --local --data <empty temp folder>` and `python -m meradb shell --local --data
<another empty temp folder>` side by side, and compare:

| # | Terminal | Check | Expect |
|---|---|---|---|
| 1 | Windows Terminal (PowerShell) | Banner | Cyan `MERA`, magenta `DB`, dim wordmark, bold tagline; the logo is revealed line by line; same as Python |
| 2 | Windows Terminal | Type `BANAO TABLE t (id INT);` then Enter; type a statement over three lines | Prompts `meradb:main> ` then `      ...> `; results green / red / tables with dim borders |
| 3 | Windows Terminal | Editing: arrow keys, Home / End, insert, Backspace, F7 | Same as in Python's shell |
| 4 | Windows Terminal | Type `DAALO MEIN t MAAN ('é😀नमस्ते');` then `DIKHAO * SE t;` | Accented, emoji and Devanagari text typed and printed intact; Backspace removes a whole emoji |
| 5 | Windows Terminal | Ctrl+Z then Enter at an empty prompt | `Phir milenge!` (after a newline), prompt returns, `$LASTEXITCODE` is 0 |
| 6 | Windows Terminal | Ctrl+C at an empty prompt, and in the middle of a continuation line | Newline, `Phir milenge!`, exit code 0; no stray `^C` text left behind that Python does not also leave |
| 7 | Windows Terminal | Run a slow statement (a cross join of two 3,000-row tables) and press Ctrl+C during it | The statement finishes, the shell exits, exit code 130, nothing printed after the result |
| 8 | Classic console (`conhost`, started with `WT_SESSION` and `TERM` unset) | Banner | Colours appear (virtual-terminal mode was switched on); after exit, later commands in that window print normally |
| 9 | Any | `set NO_COLOR=1` (or `$env:NO_COLOR=1`) then start the shell | Plain text, no reveal pause |
| 10 | Any | `cpp\build\meradb_cli shell < script.txt` and `... | more`; `meradb_cli shell > out.txt` | No colour, no animation, identical to Python's piped output; a script containing Ctrl+Z is not cut short |
| 11 | Linux / macOS terminal (if available) | Banner, a statement, Ctrl+D at an empty prompt, Ctrl+D after typing some text, Ctrl+C at a prompt, Ctrl+C during a slow statement | Same as the Windows rows; compare the Ctrl+D-after-text case with Python's and note any difference in the PR |
| 12 | Any | `meradb_cli shell -W` | The password prompt hides what is typed (console); wrong password: error on stderr, exit 1, no banner |
| 13 | Windows Terminal and classic console | Paste one line longer than 512 UTF-16 units (for example `DIKHAO '` + 600 letters + `';`) | The shell reads the whole line intact (the console read arrives in 512-unit pieces); the echoed result holds all 600 letters |
| 14 | Windows Terminal and classic console | Paste a line in which an emoji (a surrogate pair) straddles the 512-unit boundary: 511 ASCII letters inside a string literal, then `😀`, then the closing `';` | The shell reads the whole line intact; the emoji comes back whole, not as two replacement characters |
| 15 | Windows Terminal and classic console | Start `meradb_cli shell` against a server (not `--local`), run a slow statement (a cross join of two 3,000-row tables) and press Ctrl+C while it runs; repeat with `--local` | The statement result is printed in full, the connection is NOT reported as dropped (no `Server se connection toot gaya`), the shell exits with code 130. Ctrl+C is only allowed to wake the console read, never a statement's socket or file write |
| 16 | Any | "Ctrl+C just before a read": hold Ctrl+C while pressing Enter on a statement, and press Ctrl+C right as a slow statement ends; repeat a few times | Never a hang at the next prompt. A statement already entered runs to the end, then the shell exits 130 without reading another line; a Ctrl+C that reached the prompt before any text exits 0 with `Phir milenge!`. A typed, finished line is never silently discarded |

- [ ] **Step 9: Whole-change check, then commit**

```bash
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure
git diff <phase-3-base> -- meradb examples
git grep -n -i "co-authored" -- cpp docs/CPP.md README.md
```
Expected: the full suite passes (the 503 tests of the start, about 58 new unit test cases, and the three new
Python checks `shell_diff_local`, `shell_diff_server`, `shell_session`); the `git diff` of `meradb` and
`examples` prints nothing; the `git grep` prints nothing. Read `docs/CPP.md` top to bottom once: every
command it names exists, every file it names exists, the divergence list matches Design decisions D4-D7.

```bash
git add docs/CPP.md README.md
git commit -m "Document the C++ shell, its divergences and the hand-off to the next phases"
```

**Completion checklist:**
- [ ] `docs/CPP.md`: coverage, command line, "The shell", verification commands, layout rows, divergences, next phases
- [ ] every divergence in D4-D7 and the OS-wording and unprintable-character ones is listed exactly once
- [ ] README no longer says the shell is Python-only
- [ ] the manual checklist was handed to the project owner (the three terminal primitives are theirs to confirm)
- [ ] no tool names, no personal or course details anywhere in the diff

---

## Batches

| Batch | Tasks | What it delivers | Review focus |
|---|---|---|---|
| A | 1-5 | Pure pieces with no input and no backend: `pytext`, `term::Style` + terminal primitives, coloured result rendering with the Python golden generator, the generated `.help` text, banner and prompts | Golden fidelity (every escape code against Python's own output); the terminal primitives against the MSVC / Windows rules; the Unicode whitespace set |
| B | 6-10 | The shell itself: line sources, `runText` / `runFile` (and the `run` fix), dot-commands, the read-eval-print loop with the 31 Python transcripts, `meradb_cli shell` | Transcript fidelity (prompt placement, the blank-line quirk, goodbye newlines); the Phase 2 `runFile` refactor; Ctrl+C / exit-code rules |
| C | 11-13 | Cross-engine verification: the same scripts through both real shells (local, and every client/server pair), then logins, mid-session drops, fallback and hostile input | That the diffs can fail (Task 11's mutation check); no leaked processes, ports or folders |
| D | 14 | Docs, the manual terminal checklist, the hand-off | Divergence list completeness; honesty about what only a human can check |

Batches go in order; each ends with a green full suite. Within a batch, tasks go in order.

## Phase 3 completion checklist

- [ ] `meradb_cli shell` (and bare `meradb_cli`) runs the interactive shell in local mode, against a C++
      server and against a Python server, and `run` shares its output path (colour, dropped connections)
- [ ] banner, prompts, `.help` output and every colour code equal what `meradb/repl.py` prints, byte for byte
      (generated goldens, both colour modes)
- [ ] all 31 recorded Python transcripts are reproduced by `repl::run`; `shell_diff.py` matches stdout, stderr and
      exit code on 31 scripts in local mode and on 13 scripts through all four client/server pairs
- [ ] `shell_session.py` holds: logins and grants, a server stopping mid-session, local fallback, hostile input
- [ ] Windows console handled: UTF-8 code page, `ReadConsoleW`, virtual-terminal colour switched on and
      restored, piped stdin read in binary mode; POSIX tty read with `read(0)` (not compiled here)
- [ ] no new third-party dependency; no editor library (D1)
- [ ] `.hexdump`, the `^` operator and `GINO(ALAG x)` are NOT implemented (project owner's exercises); `.hexdump`
      is an unknown command in the C++ shell, as in the Python one
- [ ] `docs/CPP.md` and `README.md` updated; divergences D4-D7 recorded; hand-off for Phases 4-5 written
- [ ] manual terminal checklist (Task 14, Step 8) given to the owner
- [ ] `git diff <phase-3-base> -- meradb examples` is empty; no tool or personal details in any file
- [ ] full `ctest` green, zero compiler warnings, no process / pid file / temp folder left behind by any test

