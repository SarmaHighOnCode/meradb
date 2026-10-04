# MeraDB in C++ (Phases 1-4: engine, server, client, shell, workbench)

`cpp/` contains a C++17 port of MeraDB. It mirrors the Python implementation in
`meradb/` layer for layer: the same Hinglish grammar, the same error wording, the same
result formatting, the same **on-disk formats** and the same **wire protocol**. A data
folder written by one implementation can be opened by the other, and Python and C++
clients and servers can be mixed freely.

The Python implementation is unchanged and remains the reference.

## What is covered

Phase 1 (engine):
- Tokenizer, parser, syntax tree, planner, evaluator, aggregates, data types.
- Catalog (`catalog.json`), heap-file storage and hash indexes.
- The executor: DDL, DML (including `INSERT ... SELECT` and upsert), `DIKHAO` with
  joins, `SAMOOH`/`JINKA`, subqueries (correlated and not), set operations, views,
  `SAMJHAO` (EXPLAIN), and transactions (`SHURU`/`PAKKA`/`WAPAS`) with shadow-copy
  snapshots and crash recovery.

Phase 2 (everything around the engine):
- Users and privileges (`BANAO USER`, `HATAO USER`, `ADHIKAR DO/WAPAS`), stored in
  `users.json` (PBKDF2-HMAC-SHA256, compatible both ways with Python).
- Triggers (`BANAO/HATAO TRIGGER`, `PEHLE`/`BAAD`, `NAYA`/`PURANA`) and stored procedures
  (`BANAO/HATAO PROCEDURE`, `CHALAO`).
- The TCP server (`meradb_cli server`, thread per connection, one shared engine lock,
  port 6372), the newline-delimited JSON protocol, the client library (`Connection`), and
  the `start` / `stop` / `status` / `run` commands with Python's automatic local fallback.

Phase 3 (the interactive shell):
- `meradb_cli shell` (and a bare `meradb_cli`): the banner with the block logo, the prompts
  (`meradb:<db>> `, `meradb:<db>*> ` inside a transaction, `      ...> ` while a statement is being
  typed), statements ended by `;`, the dot-commands (`.help [word]`, `.tables`, `.schema <table>`,
  `.run <file>`, `.exit` / `.quit` / `.nikal`), ANSI colour, and Ctrl+C / Ctrl+D / end-of-input
  handling, in local mode and through a server (C++ or Python). It behaves like `python -m meradb
  shell`: the same transcript for the same input, checked by driving both shells with identical
  piped scripts (see "Verification against Python").
- `run` shares the shell's code path: coloured results on a terminal, and a dropped connection prints
  its error on stdout and the next file still runs, as in Python.

Phase 4 (the full-screen workbench):
- `meradb_cli workbench` (also `tui` and `--tui`) opens the same screen as `python -m meradb workbench`: a
  header with `host:port  |  db: <db>` and a `TRANSACTION (PAKKA / WAPAS)` marker, the schema tree, the
  results table (`KHALI`, `SACH` / `JHOOTH`, right-aligned numbers, `Results -- N row(s)`), the log (echo of
  each statement with syntax colours, green messages, red errors, `(x.x ms)`), a multi-line query editor with
  line numbers and syntax highlighting, and the footer. Every key binding, Hinglish label and message of
  `tui.py` is kept: F5 / Ctrl+R run (the selection if there is one), F6 explains, Ctrl+Up / Ctrl+Down walk
  the query history, Ctrl+S writes a CSV, Ctrl+O opens the connect dialog (server or Local mode), F1 opens
  the help (the key table plus `docs/LANGUAGE.md`), Ctrl+L clears the log, Ctrl+Q quits.
