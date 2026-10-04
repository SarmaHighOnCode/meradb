# MeraDB: A Relational Database Engine with a Hinglish Query Language

**Mini Project Report**

| | |
|---|---|
| Student | `<Your name>` (`<Roll number>`) |
| Course / Semester | `<Course, semester>` |
| College | `<College name>` |
| Guide | `<Guide name>` |
| Date | `<Submission date>` |

> **Draft.** Replace everything in `<angle brackets>`, add screenshots where marked
> 📷, and rewrite any paragraph in your own words. You must be able to explain every
> sentence in the viva.

---

## Abstract

MeraDB is a relational database management system built from scratch in Python, without
any database libraries. Instead of SQL, it is queried in a **Hinglish** language
(`DIKHAO naam SE students JAHAN umar > 18`). This makes the structure of a query language
visible to learners and shows that SQL's keywords are a design choice, not a law.

The system includes:
- a tokenizer, a recursive-descent parser and a query planner
- an executor with SQL-correct NULL (three-valued) logic
- a custom binary storage format
- hash indexes and inner/left joins using hash-join and nested-loop strategies
- aggregates with grouping
- ACID transactions with crash recovery

MeraDB runs as a **client–server** system: a background server on TCP port 6372,
a command-line shell, and a full-screen "workbench" modelled on MySQL Workbench. It supports the
complete set of DDL and DML commands, plus FOREIGN KEY, CHECK, a DATE type, VARCHAR(n)/
NUMBER(p,s) length limits, RENAME and output column aliases, and the Python version is
verified by 183 automated tests.

The whole system was then ported to **C++**, which is the version submitted for the course
(Chapter 10). The C++ version reads and writes the same data files and speaks the same network
protocol, and it is checked against the Python version by differential testing, in addition to
about 780 automated tests.

## 1. Introduction

### 1.1 Problem statement
Students use databases every day, but a DBMS is usually a black box. The goal of this
project is to **build one from first principles**, including language, parser, storage,
indexing, transactions and networking, in order to understand how each layer works.

### 1.2 Objectives
1. Design a query language with Hinglish keywords and a formal grammar.
2. Implement DDL (CREATE/DROP/ALTER/TRUNCATE) and DML (INSERT/SELECT/UPDATE/DELETE).
3. Store data persistently in a self-designed binary file format.
4. Enforce constraints: PRIMARY KEY, NOT NULL, UNIQUE, DEFAULT and data types.
5. Support filtering, sorting, aggregation, grouping, DISTINCT and joins.
6. Speed up lookups with indexes and show query plans (EXPLAIN).
7. Provide ACID transactions with crash recovery.
8. Run as a server that several clients can use at once, with a Workbench-style client.

### 1.3 Why Hinglish?
Hinglish is how many Indian students think and speak. Translating SQL into it
(`SELECT → DIKHAO`, `WHERE → JAHAN`, `JOIN → MILAO`) forces the language to be
designed deliberately, and the result stays easy to read.

## 2. Features

