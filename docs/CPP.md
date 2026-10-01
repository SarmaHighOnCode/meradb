# MeraDB in C++ (Phases 1-2: engine, server, client)

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

Not yet (Phases 3-5): the interactive shell (`meradb shell`), the workbench, and the polish
pass. `shell` and `workbench` print a note and exit 1.

## Prerequisites

- CMake 3.20 or newer
- A C++17 compiler: MinGW-w64 g++, MSVC (Visual Studio 2019 or newer), or g++/clang
  on Linux and macOS
- Network access on the first configure: nlohmann/json and Catch2 are downloaded
  automatically by CMake (`FetchContent`). Nothing else needs installing.

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
```

`meradb_cli x.mdb` means `run x.mdb`. `--data` is accepted as a long form of `-D`. `run` connects
to a server if one answers (the port of the data folder's `meradb.pid`, `MERADB_HOST` /
`MERADB_PORT`, else 127.0.0.1:6372) and otherwise opens the data folder directly ("LOCAL
mode", with a note on stderr); an explicit `-H` / `-p` never falls back. Output matches
`meradb run`: each statement's result followed by a blank line; a failing statement does not
stop the script; exit code 1 if any statement failed. Environment: `MERADB_DATA` (data
folder), `MERADB_PORT`, `MERADB_HOST`, `MERADB_PASSWORD` (the server-wide password),
`MERADB_USER`. The data folder defaults to the same per-user folder as Python
(`%LOCALAPPDATA%\MeraDB\data`, or `~/.local/share/MeraDB/data`).

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
```

`cross_engine_diff.py` runs a script through the Python engine and the C++ CLI on fresh data
folders and diffs the output and exit code (`--via-server` sends every statement through a C++
server). The harness looks for the CLI in `cpp/build` and `cpp/build/Release`; otherwise pass
`--cli`. `demo_phase1.mdb` and `rdbms_lab_coverage_phase1.mdb` are the Phase 1 copies of the
examples without the users / trigger / procedure sections; they are still compared.

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
| `cli.py` (server, start, stop, status, run) | `cli.h` / `cli.cpp`, `server_control.h` / `server_control.cpp`, `main.cpp` |
| `repl.py` (table output)            | `cli_format.h` / `cli_format.cpp`                 |

Supporting files with no Python counterpart: `pyvalue.h` / `pyvalue.cpp`
(Python-compatible equality and hashing of values) and `ordered_map.h`.

Also without a Python counterpart: `net_compat.h` / `net_compat.cpp` (Winsock / BSD sockets)
and `sys_compat.h` / `sys_compat.cpp` (environment, time, random bytes, process spawning):
the only places that include platform headers.

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
  tree copies, JSON decoding) measures the stack bytes used since the entry point and refuses
  past 512 KB (`Query bahut gehri (nested) hai (stack limit 512 KB)`); that is what keeps a
  small thread stack (1 MB MSVC, 2 MB MinGW) safe, and it trips first for most shapes (about
  150 nested parentheses, 95 nested subqueries). The caps exist so that a network client
  cannot crash the server; ordinary scripts never come near them.
- **Trigger/procedure recursion cap**: a trigger that (directly or indirectly) fires itself
  endlessly makes Python raise `RecursionError`; C++ stops at 32 levels with
  `Trigger/procedure bahut gehra chal raha hai (limit 32) -- shayad koi trigger khud ko
  baar-baar chala raha hai`.
- **Non-ASCII identifiers**: Python accepts any Unicode letter in a table or column name;
  C++ accepts ASCII letters, digits and `_` only (Unicode letter classes need tables the
  project does not depend on). Non-ASCII text in string literals and data is fine.
  Unexpected non-ASCII characters in a query are shown whole, but Python's escaping of
  unprintable ones (`'\xa0'`) is not reproduced.
- **INT overflow family**: Python integers are unbounded, C++ `INT` values are 64-bit.
  Arithmetic that overflows 64 bits is reported as an error instead of producing a big number.
- **Command-line abbreviations**: Python's `argparse` accepts unambiguous abbreviations of long
  options (`--dat` for `--data`); the C++ command line requires the full option name.
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
- **Platform coverage**: only the MinGW (Windows) build has been compiled and run so far. The
  POSIX socket and process code paths were written and reviewed but not yet built, and the
  MSVC build, including the depth-32 recursion check on MSVC's smaller default stack, is still
  to be verified.

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

## Next phases

- **Phase 3, shell** (`repl.py`): reuse `Backend` (`LocalBackend` / `Connection`), `formatResult`
  and `runFile`; `sys::readHidden` exists for password prompts; needs a line editor (linenoise
  or replxx, vendored or FetchContent) with history; the banner/animation are pure output.
  `openBackend` already implements the shell's connection logic: call it, do not copy it.
  The shell must handle `Connection` errors mid-session (`ConnectionFailed`) like `repl.py`.
- **Phase 4, workbench**: `Backend::schemaTree()` already returns the JSON the sidebar needs
  (both local and remote); FTXUI panels; a query must run off the UI thread, but a
  transaction's statements must all run on ONE thread, so use a dedicated worker thread per
  session, never a pool.
- **Phase 5, polish**: `docs/REPORT.md`; grow the divergence list above; consider Unicode
  identifiers (ICU or a small generated table of letter ranges) if full parity is wanted;
  verify the POSIX and MSVC builds first thing and fix warnings (the code follows the
  portability rules but those toolchains have not been run yet).