- Statements never block the screen: each session has ONE worker thread that runs every database call (so a
  transaction's statements stay on the thread that started it), and keys pressed meanwhile are queued. A busy
  label shows `[chal raha hai]` / `[chal raha hai +N]`.
- Ctrl+C never kills the workbench (it logs a hint); Ctrl+Q waits for a running statement, rolls an open
  transaction back and leaves the terminal as it found it. See "The workbench".

Not yet (Phase 5): the final polish pass.

## Prerequisites

- CMake 3.20 or newer
- A C++17 compiler: MinGW-w64 g++, MSVC (Visual Studio 2019 or newer), or g++/clang
  on Linux and macOS
- Network access on the first configure: nlohmann/json and Catch2 are downloaded
  automatically by CMake (`FetchContent`), and so is [FTXUI](https://github.com/ArthurSonzogni/FTXUI)
  v5.0.0 (the terminal UI library of the workbench; statically linked). Nothing else needs installing.
- `-DMERADB_WORKBENCH=OFF` builds without FTXUI (nothing is fetched for it); `workbench` then prints a
  note and exits 1, and everything else is unchanged. `-DFETCHCONTENT_SOURCE_DIR_FTXUI=<dir>` builds from an
  unpacked copy of the v5.0.0 tarball instead of downloading it (an offline checkout).

## Build

MinGW-w64 (Windows):

```
cmake -S cpp -B cpp/build -G "MinGW Makefiles"
cmake --build cpp/build
```

MSVC (Windows, from a Developer prompt):

```
cmake -S cpp -B cpp/build -G "Visual Studio 17 2022"
cmake --build cpp/build --config Release
```

Linux / macOS:

```
cmake -S cpp -B cpp/build
cmake --build cpp/build
```

The build uses `-Wall -Wextra` (`/W4` on MSVC) and is warning-free.

## Run the tests

```
ctest --test-dir cpp/build --output-on-failure
```

(With MSVC add `-C Release`.) Besides unit tests per layer, the engine tests replay
scripts under `cpp/tests/golden/` and compare every statement's message, error text,
columns and rows against outcomes recorded from the Python engine
(`cpp/tests/golden/gen_golden.py` regenerates `cpp/tests/golden_engine.h`).

## Command line

```
meradb_cli run <script.mdb> [more.mdb ...] [-D <dir>] [--local] [-H host] [-p port] [-U user] [-W] [-d database]
meradb_cli server  [-D <dir>] [--host 127.0.0.1] [--port 6372] [--password pw] [-v]
meradb_cli start   [same options]      # background; log in <data>/server.log
meradb_cli status | stop [--force] [-D <dir>] [-W]
meradb_cli shell  [-D <dir>] [--local] [-H host] [-p port] [-U user] [-W] [-d database]   # also: no command at all
meradb_cli workbench [same options as shell]   # also: tui, --tui
```

`workbench` needs a terminal on both stdin and stdout.

`meradb_cli x.mdb` means `run x.mdb`. `--data` is accepted as a long form of `-D`. `run` connects
to a server if one answers (the port of the data folder's `meradb.pid`, `MERADB_HOST` /
`MERADB_PORT`, else 127.0.0.1:6372) and otherwise opens the data folder directly ("LOCAL
mode", with a note on stderr); an explicit `-H` / `-p` never falls back. Output matches
`meradb run`: each statement's result followed by a blank line; a failing statement does not
stop the script; exit code 1 if any statement failed. Environment: `MERADB_DATA` (data
folder), `MERADB_PORT`, `MERADB_HOST`, `MERADB_PASSWORD` (the server-wide password),
`MERADB_USER`. The data folder defaults to the same per-user folder as Python
(`%LOCALAPPDATA%\MeraDB\data`, or `~/.local/share/MeraDB/data`).

### The shell

`meradb_cli shell` connects exactly like `run` (a server if one answers, otherwise a local data folder
with a note on stderr; `-U` / `-W` / `MERADB_USER` / `MERADB_PASSWORD` log in; `-d` picks the database),
prints the banner and reads statements until `.exit`, Ctrl+D (Ctrl+Z then Enter on a Windows console) or
the end of its input. A failed login or an unreachable explicit server prints an error on stderr and exits
1 before any banner.

**Statements.** A statement ends at a line whose last non-blank character is `;` (the text is not
inspected, so a `;` that ends a line inside a string literal also ends it, as in Python). Until then the
prompt is `      ...> `. The prompt is `meradb:<db>> `, with a `*` before the `>` while a transaction is open.
A statement that fails prints its error and the shell carries on. When the shell ends with a transaction
open, it is rolled back.

**Dot-commands.** A line that starts with `.` (after stripping) is a command only while no statement is
being typed, so after a line without `;` a `.tables` is statement text, as in Python. The command word is
case-insensitive.

| Command | Does |
|---|---|
| `.help` | the shell commands, the full keyword reference (syntax, SQL equivalent, description) and examples |
| `.help <word>` | only the reference rows that mention the word (several words are one phrase) |
| `.tables` | runs `DIKHAO TABLES;` |
| `.schema <table>` | runs `BATAO <table>;` (only the first word is used) |
| `.run <file>` | runs a script file like `meradb_cli run` (UTF-8, optional byte-order mark); an unreadable file prints `File nahi khuli: ...` |
| `.exit`, `.quit`, `.nikal` | prints `Phir milenge!` and leaves (extra words are ignored) |

Anything else, including `.schema` or `.run` without an argument, prints `Ye shell command nahi pata:
<line>  (.help dekho)`. `.hexdump` is not a command in either shell: it is reported as unknown.

**Input.** Piped input works the same way as a terminal and prints the same transcript, prompts included.
Lines end with `\n`, `\r\n` or a lone `\r`; a last line without a terminator still counts; Ctrl+Z in a
pipe is an ordinary character.

**Line editing.** Like Python's shell (which calls `input()` and never loads `readline`), the C++ shell has
no history file, completion or key handling of its own: what you get is the terminal's editing. On a
Windows console that is the console host's (arrow keys, Home / End, F7 history of the session); on a POSIX
terminal it is the kernel's line editing (Backspace, Ctrl+U, Ctrl+W). Input is read with `ReadConsoleW`
(any Unicode text can be typed or pasted) or `read()`. No line-editing library is used. If arrow-key
history is ever wanted it has to be added to both shells; `repl::LineSource` is the one place it would
plug in.

**Colour** is decided once, as in Python: off when `NO_COLOR` is set to a non-empty value or stdout is not a
terminal; on for a terminal on Linux and macOS; on Windows when `WT_SESSION`, `TERM` or `ConEmuANSI=ON` is
set, otherwise only if the classic console host accepts virtual-terminal processing (switched on for the
run and restored afterwards). The banner's reveal animation (40 ms per logo line) plays only when colour
is on. Results are green (messages), red (errors) and tables have dim borders; the prompt names the
database in bold cyan and marks an open transaction with a bold yellow `*`.

**Ctrl+C, Ctrl+D, end of input.** At a prompt, Ctrl+C, Ctrl+D (Ctrl+Z, Enter on a Windows console) and the
end of piped input all print a newline and `Phir milenge!` and exit with code 0; a half-typed statement is
discarded. If Ctrl+C arrives while a statement is running, the statement is allowed to finish (see the
divergences), its result is printed, no further line is read and the shell exits 130.

**Exit codes.** 0 after a normal end (`.exit`, end of input, Ctrl+C at a prompt); 130 for Ctrl+C during a
statement; 2 for a bad option (a usage error, the same in both shells); 1 when the shell cannot start
(failed login, no server for an explicit `-H` / `-p`, unusable data folder) and, on POSIX, when reading the
terminal itself fails (one line `OSError: [Errno N] ...` on stderr where Python prints a traceback). Errors inside statements never change the exit code of the shell; `run` returns 1 if
any statement failed.

### The workbench

`meradb_cli workbench` (aliases `tui` and `--tui`) connects exactly like the shell (a server if one answers,
otherwise the local data folder with a note on stderr; `-U` / `-W` / `MERADB_USER` / `MERADB_PASSWORD` log in;
`-d` picks the database; a bad option is a usage error, exit 2) and then takes over the terminal. It is built
on [FTXUI](https://github.com/ArthurSonzogni/FTXUI) and needs a terminal on stdin AND stdout: otherwise one
line on stderr (`Workbench ke liye terminal chahiye ...`) and exit 1, before anything is opened.

**Layout** (the same as `tui.py`): a header line with the title and `<host:port or local (...)>  |  db:
<db>[  |  TRANSACTION (PAKKA / WAPAS)]`; on the left the `Schema` tree (32 columns wide); on the right the
`Results` table, a `Log` (10 rows) and the `Query  [F5 = chalao, F6 = samjhao]` editor (9 rows); a footer
with the main keys. The focused panel has a heavy amber border and an inverse title; Tab / Shift+Tab cycle schema tree, results,
log, editor, and it starts in the editor. The smallest supported window is 60 x 24; below that a message asks
for a bigger terminal.

| Key | Does |
|---|---|
| F5, Ctrl+R | run the editor text, or only the selection if there is one |
| F6 | `SAMJHAO` the single statement in the editor (or in the selection); a message if there is not exactly one |
| Ctrl+Up / Ctrl+Down | previous / next query from the history (kept in memory only, consecutive duplicates collapse); the editor gets the focus |
| Ctrl+S | save the last result table to `exports/meradb-YYYYmmdd-HHMMSS.csv` in the current directory |
| Ctrl+O | connect dialog: Host, Port, Password (hidden), Database, buttons Connect / Local mode / Cancel; Enter connects, Esc cancels |
| F1, Esc, `q` | open / close the help (`q` only closes it, as in Python; these keys plus `docs/LANGUAGE.md`); PageUp / PageDown scroll it |
| Ctrl+L | clear the log |
| Ctrl+Q | quit |
| Tab, Shift+Tab | next / previous panel |
| Enter in the tree | column: insert its name at the editor cursor; table: `DIKHAO * SE <table> SIRF 100;`; database: `ISTEMAL <db>;` |
| Right / Left in the tree | expand (or step to the first child) / collapse (or step to the parent) |
| Ctrl+P / Ctrl+N | history, for terminals that do not deliver Ctrl+Up / Ctrl+Down (extra) |
| Shift+arrows / Home / End, Ctrl+A | select text in the editor; Ctrl+A selects all (extra) |
| PageUp / PageDown, mouse wheel | scroll the focused panel (extra) |
| mouse click | focus the clicked panel (extra) |

**Statements run on a worker thread.** Every database call of a session (statements, the schema refresh,
connecting, closing) runs on ONE worker thread, in order; results come back to the screen as posted
closures. So the screen never freezes, a transaction's statements always run on the thread that began it, and
keys pressed while a statement runs are accepted and queued in order (a second F5 queues a second run). The
header shows `[chal raha hai]`, or `[chal raha hai +N]` with N more waiting. The log echo of a statement
appears when it starts.

**Ctrl+Q while a statement runs.** Statements that have not started are dropped, the running one finishes (a
statement is never abandoned half-way), the backend is closed on the worker (which rolls an open transaction
back), and then the screen closes. The header says `[band ho raha hai ...]` meanwhile; a second Ctrl+Q does
nothing. Exit code 0.

**Ctrl+C never quits.** While the workbench runs, the terminal modes that turn keys into signals or flow
control are switched off (`ISIG`, `IXON` and `IEXTEN` on POSIX, `ENABLE_PROCESSED_INPUT` on Windows), so
Ctrl+C, Ctrl+S, Ctrl+Q and Ctrl+O reach the program as ordinary keys. Ctrl+C logs `Bahar niklne ke liye
Ctrl+Q dabao.` in yellow and nothing else happens. The modes are restored on every exit path. If a signal
still ends the screen (SIGTERM, a hang-up, a terminal that ignores the mode change), the program waits for
the worker and closes the backend, so an open transaction is rolled back cleanly. A hard kill, a closed
window or Ctrl+Break leaves it to crash recovery at the next start, as with Python's workbench. Ctrl+C will
not reach the workbench at all if it was started from a launcher that disabled Ctrl+C for its children (some
IDE run buttons and task runners do): start it from a normal terminal window.

**Mouse.** Mouse tracking is on (clicks focus a panel, the wheel scrolls), so the terminal's own text
selection needs the Shift key held while dragging (or your terminal's equivalent modifier).

**CSV.** Ctrl+S follows Python's `csv.writer` defaults exactly: `\r\n` line ends, minimal quoting, a lone
empty field in a one-column row written as `""`, `KHALI` written as an empty field, UTF-8 without a BOM, a
header row. The log gets `<n> row(s) CSV mein save: <absolute path>`. A failure to write is logged in red
(`CSV save nahi hua: <reason>`).

**Connecting.** The dialog starts with the current server's host and port (127.0.0.1 and 6372 for a local
backend). As in Textual's `Input`, a field selects its whole text when it gets the focus (the Host field when the
dialog opens, then each field you Tab to): the next character typed replaces it, Backspace / Delete clear it, Left /
Home go to the start, Right / End to the end, and each of those ends the selection (a character the Port field
refuses, such as a letter, changes nothing). The Pilot scenario `connect_dialog_select_on_focus` compares this with
Python key by key. Connect opens a connection with the values; Local mode opens the data folder of the `--data`
option. A failure logs `Connect nahi hua: <reason>` in bold red and changes nothing; success logs
`Connected: <description>` in bold green, refreshes the tree and header, and closes the old backend (which
rolls its transaction back).

**Help.** The help screen is the key table of `tui.py` plus an "extra keys" table, then
`docs/LANGUAGE.md`, drawn by a small Markdown renderer (headings, tables, lists, code, bold, inline code).
The language reference is embedded into the binary at build time (`cmake/embed_file.cmake`).

**Colours.** The Dracula RGB values of `highlight.RICH_STYLES` and `tui.py` (`#ff79c6` keywords, `#f1fa8c`
strings, `#bd93f9` numbers, `#6272a4` comments, `#8be9fd` types, `#50fa7b` functions and messages, `#ffb86c`
PK / UQ, `#ff5555` errors). They are truecolor where the terminal has it; FTXUI down-converts to 256 or 16
colours on weaker terminals.

**Exit codes.** 0 after a normal quit; 1 when there is no terminal, the backend cannot be opened, or the
build has no workbench (`-DMERADB_WORKBENCH=OFF`: `workbench` prints `abhi C++ version mein nahi hai`);
2 for a usage error. FTXUI is linked statically, so Python's "install Textual" path does not exist.

**What is not tested automatically.** The terminal itself: the real byte sequences a terminal sends for
Shift+arrows and Ctrl+Up, the terminal-mode guard on POSIX, the mouse, and how the screen looks. See the
manual checklist (start with the key probe).

## Server notes

One thread per connection; each connection owns its engine session (current database,
transaction) on that thread. All sessions share one instance and one lock: a statement waits
up to 10 s for another session's transaction ("Database busy hai"). A client that disconnects
in the middle of a transaction is rolled back, and so is every open transaction when the
server is stopped (Python leaves those to crash recovery). A data folder that a server is
serving cannot be opened directly by another process. The protocol is documented in
`docs/SERVER.md` (unchanged: the C++ server speaks exactly that).

## Verification against Python

Everything below needs Python and the repo checkout; each is also a ctest test.

```
python cpp/tests/cross_engine_diff.py [--via-server] examples/demo.mdb
python cpp/tests/cross_engine_diff.py [--via-server] examples/rdbms_lab_coverage.mdb
python cpp/tests/interop_check.py   --cli cpp/build/meradb_cli    # Python/C++ clients x servers, raw protocol, logins
python cpp/tests/interchange_check.py --cli cpp/build/meradb_cli  # data folders passed between the engines
python cpp/tests/fuzz_triggers.py   --cli cpp/build/meradb_cli    # seeded trigger/procedure fuzz
python cpp/tests/cli_lifecycle.py   cpp/build/meradb_cli          # start / status / run / stop with real processes
python cpp/tests/shell_diff.py      --cli cpp/build/meradb_cli    # the shell, piped scripts: local, and every client x server pair
python cpp/tests/shell_session.py   --cli cpp/build/meradb_cli    # logins, a server dropping mid-session, fallback, hostile input
python cpp/tests/shell_lower_diff.py cpp/build/meradb_cli         # .help topics and dot-command words with non-ASCII case
python cpp/tests/gen_shell_golden.py       # regenerate golden_shell.h (banner, prompts, help, colour) from repl.py
python cpp/tests/gen_shell_help.py         # regenerate src/shell_help_data.inc from repl.py's help tables
python cpp/tests/gen_shell_transcripts.py  # regenerate golden_transcripts.h (what Python's shell prints per script)
python cpp/tests/gen_lower_table.py        # regenerate the Unicode lower-case tables (needs Python 3.12 / Unicode 15.0)
python cpp/tests/workbench_diff.py --probe cpp/build/tests/wb_probe   # the workbench against Python's, scenario by scenario (needs Textual)
python cpp/tests/gen_highlight_golden.py   # regenerate golden_highlight.h (what highlight.py colours, line by line)
python cpp/tests/gen_word_table.py         # regenerate word_table.inc (Python's \w, for number boundaries in the highlighter)
```

`cross_engine_diff.py` runs a script through the Python engine and the C++ CLI on fresh data
folders and diffs the output and exit code (`--via-server` sends every statement through a C++
server). The harness looks for the CLI in `cpp/build` and `cpp/build/Release`; otherwise pass
`--cli`. `demo_phase1.mdb` and `rdbms_lab_coverage_phase1.mdb` are the Phase 1 copies of the
examples without the users / trigger / procedure sections; they are still compared.

`shell_diff.py` pipes each script of `cpp/tests/shell_scripts.py` (31 scripts: statements over several
lines, errors, every dot-command, the blank-line quirk, CRLF and lone-CR input, non-ASCII text and
whitespace, end of input in every position) into the Python and the C++ shell and compares stdout, stderr
and the exit code. In server mode it does so for 13 of those scripts through every client/server
pairing: the Python shell against a Python server is the baseline, and the C++ shell against either server
and the Python shell against the C++ server must reproduce it. `shell_session.py` covers what a script
cannot: logins and grants, a server that stops or drops the connection mid-session, local fallback and
hostile input. The generated `golden_*.h` / `.inc` files hold what Python itself printed, so the unit
tests compare the banner, prompts, `.help` text and colour codes byte for byte, and replay all 31 recorded
Python transcripts through `repl::run`. The terminal itself (line editing, Ctrl+C on a console, colour in a
real window) cannot be driven from a test; see "Manual terminal checklist".

The workbench is checked at three levels. Unit tests per piece (`wbtext`, `wbeditor`, `wbtree`, `wbworker`,
`wbsession`, `wbhelp`, `wbform`, `highlight`), and the whole window rendered into an FTXUI screen and driven
by synthetic key events against a scripted backend (`wbui`). `wbe2e` runs the real engine, the real session
with its worker thread and the real window against a temp data folder (and a real server on a free port for
the connect dialog): typing a script, the log and table colours, selection-aware F5 / F6, history, the tree,
the transaction marker, quitting with a transaction open, Ctrl+Q behind a slow statement, the CSV bytes, the
connect dialog both ways, the help screen, every size and Unicode alignment. `workbench_diff.py` drives both
workbenches through `cpp/tests/workbench_scenarios.json` (12 scenarios: start-up, DDL / DML / SELECT, errors,
history and explain, selection, the tree, transactions, CSV, clearing the log, Unicode, connecting to local
mode, the connect dialog's select-on-focus): the Python one with Textual's `Pilot` (`workbench_pilot.py`), the C++ one through the `wb_probe`
program, and compares what each shows (header, editor, history, results title and cells with their colour
kind, log entries with their kind, every tree node, the newest CSV) after normalising folders and timings.
It skips (ctest skip code 77) when Textual is not installed. The highlighter is compared line by line with
`highlight.py` (`golden_highlight.h`).

## Layout

Headers are in `cpp/include/meradb/`, sources in `cpp/src/`, tests in `cpp/tests/`.

| Python module                       | C++ files                                         |
|-------------------------------------|---------------------------------------------------|
| `errors.py`                         | `errors.h`                                        |
| `tokenizer.py`                      | `tokenizer.h` / `tokenizer.cpp`                   |
| `ast_nodes.py`                      | `ast.h` / `ast_util.cpp` (+ `ast_util.h`)         |
| `parser.py`                         | `parser.h` / `parser.cpp`                         |
| `datatypes.py`                      | `datatypes.h` / `datatypes.cpp`                   |
| `catalog.py`                        | `catalog.h` / `catalog.cpp`                       |
| `storage.py`                        | `storage.h` / `storage.cpp`                       |
| `table.py`                          | `table.h` / `table.cpp`                           |
| `planner.py`                        | `planner.h` / `planner.cpp`                       |
| `evaluator.py`                      | `evaluator.h` / `evaluator.cpp`                   |
| `aggregates.py`                     | `aggregates.h` / `aggregates.cpp`                 |
| `engine.py`                         | `engine.h` / `engine.cpp` (`Instance`, `Engine`)  |
| `users.py`                          | `users.h` / `users.cpp`, `crypto.h` / `crypto.cpp` (in-tree SHA-256, HMAC, PBKDF2) |
| `protocol.py`                       | `protocol.h` / `protocol.cpp`, `pyjson.h` / `pyjson.cpp` (Python-compatible JSON) |
| `server.py`                         | `server.h` / `server.cpp`                         |
| `client.py`                         | `client.h` / `client.cpp`, `backend.h`            |
| `cli.py` (server, start, stop, status, run, shell) | `cli.h` / `cli.cpp`, `server_control.h` / `server_control.cpp`, `main.cpp` |
| `repl.py` (the shell)               | `repl.h` / `repl.cpp` (loop, dot-commands, `runText` / `runFile`, line sources), `repl_text.h` / `repl_text.cpp` + `shell_help_data.inc` (banner, prompts, help), `cli_format.h` / `cli_format.cpp` (results, colour) |
| `repl.py` colour helpers, `str` methods | `term_style.h` / `term_style.cpp` (`term::Style`, colour detection), `pytext.h` / `pytext.cpp` + `lower_table.inc` (Python's `strip` / `split` / `ljust` / `lower` / `int()` semantics) |
| `highlight.py`                      | `highlight.h` / `highlight.cpp` + `word_table.inc` (in `meradb_core`: a lenient scanner sharing the tokenizer's keyword list) |
| `tui.py` (the workbench)            | `workbench.h` / `workbench.cpp` (`runWorkbench`: the FTXUI loop and the bridge from the worker to the screen), `wb_session.h` / `wb_session.cpp` (`MeraDBApp` without widgets: every action, the log, history, the quit protocol), `wb_worker.h` / `wb_worker.cpp` (the one thread per session), `wb_text.h` / `wb_text.cpp` (styles, log, cells, CSV, clipping, history, scrolling), `wb_editor.h` / `wb_editor.cpp` (`QueryEditor`: text buffer, selection, highlighted rows), `wb_tree.h` / `wb_tree.cpp` (the schema tree and `refresh_schema`), `wb_panels.h` / `wb_panels.cpp` (the four panels), `wb_ui.h` / `wb_ui.cpp` (the window, key routing, focus), `wb_keys.h` (every key binding; the only place that knows event bytes), `wb_form.h` / `wb_form.cpp` + `wb_dialogs.h` / `wb_dialogs.cpp` (`ConnectScreen`, `HelpScreen`), `wb_markdown.h` / `wb_markdown.cpp` + `wb_help.h` / `wb_help.cpp` (`KEYS_HELP` + the embedded `docs/LANGUAGE.md`) |
| (build helper)                      | `cmake/embed_file.cmake` turns `docs/LANGUAGE.md` into a byte array at build time |

Supporting files with no Python counterpart: `pyvalue.h` / `pyvalue.cpp`
(Python-compatible equality and hashing of values) and `ordered_map.h`.

Also without a Python counterpart: `net_compat.h` / `net_compat.cpp` (Winsock / BSD sockets)
and `sys_compat.h` / `sys_compat.cpp` (environment, time, random bytes, process spawning, and
the terminal: line reading, Ctrl+C, ANSI switch-on, and `sys::TerminalModeGuard`, which switches off
Ctrl+C and flow control for the workbench): the only places that include platform headers
(apart from `stack_guard.cpp`, which asks the OS for the thread's stack limits).

The workbench is a separate static library, `meradb_workbench`, so `meradb_core` does not depend on FTXUI:
`cli.cpp` reaches it through a function-pointer hook (`setWorkbenchRunner`) that `main.cpp` sets.

Design notes: syntax-tree nodes are owned by `std::unique_ptr` (no raw `new` or
`delete`); values are a `Value` class over `std::variant`; errors are C++
exceptions carrying the same `[Stage Galti] message` text as Python, with a
`stage()` accessor and a bare `message()` accessor.

## Known divergences from the Python engine

These are deliberate and small.

- **ALTER ADD composite constraint**: the C++ engine changes a copy of the schema and
  saves it only after the data check passes. Python changes its in-memory schema
  first, so after a failed ALTER a Python session keeps a constraint that was never
  saved.
- **Sorting mixed types** (for example TEXT against INT produced by an `AGAR`
  expression): Python raises an unhandled `TypeError`; C++ reports an execution
  error with a Hinglish message.
- **Correlated unqualified column inside a subquery over grouped rows** (in `JINKA`
  or the select list of a grouped query): Python crashes; C++ evaluates it.
- **Single-column composite constraint** (`ANOKHA (a)` on a column that is also
  unique): the two share an index in C++, so only the wording of the duplicate
  message can differ. Not reachable in practice.
- **Index cache**: each `Instance` owns its indexes; nothing is shared between two
  `Instance` objects on the same folder.
- **Parse-depth caps (hostile input)**: Python dies with `RecursionError` on a deeply nested
  query (about 110 nested parentheses). C++ limits ONE statement to 400 operator/nesting
  units (`Query bahut gehri (nested) hai (limit 400)`), statements nested inside statements
  (trigger and procedure bodies, `SAMJHAO`) to 32, and views defined over views to 32. On top
  of those counts every recursive function (parser, evaluator, planner, subquery execution,
  tree copies, restoring constants) measures the stack bytes used since the entry point and
  refuses past a budget (`Query bahut gehri (nested) hai (stack limit 512 KB)`). The budget is
  the smaller of 512 KB and half of the stack the current thread really has left below its
  entry point, asked from the OS once per thread (Windows `GetCurrentThreadStackLimits`, Linux
  `pthread_getattr_np`, macOS `pthread_get_stackaddr_np`/`pthread_get_stacksize_np`; 512 KB
  where the OS cannot say). So a 2 MB MinGW thread uses 512 KB, a 1 MB MSVC thread a little under that, while a
  512 KB macOS thread or a 128 KB musl thread gets a proportionally smaller budget (and the
  message names that smaller number). It trips first for most shapes (about 150 nested
  parentheses, 95 nested subqueries on a 1 MB stack). JSON decoding is a separate, fixed
  cap: more than 512 nested arrays/objects in one protocol frame are refused (Python's `json`
  accepts over 1,000 levels), and it is a level count, not a byte guard. The caps exist so
  that a network client cannot crash the server; ordinary scripts never come near them.
- **Very wide natural joins**: `a SAMAAN MILAO b` over thousands of common columns builds one
  `=` per column. Python raises `RecursionError` at about 500 common columns (300 passes, 600 fails); C++ combines
  them in a balanced tree (same evaluation order and results) and keeps working, so a 20,000
  column join runs.
- **Trigger/procedure recursion cap**: a trigger that (directly or indirectly) fires itself
  endlessly makes Python raise `RecursionError`; C++ stops at 32 levels with
  `Trigger/procedure bahut gehra chal raha hai (limit 32) -- shayad koi trigger khud ko
  baar-baar chala raha hai`.
- **Non-ASCII identifiers**: Python accepts any Unicode letter in a table or column name;
  C++ accepts ASCII letters, digits and `_` only (Unicode letter classes need tables the
  project does not depend on). Non-ASCII text in string literals and data is fine.
  Whitespace is NOT part of this difference: the tokenizer skips exactly the 29 characters
  Python's `str.isspace()` accepts (no-break space, U+3000, U+2028/2029, U+0085,
  U+001C..U+001F, ...) plus U+FEFF, and counts one column per character. In the
  `Ye character samajh nahi aaya: '<c>'` error the character is written like Python's
  `repr()` for ASCII controls (`'\x00'`, `'\x1b'`), U+007F..U+00AD, and the common invisible
  format characters (`'\u200b'`, `'\u2060'`); other characters Python's tables call
  unprintable (unassigned or private-use code points) are shown raw. Quoted file names and values built
  by the engine's own `repr` emulation (for example in `File nahi khuli:`) do not escape unprintable
  characters at all. The same whitespace set is used wherever Python strips or splits user text: the
  stored text of a CHECK (`SHART`) constraint, `.help` and other dot-command arguments, the server's
  verbose query log and `--port` numbers (`int()` additionally refuses U+001C..U+001F).
- **INT overflow family**: Python integers are unbounded, C++ `INT` values are 64-bit.
  Arithmetic that overflows 64 bits is reported as an error instead of producing a big number.
- **Command-line abbreviations**: Python's `argparse` accepts unambiguous abbreviations of long
  options (`--dat` for `--data`); the C++ command line requires the full option name. Glued
  short options (`-DDIR`, `-D=DIR`, `-p7`) and bundles (`-WD DIR`), and the error for a value
  given to a flag (`--local=1`), do match Python.
- **Command-line integers**: `-p` / `--port` read the text like Python's `int()` (surrounding
  blanks, a sign, `1_0`, decimal digits of any script such as `-p ٣`); numbers beyond a
  32-bit `int` are rejected as `invalid int value`, where Python converts them and fails
  later. A port outside 0-65535 or an unresolvable host is reported in the program's own words
  rather than the operating system's.
- **`stop` / `status` with `-W`** ask for the password before looking for a server; Python
  only asks once it has found one.
- **`--help` layout**: each subcommand prints argparse's text (usage, options with their help
  texts and defaults) laid out for an 80-column terminal. Python re-wraps to `COLUMNS`; C++
  always uses 80. Lines are filled by characters (not bytes) and an over-long word is chopped
  like `textwrap` does; only the break after a hyphen inside a long data-folder name is not
  reproduced.
- **Windows console**: when stdout/stderr (or stdin) is a console, the program switches its
  code page to UTF-8 for the run and restores it afterwards, so non-ASCII result text prints
  correctly; redirected output is plain UTF-8 either way. Reading a console uses `ReadConsoleW`;
  piped input is read in binary mode, so Ctrl+Z is an ordinary character and CRLF is not translated,
  as with Python's `sys.stdin`.
- **Non-ASCII arguments and environment (Windows)**: the command line and environment values
  are read through the wide Windows API and handled as UTF-8, so `--data` folders, script
  names and `MERADB_*` values with accented letters work as in Python. Unlike a MinGW
  program's usual behaviour, `*.mdb` on the command line is not expanded by the program
  (neither does Python); the shell does it, if at all.
- **Server shutdown**: stopping the server rolls back every open transaction cleanly; Python's
  daemon threads die with the process and rely on crash recovery at the next start. The data
  folder ends up the same. A session blocked sending a reply to a client that has stopped
  reading is dropped (and rolled back) once the server is stopping, so `shutdown` and Ctrl+C
  always finish; Python's daemon threads never wait for it either.
- **Protocol error wording**: `Galat message: ...` for malformed JSON carries the JSON
  library's own text in C++ (Python: `Expecting value: line 1 column 1 (char 0)`); invalid UTF-8
  and the repr in `Unknown request type: ...` match Python. An oversize line is answered with ONE
  `Message bahut bada hai` and its remainder is skipped (Python reads the tail as further
  messages and answers each).
- **`start`** launches `meradb_cli server` (this program), not `python -m meradb server`.
- **SHA-256 in-tree** (no picosha2 as the design spec suggested; see the plan's D4).
- **Shell: glyphs**: Python falls back to `#` for the logo and `*` for the separator dot when its
  stdout cannot encode `█` and `·` (a *piped* Python on Windows uses the ANSI code page). The C++
  shell always writes UTF-8 and so always prints the block and the dot. (The comparison scripts run
  Python with `PYTHONIOENCODING=utf-8`.)
- **Shell: Ctrl+C during a statement**: Python's `KeyboardInterrupt` unwinds at once (exit 130, no
  message). A C++ statement is never abandoned half-way (it holds the engine lock, and `WAPAS` exists
  because half-applied writes are not acceptable), so the shell notes the interrupt, lets the statement
  finish, reads no further line and exits 130. On Windows the Ctrl+C handler cancels only the console
  read, never a statement's socket or file write. Ctrl+C at a prompt behaves like Python (`Phir
  milenge!`, exit 0); one that lands in the few microseconds between the shell's last check and the start
  of the next read is by definition at the prompt (it is printed already) and also exits 0.
- **Shell: terminal-native line editing, no history**: the same as Python (no `readline`), but the
  terminals' own editing is what the user gets, so details such as Ctrl+D after typed text on a POSIX
  terminal are the kernel's and are not compared automatically (manual checklist).
- **Shell: input that is not valid UTF-8**: Python's strict decoder stops with a traceback; the C++ shell
  passes the bytes on and the tokenizer reports an unexpected character. A NUL byte is likewise an
  ordinary character. Nothing depends on it.
- **Shell: `.help` topics and dot-command words** use `pytext::lower`, a full Unicode `str.lower()`
  (generated tables from Python 3.12 / Unicode 15.0 by `cpp/tests/gen_lower_table.py`, including `U+0130`
  and the final-sigma rule), so `.help ÉCOLE` echoes `école` exactly as Python does. Text from a
  Unicode version other than 15.0 could differ; the generator refuses to run on another version.
  `cpp/tests/shell_lower_diff.py` compares both shells. `pytext` also follows Python's strict UTF-8
  rules, but input that is not valid UTF-8 is passed through (each bad byte is one unit) where Python
  stops with `UnicodeDecodeError`.
- **Shell: `-W` with piped input**: the password prompt reads its answer from the first line of the
  piped stdin; only a real console hides the typing. Not compared against Python.
- **Shell: operating-system wording** after `Server se connection toot gaya:` (a dropped connection) is
  the platform's own text, which differs from Python's (`[WinError 10054] ...` vs a bare message). The
  words before it match. For `File nahi khuli:` the `[Errno N]` number and text match Python for a
  missing file, a directory and an invalid name.
- **Workbench: editor selection**: Shift+arrows / Home / End, Ctrl+Shift+Left / Right and Ctrl+A
  select, and F5 / F6 use the selection as in Python. There is no mouse selection and there are no
  clipboard commands; the terminal's own paste works (it arrives as typed characters) and, because mouse
  tracking is on, the terminal's own text selection needs Shift held. Textual's `TextArea` has all of those.
  The Shift+arrow byte sequences are matched in `wb_keys.h`; `wb_keyprobe` shows what a given terminal sends.
- **Workbench: layout and wrapping**: the editor scrolls horizontally instead of wrapping lines, and the
  log wraps by characters, not at words (Textual wraps both softly / at words); the schema tree draws
  `▼` / `▶` expanders instead of guide lines; a `KHALI` cell is dim only (FTXUI has no italic); of the zebra
  stripes, even rows keep the terminal's own background and only odd rows are tinted; the help is a dialog
  titled `Madad`; table rows and code lines in the help are not wrapped to the window width.
- **Workbench: help text embedded at build time**: `docs/LANGUAGE.md` is compiled into the binary
  (a built program has no reliable path to the repository), so editing the file needs a rebuild; a
  configure that does not find the file embeds `*(docs/LANGUAGE.md nahi mila)*` and adding the file later
  needs a reconfigure. Python reads the file each time the help opens. The Markdown rendering is a small
  subset written for these two documents.
- **Workbench: colours**: the Dracula values of `highlight.RICH_STYLES` and `tui.py` as RGB; Rich's names
  (`green`, `red`, `yellow`, `dim`) are mapped to fixed RGB values (`#50fa7b`, `#ff5555`, `#f1fa8c`), and the
  editor uses the same table as the log echo, not Textual's `dracula` TextArea theme. Weaker terminals get
  FTXUI's 256 / 16 colour approximation. Python paints a whole dark theme; the C++ workbench leaves the terminal's own
  background alone and is designed for dark terminals. For light ones: every tinted cell (zebra rows, the tree and
  result cursor rows, the editor selection, the header / footer band, the dialog fields) has an explicit light
  foreground, so it stays readable, and the focused panel is marked by an amber border (`#df7a00`, where Python uses
  yellow, which vanishes on white) plus an inverse title in the terminal's own colours. The pale Dracula pastels on
  untinted cells (strings, messages, `KHALI`) are weak on a white background; no terminal-background query is made.
- **Workbench: highlighting**: `highlight.cpp` is a hand-written scanner equivalent to `highlight.py`'s
  regex, checked line by line against Python (the golden corpus is every line of both example scripts plus edge
  lines; 20,000 random lines were also compared in review). Python's `\d` and `\b` are Unicode-aware; the
  scanner uses a generated table of Python's `\w` characters (Unicode 15.0) for the boundaries of a number, so
  `5€` or `5😀` are numbers as in Python. One difference remains, and one line is dropped from the golden
  corpus for it (`cpp/tests/gen_highlight_golden.py`, `DROP`: the line `5٣`): a non-ASCII decimal digit
  (Arabic-Indic and the like) counts as a digit in Python but only as a word character in the C++ scanner,
  so `5٣` is not coloured as a number.
- **Workbench: extra keys**: `Ctrl+P` / `Ctrl+N` (history, because not every terminal delivers Ctrl+Up /
  Ctrl+Down), `Ctrl+A` (select all), PageUp / PageDown, and the mouse subset (click focuses a panel, the wheel
  scrolls; a click in the editor only focuses it and does not place the cursor). Listed in the help.
- **Workbench: paste and Tab**: in Python's workbench a typed Tab moves the focus (Textual's `TextArea` default)
  while a pasted Tab is inserted, because Textual receives pastes as one event. FTXUI 5 has no paste event, so
  the workbench switches on the terminal's bracketed-paste mode (`ESC[?2004h`) and treats what arrives between
  `ESC[200~` and `ESC[201~` as pasted: a Tab there is inserted into the editor (the editor stores a Tab as four
  spaces, Python keeps the tab character) and never moves the focus. A terminal without bracketed paste
  (the classic Windows console) delivers a pasted Tab as a typed one, which moves the focus.
- **Workbench: characters outside the BMP (emoji) on Windows**: the Windows console hands over a typed or pasted
  character as UTF-16 units, one key event each, and a pseudo console (Windows Terminal, VS Code) sends an emoji as
  an Alt+numpad key sequence whose two halves ride on the key-up of Alt. FTXUI 5.0 reads only key-downs and converts
  every unit on its own, so it dropped the emoji. At configure time (Windows only) `cmake/patch_ftxui.cmake` edits the
  downloaded FTXUI source: its input loop passes the units through `ftxui_patch::SurrogateJoiner`
  (`cpp/ftxui_patch/ftxui_win_input.h`, unit-tested in `test_wb_input.cpp`), which joins a high and a low half into
  one UTF-8 character and drops an orphan half. The edit is idempotent and fails the configure if the FTXUI source is
  not the expected v5.0.0 (also for a copy given with `FETCHCONTENT_SOURCE_DIR_FTXUI`, which is edited in place).
  Showing emoji from the database never needed it. POSIX terminals send UTF-8 and need nothing.
- **Workbench: CSV failure and cell newlines**: a CSV that cannot be written is logged in red
  (`CSV save nahi hua: <reason>`; Python would crash with a traceback). A `\n` or `\r` inside a cell is shown as
  `↵` and a tab as a space, so a table row stays one line; the CSV keeps the raw text.
- **Workbench: log cap**: the log keeps at most 20,000 lines (oldest dropped); Python's `RichLog` is
  unbounded. A single log entry larger than the cap is kept whole (older entries go, the newest is never
  truncated); Python has no such case.
- **Workbench: tree cursor kept; local-to-local connect**: after a refresh the selected row stays on the
  same node (Python resets to the top after every statement). Connecting to Local mode while the current
  backend is also a local engine builds the new engine first and closes the old one after; and a local
  connect is refused while a local transaction is open (red `Connect nahi hua: ek transaction khula hai --
  pehle PAKKA ya WAPAS karo, phir Local mode`, nothing changes), because a second engine on the same folder
  runs crash recovery, which would undo the first one's open transaction. Python opens the second engine
  regardless.
- **Workbench: Ctrl+C never quits; Ctrl+Q waits for a running statement**: Python (Textual) quits on Ctrl+Q
  at once and only hints at it for Ctrl+C. Here a running statement is never abandoned half-way, queued
  statements are dropped, an open transaction is rolled back before the screen closes, and Ctrl+C is turned
  into a log hint by switching off the terminal's signal handling for the run. A hard kill, a closed window or
  Ctrl+Break are left to crash recovery at the next start, as with Python.
- **Workbench: statements run on a worker thread**: keys pressed while a statement runs are queued (Python
  blocks its screen instead, with the same effect on ordering); a statement's echo line appears when it starts,
  with the database current at that moment; F6 parses on the worker, so its history entry and echo appear when
  the job completes; the connect dialog's port is read like Python's `int()`, with the same
  `invalid literal for int() with base 10: '...'` text for a non-number.
- **Workbench: needs a terminal; no dependency message**: with stdin or stdout not a terminal it prints one
  line and exits 1; FTXUI is linked in, so the "install Textual" message and the automatic Python fallback do
  not exist. The window needs at least 60 x 24 cells.
- **Workbench: known limits, left as they are**: every key press redraws the whole screen (about 7 KB of output per
  key at 120 x 40, without synchronized-update markers); it is instant on a local terminal but could flicker over a
  slow SSH or classic-console link. At the minimum size the log (10 rows) and the editor (9 rows) are fixed, as in
  Python, so at 60 x 24 or 80 x 24 the Results panel shows the header and about one row: use a taller window
  (about 40 rows) to see results comfortably.
- **Platform coverage**: only the MinGW (Windows) build has been compiled and run so far. The POSIX
  socket and process code paths were written and reviewed but not yet built, and the MSVC build, including
  the depth-32 recursion check on MSVC's smaller default stack, is still to be verified. The terminal
  primitives of the shell (`ReadConsoleW`, the console control handler, `sigaction` / `pselect`,
  `read` on stdin) have been compiled on MinGW only (the POSIX branch not even that), and none of them has
  been run against a real terminal by a test. The workbench (FTXUI on MinGW) was also driven once through a
  Windows pseudo-console by hand, but `sys::TerminalModeGuard` on POSIX and FTXUI on MSVC, Linux and macOS are
  unbuilt, and the workbench's keys and colours have not been checked in a real terminal window by a person.

## Python behaviours mirrored on purpose

These look like bugs in the reference engine. They are reproduced so that output and
on-disk data stay identical, and are worth fixing in both engines together.

- `ALTER ... ADD COLUMN` and `ALTER ... DROP COLUMN` rebuild the schema from name
  and columns only, silently dropping composite `ANOKHA` / composite `MUKHYA KUNJI`
  constraints from the catalog.
- `RENAME COLUMN` does not rename the column inside composite constraint lists,
  leaving stale names.
- `BANAO VIEW` runs its `DIKHAO` once at creation time as a sanity check.
- `Connection.execute` wraps the server's already-tagged error text in a second
  `MeraDBError`, so the `[Stage Galti]` tag stays in the message.
- A user login supersedes the shared server password; a `hello` with a database goes through
  `UseDatabase`, so a restricted user is refused ("superuser nahi hai").
- An unterminated last line on the wire counts as a message; the size limit counts the newline.
- Triggers on a table are not dropped or renamed with it; a `NAYA`/`PURANA` reference is
  substituted into `INSERT`, `UPDATE`, `DELETE`, `SELECT` and `CHALAO` only (subquery bodies are
  left as written).

## Manual terminal checklist

The terminal primitives cannot run under `ctest`. Run these once on real terminals and note the outcome in
the pull request, not in the repository. Build `cpp/build`, then start the two shells side by side, each
on its own empty data folder, and compare what they do:

```
cpp/build/meradb_cli shell --local -D <empty folder 1>
python -m meradb shell --local -D <empty folder 2>
```

Windows: use `cpp\build\meradb_cli.exe` (`$env:NO_COLOR=1` in PowerShell, `set NO_COLOR=1` in `cmd`). Linux and
macOS: the same commands in any terminal; `export NO_COLOR=1`.

| # | Terminal | Check | Expect |
|---|---|---|---|
| 1 | Windows Terminal (PowerShell) | Banner | Cyan `MERA`, magenta `DB`, dim wordmark, bold tagline; the logo is revealed line by line; same as Python |
| 2 | Windows Terminal | Type `BANAO TABLE t (id INT);` then Enter; type a statement over three lines | Prompts `meradb:main> ` then `      ...> `; results green / red / tables with dim borders |
| 3 | Windows Terminal | Editing: arrow keys, Home / End, insert, Backspace, F7 | Same as in Python's shell |
| 4 | Windows Terminal | Type `DAALO MEIN t MAAN ('é😀नमस्ते');` (accented letter, emoji, Devanagari) then `DIKHAO * SE t;` | The text is typed and printed intact; Backspace removes a whole emoji |
| 5 | Windows Terminal | Ctrl+Z then Enter at an empty prompt | `Phir milenge!` (after a newline), prompt returns, `$LASTEXITCODE` is 0 |
| 6 | Windows Terminal | Ctrl+C at an empty prompt, and in the middle of a continuation line | Newline, `Phir milenge!`, exit code 0; no stray `^C` text that Python does not also leave |
| 7 | Windows Terminal | Run a slow statement (see below) and press Ctrl+C during it | The statement finishes, the shell exits, exit code 130, nothing printed after the result |
| 8 | Classic console (`conhost`, started with `WT_SESSION` and `TERM` unset) | Banner | Colours appear (virtual-terminal mode was switched on); after exit, later commands in that window print normally |
| 9 | Any | Set `NO_COLOR=1`, then start the shell | Plain text, no reveal pause |
| 10 | Any | `meradb_cli shell < script.txt`, `meradb_cli shell \| more`, `meradb_cli shell > out.txt` | No colour, no animation, identical to Python's piped output; a script containing Ctrl+Z is not cut short |
| 11 | Linux / macOS terminal | Banner, a statement, Ctrl+D at an empty prompt, Ctrl+D after typing some text, Ctrl+C at a prompt, Ctrl+C during a slow statement | Same as the Windows rows; compare the Ctrl+D-after-text case with Python's and note any difference |
| 12 | Any | `meradb_cli shell -W` | The password prompt hides what is typed (console); wrong password: error on stderr, exit 1, no banner |
| 13 | Windows Terminal and classic console | Paste one line longer than 512 UTF-16 units (for example `DIKHAO '` + 600 letters + `';`) | The shell reads the whole line intact (the console read arrives in 512-unit pieces); the echoed result holds all 600 letters |
| 14 | Windows Terminal and classic console | Paste a line in which an emoji (a surrogate pair) straddles the 512-unit boundary: 511 ASCII letters inside a string literal, then the emoji, then the closing `';` | The shell reads the whole line intact; the emoji comes back whole, not as two replacement characters |
| 15 | Windows Terminal and classic console | Start `meradb_cli shell` against a server (not `--local`), run a slow statement and press Ctrl+C while it runs; repeat with `--local` | The result is printed in full, the connection is NOT reported as dropped (no `Server se connection toot gaya`), the shell exits with code 130. Ctrl+C may only wake the console read, never a statement's socket or file write |
| 16 | Any | Ctrl+C just before a read: hold Ctrl+C while pressing Enter on a statement, and press Ctrl+C right as a slow statement ends; repeat a few times | Never a hang at the next prompt. A statement already entered runs to the end, then the shell exits 130 without reading another line; a Ctrl+C that reached the prompt before any text exits 0 with `Phir milenge!`. A typed, finished line is never silently discarded |

Rows 6, 7, 15 and 16 (Ctrl+C) will not react if the shell was started from a launcher that disabled Ctrl+C
for its children (some IDE run buttons and task runners do): start it from a normal terminal window.

A slow statement: create two tables of 3,000 rows each and run a cross join of them
(`DIKHAO GINO(*) SE a MILAO b PAR 1 = 1;`), or any statement that takes a few seconds.

### Workbench

Build `cpp/build`, then start the C++ and Python workbenches side by side on separate empty data folders:

```
cpp/build/meradb_cli workbench --local -D <empty folder 1>
python -m meradb workbench --local -D <empty folder 2>
```

**Key probe (do this first, once per terminal).** `cpp/build/tests/wb_keyprobe` shows the bytes FTXUI
receives for each key. Run it, press F1 F5 F6, Ctrl+Up / Ctrl+Down, Shift+Left / Right / Up / Down / Home /
End, Ctrl+Left / Right, Ctrl+S / R / O / L / Q / P / N / A / C, Esc, Tab, Shift+Tab and PageUp / PageDown,
and press `x` to leave. Then run `wb_keyprobe --guard` (the workbench's terminal-mode guard): Ctrl+C, Ctrl+S,
Ctrl+Q, Ctrl+O and Ctrl+R must show up as bytes `03 13 11 0F 12` and must neither end nor freeze the probe.
Compare what arrives with the tables in `cpp/include/meradb/wb_keys.h`; if a terminal sends something else
for a key (for example Shift+arrows), add its bytes to that table (nothing else in the workbench knows byte
sequences) and note it in the pull request.

| # | Terminal | Check | Expect |
|---|---|---|---|
| W1 | Windows Terminal | `meradb_cli workbench --local -D <empty folder>` next to `python -m meradb workbench --local -D <other empty folder>` | Same layout, titles, footer labels and start-up log lines; colours close to the Python screen |
| W2 | Windows Terminal | Resize the window: larger, smaller than 60 x 24, back | Layout follows; the too-small message appears and disappears; no leftover characters |
| W3 | Windows Terminal | Type a two-line statement, `F5`; `Ctrl+R`; select one line with Shift+arrows and `F5`; `Ctrl+A`, `F6` | Results, log and highlighting as in Python; only the selection runs |
| W4 | Windows Terminal, classic console | `Ctrl+Up` / `Ctrl+Down`, `Ctrl+P` / `Ctrl+N`, `F1`, `Esc`, `Tab` / `Shift+Tab`, `PageUp/PageDown`, `Ctrl+S`, `Ctrl+O`, `Ctrl+L` | Every key does what the help screen says; note any key a terminal does not deliver (use `wb_keyprobe`) |
| W5 | Windows Terminal | Type `'é😀नमस्ते日本'` in a string, INSERT, SELECT | Text intact in the editor, log and table; columns stay aligned; Backspace removes a whole emoji |
| W6 | Any | Ctrl+C in the editor; then `SHURU;`, an INSERT, Ctrl+C again | A yellow hint each time; nothing quits; the transaction is still open (header marker) |
| W7 | Any | `SHURU;`, an INSERT, Ctrl+Q; restart the workbench; `DIKHAO * SE t;` | Exit code 0, terminal restored; the row is not there (rolled back); no `RECOVERY` note |
| W8 | Any | A slow statement (cross join of two 3,000-row tables, see above): while it runs resize, press `F1`, `Ctrl+L`, queue a second `F5`; then press `Ctrl+Q` | The screen stays responsive, the busy label shows `[chal raha hai +1]`; Ctrl+Q shows `[band ho raha hai ...]`, waits, then exits cleanly; the queued second statement does not run |
| W9 | Windows Terminal | Close the window with the X during a transaction; start again | Same as Python: a `RECOVERY: ...` note on stderr at the next start and the transaction rolled back |
| W10 | Classic console (`conhost`, `WT_SESSION` and `TERM` unset) | Start, use it, quit | Colours and keys work (F-keys, arrows); after exit later commands print normally and the code page / console modes are as before |
| W11 | Linux (xterm / gnome-terminal / tmux) | Start, `Ctrl+S` in the editor, `Ctrl+Q`; afterwards `stty -a` | Ctrl+S does not freeze the terminal; Ctrl+Q quits; `isig` and `ixon` are back on afterwards |
| W12 | macOS Terminal.app and iTerm2 | Same keys; `F5` / `F6` may need "Use function keys" settings; `Ctrl+O` | Note which terminals send what; the aliases (Ctrl+R, Ctrl+P / N) work everywhere |
| W13 | Any | `Ctrl+O` -> Connect to a running C++ server and to a Python server; wrong port; wrong password; `Local mode` | Header shows `host:port`; failures are a red `Connect nahi hua: ...` and the old connection stays; statements work in all combinations |
| W14 | Any | Stop the server (`meradb stop`) while connected, run a statement, then `Ctrl+O` -> Connect again | A red error line and `Ctrl+O se dobara connect karo.`; the UI stays alive; reconnect works |
| W15 | Any | A table with 20,000 rows: `DIKHAO * SE big;`, scroll with arrows / PageDown / End, Right / Left | Smooth; memory reasonable; the title shows `Results -- 20000 row(s)` |
| W16 | Any | Paste 5,000 characters over 100 lines into the editor | Responsive; all lines present; a Tab inside the pasted text is inserted (as four spaces) on terminals with bracketed paste and does not move the focus; a typed Tab still moves the focus, as in Python |
| W17 | Any | Mouse: click each panel, wheel over the log and results; drag over text with and without Shift | The clicked panel gets the heavy amber border and inverse title; the wheel scrolls; the terminal's own selection works only with Shift held (mouse tracking is on) |
| W18 | Any | `meradb_cli workbench < /dev/null`, `meradb_cli workbench \| cat` | One line on stderr (`Workbench ke liye terminal chahiye ...`), exit code 1, nothing else |
| W19 | Any | `TERM=dumb meradb_cli workbench`, `NO_COLOR=1` | Not supported / ignored: note what happens (FTXUI decides); the program must not corrupt the terminal |
| W20 | Any, with a light colour scheme (a white terminal background) | Start, run a `DIKHAO` with 4+ rows, move the results cursor, select editor text with Shift+arrows, Tab through the panels | Zebra rows, cursor rows and the selection are readable (light text on the dark tint); the focused panel's amber border and inverse title are easy to see |
| W21 | Any | `Ctrl+O`, type `47831` straight away (Host is selected, so it replaces `127.0.0.1`); `Tab` to Port and type `47831`; `Shift+Tab` back and press Right | Each field's text is drawn inverse when it gets the focus and is replaced by what you type; Port shows `47831`, not `637247831`; Right ends the selection |

Results of these go in the pull request, not in the repository.

## Next phases

- **Phase 3, shell**: done (see "The shell"). Pieces the later phases reuse, rather than copy:
  `term::Style` + `term::detectStyle` (the colour decision), `sys::AnsiConsole` / `sys::Utf8Console`
  (console set-up), `formatResult(result, style)` (table output), `repl::runText` / `runFile` (run text on a
  `Backend` and print results, including the dropped-connection handling), `repl_text.h` (`helpReference()`,
  `renderReference`, `fullHelp`, the prompt and banner text), `pytext` (Python's string semantics, incl.
  `lower`), `openBackend` (the shell's connection logic), `Backend` (`LocalBackend` / `Connection`),
  `protocol::kProgramVersion`. History was deliberately left out of the shell (Python has none); if the
  workbench wants a query history it is new in both programs and belongs to the workbench alone.
- **Phase 4, workbench**: done (see "The workbench"). Phase 5 should reuse or check: the key probe
  (`cpp/build/tests/wb_keyprobe`, with `--guard`) on every terminal, because the Shift+arrow and Ctrl+arrow
  byte sequences are the part of the workbench that only a real terminal can confirm (the table in
  `wb_keys.h` is the one place to change); the generators `gen_highlight_golden.py` and `gen_word_table.py`;
  and the Pilot comparison (`workbench_diff.py`: add a scenario to `workbench_scenarios.json` for any new
  behaviour). Verify FTXUI v5.0.0 and `sys::TerminalModeGuard` on Linux, macOS and MSVC (unbuilt so far), and
  fill in the findings of the manual checklist W1-W19 (the Windows-console results especially: classic
  console key delivery, the code page after exit). Do not use `ConsoleLineSource` or `sys::InterruptGuard`
  inside the full-screen UI: FTXUI owns the terminal and its keys.
- **Phase 5, polish**: `docs/REPORT.md`; the final test and documentation pass; grow the divergence list
  above; consider Unicode identifiers (ICU or a small generated table of letter ranges) if full parity is
  wanted, which would also let the tokenizer escape every unprintable character like Python's `repr()`.
  Verify the POSIX (Linux, macOS) and MSVC builds first thing and fix warnings (the code follows the
  portability rules but those toolchains have not been run yet), including a Debug build under MSVC to
  check the stack and depth guards on its smaller stack. The terminal primitives in `sys_compat`
  (`readTerminalLine`, `InterruptGuard` / `InterruptGate`, `AnsiConsole`) are the part most in need of a
  real run on Linux, macOS and a Visual Studio build; an optional pseudo-terminal test (Python's `pty`
  module driving both shells) is the natural next check on POSIX.