| Category | MeraDB syntax | SQL equivalent |
|----------|---------------|----------------|
| Databases | `BANAO / HATAO DATABASE`, `ISTEMAL` | CREATE/DROP DATABASE, USE |
| Tables (DDL) | `BANAO TABLE`, `HATAO TABLE`, `SUDHARO TABLE ... JODO/HATAO`, `SAAF TABLE` | CREATE, DROP, ALTER ADD/DROP COLUMN, TRUNCATE |
| Rename | `SUDHARO TABLE t NAYA_NAAM t2`, `SUDHARO TABLE t COLUMN c NAYA_NAAM c2` | ALTER TABLE ... RENAME TO / RENAME COLUMN |
| Constraints | `MUKHYA KUNJI`, `ZAROORI`, `ANOKHA`, `WARNA`, `SANDARBH`, `SHART` | PRIMARY KEY, NOT NULL, UNIQUE, DEFAULT, FOREIGN KEY, CHECK |
| Insert | `DAALO MEIN t (cols) MAAN (...), (...)` | INSERT INTO ... VALUES |
| Query | `DIKHAO [ALAG] ... SE ... JAHAN ... SAMOOH ... JINKA ... KRAM ... SIRF` | SELECT [DISTINCT] ... FROM ... WHERE ... GROUP BY ... HAVING ... ORDER BY ... LIMIT |
| Output alias | `expr KAHO alias` | `expr AS alias` |
| Joins | `MILAO ... PAR`, `BAAYAN MILAO` | JOIN ... ON, LEFT JOIN |
| Predicates | `AUR YA NAHI`, `HAI KHALI`, `JAISA`, `BEECH ... AUR`, `MEIN (...)` | AND OR NOT, IS NULL, LIKE, BETWEEN, IN |
| Aggregates | `GINO KUL AUSAT NYUNTAM ADHIKTAM` | COUNT SUM AVG MIN MAX |
| Update / delete | `BADLO ... RAKHO ... JAHAN`, `MITAO SE ... JAHAN` | UPDATE ... SET, DELETE FROM |
| Transactions | `SHURU`, `PAKKA`, `WAPAS` | BEGIN, COMMIT, ROLLBACK |
| Tools | `SAMJHAO`, `SIKODO TABLE`, `DIKHAO TABLES`, `BATAO` | EXPLAIN, VACUUM, SHOW TABLES, DESCRIBE |
| Types | `INT/ANK`, `FLOAT/DASHAMLAV`, `TEXT/SHABD`, `BOOL/HAAN_NA`, `DATE/TAREEKH` | INTEGER, REAL, TEXT, BOOLEAN, DATE |
| Type lengths | `VARCHAR(n)`, `NUMBER(p,s)` | VARCHAR(n), NUMERIC(p,s) (precision/scale accepted, not enforced) |

## 3. System requirements

| | |
|---|---|
| Language | Python 3.10 or newer |
| Libraries (engine, server, shell) | None: only the Python standard library |
| Library (workbench UI only) | Textual 8.x |
| OS | Windows, Linux or macOS |
| Hardware | Any machine that runs Python; a few MB of disk |

These are the requirements of the Python version. The C++ version needs CMake 3.20 or newer and
a C++17 compiler instead (see section 10.3); Python is then needed only for the optional
cross-check tests.

## 4. System design

### 4.1 Architecture
📷 *Insert the architecture diagram from `docs/ARCHITECTURE.md` ("The big picture").*

A query passes through five layers:

1. **Tokenizer** turns characters into tokens.
2. **Parser** turns tokens into an abstract syntax tree (AST).
3. **Planner** resolves names and chooses index vs. scan and hash join vs. nested loop.
4. **Executor** runs the plan: filter, join, group, sort, project.
5. **Storage** holds the catalog (schemas) and heap files (rows).

The **server** wraps this engine and gives each network client its own session.

### 4.2 Language design
The grammar is written in EBNF (`docs/LANGUAGE.md`) and implemented as a
recursive-descent parser: one method per grammar rule. Operator precedence
(`YA` < `AUR` < `NAHI` < comparison < `+ -` < `* / %` < unary `-`) comes from the
nesting of those methods. `BEECH` and `MEIN` are *syntactic sugar*: the parser rewrites
them into comparisons that the evaluator already understands.

### 4.3 Storage format
Each table is a *heap file*: an 8-byte magic header `MERADB01`, followed by records
`[status:1][length:4][payload]`. Each value in the payload has a 1-byte NULL tag, then an
8-byte INT or FLOAT, a 1-byte BOOL, or a length-prefixed UTF-8 TEXT. A row's id is its
byte offset. DELETE writes a 1-byte *tombstone*, UPDATE is delete + append, and
`SIKODO` compacts the file. The schemas live in a `catalog.json` per database. All
whole-file rewrites use *write to a temp file, then atomic rename*.

### 4.4 Query processing
The SELECT pipeline is: index lookup or full scan → joins → WHERE → GROUP BY +
aggregates → HAVING → ORDER BY → projection → DISTINCT → LIMIT.

