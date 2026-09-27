# MeraDB C++ Port — Design Spec

**Status:** Approved for planning
**Date:** 2026-09-27
**Deadline context:** Submission is in November; this gives roughly 5-6 weeks,
enough for a genuine full-parity port rather than a rushed cut-down one.

## Goal

Reimplement MeraDB — currently a pure-Python relational database engine with
a Hinglish query language — in C++, with **full feature parity**: every
language feature, the client/server architecture, the interactive shell, and
the full-screen workbench TUI must all work in the C++ version exactly as
they do today. Nothing that currently works may regress.

The existing Python implementation (`meradb/`) is **not removed or
modified**. It stays in the repo as the working reference implementation —
both for cross-checking C++ behavior during the port, and as a fallback if
any C++ phase runs long.

## Non-goals

- No new language features. This is a port, not a redesign of the Hinglish
  grammar or engine semantics.
- No performance rearchitecture (e.g. bytecode VM, compiled query plans).
  Tree-walking interpretation, same as the Python version, is the target —
  it's what the Python engine does and it's the pattern a DBMS mini-project
  is expected to demonstrate and defend in a viva.
- No attempt to keep the C++ version dependency-free at the cost of large
  amounts of throwaway plumbing code (e.g. hand-rolled JSON parsing). Small,
  well-known header-only libraries are acceptable (see Dependencies below).

## Dependencies

Policy: **minimal, header-only/FetchContent-able libraries where hand-rolling
would burn days without teaching anything the project is graded on.**

