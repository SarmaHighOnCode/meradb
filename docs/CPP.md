# MeraDB in C++ (Phase 1: core engine)

`cpp/` contains a C++17 port of the MeraDB query engine. It mirrors the Python
implementation in `meradb/` layer for layer: the same Hinglish grammar, the same
error wording, the same result formatting, and the **same on-disk formats**. A data
folder written by one implementation can be opened by the other.

The Python implementation is unchanged and remains the reference.

## What Phase 1 covers

- Tokenizer, parser, syntax tree, planner, evaluator, aggregates, data types.
- Catalog (`catalog.json`), heap-file storage and hash indexes.
- The executor: DDL, DML (including `INSERT ... SELECT` and upsert), `DIKHAO` with
  joins, `SAMOOH`/`JINKA`, subqueries (correlated and not), set operations, views,
  `SAMJHAO` (EXPLAIN), and transactions (`SHURU`/`PAKKA`/`WAPAS`) with shadow-copy
  snapshots and crash recovery.
- A local-mode script runner, `meradb_cli`.

Not in Phase 1 (later phases): the TCP server and client, the interactive shell and
workbench, users and privileges, triggers and stored procedures. Those statements
produce a parse error in the C++ engine.

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

## Run a script

```
cpp/build/meradb_cli run <script.mdb> [more.mdb ...] [--data <dir>]
```

Output matches `meradb run --local`: each statement's result (an ASCII table for
rows, a message, or the `[Stage Galti] ...` error) followed by a blank line. A
failing statement does not stop the script. The exit code is `1` if any statement
failed (the example scripts contain deliberate errors), otherwise `0`. `--data`
defaults to the `MERADB_DATA` environment variable if set, otherwise `data` in the current directory.

## Cross-engine verification

```
python cpp/tests/cross_engine_diff.py cpp/tests/demo_phase1.mdb
python cpp/tests/cross_engine_diff.py cpp/tests/rdbms_lab_coverage_phase1.mdb
```

The script runs a `.mdb` file through the Python engine and the C++ CLI, each on a
fresh data folder, and diffs the output and exit code. Both are also registered as ctest tests when Python is found at configure time. The harness looks for the CLI in `cpp/build` and `cpp/build/Release`; for any other build folder pass `--cli <path-to-meradb_cli>`. Both files currently match
line for line. `demo_phase1.mdb` and `rdbms_lab_coverage_phase1.mdb` are copies of
`examples/demo.mdb` and `examples/rdbms_lab_coverage.mdb` with the users and
privileges, trigger and stored-procedure sections removed (those need Phase 2).

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
| `cli.py`, `repl.py` (run, tables)   | `cli_format.h` / `cli_format.cpp`, `main.cpp`     |

Supporting files with no Python counterpart: `pyvalue.h` / `pyvalue.cpp`
(Python-compatible equality and hashing of values) and `ordered_map.h`.

Design notes: syntax-tree nodes are owned by `std::unique_ptr` (no raw `new` or
`delete`); values are a `Value` class over `std::variant`; errors are C++
exceptions carrying the same `[Stage Galti] message` text as Python, with a
`stage()` accessor and a bare `message()` accessor.

## Known divergences from the Python engine

These are deliberate and small.

- **Default data folder**: without `--data`, C++ uses `data` in the current directory; Python uses a per-user folder (`%LOCALAPPDATA%\MeraDB\data` on Windows, `~/.local/share/MeraDB/data` elsewhere). Both honour `MERADB_DATA`.
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

## Python behaviours mirrored on purpose

These look like bugs in the reference engine. They are reproduced so that output and
on-disk data stay identical, and are worth fixing in both engines together.

- `ALTER ... ADD COLUMN` and `ALTER ... DROP COLUMN` rebuild the schema from name
  and columns only, silently dropping composite `ANOKHA` / composite `MUKHYA KUNJI`
  constraints from the catalog.
- `RENAME COLUMN` does not rename the column inside composite constraint lists,
  leaving stale names.
- `BANAO VIEW` runs its `DIKHAO` once at creation time as a sanity check.