- **NULL** follows SQL three-valued logic. For example, `KHALI = KHALI` is unknown,
  so a `JAHAN` on it does not match.
- **Grouping** puts rows into buckets in a hash map. Each bucket collapses into one row
  that carries its aggregate results.

### 4.5 Indexing
Every PRIMARY KEY and UNIQUE column has an in-memory **hash index** (value → row id). It
is built lazily with one scan and kept in sync on every insert and delete. The planner
uses it for `column = constant` conditions. The same index makes uniqueness checks O(1).

### 4.6 Joins
For an equality join condition, MeraDB builds a **hash join**: it hashes the right table
once and then probes it for each left row, which costs O(n + m). Other conditions use a
**nested loop**, which costs O(n × m). LEFT JOIN pads rows that have no match with NULLs.

### 4.7 Transactions and recovery
MeraDB uses **shadow copies**:
- `SHURU` snapshots the database folder.
- `WAPAS` restores the snapshot.
- `PAKKA` discards it. An atomic rename is the commit point.

At startup, a leftover snapshot means a crash happened before commit, so it is restored.
This provides **atomicity** and **durability**.

### 4.8 Client–server and concurrency
The server runs one thread per connection. The protocol is newline-delimited JSON over
TCP, on port 6372.
- Each connection has its own session, with its own current database and transaction.
- All sessions share one lock, so statements run one at a time. This gives
  **serializable isolation**.
- A transaction holds the lock until it commits or rolls back.
- A disconnect rolls back any open transaction.
- A pid file stops two processes from opening the same data folder.

### 4.9 Constraints: SANDARBH (FOREIGN KEY) and SHART (CHECK)
`SANDARBH` requires its parent column to already be MUKHYA KUNJI or ANOKHA, so it always has
a hash index (§4.5); a child's FK value is then checked with one O(1) dict lookup, exactly
like a uniqueness check. The parent side is **RESTRICT**: deleting or changing a value that a
child still points at is refused (there is no CASCADE). A self-reference (e.g.
`employee.manager_id -> employee.id`) reuses the same code, and additionally accepts values
being inserted earlier in the *same* statement.

`SHART` cannot be stored as a parsed expression tree, because `catalog.json` is plain JSON.
Instead the catalog keeps the constraint's **source text** (the tokenizer now tracks each
token's character offset so the parser can slice it out), and `parser.parse_expression()`
(cached) turns it back into a tree whenever it must be validated or evaluated. Enforcement
reuses the existing `evaluate()` function unchanged and follows SQL's three-valued logic: a
row is rejected only if the check is exactly `JHOOTH` -- `KHALI` (unknown) is accepted, same
as `JAHAN`. Because the text can't be "patched", renaming a column a `SHART` uses is refused.

## 5. Implementation

| Module | Lines | Purpose |
|--------|-------|---------|
| tokenizer.py | 226 | Lexical analysis |
| parser.py + ast_nodes.py | 809 | Recursive-descent parser, syntax tree |
| planner.py | 212 | Binding, access paths, join strategy |
| engine.py | 1098 | Executor, sessions, transactions, recovery, constraints |
| evaluator.py + aggregates.py | 366 | Expressions, NULL logic, aggregates |
| storage.py + table.py + catalog.py + datatypes.py | 578 | Binary storage, indexes, schemas, types |
| server.py + client.py + protocol.py + cli.py | 811 | Networking and command-line tools |
| repl.py + tui.py + highlight.py | 676 | Shell and workbench |

In total, MeraDB is about 4,800 lines of Python, plus about 1,400 lines of tests.

## 6. Testing

The Python version of MeraDB has **183 automated tests** (`python -m unittest`). The C++
version has its own, larger suite, described in section 10.6:

| Test file | Tests | What it checks |
|-----------|-------|----------------|
| test_tokenizer.py | 9 | Keywords, numbers, strings, escapes, comments, error positions |
| test_parser.py | 10 | Every statement's AST, operator precedence, syntax errors |
| test_engine.py | 24 | Every DDL/DML command, constraints, NULL logic, persistence across restarts |
| test_features.py | 25 | JAISA, BEECH, MEIN, WARNA, SIKODO, aggregates, SAMOOH, JINKA, ALAG |
| test_week4.py | 27 | Joins, index use and correctness, transactions, crash recovery, EXPLAIN |
| test_server.py | 15 | Real TCP client/server, multiple sessions, isolation, passwords, `meradb start/stop`, a DATE round-trip |
| test_tui.py | 5 | The workbench, driven headlessly |
| test_constraints.py | 68 | SANDARBH, SHART, DATE, VARCHAR(n)/NUMBER(p,s), NAYA_NAAM, KAHO -- including error cases and persistence across a restart |

Sample test cases:

| # | Input | Expected | Result |
|---|-------|----------|--------|
| 1 | Insert a duplicate MUKHYA KUNJI | Error; table unchanged | Pass |
| 2 | `JAHAN umar < 100` where some umar is KHALI | Rows with KHALI are excluded | Pass |
| 3 | `BADLO s RAKHO id = id + 10` | Every row updated exactly once (Halloween problem) | Pass |
| 4 | `SHURU; MITAO SE t; WAPAS` | All rows are back | Pass |
| 5 | Process "crashes" mid-transaction, then restarts | Recovery restores the snapshot | Pass |
| 6 | Client A has a transaction open while client B reads | B waits and never sees uncommitted data | Pass |
| 7 | Client disconnects inside a transaction | Server rolls it back | Pass |
| 8 | `DIKHAO GINO(*) SE t` on an empty table | One row: 0 | Pass |
| 9 | `MITAO SE courses JAHAN id = 10` while a student still has `cid = 10` | Error (SANDARBH RESTRICT); row unchanged | Pass |
| 10 | `DAALO MEIN students MAAN (..., umar = -1)` with `SHART (umar >= 0)` | Error; row not inserted | Pass |
| 11 | `DAALO MEIN t MAAN (..., x = KHALI)` with `SHART (x > 0)` | Accepted (KHALI is unknown, not JHOOTH) | Pass |
| 12 | `SUDHARO TABLE t COLUMN x NAYA_NAAM y` where a SHART uses `x` | Error; column not renamed | Pass |

## 7. Results

📷 *Screenshot: `meradb start` and `meradb status` output.*
📷 *Screenshot: the workbench with a JOIN + GROUP BY result.*
📷 *Screenshot: a `SAMJHAO` query plan showing INDEX LOOKUP vs FULL SCAN.*

**Performance** (50,000-row table, `<your CPU>`; measured with `time.perf_counter`):

| Operation | Time |
|-----------|------|
| Insert 50,000 rows (10 statements × 5,000 rows) | 4.8 s |
| `JAHAN id = 49999` (hash index lookup) | **0.5 ms** |
| `JAHAN code = 49999` (full scan, no index) | 635 ms |
| Update 10,000 rows | 0.85 s |
| Delete 10,000 rows | 0.82 s |

The index lookup is about **1,200× faster** than a full scan, which shows why every real
database indexes its primary keys. *(Re-measure on your own machine and replace these numbers.)*

## 8. Limitations and future scope

| Limitation | How real databases solve it |
|------------|-----------------------------|
| Hash indexes only answer `=` | B-tree indexes also answer ranges and ORDER BY |
| One global lock means no parallel queries | MVCC and row-level locking |
| A transaction copies the whole database | A write-ahead log (WAL) of individual changes |
| Indexes are rebuilt after restarts and rewrites | Persistent on-disk index files |
| Rule-based planner | Cost-based optimizer using table statistics |
| Passwords sent in plain text | TLS encryption |
| `SANDARBH` (FOREIGN KEY) has no `ON DELETE CASCADE` -- only RESTRICT | CASCADE/SET NULL delete rules |
| `SHART` (CHECK) is stored as source text, not a persisted AST | A serialisable expression-tree format |
| Views are re-materialized on every read, never cached | Materialized/indexed views |
| Privileges are per `(database, table)` only -- no schema-level roles, no column-level grants, no `WITH GRANT OPTION` | Fine-grained role-based access control |
| Triggers/stored procedures have no loops, no local variables, no return value (procedures) | A full procedural language (PL/SQL, PL/pgSQL, ...) |