| Need | Library | Why |
|---|---|---|
| JSON (catalog files, wire protocol) | [nlohmann/json](https://github.com/nlohmann/json) | Single header, ubiquitous, matches the Python version's use of `json` |
| SHA-256 (for password hashing) | [picosha2](https://github.com/okdshin/PicoSHA2) | Single header; HMAC and PBKDF2 are hand-written on top of it (a few dozen lines — this is the actual "security" logic worth writing ourselves, unlike a hash primitive) |
| Full-screen TUI (workbench) | [FTXUI](https://github.com/ArthurSonzogni/FTXUI) | Small, cross-platform (Windows/Linux/Mac), CMake `FetchContent`-friendly, closest analog to what Textual provides in Python |
| Unit tests | [Catch2](https://github.com/catchorg/Catch2) | Single-header v2, or FetchContent v3; standard for C++ test suites |

Everything else — tokenizing, parsing, planning, evaluation, storage,
sockets, threading — is hand-written using the C++ standard library only.
Sockets use raw Winsock2 (Windows) / BSD sockets (POSIX) behind a small
portability shim (`net_compat.h`), not Boost.Asio — this mirrors the
Python version's direct use of the `socket` module rather than an async
framework.

## Architecture

Same layered design as the Python version (see `docs/ARCHITECTURE.md`),
translated layer-for-layer:

```
Interfaces:  shell (repl)        workbench TUI (FTXUI)      client lib
Networking:  server  <->  wire protocol (JSON lines)  <->  client
Engine:      engine (executes AST, transactions, triggers, privileges)
Query pipeline:  tokenizer -> parser -> AST -> planner -> evaluator -> aggregates
Catalog & storage:  catalog (schema)   table (rows+indexes)   storage (disk I/O)
Supporting:  datatypes (Value variant)   users (auth/privileges)   errors
```

### Key technical decisions

**AST nodes.** Python's `dataclasses` become an abstract-base-class
hierarchy: `Statement` and `Expr` base classes with virtual `accept()`
methods (visitor pattern), concrete subclasses per node type (`Select`,
`Insert`, `CreateTable`, `Join`, `SetOp`, `CreateTrigger`, ...). AST
ownership is `std::unique_ptr` throughout — no raw `new`/`delete`, no
manual memory management bugs.

**Values.** Python's dynamically-typed row values become a `Value` class
wrapping `std::variant<std::monostate, int64_t, double, std::string,
bool>`, with helpers mirroring `datatypes.py`'s coercion rules (`ANK`,
`DASHAMLAV`, `VAKYA`, `BOOLEAN`, `TARIKH`, `KHALI`/NULL).

**Catalog & storage.** Same on-disk shape as today: JSON catalog files via
nlohmann/json, same atomic-write-then-rename pattern as `storage.py`, same
crash-recovery approach. Row data stored the same way (JSON per table) —
this is a mini-project, not a production storage engine, and keeping the
on-disk format conceptually identical to the Python version makes the port
verifiable file-for-file.

**Networking.** One `std::thread` per connection, exactly mirroring
`socketserver.ThreadingTCPServer`'s "one thread per connection, one shared
`Instance` (lock + catalogs)" model. A `net_compat.h` header isolates the
`WSAStartup`/Winsock vs POSIX socket differences so the rest of the code is
platform-neutral.

**Users/privileges.** PBKDF2-HMAC-SHA256 password hashing, same as Python's
`hashlib.pbkdf2_hmac`, built from picosha2 + hand-written HMAC/PBKDF2 —
salted, 100,000 iterations, same `users.json` shape.

**Triggers/procedures.** Same substitution-based execution model as
`engine.py`'s `_substitute()` helper: NEW/OLD (`NAYA`/`PURANA`) and
procedure parameters are resolved by rewriting a cloned AST subtree with
literal values before execution, not by a general expression-interpreter
environment. Same simplifications carried over honestly (no loops, no
`RETURN`, no raw `RAISE` — the divide-by-zero veto trick still applies).

## Repo structure

```
meradb/                    # existing Python implementation — untouched
cpp/
  CMakeLists.txt
  include/meradb/
    tokenizer.h  parser.h  ast.h  planner.h  evaluator.h  aggregates.h
    datatypes.h  catalog.h  table.h  storage.h  users.h  errors.h
    protocol.h  server.h  client.h  net_compat.h
  src/
    tokenizer.cpp  parser.cpp  ast.cpp  planner.cpp  evaluator.cpp
    aggregates.cpp  datatypes.cpp  catalog.cpp  table.cpp  storage.cpp
    users.cpp  engine.cpp  protocol.cpp  server.cpp  client.cpp
    repl.cpp  tui.cpp  cli.cpp  main.cpp
  tests/
    test_tokenizer.cpp  test_parser.cpp  test_engine.cpp  ...
    (mirrors tests/test_*.py file-for-file where it makes sense)
  third_party/            # FetchContent-managed: nlohmann_json, ftxui, catch2, picosha2
docs/
  CPP.md                  # new: build/run instructions for the C++ version
```

## Phased delivery

The port is too large for a single implementation plan; it is decomposed
into five phases, each independently plannable, executable, and testable.
Nothing later blocks something earlier from already being demoable.

1. **Core engine.** tokenizer, parser, AST, planner, evaluator, aggregates,
   datatypes, catalog, table, storage, errors — driven by a local-mode CLI
   (`meradb_cpp run <file>`, no networking). Success criterion: every
   `.mdb` example script (`examples/demo.mdb`,
   `examples/rdbms_lab_coverage.mdb`) produces output equivalent to the
   Python engine.
2. **Server + client + protocol.** TCP server (thread-per-connection),
   wire protocol, users/privileges, triggers, stored procedures,
   cross-session transactions.
3. **Interactive shell.** Banner (block logo + wordmark + reveal animation),
   ANSI color detection, `.help` reference, dot-commands — feature-matched
   to `repl.py`.
4. **Workbench TUI.** FTXUI-based full-screen client matching `tui.py`'s
   panels (schema tree, query editor, results table, syntax highlighting).
5. **Tests, docs, polish.** Catch2 suite covering the same scenarios as the
   266 Python tests; `docs/CPP.md` build/run guide; README updated to
   describe both implementations; `docs/REPORT.md` updated to reflect the
   C++ version's architecture for submission.

Each phase gets its own plan document (via the writing-plans skill) and is
executed and verified before the next phase's plan is written — so scope
discovered mid-phase doesn't cascade into an already-written later plan.

## Testing / verification strategy

For each phase, correctness is checked two ways:
1. **Catch2 unit tests** per layer (tokenizer, parser, evaluator, etc.),
   covering the same edge cases as the corresponding Python test file.
2. **Cross-engine diffing**: run the same `.mdb` script through both the
   Python engine and the C++ engine and diff output. This is the strongest
   available correctness signal, since the Python engine is an
   already-verified oracle for expected behavior.

## Error handling

Same one-exception-per-layer shape as `errors.py`: a `MeraDBError` base
(with a `stage` tag and a `message()` accessor separate from `what()`, to
avoid the double-prefix bug the Python version had and fixed), with
`TokenizerError`, `ParseError`, `ExecutionError`, `StorageError`,
`ConnectionFailed` subclasses.

## Build / toolchain

CMake, C++17 minimum (using `std::variant`, `std::optional`,
structured bindings). Buildable with either MSVC or MinGW-w64/g++ on
Windows, and portable to Linux/Mac. `third_party/` libraries pulled via
CMake `FetchContent` so a fresh clone + `cmake --build` is the entire setup
required — no manual dependency installation.

## Risks / open items

- **FTXUI learning curve** (Phase 4) is the least-certain estimate — it's a
  different paradigm from Textual's reactive widget model. If it runs long,
  the fallback (already agreed) is that Phases 1-3 alone still constitute a
  complete, gradable C++ database engine with a working shell; the TUI can
  slip without blocking submission.
- **Cross-engine diffing** assumes the Python engine's current behavior is
  the correct oracle for every feature — reasonable, since it's the
  already-tested, already-graded-ready implementation.