## 9. Conclusion

MeraDB shows that the core ideas of a relational DBMS are all understandable, and all
buildable by one student in about a month:
- a formal language and its parser
- a binary storage format with tombstones
- NULL semantics
- hash indexes and join algorithms
- snapshot-based transactions with recovery
- a client–server protocol

The Hinglish syntax makes a query's structure easy to see, and the modular design
(separate tokenizer, parser, planner, executor and storage) mirrors production systems
used in industry.

<!-- ===================== BEGIN CHAPTER 10: C++ IMPLEMENTATION ===================== -->

## 10. C++ implementation

> Chapters 1 to 9 describe the first implementation, written in Python. This chapter
> describes the second one, written in C++, which is the version submitted for the course.
> The Python version was kept and is used as the reference to check the C++ version against.
> The source is in `cpp/`; the detailed technical notes are in `docs/CPP.md`.

### 10.1 Motivation
The course requires the project to be written in C++. The Python version already worked, so the
task became a *port*: rewrite every layer in C++ and keep the behaviour the same. This had
a useful side effect. Because the Python engine was already finished, it could be used as an
answer key: for any statement, the C++ engine must print exactly what the Python engine prints.
That is a much stronger test than writing expected values by hand.

The goal was **compatibility in three places**:
1. the same Hinglish language and the same output text (including error messages);
2. the same **files on disk**, so a data folder written by one version can be opened by the other;
3. the same **network protocol**, so a Python client can talk to a C++ server and the other way round.

### 10.2 Architecture: Python module to C++ module
The layers of Chapter 4 were kept one for one. Each Python module has a C++ header and source
file with the same job (headers in `cpp/include/meradb/`, sources in `cpp/src/`):

| Python module | C++ module | Purpose |
|---------------|------------|---------|
| `tokenizer.py` | `tokenizer` | Characters to tokens |
| `ast_nodes.py`, `parser.py` | `ast`, `parser` | Syntax tree, recursive-descent parser |
| `planner.py` | `planner` | Name binding, index vs. scan, join strategy |
| `engine.py` | `engine` (`Instance`, `Engine`) | Executor, sessions, transactions, recovery |
| `evaluator.py`, `aggregates.py` | `evaluator`, `aggregates` | Expressions, NULL logic, aggregates |
| `storage.py`, `table.py`, `catalog.py`, `datatypes.py` | `storage`, `table`, `catalog`, `datatypes` | Binary heap files, hash indexes, schemas, types |
| `users.py` | `users`, `crypto` | Users and privileges; SHA-256 / PBKDF2 written in the project |
| `protocol.py` | `protocol`, `pyjson` | Wire format; JSON written the way Python writes it |
| `server.py`, `client.py` | `server`, `client` | TCP server and client library |
| `cli.py` | `cli`, `server_control`, `main` | The `start`, `stop`, `status`, `run` commands |
| `repl.py` | `repl`, `repl_text`, `cli_format` | The interactive shell |
| `highlight.py`, `tui.py` | `highlight`, `wb_*` files | Syntax colours; the full-screen workbench |

A few files have no Python counterpart: `net_compat` (Windows sockets vs. POSIX sockets),
`sys_compat` (environment, time, terminal handling) and `stack_guard` (section 10.7). Platform
specific headers are included only in these files, so the rest of the code is portable.

Two C++ specific design choices: syntax-tree nodes are owned by `std::unique_ptr` (no raw
`new` or `delete` anywhere), and a database value is a `Value` class built on `std::variant`.
Errors are C++ exceptions that carry the same `[Stage Galti] message` text as Python.

In total the C++ code is about 19,000 lines in `cpp/src` and `cpp/include`, plus about 23,000
lines of tests and test data (much of the latter is generated "golden" data, see 10.6).

### 10.3 Build system
The project is built with **CMake 3.20 or newer** and needs a C++17 compiler (MinGW-w64 g++,
MSVC, g++ or clang). The libraries it uses (nlohmann/json for the catalog, Catch2 for tests,
FTXUI for the workbench) are downloaded by CMake itself with `FetchContent`; nothing is
installed by hand. The engine, server and shell form one static library (`meradb_core`) that does
not depend on FTXUI; the workbench is a second library (`meradb_workbench`), and both are linked
into one program, `meradb_cli`. The option `-DMERADB_WORKBENCH=OFF` leaves the workbench out.
The code compiles with `-Wall -Wextra` (`/W4` for MSVC) without warnings. Two build-time steps
are worth knowing: `docs/LANGUAGE.md` is turned into a byte array and compiled into the program
(it is shown in the workbench help), and on Windows a small patch is applied to the downloaded
FTXUI source so that emoji can be typed. `build.ps1` and `build.sh` wrap the CMake commands.
Exact commands are in the README.

### 10.4 Byte-compatibility with the Python version
Matching the files and the protocol *exactly* needed care in a few places:
- **Storage.** The heap-file layout of section 4.3 (magic header, status byte, length, tagged
  values) was copied byte for byte, including the tombstones and the atomic rename.
- **JSON.** `catalog.json`, `users.json` and every network message are JSON. The C++ code has its
  own JSON reader and writer (`pyjson`) that formats numbers, escapes and key order the way
  Python's `json` module does, so the files are identical.
- **Passwords.** User passwords are stored as PBKDF2-HMAC-SHA256 hashes. The C++ side has its own
  SHA-256, HMAC and PBKDF2 (`crypto`), so a password hashed by one version verifies in the other.
- **Python behaviours.** Several small Python behaviours had to be reproduced on purpose:
  what `str.strip()`, `lower()` and `int()` do with unusual characters, which characters count as
  whitespace, how `repr()` writes a string, and how Python compares and hashes values. These are
  in `pytext` and `pyvalue`.
- **Bugs kept on purpose.** A few things in the Python engine look like bugs (for example, `ALTER`
  drops composite constraints from the catalog). They are copied, because the goal was identical
  data, and they are listed in `docs/CPP.md` so that both versions can be fixed together.

### 10.5 How compatibility was verified
Compatibility is not claimed from reading the code; it is tested by running both versions.
Each of the following checks is also registered with `ctest` (they need Python):

1. **Golden files.** `gen_golden.py` runs scripts through the Python engine and records every
   statement's message, error text, columns and rows. The C++ unit tests replay the same scripts
   and compare. The same idea is used for the shell (banner, prompts, `.help` text, colour codes,
   31 recorded transcripts) and for the syntax highlighter.
2. **Differential testing on the example scripts.** `cross_engine_diff.py` runs
   `examples/demo.mdb` and `examples/rdbms_lab_coverage.mdb` through the Python engine and the C++
   program on fresh data folders and compares the output and the exit code, once directly and
   once through a C++ server.
3. **Fuzzing.** `fuzz_triggers.py` generates random trigger and stored-procedure scripts from a
   seed and compares both engines (100 seeds in the test suite). Random shell sessions and
   thousands of random tokenizer inputs were compared during development as well.
4. **Interop matrix.** `interop_check.py` starts a Python server and a C++ server and connects a
   Python client and a C++ client to each of them (all four pairs), plus raw protocol messages and
   logins with passwords and per-user privileges. `shell_diff.py` does the same for the interactive
   shell, piping the same scripts into both shells.
5. **Interchange of data folders.** `interchange_check.py` lets the two versions work on one data
   folder in turn (one builds it with tables, triggers, procedures and users, one uses it, one
   verifies it), for all 8 combinations. Every combination must print what the all-Python run
   printed and end with the same `catalog.json` and equivalent users, and the Python side must
   accept the passwords the C++ side hashed.
6. **Workbench comparison.** `workbench_diff.py` drives the Python workbench (with Textual's test
   driver) and the C++ workbench through the same 12 scenarios (start-up, queries, errors, history,
   selection, the schema tree, transactions, CSV export, Unicode, the connect dialog) and compares
   what each screen shows. It is skipped when Textual is not installed.

### 10.6 Test strategy and counts
`ctest` registers about **780 tests** (Chapter 6 counts only the 183 Python tests). They are in
three groups:
- **Unit tests** (Catch2) for every layer: tokenizer, parser, planner, evaluator, aggregates,
  storage, catalog, tables, the engine (DDL, DML, SELECT, views, EXPLAIN, users, triggers,
  procedures), protocol and JSON, crypto, server and client, shell, and the workbench pieces (text
  handling, editor, tree, worker thread, session, dialogs, whole window driven by synthetic key
  presses).
- **End-to-end tests** with real objects: a real engine on a temporary data folder, a real server on
  a free port, the real workbench session with its worker thread, and `cli_lifecycle.py`, which
  starts, queries and stops real server processes.
- **Differential tests** against the Python version, as listed in 10.5.

Tests that need Python are not registered if Python is not found. The terminal itself (how the
shell and the workbench look and react in a real window) cannot be tested by a program; for that
`docs/CPP.md` contains a manual checklist.

### 10.7 Server: threads and locking
The C++ server follows the design of section 4.8. It starts **one thread per connection**. Each
connection owns its own `Engine` object (its session: current database and transaction), and it
is used only from that thread. This matters, because a transaction must be finished by the thread
that started it. All sessions share **one `Instance`**, which means one lock and one index cache,
so statements run one at a time (serializable isolation, as in Python). A statement that has to
wait for another session's open transaction gives up after 10 seconds with the message
"Database busy hai". The threads check a stop flag regularly instead of waiting for ever in
`recv()`, so stopping the server always finishes. A client that disconnects inside a transaction
is rolled back, and so are all open transactions when the server is stopped. The start-up code
refuses to open a data folder that another process is already serving (the `meradb.pid` file).

### 10.8 Safety features for hostile input
A network client can send any text, and deeply nested queries such as `((((...))))` make a
recursive-descent parser recurse until the stack overflows and the whole server crashes. Python
avoids a crash by raising `RecursionError`, but C++ would simply die. Therefore the C++ version
has several guards:
- a **depth cap**: one statement may have at most 400 operator/nesting units, statements inside
  statements (trigger and procedure bodies) at most 32 levels, and views over views at most 32;
- a **stack-byte guard** (`stack_guard`): every recursive function measures how many bytes of
  stack have been used since it was entered and refuses past a budget of 512 KB or half of the
  stack the thread really has, whichever is smaller (the operating system is asked for the real
  size once per thread);
- a cap of 512 nested levels when decoding JSON from the network;
- a limit of 32 levels for triggers that fire each other (Python reports a `RecursionError`).

These produce a normal error message (`Query bahut gehri (nested) hai ...`) instead of a crash.
Ordinary scripts never come near the limits.

### 10.9 Interactive shell and workbench
The **shell** (`meradb_cli shell`) behaves like the Python shell: same banner, prompts, `.help`
text, dot-commands, colours and exit codes, in local mode and through a server. As in the Python
shell, line editing is whatever the terminal provides.

The **workbench** (`meradb_cli workbench`) is the C++ counterpart of the Textual workbench. It
is built with **FTXUI**, a C++ terminal UI library that is linked in statically, and has the same
layout (schema tree, results table, log, query editor), key bindings (F5, F6, Ctrl+Up/Down,
Ctrl+S, Ctrl+O, F1, Ctrl+L, Ctrl+Q) and Hinglish labels. Two design points:
- All database calls of a session run on **one worker thread**, so the screen never freezes
  during a slow statement, and a transaction's statements stay on the thread that began it. Keys
  pressed in the meantime are queued.
- Ctrl+C does not quit the workbench (it only logs a hint); Ctrl+Q waits for the running
  statement, rolls back an open transaction and restores the terminal.

📷 *Screenshot: the C++ workbench (`meradb_cli workbench`) with a JOIN + GROUP BY result.*
📷 *Screenshot: the C++ shell (`meradb_cli shell`) showing the banner and a query.*

### 10.10 Deliberate divergences from the Python version
`docs/CPP.md` lists every known difference. The main ones are:
- **Depth and stack limits** (section 10.8) instead of Python's `RecursionError`.
- **INT is 64-bit** in C++ and arithmetic that overflows is reported as an error; Python integers
  have no limit.
- **Identifiers** may contain only ASCII letters, digits and `_` in C++; Python accepts any
  Unicode letter. Text in strings and data can be any Unicode text.
- **Sorting a mix of types** (for example TEXT against INT) gives an error message in C++, while
  Python raises an unhandled exception.
- **Server shutdown** rolls back open transactions cleanly; Python leaves that to crash recovery.
- **Command line**: option abbreviations (`--dat`) are not accepted; `--help` is laid out for 80
  columns.
- **Ctrl+C during a statement**: the shell lets the running statement finish, then exits with
  code 130, instead of abandoning it half-way.
- **Workbench**: smaller differences in text selection, wrapping, colours and some extra keys,
  because FTXUI and Textual are different libraries. The workbench is designed for dark terminals.

### 10.11 Limitations
- Everything in Chapter 8 still applies (hash indexes only, one global lock, whole-database
  snapshots for transactions, passwords sent without encryption, and so on).
- The workbench has no multiple editor tabs and no mouse text selection inside the editor.
  Every key press redraws the whole screen, which could flicker on a very slow remote link.
- The terminal behaviour of the shell and the workbench is covered by a manual checklist, not by
  automatic tests.

### 10.12 What was verified where
| Platform / toolchain | Status |
|----------------------|--------|
| Windows 11, MinGW-w64 g++ (Release and Debug) | Built with `build.ps1` (Release); all 780 `ctest` tests passed; warning free |
| Linux (POSIX code paths, g++/clang) | See `docs/CPP.md` for the results |
| macOS | **Not yet verified by the author** |
| Windows, MSVC / Visual Studio | **Not yet verified by the author** (this includes the depth checks on MSVC's smaller default stack) |

The terminal-specific code (console input, Ctrl+C handling, the workbench's terminal-mode guard)
was run on Windows only. Real-terminal checks of the workbench's key delivery were not done by a
person on Linux, macOS or the classic Windows console; the manual checklist in `docs/CPP.md` is
the place to record them.

<!-- ===================== END CHAPTER 10: C++ IMPLEMENTATION ===================== -->

## References

1. R. Nystrom, *Crafting Interpreters*, 2021. https://craftinginterpreters.com
2. A. Pavlo, *CMU 15-445/645 Intro to Database Systems*, Carnegie Mellon University. https://15445.courses.cs.cmu.edu
3. J. M. Hellerstein, M. Stonebraker, J. Hamilton, "Architecture of a Database System", *Foundations and Trends in Databases*, 2007.
4. A. Petrov, *Database Internals*, O'Reilly, 2019.
5. SQLite Documentation: Architecture and File Format. https://www.sqlite.org/arch.html
6. MySQL Workbench Manual. https://dev.mysql.com/doc/workbench/en/
7. Python Standard Library: `struct`, `socketserver`, `threading`. https://docs.python.org/3/library/
8. Textual Documentation. https://textual.textualize.io

## Appendix A: Command reference

```
meradb start | stop | status        manage the background server
meradb server                       run the server in the foreground
meradb shell                        interactive command-line client
meradb workbench                    full-screen client
meradb run FILE.mdb                 run a script
```

The C++ program is called `meradb_cli` and takes the same commands and options.

## Appendix B: Sample session

```
$ meradb start
MeraDB server chal gaya: 127.0.0.1:6372  (pid 11052)

$ meradb shell
meradb:main> BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT ZAROORI, cgpa FLOAT);
Table 'students' ban gaya (3 columns)
meradb:main> DAALO MEIN students MAAN (1, 'Ravi', 8.4), (2, 'Priya', 9.1);
2 row(s) daal di
meradb:main> DIKHAO naam SE students JAHAN cgpa > 9;
+-------+
| naam  |
+-------+
| Priya |
+-------+
1 row(s)
```
