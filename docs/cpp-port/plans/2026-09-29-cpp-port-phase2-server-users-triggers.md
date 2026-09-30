# MeraDB C++ Port — Phase 2: Server, Client, Protocol, Users, Triggers, Procedures Implementation Plan

**Goal:** Finish the C++ port of everything MeraDB does *around* the core
engine: users and privileges, triggers, stored procedures, the TCP server
(thread per connection, one shared engine lock), the wire protocol, the
client library, and the `server` / `start` / `stop` / `status` / `run`
commands — verified against the Python implementation, including a
Python-client-to-C++-server and C++-client-to-Python-server interop matrix.

**Architecture:** Same layer-for-layer mirror as Phase 1. New modules mirror
`meradb/users.py`, `meradb/protocol.py`, `meradb/server.py`, `meradb/client.py`
and the Phase-B parts of `meradb/parser.py` / `meradb/engine.py`. Networking
sits behind a tiny `net_compat` shim (raw Winsock2 / BSD sockets, IPv4,
blocking I/O). The server is one `std::thread` per connection sharing one
`Instance` (one recursive timed mutex), exactly like Python's
`ThreadingTCPServer`. Triggers and procedures use Python's substitution
model: a fresh parse of the stored body text, `NAYA.col` / `PURANA.col` /
parameter references rewritten to literals, each statement run through
`Engine::executeStatement`.

**Tech Stack:** C++17, CMake + FetchContent, nlohmann/json, Catch2 v3 (all
already present from Phase 1). **No new third-party dependency** (SHA-256 /
HMAC / PBKDF2 are written in-tree; see Design decision D4). Python 3
(reference engine) is used only by the verification scripts.

**Spec:** [docs/cpp-port/specs/2026-09-27-cpp-port-design.md](../specs/2026-09-27-cpp-port-design.md)
(Phase 2 row of "Phased delivery", "Networking", "Users/privileges",
"Triggers/procedures").

**Starts from:** branch `worktree-cpp-port-phase1` at `7769680` (Phase 1
complete: 244/244 tests, both trimmed example scripts match Python).
Create the Phase 2 work on a new branch off that commit.

## Global Constraints

Everything in the Phase 1 plan's Global Constraints still applies. Additions
and the lessons that recurred in the Phase 1 reviews:

- **Python is the oracle, never the plan text.** Error wording, messages,
  result shapes, JSON key order and file formats come from reading
  `meradb/*.py`. Expected values in tests come from *running the Python
  engine* (golden generator, cross-engine scripts). When a test must hard-code
  a string, copy it from the Python source line quoted in the task, never
  retype it from memory. Where this plan and Python disagree, Python wins and
  the plan is wrong — fix the plan's test, not the engine.
- **Match error text exactly**, including the `[Stage Galti] ` prefix
  conventions: `MeraDBError::message()` is the bare text (what the server puts
  on the wire and the client re-wraps), `what()` is prefixed.
- **GCC evaluates call arguments right-to-left.** Never put two calls that can
  throw (or have side effects) in one argument list or one braced initializer.
  Bind children into named locals *in source order* first
  (`auto left = f(a); auto right = f(b); make(left, right);`).
- **Value comparison and hashing:** use `pyEquals` / `PyValueHash` /
  `PyValueEq` from `pyvalue.h`, never raw `Value ==` or `operator<` on
  `variant`s (`1 == 1.0 == SACH` in Python).
- **Iterate in Python's order.** Where Python iterates a dict (a row, a
  catalog, `users.json`, a JSON object) keep insertion order: use
  `InsertionOrderedMap` / `nlohmann::ordered_json`, iterate `scope.allKeys()`
  or schema column order, never an `unordered_map`. Anything persisted or
  sent over the wire must be produced from an ordered structure.
- **On-disk and on-wire compatibility.** A data folder written by either
  engine must be usable by the other: `catalog.json` (with `triggers` /
  `procedures` in Python's exact shape), `users.json`, `.tbl` files,
  `.wapas/`, `meradb.pid`. The wire protocol is byte-compatible in the sense
  that a Python client can talk to a C++ server and vice versa
  (Design decision D3).
- **MSVC / portability rules** (only MinGW g++ can be built on the dev
  machine, so review by reading): no `long double`, `__int128`, VLAs,
  `ssize_t`, GCC builtins, or POSIX-only headers outside `#ifdef` blocks; no
  `std::getenv` / `localtime` / `strerror` (C4996) — use the `sys_compat`
  wrappers; never include `<windows.h>` / `<winsock2.h>` from a public header;
  add `/utf-8` for MSVC; avoid `ptrdiff_t` → `long` / `int` narrowing (use
  `auto` or `static_cast`). Socket code is the only place with
  `#ifdef _WIN32`, and lives in `net_compat.cpp` / `sys_compat.cpp`.
- **Threading rules.** One `Engine` per connection, created, used and
  destroyed on that connection's thread. `Engine::close()` and `~Engine()`
  must run on the thread that ran `SHURU` (the mutex hold is owned by that
  thread). All shared state (`Instance`: catalogs, indexes, users) is only
  touched while holding `Instance::lock`, except `UserStore`, which has its own
  mutex because the handshake verifies passwords before any statement runs.
- **Windows MinGW builds need PATH exported in every shell** (see the Phase 1
  ledger, "Build env"): CMake's `bin` and WinLibs' `mingw64\bin`, e.g.
  `export PATH="/c/Program Files/CMake/bin:<winlibs>/mingw64/bin:$PATH"`.
  Processes started earlier do not inherit the user's updated PATH mid-session.
- **Tests never use fixed TCP ports.** Servers bind port 0 and read
  `port()`. Cross-engine / interop scripts that need a port ask the OS for a
  free one. Every cross-engine ctest passes `--local` to the C++ CLI so a
  developer's real server on 6372 can never change a result.
- **Hygiene:** no references to tool-generated attribution lines or tool names anywhere in code,
  comments, docs or commit messages; no personal, university or course
  details; commit messages are plain one-liners. Never modify `meradb/`
  (`git diff <phase-2-base> -- meradb` must be empty).
- **Deliberate divergences** from Python are allowed only when listed in the
  "Design decisions" section below; each one is copied into `docs/CPP.md` in
  Task 25.

---

## Design decisions

### D1. Socket layer

Raw Winsock2 (Windows) / BSD sockets (POSIX) behind `net_compat.h`, exactly
as the spec says; no Boost.Asio. IPv4 only (Python's `TCPServer` is AF_INET
too). Blocking I/O everywhere; a 100 ms `select()` tick in the accept loop
(Python uses a 0.5 s `poll_interval`) so shutdown is prompt. Connection
threads poll a stop flag every 100 ms with `select()` as well (never a bare
blocking `recv`): on Windows `shutdown()` does **not** wake a blocked `recv`,
so shutdown cannot depend on it. `Socket` is a
move-only RAII handle storing the native descriptor as `std::intptr_t`, so no
platform header leaks out. Failures throw `net::NetError`. `SIGPIPE` is
ignored process-wide on POSIX; `TCP_NODELAY` is set on every socket and each
protocol message is written with a single `send` loop. Windows uses
`SO_EXCLUSIVEADDRUSE` (Python simply does not set `SO_REUSEADDR` there);
POSIX uses `SO_REUSEADDR`. Client connect timeout 5 s (non-blocking connect
+ `select`), read timeout 60 s (`SO_RCVTIMEO`), same as `client.py`.

### D2. Threading model and the SHURU thread rule

Thread per connection, one shared `Instance` with one `std::recursive_timed_mutex`
(`Instance::lock`), statements acquire it for their duration with a
10 s timeout ("Database busy hai ..."), `SHURU` takes it one extra time and
holds it until `PAKKA` / `WAPAS`. This is what Phase 1's `Engine` already
does; Phase 2 adds nothing to the engine's locking except a shared
`Engine::acquireLock(timeout)` helper (also used by `schemaTree()`).

The interaction with the Phase 1 rule "`PAKKA`/`WAPAS` from a thread other than
`SHURU`'s throws": a connection thread reads a request, runs *all* its
statements, writes the reply, and only then reads the next request — the
whole life of a session, including its open transaction, stays on **one
thread**. Nothing may dispatch a session's requests to a pool or another
thread. `Engine` is created inside the connection thread, `close()` runs in
that thread's cleanup (rolls back an unfinished transaction), and
`~Engine()` runs there too. So **the engine's thread rule needs no
adaptation**; it becomes a checked invariant (a test in Task 18 asserts that a
transaction left open by a disconnect is rolled back and releases the lock).
This also closes the Phase 1 review's minor M2 ("close() from another thread
leaves the lock held").

Shutdown differs from Python slightly and *better*: Python's daemon threads
die with the process, leaving snapshots for crash recovery. The C++ server
stops accepting, sets a stop flag that every connection thread notices within
100 ms (each polls with `select()` between reads), and joins each connection
thread; each thread rolls back its own open transaction. The resulting data folder is the same as
after Python's crash recovery. `stop --force` (kill) still relies on
recovery at the next start.

Shared state audit (all under `Instance::lock` unless noted): `catalogs_`,
`indexes_`, `Catalog` objects, heap files; `users` has its own mutex;
`Server::sessions_` is atomic; `Server::log` has its own mutex;
`Instance::databases()` (used by `status`) only lists a directory and needs no
lock (wrapped in try/catch because a concurrent `DROP DATABASE` can remove an
entry mid-listing).

### D3. Wire protocol (read from `meradb/protocol.py` + `server.py` + `client.py`)

Newline-delimited JSON, one object per line, UTF-8, port **6372**, default host
`127.0.0.1`, protocol version 1, max line 64 MiB. `json.dumps` defaults:
separators `", "` and `": "`, `ensure_ascii=True` (non-ASCII becomes lowercase
`\uXXXX`, astral characters as surrogate pairs, DEL as `\u007f`), floats via
`float.__repr__`, non-finite floats as the bare tokens `Infinity`, `-Infinity`,
`NaN`. A C++ peer must (a) write exactly that so a Python peer parses it, and
(b) accept those bare tokens from a Python peer. nlohmann cannot do either, so
`pyjson.cpp` has a small writer (`dumpPython`) and a pre-scanner that turns
bare tokens outside strings into `{"$float": "Infinity"}` objects before
`Json::parse`.

Frames:

| Direction | Frame |
|---|---|
| client → server | `{"type": "hello", "version": 1, "password": <str or null>, "database": <str or null>, "user": <str or null>}` |
| server → client (ok) | `{"ok": true, "server": "MeraDB 1.0.0", "protocol": 1, "database": "main"}` |
| server → client (refused) | `{"ok": false, "error": "<bare message>"}` then close: `"User ya password galat hai"` (per-user login failed), `"Password galat hai"` (shared password), or the `UseDatabase` error's `message` |
| client → server | `{"type": "query", "text": "..."}` |
| server → client | `{"ok": true, "results": [{"columns": [...], "rows": [[...]], "message": "...", "error": ""}], "database": "main", "in_transaction": false}` |
| client → server | `{"type": "schema"}` → `{"ok": true, "tree": [{"name", "current", "tables": [{"name", "columns": [<Column.to_dict>]}]}]}` (or `{"ok": false, "error": <bare>}`) |
| client → server | `{"type": "status"}` → `{"ok": true, "server": "MeraDB 1.0.0", "pid": N, "data_dir": "...", "started": "YYYY-MM-DDTHH:MM:SS", "sessions": N, "databases": [...]}` |
| client → server | `{"type": "ping"}` → `{"ok": true}` |
| client → server | `{"type": "shutdown"}` → `{"ok": true}` then the server stops; from a non-loopback peer: `{"ok": false, "error": "Shutdown sirf usi computer se ho sakta hai jahan server chal raha hai"}` |
| unknown type | `{"ok": false, "error": "Unknown request type: 'foo'"}` (Python `{kind!r}`) |
| malformed line | `{"ok": false, "error": "[Protocol Galti] Galat message: ..."}` and the connection stays open; a non-object: `"[Protocol Galti] Message ek JSON object hona chahiye"`; oversize: `"... Message bahut bada hai"` |

Cell encoding: `null` (KHALI), `true`/`false`, integers, floats, strings,
DATE as `{"$date": "YYYY-MM-DD"}`. Handshake rule: the first message must be
a `hello` object or the server silently closes. A truthy `user` supersedes
the shared password entirely (SERVER.md "no username = superuser"). The
`meradb.pid` file is `json.dump(indent=2)` of `{"pid", "host", "port",
"started"}`; both engines refuse to open a data folder another process serves
(`running_server` = pid file + a successful TCP connect). Known, accepted
differences: the text of JSON parse errors (`Galat message: ...`), the OS
error text inside `ServerUnavailable`, an oversize line is discarded up to its
newline (Python keeps reading the remainder as the next "message"), and a
non-ASCII shared password is compared bytewise (Python's `hmac.compare_digest`
raises `TypeError` and drops the connection).

### D4. Authentication and password storage

`users.json` at the top of the data folder: `{name: {"salt": <32 hex>,
"hash": <64 hex>, "grants": {"db.table": [privileges sorted in
DIKHAO, DAALO, BADLO, MITAO order]}}}`, written `json.dump(indent=2)` via
temp file + rename. Hash = PBKDF2-HMAC-SHA256, 100,000 iterations, 16-byte
random salt, 32-byte output, password UTF-8. Written **in-tree**
(`crypto.h/.cpp`: SHA-256 per FIPS 180-4, HMAC, PBKDF2 with ipad/opad
midstate reuse so 100k iterations cost ~200k compressions). **Spec
discrepancy:** the spec names picosha2; we do not add it because (a)
FetchContent of an untagged single-header repo is exactly the kind of network
fragility Phase 1 had to engineer away (URL tarballs only), (b) the midstate
optimisation needs the compression function, which picosha2 does not expose,
and (c) SHA-256 is ~80 lines with published test vectors. Salt comes from the
OS CSPRNG (`BCryptGenRandom` / `/dev/urandom`), never `std::random_device`
(deterministic on some MinGW builds). Compatibility is proven in both
directions in Task 24.

### D5. Trigger and procedure semantics (read from `engine.py`)

- Stored in `catalog.json` as Python does: `triggers: {name: {"timing",
  "event", "table", "body_text"}}`, `procedures: {name: {"params": [[name,
  type], ...], "body_text"}}`; the text between `SHURU` and `KHATAM`
  including the final `;`, captured from the source, parse-validated at
  create time and re-parsed on every fire/call.
- `BANAO TRIGGER`: duplicate name → `Trigger 'x' pehle se hai`; target must
  be a real table (`Table 'x' exist nahi karta -- trigger sirf ek REAL table
  par lag sakta hai`); body parsed. No cleanup when the table is dropped or
  renamed (mirrored: such triggers are simply orphaned).
- Firing: per affected row, in creation order, `PEHLE` after validation and
  uniqueness/FK checks but before the write, `BAAD` after the write. A trigger
  body statement runs through `executeStatement` (so it takes the lock,
  re-checks the session's privileges — invoker rights — and can itself fire
  triggers). An error in either kind propagates; `PEHLE` errors abort the
  outer statement, `BAAD` errors do not undo rows already written.
  `TAKRAAV PAR BADLO`: collided rows fire `BADLO` triggers (with NAYA and
  PURANA), fresh rows fire `DAALO`. Order of upsert events follows
  `engine.py` lines 1083-1118 exactly.
- Substitution (`_substitute` / `_substitute_statement`): only `ColumnRef`s are
  replaced. Trigger: `naya.col` / `purana.col` when the matching row exists
  and has that column. Procedure: an *unqualified* ref whose name is a
  parameter. Statement kinds rewritten: INSERT (rows, select, upsert
  assignments), UPDATE, DELETE, SELECT (columns, joins, where, group by,
  having, order by), CHALAO; every other statement kind is executed as parsed.
  Subquery / IN-subquery expressions are left untouched.
- `CHALAO`: argument count checked (`... ko N argument(s) chahiye, M mile`),
  each argument evaluated as a constant expression and `coerce`d to the
  parameter type; result message `Procedure 'x' chal gaya (N statement(s)):
  m1; m2` (trailing `": "` when no statement produced a message).
  Statements run without an implicit transaction: a failure leaves earlier
  statements applied (mirrored).
- **Deliberate divergence: nesting cap.** Python has no limit: a trigger that
  updates its own table recurses until `RecursionError`, which escapes as a
  non-MeraDB exception (`[Internal Galti]` on the server, a traceback locally).
  C++ would overflow the thread stack and kill the server, so nesting of
  trigger/procedure bodies is capped at **32** levels and reports
  `ExecutionError("Trigger/procedure bahut gehra chal raha hai (limit 32) --
  shayad koi trigger khud ko baar-baar chala raha hai")`.
- **Deliberate divergence: parse budget** (Phase 1 review M1): ONE statement may
  spend at most 400 operator/nesting units (every `YA`, `AUR`, `NAHI`, arithmetic
  operator, unary minus, `AGAR`, call or grouping paren, `IN` list item, subquery
  and set-operation step costs one) or it is a `ParseError` (`Query bahut gehri
  (nested) hai (limit 400)`); a statement inside a trigger/procedure body gets a
  fresh budget and bodies nest at most 32 deep; views over views are capped at 32. Python hits `RecursionError` at roughly 110
  nested parentheses; the C++ cap is deliberately more generous but keeps a
  hostile network client from crashing the server.

### D6. Privileges

`Engine::user` (nullopt = superuser, the only kind an embedded engine ever
is). Checked once per statement in `executeStatement` after the lock and
current-database checks: SELECT needs `DIKHAO` on the FROM table and every
joined table; INSERT needs `DAALO` (plus `DIKHAO` on tables read by
`INSERT ... DIKHAO`); UPDATE `BADLO`; DELETE `MITAO`; `SAMJHAO` checks its
inner statement; set operations re-check each side when executed; **every
other statement** (DDL, `BATAO`, `DIKHAO TABLES`, `ISTEMAL`, users, grants,
triggers, procedures, `CHALAO`, `SHURU/PAKKA/WAPAS`) is superuser-only, with
the Python class name in the message (`'CreateTable'`, `'ShowTables'`,
`'Begin'` ...) — hence `astClassName()` (Task 2). Grants key on
`"<current db>.<table-or-view>"`; a view is granted by its own name.

### D7. Data folder default, embedded-vs-served protection

`defaultDataDir()` now mirrors Python (`MERADB_DATA`, else
`%LOCALAPPDATA%\MeraDB\data` on Windows, `~/.local/share/MeraDB/data`
elsewhere), replacing Phase 1's `./data`. Server, `start`/`stop`/`status`,
clients and Python then find each other by default. `Instance(dataDir,
served=false)` refuses to open a folder a server is serving, exactly like
Python.

### D8. Backend abstraction and CLI scope

`Backend` (abstract: `runScript`, `currentDb`, `inTransaction`, `schemaTree`,
`description`, `close`) with `LocalBackend` (wraps `Engine`) and
`Connection` (network) gives the CLI — and Phases 3-4's shell and workbench —
Python's duck-typed "same API for embedded and remote". Phase 2's CLI
(`meradb_cli`) supports `server`, `start`, `stop`, `status`, `run` and the
client options `-H -p -d -W -U --local -D` with Python's automatic local
fallback. `shell` / `workbench` print a "not in the C++ version yet" note and
exit 1 (Phases 3 / 4). The binary keeps the name `meradb_cli`; user-facing
messages still say `meradb` because they are Python's wording.

### D9. Where extra review effort goes

1. **Server threading, lock and transaction lifecycle** (Tasks 16, 18) —
   disconnect mid-transaction, shutdown with live sessions, lock timeout,
   `Engine` thread confinement.
2. **Trigger hooks inside INSERT / UPDATE / DELETE / upsert** (Task 10) —
   ordering of `PEHLE`/`BAAD`, shared index cache while a trigger writes to
   the same table, error propagation.
3. **Wire codec fidelity** (Task 14) — `ensure_ascii` escaping, float repr,
   non-finite tokens, dates, oversized/partial lines.
4. **Crypto and `users.json`** (Tasks 5, 6, 24) — vectors plus the two-way
   Python check.
5. **Platform socket / process code** (Tasks 13, 20) — never compiled with
   MSVC here; review against the MSVC rules above.
6. **Privilege matrix** (Task 8) — exact wording, class names, `SetOp` /
   `Explain` recursion, view grants.

---

## Reference: Phase 2 language (from `meradb/parser.py`)

Reserved words already in the C++ tokenizer (Phase 1 review M1): `USER GUPT
ADHIKAR DO KO SAB TRIGGER PEHLE BAAD PROCEDURE CHALAO`. `NAYA` / `PURANA`
are ordinary identifiers (lower-cased by the tokenizer, so a reference
arrives as `ColumnRef(name, table="naya")`). `SHURU` / `KHATAM` are reused as
block delimiters.

```
BANAO USER name GUPT 'password'            HATAO USER name
ADHIKAR DO  priv[, priv]... PAR table KO user     -- priv: DIKHAO DAALO BADLO MITAO, or SAB (all four)
ADHIKAR WAPAS priv[, priv]... PAR table SE user
BANAO TRIGGER name PEHLE|BAAD DAALO|BADLO|MITAO PAR table SHURU stmt; [stmt;]... KHATAM
HATAO TRIGGER name
BANAO PROCEDURE name ([p TYPE [, p TYPE]...]) SHURU stmt; [stmt;]... KHATAM
HATAO PROCEDURE name                       CHALAO name([expr [, expr]...])
```

## Task index (details below; batches at the end)

| # | Title | Batch |
|---|---|---|
| 1 | Build groundwork and `sys_compat` | A |
| 2 | AST nodes for Phase 2 statements and `astClassName` | A |
| 3 | Parser: users, privileges, `CHALAO`, `HATAO` variants | A |
| 4 | Parser: `SHURU ... KHATAM` bodies, `BANAO TRIGGER` / `PROCEDURE` | A |
| 5 | Crypto: SHA-256, HMAC, PBKDF2 | A |
| 6 | `UserStore` (`users.json`) | A |
| 7 | Catalog: trigger and procedure helpers | A |
| 8 | Engine: users, grants, privilege enforcement | A |
| 9 | AST substitution (`NAYA` / `PURANA` / parameters) | A |
| 10 | Engine: triggers | A |
| 11 | Engine: stored procedures | A |
| 12 | Golden scripts for users, triggers, procedures | A |
| 13 | `net_compat`: portable sockets | B |
| 14 | `pyjson` and `protocol`: codec, framing, pid file | B |
| 15 | Engine: lock helper, `schemaTree`, served-folder protection | B |
| 16 | Server | B |
| 17 | `Backend`, `LocalBackend`, client `Connection` | B |
| 18 | In-process client/server tests | B |
| 19 | Hardening: depth caps and protocol abuse | B |
| 20 | Process control: spawn, kill, `start` / `stop` / `status` logic | C |
| 21 | CLI: option parsing, dispatch, `server`, `run` with local fallback, lifecycle ctest | C |
| 22 | Cross-engine: full example scripts, local and via server | D |
| 23 | Interop matrix: Python and C++ clients x servers (+ tokenizer column fix) | D |
| 24 | Data-folder interchange and trigger/procedure differential fuzz | D |
| 25 | Docs, completion checklist, hand-off to Phases 3-5 | D |

---

## Command conventions used in every task

- Configure once (MinGW on Windows, from the repo root, PATH exported first):
  `cmake -S cpp -B cpp/build -G "MinGW Makefiles"` (Linux/macOS: omit `-G`).
  Re-run it whenever a task edits a `CMakeLists.txt`.
- Build: `cmake --build cpp/build`.
- Test everything: `ctest --test-dir cpp/build --output-on-failure`.
  Test one area: `ctest --test-dir cpp/build --output-on-failure -R "<name>"`
  where `<name>` is a fragment of the Catch2 test-case names (each task names
  its fragment; test-case names start with the area word on purpose).
- "Expected: FAIL" steps mean the build breaks (missing header) or the test
  fails; either counts.
- Python oracle: `python -m meradb run --local --data <tmpdir> <file.mdb>` from
  the repo root (the harness in Task 22 wraps this).

---

# BATCH A — language and engine features (no sockets)

### Task 1: Build groundwork and `sys_compat`

**Files:**
- Create: `cpp/include/meradb/sys_compat.h`
- Create: `cpp/src/sys_compat.cpp`
- Test: `cpp/tests/test_sys_compat.cpp`
- Modify: `cpp/CMakeLists.txt` (Threads, `/utf-8`, Windows libs and defines, new source)
- Modify: `cpp/tests/CMakeLists.txt` (add `test_sys_compat.cpp`)
- Modify: `cpp/src/engine.cpp:561` (`long pkCount` -> `auto`, Phase 1 review M5)
- Modify: `cpp/src/main.cpp` (use `sys::getEnv` instead of `std::getenv`, review M4)

**Interfaces:**
- Consumes: `meradb::StorageError` (errors.h).
- Produces, all in `namespace meradb::sys`: `getEnv(name) -> std::optional<std::string>`,
  `processId() -> std::int64_t`, `localIsoSeconds()` (`2026-09-29T14:03:07`,
  Python `datetime.isoformat(timespec="seconds")`), `localLogStamp()`
  (`2026-09-29 14:03:07`, the server log prefix), `randomBytes(n)` (OS CSPRNG),
  `homeDir()`. Every later task that touches the environment, the clock, the
  pid or randomness goes through these, so `#ifdef _WIN32` and MSVC's
  deprecation warnings stay in this one file.

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_sys_compat.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/sys_compat.h"
#include <cctype>

using namespace meradb;

TEST_CASE("sys_compat getEnv distinguishes unset from set", "[sys_compat]") {
    REQUIRE_FALSE(sys::getEnv("MERADB_SURELY_NOT_SET_VARIABLE_XYZ").has_value());
    // PATH exists on every platform CI/dev machines use (Windows lookup is case-insensitive).
    auto path = sys::getEnv("PATH");
    REQUIRE(path.has_value());
    REQUIRE_FALSE(path->empty());
}

TEST_CASE("sys_compat processId is positive", "[sys_compat]") {
    REQUIRE(sys::processId() > 0);
}

TEST_CASE("sys_compat timestamps have Python's isoformat shape", "[sys_compat]") {
    std::string iso = sys::localIsoSeconds();  // 2026-09-29T14:03:07
    REQUIRE(iso.size() == 19);
    REQUIRE(iso[4] == '-');
    REQUIRE(iso[7] == '-');
    REQUIRE(iso[10] == 'T');
    REQUIRE(iso[13] == ':');
    REQUIRE(iso[16] == ':');
    for (size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u, 11u, 12u, 14u, 15u, 17u, 18u})
        REQUIRE(std::isdigit(static_cast<unsigned char>(iso[i])));

    std::string stamp = sys::localLogStamp();  // 2026-09-29 14:03:07
    REQUIRE(stamp.size() == 19);
    REQUIRE(stamp[10] == ' ');
    REQUIRE(stamp.substr(0, 10) == iso.substr(0, 10));
}

TEST_CASE("sys_compat randomBytes returns fresh bytes each call", "[sys_compat]") {
    auto a = sys::randomBytes(16);
    auto b = sys::randomBytes(16);
    REQUIRE(a.size() == 16);
    REQUIRE(b.size() == 16);
    REQUIRE(a != b);  // 2^-128 chance of a false failure
    REQUIRE(sys::randomBytes(0).empty());
}

TEST_CASE("sys_compat homeDir is never empty", "[sys_compat]") {
    REQUIRE_FALSE(sys::homeDir().empty());
}
```

- [ ] **Step 2: Add the test file to the build and verify it fails**

Add `test_sys_compat.cpp` to `cpp/tests/CMakeLists.txt` (before the
`# test files are appended here by later tasks` line). Run:
```bash
cmake --build cpp/build
```
Expected: FAIL — `meradb/sys_compat.h` does not exist.

- [ ] **Step 3: Write the header**

```cpp
// cpp/include/meradb/sys_compat.h
//
// The only place that knows about OS differences for the small things the
// server and CLI need: environment, pid, wall clock, randomness. Public
// header: no <windows.h> here.
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace meradb::sys {

// nullopt when the variable is unset (an empty value is returned as "").
std::optional<std::string> getEnv(const std::string& name);

std::int64_t processId();

// Local time, like datetime.now().isoformat(timespec="seconds"): 2026-09-29T14:03:07
std::string localIsoSeconds();
// Local time, like f"{datetime.now():%Y-%m-%d %H:%M:%S}": 2026-09-29 14:03:07
std::string localLogStamp();

// Bytes from the operating system's CSPRNG (BCryptGenRandom / /dev/urandom).
// Throws StorageError if the OS refuses. Never uses std::random_device, which
// is deterministic on some MinGW builds.
std::vector<std::uint8_t> randomBytes(std::size_t count);

// os.path.expanduser("~"): %USERPROFILE% on Windows, $HOME elsewhere, "." if unknown.
std::string homeDir();

}  // namespace meradb::sys
```

- [ ] **Step 4: Write the implementation**

```cpp
// cpp/src/sys_compat.cpp
#include "meradb/sys_compat.h"
#include "meradb/errors.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <unistd.h>
#endif

namespace meradb::sys {

std::optional<std::string> getEnv(const std::string& name) {
#if defined(_MSC_VER)
    char* buffer = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&buffer, &length, name.c_str()) != 0 || buffer == nullptr) return std::nullopt;
    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name.c_str());
    if (value == nullptr) return std::nullopt;
    return std::string(value);
#endif
}

std::int64_t processId() {
#ifdef _WIN32
    return static_cast<std::int64_t>(GetCurrentProcessId());
#else
    return static_cast<std::int64_t>(getpid());
#endif
}

namespace {

std::tm localTm(std::time_t t) {
    std::tm out{};
#if defined(_MSC_VER)
    localtime_s(&out, &t);
#elif defined(_WIN32)
    // MinGW: std::localtime uses a static buffer, so serialise the calls.
    static std::mutex m;
    std::lock_guard<std::mutex> guard(m);
    out = *std::localtime(&t);
#else
    localtime_r(&t, &out);
#endif
    return out;
}

std::string formatNow(const char* format) {
    std::tm tmv = localTm(std::time(nullptr));
    char buffer[64];
    std::size_t n = std::strftime(buffer, sizeof buffer, format, &tmv);
    return std::string(buffer, n);
}

}  // namespace

std::string localIsoSeconds() { return formatNow("%Y-%m-%dT%H:%M:%S"); }
std::string localLogStamp() { return formatNow("%Y-%m-%d %H:%M:%S"); }

std::vector<std::uint8_t> randomBytes(std::size_t count) {
    std::vector<std::uint8_t> out(count);
    if (count == 0) return out;
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        throw StorageError("OS random generator (BCryptGenRandom) ne jawab nahi diya");
#else
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom) throw StorageError("/dev/urandom khul nahi paaya");
    urandom.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(count));
    if (!urandom) throw StorageError("/dev/urandom se random bytes nahi mile");
#endif
    return out;
}

std::string homeDir() {
#ifdef _WIN32
    if (auto v = getEnv("USERPROFILE"); v && !v->empty()) return *v;
#endif
    if (auto v = getEnv("HOME"); v && !v->empty()) return *v;
    return ".";
}

}  // namespace meradb::sys
```

- [ ] **Step 5: Update `cpp/CMakeLists.txt`**

Make these edits (leave everything else as it is):

```cmake
# after `set(CMAKE_CXX_STANDARD_REQUIRED ON)`:
find_package(Threads REQUIRED)

# replace the MSVC/else block with:
if(MSVC)
  add_compile_options(/W4 /utf-8)   # sources contain UTF-8 (Devanagari, dashes) in comments
else()
  add_compile_options(-Wall -Wextra)
endif()

# in add_library(meradb_core STATIC ...) add, before the "appended by later tasks" comment:
  src/sys_compat.cpp

# replace the target_link_libraries(meradb_core ...) line with:
target_link_libraries(meradb_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
if(WIN32)
  # Winsock + the OS random generator; keep windows.h lean and min/max macros away.
  target_link_libraries(meradb_core PUBLIC ws2_32 bcrypt)
  target_compile_definitions(meradb_core PUBLIC _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
```

- [ ] **Step 6: Fix the two Phase 1 minors**

In `cpp/src/engine.cpp` line 561 change `long pkCount = std::count_if(` to
`auto pkCount = std::count_if(` (the two comparisons below it, `pkCount > 1`
and `pkCount > 0`, stay valid for the signed `ptrdiff_t`).

In `cpp/src/main.cpp` replace
```cpp
    const char* envData = std::getenv("MERADB_DATA");
    std::string dataDir = (envData && *envData) ? envData : "data";
```
with
```cpp
    auto envData = meradb::sys::getEnv("MERADB_DATA");
    std::string dataDir = (envData && !envData->empty()) ? *envData : "data";
```
and add `#include "meradb/sys_compat.h"` to its includes. (Task 20 rewrites
`main.cpp` completely; this step only removes the MSVC warning meanwhile.)

- [ ] **Step 7: Build and run all tests**

```bash
cmake -S cpp -B cpp/build -G "MinGW Makefiles"
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure
```
Expected: 5 new `sys_compat` tests pass; the full suite is 249/249 (244 + 5);
no compiler warnings.

- [ ] **Step 8: Commit**

```bash
git add cpp/include/meradb/sys_compat.h cpp/src/sys_compat.cpp cpp/tests/test_sys_compat.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt cpp/src/engine.cpp cpp/src/main.cpp
git commit -m "Add sys_compat helpers and Phase 2 build groundwork"
```

**Completion checklist:**
- [ ] `git diff --stat` touches only the files listed above
- [ ] no `getenv`, `localtime` or `strerror` calls outside `sys_compat.cpp`
      (`grep -rn "getenv\|localtime\|strerror" cpp/src cpp/include`)
- [ ] full suite green, zero warnings

---

### Task 2: AST nodes for Phase 2 statements and `astClassName`

**Files:**
- Modify: `cpp/include/meradb/ast.h` (ten new statement structs)
- Modify: `cpp/include/meradb/ast_util.h`, `cpp/src/ast_util.cpp` (add `astClassName`)
- Test: `cpp/tests/test_ast.cpp` (append)

**Interfaces:**
- Produces: `ast::CreateUser{name,password}`, `DropUser{name}`,
  `Grant{privileges,table,user}`, `Revoke{privileges,table,user}`,
  `CreateTrigger{name,timing,event,table,bodyText}`, `DropTrigger{name}`,
  `ProcParam{name,typeName}`, `CreateProcedure{name,params,bodyText}`,
  `DropProcedure{name}`, `CallProcedure{name,args}`; and
  `std::string astClassName(const ast::Statement&)` returning the **Python
  class name** of any statement (`"CreateTable"`, `"AlterAddColumn"`, ...).
  Task 8 puts that name in the "superuser nahi hai" error, so it must match
  `type(stmt).__name__` for every statement kind, including the Phase 1 ones.

- [ ] **Step 1: Write the failing test** (append to `cpp/tests/test_ast.cpp`)

```cpp
#include "meradb/ast_util.h"

TEST_CASE("ast astClassName matches Python class names", "[ast][phase2]") {
    using namespace meradb::ast;
    CHECK(meradb::astClassName(CreateDatabase()) == "CreateDatabase");
    CHECK(meradb::astClassName(DropDatabase()) == "DropDatabase");
    CHECK(meradb::astClassName(UseDatabase()) == "UseDatabase");
    CHECK(meradb::astClassName(ShowTables()) == "ShowTables");
    CHECK(meradb::astClassName(Describe()) == "Describe");
    CHECK(meradb::astClassName(CreateView()) == "CreateView");
    CHECK(meradb::astClassName(DropView()) == "DropView");
    CHECK(meradb::astClassName(ShowViews()) == "ShowViews");
    CHECK(meradb::astClassName(CreateTable()) == "CreateTable");
    CHECK(meradb::astClassName(AlterAddComposite()) == "AlterAddComposite");
    CHECK(meradb::astClassName(DropTable()) == "DropTable");
    CHECK(meradb::astClassName(AlterAddColumn()) == "AlterAddColumn");
    CHECK(meradb::astClassName(AlterDropColumn()) == "AlterDropColumn");
    CHECK(meradb::astClassName(RenameTable()) == "RenameTable");
    CHECK(meradb::astClassName(RenameColumn()) == "RenameColumn");
    CHECK(meradb::astClassName(TruncateTable()) == "TruncateTable");
    CHECK(meradb::astClassName(CompactTable()) == "CompactTable");
    CHECK(meradb::astClassName(Begin()) == "Begin");
    CHECK(meradb::astClassName(Commit()) == "Commit");
    CHECK(meradb::astClassName(Rollback()) == "Rollback");
    CHECK(meradb::astClassName(Explain()) == "Explain");
    CHECK(meradb::astClassName(Insert()) == "Insert");
    CHECK(meradb::astClassName(Select()) == "Select");
    CHECK(meradb::astClassName(SetOp()) == "SetOp");
    CHECK(meradb::astClassName(Update()) == "Update");
    CHECK(meradb::astClassName(Delete()) == "Delete");
    CHECK(meradb::astClassName(CreateUser()) == "CreateUser");
    CHECK(meradb::astClassName(DropUser()) == "DropUser");
    CHECK(meradb::astClassName(Grant()) == "Grant");
    CHECK(meradb::astClassName(Revoke()) == "Revoke");
    CHECK(meradb::astClassName(CreateTrigger()) == "CreateTrigger");
    CHECK(meradb::astClassName(DropTrigger()) == "DropTrigger");
    CHECK(meradb::astClassName(CreateProcedure()) == "CreateProcedure");
    CHECK(meradb::astClassName(DropProcedure()) == "DropProcedure");
    CHECK(meradb::astClassName(CallProcedure()) == "CallProcedure");
}

TEST_CASE("ast Phase 2 statements hold their fields", "[ast][phase2]") {
    ast::Grant g;
    g.privileges = {"DIKHAO", "DAALO"};
    g.table = "students";
    g.user = "ravi";
    REQUIRE(g.privileges.size() == 2);

    ast::CreateProcedure p;
    p.name = "raise";
    p.params.push_back({"pct", "INT"});
    p.bodyText = "BADLO t RAKHO x = 1;";
    REQUIRE(p.params[0].typeName == "INT");
}
```

- [ ] **Step 2: Verify it fails**

```bash
cmake --build cpp/build
```
Expected: FAIL — `CreateUser` etc. and `astClassName` are undeclared.

- [ ] **Step 3: Add the AST structs**

Append to `cpp/include/meradb/ast.h`, just before the closing
`}  // namespace meradb::ast`:

```cpp
// ---- users & privileges (Phase 2) ----
struct CreateUser : Statement { std::string name; std::string password; };
struct DropUser : Statement { std::string name; };
struct Grant : Statement {
    std::vector<std::string> privileges;  // DIKHAO | DAALO | BADLO | MITAO (SAB is expanded by the parser)
    std::string table;                    // a table OR a view name
    std::string user;
};
struct Revoke : Statement {
    std::vector<std::string> privileges;
    std::string table;
    std::string user;
};

// ---- triggers & stored procedures (Phase 2) ----
struct CreateTrigger : Statement {
    std::string name;
    std::string timing;    // PEHLE | BAAD
    std::string event;     // DAALO | BADLO | MITAO
    std::string table;
    std::string bodyText;  // raw source between SHURU and KHATAM, ending with the last ';'
};
struct DropTrigger : Statement { std::string name; };

struct ProcParam { std::string name; std::string typeName; };  // typeName is normalised (INT, TEXT, ...)
struct CreateProcedure : Statement {
    std::string name;
    std::vector<ProcParam> params;
    std::string bodyText;
};
struct DropProcedure : Statement { std::string name; };
struct CallProcedure : Statement {
    std::string name;
    std::vector<std::unique_ptr<Expr>> args;
};
```

- [ ] **Step 4: Add `astClassName`**

In `cpp/include/meradb/ast_util.h`, before the closing namespace:

```cpp
// The Python class name of a statement (type(stmt).__name__ in meradb/ast_nodes.py):
// used verbatim in the "superuser nahi hai" privilege error, so it must match
// for every statement kind. Returns "Statement" for an unknown subclass.
std::string astClassName(const ast::Statement& stmt);
```
(add `#include <string>`).

In `cpp/src/ast_util.cpp`, inside `namespace meradb`:

```cpp
std::string astClassName(const ast::Statement& stmt) {
    using namespace ast;
#define MERADB_CLASS_NAME(T) if (dynamic_cast<const T*>(&stmt)) return #T;
    MERADB_CLASS_NAME(CreateDatabase) MERADB_CLASS_NAME(DropDatabase) MERADB_CLASS_NAME(UseDatabase)
    MERADB_CLASS_NAME(ShowTables) MERADB_CLASS_NAME(Describe) MERADB_CLASS_NAME(CreateView)
    MERADB_CLASS_NAME(DropView) MERADB_CLASS_NAME(ShowViews) MERADB_CLASS_NAME(CreateTable)
    MERADB_CLASS_NAME(AlterAddComposite) MERADB_CLASS_NAME(DropTable) MERADB_CLASS_NAME(AlterAddColumn)
    MERADB_CLASS_NAME(AlterDropColumn) MERADB_CLASS_NAME(RenameTable) MERADB_CLASS_NAME(RenameColumn)
    MERADB_CLASS_NAME(TruncateTable) MERADB_CLASS_NAME(CompactTable) MERADB_CLASS_NAME(Begin)
    MERADB_CLASS_NAME(Commit) MERADB_CLASS_NAME(Rollback) MERADB_CLASS_NAME(Explain)
    MERADB_CLASS_NAME(Insert) MERADB_CLASS_NAME(Select) MERADB_CLASS_NAME(SetOp)
    MERADB_CLASS_NAME(Update) MERADB_CLASS_NAME(Delete) MERADB_CLASS_NAME(CreateUser)
    MERADB_CLASS_NAME(DropUser) MERADB_CLASS_NAME(Grant) MERADB_CLASS_NAME(Revoke)
    MERADB_CLASS_NAME(CreateTrigger) MERADB_CLASS_NAME(DropTrigger) MERADB_CLASS_NAME(CreateProcedure)
    MERADB_CLASS_NAME(DropProcedure) MERADB_CLASS_NAME(CallProcedure)
#undef MERADB_CLASS_NAME
    return "Statement";
}
```

- [ ] **Step 5: Build and run the tests**

```bash
cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R "ast"
```
Expected: PASS (all `ast` tests including the two new ones).

- [ ] **Step 6: Commit**

```bash
git add cpp/include/meradb/ast.h cpp/include/meradb/ast_util.h cpp/src/ast_util.cpp cpp/tests/test_ast.cpp
git commit -m "Add Phase 2 AST nodes and astClassName"
```

**Completion checklist:**
- [ ] all 34 statement kinds map to their Python class name (the test lists them)
- [ ] no engine or parser change in this task

---

### Task 3: Parser — users, privileges, `CHALAO`, `HATAO` variants

**Files:**
- Modify: `cpp/include/meradb/parser.h` (declare helpers)
- Modify: `cpp/src/parser.cpp` (dispatch entries, `parseBanao`, `parseHatao`, new functions)
- Test: `cpp/tests/test_parser_phase2.cpp` (new; Task 4 appends to it)
- Modify: `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 2's AST nodes; `Parser::expectKeyword/expectIdent/matchKeyword`.
- Produces: `parseScript` now understands `BANAO USER n GUPT 'pw'`,
  `HATAO USER|TRIGGER|PROCEDURE n`, `ADHIKAR DO privs PAR t KO u`,
  `ADHIKAR WAPAS privs PAR t SE u` (`SAB` = all four, in the order DIKHAO,
  DAALO, BADLO, MITAO) and `CHALAO name(expr, ...)`. Error wording is
  Python's (`meradb/parser.py` lines 143-157, 369-432).

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_parser_phase2.cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "meradb/errors.h"
#include "meradb/parser.h"

using namespace meradb;
using Catch::Matchers::ContainsSubstring;

namespace {
template <typename T>
const T& as(const std::unique_ptr<ast::Statement>& s) {
    auto* p = dynamic_cast<const T*>(s.get());
    REQUIRE(p != nullptr);
    return *p;
}
}  // namespace

TEST_CASE("parser_phase2 BANAO USER and HATAO USER", "[parser][phase2]") {
    auto stmts = parseScript("BANAO USER ravi GUPT 'se cret'; HATAO USER ravi;");
    REQUIRE(stmts.size() == 2);
    auto& create = as<ast::CreateUser>(stmts[0]);
    CHECK(create.name == "ravi");
    CHECK(create.password == "se cret");
    CHECK(as<ast::DropUser>(stmts[1]).name == "ravi");
}

TEST_CASE("parser_phase2 BANAO USER errors use Python's wording", "[parser][phase2]") {
    REQUIRE_THROWS_WITH(parseScript("BANAO USER ravi;"),
                        "[Parser Galti] 'GUPT' expected tha, par ';' mila (line 1, col 16)");
    REQUIRE_THROWS_WITH(parseScript("BANAO USER ravi GUPT secret;"),
                        "[Parser Galti] password (quotes ke andar ek string) expected tha, par 'secret' mila (line 1, col 22)");
    REQUIRE_THROWS_WITH(parseScript("BANAO USER GUPT 'x';"), ContainsSubstring("user ka naam expected tha"));
}

TEST_CASE("parser_phase2 GRANT and REVOKE", "[parser][phase2]") {
    auto stmts = parseScript(
        "ADHIKAR DO DIKHAO, DAALO PAR students KO ravi;"
        "ADHIKAR DO SAB PAR students KO asha;"
        "ADHIKAR WAPAS DAALO PAR students SE ravi;");
    REQUIRE(stmts.size() == 3);
    auto& grant = as<ast::Grant>(stmts[0]);
    CHECK(grant.privileges == std::vector<std::string>{"DIKHAO", "DAALO"});
    CHECK(grant.table == "students");
    CHECK(grant.user == "ravi");
    CHECK(as<ast::Grant>(stmts[1]).privileges == std::vector<std::string>{"DIKHAO", "DAALO", "BADLO", "MITAO"});
    auto& revoke = as<ast::Revoke>(stmts[2]);
    CHECK(revoke.privileges == std::vector<std::string>{"DAALO"});
    CHECK(revoke.user == "ravi");
}

TEST_CASE("parser_phase2 ADHIKAR errors", "[parser][phase2]") {
    REQUIRE_THROWS_WITH(parseScript("ADHIKAR;"),
                        "[Parser Galti] ADHIKAR ke baad DO (grant) ya WAPAS (revoke) expected tha, par ';' mila (line 1, col 8)");
    REQUIRE_THROWS_WITH(parseScript("ADHIKAR DO CHALAO PAR t KO u;"),
                        "[Parser Galti] Adhikar ka naam expected tha (DIKHAO, DAALO, BADLO, MITAO, ya SAB), par 'CHALAO' mila (line 1, col 12)");
    REQUIRE_THROWS_WITH(parseScript("ADHIKAR DO DIKHAO PAR t SE u;"), ContainsSubstring("'KO' expected tha"));
    REQUIRE_THROWS_WITH(parseScript("ADHIKAR WAPAS DIKHAO PAR t KO u;"), ContainsSubstring("'SE' expected tha"));
}

TEST_CASE("parser_phase2 HATAO TRIGGER and HATAO PROCEDURE", "[parser][phase2]") {
    auto stmts = parseScript("HATAO TRIGGER audit_it; HATAO PROCEDURE badhao;");
    CHECK(as<ast::DropTrigger>(stmts[0]).name == "audit_it");
    CHECK(as<ast::DropProcedure>(stmts[1]).name == "badhao");
}

TEST_CASE("parser_phase2 CHALAO parses arguments as expressions", "[parser][phase2]") {
    auto stmts = parseScript("CHALAO badhao(10, 'CS', 2 + 3); CHALAO nothing();");
    auto& call = as<ast::CallProcedure>(stmts[0]);
    CHECK(call.name == "badhao");
    REQUIRE(call.args.size() == 3);
    CHECK(dynamic_cast<const ast::Literal*>(call.args[0].get()) != nullptr);
    CHECK(dynamic_cast<const ast::BinaryOp*>(call.args[2].get()) != nullptr);
    CHECK(as<ast::CallProcedure>(stmts[1]).args.empty());
    REQUIRE_THROWS_WITH(parseScript("CHALAO badhao 10;"), ContainsSubstring("'(' expected tha"));
}
```

- [ ] **Step 2: Add to the build and verify it fails**

Add `test_parser_phase2.cpp` to `cpp/tests/CMakeLists.txt`. Run
`cmake --build cpp/build && ctest --test-dir cpp/build -R parser_phase2`.
Expected: FAIL — `BANAO USER` currently raises `'TABLE' expected tha`.

- [ ] **Step 3: Declare the helpers** in `cpp/include/meradb/parser.h`, in the
`private:` section after `parseHatao();`:

```cpp
    // Phase 2: users / privileges / procedure calls
    std::unique_ptr<ast::Statement> parseAdhikar();
    std::unique_ptr<ast::Statement> parseChalao();
    std::vector<std::string> parsePrivilegeList();
    std::string expectPrivilege();
    std::string expectString(const std::string& what);
```

- [ ] **Step 4: Implement**

In `cpp/src/parser.cpp`, `parseStatement`: add the two dispatch lines after the
`SAMJHAO` block, before `error("Ye command nahi pata")`:

```cpp
    if (kw == "ADHIKAR") { advance(); return parseAdhikar(); }
    if (kw == "CHALAO") { advance(); return parseChalao(); }
```

In `parseBanao`, after the `VIEW` line and before `expectKeyword("TABLE")`:

```cpp
    // BANAO USER naam GUPT 'password'
    if (matchKeyword("USER")) {
        auto u = std::make_unique<CreateUser>();
        u->name = expectIdent("user ka naam");
        expectKeyword("GUPT");
        u->password = expectString("password");
        return u;
    }
```

In `parseHatao`, after the `VIEW` block and before `expectKeyword("TABLE")`:

```cpp
    if (matchKeyword("USER")) {
        auto d = std::make_unique<DropUser>();
        d->name = expectIdent("user ka naam");
        return d;
    }
    if (matchKeyword("TRIGGER")) {
        auto d = std::make_unique<DropTrigger>();
        d->name = expectIdent("trigger ka naam");
        return d;
    }
    if (matchKeyword("PROCEDURE")) {
        auto d = std::make_unique<DropProcedure>();
        d->name = expectIdent("procedure ka naam");
        return d;
    }
```

New functions (after `parseHatao`'s definition ends):

```cpp
std::string Parser::expectString(const std::string& what) {
    if (peek().type != TokenType::String) error(what + " (quotes ke andar ek string) expected tha");
    return advance().textValue;
}

std::string Parser::expectPrivilege() {
    static const char* const kPrivileges[] = {"DIKHAO", "DAALO", "BADLO", "MITAO"};
    if (peek().type == TokenType::Keyword) {
        for (const char* p : kPrivileges) {
            if (peek().textValue == p) {
                advance();
                return p;
            }
        }
    }
    error("Adhikar ka naam expected tha (DIKHAO, DAALO, BADLO, MITAO, ya SAB)");
}

std::vector<std::string> Parser::parsePrivilegeList() {
    if (matchKeyword("SAB")) return {"DIKHAO", "DAALO", "BADLO", "MITAO"};
    std::vector<std::string> privileges;
    privileges.push_back(expectPrivilege());
    while (matchSymbol(",")) privileges.push_back(expectPrivilege());
    return privileges;
}

std::unique_ptr<Statement> Parser::parseAdhikar() {
    // ADHIKAR DO priv, priv PAR table KO user      -- GRANT
    // ADHIKAR WAPAS priv, priv PAR table SE user    -- REVOKE
    if (matchKeyword("DO")) {
        auto g = std::make_unique<Grant>();
        g->privileges = parsePrivilegeList();
        expectKeyword("PAR");
        g->table = expectIdent("table/view ka naam");
        expectKeyword("KO");
        g->user = expectIdent("user ka naam");
        return g;
    }
    if (matchKeyword("WAPAS")) {
        auto r = std::make_unique<Revoke>();
        r->privileges = parsePrivilegeList();
        expectKeyword("PAR");
        r->table = expectIdent("table/view ka naam");
        expectKeyword("SE");
        r->user = expectIdent("user ka naam");
        return r;
    }
    error("ADHIKAR ke baad DO (grant) ya WAPAS (revoke) expected tha");
}

std::unique_ptr<Statement> Parser::parseChalao() {
    // CHALAO naam(expr, expr, ...)
    auto call = std::make_unique<CallProcedure>();
    call->name = expectIdent("procedure ka naam");
    expectSymbol("(");
    if (!checkSymbol(")")) {
        call->args.push_back(parseExpressionEntry());
        while (matchSymbol(",")) call->args.push_back(parseExpressionEntry());
    }
    expectSymbol(")");
    return call;
}
```

- [ ] **Step 5: Build and run**

```bash
cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R "parser"
```
Expected: PASS (all parser tests, old and new).

- [ ] **Step 6: Commit**

```bash
git add cpp/include/meradb/parser.h cpp/src/parser.cpp cpp/tests/test_parser_phase2.cpp cpp/tests/CMakeLists.txt
git commit -m "Parse users, privileges, CHALAO and HATAO variants"
```

**Completion checklist:**
- [ ] error strings copied from `meradb/parser.py`, positions verified by the exact-match tests
- [ ] `SAB` expands in Python's order
- [ ] no engine change yet — an engine given these statements still says "abhi supported nahi hai"

---

### Task 4: Parser — `SHURU ... KHATAM` bodies, `BANAO TRIGGER`, `BANAO PROCEDURE`

**Files:**
- Modify: `cpp/include/meradb/parser.h`, `cpp/src/parser.cpp`
- Test: `cpp/tests/test_parser_phase2.cpp` (append)

**Interfaces:**
- Consumes: Task 3's parser; `normalizeType` (datatypes.h); `Parser::sourceText_`.
- Produces: `CreateTrigger` / `CreateProcedure` AST nodes whose `bodyText` is
  the raw source from the first token after `SHURU` to the end of the last
  `;` before `KHATAM` — exactly Python's `self.text[start:end]` — and whose body
  has been parse-validated (a typo fails at CREATE time).

- [ ] **Step 1: Write the failing test** (append to `test_parser_phase2.cpp`)

```cpp
TEST_CASE("parser_phase2 BANAO TRIGGER captures the raw body text", "[parser][phase2]") {
    auto stmts = parseScript(
        "BANAO TRIGGER note_it BAAD DAALO PAR accounts SHURU\n"
        "  DAALO MEIN audit_log MAAN (NAYA.id, 'new');\n"
        "  BADLO counters RAKHO n = n + 1;\n"
        "KHATAM;");
    REQUIRE(stmts.size() == 1);
    auto& t = as<ast::CreateTrigger>(stmts[0]);
    CHECK(t.name == "note_it");
    CHECK(t.timing == "BAAD");
    CHECK(t.event == "DAALO");
    CHECK(t.table == "accounts");
    // starts at the first body token, ends right after the last ';' (no leading/trailing whitespace)
    CHECK(t.bodyText == "DAALO MEIN audit_log MAAN (NAYA.id, 'new');\n  BADLO counters RAKHO n = n + 1;");
}

TEST_CASE("parser_phase2 trigger timing and event alternatives", "[parser][phase2]") {
    auto stmts = parseScript(
        "BANAO TRIGGER a PEHLE BADLO PAR t SHURU DIKHAO * SE t; KHATAM;"
        "BANAO TRIGGER b PEHLE MITAO PAR t SHURU DIKHAO * SE t; KHATAM;");
    CHECK(as<ast::CreateTrigger>(stmts[0]).timing == "PEHLE");
    CHECK(as<ast::CreateTrigger>(stmts[0]).event == "BADLO");
    CHECK(as<ast::CreateTrigger>(stmts[1]).event == "MITAO");
}

TEST_CASE("parser_phase2 BANAO TRIGGER errors", "[parser][phase2]") {
    REQUIRE_THROWS_WITH(parseScript("BANAO TRIGGER x DAALO PAR t SHURU DIKHAO * SE t; KHATAM;"),
                        ContainsSubstring("BANAO TRIGGER naam ke baad PEHLE ya BAAD expected tha"));
    REQUIRE_THROWS_WITH(parseScript("BANAO TRIGGER x PEHLE SAAF PAR t SHURU DIKHAO * SE t; KHATAM;"),
                        ContainsSubstring("PEHLE/BAAD ke baad DAALO, BADLO ya MITAO expected tha"));
    REQUIRE_THROWS_WITH(parseScript("BANAO TRIGGER x PEHLE DAALO t SHURU DIKHAO * SE t; KHATAM;"),
                        ContainsSubstring("'PAR' expected tha"));
    REQUIRE_THROWS_WITH(parseScript("BANAO TRIGGER x PEHLE DAALO PAR t SHURU KHATAM;"),
                        ContainsSubstring("TRIGGER ke SHURU...KHATAM ke andar kam se kam ek statement chahiye"));
    REQUIRE_THROWS_WITH(parseScript("BANAO TRIGGER x PEHLE DAALO PAR t SHURU DIKHAO * SE t;"),
                        ContainsSubstring("TRIGGER ka SHURU...KHATAM band nahi hua (KHATAM missing)"));
    // a typo inside the body is caught at CREATE time
    REQUIRE_THROWS_WITH(parseScript("BANAO TRIGGER x PEHLE DAALO PAR t SHURU DAALO ; KHATAM;"),
                        ContainsSubstring("expected tha"));
    // every statement in the body needs its own ';'
    REQUIRE_THROWS_WITH(parseScript("BANAO TRIGGER x PEHLE DAALO PAR t SHURU DIKHAO * SE t KHATAM;"),
                        ContainsSubstring("';' expected tha"));
}

TEST_CASE("parser_phase2 BANAO PROCEDURE with parameters", "[parser][phase2]") {
    auto stmts = parseScript(
        "BANAO PROCEDURE badhao(dept TEXT, pct ANK) SHURU\n"
        "  BADLO emp RAKHO salary = salary + pct JAHAN d = dept;\n"
        "KHATAM;"
        "BANAO PROCEDURE kuch() SHURU DIKHAO * SE t; KHATAM;");
    auto& p = as<ast::CreateProcedure>(stmts[0]);
    CHECK(p.name == "badhao");
    REQUIRE(p.params.size() == 2);
    CHECK(p.params[0].name == "dept");
    CHECK(p.params[0].typeName == "TEXT");
    CHECK(p.params[1].name == "pct");
    CHECK(p.params[1].typeName == "INT");  // ANK is an alias, normalised at parse time
    CHECK(p.bodyText == "BADLO emp RAKHO salary = salary + pct JAHAN d = dept;");
    CHECK(as<ast::CreateProcedure>(stmts[1]).params.empty());
}

TEST_CASE("parser_phase2 BANAO PROCEDURE errors", "[parser][phase2]") {
    REQUIRE_THROWS_WITH(parseScript("BANAO PROCEDURE p(x nonsense) SHURU DIKHAO * SE t; KHATAM;"),
                        ContainsSubstring("Parameter 'x' ka type expected tha (INT/ANK, FLOAT, TEXT/SHABD, BOOL, DATE/TAREEKH, ...)"));
    REQUIRE_THROWS_WITH(parseScript("BANAO PROCEDURE p(x INT SHURU DIKHAO * SE t; KHATAM;"), ContainsSubstring("')' expected tha"));
    REQUIRE_THROWS_WITH(parseScript("BANAO PROCEDURE p() SHURU KHATAM;"),
                        ContainsSubstring("PROCEDURE ke SHURU...KHATAM ke andar kam se kam ek statement chahiye"));
}
```

- [ ] **Step 2: Verify it fails**

```bash
cmake --build cpp/build && ctest --test-dir cpp/build -R parser_phase2
```
Expected: FAIL — `BANAO TRIGGER` gives `'TABLE' expected tha`.

- [ ] **Step 3: Declare** in `parser.h` next to Task 3's declarations:

```cpp
    std::string parseBlockBody(const std::string& what);
    std::unique_ptr<ast::Statement> parseCreateTrigger();
    std::unique_ptr<ast::Statement> parseCreateProcedure();
    ast::ProcParam parseProcParam();
```

- [ ] **Step 4: Implement** in `parser.cpp`. Dispatch in `parseBanao`, after the
`USER` block from Task 3:

```cpp
    // BANAO TRIGGER naam PEHLE|BAAD DAALO|BADLO|MITAO PAR table SHURU ... KHATAM
    if (matchKeyword("TRIGGER")) return parseCreateTrigger();
    // BANAO PROCEDURE naam (p1 TYPE, ...) SHURU ... KHATAM
    if (matchKeyword("PROCEDURE")) return parseCreateProcedure();
```

New functions:

```cpp
// SHURU stmt; stmt; ... KHATAM -- captures the RAW SOURCE TEXT between SHURU and
// KHATAM ("store source text, reparse fresh later", like BANAO VIEW), and
// parse-validates every statement NOW. Mirrors Parser._parse_block_body.
std::string Parser::parseBlockBody(const std::string& what) {
    expectKeyword("SHURU");
    size_t start = peek().start < 0 ? 0 : static_cast<size_t>(peek().start);
    size_t statementCount = 0;
    while (!checkKeyword("KHATAM")) {
        if (peek().type == TokenType::Eof) error(what + " ka SHURU...KHATAM band nahi hua (KHATAM missing)");
        auto body = parseStatement();
        (void)body;  // only validated here; the text is stored and re-parsed when used
        ++statementCount;
        expectSymbol(";");
    }
    if (statementCount == 0) error(what + " ke SHURU...KHATAM ke andar kam se kam ek statement chahiye");
    size_t end = static_cast<size_t>(peek(-1).end);  // end of the last ';'
    std::string bodyText = sourceText_.substr(start, end - start);
    expectKeyword("KHATAM");
    return bodyText;
}

std::unique_ptr<Statement> Parser::parseCreateTrigger() {
    auto t = std::make_unique<CreateTrigger>();
    t->name = expectIdent("trigger ka naam");
    if (matchKeyword("PEHLE")) t->timing = "PEHLE";
    else if (matchKeyword("BAAD")) t->timing = "BAAD";
    else error("BANAO TRIGGER naam ke baad PEHLE ya BAAD expected tha");
    for (const char* kw : {"DAALO", "BADLO", "MITAO"}) {
        if (matchKeyword(kw)) {
            t->event = kw;
            break;
        }
    }
    if (t->event.empty()) error("PEHLE/BAAD ke baad DAALO, BADLO ya MITAO expected tha");
    expectKeyword("PAR");
    t->table = expectIdent("table ka naam");
    t->bodyText = parseBlockBody("TRIGGER");
    return t;
}

ProcParam Parser::parseProcParam() {
    ProcParam param;
    param.name = expectIdent("parameter ka naam");
    std::optional<std::string> typeName;
    if (peek().type == TokenType::Ident) typeName = normalizeType(peek().textValue);
    if (!typeName) {
        error("Parameter '" + param.name + "' ka type expected tha (INT/ANK, FLOAT, TEXT/SHABD, BOOL, DATE/TAREEKH, ...)");
    }
    advance();
    param.typeName = *typeName;
    return param;
}

std::unique_ptr<Statement> Parser::parseCreateProcedure() {
    auto p = std::make_unique<CreateProcedure>();
    p->name = expectIdent("procedure ka naam");
    expectSymbol("(");
    if (!checkSymbol(")")) {
        p->params.push_back(parseProcParam());
        while (matchSymbol(",")) p->params.push_back(parseProcParam());
    }
    expectSymbol(")");
    p->bodyText = parseBlockBody("PROCEDURE");
    return p;
}
```

Note: `parseBlockBody` reads `sourceText_`; `parseScript(text)` already passes the
text to `Parser`. A `Parser` built without source text (`Parser(tokens)`) would
yield an empty body — every production caller passes it.

- [ ] **Step 5: Build and run**

```bash
cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R "parser"
```
Expected: PASS.

- [ ] **Step 6: Cross-check two error strings against Python** (cheap oracle
check, no code change):

```bash
python -c "from meradb.parser import parse; 
import sys
for q in ['BANAO TRIGGER x PEHLE DAALO PAR t SHURU KHATAM;','BANAO PROCEDURE p(x nonsense) SHURU DIKHAO * SE t; KHATAM;']:
    try: parse(q)
    except Exception as e: print(e)"
```
Expected: the messages contain the same text as the two `ContainsSubstring`
checks above. If a message differs, the Python text wins — fix the C++ string.

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/parser.h cpp/src/parser.cpp cpp/tests/test_parser_phase2.cpp
git commit -m "Parse BANAO TRIGGER and BANAO PROCEDURE with stored bodies"
```

**Completion checklist:**
- [ ] body text is byte-identical to Python's slice (start of first token .. end of last `;`)
- [ ] bodies are parse-validated (typo and missing `;` rejected)
- [ ] `SHURU` inside a body parses as a `Begin` statement, like Python (not an error at parse time)

---

### Task 5: Crypto — SHA-256, HMAC, PBKDF2

**Files:**
- Create: `cpp/include/meradb/crypto.h`, `cpp/src/crypto.cpp`
- Test: `cpp/tests/test_crypto.cpp`
- Modify: `cpp/CMakeLists.txt` (source + `-O2` for this one file), `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `StorageError` (for `fromHex`).
- Produces (`namespace meradb::crypto`): `Bytes` (`std::vector<uint8_t>`),
  `class Sha256` (incremental; copyable so a copy is a "midstate"),
  `class HmacSha256` (key schedule done once, `mac()` reuses the ipad/opad
  midstates), `sha256`, `hmacSha256`, `pbkdf2HmacSha256(password, salt,
  iterations, keyLength)`, `toHex` (lowercase), `fromHex`, `toBytes`.
  `UserStore` (Task 6) calls `pbkdf2HmacSha256(pw, salt, 100000, 32)`, which
  must equal Python's `hashlib.pbkdf2_hmac("sha256", pw.encode(), salt, 100000)`.

Why written here and not taken from a library: see Design decision D4 (no new
FetchContent dependency, and PBKDF2 at 100,000 iterations needs midstate reuse
to stay fast).

- [ ] **Step 1: Write the failing test**

The expected values below were produced by Python's `hashlib` / `hmac`
(FIPS 180-4 and RFC 4231 / RFC 6070-style vectors, RFC 7914 for the 64-byte one).

```cpp
// cpp/tests/test_crypto.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/crypto.h"
#include "meradb/errors.h"

using namespace meradb;
using namespace meradb::crypto;

TEST_CASE("crypto sha256 known answers", "[crypto]") {
    CHECK(toHex(sha256(std::string(""))) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(toHex(sha256(std::string("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(toHex(sha256(std::string("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("crypto sha256 streaming equals one-shot at every split", "[crypto]") {
    std::string data(200, 'a');
    const std::string expected = "c2a908d98f5df987ade41b5fce213067efbcc21ef2240212a41e54b5e7c28ae5";
    CHECK(toHex(sha256(data)) == expected);
    for (size_t split : {1u, 55u, 56u, 63u, 64u, 65u, 128u, 199u}) {
        Sha256 h;
        h.update(reinterpret_cast<const uint8_t*>(data.data()), split);
        h.update(reinterpret_cast<const uint8_t*>(data.data()) + split, data.size() - split);
        CHECK(toHex(h.finish()) == expected);
    }
}

TEST_CASE("crypto hmac-sha256 RFC 4231 vectors", "[crypto]") {
    Bytes key1(20, 0x0b);
    CHECK(toHex(hmacSha256(key1, toBytes("Hi There"))) ==
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    CHECK(toHex(hmacSha256(toBytes("Jefe"), toBytes("what do ya want for nothing?"))) ==
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    // a key longer than the 64-byte block is hashed first
    Bytes longKey(131, 0xaa);
    Bytes shortKey = sha256(longKey);
    CHECK(hmacSha256(longKey, toBytes("x")) == hmacSha256(shortKey, toBytes("x")));
}

TEST_CASE("crypto pbkdf2-hmac-sha256 known answers", "[crypto]") {
    Bytes salt = toBytes("salt");
    CHECK(toHex(pbkdf2HmacSha256("password", salt, 1, 32)) ==
          "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b");
    CHECK(toHex(pbkdf2HmacSha256("password", salt, 2, 32)) ==
          "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43");
    CHECK(toHex(pbkdf2HmacSha256("password", salt, 4096, 32)) ==
          "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a");
    // two output blocks (dkLen = 64), RFC 7914 section 11
    CHECK(toHex(pbkdf2HmacSha256("passwd", salt, 1, 64)) ==
          "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
          "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
}

TEST_CASE("crypto pbkdf2 matches Python at MeraDB's 100000 iterations", "[crypto]") {
    Bytes salt;
    for (uint8_t i = 0; i < 16; ++i) salt.push_back(i);
    CHECK(toHex(pbkdf2HmacSha256("pw", salt, 100000, 32)) ==
          "fbe79903d759b826e1d54bd9e4f8beb2de5a87ac86037c76328ad485fd894671");
    // UTF-8 bytes of the password are hashed, like password.encode("utf-8")
    CHECK(toHex(pbkdf2HmacSha256("p\xC3\xA4ssw\xC3\xB6rd", salt, 100000, 32)) ==
          "b6b9a7d7879a26a7ad99c592249c145952ca50ec9fefae618844337989ef6a53");
}

TEST_CASE("crypto hex helpers round-trip and validate", "[crypto]") {
    Bytes b = {0x00, 0x0f, 0xa5, 0xff};
    CHECK(toHex(b) == "000fa5ff");
    CHECK(fromHex("000fa5ff") == b);
    CHECK(fromHex("000FA5FF") == b);
    CHECK_THROWS_AS(fromHex("abc"), StorageError);   // odd length
    CHECK_THROWS_AS(fromHex("zz"), StorageError);    // not hex
}
```

- [ ] **Step 2: Add to the build and verify it fails**

Add `test_crypto.cpp` to `cpp/tests/CMakeLists.txt`.
`cmake --build cpp/build` — Expected: FAIL, `meradb/crypto.h` does not exist.

- [ ] **Step 3: Write the header**

```cpp
// cpp/include/meradb/crypto.h
//
// SHA-256, HMAC-SHA-256 and PBKDF2-HMAC-SHA-256, written out in full so the
// password store needs no third-party library. Byte-compatible with Python's
// hashlib (verified by known-answer tests). Educational, not hardened: no
// constant-time guarantees beyond what a plain implementation gives.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace meradb::crypto {

using Bytes = std::vector<std::uint8_t>;

class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const std::uint8_t* data, std::size_t length);
    void update(const Bytes& data) { update(data.data(), data.size()); }
    // The 32-byte digest. The object must be reset() before it is used again.
    Bytes finish();

private:
    std::uint32_t h_[8];
    std::uint8_t buffer_[64];
    std::size_t bufferLength_ = 0;
    std::uint64_t totalLength_ = 0;
    void compress(const std::uint8_t* block);
};

// HMAC with the key schedule done once: mac() copies the two saved midstates,
// so PBKDF2's 100,000 iterations cost two compression calls each.
class HmacSha256 {
public:
    explicit HmacSha256(const Bytes& key);
    Bytes mac(const std::uint8_t* message, std::size_t length) const;
    Bytes mac(const Bytes& message) const { return mac(message.data(), message.size()); }

private:
    Sha256 inner_;  // state after absorbing key XOR 0x36
    Sha256 outer_;  // state after absorbing key XOR 0x5c
};

Bytes sha256(const Bytes& data);
Bytes sha256(const std::string& data);
Bytes hmacSha256(const Bytes& key, const Bytes& message);
// Python: hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, iterations, keyLength)
Bytes pbkdf2HmacSha256(const std::string& password, const Bytes& salt, std::uint32_t iterations,
                       std::size_t keyLength);

Bytes toBytes(const std::string& text);  // the raw bytes of a std::string (UTF-8 stays UTF-8)
std::string toHex(const Bytes& bytes);   // lowercase, like bytes.hex()
Bytes fromHex(const std::string& hex);   // either case; StorageError if malformed

}  // namespace meradb::crypto
```

- [ ] **Step 4: Write the implementation**

```cpp
// cpp/src/crypto.cpp
#include "meradb/crypto.h"
#include "meradb/errors.h"
#include <algorithm>
#include <cstring>

namespace meradb::crypto {

namespace {

const std::uint32_t kRoundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline std::uint32_t rotr(std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

void Sha256::reset() {
    static const std::uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::copy(init, init + 8, h_);
    bufferLength_ = 0;
    totalLength_ = 0;
}

void Sha256::compress(const std::uint8_t* block) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[4 * i]) << 24) | (static_cast<std::uint32_t>(block[4 * i + 1]) << 16) |
               (static_cast<std::uint32_t>(block[4 * i + 2]) << 8) | static_cast<std::uint32_t>(block[4 * i + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t bigS1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        std::uint32_t ch = (e & f) ^ (~e & g);
        std::uint32_t t1 = h + bigS1 + ch + kRoundConstants[i] + w[i];
        std::uint32_t bigS0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t t2 = bigS0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += h;
}

void Sha256::update(const std::uint8_t* data, std::size_t length) {
    totalLength_ += length;
    if (bufferLength_ > 0) {
        std::size_t take = std::min(length, sizeof buffer_ - bufferLength_);
        std::memcpy(buffer_ + bufferLength_, data, take);
        bufferLength_ += take;
        data += take;
        length -= take;
        if (bufferLength_ < sizeof buffer_) return;
        compress(buffer_);
        bufferLength_ = 0;
    }
    while (length >= 64) {
        compress(data);
        data += 64;
        length -= 64;
    }
    if (length > 0) {
        std::memcpy(buffer_, data, length);
        bufferLength_ = length;
    }
}

Bytes Sha256::finish() {
    const std::uint64_t bits = totalLength_ * 8;
    std::uint8_t padding[72] = {0x80};
    std::size_t paddingLength = bufferLength_ < 56 ? 56 - bufferLength_ : 120 - bufferLength_;
    update(padding, paddingLength);
    std::uint8_t lengthBytes[8];
    for (int i = 0; i < 8; ++i) lengthBytes[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
    update(lengthBytes, 8);
    Bytes out(32);
    for (int i = 0; i < 8; ++i) {
        out[4 * i] = static_cast<std::uint8_t>(h_[i] >> 24);
        out[4 * i + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
        out[4 * i + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
        out[4 * i + 3] = static_cast<std::uint8_t>(h_[i]);
    }
    return out;
}

HmacSha256::HmacSha256(const Bytes& key) {
    Bytes block = key.size() > 64 ? sha256(key) : key;
    block.resize(64, 0);
    Bytes ipad(64), opad(64);
    for (std::size_t i = 0; i < 64; ++i) {
        ipad[i] = static_cast<std::uint8_t>(block[i] ^ 0x36);
        opad[i] = static_cast<std::uint8_t>(block[i] ^ 0x5c);
    }
    inner_.update(ipad);
    outer_.update(opad);
}

Bytes HmacSha256::mac(const std::uint8_t* message, std::size_t length) const {
    Sha256 inner = inner_;  // copy = start from the midstate
    inner.update(message, length);
    Bytes innerDigest = inner.finish();
    Sha256 outer = outer_;
    outer.update(innerDigest);
    return outer.finish();
}

Bytes sha256(const Bytes& data) {
    Sha256 h;
    h.update(data);
    return h.finish();
}

Bytes sha256(const std::string& data) { return sha256(toBytes(data)); }

Bytes hmacSha256(const Bytes& key, const Bytes& message) { return HmacSha256(key).mac(message); }

Bytes pbkdf2HmacSha256(const std::string& password, const Bytes& salt, std::uint32_t iterations,
                       std::size_t keyLength) {
    if (iterations == 0) throw StorageError("PBKDF2: iterations kam se kam 1 chahiye");
    HmacSha256 prf(toBytes(password));
    Bytes out;
    out.reserve(keyLength);
    for (std::uint32_t block = 1; out.size() < keyLength; ++block) {
        Bytes message = salt;
        message.push_back(static_cast<std::uint8_t>(block >> 24));
        message.push_back(static_cast<std::uint8_t>(block >> 16));
        message.push_back(static_cast<std::uint8_t>(block >> 8));
        message.push_back(static_cast<std::uint8_t>(block));
        Bytes u = prf.mac(message);
        Bytes t = u;
        for (std::uint32_t i = 1; i < iterations; ++i) {
            u = prf.mac(u);
            for (std::size_t j = 0; j < t.size(); ++j) t[j] ^= u[j];
        }
        std::size_t take = std::min<std::size_t>(t.size(), keyLength - out.size());
        out.insert(out.end(), t.begin(), t.begin() + static_cast<std::ptrdiff_t>(take));
    }
    return out;
}

Bytes toBytes(const std::string& text) { return Bytes(text.begin(), text.end()); }

std::string toHex(const Bytes& bytes) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 0x0f];
    }
    return out;
}

Bytes fromHex(const std::string& hex) {
    if (hex.size() % 2 != 0) throw StorageError("hex string ki length odd hai");
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        throw StorageError("hex string mein galat character hai");
    };
    Bytes out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2)
        out.push_back(static_cast<std::uint8_t>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
    return out;
}

}  // namespace meradb::crypto
```

- [ ] **Step 5: CMake**

In `cpp/CMakeLists.txt` add `src/crypto.cpp` to `meradb_core` and, after the
`add_library`, keep PBKDF2 fast even in an unoptimised default build:

```cmake
if(NOT MSVC)
  # 100,000 PBKDF2 iterations = 200,000 SHA-256 compressions per login; -O0 is ~10x too slow.
  set_source_files_properties(src/crypto.cpp PROPERTIES COMPILE_OPTIONS "-O2")
endif()
```

- [ ] **Step 6: Build and run**

```bash
cmake -S cpp -B cpp/build -G "MinGW Makefiles" && cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R crypto
```
Expected: 6 tests PASS in well under a second each. If a vector disagrees,
regenerate it with Python (`hashlib.pbkdf2_hmac(...)`) — the implementation, not
the vector, is presumed wrong until Python says otherwise.

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/crypto.h cpp/src/crypto.cpp cpp/tests/test_crypto.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add SHA-256, HMAC and PBKDF2 implementations"
```

**Completion checklist:**
- [ ] all vectors from the plan pass unchanged
- [ ] no signed-shift or overflow UB (all arithmetic on `uint32_t` / `uint64_t`)
- [ ] a 100,000-iteration hash finishes in about a tenth of a second on a dev machine

---

### Task 6: `UserStore` — `users.json`

**Files:**
- Create: `cpp/include/meradb/users.h`, `cpp/src/users.cpp`
- Test: `cpp/tests/test_users.cpp`
- Modify: `cpp/CMakeLists.txt`, `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `crypto::pbkdf2HmacSha256`, `sys::randomBytes`, `ExecutionError` /
  `StorageError`.
- Produces: `class UserStore` mirroring `meradb/users.py`: `exists`, `create`,
  `drop`, `verify`, `grant`, `revoke`, `hasPrivilege`, `names`, plus
  `allPrivileges()` (`DIKHAO, DAALO, BADLO, MITAO`) and `PBKDF2_ITERATIONS`.
  Thread-safe: every method takes an internal mutex, and the 100,000-iteration
  hash is computed **outside** it so a login does not block other sessions'
  grants (the handshake calls `verify` before any engine lock is held).
  The file format is Python's exactly (Design decision D4).

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_users.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/crypto.h"
#include "meradb/errors.h"
#include "meradb/users.h"
#include "test_util.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

using namespace meradb;
using meradb_test::TempDir;
using meradb_test::readText;
namespace fs = std::filesystem;

namespace {
void writeFile(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}
}  // namespace

TEST_CASE("users create stores salt and PBKDF2 hash, never the password", "[users]") {
    TempDir dir;
    UserStore users(dir.str());
    users.create("ravi", "hunter2");
    REQUIRE(users.exists("ravi"));
    REQUIRE_FALSE(users.exists("asha"));

    std::string text = readText(dir.file("users.json"));
    CHECK(text.find("hunter2") == std::string::npos);
    auto j = nlohmann::ordered_json::parse(text);
    REQUIRE(j.contains("ravi"));
    std::string saltHex = j["ravi"]["salt"];
    std::string hashHex = j["ravi"]["hash"];
    CHECK(saltHex.size() == 32);   // 16 random bytes
    CHECK(hashHex.size() == 64);   // 32-byte digest
    CHECK(hashHex == crypto::toHex(crypto::pbkdf2HmacSha256("hunter2", crypto::fromHex(saltHex), 100000, 32)));
    CHECK(j["ravi"]["grants"].is_object());
    CHECK(j["ravi"]["grants"].empty());
}

TEST_CASE("users verify accepts the right password only", "[users]") {
    TempDir dir;
    UserStore users(dir.str());
    users.create("ravi", "hunter2");
    CHECK(users.verify("ravi", "hunter2"));
    CHECK_FALSE(users.verify("ravi", "hunter3"));
    CHECK_FALSE(users.verify("ravi", ""));
    CHECK_FALSE(users.verify("nobody", "hunter2"));
}

TEST_CASE("users duplicate and missing names use Python's wording", "[users]") {
    TempDir dir;
    UserStore users(dir.str());
    users.create("ravi", "x");
    REQUIRE_THROWS_WITH(users.create("ravi", "y"), "[Execution Galti] User 'ravi' pehle se hai");
    REQUIRE_THROWS_WITH(users.drop("ghost"), "[Execution Galti] User 'ghost' exist nahi karta");
    REQUIRE_THROWS_WITH(users.grant("ghost", "main", "t", {"DIKHAO"}), "[Execution Galti] User 'ghost' exist nahi karta");
    REQUIRE_THROWS_WITH(users.revoke("ghost", "main", "t", {"DIKHAO"}), "[Execution Galti] User 'ghost' exist nahi karta");
    users.drop("ravi");
    CHECK_FALSE(users.exists("ravi"));
}

TEST_CASE("users grants are kept in DIKHAO/DAALO/BADLO/MITAO order and revoked cleanly", "[users]") {
    TempDir dir;
    UserStore users(dir.str());
    users.create("ravi", "x");
    users.grant("ravi", "main", "students", {"MITAO", "DIKHAO"});
    users.grant("ravi", "main", "students", {"BADLO", "DIKHAO"});
    CHECK(users.hasPrivilege("ravi", "main", "students", "DIKHAO"));
    CHECK(users.hasPrivilege("ravi", "main", "students", "MITAO"));
    CHECK_FALSE(users.hasPrivilege("ravi", "main", "students", "DAALO"));
    CHECK_FALSE(users.hasPrivilege("ravi", "other", "students", "DIKHAO"));   // keyed by database too
    CHECK_FALSE(users.hasPrivilege("nobody", "main", "students", "DIKHAO"));

    auto j = nlohmann::ordered_json::parse(readText(dir.file("users.json")));
    CHECK(j["ravi"]["grants"]["main.students"] == nlohmann::ordered_json::parse(R"(["DIKHAO","BADLO","MITAO"])"));

    users.revoke("ravi", "main", "students", {"DIKHAO", "BADLO"});
    users.revoke("ravi", "main", "students", {"MITAO"});
    j = nlohmann::ordered_json::parse(readText(dir.file("users.json")));
    CHECK(j["ravi"]["grants"].empty());  // the key disappears once nothing is left
    users.revoke("ravi", "main", "students", {"DAALO"});  // revoking what was never granted is fine
}

TEST_CASE("users file has exactly Python's json.dump(indent=2) shape", "[users]") {
    TempDir dir;
    writeFile(dir.path() / "users.json",
              "{\n"
              "  \"ravi\": {\n"
              "    \"salt\": \"000102030405060708090a0b0c0d0e0f\",\n"
              "    \"hash\": \"fbe79903d759b826e1d54bd9e4f8beb2de5a87ac86037c76328ad485fd894671\",\n"
              "    \"grants\": {\n"
              "      \"main.students\": [\n"
              "        \"DIKHAO\",\n"
              "        \"DAALO\"\n"
              "      ]\n"
              "    }\n"
              "  }\n"
              "}");
    // that file was written by the Python engine for password "pw": we must accept it ...
    UserStore users(dir.str());
    CHECK(users.verify("ravi", "pw"));
    CHECK_FALSE(users.verify("ravi", "PW"));
    CHECK(users.hasPrivilege("ravi", "main", "students", "DAALO"));

    // ... and write it back byte for byte (no trailing newline, 2-space indent, insertion order)
    users.grant("ravi", "main", "students", {"DIKHAO"});  // no change in content
    CHECK(readText(dir.file("users.json")) ==
          "{\n"
          "  \"ravi\": {\n"
          "    \"salt\": \"000102030405060708090a0b0c0d0e0f\",\n"
          "    \"hash\": \"fbe79903d759b826e1d54bd9e4f8beb2de5a87ac86037c76328ad485fd894671\",\n"
          "    \"grants\": {\n"
          "      \"main.students\": [\n"
          "        \"DIKHAO\",\n"
          "        \"DAALO\"\n"
          "      ]\n"
          "    }\n"
          "  }\n"
          "}");
}

TEST_CASE("users persist across instances and non-ASCII passwords work", "[users]") {
    TempDir dir;
    {
        UserStore users(dir.str());
        users.create("asha", "p\xC3\xA4ssw\xC3\xB6rd");
    }
    UserStore again(dir.str());
    CHECK(again.exists("asha"));
    CHECK(again.verify("asha", "p\xC3\xA4ssw\xC3\xB6rd"));
    CHECK_FALSE(again.verify("asha", "passwoerd"));
    CHECK(again.names() == std::vector<std::string>{"asha"});
    // no leftover temp file
    CHECK_FALSE(fs::exists(dir.path() / "users.json.tmp"));
}

TEST_CASE("users corrupt file is a StorageError, not a crash", "[users]") {
    TempDir dir;
    writeFile(dir.path() / "users.json", "{ not json");
    REQUIRE_THROWS_AS(UserStore(dir.str()), StorageError);
}
```

- [ ] **Step 2: Add to the build and verify it fails**

Add `test_users.cpp`. `cmake --build cpp/build` — Expected: FAIL, no `users.h`.

- [ ] **Step 3: Header**

```cpp
// cpp/include/meradb/users.h
//
// USERS & PRIVILEGES: the server-wide user store. One users.json at the TOP of
// the data folder (not per database), same format as meradb/users.py:
//
//   { "ravi": { "salt": "<hex>", "hash": "<hex>", "grants": {"main.students": ["DIKHAO", "DAALO"]} } }
//
// Passwords are never stored: PBKDF2-HMAC-SHA256, 100,000 iterations, a random
// 16-byte salt per user. Every method is thread-safe.
#pragma once
#include <cstdint>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace meradb {

constexpr std::uint32_t PBKDF2_ITERATIONS = 100000;

// DIKHAO, DAALO, BADLO, MITAO -- the order privileges are stored in.
const std::vector<std::string>& allPrivileges();

class UserStore {
public:
    static constexpr const char* kFileName = "users.json";

    explicit UserStore(std::string dataDir);  // loads users.json if it exists

    bool exists(const std::string& name) const;
    void create(const std::string& name, const std::string& password);
    void drop(const std::string& name);
    bool verify(const std::string& name, const std::string& password) const;

    // Grants are keyed "database.table" (a VIEW uses the same keyspace).
    void grant(const std::string& name, const std::string& db, const std::string& table,
               const std::vector<std::string>& privileges);
    void revoke(const std::string& name, const std::string& db, const std::string& table,
                const std::vector<std::string>& privileges);
    bool hasPrivilege(const std::string& name, const std::string& db, const std::string& table,
                      const std::string& privilege) const;

    std::vector<std::string> names() const;  // creation order

private:
    mutable std::mutex mutex_;
    std::string path_;
    nlohmann::ordered_json users_ = nlohmann::ordered_json::object();

    void load();
    void saveLocked();  // caller holds mutex_
};

}  // namespace meradb
```

- [ ] **Step 4: Implementation**

```cpp
// cpp/src/users.cpp
#include "meradb/users.h"
#include "meradb/crypto.h"
#include "meradb/errors.h"
#include "meradb/sys_compat.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace meradb {

const std::vector<std::string>& allPrivileges() {
    static const std::vector<std::string> privileges = {"DIKHAO", "DAALO", "BADLO", "MITAO"};
    return privileges;
}

namespace {

std::string hashHex(const std::string& password, const crypto::Bytes& salt) {
    return crypto::toHex(crypto::pbkdf2HmacSha256(password, salt, PBKDF2_ITERATIONS, 32));
}

std::set<std::string> currentGrants(const json& user, const std::string& key) {
    std::set<std::string> out;
    auto grants = user.find("grants");
    if (grants == user.end() || !grants->is_object()) return out;
    auto entry = grants->find(key);
    if (entry == grants->end() || !entry->is_array()) return out;
    for (const auto& p : *entry)
        if (p.is_string()) out.insert(p.get<std::string>());
    return out;
}

json sortedPrivileges(const std::set<std::string>& current) {
    json out = json::array();
    for (const auto& p : allPrivileges())
        if (current.count(p)) out.push_back(p);
    return out;
}

ExecutionError noSuchUser(const std::string& name) { return ExecutionError("User '" + name + "' exist nahi karta"); }

}  // namespace

UserStore::UserStore(std::string dataDir) : path_((fs::path(dataDir) / kFileName).string()) { load(); }

void UserStore::load() {
    if (!fs::exists(path_)) return;
    std::ifstream in(path_, std::ios::binary);
    if (!in) throw StorageError(path_ + " khul nahi paayi");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    try {
        users_ = json::parse(text);
    } catch (const nlohmann::json::exception& e) {
        throw StorageError(path_ + " corrupt hai: " + e.what());
    }
    if (!users_.is_object()) throw StorageError(path_ + " corrupt hai: object expected tha");
}

void UserStore::saveLocked() {
    std::string text;
    try {
        text = users_.dump(2, ' ', true);  // json.dump(indent=2), ensure_ascii
    } catch (const nlohmann::json::exception& e) {
        throw StorageError(std::string("users.json likh nahi paaye: ") + e.what());
    }
    std::string tmp = path_ + ".tmp";
    {
        std::ofstream out(tmp);  // text mode on purpose: same line endings Python's open(..., "w") writes
        if (!out) throw StorageError(tmp + " likh nahi paaye");
        out << text;
        out.flush();
        if (!out) throw StorageError(tmp + " likh nahi paaye");
    }
    std::error_code ec;
    fs::rename(tmp, path_, ec);
    if (ec) throw StorageError(path_ + " save nahi hua: " + ec.message());
}

bool UserStore::exists(const std::string& name) const {
    std::lock_guard<std::mutex> guard(mutex_);
    return users_.contains(name);
}

std::vector<std::string> UserStore::names() const {
    std::lock_guard<std::mutex> guard(mutex_);
    std::vector<std::string> out;
    for (auto it = users_.begin(); it != users_.end(); ++it) out.push_back(it.key());
    return out;
}

void UserStore::create(const std::string& name, const std::string& password) {
    if (exists(name)) throw ExecutionError("User '" + name + "' pehle se hai");
    // the slow part (100,000 iterations) runs without the lock
    crypto::Bytes salt = sys::randomBytes(16);
    std::string hash = hashHex(password, salt);
    std::lock_guard<std::mutex> guard(mutex_);
    if (users_.contains(name)) throw ExecutionError("User '" + name + "' pehle se hai");  // lost a race
    json user = json::object();
    user["salt"] = crypto::toHex(salt);
    user["hash"] = hash;
    user["grants"] = json::object();
    users_[name] = std::move(user);
    try {
        saveLocked();
    } catch (...) {
        users_.erase(name);
        throw;
    }
}

void UserStore::drop(const std::string& name) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!users_.contains(name)) throw noSuchUser(name);
    users_.erase(name);
    saveLocked();
}

bool UserStore::verify(const std::string& name, const std::string& password) const {
    std::string saltHex, expected;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto it = users_.find(name);
        if (it == users_.end()) return false;
        try {
            saltHex = (*it).at("salt").get<std::string>();
            expected = (*it).at("hash").get<std::string>();
        } catch (const nlohmann::json::exception&) {
            return false;  // a malformed entry never authenticates
        }
    }
    crypto::Bytes salt;
    try {
        salt = crypto::fromHex(saltHex);
    } catch (const StorageError&) {
        return false;
    }
    return hashHex(password, salt) == expected;
}

void UserStore::grant(const std::string& name, const std::string& db, const std::string& table,
                      const std::vector<std::string>& privileges) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = users_.find(name);
    if (it == users_.end()) throw noSuchUser(name);
    std::string key = db + "." + table;
    std::set<std::string> current = currentGrants(*it, key);
    current.insert(privileges.begin(), privileges.end());
    (*it)["grants"][key] = sortedPrivileges(current);
    saveLocked();
}

void UserStore::revoke(const std::string& name, const std::string& db, const std::string& table,
                       const std::vector<std::string>& privileges) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = users_.find(name);
    if (it == users_.end()) throw noSuchUser(name);
    std::string key = db + "." + table;
    std::set<std::string> current = currentGrants(*it, key);
    for (const auto& p : privileges) current.erase(p);
    if (!current.empty()) (*it)["grants"][key] = sortedPrivileges(current);
    else (*it)["grants"].erase(key);
    saveLocked();
}

bool UserStore::hasPrivilege(const std::string& name, const std::string& db, const std::string& table,
                             const std::string& privilege) const {
    std::lock_guard<std::mutex> guard(mutex_);
    auto it = users_.find(name);
    if (it == users_.end()) return false;
    return currentGrants(*it, db + "." + table).count(privilege) > 0;
}

}  // namespace meradb
```

- [ ] **Step 5: CMake, build, run**

Add `src/users.cpp` to `meradb_core`, then
```bash
cmake -S cpp -B cpp/build -G "MinGW Makefiles" && cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R users
```
Expected: 7 tests PASS (each hash costs ~0.1 s). If the byte-for-byte test
fails only on line endings, `readText` reads in text mode so CRLF is already
normalised; a real difference is a bug in key order or indentation.

- [ ] **Step 6: Commit**

```bash
git add cpp/include/meradb/users.h cpp/src/users.cpp cpp/tests/test_users.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add UserStore with Python-compatible users.json"
```

**Completion checklist:**
- [ ] a `users.json` written by Python is read, verified and re-written identically
- [ ] the hash is computed outside `mutex_`
- [ ] error texts match `meradb/users.py` lines 71, 82, 100, 109

---

### Task 7: Catalog helpers for triggers and procedures

**Files:**
- Modify: `cpp/include/meradb/catalog.h`, `cpp/src/catalog.cpp`
- Test: `cpp/tests/test_catalog.cpp` (append)

**Interfaces:**
- Consumes: the existing `Catalog::triggers` / `procedures` (`ordered_json`,
  already carried through load/save by Phase 1).
- Produces: typed, order-preserving access with the same on-disk shape as
  `Catalog.add_trigger` / `add_procedure` in `meradb/catalog.py`:
  ```cpp
  struct TriggerInfo { std::string name, timing, event, table, bodyText; };
  struct ProcedureInfo { std::vector<std::pair<std::string, std::string>> params; std::string bodyText; };
  bool hasTrigger(const std::string&) const;
  void addTrigger(name, timing, event, table, bodyText);   // sets + saves; keeps memory == disk if the save fails
  void removeTrigger(const std::string&);
  std::vector<TriggerInfo> triggersFor(timing, event, table) const;  // creation order; returns COPIES
  bool hasProcedure(const std::string&) const;
  void addProcedure(name, params, bodyText);
  void removeProcedure(const std::string&);
  std::optional<ProcedureInfo> findProcedure(const std::string&) const;
  ```
  `triggersFor` returns copies on purpose: a superuser's trigger body may run
  DDL (`BANAO TRIGGER`, `HATAO TRIGGER`), so the catalog can change while the
  engine iterates the matching triggers — copies keep that safe.

- [ ] **Step 1: Write the failing test** (append to `test_catalog.cpp`; it
already includes `TempDir` and `Catalog`)

```cpp
TEST_CASE("catalog triggers keep creation order and Python's JSON shape", "[catalog][phase2]") {
    meradb_test::TempDir dir;
    {
        meradb::Catalog cat(dir.str());
        cat.addTrigger("first", "PEHLE", "DAALO", "accounts", "DAALO MEIN log MAAN (NAYA.id);");
        cat.addTrigger("second", "BAAD", "DAALO", "accounts", "DIKHAO * SE t;");
        cat.addTrigger("third", "PEHLE", "DAALO", "accounts", "DIKHAO * SE t;");
        CHECK(cat.hasTrigger("second"));
        CHECK_FALSE(cat.hasTrigger("nope"));

        auto pehle = cat.triggersFor("PEHLE", "DAALO", "accounts");
        REQUIRE(pehle.size() == 2);
        CHECK(pehle[0].name == "first");
        CHECK(pehle[1].name == "third");
        CHECK(pehle[0].bodyText == "DAALO MEIN log MAAN (NAYA.id);");
        CHECK(cat.triggersFor("PEHLE", "MITAO", "accounts").empty());
        CHECK(cat.triggersFor("PEHLE", "DAALO", "other").empty());

        cat.removeTrigger("first");
    }
    // reload from disk: same content, same order
    meradb::Catalog again(dir.str());
    auto all = again.triggersFor("PEHLE", "DAALO", "accounts");
    REQUIRE(all.size() == 1);
    CHECK(all[0].name == "third");

    auto j = nlohmann::ordered_json::parse(meradb_test::readText(dir.file("catalog.json")));
    REQUIRE(j["triggers"].is_object());
    // Python writes the keys in this order: timing, event, table, body_text
    std::vector<std::string> keys;
    for (auto it = j["triggers"]["second"].begin(); it != j["triggers"]["second"].end(); ++it) keys.push_back(it.key());
    CHECK(keys == std::vector<std::string>{"timing", "event", "table", "body_text"});
}

TEST_CASE("catalog procedures store [name, type] pairs then body_text", "[catalog][phase2]") {
    meradb_test::TempDir dir;
    {
        meradb::Catalog cat(dir.str());
        cat.addProcedure("badhao", {{"dept", "TEXT"}, {"pct", "INT"}}, "BADLO emp RAKHO s = s + pct;");
        CHECK(cat.hasProcedure("badhao"));
    }
    meradb::Catalog again(dir.str());
    auto p = again.findProcedure("badhao");
    REQUIRE(p.has_value());
    REQUIRE(p->params.size() == 2);
    CHECK(p->params[0] == std::make_pair(std::string("dept"), std::string("TEXT")));
    CHECK(p->params[1].second == "INT");
    CHECK(p->bodyText == "BADLO emp RAKHO s = s + pct;");
    CHECK_FALSE(again.findProcedure("ghost").has_value());

    auto j = nlohmann::ordered_json::parse(meradb_test::readText(dir.file("catalog.json")));
    CHECK(j["procedures"]["badhao"]["params"] == nlohmann::ordered_json::parse(R"([["dept","TEXT"],["pct","INT"]])"));
    std::vector<std::string> keys;
    for (auto it = j["procedures"]["badhao"].begin(); it != j["procedures"]["badhao"].end(); ++it) keys.push_back(it.key());
    CHECK(keys == std::vector<std::string>{"params", "body_text"});

    again.removeProcedure("badhao");
    CHECK_FALSE(again.hasProcedure("badhao"));
    meradb::Catalog third(dir.str());
    CHECK_FALSE(third.hasProcedure("badhao"));
}

TEST_CASE("catalog trigger body text with unicode and newlines round-trips", "[catalog][phase2]") {
    meradb_test::TempDir dir;
    std::string body = "DAALO MEIN log MAAN ('caf\xC3\xA9\\n');\nDIKHAO * SE t;";
    { meradb::Catalog cat(dir.str()); cat.addTrigger("t", "BAAD", "MITAO", "x", body); }
    meradb::Catalog again(dir.str());
    CHECK(again.triggersFor("BAAD", "MITAO", "x").at(0).bodyText == body);
}
```

- [ ] **Step 2: Verify it fails** — `cmake --build cpp/build`; Expected: FAIL (`addTrigger` undeclared).

- [ ] **Step 3: Header** — in `catalog.h` add, inside `class Catalog` (public, after `removeView`):

```cpp
    // ---- triggers & stored procedures (Phase 2). Stored exactly as catalog.py stores them ----
    struct TriggerInfo {
        std::string name, timing, event, table, bodyText;
    };
    struct ProcedureInfo {
        std::vector<std::pair<std::string, std::string>> params;  // (name, normalised type)
        std::string bodyText;
    };
    bool hasTrigger(const std::string& name) const { return triggers.contains(name); }
    void addTrigger(const std::string& name, const std::string& timing, const std::string& event,
                    const std::string& table, const std::string& bodyText);
    void removeTrigger(const std::string& name);
    // Matching triggers in CREATION order. Copies, so the caller may run statements
    // that change the catalog while iterating.
    std::vector<TriggerInfo> triggersFor(const std::string& timing, const std::string& event,
                                         const std::string& table) const;
    bool hasProcedure(const std::string& name) const { return procedures.contains(name); }
    void addProcedure(const std::string& name, const std::vector<std::pair<std::string, std::string>>& params,
                      const std::string& bodyText);
    void removeProcedure(const std::string& name);
    std::optional<ProcedureInfo> findProcedure(const std::string& name) const;
```

- [ ] **Step 4: Implementation** — in `catalog.cpp` (the file has `using json = nlohmann::ordered_json` in its
anonymous namespace / at top; if the alias is not visible at namespace scope, add
`using json = nlohmann::ordered_json;` inside `namespace meradb` above these functions), after `Catalog::removeView`:

```cpp
void Catalog::addTrigger(const std::string& name, const std::string& timing, const std::string& event,
                         const std::string& table, const std::string& bodyText) {
    json entry = json::object();  // key order = Python's dict literal order
    entry["timing"] = timing;
    entry["event"] = event;
    entry["table"] = table;
    entry["body_text"] = bodyText;
    renderJson(entry);  // fails early (invalid UTF-8) before memory is touched
    std::optional<json> previous;
    if (triggers.contains(name)) previous = triggers[name];
    triggers[name] = entry;
    try {
        save();
    } catch (...) {
        if (previous) triggers[name] = *previous;
        else triggers.erase(name);
        throw;
    }
}

void Catalog::removeTrigger(const std::string& name) {
    triggers.erase(name);
    save();
}

std::vector<Catalog::TriggerInfo> Catalog::triggersFor(const std::string& timing, const std::string& event,
                                                       const std::string& table) const {
    std::vector<TriggerInfo> out;
    for (auto it = triggers.begin(); it != triggers.end(); ++it) {
        const json& t = it.value();
        if (t.value("timing", "") == timing && t.value("event", "") == event && t.value("table", "") == table)
            out.push_back({it.key(), timing, event, table, t.value("body_text", "")});
    }
    return out;
}

void Catalog::addProcedure(const std::string& name, const std::vector<std::pair<std::string, std::string>>& params,
                           const std::string& bodyText) {
    json entry = json::object();
    json list = json::array();
    for (const auto& [pname, ptype] : params) list.push_back(json::array({pname, ptype}));
    entry["params"] = std::move(list);
    entry["body_text"] = bodyText;
    renderJson(entry);
    std::optional<json> previous;
    if (procedures.contains(name)) previous = procedures[name];
    procedures[name] = entry;
    try {
        save();
    } catch (...) {
        if (previous) procedures[name] = *previous;
        else procedures.erase(name);
        throw;
    }
}

void Catalog::removeProcedure(const std::string& name) {
    procedures.erase(name);
    save();
}

std::optional<Catalog::ProcedureInfo> Catalog::findProcedure(const std::string& name) const {
    auto it = procedures.find(name);
    if (it == procedures.end()) return std::nullopt;
    ProcedureInfo info;
    try {
        for (const auto& p : it->at("params")) info.params.emplace_back(p.at(0).get<std::string>(), p.at(1).get<std::string>());
        info.bodyText = it->at("body_text").get<std::string>();
    } catch (const nlohmann::json::exception& e) {
        throw StorageError("catalog.json corrupt hai: procedure '" + name + "' samajh nahi aaya");
    }
    return info;
}
```

- [ ] **Step 5: Build and run**

```bash
cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R "catalog"
```
Expected: PASS (old and new catalog tests).

- [ ] **Step 6: Commit**

```bash
git add cpp/include/meradb/catalog.h cpp/src/catalog.cpp cpp/tests/test_catalog.cpp
git commit -m "Add catalog helpers for triggers and stored procedures"
```

**Completion checklist:**
- [ ] key order inside each entry equals Python's (`timing,event,table,body_text` / `params,body_text`)
- [ ] a failed save leaves memory and disk in agreement
- [ ] Phase 1 catalog tests (including non-finite defaults) still pass untouched

---

### Task 8: Engine — users, grants and privilege enforcement

**Files:**
- Modify: `cpp/include/meradb/engine.h`, `cpp/src/engine.cpp`
- Test: `cpp/tests/test_engine_users.cpp` (new)
- Modify: `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `UserStore` (Task 6), `ast::CreateUser/DropUser/Grant/Revoke`,
  `astClassName` (Task 2).
- Produces:
  - `Instance::users() -> UserStore&` (one store per data folder, shared by every session).
  - `Engine::user` (`std::optional<std::string>`; `nullopt` = superuser, the only kind an embedded engine ever is).
  - `Engine::executeStatement` runs `checkPrivileges` after the lock and the
    "database gone" check, exactly where Python does (design decision D6).
  - Statements `BANAO USER`, `HATAO USER`, `ADHIKAR DO`, `ADHIKAR WAPAS`.

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_engine_users.cpp
#include <catch2/catch_test_macros.hpp>
#include "golden_runner.h"   // runLast
#include "meradb/engine.h"
#include "test_util.h"

using namespace meradb;
using meradb_test::TempDir;
using meradb_test::runLast;

namespace {
std::string notSuper(const std::string& user, const std::string& cls) {
    return "[Execution Galti] '" + user + "' superuser nahi hai -- '" + cls +
           "' jaisa DDL/admin command sirf superuser (bina username connect kiya session) chala sakta hai";
}
std::string noPriv(const std::string& user, const std::string& table, const std::string& priv) {
    return "[Execution Galti] '" + user + "' ko table '" + table + "' par " + priv + " ka adhikar nahi hai";
}
struct Fixture {
    TempDir dir;
    std::shared_ptr<Instance> inst = std::make_shared<Instance>(dir.str());
    Engine admin{inst};
    Engine ravi{inst};
    Fixture() {
        ravi.user = "ravi";
        admin.execute("BANAO USER ravi GUPT 'pw';"
                      "BANAO TABLE students (id INT MUKHYA KUNJI, name TEXT);"
                      "BANAO TABLE other (id INT);"
                      "DAALO MEIN students MAAN (1, 'a');"
                      "DAALO MEIN other MAAN (7);");
    }
};
}  // namespace

TEST_CASE("engine_users user and grant statements return Python's messages", "[engine][users]") {
    TempDir dir;
    Engine e(dir.str());
    CHECK(runLast(e, "BANAO USER ravi GUPT 'pw';").message == "User 'ravi' ban gaya");
    CHECK(runLast(e, "BANAO USER ravi GUPT 'pw';").error == "[Execution Galti] User 'ravi' pehle se hai");
    CHECK(runLast(e, "ADHIKAR DO DIKHAO, DAALO PAR students KO ravi;").message ==
          "'ravi' ko 'main.students' par DIKHAO, DAALO ka adhikar mil gaya");
    CHECK(runLast(e, "ADHIKAR WAPAS DAALO PAR students SE ravi;").message ==
          "'ravi' se 'main.students' par DAALO ka adhikar wapas le liya");
    CHECK(runLast(e, "ADHIKAR DO SAB PAR students KO ghost;").error == "[Execution Galti] User 'ghost' exist nahi karta");
    CHECK(runLast(e, "HATAO USER ravi;").message == "User 'ravi' hata diya");
    CHECK(runLast(e, "HATAO USER ravi;").error == "[Execution Galti] User 'ravi' exist nahi karta");
}

TEST_CASE("engine_users grants are recorded against the CURRENT database", "[engine][users]") {
    TempDir dir;
    Engine e(dir.str());
    e.execute("BANAO USER ravi GUPT 'pw'; BANAO DATABASE school; ISTEMAL school;");
    CHECK(runLast(e, "ADHIKAR DO DIKHAO PAR t KO ravi;").message == "'ravi' ko 'school.t' par DIKHAO ka adhikar mil gaya");
    CHECK(e.instance().users().hasPrivilege("ravi", "school", "t", "DIKHAO"));
    CHECK_FALSE(e.instance().users().hasPrivilege("ravi", "main", "t", "DIKHAO"));
}

TEST_CASE("engine_users superuser sessions are never checked", "[engine][users]") {
    Fixture f;
    CHECK(runLast(f.admin, "DIKHAO * SE students;").error.empty());
    CHECK(runLast(f.admin, "HATAO TABLE other;").error.empty());
}

TEST_CASE("engine_users SELECT needs DIKHAO on the FROM table and every joined table", "[engine][users]") {
    Fixture f;
    CHECK(runLast(f.ravi, "DIKHAO * SE students;").error == noPriv("ravi", "students", "DIKHAO"));
    f.admin.execute("ADHIKAR DO DIKHAO PAR students KO ravi;");
    CHECK(runLast(f.ravi, "DIKHAO * SE students;").error.empty());
    CHECK(runLast(f.ravi, "DIKHAO * SE students s MILAO other o PAR s.id = o.id;").error == noPriv("ravi", "other", "DIKHAO"));
    f.admin.execute("ADHIKAR DO DIKHAO PAR other KO ravi;");
    CHECK(runLast(f.ravi, "DIKHAO * SE students s MILAO other o PAR s.id = o.id;").error.empty());
}

TEST_CASE("engine_users DML privileges are checked per statement kind", "[engine][users]") {
    Fixture f;
    CHECK(runLast(f.ravi, "DAALO MEIN students MAAN (2, 'b');").error == noPriv("ravi", "students", "DAALO"));
    CHECK(runLast(f.ravi, "BADLO students RAKHO name = 'z';").error == noPriv("ravi", "students", "BADLO"));
    CHECK(runLast(f.ravi, "MITAO SE students;").error == noPriv("ravi", "students", "MITAO"));
    f.admin.execute("ADHIKAR DO DAALO, BADLO PAR students KO ravi;");
    CHECK(runLast(f.ravi, "DAALO MEIN students MAAN (2, 'b');").message == "1 row(s) daal di");
    CHECK(runLast(f.ravi, "BADLO students RAKHO name = 'z' JAHAN id = 2;").message == "1 row(s) badal di");
    CHECK(runLast(f.ravi, "MITAO SE students;").error == noPriv("ravi", "students", "MITAO"));
}

TEST_CASE("engine_users INSERT ... DIKHAO also needs DIKHAO on the source", "[engine][users]") {
    Fixture f;
    f.admin.execute("ADHIKAR DO DAALO PAR other KO ravi;");
    CHECK(runLast(f.ravi, "DAALO MEIN other DIKHAO id SE students;").error == noPriv("ravi", "students", "DIKHAO"));
    f.admin.execute("ADHIKAR DO DIKHAO PAR students KO ravi;");
    CHECK(runLast(f.ravi, "DAALO MEIN other DIKHAO id SE students;").message == "1 row(s) daal di");
}

TEST_CASE("engine_users everything else is superuser-only and names the Python class", "[engine][users]") {
    Fixture f;
    f.admin.execute("ADHIKAR DO SAB PAR students KO ravi;");
    CHECK(runLast(f.ravi, "BANAO TABLE x (id INT);").error == notSuper("ravi", "CreateTable"));
    CHECK(runLast(f.ravi, "HATAO TABLE students;").error == notSuper("ravi", "DropTable"));
    CHECK(runLast(f.ravi, "SHURU;").error == notSuper("ravi", "Begin"));
    CHECK(runLast(f.ravi, "PAKKA;").error == notSuper("ravi", "Commit"));
    CHECK(runLast(f.ravi, "BANAO USER x GUPT 'y';").error == notSuper("ravi", "CreateUser"));
    CHECK(runLast(f.ravi, "ADHIKAR DO SAB PAR other KO ravi;").error == notSuper("ravi", "Grant"));
    CHECK(runLast(f.ravi, "ISTEMAL main;").error == notSuper("ravi", "UseDatabase"));
    CHECK(runLast(f.ravi, "nonsense;").error.find("Parser Galti") != std::string::npos);  // parse errors come first
}

TEST_CASE("engine_users SAMJHAO checks the inner statement", "[engine][users]") {
    Fixture f;
    CHECK(runLast(f.ravi, "SAMJHAO DIKHAO * SE students;").error == noPriv("ravi", "students", "DIKHAO"));
    CHECK(runLast(f.ravi, "SAMJHAO BANAO TABLE x (id INT);").error == notSuper("ravi", "CreateTable"));
    f.admin.execute("ADHIKAR DO DIKHAO PAR students KO ravi;");
    CHECK(runLast(f.ravi, "SAMJHAO DIKHAO * SE students;").error.empty());
}

TEST_CASE("engine_users set operations re-check every side", "[engine][users]") {
    Fixture f;
    f.admin.execute("ADHIKAR DO DIKHAO PAR students KO ravi;");
    CHECK(runLast(f.ravi, "DIKHAO id SE students SANYUKT DIKHAO id SE other;").error == noPriv("ravi", "other", "DIKHAO"));
    f.admin.execute("ADHIKAR DO DIKHAO PAR other KO ravi;");
    CHECK(runLast(f.ravi, "DIKHAO id SE students SANYUKT DIKHAO id SE other;").error.empty());
}

TEST_CASE("engine_users a view is granted by its own name", "[engine][users]") {
    Fixture f;
    f.admin.execute("BANAO VIEW v KAHO DIKHAO id SE students;");
    CHECK(runLast(f.ravi, "DIKHAO * SE v;").error == noPriv("ravi", "v", "DIKHAO"));
    f.admin.execute("ADHIKAR DO DIKHAO PAR v KO ravi;");
    // no DIKHAO on `students` itself is needed: the view's stored query is not re-checked
    CHECK(runLast(f.ravi, "DIKHAO * SE v;").error.empty());
    CHECK(runLast(f.ravi, "DIKHAO * SE students;").error == noPriv("ravi", "students", "DIKHAO"));
}

TEST_CASE("engine_users users survive a new Instance on the same folder", "[engine][users]") {
    TempDir dir;
    { Engine e(dir.str()); e.execute("BANAO USER ravi GUPT 'pw'; ADHIKAR DO DIKHAO PAR t KO ravi;"); }
    Engine again(dir.str());
    CHECK(again.instance().users().verify("ravi", "pw"));
    CHECK(again.instance().users().hasPrivilege("ravi", "main", "t", "DIKHAO"));
}
```

- [ ] **Step 2: Add to the build and verify it fails**

Add `test_engine_users.cpp`. `cmake --build cpp/build` — Expected: FAIL (`Engine::user`, `Instance::users` missing).

- [ ] **Step 3: Engine header changes** (`cpp/include/meradb/engine.h`)

- `#include "meradb/users.h"`.
- In `class Instance` public section add
  `UserStore& users() { return users_; }`; in its private section add
  `UserStore users_;` **directly after** `std::string dataDir_;` (initialisation
  order follows declaration order and `users_` needs `dataDir_`).
- In `class Engine` public section add, next to `currentDb`:
  ```cpp
      // The session's authenticated username. nullopt = SUPERUSER (unrestricted):
      // every embedded Engine and every server session that connected without a
      // username. See docs/SERVER.md "no username = superuser".
      std::optional<std::string> user;
  ```
- Private handlers and helpers:
  ```cpp
      Result execCreateUser(const ast::CreateUser&);
      Result execDropUser(const ast::DropUser&);
      Result execGrant(const ast::Grant&);
      Result execRevoke(const ast::Revoke&);

      void checkPrivileges(const ast::Statement& stmt);
      void requirePrivilege(const std::string& privilege, const std::string& table);
      static std::vector<std::string> tablesRead(const ast::Select& stmt);
  ```

- [ ] **Step 4: Engine implementation** (`cpp/src/engine.cpp`)

Constructor: `Instance::Instance(std::string dataDir) : dataDir_(fs::absolute(fs::path(dataDir)).string()), users_(dataDir_) {`.
Add `#include "meradb/ast_util.h"` if missing (it is already included).

In `executeStatement`, right after the "database gone" block and before
`using namespace ast;`:

```cpp
    checkPrivileges(stmt);
```
and add four dispatch lines before the final `throw`:
```cpp
    if (auto* s = dynamic_cast<const CreateUser*>(&stmt)) return execCreateUser(*s);
    if (auto* s = dynamic_cast<const DropUser*>(&stmt)) return execDropUser(*s);
    if (auto* s = dynamic_cast<const Grant*>(&stmt)) return execGrant(*s);
    if (auto* s = dynamic_cast<const Revoke*>(&stmt)) return execRevoke(*s);
```

New code (after `Engine::close()`):

```cpp
// ============================================================================
// users & privileges (server-wide -- see users.h)
// ============================================================================

std::vector<std::string> Engine::tablesRead(const ast::Select& stmt) {
    // The FROM table plus every MILAO'd table -- NOT tables read only through a
    // VIEW's own stored query (that runs via resolveSource, never through
    // executeStatement, so it is never re-checked; a view is meant to be granted
    // by its own name).
    std::vector<std::string> tables{stmt.table};
    for (const auto& join : stmt.joins) tables.push_back(join.table);
    return tables;
}

void Engine::requirePrivilege(const std::string& privilege, const std::string& table) {
    if (!instance_->users().hasPrivilege(*user, currentDb, table, privilege))
        throw ExecutionError("'" + *user + "' ko table '" + table + "' par " + privilege + " ka adhikar nahi hai");
}

void Engine::checkPrivileges(const ast::Statement& stmt) {
    if (!user) return;  // superuser: checking is skipped entirely
    using namespace ast;
    if (auto* ex = dynamic_cast<const Explain*>(&stmt)) {
        checkPrivileges(*ex->statement);
        return;
    }
    if (dynamic_cast<const SetOp*>(&stmt)) return;  // execSetOp re-enters executeStatement for each side
    if (auto* sel = dynamic_cast<const Select*>(&stmt)) {
        for (const auto& t : tablesRead(*sel)) requirePrivilege("DIKHAO", t);
        return;
    }
    if (auto* ins = dynamic_cast<const Insert*>(&stmt)) {
        requirePrivilege("DAALO", ins->table);
        if (ins->select)
            for (const auto& t : tablesRead(*ins->select)) requirePrivilege("DIKHAO", t);
        return;
    }
    if (auto* upd = dynamic_cast<const Update*>(&stmt)) {
        requirePrivilege("BADLO", upd->table);
        return;
    }
    if (auto* del = dynamic_cast<const Delete*>(&stmt)) {
        requirePrivilege("MITAO", del->table);
        return;
    }
    // Everything else: DDL, VIEW/TRIGGER/PROCEDURE management, users and grants,
    // transactions, database switching.
    throw ExecutionError("'" + *user + "' superuser nahi hai -- '" + astClassName(stmt) +
                         "' jaisa DDL/admin command sirf superuser (bina username connect kiya session) chala sakta hai");
}

Result Engine::execCreateUser(const ast::CreateUser& stmt) {
    instance_->users().create(stmt.name, stmt.password);
    return messageResult("User '" + stmt.name + "' ban gaya");
}

Result Engine::execDropUser(const ast::DropUser& stmt) {
    instance_->users().drop(stmt.name);
    return messageResult("User '" + stmt.name + "' hata diya");
}

Result Engine::execGrant(const ast::Grant& stmt) {
    instance_->users().grant(stmt.user, currentDb, stmt.table, stmt.privileges);
    return messageResult("'" + stmt.user + "' ko '" + currentDb + "." + stmt.table + "' par " +
                         joinStrs(stmt.privileges, ", ") + " ka adhikar mil gaya");
}

Result Engine::execRevoke(const ast::Revoke& stmt) {
    instance_->users().revoke(stmt.user, currentDb, stmt.table, stmt.privileges);
    return messageResult("'" + stmt.user + "' se '" + currentDb + "." + stmt.table + "' par " +
                         joinStrs(stmt.privileges, ", ") + " ka adhikar wapas le liya");
}
```

Note (mirrored Python quirk): `ADHIKAR DO ... PAR t KO u` does **not** check that
`t` exists — a grant may name a table that is created later. Do not add a check.

- [ ] **Step 5: Build and run**

```bash
cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R "engine_users|users|catalog"
```
Expected: PASS. The full suite must still be green
(`ctest --test-dir cpp/build --output-on-failure`).

- [ ] **Step 6: Cross-check the exact strings against Python** (once, by hand)

```bash
python -c "
from meradb.engine import Engine, Instance
import tempfile
d=tempfile.mkdtemp(); a=Engine(d); r=Engine(a.instance); r.user='ravi'
a.execute(\"BANAO USER ravi GUPT 'pw'; BANAO TABLE t (id INT);\")
for q in ['DIKHAO * SE t;','SHURU;','BANAO TABLE x (id INT);','SAMJHAO HATAO TABLE t;']:
    print(r.run_script(q)[-1].error)"
```
Expected: the same four sentences the C++ tests assert.

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/engine.h cpp/src/engine.cpp cpp/tests/test_engine_users.cpp cpp/tests/CMakeLists.txt
git commit -m "Add users, grants and privilege enforcement to the engine"
```

**Completion checklist:**
- [ ] privilege check sits after the lock and the "database gone" check
- [ ] `Explain` recurses, `SetOp` is skipped (each side re-checks), views use their own name
- [ ] user/grant statements are superuser-only for restricted sessions (`CreateUser`, `Grant` ... in the message)
- [ ] no change to embedded behaviour: the Phase 1 suite is untouched and green

---

### Task 9: AST substitution (`NAYA` / `PURANA` / parameters)

**Files:**
- Create: `cpp/include/meradb/substitute.h`, `cpp/src/substitute.cpp`
- Test: `cpp/tests/test_substitute.cpp`
- Modify: `cpp/CMakeLists.txt`, `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `ast::*` (Task 2 included), `Value`.
- Produces:
  ```cpp
  using RefReplacer = std::function<std::optional<Value>(const ast::ColumnRef&)>;
  void substituteInPlace(std::unique_ptr<ast::Expr>& expr, const RefReplacer& replace);
  void substituteStatementInPlace(ast::Statement& stmt, const RefReplacer& replace);
  ```
  In-place (Python builds new nodes, but its callers always pass a freshly
  parsed body, so mutating the fresh tree is equivalent and cheaper). Semantics
  copied from `Engine._substitute` / `_substitute_statement` (engine.py 620-682):
  every `ColumnRef` for which `replace` returns a value becomes a `Literal`;
  `Literal`/`Star` are untouched; **`Subquery` and `InSubquery` are left exactly
  as parsed** (including the `left` operand of `InSubquery`); statements
  rewritten: `Insert` (rows, its `select`, upsert assignments), `Update`
  (assignments, where), `Delete` (where), `Select` (columns, join `on`, where,
  group by, having, order by), `CallProcedure` (args). **Every other statement
  kind is left untouched.**

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_substitute.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/parser.h"
#include "meradb/substitute.h"

using namespace meradb;

namespace {
// naya.x -> the given int
RefReplacer nayaX(int64_t value) {
    return [value](const ast::ColumnRef& ref) -> std::optional<Value> {
        if (ref.table && *ref.table == "naya" && ref.name == "x") return Value(value);
        return std::nullopt;
    };
}
// a BARE (unqualified) `pct` -> the given int, like a procedure parameter
RefReplacer paramPct(int64_t value) {
    return [value](const ast::ColumnRef& ref) -> std::optional<Value> {
        if (!ref.table && ref.name == "pct") return Value(value);
        return std::nullopt;
    };
}
const ast::Literal* asLiteral(const ast::Expr* e) { return dynamic_cast<const ast::Literal*>(e); }
int64_t intOf(const ast::Expr* e) {
    auto* lit = asLiteral(e);
    REQUIRE(lit != nullptr);
    return std::get<int64_t>(lit->value.data);
}
template <typename T>
T& first(std::vector<std::unique_ptr<ast::Statement>>& stmts) {
    auto* p = dynamic_cast<T*>(stmts.at(0).get());
    REQUIRE(p != nullptr);
    return *p;
}
}  // namespace

TEST_CASE("substitute UPDATE assignments and WHERE", "[substitute]") {
    auto stmts = parseScript("BADLO t RAKHO a = naya.x + 1 JAHAN id = naya.x;");
    auto& upd = first<ast::Update>(stmts);
    substituteStatementInPlace(upd, nayaX(5));
    auto* sum = dynamic_cast<const ast::BinaryOp*>(upd.assignments[0].second.get());
    REQUIRE(sum != nullptr);
    CHECK(intOf(sum->left.get()) == 5);
    auto* eq = dynamic_cast<const ast::BinaryOp*>(upd.where.get());
    REQUIRE(eq != nullptr);
    CHECK(dynamic_cast<const ast::ColumnRef*>(eq->left.get()) != nullptr);  // `id` is not a NAYA reference
    CHECK(intOf(eq->right.get()) == 5);
}

TEST_CASE("substitute INSERT rows, INSERT..SELECT and upsert assignments", "[substitute]") {
    auto stmts = parseScript(
        "DAALO MEIN t MAAN (naya.x, 'a'), (naya.x + 1, 'b') TAKRAAV PAR BADLO a = naya.x;"
        "DAALO MEIN t DIKHAO naya.x SE u JAHAN id = naya.x;");
    auto& ins = first<ast::Insert>(stmts);
    substituteStatementInPlace(ins, nayaX(7));
    CHECK(intOf(ins.rows[0][0].get()) == 7);
    auto* plus = dynamic_cast<const ast::BinaryOp*>(ins.rows[1][0].get());
    REQUIRE(plus != nullptr);
    CHECK(intOf(plus->left.get()) == 7);
    REQUIRE(ins.onConflictUpdate.has_value());
    CHECK(intOf((*ins.onConflictUpdate)[0].second.get()) == 7);

    auto& sel = *dynamic_cast<ast::Insert&>(*stmts[1]).select;
    substituteStatementInPlace(*stmts[1], nayaX(9));
    CHECK(intOf(sel.columns[0].get()) == 9);
    CHECK(intOf(dynamic_cast<const ast::BinaryOp*>(sel.where.get())->right.get()) == 9);
}

TEST_CASE("substitute SELECT columns, join ON, group by, having, order by", "[substitute]") {
    auto stmts = parseScript(
        "DIKHAO naya.x, GINO(*) SE t MILAO u PAR t.id = naya.x JAHAN naya.x > 0 "
        "SAMOOH naya.x JINKA GINO(*) > naya.x KRAM naya.x;");
    auto& sel = first<ast::Select>(stmts);
    substituteStatementInPlace(sel, nayaX(3));
    CHECK(intOf(sel.columns[0].get()) == 3);
    CHECK(intOf(dynamic_cast<const ast::BinaryOp*>(sel.joins[0].on.get())->right.get()) == 3);
    CHECK(intOf(dynamic_cast<const ast::BinaryOp*>(sel.where.get())->left.get()) == 3);
    CHECK(intOf(sel.groupBy[0].get()) == 3);
    CHECK(intOf(dynamic_cast<const ast::BinaryOp*>(sel.having.get())->right.get()) == 3);
    CHECK(intOf(sel.orderBy[0].expr.get()) == 3);
}

TEST_CASE("substitute leaves subqueries and non-DML statements alone", "[substitute]") {
    auto stmts = parseScript("DIKHAO * SE t JAHAN id MEIN (DIKHAO naya.x SE u);");
    auto& sel = first<ast::Select>(stmts);
    substituteStatementInPlace(sel, nayaX(1));
    auto* in = dynamic_cast<const ast::InSubquery*>(sel.where.get());
    REQUIRE(in != nullptr);
    // the subquery's own NAYA reference is NOT replaced (Python: "out of scope")
    CHECK(dynamic_cast<const ast::ColumnRef*>(in->subquery->statement->columns[0].get()) != nullptr);

    auto ddl = parseScript("BANAO TABLE x (id INT);");
    substituteStatementInPlace(*ddl[0], nayaX(1));  // no effect, no throw
    CHECK(dynamic_cast<ast::CreateTable*>(ddl[0].get()) != nullptr);
}

TEST_CASE("substitute procedure parameters replace only BARE names", "[substitute]") {
    auto stmts = parseScript("BADLO emp RAKHO salary = salary + pct JAHAN emp.pct = pct;");
    auto& upd = first<ast::Update>(stmts);
    substituteStatementInPlace(upd, paramPct(10));
    auto* sum = dynamic_cast<const ast::BinaryOp*>(upd.assignments[0].second.get());
    CHECK(intOf(sum->right.get()) == 10);
    auto* eq = dynamic_cast<const ast::BinaryOp*>(upd.where.get());
    CHECK(dynamic_cast<const ast::ColumnRef*>(eq->left.get()) != nullptr);  // emp.pct is qualified: untouched
    CHECK(intOf(eq->right.get()) == 10);
}

TEST_CASE("substitute reaches CASE, COALESCE, IS NULL and unary operands", "[substitute]") {
    // PEHLA(...) = COALESCE, AGAR ... TAB ... WARNA ... KHATAM = CASE, HAI KHALI = IS NULL
    auto stmts = parseScript(
        "BADLO t RAKHO a = PEHLA(naya.x, 0), b = AGAR naya.x > 1 TAB -naya.x WARNA 0 KHATAM JAHAN naya.x HAI KHALI;");
    auto& upd = first<ast::Update>(stmts);
    substituteStatementInPlace(upd, nayaX(4));
    auto* co = dynamic_cast<const ast::Coalesce*>(upd.assignments[0].second.get());
    REQUIRE(co != nullptr);
    CHECK(intOf(co->args[0].get()) == 4);
    auto* cw = dynamic_cast<const ast::CaseWhen*>(upd.assignments[1].second.get());
    REQUIRE(cw != nullptr);
    CHECK(intOf(dynamic_cast<const ast::BinaryOp*>(cw->branches[0].first.get())->left.get()) == 4);
    auto* neg = dynamic_cast<const ast::UnaryOp*>(cw->branches[0].second.get());
    REQUIRE(neg != nullptr);
    CHECK(intOf(neg->operand.get()) == 4);
    auto* isnull = dynamic_cast<const ast::IsNull*>(upd.where.get());
    REQUIRE(isnull != nullptr);
    CHECK(intOf(isnull->expr.get()) == 4);
}

TEST_CASE("substitute CALL arguments", "[substitute]") {
    auto stmts = parseScript("CHALAO other(naya.x, 5);");
    auto& call = first<ast::CallProcedure>(stmts);
    substituteStatementInPlace(call, nayaX(2));
    CHECK(intOf(call.args[0].get()) == 2);
    CHECK(intOf(call.args[1].get()) == 5);
}
```

- [ ] **Step 2: Add to the build and verify it fails**

Add `test_substitute.cpp`; `cmake --build cpp/build` — Expected: FAIL, no `substitute.h`.

- [ ] **Step 3: Header**

```cpp
// cpp/include/meradb/substitute.h
//
// Generic "replace column references by constants" rewrite, shared by triggers
// (NAYA.col / PURANA.col) and stored procedures (parameter names). Mirrors
// Engine._substitute / _substitute_statement in meradb/engine.py.
#pragma once
#include "meradb/ast.h"
#include <functional>
#include <memory>
#include <optional>

namespace meradb {

// Returns a value to substitute for this reference, or nullopt to leave it alone.
using RefReplacer = std::function<std::optional<Value>(const ast::ColumnRef&)>;

// Replaces matching ColumnRefs in the tree by Literals. A null slot is fine.
// Subquery / InSubquery nodes are deliberately not entered.
void substituteInPlace(std::unique_ptr<ast::Expr>& expr, const RefReplacer& replace);

// Rewrites the expressions of Insert / Update / Delete / Select / CallProcedure;
// any other statement kind is left untouched.
void substituteStatementInPlace(ast::Statement& stmt, const RefReplacer& replace);

}  // namespace meradb
```

- [ ] **Step 4: Implementation**

```cpp
// cpp/src/substitute.cpp
#include "meradb/substitute.h"
#include "meradb/errors.h"

namespace meradb {

using namespace ast;

void substituteInPlace(std::unique_ptr<Expr>& expr, const RefReplacer& replace) {
    if (!expr) return;
    Expr* node = expr.get();
    if (dynamic_cast<Literal*>(node) || dynamic_cast<Star*>(node)) return;
    if (auto* ref = dynamic_cast<ColumnRef*>(node)) {
        if (auto value = replace(*ref)) expr = std::make_unique<Literal>(std::move(*value));  // `ref` dies here
        return;
    }
    if (auto* bin = dynamic_cast<BinaryOp*>(node)) {
        substituteInPlace(bin->left, replace);
        substituteInPlace(bin->right, replace);
        return;
    }
    if (auto* un = dynamic_cast<UnaryOp*>(node)) {
        substituteInPlace(un->operand, replace);
        return;
    }
    if (auto* isNull = dynamic_cast<IsNull*>(node)) {
        substituteInPlace(isNull->expr, replace);
        return;
    }
    if (auto* fn = dynamic_cast<FuncCall*>(node)) {
        substituteInPlace(fn->arg, replace);
        return;
    }
    if (auto* co = dynamic_cast<Coalesce*>(node)) {
        for (auto& arg : co->args) substituteInPlace(arg, replace);
        return;
    }
    if (auto* cw = dynamic_cast<CaseWhen*>(node)) {
        for (auto& branch : cw->branches) {
            substituteInPlace(branch.first, replace);
            substituteInPlace(branch.second, replace);
        }
        substituteInPlace(cw->elseExpr, replace);
        return;
    }
    if (dynamic_cast<Subquery*>(node) || dynamic_cast<InSubquery*>(node)) return;  // out of scope, like Python
    throw ExecutionError("Unknown expression");
}

void substituteStatementInPlace(Statement& stmt, const RefReplacer& replace) {
    if (auto* ins = dynamic_cast<Insert*>(&stmt)) {
        for (auto& row : ins->rows)
            for (auto& e : row) substituteInPlace(e, replace);
        if (ins->select) substituteStatementInPlace(*ins->select, replace);
        if (ins->onConflictUpdate)
            for (auto& assignment : *ins->onConflictUpdate) substituteInPlace(assignment.second, replace);
        return;
    }
    if (auto* upd = dynamic_cast<Update*>(&stmt)) {
        for (auto& assignment : upd->assignments) substituteInPlace(assignment.second, replace);
        substituteInPlace(upd->where, replace);
        return;
    }
    if (auto* del = dynamic_cast<Delete*>(&stmt)) {
        substituteInPlace(del->where, replace);
        return;
    }
    if (auto* sel = dynamic_cast<Select*>(&stmt)) {
        for (auto& c : sel->columns) substituteInPlace(c, replace);
        for (auto& j : sel->joins) substituteInPlace(j.on, replace);
        substituteInPlace(sel->where, replace);
        for (auto& g : sel->groupBy) substituteInPlace(g, replace);
        substituteInPlace(sel->having, replace);
        for (auto& o : sel->orderBy) substituteInPlace(o.expr, replace);
        return;
    }
    if (auto* call = dynamic_cast<CallProcedure*>(&stmt)) {
        for (auto& a : call->args) substituteInPlace(a, replace);
        return;
    }
    // every other statement kind: nothing a NAYA/PURANA/parameter substitution could mean
}

}  // namespace meradb
```

- [ ] **Step 5: Build and run** — add `src/substitute.cpp` to `meradb_core`;

```bash
cmake -S cpp -B cpp/build -G "MinGW Makefiles" && cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R substitute
```
Expected: 7 tests PASS.

- [ ] **Step 6: Commit**

```bash
git add cpp/include/meradb/substitute.h cpp/src/substitute.cpp cpp/tests/test_substitute.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add AST substitution for triggers and procedures"
```

**Completion checklist:**
- [ ] subquery interiors and `InSubquery::left` are not entered (matches Python)
- [ ] DDL / transaction statements pass through untouched
- [ ] `expr = make_unique<Literal>` is the last use of `ref` (no dangling access)

---

### Task 10: Engine — triggers

**Files:**
- Modify: `cpp/include/meradb/engine.h`, `cpp/src/engine.cpp`
- Test: `cpp/tests/test_engine_triggers.cpp` (new)
- Modify: `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 7 (`Catalog::addTrigger`, `triggersFor`), Task 9 (`substitute`), Task 4 (parser), Task 8 (`executeStatement` privilege check).
- Produces: `BANAO TRIGGER` / `HATAO TRIGGER`; `Engine::fireTriggers(timing, event, table, newRow, oldRow)`; the shared body runner `Engine::runBody(bodyText, replace, results)` (Task 11 reuses it); a nesting cap `kMaxBodyDepth = 32` (Design decision D5). **This is a risky task — see D9 item 2: review the hook order in the three DML paths line by line against `engine.py`.**

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_engine_triggers.cpp
#include <catch2/catch_test_macros.hpp>
#include "golden_runner.h"
#include "meradb/engine.h"
#include "test_util.h"
#include <thread>

using namespace meradb;
using meradb_test::TempDir;
using meradb_test::runLast;

namespace {

std::string cell(const Result& r, size_t row, size_t col) { return formatValue(r.rows.at(row).at(col)); }

struct Db {
    TempDir dir;
    Engine e{dir.str()};
    Db() {
        e.execute("BANAO TABLE accounts (id INT MUKHYA KUNJI, balance INT);"
                  "BANAO TABLE audit (id INT, note TEXT);");
    }
    Result q(const std::string& sql) { return runLast(e, sql); }
    std::vector<std::string> audit() {  // "id:note" per row, heap order
        std::vector<std::string> out;
        Result r = q("DIKHAO id, note SE audit;");
        for (size_t i = 0; i < r.rows.size(); ++i) out.push_back(cell(r, i, 0) + ":" + cell(r, i, 1));
        return out;
    }
};

}  // namespace

TEST_CASE("engine_triggers create and drop use Python's messages", "[engine][triggers]") {
    Db d;
    CHECK(d.q("BANAO TRIGGER t1 BAAD DAALO PAR accounts SHURU DIKHAO * SE t; KHATAM;").message ==
          "Trigger 't1' ban gaya (BAAD DAALO PAR accounts)");
    CHECK(d.q("BANAO TRIGGER t1 BAAD DAALO PAR accounts SHURU DIKHAO * SE t; KHATAM;").error ==
          "[Execution Galti] Trigger 't1' pehle se hai");
    CHECK(d.q("BANAO TRIGGER t2 BAAD DAALO PAR ghost SHURU DIKHAO * SE t; KHATAM;").error ==
          "[Execution Galti] Table 'ghost' exist nahi karta -- trigger sirf ek REAL table par lag sakta hai");
    d.q("BANAO VIEW av KAHO DIKHAO * SE accounts;");
    CHECK(d.q("BANAO TRIGGER t3 BAAD DAALO PAR av SHURU DIKHAO * SE t; KHATAM;").error ==
          "[Execution Galti] Table 'av' exist nahi karta -- trigger sirf ek REAL table par lag sakta hai");
    CHECK(d.q("HATAO TRIGGER t1;").message == "Trigger 't1' hata diya");
    CHECK(d.q("HATAO TRIGGER t1;").error == "[Execution Galti] Trigger 't1' exist nahi karta");
}

TEST_CASE("engine_triggers AFTER INSERT fires once per row with NAYA values", "[engine][triggers]") {
    Db d;
    d.q("BANAO TRIGGER t BAAD DAALO PAR accounts SHURU DAALO MEIN audit MAAN (NAYA.id, 'inserted'); KHATAM;");
    CHECK(d.q("DAALO MEIN accounts MAAN (1, 100), (2, 50);").message == "2 row(s) daal di");
    CHECK(d.audit() == std::vector<std::string>{"1:inserted", "2:inserted"});
}

TEST_CASE("engine_triggers UPDATE sees NAYA and PURANA, DELETE sees PURANA", "[engine][triggers]") {
    Db d;
    d.q("DAALO MEIN accounts MAAN (1, 100), (2, 50);");
    d.q("BANAO TRIGGER u BAAD BADLO PAR accounts SHURU DAALO MEIN audit MAAN (PURANA.balance, 'was'); "
        "DAALO MEIN audit MAAN (NAYA.balance, 'now'); KHATAM;");
    d.q("BANAO TRIGGER x BAAD MITAO PAR accounts SHURU DAALO MEIN audit MAAN (PURANA.id, 'gone'); KHATAM;");
    CHECK(d.q("BADLO accounts RAKHO balance = balance + 5 JAHAN id = 1;").message == "1 row(s) badal di");
    CHECK(d.q("MITAO SE accounts JAHAN id = 2;").message == "1 row(s) mita di");
    CHECK(d.audit() == std::vector<std::string>{"100:was", "105:now", "2:gone"});
}

TEST_CASE("engine_triggers fire in creation order, per row", "[engine][triggers]") {
    Db d;
    d.q("BANAO TRIGGER a BAAD DAALO PAR accounts SHURU DAALO MEIN audit MAAN (NAYA.id, 'first'); KHATAM;");
    d.q("BANAO TRIGGER b BAAD DAALO PAR accounts SHURU DAALO MEIN audit MAAN (NAYA.id, 'second'); KHATAM;");
    d.q("DAALO MEIN accounts MAAN (1, 0), (2, 0);");
    CHECK(d.audit() == std::vector<std::string>{"1:first", "1:second", "2:first", "2:second"});
}

TEST_CASE("engine_triggers BEFORE trigger error vetoes the whole statement", "[engine][triggers]") {
    Db d;
    d.q("BANAO TRIGGER veto PEHLE DAALO PAR accounts SHURU MITAO SE nosuch; KHATAM;");
    CHECK(d.q("DAALO MEIN accounts MAAN (1, 1), (2, 2);").error == "[Execution Galti] Table 'nosuch' exist nahi karta");
    CHECK(d.q("DIKHAO * SE accounts;").rows.empty());
}

TEST_CASE("engine_triggers AFTER trigger error does not undo written rows", "[engine][triggers]") {
    Db d;
    d.q("BANAO TRIGGER bad BAAD DAALO PAR accounts SHURU MITAO SE nosuch; KHATAM;");
    CHECK(d.q("DAALO MEIN accounts MAAN (1, 1), (2, 2);").error == "[Execution Galti] Table 'nosuch' exist nahi karta");
    CHECK(d.q("DIKHAO * SE accounts;").rows.size() == 2);  // known limitation, mirrored from Python
}

TEST_CASE("engine_triggers upsert collisions fire UPDATE triggers, fresh rows fire INSERT triggers", "[engine][triggers]") {
    Db d;
    d.q("DAALO MEIN accounts MAAN (1, 10);");
    d.q("BANAO TRIGGER i BAAD DAALO PAR accounts SHURU DAALO MEIN audit MAAN (NAYA.id, 'ins'); KHATAM;");
    d.q("BANAO TRIGGER u BAAD BADLO PAR accounts SHURU DAALO MEIN audit MAAN (NAYA.id, 'upd'); KHATAM;");
    CHECK(d.q("DAALO MEIN accounts MAAN (1, 99), (2, 5) TAKRAAV PAR BADLO balance = balance;").message ==
          "1 row(s) daali, 1 row(s) TAKRAAV par badli");
    // Python order: the updated (colliding) rows finish first, then the fresh rows are inserted
    CHECK(d.audit() == std::vector<std::string>{"1:upd", "2:ins"});
}

TEST_CASE("engine_triggers a trigger writing to its own table shares the index", "[engine][triggers]") {
    Db d;
    d.q("DAALO MEIN accounts MAAN (1, 10);");
    d.q("BANAO TRIGGER mirror BAAD BADLO PAR accounts SHURU DAALO MEIN accounts MAAN (NAYA.id + 100, 0); KHATAM;");
    CHECK(d.q("BADLO accounts RAKHO balance = 11 JAHAN id = 1;").error.empty());
    CHECK(d.q("DIKHAO id SE accounts;").rows.size() == 2);   // 1 and 101
    // the second update tries to insert 101 again: the shared unique index must notice
    CHECK_FALSE(d.q("BADLO accounts RAKHO balance = 12 JAHAN id = 1;").error.empty());
    CHECK(d.q("DIKHAO id SE accounts;").rows.size() == 2);
}

TEST_CASE("engine_triggers survive a restart and belong to their database", "[engine][triggers]") {
    TempDir dir;
    {
        Engine e(dir.str());
        e.execute("BANAO TABLE t (id INT); BANAO TABLE log (id INT);"
                  "BANAO TRIGGER tr BAAD DAALO PAR t SHURU DAALO MEIN log MAAN (NAYA.id); KHATAM;");
    }
    Engine e(dir.str());
    e.execute("DAALO MEIN t MAAN (7);");
    CHECK(cell(runLast(e, "DIKHAO id SE log;"), 0, 0) == "7");
    e.execute("BANAO DATABASE other; ISTEMAL other; BANAO TABLE t (id INT); BANAO TABLE log (id INT);");
    e.execute("DAALO MEIN t MAAN (8);");                      // no trigger in `other`
    CHECK(runLast(e, "DIKHAO id SE log;").rows.empty());
}

TEST_CASE("engine_triggers run with the invoker's privileges", "[engine][triggers]") {
    TempDir dir;
    auto inst = std::make_shared<Instance>(dir.str());
    Engine admin(inst);
    Engine ravi(inst);
    ravi.user = "ravi";
    admin.execute("BANAO USER ravi GUPT 'pw'; BANAO TABLE t (id INT); BANAO TABLE log (id INT);"
                  "BANAO TRIGGER tr BAAD DAALO PAR t SHURU DAALO MEIN log MAAN (NAYA.id); KHATAM;"
                  "ADHIKAR DO DAALO PAR t KO ravi;");
    CHECK(runLast(ravi, "DAALO MEIN t MAAN (1);").error ==
          "[Execution Galti] 'ravi' ko table 'log' par DAALO ka adhikar nahi hai");
    admin.execute("ADHIKAR DO DAALO PAR log KO ravi;");
    CHECK(runLast(ravi, "DAALO MEIN t MAAN (2);").error.empty());
}

TEST_CASE("engine_triggers a self-triggering trigger hits the nesting cap, not the stack", "[engine][triggers]") {
    Db d;
    d.q("DAALO MEIN accounts MAAN (1, 10);");
    d.q("BANAO TRIGGER loop PEHLE BADLO PAR accounts SHURU BADLO accounts RAKHO balance = balance + 1; KHATAM;");
    // run on a plain std::thread: the server's connection threads have the default stack size
    std::string error;
    std::thread worker([&] { error = d.q("BADLO accounts RAKHO balance = 0;").error; });
    worker.join();
    CHECK(error.find("bahut gehra") != std::string::npos);
    CHECK(cell(d.q("DIKHAO balance SE accounts;"), 0, 0) == "10");   // a PEHLE trigger runs before any write
}

TEST_CASE("engine_triggers rollback undoes trigger side effects", "[engine][triggers]") {
    Db d;
    d.q("BANAO TRIGGER t BAAD DAALO PAR accounts SHURU DAALO MEIN audit MAAN (NAYA.id, 'ins'); KHATAM;");
    d.q("SHURU;");
    d.q("DAALO MEIN accounts MAAN (1, 1);");
    CHECK(d.audit().size() == 1);
    d.q("WAPAS;");
    CHECK(d.audit().empty());
    CHECK(d.q("DIKHAO * SE accounts;").rows.empty());
}
```

- [ ] **Step 2: Add to the build and verify it fails**

Add `test_engine_triggers.cpp`; `cmake --build cpp/build` — Expected: FAIL (`BANAO TRIGGER` parses but the engine says "Ye statement abhi supported nahi hai").

- [ ] **Step 3: Header changes** (`engine.h`)

Add `#include "meradb/substitute.h"`. In `Engine` private section:

```cpp
    Result execCreateTrigger(const ast::CreateTrigger&);
    Result execDropTrigger(const ast::DropTrigger&);

    // Trigger / procedure bodies (Design decisions D5). `newRow`/`oldRow` are plain
    // {column: value} dicts, independent of any Scope's "table.col" aliasing.
    void fireTriggers(const std::string& timing, const std::string& event, const std::string& table,
                      const Row* newRow, const Row* oldRow);
    // Parse `bodyText` fresh, substitute, run each statement through executeStatement
    // (so it takes the lock, re-checks privileges and can fire further triggers).
    void runBody(const std::string& bodyText, const RefReplacer& replace, std::vector<Result>* results);
    static Row rowDict(const TableSchema& schema, const std::vector<Value>& values);

    static constexpr int kMaxBodyDepth = 32;
    int bodyDepth_ = 0;  // trigger/procedure bodies currently executing on this session
```

- [ ] **Step 4: Engine implementation** (`engine.cpp`)

Dispatch (in `executeStatement`, before the final `throw`):
```cpp
    if (auto* s = dynamic_cast<const CreateTrigger*>(&stmt)) return execCreateTrigger(*s);
    if (auto* s = dynamic_cast<const DropTrigger*>(&stmt)) return execDropTrigger(*s);
```

New section (after the users section):

```cpp
// ============================================================================
// triggers (fire once per affected row on DAALO/BADLO/MITAO)
// ============================================================================

Result Engine::execCreateTrigger(const ast::CreateTrigger& stmt) {
    Catalog& cat = catalog();
    if (cat.hasTrigger(stmt.name)) throw ExecutionError("Trigger '" + stmt.name + "' pehle se hai");
    if (cat.find(stmt.table) == nullptr)
        throw ExecutionError("Table '" + stmt.table + "' exist nahi karta -- trigger sirf ek REAL table par lag sakta hai");
    parseScript(stmt.bodyText);  // sanity check: the body must parse cleanly
    cat.addTrigger(stmt.name, stmt.timing, stmt.event, stmt.table, stmt.bodyText);
    return messageResult("Trigger '" + stmt.name + "' ban gaya (" + stmt.timing + " " + stmt.event + " PAR " +
                         stmt.table + ")");
}

Result Engine::execDropTrigger(const ast::DropTrigger& stmt) {
    Catalog& cat = catalog();
    if (!cat.hasTrigger(stmt.name)) throw ExecutionError("Trigger '" + stmt.name + "' exist nahi karta");
    cat.removeTrigger(stmt.name);
    return messageResult("Trigger '" + stmt.name + "' hata diya");
}

Row Engine::rowDict(const TableSchema& schema, const std::vector<Value>& values) {
    Row row;
    for (size_t i = 0; i < schema.columns.size() && i < values.size(); ++i) row[schema.columns[i].name] = values[i];
    return row;
}

void Engine::runBody(const std::string& bodyText, const RefReplacer& replace, std::vector<Result>* results) {
    struct DepthGuard {  // local class: same access rights as the enclosing member function
        Engine& engine;
        explicit DepthGuard(Engine& e) : engine(e) {
            if (engine.bodyDepth_ >= kMaxBodyDepth)
                throw ExecutionError(
                    "Trigger/procedure bahut gehra chal raha hai (limit " + std::to_string(kMaxBodyDepth) +
                    ") -- shayad koi trigger khud ko baar-baar chala raha hai");
            ++engine.bodyDepth_;
        }
        ~DepthGuard() { --engine.bodyDepth_; }
    } guard(*this);

    auto statements = parseScript(bodyText);  // fresh parse every time, like Python
    for (auto& stmt : statements) {
        substituteStatementInPlace(*stmt, replace);
        Result r = executeStatement(*stmt);
        if (results) results->push_back(std::move(r));
    }
}

void Engine::fireTriggers(const std::string& timing, const std::string& event, const std::string& table,
                          const Row* newRow, const Row* oldRow) {
    // Copies (Catalog::triggersFor): a body may run DDL that changes the catalog.
    for (const auto& trigger : catalog().triggersFor(timing, event, table)) {
        RefReplacer replace = [newRow, oldRow](const ast::ColumnRef& ref) -> std::optional<Value> {
            if (ref.table && *ref.table == "naya" && newRow) {
                auto it = newRow->find(ref.name);
                if (it != newRow->end()) return it->second;
            }
            if (ref.table && *ref.table == "purana" && oldRow) {
                auto it = oldRow->find(ref.name);
                if (it != oldRow->end()) return it->second;
            }
            return std::nullopt;
        };
        runBody(trigger.bodyText, replace, nullptr);
    }
}
```

Now wire the hooks. The order below is Python's (`engine.py` 1060-1118, 1495-1509,
1534-1540); do not reorder.

`execInsert`, plain insert branch — replace
```cpp
        checkUnique(*t, newRows);
        checkFk(*t, newRows);
        t->insertMany(newRows);
        return messageResult(std::to_string(newRows.size()) + " row(s) daal di");
```
with
```cpp
        checkUnique(*t, newRows);
        checkFk(*t, newRows);
        std::vector<Row> newDicts;
        for (const auto& r : newRows) newDicts.push_back(rowDict(schema, r));
        for (const auto& nr : newDicts) fireTriggers("PEHLE", "DAALO", stmt.table, &nr, nullptr);
        t->insertMany(newRows);
        for (const auto& nr : newDicts) fireTriggers("BAAD", "DAALO", stmt.table, &nr, nullptr);
        return messageResult(std::to_string(newRows.size()) + " row(s) daal di");
```

`execInsert`, upsert branch — after `checkFk(*t, toInsert);` add
```cpp
    std::vector<Row> insertDicts;
    for (const auto& r : toInsert) insertDicts.push_back(rowDict(schema, r));
    for (const auto& nr : insertDicts) fireTriggers("PEHLE", "DAALO", stmt.table, &nr, nullptr);
```
then replace the `if (!updatedNewRows.empty()) { ... }` block and the closing
`t->insertMany(toInsert);` with
```cpp
    if (!updatedNewRows.empty()) {
        std::set<int64_t> ignore;
        for (const auto& [rowId, oldValues] : updatedTargets) {
            (void)oldValues;
            ignore.insert(rowId);
        }
        checkUnique(*t, updatedNewRows, ignore);
        checkFk(*t, updatedNewRows);
        // a TAKRAAV collision is really an UPDATE of an existing row, so it fires
        // BADLO triggers (not DAALO) -- matches real upsert semantics
        std::vector<Row> oldDicts, updatedDicts;
        for (const auto& target : updatedTargets) oldDicts.push_back(rowDict(schema, target.second));
        for (const auto& nv : updatedNewRows) updatedDicts.push_back(rowDict(schema, nv));
        for (size_t i = 0; i < oldDicts.size(); ++i)
            fireTriggers("PEHLE", "BADLO", stmt.table, &updatedDicts[i], &oldDicts[i]);
        t->deleteMany(updatedTargets);
        t->insertMany(updatedNewRows);
        for (size_t i = 0; i < oldDicts.size(); ++i)
            fireTriggers("BAAD", "BADLO", stmt.table, &updatedDicts[i], &oldDicts[i]);
    }
    t->insertMany(toInsert);
    for (const auto& nr : insertDicts) fireTriggers("BAAD", "DAALO", stmt.table, &nr, nullptr);
```

`execUpdate` — replace the two lines `t->deleteMany(targets); t->insertMany(newRows);` (keep the comment above them) with
```cpp
    // Plain {column: value} dicts for trigger NAYA/PURANA substitution --
    // independent of Scope's "table.col" aliasing, which triggers don't use.
    std::vector<Row> oldDicts, newDicts;
    for (const auto& target : targets) oldDicts.push_back(rowDict(schema, target.second));
    for (const auto& nr : newRows) newDicts.push_back(rowDict(schema, nr));
    for (size_t i = 0; i < oldDicts.size(); ++i) fireTriggers("PEHLE", "BADLO", stmt.table, &newDicts[i], &oldDicts[i]);
    t->deleteMany(targets);
    t->insertMany(newRows);
    for (size_t i = 0; i < oldDicts.size(); ++i) fireTriggers("BAAD", "BADLO", stmt.table, &newDicts[i], &oldDicts[i]);
```

`execDelete` — replace `t->deleteMany(doomed);` with
```cpp
    std::vector<Row> oldDicts;
    for (const auto& d : doomed) oldDicts.push_back(rowDict(schema, d.second));
    for (const auto& od : oldDicts) fireTriggers("PEHLE", "MITAO", stmt.table, nullptr, &od);
    t->deleteMany(doomed);
    for (const auto& od : oldDicts) fireTriggers("BAAD", "MITAO", stmt.table, nullptr, &od);
```

Two things a reviewer must check here (both are where a port silently drifts):
(a) the `PEHLE` loops run **after** every validation/uniqueness/FK check and
**before** the write; (b) upsert fires `PEHLE DAALO` for fresh rows *before* the
old rows are read for the update, and `BAAD DAALO` *after* `insertMany(toInsert)`.

- [ ] **Step 5: Build and run**

```bash
cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R "engine_triggers"
```
Expected: 11 tests PASS. If the `std::thread` test crashes (stack overflow), lower
`kMaxBodyDepth` (16 is safe on a 1 MB stack) and note the value in Task 25's
divergence list; do not raise the thread's stack instead (std::thread cannot).

- [ ] **Step 6: Whole suite** — `ctest --test-dir cpp/build --output-on-failure`. Expected: all green (the Phase 1 golden DML tests exercise the modified paths with no triggers defined).

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/engine.h cpp/src/engine.cpp cpp/tests/test_engine_triggers.cpp cpp/tests/CMakeLists.txt
git commit -m "Add triggers with NAYA/PURANA substitution to the engine"
```

**Completion checklist:**
- [ ] hook order verified against `engine.py` for INSERT, upsert, UPDATE, DELETE
- [ ] triggers copied before iteration; body statements run through `executeStatement`
- [ ] nesting cap proven on a `std::thread`; message deliberately differs from Python (recorded in D5)
- [ ] the rollback test proves trigger writes live inside the transaction snapshot

---

### Task 11: Engine — stored procedures

**Files:**
- Modify: `cpp/include/meradb/engine.h`, `cpp/src/engine.cpp`
- Test: `cpp/tests/test_engine_procedures.cpp` (new), `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `runBody` (Task 10), `Catalog::addProcedure/findProcedure` (Task 7), `evaluate`, `coerce`.
- Produces: `BANAO PROCEDURE`, `HATAO PROCEDURE`, `CHALAO`, with Python's messages and semantics (Design decision D5).

- [ ] **Step 1: Write the failing test**

```cpp
// cpp/tests/test_engine_procedures.cpp
#include <catch2/catch_test_macros.hpp>
#include "golden_runner.h"
#include "meradb/engine.h"
#include "test_util.h"

using namespace meradb;
using meradb_test::TempDir;
using meradb_test::runLast;

namespace {
struct Db {
    TempDir dir;
    Engine e{dir.str()};
    Db() { e.execute("BANAO TABLE accounts (id INT MUKHYA KUNJI, balance INT);"); }
    Result q(const std::string& sql) { return runLast(e, sql); }
};
}  // namespace

TEST_CASE("engine_procedures create and drop use Python's messages", "[engine][procedures]") {
    Db d;
    CHECK(d.q("BANAO PROCEDURE p(a INT, b TEXT) SHURU DIKHAO * SE t; KHATAM;").message == "Procedure 'p' ban gaya (2 parameter(s))");
    CHECK(d.q("BANAO PROCEDURE p() SHURU DIKHAO * SE t; KHATAM;").error == "[Execution Galti] Procedure 'p' pehle se hai");
    CHECK(d.q("BANAO PROCEDURE q(a INT, a TEXT) SHURU DIKHAO * SE t; KHATAM;").error ==
          "[Execution Galti] Procedure 'q': ek parameter naam do baar diya hai");
    CHECK(d.q("HATAO PROCEDURE p;").message == "Procedure 'p' hata diya");
    CHECK(d.q("HATAO PROCEDURE p;").error == "[Execution Galti] Procedure 'p' exist nahi karta");
}

TEST_CASE("engine_procedures CHALAO substitutes parameters and reports every message", "[engine][procedures]") {
    Db d;
    d.q("BANAO PROCEDURE add_acct(pid INT, bal INT) SHURU DAALO MEIN accounts MAAN (pid, bal); "
        "BADLO accounts RAKHO balance = balance + bal JAHAN id = pid; KHATAM;");
    Result r = d.q("CHALAO add_acct(5, 10);");
    CHECK(r.message == "Procedure 'add_acct' chal gaya (2 statement(s)): 1 row(s) daal di; 1 row(s) badal di");
    Result rows = d.q("DIKHAO id, balance SE accounts;");
    REQUIRE(rows.rows.size() == 1);
    CHECK(formatValue(rows.rows[0][0]) == "5");
    CHECK(formatValue(rows.rows[0][1]) == "20");
}

TEST_CASE("engine_procedures arguments are constant expressions coerced to the parameter type", "[engine][procedures]") {
    Db d;
    d.q("BANAO TABLE prices (id INT, amount FLOAT);");
    d.q("BANAO PROCEDURE put(pid INT, amt FLOAT) SHURU DAALO MEIN prices MAAN (pid, amt); KHATAM;");
    CHECK(d.q("CHALAO put(1 + 1, 3);").error.empty());               // 3 (INT) widens to 3.0 for a FLOAT parameter
    Result r = d.q("DIKHAO id, amount SE prices;");
    CHECK(formatValue(r.rows.at(0).at(0)) == "2");
    CHECK(formatValue(r.rows.at(0).at(1)) == "3.0");
    CHECK_FALSE(d.q("CHALAO put('x', 1);").error.empty());            // TEXT does not fit an INT parameter
    CHECK_FALSE(d.q("CHALAO put(id, 1);").error.empty());             // no row here: a column reference is an error
}

TEST_CASE("engine_procedures argument count and unknown name errors", "[engine][procedures]") {
    Db d;
    d.q("BANAO PROCEDURE p(a INT, b INT) SHURU DIKHAO * SE t; KHATAM;");
    CHECK(d.q("CHALAO p(1);").error == "[Execution Galti] Procedure 'p' ko 2 argument(s) chahiye, 1 mile");
    CHECK(d.q("CHALAO p(1, 2, 3);").error == "[Execution Galti] Procedure 'p' ko 2 argument(s) chahiye, 3 mile");
    CHECK(d.q("CHALAO nope();").error == "[Execution Galti] Procedure 'nope' exist nahi karta");
}

TEST_CASE("engine_procedures a failing statement keeps the earlier ones applied", "[engine][procedures]") {
    Db d;
    d.q("BANAO PROCEDURE two(pid INT) SHURU DAALO MEIN accounts MAAN (pid, 1); DAALO MEIN nosuch MAAN (1); KHATAM;");
    CHECK(d.q("CHALAO two(7);").error == "[Execution Galti] Table 'nosuch' exist nahi karta");
    CHECK(d.q("DIKHAO * SE accounts;").rows.size() == 1);   // mirrored: no implicit transaction
}

TEST_CASE("engine_procedures can call other procedures and be used by triggers", "[engine][procedures]") {
    Db d;
    d.q("BANAO TABLE audit (id INT, note TEXT);");
    d.q("BANAO PROCEDURE note(i INT, t TEXT) SHURU DAALO MEIN audit MAAN (i, t); KHATAM;");
    d.q("BANAO PROCEDURE outer_p(i INT) SHURU CHALAO note(i, 'from outer'); KHATAM;");
    CHECK(d.q("CHALAO outer_p(3);").message ==
          "Procedure 'outer_p' chal gaya (1 statement(s)): Procedure 'note' chal gaya (1 statement(s)): 1 row(s) daal di");
    d.q("BANAO TRIGGER tr BAAD DAALO PAR accounts SHURU CHALAO note(NAYA.id, 'trigger'); KHATAM;");
    d.q("DAALO MEIN accounts MAAN (9, 0);");
    CHECK(d.q("DIKHAO * SE audit;").rows.size() == 2);
}

TEST_CASE("engine_procedures recursion hits the nesting cap", "[engine][procedures]") {
    Db d;
    d.q("BANAO PROCEDURE r() SHURU CHALAO r(); KHATAM;");
    CHECK(d.q("CHALAO r();").error.find("bahut gehra") != std::string::npos);
}

TEST_CASE("engine_procedures are superuser-only for restricted sessions", "[engine][procedures]") {
    TempDir dir;
    auto inst = std::make_shared<Instance>(dir.str());
    Engine admin(inst), ravi(inst);
    ravi.user = "ravi";
    admin.execute("BANAO USER ravi GUPT 'pw'; BANAO PROCEDURE p() SHURU DIKHAO * SE t; KHATAM;");
    CHECK(runLast(ravi, "CHALAO p();").error ==
          "[Execution Galti] 'ravi' superuser nahi hai -- 'CallProcedure' jaisa DDL/admin command sirf superuser "
          "(bina username connect kiya session) chala sakta hai");
    CHECK(runLast(ravi, "BANAO PROCEDURE z() SHURU DIKHAO * SE t; KHATAM;").error.find("'CreateProcedure'") != std::string::npos);
}
```

- [ ] **Step 2: Add to the build and verify it fails** — Expected: FAIL ("Ye statement abhi supported nahi hai").

- [ ] **Step 3: Header** — private section of `Engine`:

```cpp
    Result execCreateProcedure(const ast::CreateProcedure&);
    Result execDropProcedure(const ast::DropProcedure&);
    Result execCallProcedure(const ast::CallProcedure&);
```

- [ ] **Step 4: Implementation** — dispatch lines in `executeStatement`:

```cpp
    if (auto* s = dynamic_cast<const CreateProcedure*>(&stmt)) return execCreateProcedure(*s);
    if (auto* s = dynamic_cast<const DropProcedure*>(&stmt)) return execDropProcedure(*s);
    if (auto* s = dynamic_cast<const CallProcedure*>(&stmt)) return execCallProcedure(*s);
```

and, after `fireTriggers`:

```cpp
// ============================================================================
// stored procedures (a named, parameterised sequence of statements)
// ============================================================================

Result Engine::execCreateProcedure(const ast::CreateProcedure& stmt) {
    Catalog& cat = catalog();
    if (cat.hasProcedure(stmt.name)) throw ExecutionError("Procedure '" + stmt.name + "' pehle se hai");
    std::set<std::string> names;
    for (const auto& p : stmt.params) names.insert(p.name);
    if (names.size() != stmt.params.size())
        throw ExecutionError("Procedure '" + stmt.name + "': ek parameter naam do baar diya hai");
    parseScript(stmt.bodyText);  // sanity check: the body must parse cleanly
    std::vector<std::pair<std::string, std::string>> params;
    for (const auto& p : stmt.params) params.emplace_back(p.name, p.typeName);
    cat.addProcedure(stmt.name, params, stmt.bodyText);
    return messageResult("Procedure '" + stmt.name + "' ban gaya (" + std::to_string(stmt.params.size()) +
                         " parameter(s))");
}

Result Engine::execDropProcedure(const ast::DropProcedure& stmt) {
    Catalog& cat = catalog();
    if (!cat.hasProcedure(stmt.name)) throw ExecutionError("Procedure '" + stmt.name + "' exist nahi karta");
    cat.removeProcedure(stmt.name);
    return messageResult("Procedure '" + stmt.name + "' hata diya");
}

Result Engine::execCallProcedure(const ast::CallProcedure& stmt) {
    auto proc = catalog().findProcedure(stmt.name);
    if (!proc) throw ExecutionError("Procedure '" + stmt.name + "' exist nahi karta");
    if (stmt.args.size() != proc->params.size())
        throw ExecutionError("Procedure '" + stmt.name + "' ko " + std::to_string(proc->params.size()) +
                             " argument(s) chahiye, " + std::to_string(stmt.args.size()) + " mile");

    // Arguments are evaluated as CONSTANT expressions (no outer row at a bare CHALAO),
    // left to right, each coerced to its parameter's type.
    std::unordered_map<std::string, Value> values;
    for (size_t i = 0; i < proc->params.size(); ++i) {
        const auto& [pname, ptype] = proc->params[i];
        Value raw = evaluate(*stmt.args[i], Row{});
        values[pname] = coerce(raw, ptype, pname);
    }

    // Only a BARE (unqualified) reference matching a parameter name is substituted.
    RefReplacer replace = [&values](const ast::ColumnRef& ref) -> std::optional<Value> {
        if (!ref.table) {
            auto it = values.find(ref.name);
            if (it != values.end()) return it->second;
        }
        return std::nullopt;
    };

    std::vector<Result> results;
    runBody(proc->bodyText, replace, &results);
    std::string summary;
    for (const auto& r : results) {
        if (r.message.empty()) continue;
        if (!summary.empty()) summary += "; ";
        summary += r.message;
    }
    return messageResult("Procedure '" + stmt.name + "' chal gaya (" + std::to_string(results.size()) +
                         " statement(s)): " + summary);
}
```

- [ ] **Step 5: Build and run**

```bash
cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R "engine_procedures|engine_triggers|engine_users"
```
Expected: PASS. Then the full suite.

- [ ] **Step 6: Commit**

```bash
git add cpp/include/meradb/engine.h cpp/src/engine.cpp cpp/tests/test_engine_procedures.cpp cpp/tests/CMakeLists.txt
git commit -m "Add stored procedures to the engine"
```

**Completion checklist:**
- [ ] messages copied from `engine.py` 570-612
- [ ] arguments evaluated left to right into a named map (no right-to-left argument-evaluation dependence)
- [ ] the nested-procedure message shows the inner summary inside the outer one (as in Python)

---

### Task 12: Golden scripts for users, triggers and procedures

**Files:**
- Modify: `cpp/tests/golden/gen_golden.py` (sessions via `@name:` line prefix)
- Modify: `cpp/tests/golden_runner.h` (same prefix, shared `Instance`)
- Create: `cpp/tests/golden/users_basic.mdb`, `users_privileges.mdb`, `trigger_basic.mdb`, `trigger_upsert.mdb`, `trigger_nested.mdb`, `proc_basic.mdb`, `proc_errors.mdb`, `parse_phase2.mdb`
- Regenerate: `cpp/tests/golden_engine.h` (generated, committed)
- Modify: `cpp/tests/test_engine_users.cpp`, `test_engine_triggers.cpp`, `test_engine_procedures.cpp`, `test_parser_phase2.cpp` (one golden test case each)

**Interfaces:**
- Consumes: everything in Tasks 2-11.
- Produces: byte-level agreement with Python on messages, error wording, columns and rows for the Phase 2 features. Format: one statement per line; a line starting `@name:` runs on a **restricted session** for user `name` that shares the script's `Instance` (created on first use); every other line runs on the superuser session. The `sql` string stored in `golden_engine.h` keeps the whole line, prefix included, and both sides parse the prefix identically.

- [ ] **Step 1: Extend the generator** (`gen_golden.py`)

Add `import re` at the top. Replace the per-script block in `main()`:

```python
        with tempfile.TemporaryDirectory() as data:
            engine = Engine(data)
            sessions = {}
            out.append(f'        {{"{group}", "{name}", {{')
            for sql in statements:
                target, text = engine, sql
                m = re.match(r"^@(\w+):\s*(.*)$", sql)
                if m:
                    user, text = m.group(1), m.group(2)
                    if user not in sessions:
                        sessions[user] = Engine(engine.instance)   # shares the Instance (and its lock)
                        sessions[user].user = user
                    target = sessions[user]
                result = target.run_script(text)[-1]
                out.append(f"            {{{raw(sql)}, {raw(canonical(result))}}},")
            out.append("        }},")
```
Update the module docstring with one sentence describing the `@name:` prefix.

- [ ] **Step 2: Extend the C++ runner** (`golden_runner.h`)

Add `#include <map>` and `#include <memory>`, and replace `replayGolden`:

```cpp
// Replay every golden script of `group`. One Instance per script; `@name: sql` lines
// run on a restricted Engine for user `name` created on first use.
inline void replayGolden(const std::string& group) {
    int scriptsRun = 0;
    for (const auto& script : golden::scripts()) {
        if (group != script.group) continue;
        ++scriptsRun;
        TempDir dir;
        auto instance = std::make_shared<meradb::Instance>(dir.str());
        meradb::Engine admin(instance);
        std::map<std::string, std::unique_ptr<meradb::Engine>> sessions;
        int n = 0;
        for (const auto& step : script.steps) {
            ++n;
            INFO("script " << script.group << "_" << script.name << ", step " << n << ": " << step.sql);
            std::string sql = step.sql;
            meradb::Engine* target = &admin;
            if (!sql.empty() && sql[0] == '@') {
                size_t colon = sql.find(':');
                REQUIRE(colon != std::string::npos);
                std::string user = sql.substr(1, colon - 1);
                sql = sql.substr(colon + 1);
                sql.erase(0, sql.find_first_not_of(" \t"));
                auto& slot = sessions[user];
                if (!slot) {
                    slot = std::make_unique<meradb::Engine>(instance);
                    slot->user = user;
                }
                target = slot.get();
            }
            CHECK(canonical(runLast(*target, sql)) == step.expect);
        }
    }
    REQUIRE(scriptsRun > 0);
}
```

- [ ] **Step 3: Write the scripts** (one statement per line; comments are not supported — the generator treats every non-blank line as a statement)

`cpp/tests/golden/users_basic.mdb`:
```
BANAO USER ravi GUPT 'pw1'
BANAO USER ravi GUPT 'pw2'
BANAO USER asha GUPT 'x'
HATAO USER asha
HATAO USER asha
BANAO TABLE students (id INT MUKHYA KUNJI, name TEXT, cgpa FLOAT)
DAALO MEIN students MAAN (1, 'Ravi', 8.5), (2, 'Asha', 9.1)
@ravi: DIKHAO * SE students
ADHIKAR DO DIKHAO PAR students KO ravi
@ravi: DIKHAO * SE students
@ravi: DAALO MEIN students MAAN (3, 'Meera', 7.0)
ADHIKAR DO DAALO, BADLO PAR students KO ravi
@ravi: DAALO MEIN students MAAN (3, 'Meera', 7.0)
@ravi: BADLO students RAKHO cgpa = 6.5 JAHAN id = 3
@ravi: MITAO SE students JAHAN id = 3
ADHIKAR WAPAS BADLO PAR students SE ravi
@ravi: BADLO students RAKHO cgpa = 6.0 JAHAN id = 3
ADHIKAR DO SAB PAR students KO ghost
ADHIKAR WAPAS SAB PAR nothing SE ravi
@ravi: BANAO TABLE x (id INT)
@ravi: HATAO TABLE students
@ravi: SHURU
@ravi: PAKKA
@ravi: WAPAS
@ravi: SAMJHAO DIKHAO * SE students
@ravi: SAMJHAO HATAO TABLE students
@ravi: BANAO USER z GUPT 'z'
@ravi: ADHIKAR DO SAB PAR students KO ravi
@ravi: ISTEMAL main
@ravi: BANAO DATABASE d2
@ravi: BANAO VIEW v KAHO DIKHAO * SE students
@ravi: HATAO USER ravi
DIKHAO * SE students
```

`users_privileges.mdb`:
```
BANAO TABLE a (id INT MUKHYA KUNJI, v TEXT)
BANAO TABLE b (id INT MUKHYA KUNJI, w TEXT)
DAALO MEIN a MAAN (1, 'x'), (2, 'y')
DAALO MEIN b MAAN (1, 'p')
BANAO VIEW av KAHO DIKHAO id, v SE a
BANAO USER u1 GUPT 'p'
ADHIKAR DO DIKHAO PAR a KO u1
@u1: DIKHAO * SE a MILAO b PAR a.id = b.id
ADHIKAR DO DIKHAO PAR b KO u1
@u1: DIKHAO a.v, b.w SE a MILAO b PAR a.id = b.id
@u1: DIKHAO id SE a SANYUKT DIKHAO id SE b
@u1: DIKHAO id SE a SAAJHA DIKHAO id SE b
@u1: DIKHAO * SE av
ADHIKAR DO DIKHAO PAR av KO u1
@u1: DIKHAO * SE av
@u1: DAALO MEIN b DIKHAO id, v SE a
ADHIKAR DO DAALO PAR b KO u1
@u1: DAALO MEIN b DIKHAO id + 10, v SE a
@u1: DIKHAO * SE b
@u1: SAMJHAO DIKHAO * SE b
@u1: SAMJHAO BADLO b RAKHO w = 'q'
@u1: SAMJHAO BANAO TABLE t (id INT)
@u1: CHALAO nothing()
@u1: BANAO PROCEDURE p() SHURU DIKHAO * SE t; KHATAM
@u1: BANAO TRIGGER t BAAD DAALO PAR b SHURU DIKHAO * SE t; KHATAM
@u1: HATAO VIEW av
@u1: DIKHAO * SE nosuchtable
ADHIKAR WAPAS DIKHAO PAR a SE u1
@u1: DIKHAO * SE a
BANAO DATABASE other
ISTEMAL other
@u1: DIKHAO * SE a
```
(The last `@u1` line runs on `u1`'s own session, whose current database is still `main`: it is denied on `main.a` — a session's database is its own.) Note that the `@u1:` engine and the admin engine each have their own `currentDb`; `ISTEMAL other` on the admin changes only the admin session.

`trigger_basic.mdb`:
```
BANAO TABLE accounts (id INT MUKHYA KUNJI, balance INT)
BANAO TABLE audit (id INT, note TEXT)
BANAO TRIGGER t_ins BAAD DAALO PAR accounts SHURU DAALO MEIN audit MAAN (NAYA.id, 'inserted'); KHATAM
BANAO TRIGGER t_ins BAAD DAALO PAR accounts SHURU DIKHAO * SE t; KHATAM
BANAO TRIGGER t_bad BAAD DAALO PAR ghost SHURU DIKHAO * SE t; KHATAM
BANAO VIEW av KAHO DIKHAO * SE accounts
BANAO TRIGGER t_view BAAD DAALO PAR av SHURU DIKHAO * SE t; KHATAM
DAALO MEIN accounts MAAN (1, 100), (2, 50)
DIKHAO * SE audit
BANAO TRIGGER t_upd BAAD BADLO PAR accounts SHURU DAALO MEIN audit MAAN (PURANA.balance, 'was'); DAALO MEIN audit MAAN (NAYA.balance, 'now'); KHATAM
BADLO accounts RAKHO balance = balance + 5 JAHAN id = 1
DIKHAO * SE audit
BANAO TRIGGER t_del BAAD MITAO PAR accounts SHURU DAALO MEIN audit MAAN (PURANA.id, 'gone'); KHATAM
MITAO SE accounts JAHAN id = 2
DIKHAO * SE audit
BANAO TRIGGER t_second BAAD DAALO PAR accounts SHURU DAALO MEIN audit MAAN (NAYA.id, 'second'); KHATAM
DAALO MEIN accounts MAAN (3, 1)
DIKHAO * SE audit
HATAO TRIGGER t_second
HATAO TRIGGER t_second
DAALO MEIN accounts MAAN (4, 1)
DIKHAO * SE audit
BANAO TRIGGER t_veto PEHLE DAALO PAR accounts SHURU MITAO SE nosuch; KHATAM
DAALO MEIN accounts MAAN (9, 9)
DIKHAO * SE accounts
HATAO TRIGGER t_veto
BANAO TRIGGER t_fail BAAD DAALO PAR accounts SHURU MITAO SE nosuch; KHATAM
DAALO MEIN accounts MAAN (10, 10), (11, 11)
DIKHAO * SE accounts
HATAO TRIGGER t_fail
SHURU
DAALO MEIN accounts MAAN (20, 1)
DIKHAO * SE audit
WAPAS
DIKHAO * SE audit
DIKHAO * SE accounts
```

`trigger_upsert.mdb`:
```
BANAO TABLE inv (id INT MUKHYA KUNJI, qty INT)
BANAO TABLE log (id INT, note TEXT)
BANAO TRIGGER i_ins BAAD DAALO PAR inv SHURU DAALO MEIN log MAAN (NAYA.id, 'ins'); KHATAM
BANAO TRIGGER i_upd BAAD BADLO PAR inv SHURU DAALO MEIN log MAAN (NAYA.id, 'upd'); DAALO MEIN log MAAN (PURANA.qty, 'old'); KHATAM
BANAO TRIGGER i_pre PEHLE DAALO PAR inv SHURU DAALO MEIN log MAAN (NAYA.id, 'pre'); KHATAM
BANAO TRIGGER i_prebadlo PEHLE BADLO PAR inv SHURU DAALO MEIN log MAAN (NAYA.qty, 'pre-upd'); KHATAM
DAALO MEIN inv MAAN (1, 10)
DAALO MEIN inv MAAN (1, 20), (2, 5) TAKRAAV PAR BADLO qty = qty
DIKHAO * SE inv
DIKHAO * SE log
DAALO MEIN inv MAAN (1, 30) TAKRAAV PAR BADLO qty = 77
DIKHAO * SE inv
DIKHAO * SE log
```

`trigger_nested.mdb`:
```
BANAO TABLE acc (id INT MUKHYA KUNJI, bal INT)
BANAO TRIGGER mirror BAAD BADLO PAR acc SHURU DAALO MEIN acc MAAN (NAYA.id + 100, 0); KHATAM
DAALO MEIN acc MAAN (1, 10)
BADLO acc RAKHO bal = 11 JAHAN id = 1
DIKHAO * SE acc
BADLO acc RAKHO bal = 12 JAHAN id = 1
DIKHAO * SE acc
BANAO TABLE hist (id INT, note TEXT)
BANAO TRIGGER h1 PEHLE MITAO PAR acc SHURU DAALO MEIN hist MAAN (PURANA.id, 'deleting'); KHATAM
MITAO SE acc JAHAN id > 100
DIKHAO * SE hist
DIKHAO * SE acc
HATAO TABLE hist
MITAO SE acc JAHAN id = 1
DIKHAO * SE acc
```
(after `HATAO TABLE hist` the orphan trigger `h1` remains and now fails: Python mirrors this "orphan trigger" behaviour.)

`proc_basic.mdb`:
```
BANAO TABLE accounts (id INT MUKHYA KUNJI, balance INT)
BANAO TABLE prices (id INT, amount FLOAT)
BANAO PROCEDURE add_acct(pid INT, bal INT) SHURU DAALO MEIN accounts MAAN (pid, bal); BADLO accounts RAKHO balance = balance + bal JAHAN id = pid; KHATAM
CHALAO add_acct(5, 10)
DIKHAO * SE accounts
BANAO PROCEDURE put(pid INT, amt FLOAT) SHURU DAALO MEIN prices MAAN (pid, amt); KHATAM
CHALAO put(1 + 1, 3)
DIKHAO * SE prices
BANAO PROCEDURE note(i INT) SHURU DIKHAO i SE accounts; KHATAM
BANAO PROCEDURE outer_p(i INT) SHURU CHALAO put(i, 2.5); KHATAM
CHALAO outer_p(7)
DIKHAO * SE prices
BANAO PROCEDURE noargs() SHURU DIKHAO * SE accounts; MITAO SE accounts JAHAN id = 5; KHATAM
CHALAO noargs()
DIKHAO * SE accounts
HATAO PROCEDURE noargs
CHALAO noargs()
BANAO PROCEDURE shadow(balance INT) SHURU DIKHAO * SE accounts JAHAN balance = balance; KHATAM
CHALAO shadow(1)
```
(The last two lines exercise the documented name-collision caveat: a bare column named like a parameter is replaced.)

`proc_errors.mdb`:
```
BANAO TABLE t (id INT, name TEXT)
BANAO PROCEDURE p(a INT, b TEXT) SHURU DAALO MEIN t MAAN (a, b); KHATAM
BANAO PROCEDURE p() SHURU DIKHAO * SE t; KHATAM
BANAO PROCEDURE q(a INT, a TEXT) SHURU DIKHAO * SE t; KHATAM
BANAO PROCEDURE r(a nonsense) SHURU DIKHAO * SE t; KHATAM
BANAO PROCEDURE s() SHURU KHATAM
BANAO PROCEDURE u() SHURU DAALO ; KHATAM
CHALAO p(1)
CHALAO p(1, 'x', 3)
CHALAO p('x', 'y')
CHALAO p(1, 2)
CHALAO p(id, 'y')
CHALAO p(1, 'ok')
DIKHAO * SE t
CHALAO nope()
BANAO PROCEDURE two(pid INT) SHURU DAALO MEIN t MAAN (pid, 'first'); DAALO MEIN nosuch MAAN (1); KHATAM
CHALAO two(7)
DIKHAO * SE t
HATAO PROCEDURE p
HATAO PROCEDURE p
```

`parse_phase2.mdb` (parse errors; group `parse`, so it is replayed by the existing "golden parse scripts" test case):
```
BANAO USER ravi
BANAO USER ravi GUPT secret
BANAO USER GUPT 'x'
ADHIKAR
ADHIKAR DO CHALAO PAR t KO u
ADHIKAR DO DIKHAO PAR t SE u
ADHIKAR WAPAS DIKHAO PAR t KO u
ADHIKAR DO PAR t KO u
HATAO USER
HATAO TRIGGER
HATAO PROCEDURE 5
CHALAO p
CHALAO p(1,
CHALAO p(1 2)
BANAO TRIGGER x DAALO PAR t SHURU DIKHAO * SE t; KHATAM
BANAO TRIGGER x PEHLE SAAF PAR t SHURU DIKHAO * SE t; KHATAM
BANAO TRIGGER x PEHLE DAALO t SHURU DIKHAO * SE t; KHATAM
BANAO TRIGGER x PEHLE DAALO PAR t DIKHAO * SE t; KHATAM
BANAO TRIGGER x PEHLE DAALO PAR t SHURU KHATAM
BANAO TRIGGER x PEHLE DAALO PAR t SHURU DIKHAO * SE t;
BANAO TRIGGER x PEHLE DAALO PAR t SHURU DIKHAO * SE t KHATAM
BANAO TRIGGER x PEHLE DAALO PAR t SHURU DAALO ; KHATAM
BANAO PROCEDURE p SHURU DIKHAO * SE t; KHATAM
BANAO PROCEDURE p(x) SHURU DIKHAO * SE t; KHATAM
BANAO PROCEDURE p(x INT SHURU DIKHAO * SE t; KHATAM
BANAO PROCEDURE p(x INT,) SHURU DIKHAO * SE t; KHATAM
BANAO PROCEDURE p() DIKHAO * SE t; KHATAM
```

- [ ] **Step 4: Regenerate** — from the repo root (the generator imports the Python engine):

```bash
python cpp/tests/golden/gen_golden.py
git diff --stat cpp/tests/golden_engine.h
```
Expected: `wrote .../golden_engine.h`; only additions for the new scripts (the
existing Phase 1 entries are unchanged — check that `git diff` shows no `-` lines
except the header if any).

- [ ] **Step 5: Add the replay test cases** (one line of body each):

```cpp
// test_engine_users.cpp
TEST_CASE("engine_users golden scripts match the Python engine", "[engine][users][golden]") {
    meradb_test::replayGolden("users");
}
// test_engine_triggers.cpp
TEST_CASE("engine_triggers golden scripts match the Python engine", "[engine][triggers][golden]") {
    meradb_test::replayGolden("trigger");
}
// test_engine_procedures.cpp
TEST_CASE("engine_procedures golden scripts match the Python engine", "[engine][procedures][golden]") {
    meradb_test::replayGolden("proc");
}
```
The `parse` group is already replayed by an existing Phase 1 test case; confirm with
`grep -rn 'replayGolden("parse")' cpp/tests` and add the same one-liner to
`test_parser_phase2.cpp` only if nothing replays it.

- [ ] **Step 6: Build and run**

```bash
cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R golden
```
Expected: PASS. **Every mismatch is a C++ bug** (or, rarely, a documented
divergence): read the `INFO` line — it names script and step — then fix the C++
code, never the generated header. Likely first failures: a parse-error column
(byte vs code point), the privilege message for a statement class name, or
message spelling in a trigger error path.

- [ ] **Step 7: Commit**

```bash
git add cpp/tests/golden cpp/tests/golden_engine.h cpp/tests/golden_runner.h cpp/tests/test_engine_users.cpp cpp/tests/test_engine_triggers.cpp cpp/tests/test_engine_procedures.cpp cpp/tests/test_parser_phase2.cpp
git commit -m "Add golden scripts for users, triggers and procedures"
```

**Completion checklist:**
- [ ] `golden_engine.h` regenerated, no hand edits
- [ ] every new script step passes with the Python-recorded output
- [ ] the `@name:` prefix is documented in `gen_golden.py`'s docstring
- [ ] end of Batch A: full suite green, zero warnings, `git diff <base> -- meradb` empty

---

# BATCH B — network layer: sockets, protocol, server, client (Tasks 13-19)

Everything from here on links `ws2_32` on Windows (already added in Task 1) and
uses `Threads::Threads` (same task). Every new source file goes into
`add_library(meradb_core ...)` in `cpp/CMakeLists.txt`, every new test file into
`add_executable(meradb_tests ...)` in `cpp/tests/CMakeLists.txt` (before the
"appended by later tasks" comment in both).

### Task 13: `net_compat` — portable sockets

**Files:**
- Create: `cpp/include/meradb/net_compat.h`, `cpp/src/net_compat.cpp`
- Test: `cpp/tests/test_net_compat.cpp`
- Modify: `cpp/CMakeLists.txt` (add `src/net_compat.cpp`), `cpp/tests/CMakeLists.txt` (add `test_net_compat.cpp`)

**Interfaces:**
- Consumes: nothing from the engine (this is the only file pair that includes
  `<winsock2.h>` / `<sys/socket.h>`).
- Produces (namespace `meradb::net`):
  ```cpp
  class NetError : public std::runtime_error;            // every failure below
  class Socket {                                          // move-only RAII; handle stored as std::intptr_t
      bool valid() const; void close(); void shutdownBoth();
      bool waitReadable(double seconds);                  // select(); true = data or EOF is ready
      void sendAll(const std::string&);                   // loops until every byte is out
      IoResult recvSome(char* buffer, std::size_t capacity); // 0 = orderly EOF
      void setReceiveTimeout(double seconds);
      std::string peerAddress() const; int peerPort() const; int localPort() const;
  };
  Socket connectTo(const std::string& host, int port, double timeoutSeconds);   // non-blocking connect + select
  Socket listenOn(const std::string& host, int port, int backlog);              // port 0 = OS chooses
  std::optional<Socket> acceptWithTimeout(Socket& listener, double seconds);    // nullopt on timeout
  bool portOpen(const std::string& host, int port, double timeoutSeconds);
  ```
- Design points the code must keep (they come from real failures found while
  prototyping): on Windows `shutdown()` does **not** wake a thread blocked in
  `recv`, so nothing in this project relies on it for shutdown — threads poll
  with `waitReadable` (D1/D2); a failed non-blocking `connect` shows up in the
  *exception* set of `select` on Windows and in the *write* set on POSIX, so
  `connectTo` checks both and then `SO_ERROR`; Windows listens with
  `SO_EXCLUSIVEADDRUSE`, POSIX with `SO_REUSEADDR`; `SIGPIPE` is ignored once
  per process and `MSG_NOSIGNAL` is used where it exists.

- [ ] **Step 1: Write the failing test** — `cpp/tests/test_net_compat.cpp`

```cpp
// cpp/tests/test_net_compat.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/net_compat.h"
#include <algorithm>
#include <string>
#include <thread>

using namespace meradb::net;

namespace {

// Reads exactly `count` bytes (recvSome may return fewer per call).
std::string readExactly(Socket& s, std::size_t count) {
    std::string out;
    char buffer[256];
    while (out.size() < count) {
        std::size_t n = s.recvSome(buffer, std::min(sizeof buffer, count - out.size()));
        if (n == 0) break;
        out.append(buffer, n);
    }
    return out;
}

}  // namespace

TEST_CASE("net_compat listens on an OS-chosen port and accepts a client", "[net]") {
    Socket listener = listenOn("127.0.0.1", 0);
    int port = listener.localPort();
    REQUIRE(port > 0);

    Socket client = connectTo("127.0.0.1", port, 2.0);
    Socket served = acceptWithTimeout(listener, 2.0);
    REQUIRE(served.valid());
    CHECK(served.peerAddress() == "127.0.0.1");
    CHECK(served.peerPort() == client.localPort());

    client.sendAll("hello\n");
    CHECK(readExactly(served, 6) == "hello\n");
    served.sendAll("world");
    CHECK(readExactly(client, 5) == "world");
}

TEST_CASE("net_compat carries a payload larger than the socket buffers", "[net]") {
    Socket listener = listenOn("127.0.0.1", 0);
    Socket client = connectTo("127.0.0.1", listener.localPort(), 2.0);
    Socket served = acceptWithTimeout(listener, 2.0);
    const std::string big(3 * 1024 * 1024, 'x');  // sendAll must loop over partial sends
    std::thread writer([&] { client.sendAll(big); });
    std::string got;
    char buffer[65536];
    while (got.size() < big.size()) {
        std::size_t n = served.recvSome(buffer, sizeof buffer);
        REQUIRE(n > 0);
        got.append(buffer, n);
    }
    writer.join();
    CHECK(got == big);
}

TEST_CASE("net_compat accept times out with an invalid socket", "[net]") {
    Socket listener = listenOn("127.0.0.1", 0);
    Socket none = acceptWithTimeout(listener, 0.05);
    CHECK_FALSE(none.valid());
}

TEST_CASE("net_compat connecting to a closed port throws", "[net]") {
    int port;
    {
        Socket listener = listenOn("127.0.0.1", 0);
        port = listener.localPort();
    }  // closed again: nothing listens here now
    CHECK_THROWS_AS(connectTo("127.0.0.1", port, 2.0), NetError);
    CHECK_FALSE(portOpen("127.0.0.1", port, 0.5));
}

TEST_CASE("net_compat portOpen sees a live listener", "[net]") {
    Socket listener = listenOn("127.0.0.1", 0);
    CHECK(portOpen("127.0.0.1", listener.localPort(), 1.0));
}

TEST_CASE("net_compat refuses a second listener on a busy port", "[net]") {
    Socket first = listenOn("127.0.0.1", 0);
    CHECK_THROWS_AS(listenOn("127.0.0.1", first.localPort()), NetError);
}

TEST_CASE("net_compat receive timeout reports 'timed out'", "[net]") {
    Socket listener = listenOn("127.0.0.1", 0);
    Socket client = connectTo("127.0.0.1", listener.localPort(), 2.0);
    Socket served = acceptWithTimeout(listener, 2.0);
    client.setReceiveTimeout(0.1);
    char buffer[8];
    try {
        client.recvSome(buffer, sizeof buffer);
        FAIL("expected a timeout");
    } catch (const NetError& e) {
        CHECK(std::string(e.what()) == "timed out");
    }
}

TEST_CASE("net_compat recvSome returns 0 after the peer closes", "[net]") {
    Socket listener = listenOn("127.0.0.1", 0);
    Socket client = connectTo("127.0.0.1", listener.localPort(), 2.0);
    Socket served = acceptWithTimeout(listener, 2.0);
    client.close();
    char buffer[8];
    CHECK(served.recvSome(buffer, sizeof buffer) == 0);
}

TEST_CASE("net_compat waitReadable times out then sees data then end-of-file", "[net]") {
    Socket listener = listenOn("127.0.0.1", 0);
    Socket client = connectTo("127.0.0.1", listener.localPort(), 2.0);
    Socket served = acceptWithTimeout(listener, 2.0);
    CHECK_FALSE(served.waitReadable(0.05));
    client.sendAll("x");
    CHECK(served.waitReadable(2.0));
    char buffer[8];
    CHECK(served.recvSome(buffer, sizeof buffer) == 1);
    client.close();
    CHECK(served.waitReadable(2.0));  // a closed peer is "readable": recvSome returns 0
    CHECK(served.recvSome(buffer, sizeof buffer) == 0);
}

TEST_CASE("net_compat shutdownBoth makes the peer see end-of-file", "[net]") {
    Socket listener = listenOn("127.0.0.1", 0);
    Socket client = connectTo("127.0.0.1", listener.localPort(), 2.0);
    Socket served = acceptWithTimeout(listener, 2.0);
    served.shutdownBoth();
    char buffer[8];
    client.setReceiveTimeout(2.0);
    CHECK(client.recvSome(buffer, sizeof buffer) == 0);
    Socket never;
    never.shutdownBoth();  // an invalid socket is ignored, never throws
}

TEST_CASE("net_compat Socket is move-only and closes exactly once", "[net]") {
    Socket a = listenOn("127.0.0.1", 0);
    int port = a.localPort();
    Socket b = std::move(a);
    CHECK_FALSE(a.valid());  // NOLINT: moved-from state is specified
    CHECK(b.valid());
    CHECK(b.localPort() == port);
    b.close();
    CHECK_FALSE(b.valid());
    b.close();  // harmless
}
```

- [ ] **Step 2: Add the test to the build and verify it fails**

In `cpp/tests/CMakeLists.txt` add `test_net_compat.cpp` to `meradb_tests`, then:

```bash
cmake -S cpp -B cpp/build -G "MinGW Makefiles"
cmake --build cpp/build
```
Expected: FAIL — `meradb/net_compat.h: No such file or directory`.

- [ ] **Step 3: Header** — `cpp/include/meradb/net_compat.h`

```cpp
// cpp/include/meradb/net_compat.h
//
// The only place that knows about Winsock vs BSD sockets. IPv4 TCP, blocking
// I/O, RAII. Public header: no <winsock2.h> / <sys/socket.h> here, the
// native descriptor is carried as a std::intptr_t.
#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace meradb::net {

// what() is the operating system's error text (or "timed out").
class NetError : public std::runtime_error {
public:
    explicit NetError(const std::string& message) : std::runtime_error(message) {}
};

// A move-only owner of one TCP socket (a connection or a listener).
class Socket {
public:
    Socket() = default;
    explicit Socket(std::intptr_t handle) : handle_(handle) {}
    ~Socket() { close(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept : handle_(other.handle_) { other.handle_ = kInvalid; }
    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = other.handle_;
            other.handle_ = kInvalid;
        }
        return *this;
    }

    bool valid() const { return handle_ != kInvalid; }
    std::intptr_t handle() const { return handle_; }

    // Closes the descriptor. Never call it while another thread is using this
    // socket: stop and join that thread first.
    void close();
    // Half-closes both directions so the PEER sees end-of-file. Never throws.
    // (It does NOT reliably wake a local thread blocked in recvSome -- on
    // Windows a blocked recv ignores it. Threads that must be stoppable poll
    // with waitReadable() instead.)
    void shutdownBoth();
    // Waits up to `seconds` until a recvSome() would not block (data arrived
    // or the peer closed). Returns false on timeout. Throws NetError.
    bool waitReadable(double seconds);

    // Sends every byte (looping over partial sends). Throws NetError.
    void sendAll(const std::string& data);
    // Reads at most `capacity` bytes. Returns 0 when the peer closed the
    // connection. Throws NetError on an error or when the receive timeout
    // (see setReceiveTimeout) expires ("timed out").
    std::size_t recvSome(char* buffer, std::size_t capacity);
    // 0 = wait forever.
    void setReceiveTimeout(double seconds);

    std::string peerAddress() const;  // "127.0.0.1", or "?" if unknown
    int peerPort() const;             // 0 if unknown
    int localPort() const;            // 0 if unknown

    static constexpr std::intptr_t kInvalid = -1;

private:
    std::intptr_t handle_ = kInvalid;
};

// Connects to host:port (IPv4; `host` may be a name). Throws NetError
// ("timed out", "Connection refused", ...).
Socket connectTo(const std::string& host, int port, double timeoutSeconds);

// Binds and listens. An empty host means every interface; port 0 lets the OS
// choose (read it back with Socket::localPort). Windows uses
// SO_EXCLUSIVEADDRUSE (a second server can never share the port), POSIX uses
// SO_REUSEADDR. Throws NetError.
Socket listenOn(const std::string& host, int port, int backlog = 64);

// Waits up to `timeoutSeconds` for a connection; returns an invalid Socket on
// timeout. The accepted socket has TCP_NODELAY set. Throws NetError.
Socket acceptWithTimeout(Socket& listener, double timeoutSeconds);

// "Is something listening?" -- a real connect attempt, like protocol.port_open.
bool portOpen(const std::string& host, int port, double timeoutSeconds = 0.5);

}  // namespace meradb::net
```

- [ ] **Step 4: Implementation** — `cpp/src/net_compat.cpp`

```cpp
// cpp/src/net_compat.cpp
#include "meradb/net_compat.h"
#include <cmath>
#include <csignal>
#include <cstring>
#include <system_error>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace meradb::net {

namespace {

#ifdef _WIN32
using Native = SOCKET;
using SockLen = int;
using IoLen = int;
using IoCap = int;
constexpr Native kBadNative = INVALID_SOCKET;
int lastError() { return WSAGetLastError(); }
void closeNative(Native s) { ::closesocket(s); }
bool isTimeout(int e) { return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK; }
bool isInterrupted(int) { return false; }
#else
using Native = int;
using SockLen = socklen_t;
using IoLen = ssize_t;
using IoCap = std::size_t;
constexpr Native kBadNative = -1;
int lastError() { return errno; }
void closeNative(Native s) { ::close(s); }
bool isTimeout(int e) { return e == EAGAIN || e == EWOULDBLOCK; }
bool isInterrupted(int e) { return e == EINTR; }
#endif

Native toNative(std::intptr_t h) { return static_cast<Native>(h); }

// The operating system's own wording ("Connection refused", ...).
std::string errorText(int code) { return std::system_category().message(code); }

// One-time process setup: Winsock on Windows; on POSIX a peer that vanished
// must produce an error from send(), not kill the process with SIGPIPE.
void ensureInit() {
    static const bool done = [] {
#ifdef _WIN32
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw NetError("Winsock start nahi hua");
#else
        std::signal(SIGPIPE, SIG_IGN);
#endif
        return true;
    }();
    (void)done;
}

sockaddr_in resolveV4(const std::string& host, int port, bool passive) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (passive) hints.ai_flags = AI_PASSIVE;
    addrinfo* found = nullptr;
    const std::string portText = std::to_string(port);
    const char* node = host.empty() ? nullptr : host.c_str();
    int rc = ::getaddrinfo(node, portText.c_str(), &hints, &found);
    if (rc != 0 || found == nullptr) throw NetError("address nahi mila: " + host);
    sockaddr_in out{};
    std::memcpy(&out, found->ai_addr, sizeof out);
    ::freeaddrinfo(found);
    return out;
}

void setNonBlocking(Native s, bool on) {
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    ::ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = ::fcntl(s, F_GETFL, 0);
    ::fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

void setNoDelay(Native s) {
    int one = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
}

timeval toTimeval(double seconds) {
    timeval tv{};
    double whole = std::floor(seconds);
    tv.tv_sec = static_cast<decltype(tv.tv_sec)>(whole);
    tv.tv_usec = static_cast<decltype(tv.tv_usec)>((seconds - whole) * 1e6);
    return tv;
}

// select() until the socket is readable (or, with `forWrite`, writable).
// Failed connects show up in the "exceptional" set on Windows, so that set is
// always watched and counts as "ready" (the caller then reads SO_ERROR).
bool waitReady(Native s, bool forWrite, double timeoutSeconds) {
#ifndef _WIN32
    if (s >= FD_SETSIZE) throw NetError("bahut zyada connections khule hain");
#endif
    fd_set set;
    fd_set except;
    FD_ZERO(&set);
    FD_ZERO(&except);
    FD_SET(s, &set);
    FD_SET(s, &except);
    timeval tv = toTimeval(timeoutSeconds < 0 ? 0 : timeoutSeconds);
    int rc = ::select(static_cast<int>(s) + 1, forWrite ? nullptr : &set, forWrite ? &set : nullptr, &except, &tv);
    if (rc < 0) {
        if (isInterrupted(lastError())) return false;
        throw NetError(errorText(lastError()));
    }
    return rc > 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Socket
// ---------------------------------------------------------------------------

void Socket::close() {
    if (handle_ == kInvalid) return;
    closeNative(toNative(handle_));
    handle_ = kInvalid;
}

void Socket::shutdownBoth() {
    if (handle_ == kInvalid) return;
#ifdef _WIN32
    ::shutdown(toNative(handle_), SD_BOTH);
#else
    ::shutdown(toNative(handle_), SHUT_RDWR);
#endif
}

bool Socket::waitReadable(double seconds) {
    if (handle_ == kInvalid) throw NetError("socket band hai");
    return waitReady(toNative(handle_), false, seconds);
}

void Socket::sendAll(const std::string& data) {
    if (handle_ == kInvalid) throw NetError("socket band hai");
    std::size_t sent = 0;
    while (sent < data.size()) {
        std::size_t chunk = data.size() - sent;
        if (chunk > 1u << 20) chunk = 1u << 20;
        IoLen n = ::send(toNative(handle_), data.data() + sent, static_cast<IoCap>(chunk), 0);
        if (n < 0) {
            int e = lastError();
            if (isInterrupted(e)) continue;
            throw NetError(errorText(e));
        }
        sent += static_cast<std::size_t>(n);
    }
}

std::size_t Socket::recvSome(char* buffer, std::size_t capacity) {
    if (handle_ == kInvalid) throw NetError("socket band hai");
    for (;;) {
        IoLen n = ::recv(toNative(handle_), buffer, static_cast<IoCap>(capacity), 0);
        if (n >= 0) return static_cast<std::size_t>(n);
        int e = lastError();
        if (isInterrupted(e)) continue;
        if (isTimeout(e)) throw NetError("timed out");
        throw NetError(errorText(e));
    }
}

void Socket::setReceiveTimeout(double seconds) {
    if (handle_ == kInvalid) return;
#ifdef _WIN32
    DWORD ms = seconds <= 0 ? 0 : static_cast<DWORD>(seconds * 1000.0);
    ::setsockopt(toNative(handle_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
#else
    timeval tv = toTimeval(seconds <= 0 ? 0 : seconds);
    ::setsockopt(toNative(handle_), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

std::string Socket::peerAddress() const {
    if (handle_ == kInvalid) return "?";
    sockaddr_in addr{};
    SockLen len = sizeof addr;
    if (::getpeername(toNative(handle_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return "?";
    char text[64] = {0};
    if (::inet_ntop(AF_INET, &addr.sin_addr, text, sizeof text) == nullptr) return "?";
    return text;
}

int Socket::peerPort() const {
    if (handle_ == kInvalid) return 0;
    sockaddr_in addr{};
    SockLen len = sizeof addr;
    if (::getpeername(toNative(handle_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

int Socket::localPort() const {
    if (handle_ == kInvalid) return 0;
    sockaddr_in addr{};
    SockLen len = sizeof addr;
    if (::getsockname(toNative(handle_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) return 0;
    return ntohs(addr.sin_port);
}

// ---------------------------------------------------------------------------
// free functions
// ---------------------------------------------------------------------------

Socket connectTo(const std::string& host, int port, double timeoutSeconds) {
    ensureInit();
    sockaddr_in addr = resolveV4(host, port, false);
    Native s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBadNative) throw NetError(errorText(lastError()));
    Socket sock(static_cast<std::intptr_t>(s));  // owned (and closed on any throw) from here on

    setNonBlocking(s, true);
    if (::connect(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        int e = lastError();
#ifdef _WIN32
        bool inProgress = (e == WSAEWOULDBLOCK);
#else
        bool inProgress = (e == EINPROGRESS || e == EINTR);
#endif
        if (!inProgress) throw NetError(errorText(e));
        if (!waitReady(s, true, timeoutSeconds)) throw NetError("timed out");
        int soError = 0;
        SockLen len = sizeof soError;
        ::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len);
        if (soError != 0) throw NetError(errorText(soError));
    }
    setNonBlocking(s, false);
    setNoDelay(s);
    return sock;
}

Socket listenOn(const std::string& host, int port, int backlog) {
    ensureInit();
    sockaddr_in addr = resolveV4(host, port, true);
    Native s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s == kBadNative) throw NetError(errorText(lastError()));
    Socket sock(static_cast<std::intptr_t>(s));

    int one = 1;
#ifdef _WIN32
    ::setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&one), sizeof one);
#else
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) throw NetError(errorText(lastError()));
    if (::listen(s, backlog) != 0) throw NetError(errorText(lastError()));
    return sock;
}

Socket acceptWithTimeout(Socket& listener, double timeoutSeconds) {
    if (!listener.valid()) throw NetError("listener band hai");
    Native l = toNative(listener.handle());
    if (!waitReady(l, false, timeoutSeconds)) return Socket();
    Native c = ::accept(l, nullptr, nullptr);
    if (c == kBadNative) {
        int e = lastError();
        if (isInterrupted(e) || isTimeout(e)) return Socket();
#ifndef _WIN32
        if (e == ECONNABORTED) return Socket();
#endif
        throw NetError(errorText(e));
    }
    Socket accepted(static_cast<std::intptr_t>(c));
    setNoDelay(c);
    return accepted;
}

bool portOpen(const std::string& host, int port, double timeoutSeconds) {
    try {
        Socket s = connectTo(host, port, timeoutSeconds);
        return s.valid();
    } catch (const NetError&) {
        return false;
    }
}

}  // namespace meradb::net
```

- [ ] **Step 5: CMake** — in `cpp/CMakeLists.txt` add `src/net_compat.cpp` to `meradb_core`.

- [ ] **Step 6: Build and run**

```bash
cmake -S cpp -B cpp/build -G "MinGW Makefiles"
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R net_compat
```
Expected: 11 test cases pass, no warnings.

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/net_compat.h cpp/src/net_compat.cpp cpp/tests/test_net_compat.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add net_compat: portable TCP sockets for the server and client"
```

**Completion checklist:**
- [ ] no `<winsock2.h>`, `<sys/socket.h>` or `closesocket` outside `net_compat.*`
      (`grep -rn "winsock2\|sys/socket" cpp/src cpp/include cpp/tests` lists only these two files)
- [ ] every `int`/`long` returned from `send`/`recv` is converted with an explicit cast (zero MSVC C4267/C4244 warnings on review)
- [ ] the tests use port 0 (never a fixed port)
- [ ] full suite green, zero warnings

---

### Task 14: `pyjson` and `protocol` — codec, framing, pid file

**Files:**
- Create: `cpp/include/meradb/pyjson.h`, `cpp/src/pyjson.cpp`, `cpp/include/meradb/protocol.h`, `cpp/src/protocol.cpp`
- Test: `cpp/tests/test_pyjson.cpp`, `cpp/tests/test_protocol.cpp`
- Modify: `cpp/CMakeLists.txt` (add both `.cpp`), `cpp/tests/CMakeLists.txt` (add both tests)

**Interfaces:**
- Consumes: `net::Socket` (Task 13), `Result` / `Value` (Phase 1), `sys::getEnv`, `pyReprFloat`
  (`datatypes.h`), `nlohmann::ordered_json`.
- Produces:
  - `pyjson::Json` (an `ordered_json`), `pyjson::dump(const Json&, int indent = -1, char indentChar = ' ', bool trailingNewline = false)`
    producing **exactly Python's `json.dumps` bytes** (`", "` / `": "` separators,
    `ensure_ascii` with lowercase `\uXXXX` and surrogate pairs, DEL as `\u007f`,
    floats via `pyReprFloat`, bare `Infinity` / `-Infinity` / `NaN`), and
    `pyjson::parse(text)` accepting what Python's `json.loads` accepts (including those
    bare constants), throwing `pyjson::ParseFailure`. Depth is capped
    (`kMaxParseDepth = 512`) so a hostile line cannot overflow the stack.
  - `protocol::MessageReader(socket, maxBytes, stopFlag)` with `receive()`
    (`nullopt` = peer closed or stop flag set), `protocol::send`, `valueToWire` /
    `valueFromWire`, `resultToJson` / `resultFromJson` (`Result.to_dict` /
    `from_dict`), `defaultDataDir`, the pid-file functions, `runningServer`.
- Python facts this task encodes (all verified by running the Python code):
  an unterminated last line is still a message; the size limit counts the
  newline; `Message bahut bada hai` is answered once and the rest of that
  overlong line is discarded; DATE cells travel as `{"$date": "YYYY-MM-DD"}`;
  the pid file is `json.dump(indent=2)` with no trailing newline.

- [ ] **Step 1: Write the failing tests**

`cpp/tests/test_pyjson.cpp`:

```cpp
// cpp/tests/test_pyjson.cpp
//
// Expected strings were produced by Python's json.dumps (see the task text);
// they are the wire format both engines must speak.
#include <catch2/catch_test_macros.hpp>
#include "meradb/pyjson.h"
#include <cmath>
#include <limits>

using namespace meradb::pyjson;

TEST_CASE("pyjson dump matches json.dumps for a Result message", "[pyjson]") {
    // json.dumps of a real one-row result: unicode, an astral character, a quote, a backslash-free DEL
    Json cell_text = std::string("h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x98\x80 \\\" \x7f");
    Json date = Json::object();
    date["$date"] = "2024-01-05";
    Json row = Json::array({1.0, date, cell_text, true, nullptr});
    Json result = Json::object();
    result["columns"] = Json::array({"a", "d", "t", "b", "n"});
    result["rows"] = Json::array({row});
    result["message"] = "1 row(s)";
    result["error"] = "";
    Json message = Json::object();
    message["ok"] = true;
    message["results"] = Json::array({result});

    CHECK(dump(message) ==
          "{\"ok\": true, \"results\": [{\"columns\": [\"a\", \"d\", \"t\", \"b\", \"n\"], \"rows\": "
          "[[1.0, {\"$date\": \"2024-01-05\"}, \"h\\u00e9llo \\u4e16\\u754c \\ud83d\\ude00 \\\\\\\" \\u007f\", true, null]], "
          "\"message\": \"1 row(s)\", \"error\": \"\"}]}");
}

TEST_CASE("pyjson round-trips Python's own output byte for byte", "[pyjson]") {
    // json.dumps([1e20, 1e-5, -0.0, 0.1, 1e16, 123456789.125, inf, -inf, nan, 2**64-ish, control chars, {}, [], {"k": []}])
    const std::string python =
        "[1e+20, 1e-05, -0.0, 0.1, 1e+16, 123456789.125, Infinity, -Infinity, NaN, 12345678901234567890, "
        "\"a\\tb\\n\\u0001\", {}, [], {\"k\": []}]";
    Json parsed = parse(python);
    REQUIRE(parsed.is_array());
    CHECK(parsed[0].is_number_float());
    CHECK(std::isinf(parsed[6].get<double>()));
    CHECK(parsed[6].get<double>() > 0);
    CHECK(parsed[7].get<double>() < 0);
    CHECK(std::isnan(parsed[8].get<double>()));
    CHECK(parsed[9].is_number_unsigned());
    CHECK(dump(parsed) == python);
}

TEST_CASE("pyjson keeps object key order", "[pyjson]") {
    Json o = Json::object();
    o["z"] = 1;
    o["a"] = Json::object({{"y", 2}, {"b", 3}});
    CHECK(dump(o) == "{\"z\": 1, \"a\": {\"y\": 2, \"b\": 3}}");
    CHECK(dump(parse("{\"z\": 1, \"a\": 2}")) == "{\"z\": 1, \"a\": 2}");
}

TEST_CASE("pyjson bare constants inside strings are left alone", "[pyjson]") {
    Json parsed = parse("[\"Infinity\", \"say NaN\", \"-Infinity\"]");
    CHECK(parsed[0] == "Infinity");
    CHECK(parsed[1] == "say NaN");
    CHECK(parsed[2] == "-Infinity");
    CHECK(dump(parsed) == "[\"Infinity\", \"say NaN\", \"-Infinity\"]");
}

TEST_CASE("pyjson writes non-finite doubles as bare tokens", "[pyjson]") {
    Json values = Json::array({std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()});
    CHECK(dump(values) == "[Infinity, -Infinity, NaN]");
}

TEST_CASE("pyjson dump escapes invalid UTF-8 as U+FFFD instead of failing", "[pyjson]") {
    CHECK(dump(Json(std::string("a\xC3(b"))) == "\"a\\ufffd(b\"");
}

TEST_CASE("pyjson rejects malformed and over-nested text", "[pyjson]") {
    CHECK_THROWS_AS(parse(""), ParseFailure);
    CHECK_THROWS_AS(parse("{"), ParseFailure);
    CHECK_THROWS_AS(parse("[1] x"), ParseFailure);
    CHECK_THROWS_AS(parse("{'a': 1}"), ParseFailure);
    const std::string deep = std::string(kMaxParseDepth + 1, '[') + std::string(kMaxParseDepth + 1, ']');
    CHECK_THROWS_AS(parse(deep), ParseFailure);
    const std::string fine = std::string(kMaxParseDepth, '[') + std::string(kMaxParseDepth, ']');
    CHECK_NOTHROW(parse(fine));
}

TEST_CASE("pyjson isValidUtf8 is strict", "[pyjson]") {
    CHECK(isValidUtf8(""));
    CHECK(isValidUtf8("h\xC3\xA9llo \xE4\xB8\x96 \xF0\x9F\x98\x80"));
    CHECK_FALSE(isValidUtf8("\xC3("));          // bad continuation
    CHECK_FALSE(isValidUtf8("\xC0\x80"));       // overlong NUL
    CHECK_FALSE(isValidUtf8("\xED\xA0\x80"));   // a UTF-16 surrogate
    CHECK_FALSE(isValidUtf8("\xE4\xB8"));       // truncated
    CHECK_FALSE(isValidUtf8("\xF4\x90\x80\x80"));  // above U+10FFFF
    CHECK_FALSE(isValidUtf8("\xFF"));
}
```

`cpp/tests/test_protocol.cpp`:

```cpp
// cpp/tests/test_protocol.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/errors.h"
#include "meradb/protocol.h"
#include "test_util.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace meradb;
using namespace meradb::protocol;

namespace {

// A connected pair of loopback sockets: `client` writes, `server` reads.
struct Pair {
    net::Socket listener = net::listenOn("127.0.0.1", 0);
    net::Socket client = net::connectTo("127.0.0.1", listener.localPort(), 2.0);
    net::Socket server = net::acceptWithTimeout(listener, 2.0);
};

}  // namespace

// ---- framing ----

TEST_CASE("protocol reader splits messages and keeps the remainder", "[protocol]") {
    Pair p;
    MessageReader reader(p.server);
    p.client.sendAll("{\"a\": 1}\n{\"b\"");
    auto first = reader.receive();
    REQUIRE(first.has_value());
    CHECK(pyjson::dump(*first) == "{\"a\": 1}");
    p.client.sendAll(": 2}\n{\"c\": 3}\n");
    CHECK(pyjson::dump(*reader.receive()) == "{\"b\": 2}");
    CHECK(pyjson::dump(*reader.receive()) == "{\"c\": 3}");
    p.client.close();
    CHECK_FALSE(reader.receive().has_value());  // clean end of stream
}

TEST_CASE("protocol send writes one line of Python-style JSON", "[protocol]") {
    Pair p;
    Json message = Json::object();
    message["type"] = "query";
    message["text"] = "DIKHAO * SE t";
    send(p.client, message);
    p.client.close();
    MessageReader reader(p.server);
    auto got = reader.receive();
    REQUIRE(got.has_value());
    CHECK(got->at("text") == "DIKHAO * SE t");
    char buffer[4];
    CHECK(p.server.recvSome(buffer, sizeof buffer) == 0);  // nothing but that one line was sent
}

TEST_CASE("protocol reader reports bad messages and carries on", "[protocol]") {
    Pair p;
    MessageReader reader(p.server);
    p.client.sendAll("not json\n[1, 2]\n\n{\"ok\": true}\n");
    try {
        reader.receive();
        FAIL("expected a ProtocolError");
    } catch (const ProtocolError& e) {
        CHECK(std::string(e.what()).rfind("Galat message: ", 0) == 0);
    }
    try {
        reader.receive();
        FAIL("expected a ProtocolError");
    } catch (const ProtocolError& e) {
        CHECK(std::string(e.what()) == "Message ek JSON object hona chahiye");
    }
    CHECK_THROWS_AS(reader.receive(), ProtocolError);  // the empty line
    auto good = reader.receive();
    REQUIRE(good.has_value());
    CHECK(good->at("ok") == true);
}

TEST_CASE("protocol reader rejects invalid UTF-8", "[protocol]") {
    Pair p;
    MessageReader reader(p.server);
    p.client.sendAll("{\"a\": \"\xC3(\"}\n{\"b\": 1}\n");
    CHECK_THROWS_AS(reader.receive(), ProtocolError);
    CHECK(reader.receive().has_value());
}

TEST_CASE("protocol reader accepts an unterminated last line like readline does", "[protocol]") {
    Pair p;
    MessageReader reader(p.server);
    p.client.sendAll("{\"last\": true}");
    p.client.close();
    auto got = reader.receive();
    REQUIRE(got.has_value());
    CHECK(got->at("last") == true);
    CHECK_FALSE(reader.receive().has_value());
}

TEST_CASE("protocol reader discards an oversize line and recovers", "[protocol]") {
    Pair p;
    MessageReader reader(p.server, 64);  // tiny cap for the test
    p.client.sendAll(std::string(200, 'x') + "\n{\"after\": 1}\n");
    try {
        reader.receive();
        FAIL("expected a ProtocolError");
    } catch (const ProtocolError& e) {
        CHECK(std::string(e.what()) == "Message bahut bada hai");
    }
    auto after = reader.receive();  // the rest of the long line is skipped, not read as a message
    REQUIRE(after.has_value());
    CHECK(after->at("after") == 1);
}

TEST_CASE("protocol reader size limit counts the newline like Python", "[protocol]") {
    Pair p;
    MessageReader reader(p.server, 16);
    // 15 bytes + "\n" = 16: allowed. 16 bytes + "\n" = 17: too big.
    p.client.sendAll("{\"k\": \"123456\"}\n");  // 15 bytes
    CHECK(reader.receive().has_value());
    p.client.sendAll("{\"k\": \"1234567\"}\n");  // 16 bytes
    CHECK_THROWS_AS(reader.receive(), ProtocolError);
}

TEST_CASE("protocol reader gives up when its stop flag is set", "[protocol]") {
    Pair p;
    std::atomic<bool> stop{false};
    MessageReader reader(p.server, kMaxMessageBytes, &stop);
    std::atomic<bool> gaveUp{false};
    std::thread t([&] {
        auto got = reader.receive();  // blocks: the peer says nothing
        gaveUp = !got.has_value();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK_FALSE(gaveUp.load());
    stop = true;
    t.join();
    CHECK(gaveUp.load());
}

// ---- cells and results ----

TEST_CASE("protocol results encode exactly like Python's Result.to_dict", "[protocol]") {
    Result r;
    r.columns = {"a", "d", "t", "b", "n"};
    r.rows.push_back({Value(1.0), Value(parseDate("2024-01-05")),
                      Value(std::string("h\xC3\xA9llo \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x98\x80 \\\" \x7f")), Value(true),
                      Value()});
    r.message = "1 row(s)";
    Json message = Json::object();
    message["ok"] = true;
    message["results"] = Json::array({resultToJson(r)});
    CHECK(pyjson::dump(message) ==
          "{\"ok\": true, \"results\": [{\"columns\": [\"a\", \"d\", \"t\", \"b\", \"n\"], \"rows\": "
          "[[1.0, {\"$date\": \"2024-01-05\"}, \"h\\u00e9llo \\u4e16\\u754c \\ud83d\\ude00 \\\\\\\" \\u007f\", true, null]], "
          "\"message\": \"1 row(s)\", \"error\": \"\"}]}");
}

TEST_CASE("protocol results survive a round trip and keep int/float apart", "[protocol]") {
    Result r;
    r.columns = {"i", "f", "s", "b", "n", "d"};
    r.rows.push_back({Value(int64_t{7}), Value(7.0), Value(std::string("x")), Value(false), Value(),
                      Value(parseDate("2005-06-15"))});
    r.rows.push_back({Value(int64_t{-9007199254740993}), Value(1e20), Value(std::string("")), Value(true), Value(),
                      Value()});
    Result back = resultFromJson(pyjson::parse(pyjson::dump(resultToJson(r))));
    REQUIRE(back.rows.size() == 2);
    CHECK(back.columns == r.columns);
    CHECK(std::holds_alternative<int64_t>(back.rows[0][0].data));
    CHECK(std::holds_alternative<double>(back.rows[0][1].data));  // 7.0 stays a float
    CHECK(std::get<int64_t>(back.rows[1][0].data) == -9007199254740993);
    CHECK(std::holds_alternative<Date>(back.rows[0][5].data));
    CHECK(std::get<Date>(back.rows[0][5].data) == parseDate("2005-06-15"));
    CHECK(back.rows[0][4].isNull());
}

TEST_CASE("protocol result decoding tolerates missing keys and rejects garbage", "[protocol]") {
    Result empty = resultFromJson(pyjson::parse("{}"));
    CHECK(empty.columns.empty());
    CHECK(empty.rows.empty());
    CHECK(empty.error.empty());
    CHECK_THROWS_AS(resultFromJson(pyjson::parse("[]")), ProtocolError);
    CHECK_THROWS_AS(resultFromJson(pyjson::parse("{\"rows\": [[[1]]]}")), ProtocolError);
    CHECK_THROWS_AS(resultFromJson(pyjson::parse("{\"rows\": [[{\"$date\": \"2024-13-45\"}]]}")), ProtocolError);
    CHECK_THROWS_AS(resultFromJson(pyjson::parse("{\"message\": 5}")), ProtocolError);
}

// ---- data folder and pid file ----

TEST_CASE("protocol defaultDataDirFrom follows Python's default_data_dir", "[protocol]") {
    namespace fs = std::filesystem;
    CHECK(defaultDataDirFrom("/custom/data", "C:\\Users\\x\\AppData\\Local", "/home/x", true) == "/custom/data");
    CHECK(defaultDataDirFrom(std::string(""), std::string("C:\\L"), "/home/x", true) ==
          (fs::path("C:\\L") / "MeraDB" / "data").string());  // an empty MERADB_DATA counts as unset
    CHECK(defaultDataDirFrom(std::nullopt, std::string("C:\\L"), "/home/x", false) ==
          (fs::path("/home/x") / ".local" / "share" / "MeraDB" / "data").string());  // LOCALAPPDATA is Windows-only
    CHECK(defaultDataDirFrom(std::nullopt, std::nullopt, "/home/x", true) ==
          (fs::path("/home/x") / ".local" / "share" / "MeraDB" / "data").string());
    CHECK_FALSE(defaultDataDir().empty());
}

TEST_CASE("protocol pid file has Python's json.dump(indent=2) shape", "[protocol]") {
    meradb_test::TempDir dir;
    Json info = Json::object();
    info["pid"] = 1234;
    info["host"] = "127.0.0.1";
    info["port"] = 6372;
    info["started"] = "2026-09-29T14:03:07";
    writePidFile(dir.str(), info);
    CHECK(meradb_test::readText(pidFilePath(dir.str())) ==
          "{\n  \"pid\": 1234,\n  \"host\": \"127.0.0.1\",\n  \"port\": 6372,\n  \"started\": \"2026-09-29T14:03:07\"\n}");
    auto back = readPidFile(dir.str());
    REQUIRE(back.has_value());
    CHECK(back->at("port") == 6372);
}

TEST_CASE("protocol removePidFile only removes the file of the given process", "[protocol]") {
    meradb_test::TempDir dir;
    Json info = Json::object();
    info["pid"] = 42;
    writePidFile(dir.str(), info);
    removePidFile(dir.str(), 41);
    CHECK(readPidFile(dir.str()).has_value());
    removePidFile(dir.str(), 42);
    CHECK_FALSE(readPidFile(dir.str()).has_value());
    removePidFile(dir.str(), 42);  // already gone: harmless
}

TEST_CASE("protocol readPidFile ignores junk", "[protocol]") {
    meradb_test::TempDir dir;
    CHECK_FALSE(readPidFile(dir.str()).has_value());  // no file
    std::ofstream(pidFilePath(dir.str())) << "not json";
    CHECK_FALSE(readPidFile(dir.str()).has_value());
    std::ofstream(pidFilePath(dir.str())) << "[1, 2]";
    CHECK_FALSE(readPidFile(dir.str()).has_value());
}

TEST_CASE("protocol runningServer needs a live listener behind the pid file", "[protocol]") {
    meradb_test::TempDir dir;
    CHECK_FALSE(runningServer(dir.str()).has_value());

    net::Socket listener = net::listenOn("127.0.0.1", 0);
    Json info = Json::object();
    info["pid"] = 1;
    info["host"] = "0.0.0.0";  // a server bound to every interface is reached via loopback
    info["port"] = listener.localPort();
    writePidFile(dir.str(), info);
    auto alive = runningServer(dir.str());
    REQUIRE(alive.has_value());
    CHECK(alive->at("pid") == 1);

    listener.close();
    CHECK_FALSE(runningServer(dir.str()).has_value());  // stale: nothing listens any more
}
```

- [ ] **Step 2: Add both tests to `cpp/tests/CMakeLists.txt`; verify failure**

```bash
cmake --build cpp/build
```
Expected: FAIL — `meradb/pyjson.h: No such file or directory`.

- [ ] **Step 3: `pyjson`**

`cpp/include/meradb/pyjson.h`:

```cpp
// cpp/include/meradb/pyjson.h
//
// JSON exactly the way Python's `json` module writes and reads it, so a
// Python peer and a C++ peer understand each other on the wire (nlohmann
// alone can do neither of the two things below):
//
//   dump   json.dumps(obj): separators ", " and ": ", ensure_ascii (every
//          non-ASCII character as a lowercase \uXXXX escape, astral ones as
//          surrogate pairs), floats spelled like float.__repr__, and the bare
//          tokens Infinity / -Infinity / NaN for non-finite floats.
//   parse  json.loads(text): additionally accepts those bare tokens.
#pragma once
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace meradb::pyjson {

using Json = nlohmann::ordered_json;

// Thrown by parse(); what() is a short reason (the exact wording differs from
// Python's JSONDecodeError -- a documented, accepted difference).
class ParseFailure : public std::runtime_error {
public:
    explicit ParseFailure(const std::string& message) : std::runtime_error(message) {}
};

// More nested arrays/objects than this are refused (Python fails around 1000
// with a RecursionError, which the server would not survive gracefully).
constexpr int kMaxParseDepth = 512;

std::string dump(const Json& value);
Json parse(const std::string& text);

// Strict UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF).
bool isValidUtf8(const std::string& text);

}  // namespace meradb::pyjson
```

`cpp/src/pyjson.cpp`:

```cpp
// cpp/src/pyjson.cpp
#include "meradb/pyjson.h"
#include "meradb/datatypes.h"
#include <cmath>
#include <cstdint>
#include <limits>

namespace meradb::pyjson {

namespace {

// Decodes one UTF-8 sequence starting at s[i]. Returns its length, or 0 when
// the bytes are not strictly valid UTF-8.
std::size_t decodeUtf8(const std::string& s, std::size_t i, std::uint32_t& cp) {
    const auto byteAt = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    unsigned char b0 = byteAt(i);
    std::size_t len;
    std::uint32_t minimum;
    if (b0 < 0x80) {
        cp = b0;
        return 1;
    } else if ((b0 & 0xE0) == 0xC0) {
        len = 2;
        cp = b0 & 0x1F;
        minimum = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        len = 3;
        cp = b0 & 0x0F;
        minimum = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        len = 4;
        cp = b0 & 0x07;
        minimum = 0x10000;
    } else {
        return 0;
    }
    if (i + len > s.size()) return 0;
    for (std::size_t k = 1; k < len; ++k) {
        unsigned char b = byteAt(i + k);
        if ((b & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return len;
}

void appendEscape(std::string& out, std::uint32_t unit) {
    static const char* hex = "0123456789abcdef";
    out += "\\u";
    out += hex[(unit >> 12) & 0xF];
    out += hex[(unit >> 8) & 0xF];
    out += hex[(unit >> 4) & 0xF];
    out += hex[unit & 0xF];
}

void writeString(std::string& out, const std::string& s) {
    out += '"';
    for (std::size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (c < 0x20 || c == 0x7f) appendEscape(out, c);  // Python escapes DEL too
                    else out += static_cast<char>(c);
            }
            ++i;
            continue;
        }
        std::uint32_t cp = 0;
        std::size_t len = decodeUtf8(s, i, cp);
        if (len == 0) {  // not valid UTF-8 (cannot come from a Python str): U+FFFD
            cp = 0xFFFD;
            len = 1;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            appendEscape(out, 0xD800 + (cp >> 10));
            appendEscape(out, 0xDC00 + (cp & 0x3FF));
        } else {
            appendEscape(out, cp);
        }
        i += len;
    }
    out += '"';
}

void writeValue(std::string& out, const Json& j) {
    if (j.is_null()) {
        out += "null";
    } else if (j.is_boolean()) {
        out += j.get<bool>() ? "true" : "false";
    } else if (j.is_number_unsigned()) {
        out += std::to_string(j.get<std::uint64_t>());
    } else if (j.is_number_integer()) {
        out += std::to_string(j.get<std::int64_t>());
    } else if (j.is_number_float()) {
        double d = j.get<double>();
        if (std::isnan(d)) out += "NaN";
        else if (std::isinf(d)) out += d > 0 ? "Infinity" : "-Infinity";
        else out += pyReprFloat(d);
    } else if (j.is_string()) {
        writeString(out, j.get_ref<const std::string&>());
    } else if (j.is_array()) {
        out += '[';
        bool first = true;
        for (const auto& item : j) {
            if (!first) out += ", ";
            first = false;
            writeValue(out, item);
        }
        out += ']';
    } else if (j.is_object()) {
        out += '{';
        bool first = true;
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!first) out += ", ";
            first = false;
            writeString(out, it.key());
            out += ": ";
            writeValue(out, it.value());
        }
        out += '}';
    } else {
        out += "null";  // binary blobs never occur in MeraDB messages
    }
}

// Python writes non-finite floats as bare words nlohmann cannot read. Outside
// strings they are swapped for {"$float": "..."} markers (undone after
// parsing); the same pass counts nesting so a hostile message cannot be deep.
std::string markBareConstants(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool inString = false;
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (inString) {
            out += c;
            if (c == '\\' && i + 1 < text.size()) out += text[++i];
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '[' || c == '{') {
            if (++depth > kMaxParseDepth) throw ParseFailure("nesting bahut gehri hai");
        } else if (c == ']' || c == '}') {
            if (depth > 0) --depth;
        } else if (c == 'I' && text.compare(i, 8, "Infinity") == 0) {
            out += "{\"$float\": \"Infinity\"}";
            i += 7;
            continue;
        } else if (c == '-' && text.compare(i + 1, 8, "Infinity") == 0) {
            out += "{\"$float\": \"-Infinity\"}";
            i += 8;
            continue;
        } else if (c == 'N' && text.compare(i, 3, "NaN") == 0) {
            out += "{\"$float\": \"NaN\"}";
            i += 2;
            continue;
        }
        out += c;
    }
    return out;
}

void restoreConstants(Json& j) {
    if (j.is_object()) {
        if (j.size() == 1 && j.begin().key() == "$float" && j.begin().value().is_string()) {
            const std::string word = j.begin().value().get<std::string>();
            if (word == "Infinity") { j = std::numeric_limits<double>::infinity(); return; }
            if (word == "-Infinity") { j = -std::numeric_limits<double>::infinity(); return; }
            if (word == "NaN") { j = std::numeric_limits<double>::quiet_NaN(); return; }
        }
        for (auto it = j.begin(); it != j.end(); ++it) restoreConstants(it.value());
    } else if (j.is_array()) {
        for (auto& item : j) restoreConstants(item);
    }
}

}  // namespace

std::string dump(const Json& value) {
    std::string out;
    writeValue(out, value);
    return out;
}

Json parse(const std::string& text) {
    std::string marked = markBareConstants(text);
    Json result;
    try {
        result = Json::parse(marked);
    } catch (const nlohmann::json::exception& e) {
        throw ParseFailure(e.what());
    }
    restoreConstants(result);
    return result;
}

bool isValidUtf8(const std::string& text) {
    for (std::size_t i = 0; i < text.size();) {
        std::uint32_t cp = 0;
        std::size_t len = decodeUtf8(text, i, cp);
        if (len == 0) return false;
        i += len;
    }
    return true;
}

}  // namespace meradb::pyjson
```

- [ ] **Step 4: `protocol`**

`cpp/include/meradb/protocol.h`:

```cpp
// cpp/include/meradb/protocol.h
//
// The MeraDB WIRE PROTOCOL: newline-delimited JSON over TCP, one object per
// line. Mirrors meradb/protocol.py (framing, pid file, default data folder)
// plus the Result <-> JSON codec from meradb/engine.py (to_dict/from_dict).
// See docs/SERVER.md for the message catalogue.
#pragma once
#include "meradb/engine.h"
#include "meradb/net_compat.h"
#include "meradb/pyjson.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace meradb::protocol {

using pyjson::Json;

constexpr const char* kDefaultHost = "127.0.0.1";
constexpr int kDefaultPort = 6372;  // M-E-R-A on a phone keypad
constexpr int kVersion = 1;
constexpr std::size_t kMaxMessageBytes = 64u * 1024u * 1024u;  // refuse absurdly large lines
constexpr const char* kPidFile = "meradb.pid";
constexpr const char* kServerName = "MeraDB 1.0.0";  // "MeraDB " + Python's __version__

// The bare message is what goes on the wire ("[Protocol Galti] " is added by
// the server when it reports one).
class ProtocolError : public std::runtime_error {
public:
    explicit ProtocolError(const std::string& message) : std::runtime_error(message) {}
};

// Reads newline-delimited JSON objects from a socket, keeping the bytes that
// follow a message for the next call.
class MessageReader {
public:
    // With `stop`, the reader polls the flag every 100 ms while waiting for
    // data and gives up (returns nullopt, as if the peer had closed) once it is
    // set -- how server threads are stopped without relying on socket tricks.
    explicit MessageReader(net::Socket& socket, std::size_t maxBytes = kMaxMessageBytes,
                           const std::atomic<bool>* stop = nullptr)
        : socket_(socket), maxBytes_(maxBytes), stop_(stop) {}

    // nullopt = the peer closed the connection (or `stop` was set).
    // Throws ProtocolError for a bad message -- the reader stays usable, the next
    // call continues with the next line. net::NetError passes through.
    std::optional<Json> receive();

private:
    enum class Fill { Data, Eof, Stopped };
    Fill fill();
    Json parseLine(const std::string& line) const;

    net::Socket& socket_;
    std::size_t maxBytes_;
    const std::atomic<bool>* stop_;
    std::string buffer_;
    bool discarding_ = false;  // skipping the rest of an oversize line
};

// One JSON object plus "\n". Throws net::NetError.
void send(net::Socket& socket, const Json& message);

// ---- cells and results (engine.py: Result.to_dict / from_dict) ----
// null / bool / int / float / string as themselves; a DATE as {"$date": "YYYY-MM-DD"}.
Json valueToWire(const Value& value);
Value valueFromWire(const Json& json);  // throws ProtocolError
Json resultToJson(const Result& result);
Result resultFromJson(const Json& json);  // throws ProtocolError

// ---- data folder and pid file ----
// Where databases live unless -D / --data says otherwise: MERADB_DATA, else
// %LOCALAPPDATA%\MeraDB\data (Windows) or ~/.local/share/MeraDB/data.
std::string defaultDataDir();
std::string defaultDataDirFrom(const std::optional<std::string>& meradbData,
                               const std::optional<std::string>& localAppData, const std::string& home,
                               bool windows);

std::string pidFilePath(const std::string& dataDir);
std::optional<Json> readPidFile(const std::string& dataDir);  // nullopt: missing or unreadable
void writePidFile(const std::string& dataDir, const Json& info);  // json.dump(indent=2)
// Removes the file, but only if it still belongs to process `pid`.
void removePidFile(const std::string& dataDir, std::int64_t pid);
// The pid-file info if a server really answers on that port, else nullopt
// (a stale file left by a crashed server).
std::optional<Json> runningServer(const std::string& dataDir);

}  // namespace meradb::protocol
```

`cpp/src/protocol.cpp`:

```cpp
// cpp/src/protocol.cpp
#include "meradb/protocol.h"
#include "meradb/errors.h"
#include "meradb/sys_compat.h"
#include <filesystem>
#include <fstream>
#include <sstream>

namespace meradb::protocol {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// framing
// ---------------------------------------------------------------------------

MessageReader::Fill MessageReader::fill() {
    if (stop_ != nullptr) {
        while (!socket_.waitReadable(0.1))
            if (stop_->load()) return Fill::Stopped;
    }
    char chunk[65536];
    std::size_t n = socket_.recvSome(chunk, sizeof chunk);
    if (n == 0) return Fill::Eof;
    buffer_.append(chunk, n);
    return Fill::Data;
}

Json MessageReader::parseLine(const std::string& line) const {
    if (!pyjson::isValidUtf8(line)) throw ProtocolError("Galat message: invalid UTF-8");
    Json message;
    try {
        message = pyjson::parse(line);
    } catch (const pyjson::ParseFailure& e) {
        throw ProtocolError(std::string("Galat message: ") + e.what());
    }
    if (!message.is_object()) throw ProtocolError("Message ek JSON object hona chahiye");
    return message;
}

std::optional<Json> MessageReader::receive() {
    for (;;) {
        if (discarding_) {  // the rest of an oversize line: drop it up to its newline
            auto newline = buffer_.find('\n');
            if (newline != std::string::npos) {
                buffer_.erase(0, newline + 1);
                discarding_ = false;
            } else {
                buffer_.clear();
                if (fill() != Fill::Data) return std::nullopt;
                continue;
            }
        }
        auto newline = buffer_.find('\n');
        if (newline != std::string::npos) {
            std::string line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            if (line.size() + 1 > maxBytes_) throw ProtocolError("Message bahut bada hai");  // Python counts the "\n"
            return parseLine(line);
        }
        if (buffer_.size() >= maxBytes_) {
            buffer_.clear();
            discarding_ = true;
            throw ProtocolError("Message bahut bada hai");
        }
        Fill state = fill();
        if (state == Fill::Stopped) return std::nullopt;
        if (state == Fill::Eof) {
            if (buffer_.empty()) return std::nullopt;
            std::string tail = std::move(buffer_);  // Python's readline() hands back an unterminated tail too
            buffer_.clear();
            return parseLine(tail);
        }
    }
}

void send(net::Socket& socket, const Json& message) { socket.sendAll(pyjson::dump(message) + "\n"); }

// ---------------------------------------------------------------------------
// cells and results
// ---------------------------------------------------------------------------

Json valueToWire(const Value& value) {
    const auto& d = value.data;
    if (std::holds_alternative<std::monostate>(d)) return nullptr;
    if (std::holds_alternative<bool>(d)) return std::get<bool>(d);
    if (std::holds_alternative<int64_t>(d)) return static_cast<std::int64_t>(std::get<int64_t>(d));
    if (std::holds_alternative<double>(d)) return std::get<double>(d);
    if (std::holds_alternative<std::string>(d)) return std::get<std::string>(d);
    Json date = Json::object();
    date["$date"] = std::get<Date>(d).isoFormat();
    return date;
}

Value valueFromWire(const Json& json) {
    if (json.is_null()) return Value();
    if (json.is_boolean()) return Value(json.get<bool>());
    if (json.is_number_unsigned()) {
        std::uint64_t u = json.get<std::uint64_t>();
        if (u <= static_cast<std::uint64_t>(INT64_MAX)) return Value(static_cast<int64_t>(u));
        return Value(static_cast<double>(u));  // beyond 64 bits: the closest a C++ INT can get
    }
    if (json.is_number_integer()) return Value(static_cast<int64_t>(json.get<std::int64_t>()));
    if (json.is_number_float()) return Value(json.get<double>());
    if (json.is_string()) return Value(json.get<std::string>());
    if (json.is_object() && json.contains("$date") && json.at("$date").is_string()) {
        try {
            return Value(parseDate(json.at("$date").get<std::string>()));
        } catch (const MeraDBError&) {
            throw ProtocolError("Galat message: $date ki value date nahi hai");
        }
    }
    throw ProtocolError("Galat message: cell ki value samajh nahi aayi");
}

Json resultToJson(const Result& result) {
    Json columns = Json::array();
    for (const auto& name : result.columns) columns.push_back(name);
    Json rows = Json::array();
    for (const auto& row : result.rows) {
        Json cells = Json::array();
        for (const auto& value : row) cells.push_back(valueToWire(value));
        rows.push_back(std::move(cells));
    }
    Json out = Json::object();
    out["columns"] = std::move(columns);
    out["rows"] = std::move(rows);
    out["message"] = result.message;
    out["error"] = result.error;
    return out;
}

Result resultFromJson(const Json& json) {
    Result result;
    try {
        if (!json.is_object()) throw ProtocolError("Galat message: result ek object hona chahiye");
        if (json.contains("columns"))
            for (const auto& name : json.at("columns")) result.columns.push_back(name.get<std::string>());
        if (json.contains("rows")) {
            for (const auto& row : json.at("rows")) {
                std::vector<Value> cells;
                for (const auto& cell : row) cells.push_back(valueFromWire(cell));
                result.rows.push_back(std::move(cells));
            }
        }
        if (json.contains("message")) result.message = json.at("message").get<std::string>();
        if (json.contains("error")) result.error = json.at("error").get<std::string>();
    } catch (const nlohmann::json::exception& e) {
        throw ProtocolError(std::string("Galat message: ") + e.what());
    }
    return result;
}

// ---------------------------------------------------------------------------
// data folder and pid file
// ---------------------------------------------------------------------------

std::string defaultDataDirFrom(const std::optional<std::string>& meradbData,
                               const std::optional<std::string>& localAppData, const std::string& home,
                               bool windows) {
    if (meradbData && !meradbData->empty()) return *meradbData;
    std::string base;
    if (windows && localAppData && !localAppData->empty()) base = *localAppData;
    if (base.empty()) base = (fs::path(home) / ".local" / "share").string();
    return (fs::path(base) / "MeraDB" / "data").string();
}

std::string defaultDataDir() {
#ifdef _WIN32
    const bool windows = true;
#else
    const bool windows = false;
#endif
    return defaultDataDirFrom(sys::getEnv("MERADB_DATA"), sys::getEnv("LOCALAPPDATA"), sys::homeDir(), windows);
}

std::string pidFilePath(const std::string& dataDir) { return (fs::path(dataDir) / kPidFile).string(); }

std::optional<Json> readPidFile(const std::string& dataDir) {
    std::ifstream file(pidFilePath(dataDir), std::ios::binary);
    if (!file) return std::nullopt;
    std::ostringstream text;
    text << file.rdbuf();
    try {
        Json info = pyjson::parse(text.str());
        if (!info.is_object()) return std::nullopt;
        return info;
    } catch (const pyjson::ParseFailure&) {
        return std::nullopt;
    }
}

void writePidFile(const std::string& dataDir, const Json& info) {
    std::ofstream file(pidFilePath(dataDir), std::ios::binary | std::ios::trunc);
    if (!file) throw StorageError("meradb.pid likh nahi paaye: " + pidFilePath(dataDir));
    file << info.dump(2, ' ', true);  // json.dump(info, f, indent=2): no trailing newline
    if (!file) throw StorageError("meradb.pid likh nahi paaye: " + pidFilePath(dataDir));
}

void removePidFile(const std::string& dataDir, std::int64_t pid) {
    auto info = readPidFile(dataDir);
    if (info && info->contains("pid") && info->at("pid").is_number_integer() &&
        info->at("pid").get<std::int64_t>() == pid) {
        std::error_code ec;
        fs::remove(pidFilePath(dataDir), ec);
    }
}

std::optional<Json> runningServer(const std::string& dataDir) {
    auto info = readPidFile(dataDir);
    if (!info) return std::nullopt;
    std::string host = kDefaultHost;
    if (info->contains("host") && info->at("host").is_string()) host = info->at("host").get<std::string>();
    if (host == "0.0.0.0" || host.empty() || host == "::") host = kDefaultHost;
    int port = kDefaultPort;
    if (info->contains("port") && info->at("port").is_number_integer()) port = info->at("port").get<int>();
    if (net::portOpen(host, port)) return info;
    return std::nullopt;  // stale pid file left behind by a crashed server
}

}  // namespace meradb::protocol
```

- [ ] **Step 5: CMake** — add `src/pyjson.cpp` and `src/protocol.cpp` to `meradb_core`.

- [ ] **Step 6: Build and run**

```bash
cmake -S cpp -B cpp/build -G "MinGW Makefiles"
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "pyjson|protocol"
```
Expected: 8 `pyjson` + 16 `protocol` test cases pass. If a wire-byte test differs from
Python, run the same value through `python -c "import json; print(json.dumps(...))"` —
the Python bytes win.

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/pyjson.h cpp/src/pyjson.cpp cpp/include/meradb/protocol.h cpp/src/protocol.cpp cpp/tests/test_pyjson.cpp cpp/tests/test_protocol.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add the Python-compatible JSON writer and the wire protocol codec"
```

**Completion checklist:**
- [ ] the byte-for-byte Python vectors in `test_pyjson.cpp` pass (unicode, astral character, DEL, `1e+20`, `-0.0`, `Infinity`, big ints)
- [ ] `MessageReader` has tests for: split reads, two messages in one read, unterminated last line, oversize (newline included), stop flag
- [ ] the pid file round-trips and `removePidFile` refuses a pid that is not its own
- [ ] no `std::getenv` (uses `sys::getEnv`)
- [ ] full suite green, zero warnings

---

### Task 15: Engine — lock helper, `schemaTree`, served-folder protection

**Files:**
- Modify: `cpp/include/meradb/engine.h`, `cpp/src/engine.cpp`
- Test: `cpp/tests/test_engine_server_support.cpp` (create)
- Modify: `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `protocol::runningServer` (Task 14), `Column::toJson` (Phase 1).
- Produces:
  - `Instance(std::string dataDir, bool served = false)`: when `served` is false and a
    server really answers for that folder, throws
    `MeraDBError("Is data folder par MeraDB server chal raha hai (port N). Seedha files mat kholo -- `meradb shell` se server se connect karo.")`
    (Python's `Instance(served=False)` behaviour). The server itself passes `true`.
  - `Engine::acquireLock(double timeoutSeconds)`: the "wait for the instance lock or throw
    `Database busy hai ...`" block of `executeStatement`, extracted so
    `schemaTree()` can use a 2 s timeout.
  - `Engine::schemaTree()`: `[{"name", "current", "tables": [{"name", "columns": [Column::toJson...]}]}]`,
    databases in listing order, tables sorted by name.
  - `Instance::databases()` made safe against a concurrent `DROP DATABASE` (error-code
    directory iteration).

- [ ] **Step 1: Write the failing test** — `cpp/tests/test_engine_server_support.cpp`

```cpp
// cpp/tests/test_engine_server_support.cpp
#include <catch2/catch_test_macros.hpp>
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "meradb/protocol.h"
#include "test_util.h"
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

using namespace meradb;

namespace {

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace

TEST_CASE("engine_server schemaTree matches Python's schema_tree JSON", "[engine][server]") {
    meradb_test::TempDir dir;
    Engine e(dir.str());
    e.execute("BANAO TABLE zeta (id INT MUKHYA KUNJI, naam TEXT ZAROORI); "
              "BANAO TABLE alpha (a FLOAT WARNA 1.5, d DATE); BANAO DATABASE other");
    // json.dumps(Engine.schema_tree()) from the Python engine for the same statements.
    CHECK(pyjson::dump(e.schemaTree()) ==
          "[{\"name\": \"main\", \"current\": true, \"tables\": [{\"name\": \"alpha\", \"columns\": ["
          "{\"name\": \"a\", \"type_name\": \"FLOAT\", \"primary_key\": false, \"not_null\": false, \"unique\": false, "
          "\"default\": 1.5, \"max_length\": null, \"ref_table\": null, \"ref_column\": null, \"check\": null}, "
          "{\"name\": \"d\", \"type_name\": \"DATE\", \"primary_key\": false, \"not_null\": false, \"unique\": false, "
          "\"default\": null, \"max_length\": null, \"ref_table\": null, \"ref_column\": null, \"check\": null}]}, "
          "{\"name\": \"zeta\", \"columns\": ["
          "{\"name\": \"id\", \"type_name\": \"INT\", \"primary_key\": true, \"not_null\": false, \"unique\": false, "
          "\"default\": null, \"max_length\": null, \"ref_table\": null, \"ref_column\": null, \"check\": null}, "
          "{\"name\": \"naam\", \"type_name\": \"TEXT\", \"primary_key\": false, \"not_null\": true, \"unique\": false, "
          "\"default\": null, \"max_length\": null, \"ref_table\": null, \"ref_column\": null, \"check\": null}]}]}, "
          "{\"name\": \"other\", \"current\": false, \"tables\": []}]");
}

TEST_CASE("engine_server schemaTree marks the session's current database", "[engine][server]") {
    meradb_test::TempDir dir;
    Engine e(dir.str());
    e.execute("BANAO DATABASE college; ISTEMAL college");
    auto tree = e.schemaTree();
    REQUIRE(tree.size() == 2);
    CHECK(tree[0]["name"] == "college");
    CHECK(tree[0]["current"] == true);
    CHECK(tree[1]["name"] == "main");
    CHECK(tree[1]["current"] == false);
}

TEST_CASE("engine_server a statement waits for another session's transaction then reports busy", "[engine][server]") {
    meradb_test::TempDir dir;
    auto instance = std::make_shared<Instance>(dir.str());
    Engine main(instance);

    std::atomic<bool> holding{false};
    std::atomic<bool> release{false};
    std::thread holder([&] {  // SHURU and WAPAS must run on the SAME thread
        Engine other(instance);
        other.execute("SHURU");
        holding = true;
        while (!release) sleepMs(5);
        other.execute("WAPAS");
    });
    while (!holding) sleepMs(5);

    instance->lockTimeoutSeconds = 0.2;
    auto results = main.runScript("DIKHAO TABLES");
    REQUIRE(results.size() == 1);
    CHECK(results[0].error.find("Database busy hai") != std::string::npos);

    auto started = std::chrono::steady_clock::now();
    try {
        main.schemaTree();  // waits 2 s, not lockTimeoutSeconds
        FAIL("expected the busy error");
    } catch (const MeraDBError& e) {
        CHECK(e.message().find("Database busy hai") == 0);
    }
    auto waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(waited >= 1.5);

    release = true;
    holder.join();
    CHECK(main.runScript("DIKHAO TABLES")[0].error.empty());  // the lock is free again
    CHECK_NOTHROW(main.schemaTree());
}

TEST_CASE("engine_server an embedded engine refuses a folder a server is serving", "[engine][server]") {
    meradb_test::TempDir dir;
    net::Socket listener = net::listenOn("127.0.0.1", 0);
    pyjson::Json info = pyjson::Json::object();
    info["pid"] = 1;
    info["host"] = "127.0.0.1";
    info["port"] = listener.localPort();
    protocol::writePidFile(dir.str(), info);

    try {
        Engine e(dir.str());
        FAIL("expected the refusal");
    } catch (const MeraDBError& e) {
        CHECK(e.message() == "Is data folder par MeraDB server chal raha hai (port " + std::to_string(listener.localPort()) +
                                 "). Seedha files mat kholo -- `meradb shell` se server se connect karo.");
    }
    CHECK_NOTHROW(Instance(dir.str(), /*served=*/true));  // the server process itself opens it

    listener.close();
    CHECK_NOTHROW(Engine(dir.str()));  // stale pid file: nothing listens any more
}

TEST_CASE("engine_server databases() lists directories only", "[engine][server]") {
    meradb_test::TempDir dir;
    Instance instance(dir.str());
    std::filesystem::create_directories(dir.path() / "zoo");
    std::filesystem::create_directories(dir.path() / ".hidden");
    std::ofstream(dir.file("users.json")) << "{}";
    CHECK(instance.databases() == std::vector<std::string>{"main", "zoo"});
}
```

- [ ] **Step 2: Add to `cpp/tests/CMakeLists.txt`; verify it fails**

```bash
cmake --build cpp/build
```
Expected: FAIL (`schemaTree` is not a member of `Engine`, `Instance` has no 2-argument constructor).

- [ ] **Step 3: Edit `cpp/include/meradb/engine.h`**

3a. `class Instance`: replace `explicit Instance(std::string dataDir);` with

```cpp
    // `served` is true only inside the server process. Otherwise the constructor refuses a
    // folder that a running server is serving (two processes must never write the same files).
    explicit Instance(std::string dataDir, bool served = false);
```

3b. `class Engine`, public, directly after `void close();` add

```cpp
    // Every database -> table -> column as plain JSON (what the server sends for the
    // workbench sidebar). Waits at most 2 s for the lock, like Python's schema_tree().
    nlohmann::ordered_json schemaTree();
```

3c. `class Engine`, private, directly before `Result guarded(const ast::Statement& stmt);` add

```cpp
    // Waits up to `timeoutSeconds` for the instance lock; throws the "Database busy hai" error.
    std::unique_lock<std::recursive_timed_mutex> acquireLock(double timeoutSeconds);
```

- [ ] **Step 4: Edit `cpp/src/engine.cpp`** — five edits, each a find/replace of the exact old text.

4a. Add `#include "meradb/protocol.h"` directly after `#include "meradb/engine.h"`.

4b. Instance constructor. Replace
```cpp
Instance::Instance(std::string dataDir) : dataDir_(fs::absolute(fs::path(dataDir)).string()), users_(dataDir_) {
    fs::create_directories(dataDir_);
```
with
```cpp
Instance::Instance(std::string dataDir, bool served)
    : dataDir_(fs::absolute(fs::path(dataDir)).string()), users_(dataDir_) {
    fs::create_directories(dataDir_);
    if (!served) {
        // Two processes must never write the same files: refuse a folder a server is serving.
        if (auto info = protocol::runningServer(dataDir_)) {
            std::string port = info->contains("port") ? info->at("port").dump() : "None";
            throw MeraDBError("Is data folder par MeraDB server chal raha hai (port " + port +
                              "). Seedha files mat kholo -- `meradb shell` se server se connect karo.");
        }
    }
```
(the rest of the constructor body stays).

4c. `Instance::databases()`: replace the `for (const auto& entry : fs::directory_iterator(dataDir_)) { ... }` loop with

```cpp
    // Error-code overloads: a concurrent DROP DATABASE may remove an entry mid-listing.
    std::vector<std::string> names;
    std::error_code ec;
    for (fs::directory_iterator it(dataDir_, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entryEc;
        std::string name = it->path().filename().string();
        if (!name.empty() && name[0] != '.' && it->is_directory(entryEc) && !entryEc) names.push_back(name);
    }
```
(keep the `std::vector<std::string> names;` declaration only once, and the sort / return that follow).

4d. `Engine::executeStatement`: replace the block
```cpp
    std::unique_lock<std::recursive_timed_mutex> guard(instance_->lock, std::defer_lock);
    auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::duration<double>(instance_->lockTimeoutSeconds));
    if (!guard.try_lock_for(wait))
        throw ExecutionError("Database busy hai -- kisi aur session ka transaction chal raha hai. Thodi der baad try karo.");
```
with `auto guard = acquireLock(instance_->lockTimeoutSeconds);`

4e. Directly before `void Engine::close() {` add:

```cpp
std::unique_lock<std::recursive_timed_mutex> Engine::acquireLock(double timeoutSeconds) {
    std::unique_lock<std::recursive_timed_mutex> guard(instance_->lock, std::defer_lock);
    auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(timeoutSeconds));
    if (!guard.try_lock_for(wait))
        throw ExecutionError("Database busy hai -- kisi aur session ka transaction chal raha hai. Thodi der baad try karo.");
    return guard;
}

nlohmann::ordered_json Engine::schemaTree() {
    using nlohmann::ordered_json;
    auto guard = acquireLock(2.0);
    ordered_json tree = ordered_json::array();
    for (const auto& db : instance_->databases()) {
        Catalog& cat = instance_->catalog(db);
        std::vector<std::string> names;
        for (const auto& entry : cat.tables) names.push_back(entry.first);
        std::sort(names.begin(), names.end());  // Python: sorted(catalog.tables)
        ordered_json tables = ordered_json::array();
        for (const auto& name : names) {
            ordered_json columns = ordered_json::array();
            for (const auto& col : cat.tables.at(name).columns) columns.push_back(col.toJson());
            ordered_json table = ordered_json::object();
            table["name"] = name;
            table["columns"] = std::move(columns);
            tables.push_back(std::move(table));
        }
        ordered_json entry = ordered_json::object();
        entry["name"] = db;
        entry["current"] = (db == currentDb);
        entry["tables"] = std::move(tables);
        tree.push_back(std::move(entry));
    }
    return tree;
}

```

- [ ] **Step 5: Build and run**

```bash
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "engine_server"
ctest --test-dir cpp/build --output-on-failure
```
Expected: 5 `engine_server` test cases pass; the whole suite still green (Phase 1 tests
construct `Instance` with one argument, which still compiles).

- [ ] **Step 6: Commit**

```bash
git add cpp/include/meradb/engine.h cpp/src/engine.cpp cpp/tests/test_engine_server_support.cpp cpp/tests/CMakeLists.txt
git commit -m "Add Engine::schemaTree, the lock helper and served-folder protection"
```

**Completion checklist:**
- [ ] the schema-tree test compares against the exact JSON Python produced (key order included)
- [ ] a non-served `Instance` on a folder with a live server's pid file throws the exact message
- [ ] `Engine` constructed from an existing `shared_ptr<Instance>` (the server's way) does not re-check the folder
- [ ] full suite green, zero warnings

---

### Task 16: Server

**Files:**
- Create: `cpp/include/meradb/server.h`, `cpp/src/server.cpp`, `cpp/tests/server_fixture.h`
- Test: `cpp/tests/test_server.cpp`
- Modify: `cpp/CMakeLists.txt` (add `src/server.cpp`), `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `net_compat`, `protocol`, `Instance(dir, true)`, `Engine(std::shared_ptr<Instance>)`.
- Produces: `Server` (`port()`, `sessions()`, `instance()`, `started()`, `serveForever()`,
  `requestStop()`), `ServerOptions`, `int serve(ServerOptions)` (the `meradb server` body:
  pid file, log lines like Python's `serve()`, SIGINT/SIGTERM set an atomic flag).
- Protocol facts (from `meradb/server.py`, verified by probing a live Python server):
  `hello` must be the first message and carries `password` / `database` / `user`; a wrong
  shared password is `{"ok": false, "error": "Galat password"}` and the connection is closed;
  a user login supersedes the shared password; a `hello` with a database goes through
  `UseDatabase`, so a restricted user is refused with the "superuser nahi hai" text;
  request types are `query`, `schema`, `status`, `ping`, `shutdown`; `shutdown` is honoured
  from loopback peers only; a bad message is `{"ok": false, "error": "[Protocol Galti] ..."}`
  and the session goes on; unknown type is `Unknown message type: 'x'`; the `status` reply
  key order is `ok, server, pid, data_dir, started, sessions, databases`.
- Threading rules this task must follow (D2): one thread per connection; the `Engine` is
  created, used and destroyed on that thread; the accept loop and all connection threads
  poll the stop flag every 100 ms (never rely on `shutdown()` to wake `recv`); a `Cleanup`
  object in the thread rolls back an open transaction (`session.close()`) and decrements
  the session count whatever way the thread ends.

- [ ] **Step 1: Write the fixture and the failing tests**

`cpp/tests/server_fixture.h` (a real server on a free port with a captured log, plus a
bare-bones protocol client — reused by Tasks 17-19 and 21):

```cpp
// cpp/tests/server_fixture.h -- a real server on a free port, plus a bare-bones protocol client.
#pragma once
#include "meradb/protocol.h"
#include "meradb/server.h"
#include "test_util.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace meradb_test {

inline void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Polls `condition` for up to `seconds`; true if it became true.
template <typename F>
bool waitFor(F condition, double seconds = 3.0) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        sleepMs(10);
    }
    return condition();
}

// A server bound to 127.0.0.1 on an OS-chosen port, serving a temp data folder.
// Statements wait at most 0.5 s for another session's transaction (the Python
// tests do the same). The destructor stops it and joins everything.
class RunningServer {
public:
    explicit RunningServer(const std::string& password = "", bool verbose = false,
                           std::size_t maxMessageBytes = meradb::protocol::kMaxMessageBytes) {
        meradb::ServerOptions options;
        options.dataDir = dir_.str();
        options.port = 0;
        options.password = password;
        options.verbose = verbose;
        options.maxMessageBytes = maxMessageBytes;
        options.log = [this](const std::string& line) {
            std::lock_guard<std::mutex> guard(logMutex_);
            lines_.push_back(line);
        };
        server_ = std::make_unique<meradb::Server>(options);
        server_->instance().lockTimeoutSeconds = 0.5;
        thread_ = std::thread([this] {
            server_->serveForever();
            stopped_ = true;
        });
    }
    ~RunningServer() { stop(); }
    RunningServer(const RunningServer&) = delete;
    RunningServer& operator=(const RunningServer&) = delete;

    void stop() {
        server_->requestStop();
        if (thread_.joinable()) thread_.join();
    }

    int port() const { return server_->port(); }
    meradb::Server& server() { return *server_; }
    meradb::Instance& instance() { return server_->instance(); }
    const std::string& dataDir() const { return server_->instance().dataDir(); }
    bool stopped() const { return stopped_.load(); }
    std::vector<std::string> logLines() {
        std::lock_guard<std::mutex> guard(logMutex_);
        return lines_;
    }
    bool logContains(const std::string& text) {
        for (const auto& line : logLines())
            if (line.find(text) != std::string::npos) return true;
        return false;
    }

private:
    TempDir dir_;
    std::mutex logMutex_;
    std::vector<std::string> lines_;
    std::unique_ptr<meradb::Server> server_;
    std::thread thread_;
    std::atomic<bool> stopped_{false};
};

// The protocol by hand, with no client library: send whatever you like, read
// back exactly what the server sent.
class RawClient {
public:
    explicit RawClient(int port, double timeoutSeconds = 5.0)
        : socket_(meradb::net::connectTo("127.0.0.1", port, 2.0)), reader_(socket_) {
        socket_.setReceiveTimeout(timeoutSeconds);
    }
    void send(const meradb::protocol::Json& message) { meradb::protocol::send(socket_, message); }
    void sendRaw(const std::string& bytes) { socket_.sendAll(bytes); }
    std::optional<meradb::protocol::Json> receive() { return reader_.receive(); }
    meradb::protocol::Json request(const meradb::protocol::Json& message) {
        send(message);
        return receive().value();
    }
    // The hello of protocol.py's Connection; returns the server's reply.
    meradb::protocol::Json hello(const meradb::protocol::Json& password = nullptr,
                                 const meradb::protocol::Json& database = nullptr,
                                 const meradb::protocol::Json& user = nullptr) {
        meradb::protocol::Json message = meradb::protocol::Json::object();
        message["type"] = "hello";
        message["version"] = 1;
        message["password"] = password;
        message["database"] = database;
        message["user"] = user;
        return request(message);
    }
    meradb::protocol::Json query(const std::string& text) {
        meradb::protocol::Json message = meradb::protocol::Json::object();
        message["type"] = "query";
        message["text"] = text;
        return request(message);
    }
    meradb::net::Socket& socket() { return socket_; }

private:
    meradb::net::Socket socket_;
    meradb::protocol::MessageReader reader_;
};

}  // namespace meradb_test
```

`cpp/tests/test_server.cpp`:

```cpp
// cpp/tests/test_server.cpp -- the server, spoken to with the raw protocol (no client library yet).
#include <catch2/catch_test_macros.hpp>
#include "server_fixture.h"

using namespace meradb;
using namespace meradb_test;
using protocol::Json;

namespace {

std::string dumped(const Json& j) { return pyjson::dump(j); }

bool isClosed(RawClient& c) {
    try {
        return !c.receive().has_value();
    } catch (const net::NetError&) {
        return true;
    }
}

}  // namespace

TEST_CASE("server hello is answered with the version banner", "[server]") {
    RunningServer s;
    RawClient c(s.port());
    CHECK(dumped(c.hello()) == "{\"ok\": true, \"server\": \"MeraDB 1.0.0\", \"protocol\": 1, \"database\": \"main\"}");
}

TEST_CASE("server ping and unknown request types", "[server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    CHECK(dumped(c.request(Json{{"type", "ping"}})) == "{\"ok\": true}");
    CHECK(dumped(c.request(Json{{"type", "foo"}})) == "{\"ok\": false, \"error\": \"Unknown request type: 'foo'\"}");
    CHECK(dumped(c.request(Json::object())) == "{\"ok\": false, \"error\": \"Unknown request type: None\"}");
    CHECK(dumped(c.request(Json{{"type", 5}})) == "{\"ok\": false, \"error\": \"Unknown request type: 5\"}");
}

TEST_CASE("server refuses a wrong shared password and then closes", "[server]") {
    RunningServer s("s3cret");
    {
        RawClient c(s.port());
        CHECK(dumped(c.hello("nope")) == "{\"ok\": false, \"error\": \"Password galat hai\"}");
        CHECK(isClosed(c));
    }
    {
        RawClient c(s.port());
        CHECK(dumped(c.hello(nullptr)) == "{\"ok\": false, \"error\": \"Password galat hai\"}");  // no password at all
    }
    RawClient good(s.port());
    CHECK(good.hello("s3cret")["ok"] == true);
    CHECK(waitFor([&] { return s.logContains("login fail (galat password)"); }));
}

TEST_CASE("server per-user login supersedes the shared password", "[server]") {
    RunningServer s("s3cret");
    s.instance().users().create("ravi", "pw1");
    {
        RawClient c(s.port());
        CHECK(dumped(c.hello("pw1", nullptr, "ravi")) ==
              "{\"ok\": true, \"server\": \"MeraDB 1.0.0\", \"protocol\": 1, \"database\": \"main\"}");
    }
    {
        RawClient c(s.port());  // the SHARED password does not open a user login
        CHECK(dumped(c.hello("s3cret", nullptr, "ravi")) == "{\"ok\": false, \"error\": \"User ya password galat hai\"}");
        CHECK(isClosed(c));
    }
    {
        RawClient c(s.port());
        CHECK(dumped(c.hello("pw1", nullptr, "ghost")) == "{\"ok\": false, \"error\": \"User ya password galat hai\"}");
    }
    CHECK(waitFor([&] { return s.logContains("login fail (galat user/password: 'ghost')"); }));
}

TEST_CASE("server hello can pick the database", "[server]") {
    RunningServer s;
    {
        RawClient admin(s.port());
        admin.hello();
        admin.query("BANAO DATABASE college");
    }
    RawClient c(s.port());
    CHECK(c.hello(nullptr, "college")["database"] == "college");
    RawClient missing(s.port());
    CHECK(dumped(missing.hello(nullptr, "nope")) == "{\"ok\": false, \"error\": \"Database 'nope' exist nahi karta\"}");
    CHECK(isClosed(missing));
}

TEST_CASE("server refuses a restricted user who asks for a database at hello", "[server]") {
    RunningServer s;
    s.instance().users().create("ravi", "pw1");
    RawClient c(s.port());
    Json reply = c.hello("pw1", "main", "ravi");  // ISTEMAL is superuser-only, exactly like Python
    CHECK(reply["ok"] == false);
    CHECK(reply["error"].get<std::string>().find("superuser nahi hai") != std::string::npos);
}

TEST_CASE("server closes a connection whose first message is not a hello", "[server]") {
    RunningServer s;
    {
        RawClient c(s.port());
        c.send(Json{{"type", "ping"}});
        CHECK(isClosed(c));
    }
    {
        RawClient c(s.port());
        c.sendRaw("garbage\n");
        CHECK(isClosed(c));
    }
    {
        RawClient c(s.port());
        c.socket().shutdownBoth();  // connect and say nothing at all
    }
    RawClient fine(s.port());
    CHECK(fine.hello()["ok"] == true);  // the server is unharmed
}

TEST_CASE("server runs a script and returns Python-shaped results", "[server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    Json reply = c.query("BANAO TABLE t (i INT, f FLOAT, s TEXT, b BOOL); "
                         "DAALO MEIN t MAAN (1, 2.5, 'Ravi''s', SACH), (KHALI, 7.0, KHALI, JHOOTH); DIKHAO * SE t");
    CHECK(dumped(reply) ==
          "{\"ok\": true, \"results\": ["
          "{\"columns\": [], \"rows\": [], \"message\": \"Table 't' ban gaya (4 columns)\", \"error\": \"\"}, "
          "{\"columns\": [], \"rows\": [], \"message\": \"2 row(s) daal di\", \"error\": \"\"}, "
          "{\"columns\": [\"i\", \"f\", \"s\", \"b\"], \"rows\": [[1, 2.5, \"Ravi's\", true], [null, 7.0, null, false]], "
          "\"message\": \"2 row(s)\", \"error\": \"\"}], \"database\": \"main\", \"in_transaction\": false}");
}

TEST_CASE("server reports errors per statement and keeps going", "[server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    Json reply = c.query("DIKHAO * SE nahi_hai; DIKHAO TABLES;");
    REQUIRE(reply["results"].size() == 2);
    CHECK(reply["results"][0]["error"].get<std::string>().find("exist nahi karta") != std::string::npos);
    CHECK(reply["results"][1]["error"] == "");
    Json parse = c.query("DIKHAO SE");
    CHECK(parse["results"][0]["error"].get<std::string>().find("[Parser Galti]") == 0);
}

TEST_CASE("server tracks the session's database and transaction in every reply", "[server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    Json reply = c.query("BANAO DATABASE college; ISTEMAL college; SHURU");
    CHECK(reply["database"] == "college");
    CHECK(reply["in_transaction"] == true);
    reply = c.query("WAPAS");
    CHECK(reply["in_transaction"] == false);
}

TEST_CASE("server survives bad lines and keeps the connection open", "[server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    c.sendRaw("this is not json\n");
    Json bad = c.receive().value();
    CHECK(bad["ok"] == false);
    CHECK(bad["error"].get<std::string>().rfind("[Protocol Galti] Galat message: ", 0) == 0);
    c.sendRaw("[1, 2, 3]\n");
    CHECK(dumped(c.receive().value()) ==
          "{\"ok\": false, \"error\": \"[Protocol Galti] Message ek JSON object hona chahiye\"}");
    CHECK(dumped(c.request(Json{{"type", "ping"}})) == "{\"ok\": true}");
}

TEST_CASE("server status reports pid folder start time sessions and databases", "[server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    Json st = c.request(Json{{"type", "status"}});
    CHECK(st["ok"] == true);
    CHECK(st["server"] == "MeraDB 1.0.0");
    CHECK(st["pid"].get<int>() > 0);
    CHECK(st["data_dir"] == s.dataDir());
    CHECK(st["started"].get<std::string>().size() == 19);
    CHECK(st["sessions"] == 1);
    CHECK(dumped(st["databases"]) == "[\"main\"]");
    std::vector<std::string> keys;
    for (auto it = st.begin(); it != st.end(); ++it) keys.push_back(it.key());
    CHECK(keys == std::vector<std::string>{"ok", "server", "pid", "data_dir", "started", "sessions", "databases"});
}

TEST_CASE("server schema request returns the tree", "[server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    c.query("BANAO TABLE t (id INT MUKHYA KUNJI)");
    Json reply = c.request(Json{{"type", "schema"}});
    CHECK(reply["ok"] == true);
    CHECK(reply["tree"][0]["name"] == "main");
    CHECK(reply["tree"][0]["tables"][0]["columns"][0]["primary_key"] == true);
}

TEST_CASE("server counts sessions and logs connects and disconnects", "[server]") {
    RunningServer s;
    {
        RawClient a(s.port());
        a.hello();
        RawClient b(s.port());
        b.hello();
        CHECK(waitFor([&] { return s.server().sessions() == 2; }));
        CHECK(s.logContains("connected  (active sessions: "));
    }
    CHECK(waitFor([&] { return s.server().sessions() == 0; }));
    CHECK(s.logContains("disconnected  (active sessions: 0)"));
    for (const auto& line : s.logLines()) CHECK(line.size() > 21);  // "YYYY-MM-DD HH:MM:SS  " prefix
}

TEST_CASE("server logs queries only in verbose mode", "[server]") {
    RunningServer quiet;
    RunningServer loud("", /*verbose=*/true);
    for (RunningServer* s : {&quiet, &loud}) {
        RawClient c(s->port());
        c.hello();
        c.query("DIKHAO   TABLES");
        c.query("DIKHAO * SE gayab");
    }
    CHECK(loud.logContains("[main]  DIKHAO TABLES"));  // whitespace collapsed like Python
    CHECK_FALSE(quiet.logContains("[main]  DIKHAO TABLES"));
    CHECK(quiet.logContains("Table 'gayab' exist nahi karta"));  // errors are always logged
}

TEST_CASE("server accepts a shutdown request from this machine and stops", "[server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    CHECK(dumped(c.request(Json{{"type", "shutdown"}})) == "{\"ok\": true}");
    CHECK(waitFor([&] { return s.stopped(); }));
    CHECK(s.logContains("shutdown requested"));
    CHECK_THROWS_AS(RawClient(s.port()), net::NetError);  // the port is closed
}

TEST_CASE("server stops promptly even with idle clients connected", "[server]") {
    auto s = std::make_unique<RunningServer>();
    RawClient idle(s->port());
    idle.hello();
    RawClient silent(s->port());  // never even says hello
    auto started = std::chrono::steady_clock::now();
    s->stop();
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(took < 3.0);
    CHECK(isClosed(idle));
    CHECK(isClosed(silent));
}
```

- [ ] **Step 2: Add to `cpp/tests/CMakeLists.txt`; verify it fails**

```bash
cmake --build cpp/build
```
Expected: FAIL — `meradb/server.h: No such file or directory`.

- [ ] **Step 3: Header** — `cpp/include/meradb/server.h`

```cpp
// cpp/include/meradb/server.h
//
// The MeraDB SERVER: many clients, one database (mirrors meradb/server.py).
//
//      shell / workbench / app ── TCP :6372 ── Server ─┬─ Engine (session 1) ─┐
//         (newline-delimited JSON)                     ├─ Engine (session 2) ─┼─ Instance ── data/
//                                                      └─ Engine (session 3) ─┘  (one lock, one cache)
//
//   * One std::thread per connection. Each connection owns its OWN Engine (its
//     current database and transaction), created, used and destroyed on that
//     thread -- which is exactly what Engine's "SHURU thread rule" needs. All
//     sessions share ONE Instance, hence one lock and one index cache.
//   * A client that disconnects in the middle of a transaction is rolled back.
//   * Threads never block for ever in recv(): they poll a stop flag every
//     100 ms, so requestStop() (also callable from a signal handler) ends the
//     server promptly, and every session rolls back its own open transaction.
#pragma once
#include "meradb/engine.h"
#include "meradb/net_compat.h"
#include "meradb/protocol.h"
#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace meradb {

struct ServerOptions {
    std::string dataDir;
    std::string host = protocol::kDefaultHost;
    int port = protocol::kDefaultPort;  // 0 = let the OS choose (tests); read it back with Server::port()
    std::string password;               // empty = anyone may connect (the shared password)
    bool verbose = false;               // log every query
    // Where log lines go (already stamped "YYYY-MM-DD HH:MM:SS  message"). Default: stdout.
    std::function<void(const std::string&)> log;
    std::size_t maxMessageBytes = protocol::kMaxMessageBytes;  // tests lower it
};

class Server {
public:
    // Opens the data folder as the serving process and binds the port.
    // Throws MeraDBError (folder problems) or net::NetError (port busy, ...).
    explicit Server(ServerOptions options);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    int port() const { return port_; }
    int sessions() const { return sessions_.load(); }
    Instance& instance() { return *instance_; }
    const std::string& started() const { return started_; }
    const ServerOptions& options() const { return options_; }
    void log(const std::string& message);

    // Accept loop; returns after requestStop() (or a client's shutdown request),
    // once every connection thread has finished.
    void serveForever();
    // Async-signal-safe: only sets a flag.
    void requestStop() { stop_ = true; }

private:
    struct Connection {
        net::Socket socket;
        std::thread thread;
        std::atomic<bool> finished{false};
    };

    void serveConnection(Connection& connection);
    protocol::Json dispatch(Engine& session, const protocol::Json& request, const std::string& peerIp,
                            const std::string& peer, bool& stopConnection);
    void reap(bool everything);

    ServerOptions options_;
    std::shared_ptr<Instance> instance_;
    net::Socket listener_;
    int port_ = 0;
    std::string started_;
    std::atomic<bool> stop_{false};
    std::atomic<int> sessions_{0};
    std::mutex logMutex_;
    std::list<std::unique_ptr<Connection>> connections_;  // touched only by the serveForever thread
};

// `meradb server`: run in the foreground until Ctrl+C or `meradb stop`.
// Writes/removes meradb.pid, logs like Python's serve(). Returns the exit code.
int serve(ServerOptions options);

}  // namespace meradb
```

- [ ] **Step 4: Implementation** — `cpp/src/server.cpp`

```cpp
// cpp/src/server.cpp
#include "meradb/server.h"
#include "meradb/ast.h"
#include "meradb/datatypes.h"
#include "meradb/errors.h"
#include "meradb/sys_compat.h"
#include <algorithm>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <system_error>

namespace meradb {

using protocol::Json;

namespace {

// Python's LOOPBACK tuple (matched against the peer's textual address).
bool isLoopback(const std::string& ip) { return ip == "127.0.0.1" || ip == "::1" || ip == "localhost"; }

const Json* field(const Json& object, const char* key) {
    auto it = object.find(key);
    return it == object.end() ? nullptr : &*it;
}

// Python truthiness of a JSON value (`if user:` / `x or ""`).
bool pyTruthy(const Json* j) {
    if (j == nullptr || j->is_null()) return false;
    if (j->is_boolean()) return j->get<bool>();
    if (j->is_number_integer()) return j->is_number_unsigned() ? j->get<std::uint64_t>() != 0 : j->get<std::int64_t>() != 0;
    if (j->is_number_float()) return j->get<double>() != 0.0;
    if (j->is_string()) return !j->get_ref<const std::string&>().empty();
    return !j->empty();
}

// Python's str() of a JSON value.
std::string pyStr(const Json& j) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_null()) return "None";
    if (j.is_boolean()) return j.get<bool>() ? "True" : "False";
    if (j.is_number_float()) return pyReprFloat(j.get<double>());
    if (j.is_number_unsigned()) return std::to_string(j.get<std::uint64_t>());
    if (j.is_number_integer()) return std::to_string(j.get<std::int64_t>());
    return pyjson::dump(j);
}

// Python's repr() of a JSON value, for `{kind!r}`.
std::string pyReprJson(const Json& j) { return j.is_string() ? pyRepr(j.get<std::string>()) : pyStr(j); }

// hmac.compare_digest: the running time does not depend on where the strings differ.
bool constantTimeEquals(const std::string& a, const std::string& b) {
    unsigned char diff = a.size() == b.size() ? 0 : 1;
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

// ' '.join(text.split())[:limit]  (limit counts characters, not bytes)
std::string collapseWhitespace(const std::string& text, std::size_t limit) {
    std::string out;
    bool pendingSpace = false;
    std::size_t chars = 0;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f') {
            pendingSpace = !out.empty();
            continue;
        }
        const bool startsCharacter = (static_cast<unsigned char>(c) & 0xC0) != 0x80;
        if (pendingSpace) {
            if (chars >= limit) break;
            out += ' ';
            ++chars;
            pendingSpace = false;
        }
        if (startsCharacter) {
            if (chars >= limit) break;
            ++chars;
        }
        out += c;
    }
    return out;
}

Json failure(const std::string& message) {
    Json reply = Json::object();
    reply["ok"] = false;
    reply["error"] = message;
    return reply;
}

Json success() {
    Json reply = Json::object();
    reply["ok"] = true;
    return reply;
}

}  // namespace

// ============================================================================
// Server
// ============================================================================

Server::Server(ServerOptions options) : options_(std::move(options)) {
    if (!options_.log) options_.log = [](const std::string& line) { std::cout << line << std::endl; };
    instance_ = std::make_shared<Instance>(options_.dataDir, /*served=*/true);
    started_ = sys::localIsoSeconds();
    listener_ = net::listenOn(options_.host, options_.port);
    port_ = listener_.localPort();
}

Server::~Server() {
    stop_ = true;
    reap(true);  // only non-empty if serveForever() was never allowed to finish
}

void Server::log(const std::string& message) {
    std::lock_guard<std::mutex> guard(logMutex_);
    options_.log(sys::localLogStamp() + "  " + message);
}

void Server::reap(bool everything) {
    for (auto it = connections_.begin(); it != connections_.end();) {
        if (everything || (*it)->finished.load()) {
            if ((*it)->thread.joinable()) (*it)->thread.join();
            it = connections_.erase(it);
        } else {
            ++it;
        }
    }
}

void Server::serveForever() {
    while (!stop_) {
        net::Socket accepted;
        try {
            accepted = net::acceptWithTimeout(listener_, 0.1);
        } catch (const net::NetError& e) {
            log(std::string("accept fail: ") + e.what());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        reap(false);
        if (!accepted.valid()) continue;

        auto connection = std::make_unique<Connection>();
        connection->socket = std::move(accepted);
        Connection* raw = connection.get();
        try {
            raw->thread = std::thread([this, raw] {
                serveConnection(*raw);
                raw->finished = true;
            });
        } catch (const std::system_error& e) {
            log(std::string("connection thread start nahi hua: ") + e.what());
            continue;  // `connection` (and its socket) is dropped
        }
        connections_.push_back(std::move(connection));
    }
    listener_.close();
    reap(true);  // stop_ is set: readers give up within 100 ms; each thread rolls back its own transaction
}

// Runs on the connection's own thread from the first byte to the last: the
// Engine (and any transaction it opens) never changes threads.
void Server::serveConnection(Connection& connection) {
    net::Socket& socket = connection.socket;
    const std::string peerIp = socket.peerAddress();
    const std::string peer = peerIp + ":" + std::to_string(socket.peerPort());
    try {
        protocol::MessageReader reader(socket, options_.maxMessageBytes, &stop_);

        // ---- 1. handshake ----
        std::optional<Json> hello;
        try {
            hello = reader.receive();
        } catch (const protocol::ProtocolError&) {
            return;
        }
        const Json* type = hello ? field(*hello, "type") : nullptr;
        if (!hello || type == nullptr || !type->is_string() || type->get<std::string>() != "hello") return;

        const Json* userField = field(*hello, "user");
        const Json* passwordField = field(*hello, "password");
        const bool hasUser = pyTruthy(userField);
        const std::string user = hasUser ? pyStr(*userField) : "";
        const std::string password = pyTruthy(passwordField) ? pyStr(*passwordField) : "";
        if (hasUser) {
            // A per-user login SUPERSEDES the shared server password entirely.
            // The message is bare: the client wraps it in ConnectionFailed, which adds the "[Connection Galti] " tag.
            if (!instance_->users().verify(user, password)) {
                protocol::send(socket, failure("User ya password galat hai"));
                log(peer + "  login fail (galat user/password: " + pyRepr(user) + ")");
                return;
            }
        } else if (!options_.password.empty() && !constantTimeEquals(password, options_.password)) {
            protocol::send(socket, failure("Password galat hai"));
            log(peer + "  login fail (galat password)");
            return;
        }

        Engine session(instance_);
        if (hasUser) session.user = user;
        ++sessions_;
        log(peer + "  connected" + (hasUser ? " as " + pyRepr(user) : std::string()) +
            "  (active sessions: " + std::to_string(sessions_.load()) + ")");

        struct Cleanup {  // Python's `finally:` -- roll back, count down, log
            Server& server;
            Engine& session;
            const std::string& peer;
            ~Cleanup() {
                try {
                    session.close();  // rolls back an unfinished transaction, on the thread that began it
                } catch (...) {
                }
                --server.sessions_;
                server.log(peer + "  disconnected  (active sessions: " + std::to_string(server.sessions_.load()) + ")");
            }
        } cleanup{*this, session, peer};

        const Json* database = field(*hello, "database");
        if (pyTruthy(database)) {
            try {
                ast::UseDatabase use;
                use.name = pyStr(*database);
                session.executeStatement(use);
            } catch (const MeraDBError& e) {
                // .message(), not what(): the client wraps this in ConnectionFailed (see above)
                protocol::send(socket, failure(e.message()));
                return;
            }
        }
        Json ready = Json::object();
        ready["ok"] = true;
        ready["server"] = protocol::kServerName;
        ready["protocol"] = protocol::kVersion;
        ready["database"] = session.currentDb;
        protocol::send(socket, ready);

        // ---- 2. request loop ----
        for (;;) {
            std::optional<Json> request;
            try {
                request = reader.receive();
            } catch (const protocol::ProtocolError& e) {
                protocol::send(socket, failure(std::string("[Protocol Galti] ") + e.what()));
                continue;
            }
            if (!request) break;  // client closed the connection (or the server is stopping)
            bool stopConnection = false;
            Json reply;
            try {
                reply = dispatch(session, *request, peerIp, peer, stopConnection);
            } catch (const std::exception& e) {  // a bug must not kill the whole server
                log(peer + "  INTERNAL ERROR\n" + e.what());
                reply = failure(std::string("[Internal Galti] ") + e.what());
            }
            protocol::send(socket, reply);
            if (stopConnection) break;
        }
    } catch (const net::NetError&) {
        // client vanished
    } catch (const std::exception& e) {
        log(peer + "  INTERNAL ERROR\n" + e.what());
    }
}

Json Server::dispatch(Engine& session, const Json& request, const std::string& peerIp, const std::string& peer,
                      bool& stopConnection) {
    const Json* typeField = field(request, "type");
    const Json kind = typeField ? *typeField : Json(nullptr);
    const std::string type = kind.is_string() ? kind.get<std::string>() : "";

    if (type == "query") {
        const Json* textField = field(request, "text");
        const std::string text = textField ? pyStr(*textField) : "";
        if (options_.verbose) log(peer + "  [" + session.currentDb + "]  " + collapseWhitespace(text, 200));
        std::vector<Result> results;
        try {
            results = session.runScript(text);
        } catch (const std::exception& e) {
            log(peer + "  INTERNAL ERROR\n" + e.what());
            Result failed;
            failed.error = std::string("[Internal Galti] ") + e.what();
            results.push_back(std::move(failed));
        }
        Json encoded = Json::array();
        for (const auto& r : results) {
            if (!r.error.empty()) log(peer + "  " + r.error);
            encoded.push_back(protocol::resultToJson(r));
        }
        Json reply = success();
        reply["results"] = std::move(encoded);
        reply["database"] = session.currentDb;
        reply["in_transaction"] = session.inTransaction();
        return reply;
    }

    if (type == "schema") {
        try {
            Json reply = success();
            reply["tree"] = session.schemaTree();
            return reply;
        } catch (const MeraDBError& e) {
            return failure(e.message());  // bare: the client wraps it in ConnectionFailed
        }
    }

    if (type == "status") {
        Json reply = success();
        reply["server"] = protocol::kServerName;
        reply["pid"] = static_cast<std::int64_t>(sys::processId());
        reply["data_dir"] = instance_->dataDir();
        reply["started"] = started_;
        reply["sessions"] = sessions_.load();
        Json databases = Json::array();
        for (const auto& name : instance_->databases()) databases.push_back(name);
        reply["databases"] = std::move(databases);
        return reply;
    }

    if (type == "ping") return success();

    if (type == "shutdown") {
        if (!isLoopback(peerIp))
            return failure("Shutdown sirf usi computer se ho sakta hai jahan server chal raha hai");
        log(peer + "  shutdown requested");
        stop_ = true;
        stopConnection = true;
        return success();
    }

    return failure("Unknown request type: " + pyReprJson(kind));
}

// ============================================================================
// `meradb server`
// ============================================================================

namespace {

std::atomic<Server*> g_running{nullptr};
std::atomic<bool> g_signalled{false};

extern "C" void onStopSignal(int) {
    g_signalled = true;
    if (Server* server = g_running.load()) server->requestStop();
}

std::string plainText(const Json& j) { return j.is_string() ? j.get<std::string>() : j.dump(); }

}  // namespace

int serve(ServerOptions options) {
    namespace fs = std::filesystem;
    options.dataDir = fs::absolute(fs::path(options.dataDir)).string();
    std::error_code ec;
    if (fs::is_directory(options.dataDir, ec)) {
        if (auto existing = protocol::runningServer(options.dataDir)) {
            std::cerr << "Is data folder ka server pehle se chal raha hai: "
                      << plainText(existing->value("host", Json(protocol::kDefaultHost))) << ":"
                      << plainText(existing->value("port", Json(protocol::kDefaultPort))) << " (pid "
                      << plainText(existing->value("pid", Json(nullptr))) << ")\n";
            return 1;
        }
    }

    std::unique_ptr<Server> server;
    try {
        server = std::make_unique<Server>(options);
    } catch (const net::NetError& e) {
        std::cerr << "Server start nahi hua (" << options.host << ":" << options.port << "): " << e.what() << "\n";
        return 1;
    } catch (const MeraDBError& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    const std::int64_t pid = sys::processId();
    Json info = Json::object();
    info["pid"] = pid;
    info["host"] = options.host;
    info["port"] = server->port();
    info["started"] = server->started();
    try {
        protocol::writePidFile(options.dataDir, info);
    } catch (const MeraDBError& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }

    server->log(std::string(protocol::kServerName) + " server chal raha hai  ->  " + options.host + ":" +
                std::to_string(server->port()));
    server->log("data folder: " + options.dataDir);
    for (const auto& db : server->instance().recovered())
        server->log("RECOVERY: database '" + db + "' ka adhoora transaction WAPAS kiya (pichli baar crash hua tha)");
    if (options.password.empty()) {
        server->log("password: nahi (koi bhi connect kar sakta hai)");
        if (options.host != "127.0.0.1" && options.host != "::1" && options.host != "localhost")
            server->log("WARNING: bina password ke network par khula hai! --password use karo");
    }
    server->log("band karne ke liye: Ctrl+C  ya  meradb stop");

    g_signalled = false;
    g_running = server.get();
    std::signal(SIGINT, onStopSignal);
    std::signal(SIGTERM, onStopSignal);
    server->serveForever();
    g_running = nullptr;
    if (g_signalled) server->log("Ctrl+C -- band ho raha hai");

    protocol::removePidFile(options.dataDir, pid);
    server->log("server band");
    return 0;
}

}  // namespace meradb
```

- [ ] **Step 5: CMake** — add `src/server.cpp` to `meradb_core`.

- [ ] **Step 6: Build and run**

```bash
cmake -S cpp -B cpp/build -G "MinGW Makefiles"
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "^server "
```
Expected: 17 `server` test cases pass. Run them three times in a row
(`ctest ... --repeat until-fail:3`): a flaky result means a thread-lifecycle bug, not a
test problem.

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/server.h cpp/src/server.cpp cpp/tests/server_fixture.h cpp/tests/test_server.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add the TCP server: thread per connection, one shared engine lock"
```

**Completion checklist:**
- [ ] every reply's key order and error wording matches the Python server (tests compare exact strings)
- [ ] `Engine` is created inside the connection thread (grep `serveConnection`)
- [ ] no thread ever blocks in `recv` without the stop-flag poll
- [ ] a client killed mid-transaction frees the lock and rolls back (Task 18 asserts it)
- [ ] passes three runs in a row; full suite green, zero warnings

---

### Task 17: `Backend`, `LocalBackend` and the client `Connection`

**Files:**
- Create: `cpp/include/meradb/backend.h`, `cpp/include/meradb/client.h`, `cpp/src/client.cpp`
- Test: `cpp/tests/test_client.cpp`
- Modify: `cpp/CMakeLists.txt` (add `src/client.cpp`), `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `net_compat`, `protocol`, `Server` (tests only).
- Produces: `Backend` (D8), `LocalBackend(dataDir)` / `LocalBackend(shared_ptr<Instance>)`,
  `ConnectOptions{host, port, password, database, user, connectTimeoutSeconds = 5, timeoutSeconds = 60}`,
  `Connection` (a `Backend`) with `status()` and `shutdown()`.
- Python quirks mirrored on purpose: `Connection.execute` re-wraps the server's already tagged
  error as a plain `MeraDBError`, so the text keeps its `[Stage Galti]` tag; a nobody-listening
  connect is `ServerUnavailable("{host}:{port} par MeraDB server nahi mila (...)")`, everything
  else `ConnectionFailed`; a lost server mid-session reports
  `Server ne connection band kar diya` or `Server se connection toot gaya: ...`.

- [ ] **Step 1: Write the failing test** — `cpp/tests/test_client.cpp`

```cpp
// cpp/tests/test_client.cpp -- Backend, LocalBackend and the client Connection.
#include <catch2/catch_test_macros.hpp>
#include "meradb/client.h"
#include "meradb/errors.h"
#include "server_fixture.h"

using namespace meradb;
using namespace meradb_test;

namespace {

ConnectOptions to(const RunningServer& s) {
    ConnectOptions options;
    options.port = s.port();
    return options;
}

}  // namespace

// ---- LocalBackend ----

TEST_CASE("client LocalBackend wraps an embedded engine", "[client]") {
    TempDir dir;
    LocalBackend b(dir.str());
    CHECK(b.description() == "local (" + b.engine().instance().dataDir() + ")");
    CHECK(b.currentDb() == "main");
    b.execute("BANAO TABLE t (x INT); SHURU");
    CHECK(b.inTransaction());
    CHECK(b.runScript("DIKHAO * SE nahi_hai")[0].error.find("exist nahi karta") != std::string::npos);
    CHECK_THROWS_AS(b.execute("DIKHAO SE"), ParseError);  // the real exception, like Engine.execute
    CHECK(b.schemaTree()[0]["tables"][0]["name"] == "t");
    b.close();  // rolls the transaction back
    CHECK_FALSE(b.inTransaction());
}

// ---- Connection ----

TEST_CASE("client Connection round-trips types and dates", "[client]") {
    RunningServer s;
    Connection db(to(s));
    CHECK(db.serverVersion() == "MeraDB 1.0.0");
    CHECK(db.description() == "127.0.0.1:" + std::to_string(s.port()));
    CHECK(db.currentDb() == "main");
    db.execute("BANAO TABLE t (i INT, f FLOAT, s TEXT, b BOOL, d DATE); "
               "DAALO MEIN t MAAN (1, 2.5, 'Ravi''s', SACH, '2005-06-15'), (KHALI, 7.0, KHALI, JHOOTH, KHALI)");
    auto result = db.execute("DIKHAO * SE t KRAM i")[0];
    REQUIRE(result.rows.size() == 2);
    CHECK(result.columns == std::vector<std::string>{"i", "f", "s", "b", "d"});
    const auto& null_row = result.rows[0];  // KHALI sorts first
    CHECK(null_row[0].isNull());
    CHECK(std::holds_alternative<double>(null_row[1].data));  // 7.0 stays a float
    CHECK(std::get<double>(null_row[1].data) == 7.0);
    CHECK(std::get<bool>(null_row[3].data) == false);
    const auto& full_row = result.rows[1];
    CHECK(std::get<int64_t>(full_row[0].data) == 1);
    CHECK(std::get<std::string>(full_row[2].data) == "Ravi's");
    CHECK(std::get<Date>(full_row[4].data) == parseDate("2005-06-15"));
}

TEST_CASE("client Connection runScript reports errors per statement", "[client]") {
    RunningServer s;
    Connection db(to(s));
    auto results = db.runScript("DIKHAO * SE nahi_hai; DIKHAO TABLES;");
    REQUIRE(results.size() == 2);
    CHECK(results[0].error.find("exist nahi karta") != std::string::npos);
    CHECK(results[1].error.empty());
    CHECK(db.runScript("DIKHAO SE")[0].error.find("Parser") != std::string::npos);
}

TEST_CASE("client Connection execute throws the first error wrapped once more", "[client]") {
    RunningServer s;
    Connection db(to(s));
    try {
        db.execute("DIKHAO * SE nahi_hai");
        FAIL("expected an error");
    } catch (const MeraDBError& e) {
        // the server's text already has its own tag; Connection.execute adds the plain one, like Python
        CHECK(e.message() == "[Execution Galti] Table 'nahi_hai' exist nahi karta");
        CHECK(std::string(e.what()) == "[MeraDB Galti] [Execution Galti] Table 'nahi_hai' exist nahi karta");
    }
}

TEST_CASE("client Connection tracks database and transaction state", "[client]") {
    RunningServer s;
    Connection db(to(s));
    db.execute("BANAO DATABASE college; ISTEMAL college");
    CHECK(db.currentDb() == "college");
    db.execute("SHURU");
    CHECK(db.inTransaction());
    db.execute("WAPAS");
    CHECK_FALSE(db.inTransaction());
}

TEST_CASE("client Connection can start in a database", "[client]") {
    RunningServer s;
    {
        Connection admin(to(s));
        admin.execute("BANAO DATABASE college");
    }
    ConnectOptions options = to(s);
    options.database = "college";
    Connection db(options);
    CHECK(db.currentDb() == "college");
    options.database = "nope";
    try {
        Connection bad(options);
        FAIL("expected a refusal");
    } catch (const ConnectionFailed& e) {
        CHECK(std::string(e.what()) == "[Connection Galti] Database 'nope' exist nahi karta");
    }
}

TEST_CASE("client Connection reports a missing server as ServerUnavailable", "[client]") {
    int port;
    {
        net::Socket listener = net::listenOn("127.0.0.1", 0);
        port = listener.localPort();
    }
    ConnectOptions options;
    options.port = port;
    try {
        Connection db(options);
        FAIL("expected ServerUnavailable");
    } catch (const ServerUnavailable& e) {
        std::string text = e.message();
        CHECK(text.rfind("127.0.0.1:" + std::to_string(port) + " par MeraDB server nahi mila (", 0) == 0);
        CHECK(text.back() == ')');
    }
}

TEST_CASE("client Connection wrong password is ConnectionFailed not ServerUnavailable", "[client]") {
    RunningServer s("s3cret");
    ConnectOptions options = to(s);
    options.password = "nope";
    try {
        Connection db(options);
        FAIL("expected a refusal");
    } catch (const ServerUnavailable&) {
        FAIL("must not be ServerUnavailable");
    } catch (const ConnectionFailed& e) {
        CHECK(std::string(e.what()) == "[Connection Galti] Password galat hai");
    }
    options.password = "s3cret";
    Connection ok(options);
    CHECK(ok.execute("DIKHAO TABLES")[0].rows.empty());
    ConnectOptions none = to(s);  // no password at all
    CHECK_THROWS_AS(Connection(none), ConnectionFailed);
}

TEST_CASE("client Connection logs in as a user", "[client]") {
    RunningServer s;
    s.instance().users().create("ravi", "secret123");
    ConnectOptions options = to(s);
    options.user = "ravi";
    options.password = "wrong";
    try {
        Connection db(options);
        FAIL("expected a refusal");
    } catch (const ConnectionFailed& e) {
        CHECK(std::string(e.what()) == "[Connection Galti] User ya password galat hai");
    }
    options.password = "secret123";
    Connection db(options);
    CHECK(db.runScript("BANAO TABLE t2 (a INT)")[0].error.find("superuser nahi hai") != std::string::npos);
}

TEST_CASE("client Connection schemaTree and status", "[client]") {
    RunningServer s;
    Connection db(to(s));
    db.execute("BANAO TABLE t (id INT MUKHYA KUNJI)");
    auto tree = db.schemaTree();
    CHECK(tree[0]["name"] == "main");
    CHECK(tree[0]["tables"][0]["columns"][0]["primary_key"] == true);
    auto st = db.status();
    CHECK(st["ok"] == true);
    CHECK(st["sessions"] == 1);
}

TEST_CASE("client Connection fails cleanly after close or when the server goes away", "[client]") {
    auto s = std::make_unique<RunningServer>();
    Connection db(to(*s));
    Connection closed(to(*s));
    closed.close();
    CHECK_THROWS_AS(closed.runScript("DIKHAO TABLES"), ConnectionFailed);

    s->stop();  // the server hangs up on live connections
    try {
        db.runScript("DIKHAO TABLES");
        FAIL("expected ConnectionFailed");
    } catch (const ConnectionFailed& e) {
        // a clean hang-up, or a reset while writing -- both are Python's two ConnectionFailed texts
        const std::string text = e.message();
        CHECK((text.rfind("Server ne connection band kar diya", 0) == 0 ||
               text.rfind("Server se connection toot gaya: ", 0) == 0));
    }
}

TEST_CASE("client Connection times out on a server that never answers", "[client]") {
    net::Socket mute = net::listenOn("127.0.0.1", 0);  // accepts connections at the OS level, says nothing
    ConnectOptions options;
    options.port = mute.localPort();
    options.timeoutSeconds = 0.3;
    try {
        Connection db(options);
        FAIL("expected a timeout");
    } catch (const ConnectionFailed& e) {
        CHECK(e.message() == "Server se connection toot gaya: timed out");
    }
}

TEST_CASE("client Connection shutdown stops the server", "[client]") {
    RunningServer s;
    Connection db(to(s));
    db.shutdown();
    CHECK(waitFor([&] { return s.stopped(); }));
}
```

- [ ] **Step 2: Add to `cpp/tests/CMakeLists.txt`; verify it fails**

```bash
cmake --build cpp/build
```
Expected: FAIL — `meradb/client.h: No such file or directory`.

- [ ] **Step 3: Headers**

`cpp/include/meradb/backend.h`:

```cpp
// cpp/include/meradb/backend.h
//
// "Something you can run MeraDB statements against": an embedded engine
// (LocalBackend) or a connection to a server (Connection, client.h). Python
// gets this by duck typing (Engine and Connection have the same methods); C++
// needs an explicit interface. The command-line tool -- and, later, the shell
// and the workbench -- only ever talk to a Backend.
#pragma once
#include "meradb/engine.h"
#include <memory>
#include <string>
#include <vector>

namespace meradb {

class Backend {
public:
    virtual ~Backend() = default;

    // Never throws a MeraDBError for a bad statement: it gets Result.error.
    virtual std::vector<Result> runScript(const std::string& text) = 0;
    // Like runScript but throws the first error (Python's Engine.execute / Connection.execute).
    virtual std::vector<Result> execute(const std::string& text) = 0;
    virtual std::string currentDb() = 0;
    virtual bool inTransaction() = 0;
    virtual nlohmann::ordered_json schemaTree() = 0;
    // "local (<data folder>)" or "<host>:<port>".
    virtual std::string description() = 0;
    virtual void close() = 0;
};

// The engine runs inside this process and opens the data folder directly (like SQLite).
class LocalBackend : public Backend {
public:
    explicit LocalBackend(const std::string& dataDir) : engine_(dataDir) {}
    explicit LocalBackend(std::shared_ptr<Instance> instance) : engine_(std::move(instance)) {}

    Engine& engine() { return engine_; }

    std::vector<Result> runScript(const std::string& text) override { return engine_.runScript(text); }
    std::vector<Result> execute(const std::string& text) override { return engine_.execute(text); }
    std::string currentDb() override { return engine_.currentDb; }
    bool inTransaction() override { return engine_.inTransaction(); }
    nlohmann::ordered_json schemaTree() override { return engine_.schemaTree(); }
    std::string description() override { return "local (" + engine_.instance().dataDir() + ")"; }
    void close() override { engine_.close(); }

private:
    Engine engine_;
};

}  // namespace meradb
```

`cpp/include/meradb/client.h`:

```cpp
// cpp/include/meradb/client.h
//
// The MeraDB CLIENT library: talk to a running server from C++ (mirrors
// meradb/client.py).
//
//     meradb::ConnectOptions options;      // 127.0.0.1:6372 by default
//     meradb::Connection db(options);
//     for (const auto& result : db.runScript("DIKHAO * SE students;")) ...
//
// A Connection is a Backend, so the shell / workbench / `meradb run` use it
// exactly like an embedded engine.
#pragma once
#include "meradb/backend.h"
#include "meradb/net_compat.h"
#include "meradb/protocol.h"
#include <memory>
#include <optional>
#include <string>

namespace meradb {

struct ConnectOptions {
    std::string host = protocol::kDefaultHost;
    int port = protocol::kDefaultPort;
    std::optional<std::string> password;  // the shared server password
    std::optional<std::string> database;  // start in this database
    std::optional<std::string> user;      // log in as this user (then `password` is THEIR password)
    double connectTimeoutSeconds = 5.0;
    // Queries may legitimately wait (up to ~10 s) for another client's transaction.
    double timeoutSeconds = 60.0;
};

class Connection : public Backend {
public:
    // Throws ServerUnavailable (nobody listening) or ConnectionFailed (refused, e.g. wrong password).
    explicit Connection(const ConnectOptions& options);
    ~Connection() override { close(); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    std::vector<Result> runScript(const std::string& text) override;
    // Python quirk kept on purpose: the server's error text already carries its
    // "[Stage Galti]" tag, and is wrapped once more as a plain MeraDBError.
    std::vector<Result> execute(const std::string& text) override;
    std::string currentDb() override { return currentDb_; }
    bool inTransaction() override { return inTransaction_; }
    nlohmann::ordered_json schemaTree() override;
    std::string description() override { return options_.host + ":" + std::to_string(options_.port); }
    void close() override { socket_.close(); }

    const std::string& serverVersion() const { return serverVersion_; }
    protocol::Json status();  // the server's raw status reply
    void shutdown();          // ask the server to stop (loopback only)

private:
    protocol::Json request(const protocol::Json& message);

    ConnectOptions options_;
    net::Socket socket_;
    std::unique_ptr<protocol::MessageReader> reader_;
    std::string currentDb_ = DEFAULT_DATABASE;
    bool inTransaction_ = false;
    std::string serverVersion_ = "MeraDB";
};

}  // namespace meradb
```

- [ ] **Step 4: Implementation** — `cpp/src/client.cpp`

```cpp
// cpp/src/client.cpp
#include "meradb/client.h"
#include "meradb/errors.h"

namespace meradb {

using protocol::Json;

namespace {

bool isOk(const Json& reply) {
    auto it = reply.find("ok");
    return it != reply.end() && it->is_boolean() && it->get<bool>();
}

// reply.get(key, fallback) for a string-valued key
std::string stringField(const Json& reply, const char* key, const std::string& fallback) {
    auto it = reply.find(key);
    return (it != reply.end() && it->is_string()) ? it->get<std::string>() : fallback;
}

}  // namespace

Connection::Connection(const ConnectOptions& options) : options_(options) {
    try {
        socket_ = net::connectTo(options.host, options.port, options.connectTimeoutSeconds);
    } catch (const net::NetError& e) {
        throw ServerUnavailable(options.host + ":" + std::to_string(options.port) + " par MeraDB server nahi mila (" +
                                e.what() + ")");
    }
    socket_.setReceiveTimeout(options.timeoutSeconds);
    reader_ = std::make_unique<protocol::MessageReader>(socket_);

    // `user`, if given, authenticates as that SPECIFIC user (checked against users.json)
    // instead of the single shared server password.
    Json hello = Json::object();
    hello["type"] = "hello";
    hello["version"] = protocol::kVersion;
    hello["password"] = options.password ? Json(*options.password) : Json(nullptr);
    hello["database"] = options.database ? Json(*options.database) : Json(nullptr);
    hello["user"] = options.user ? Json(*options.user) : Json(nullptr);
    Json reply = request(hello);
    if (!isOk(reply)) {
        close();
        throw ConnectionFailed(stringField(reply, "error", "Server ne connection mana kar diya"));
    }
    serverVersion_ = stringField(reply, "server", "MeraDB");
    currentDb_ = stringField(reply, "database", DEFAULT_DATABASE);
    inTransaction_ = false;
}

Json Connection::request(const Json& message) {
    try {
        protocol::send(socket_, message);
        auto reply = reader_->receive();
        if (!reply) throw ConnectionFailed("Server ne connection band kar diya");
        return *reply;
    } catch (const net::NetError& e) {
        throw ConnectionFailed(std::string("Server se connection toot gaya: ") + e.what());
    } catch (const protocol::ProtocolError& e) {
        throw ConnectionFailed(std::string("Server se connection toot gaya: ") + e.what());
    }
}

std::vector<Result> Connection::runScript(const std::string& text) {
    Json message = Json::object();
    message["type"] = "query";
    message["text"] = text;
    Json reply = request(message);
    if (!isOk(reply)) {
        Result failed;
        failed.error = stringField(reply, "error", "Unknown error");
        return {failed};
    }
    try {
        std::vector<Result> results;
        for (const auto& encoded : reply.at("results")) results.push_back(protocol::resultFromJson(encoded));
        currentDb_ = stringField(reply, "database", currentDb_);
        auto txn = reply.find("in_transaction");
        inTransaction_ = txn != reply.end() && txn->is_boolean() && txn->get<bool>();
        return results;
    } catch (const protocol::ProtocolError& e) {
        throw ConnectionFailed(std::string("Server se connection toot gaya: ") + e.what());
    } catch (const nlohmann::json::exception& e) {
        throw ConnectionFailed(std::string("Server se connection toot gaya: Galat message: ") + e.what());
    }
}

std::vector<Result> Connection::execute(const std::string& text) {
    auto results = runScript(text);
    for (const auto& r : results)
        if (!r.error.empty()) throw MeraDBError(r.error);
    return results;
}

nlohmann::ordered_json Connection::schemaTree() {
    Json message = Json::object();
    message["type"] = "schema";
    Json reply = request(message);
    if (!isOk(reply)) throw ConnectionFailed(stringField(reply, "error", "schema nahi mila"));
    auto tree = reply.find("tree");
    if (tree == reply.end()) throw ConnectionFailed("schema nahi mila");
    return *tree;
}

Json Connection::status() {
    Json message = Json::object();
    message["type"] = "status";
    return request(message);
}

void Connection::shutdown() {
    Json message = Json::object();
    message["type"] = "shutdown";
    Json reply = request(message);
    if (!isOk(reply)) throw ConnectionFailed(stringField(reply, "error", "shutdown fail"));
}

}  // namespace meradb
```

- [ ] **Step 5: CMake** — add `src/client.cpp` to `meradb_core`.

- [ ] **Step 6: Build and run**

```bash
cmake -S cpp -B cpp/build -G "MinGW Makefiles"
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "^client "
```
Expected: 13 `client` test cases pass (Task 18's `client_server` cases are not built yet).

- [ ] **Step 7: Commit**

```bash
git add cpp/include/meradb/backend.h cpp/include/meradb/client.h cpp/src/client.cpp cpp/tests/test_client.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add Backend, LocalBackend and the client Connection"
```

**Completion checklist:**
- [ ] `Connection` and `LocalBackend` are interchangeable through `Backend&` (a test runs the same script through both)
- [ ] error texts match `client.py` character for character
- [ ] the destructor of a `Connection` never throws
- [ ] full suite green, zero warnings

---

### Task 18: In-process client/server tests

**Files:**
- Test: `cpp/tests/test_client_server.cpp` (create)
- Modify: `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `RunningServer`, `Connection` (Tasks 16-17). No production code changes — this
  task exists to pin down the multi-session behaviour the design rests on. If a test fails,
  fix the *server or engine*, not the test.

These are the C++ twins of `tests/test_server.py` and of the concurrency notes in
`meradb/engine.py`: session-local current database, shared constraints, a transaction
that spans requests, isolation (`Database busy hai`), a waiter proceeding after `PAKKA`,
rollback on disconnect that frees the lock, `status` counts, 60 short connections,
users over the wire, 8 threads x 25 concurrent inserts, different databases in different
sessions.

- [ ] **Step 1: Write the tests** — `cpp/tests/test_client_server.cpp`

```cpp
// cpp/tests/test_client_server.cpp
//
// Client <-> server over real loopback sockets. A port of the scenarios in
// tests/test_server.py and the "users over the wire" class of tests/test_phase_b.py,
// plus the concurrency checks the thread-per-connection design promises.
#include <catch2/catch_test_macros.hpp>
#include "meradb/client.h"
#include "meradb/errors.h"
#include "server_fixture.h"
#include <atomic>
#include <thread>
#include <vector>

using namespace meradb;
using namespace meradb_test;

namespace {

ConnectOptions to(const RunningServer& s) {
    ConnectOptions options;
    options.port = s.port();
    return options;
}

std::vector<std::vector<Value>> rowsOf(Backend& db, const std::string& text) { return db.execute(text)[0].rows; }

}  // namespace

TEST_CASE("client_server sessions have their own current database", "[client_server]") {
    RunningServer s;
    Connection a(to(s)), b(to(s));
    a.execute("BANAO DATABASE college; ISTEMAL college; BANAO TABLE t (x INT)");
    CHECK(a.currentDb() == "college");
    CHECK(b.currentDb() == "main");
    CHECK(rowsOf(b, "DIKHAO TABLES").empty());
    ConnectOptions options = to(s);
    options.database = "college";
    Connection c(options);
    auto tables = rowsOf(c, "DIKHAO TABLES");
    REQUIRE(tables.size() == 1);
    CHECK(std::get<std::string>(tables[0][0].data) == "t");
}

TEST_CASE("client_server writes are visible to other clients and constraints are shared", "[client_server]") {
    RunningServer s;
    Connection a(to(s)), b(to(s));
    a.execute("BANAO TABLE t (id INT MUKHYA KUNJI); DAALO MEIN t MAAN (1)");
    auto rows = rowsOf(b, "DIKHAO * SE t JAHAN id = 1");
    REQUIRE(rows.size() == 1);
    CHECK(std::get<int64_t>(rows[0][0].data) == 1);
    CHECK_THROWS_AS(b.execute("DAALO MEIN t MAAN (1)"), MeraDBError);  // uniqueness is shared too
}

TEST_CASE("client_server a transaction spans several requests on one session", "[client_server]") {
    RunningServer s;
    Connection a(to(s)), b(to(s));
    a.execute("BANAO TABLE t (x INT)");
    a.execute("SHURU");
    a.execute("DAALO MEIN t MAAN (1)");  // separate requests, same session, same server thread
    CHECK(a.inTransaction());
    a.execute("PAKKA");
    CHECK_FALSE(a.inTransaction());
    CHECK(rowsOf(b, "DIKHAO * SE t").size() == 1);
}

TEST_CASE("client_server transactions are isolated between clients", "[client_server]") {
    RunningServer s;
    Connection a(to(s)), b(to(s));
    a.execute("BANAO TABLE t (x INT)");
    a.execute("SHURU; DAALO MEIN t MAAN (1)");
    CHECK(a.inTransaction());
    CHECK(b.runScript("DIKHAO * SE t")[0].error.find("busy") != std::string::npos);  // b must wait for a
    a.execute("WAPAS");
    CHECK(rowsOf(b, "DIKHAO * SE t").empty());
}

TEST_CASE("client_server a waiting client proceeds when the transaction ends", "[client_server]") {
    RunningServer s;
    s.instance().lockTimeoutSeconds = 5.0;  // long enough to outwait the sleep below
    Connection a(to(s)), b(to(s));
    a.execute("BANAO TABLE t (x INT); SHURU; DAALO MEIN t MAAN (1)");
    std::atomic<bool> done{false};
    std::string error = "unset";
    std::size_t rowCount = 0;
    std::thread waiter([&] {
        auto r = b.runScript("DIKHAO * SE t");
        error = r[0].error;
        rowCount = r[0].rows.size();
        done = true;
    });
    sleepMs(250);
    CHECK_FALSE(done.load());  // still waiting for a's lock
    a.execute("PAKKA");
    waiter.join();
    CHECK(error.empty());
    CHECK(rowCount == 1);  // b sees the committed row
}

TEST_CASE("client_server a disconnect rolls the open transaction back and frees the lock", "[client_server]") {
    RunningServer s;
    {
        Connection a(to(s));
        a.execute("BANAO TABLE t (x INT); DAALO MEIN t MAAN (1); SHURU; MITAO SE t");
        a.close();
    }
    Connection b(to(s));
    auto started = std::chrono::steady_clock::now();
    auto rows = rowsOf(b, "DIKHAO * SE t");  // would report "busy" after 0.5 s if the lock were still held
    double waited = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(rows.size() == 1);
    CHECK(waited < 0.4);
    CHECK(s.logContains("disconnected"));
}

TEST_CASE("client_server status counts live sessions", "[client_server]") {
    RunningServer s;
    Connection a(to(s));
    {
        Connection b(to(s));
        CHECK(a.status()["sessions"] == 2);
    }
    CHECK(waitFor([&] { return a.status()["sessions"] == 1; }));
    CHECK(a.status()["databases"][0] == "main");
}

TEST_CASE("client_server many short connections leave no sessions behind", "[client_server]") {
    RunningServer s;
    for (int i = 0; i < 60; ++i) {
        Connection c(to(s));
        c.execute("DIKHAO TABLES");
    }
    CHECK(waitFor([&] { return s.server().sessions() == 0; }));
}

TEST_CASE("client_server users over the wire get exactly what they were granted", "[client_server]") {
    RunningServer s;
    {
        Connection admin(to(s));
        admin.execute("BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT)");
        admin.execute("DAALO MEIN students MAAN (1, 'Ravi')");
    }
    // The same as `BANAO USER ravi GUPT 'secret123'; ADHIKAR DO DIKHAO PAR students KO ravi`.
    s.instance().users().create("ravi", "secret123");
    s.instance().users().grant("ravi", "main", "students", {"DIKHAO"});

    ConnectOptions options = to(s);
    options.user = "ravi";
    options.password = "wrong";
    CHECK_THROWS_AS(Connection(options), ConnectionFailed);
    options.user = "ghost";
    options.password = "whatever";
    CHECK_THROWS_AS(Connection(options), ConnectionFailed);

    options.user = "ravi";
    options.password = "secret123";
    Connection ravi(options);
    auto rows = rowsOf(ravi, "DIKHAO * SE students");
    REQUIRE(rows.size() == 1);
    CHECK(std::get<std::string>(rows[0][1].data) == "Ravi");

    auto denied = ravi.runScript("DAALO MEIN students MAAN (2, 'Simran')");
    CHECK(denied[0].error.find("adhikar nahi hai") != std::string::npos);
    auto ddl = ravi.runScript("BANAO TABLE t2 (a INT)");
    CHECK(ddl[0].error.find("superuser nahi hai") != std::string::npos);

    Connection nobody(to(s));  // no user= at all: exactly the unrestricted behaviour
    nobody.execute("DAALO MEIN students MAAN (3, 'Anjali')");
    CHECK(rowsOf(nobody, "DIKHAO * SE students").size() == 2);
}

TEST_CASE("client_server concurrent clients do not lose or corrupt writes", "[client_server]") {
    RunningServer s;
    s.instance().lockTimeoutSeconds = 10.0;
    {
        Connection admin(to(s));
        admin.execute("BANAO TABLE t (id INT MUKHYA KUNJI, who INT)");
    }
    constexpr int kThreads = 8;
    constexpr int kPerThread = 25;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            try {
                Connection c(to(s));
                for (int i = 0; i < kPerThread; ++i) {
                    auto r = c.runScript("DAALO MEIN t MAAN (" + std::to_string(t * 1000 + i) + ", " + std::to_string(t) + ")");
                    if (!r[0].error.empty()) ++failures;
                }
            } catch (const std::exception&) {
                ++failures;
            }
        });
    }
    for (auto& th : threads) th.join();
    CHECK(failures.load() == 0);
    Connection check(to(s));
    auto count = rowsOf(check, "DIKHAO GINO(*) SE t");
    CHECK(std::get<int64_t>(count[0][0].data) == kThreads * kPerThread);
    // the primary key index survived the storm: every id is findable
    CHECK(rowsOf(check, "DIKHAO * SE t JAHAN id = 7024").size() == 1);
    CHECK_THROWS_AS(check.execute("DAALO MEIN t MAAN (7024, 7)"), MeraDBError);
}

TEST_CASE("client_server each session keeps its own transaction while others work", "[client_server]") {
    RunningServer s;
    s.instance().lockTimeoutSeconds = 5.0;
    Connection setup(to(s));
    setup.execute("BANAO DATABASE one; BANAO DATABASE two");
    // two sessions, each transacting in a DIFFERENT database, one after the other on the same lock
    ConnectOptions optionsOne = to(s);
    optionsOne.database = "one";
    Connection a(optionsOne);
    a.execute("BANAO TABLE t (x INT); SHURU; DAALO MEIN t MAAN (1); PAKKA");
    ConnectOptions optionsTwo = to(s);
    optionsTwo.database = "two";
    Connection b(optionsTwo);
    b.execute("BANAO TABLE t (x INT); SHURU; DAALO MEIN t MAAN (2); WAPAS");
    CHECK(rowsOf(a, "DIKHAO * SE t").size() == 1);
    CHECK(rowsOf(b, "DIKHAO * SE t").empty());
}
```

- [ ] **Step 2: Add to `cpp/tests/CMakeLists.txt`, build, run**

```bash
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R client_server --repeat until-fail:5
```
Expected: 11 test cases pass, five times in a row. Fixing a failure means
fixing `server.cpp` / `engine.cpp`. Typical causes: an engine created on the wrong thread
(the transaction rule throws), a session counter decremented before the thread's cleanup ran,
a reply written after the socket was closed.

- [ ] **Step 3: Commit**

```bash
git add cpp/tests/test_client_server.cpp cpp/tests/CMakeLists.txt
git commit -m "Add in-process client/server tests: isolation, transactions, disconnects"
```

**Completion checklist:**
- [ ] the rollback-on-disconnect test asserts both the data (row absent) and the lock (another session can write at once)
- [ ] the concurrent-insert test checks the final row count is exactly 8 x 25 and every id is present once
- [ ] stable over five repeated runs
- [ ] full suite green, zero warnings

---

### Task 19: Hardening — depth caps and protocol abuse

**Files:**
- Modify: `cpp/include/meradb/parser.h`, `cpp/src/parser.cpp`, `cpp/include/meradb/engine.h`, `cpp/src/engine.cpp`
- Test: `cpp/tests/test_hardening.cpp` (engine-level), `cpp/tests/test_server_hardening.cpp` (over the wire); `cpp/tests/stack_probe.cpp` (not part of the build; a re-verification tool)
- Modify: `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Produces: a per-statement parse budget, a statement-nesting cap and a view-depth cap —
  the deliberate divergences of D5 (Python dies with `RecursionError`; a C++ server would
  overflow a thread stack and take every client down with it).
  - Every operator and every nesting level of ONE statement spends one unit of a budget of
    **400** (`kMaxNesting`): `YA`, `AUR`, `NAHI`, binary `+ - * / %`, unary `-`, `AGAR`,
    function-call `(`, grouping `(`, the `MEIN (` list and each of its items (the list is
    rewritten into an `OR` chain), each `MEIN (subquery)`, each set-operation step.
    Error: `[Parser Galti] Query bahut gehri (nested) hai (limit 400)`.
  - A statement inside another statement (`SAMJHAO`, trigger and procedure bodies) starts
    with a fresh budget; statements may nest **32** deep
    (`Statements bahut gehre nested hain (limit 32)`).
  - Views over views: **32** levels (`View 'X' bahut gehri nested hai (limit 32 views ek ke andar ek)`).
    Creating a view runs its query once, so a 33rd level is refused at creation.
- Why 400 (measured, not guessed): `cpp/tests/stack_probe.cpp` runs 390-deep parses and
  evaluations of every construct above on a thread with a **256 KB** stack and passes;
  the smallest default thread stack this project meets is 1 MB (MinGW/Windows).

- [ ] **Step 1: Write the failing tests**

`cpp/tests/test_hardening.cpp` (the engine work runs on `std::async` threads and the
assertions run on the main thread — Catch2 assertions must not run on other threads):

```cpp
// cpp/tests/test_hardening.cpp -- hostile input must produce an error, never a crash.
//
// A network client can send any query it likes. Python answers absurdly deep
// input with a RecursionError; a C++ server would overflow its thread stack
// and die. So the parser spends one unit of a per-statement budget on every
// operator and nesting level (limit 400), statements nest at most 32 deep and
// views over views at most 32 deep. The work runs on a std::thread, because
// that is where the server runs statements (a smaller stack than main's);
// Catch2 assertions stay on the test's own thread.
#include <catch2/catch_test_macros.hpp>
#include "meradb/engine.h"
#include "meradb/errors.h"
#include "meradb/parser.h"
#include "test_util.h"
#include <future>
#include <string>
#include <vector>

using namespace meradb;

namespace {

template <typename F>
auto onThread(F body) {
    return std::async(std::launch::async, body).get();
}

std::string parens(int depth, const std::string& core) {
    return std::string(static_cast<size_t>(depth), '(') + core + std::string(static_cast<size_t>(depth), ')');
}

std::string repeated(const std::string& unit, int times, const std::string& separator) {
    std::string out;
    for (int i = 0; i < times; ++i) out += (i ? separator : std::string()) + unit;
    return out;
}

// The parse error's message without its ", par ... mila (line, col)" tail; "" when it parses.
std::string parseErrorOf(const std::string& sql) {
    try {
        parseScript(sql);
    } catch (const ParseError& e) {
        return e.message().substr(0, e.message().find(", par "));
    }
    return "";
}

const std::string kTooDeep = "Query bahut gehri (nested) hai (limit 400)";

}  // namespace

TEST_CASE("hardening the parser accepts nesting up to the budget and refuses more", "[hardening]") {
    auto r = onThread([] {
        return std::vector<std::string>{parseErrorOf("DIKHAO * SE t JAHAN " + parens(300, "1 = 1")),
                                        parseErrorOf("DIKHAO * SE t JAHAN " + parens(401, "1 = 1")),
                                        parseErrorOf("DIKHAO * SE t JAHAN " + parens(100000, "1 = 1"))};
    });
    CHECK(r[0].empty());
    CHECK(r[1] == kTooDeep);
    CHECK(r[2] == kTooDeep);
}

TEST_CASE("hardening long operator chains are bounded too", "[hardening]") {
    auto r = onThread([] {
        return std::vector<std::string>{
            parseErrorOf("DIKHAO * SE t JAHAN x = " + repeated("1", 350, " + ")),
            parseErrorOf("DIKHAO * SE t JAHAN x = " + repeated("1", 500, " + ")),
            parseErrorOf("DIKHAO * SE t JAHAN x = " + repeated("1", 100000, " + ")),
            parseErrorOf("DIKHAO * SE t JAHAN " + repeated("1 = 1", 500, " AUR ")),
            parseErrorOf("DIKHAO * SE t JAHAN " + repeated("1 = 1", 500, " YA ")),
            parseErrorOf("DIKHAO * SE t JAHAN x = " + repeated("2", 500, " * ")),
            parseErrorOf("DIKHAO * SE t JAHAN x = " + repeated("-", 500, " ") + " 1"),
            parseErrorOf("DIKHAO * SE t JAHAN " + repeated("NAHI", 500, " ") + " x = 1"),
            parseErrorOf("DIKHAO * SE t JAHAN x = " + repeated("AGAR 1 = 1 TAB 1 WARNA", 500, " ") + " 0 " +
                         repeated("KHATAM", 500, " ")),
            parseErrorOf("DIKHAO * SE t JAHAN x MEIN (" + repeated("1", 300, ", ") + ")"),
            parseErrorOf("DIKHAO * SE t JAHAN x MEIN (" + repeated("1", 5000, ", ") + ")")};
    });
    CHECK(r[0].empty());
    for (size_t i = 1; i <= 8; ++i) CHECK(r[i] == kTooDeep);
    CHECK(r[9].empty());       // an IN list becomes an OR chain, one level per item ...
    CHECK(r[10] == kTooDeep);  // ... so it is bounded like any other chain (Python fails near 450)
}

TEST_CASE("hardening set-operation chains and nested subqueries are bounded", "[hardening]") {
    auto r = onThread([] {
        std::string nested = "DIKHAO * SE t JAHAN x MEIN (DIKHAO x SE t)";
        for (int i = 0; i < 500; ++i) nested = "DIKHAO * SE t JAHAN x MEIN (" + nested + ")";
        return std::vector<std::string>{parseErrorOf("DIKHAO * SE t " + repeated("SANYUKT DIKHAO * SE t", 500, " ")),
                                        parseErrorOf("DIKHAO * SE t " + repeated("SANYUKT DIKHAO * SE t", 50, " ")),
                                        parseErrorOf(nested)};
    });
    CHECK(r[0] == kTooDeep);
    CHECK(r[1].empty());
    CHECK(r[2] == kTooDeep);
}

TEST_CASE("hardening each statement has its own budget", "[hardening]") {
    auto r = onThread([] {
        const std::string one = "DIKHAO * SE t JAHAN " + repeated("1 = 1", 200, " AUR ") + ";";
        return parseErrorOf(repeated(one, 100, "\n"));  // 100 statements x 199 operators
    });
    CHECK(r.empty());
}

TEST_CASE("hardening SAMJHAO cannot be stacked without limit", "[hardening]") {
    auto r = onThread([] {
        return std::vector<std::string>{parseErrorOf(repeated("SAMJHAO", 20, " ") + " DIKHAO * SE t"),
                                        parseErrorOf(repeated("SAMJHAO", 100, " ") + " DIKHAO * SE t")};
    });
    CHECK(r[0].empty());
    CHECK(r[1] == "Statements bahut gehre nested hain (limit 32)");
}

TEST_CASE("hardening a million open parentheses fail fast", "[hardening]") {
    auto r = onThread([] { return parseErrorOf("DIKHAO * SE t JAHAN " + std::string(1000000, '(')); });
    CHECK(r == kTooDeep);
}

TEST_CASE("hardening deep-but-legal expressions evaluate without exhausting the stack", "[hardening]") {
    meradb_test::TempDir dir;
    auto r = onThread([&] {
        Engine e(dir.str());
        e.execute("BANAO TABLE t (x INT); DAALO MEIN t MAAN (1), (2)");
        std::vector<Result> out;
        out.push_back(e.runScript("DIKHAO * SE t JAHAN " + parens(390, "x = 1"))[0]);
        out.push_back(e.runScript("DIKHAO * SE t JAHAN x = " + repeated("1", 390, " * "))[0]);
        out.push_back(e.runScript("DIKHAO * SE t JAHAN " + repeated("x > 0", 390, " AUR "))[0]);
        out.push_back(e.runScript("SAMJHAO DIKHAO * SE t JAHAN " + parens(390, "x = 1"))[0]);
        out.push_back(e.runScript("DIKHAO * SE t JAHAN x MEIN (DIKHAO x SE t JAHAN " + parens(390, "x = 2") + ")")[0]);
        out.push_back(e.runScript("DIKHAO * SE t JAHAN " + parens(500, "x = 1"))[0]);
        return out;
    });
    for (size_t i = 0; i < 5; ++i) CHECK(r[i].error.empty());
    CHECK(r[0].rows.size() == 1);
    CHECK(r[1].rows.size() == 1);
    CHECK(r[2].rows.size() == 2);
    CHECK(r[4].rows.size() == 1);
    CHECK(r[5].error.rfind("[Parser Galti] " + kTooDeep, 0) == 0);
}

TEST_CASE("hardening views over views are capped", "[hardening]") {
    meradb_test::TempDir dir;
    auto r = onThread([&] {
        Engine e(dir.str());
        e.execute("BANAO TABLE t (x INT); DAALO MEIN t MAAN (7); BANAO VIEW v1 KAHO DIKHAO * SE t");
        for (int i = 2; i <= 33; ++i)  // creating a view runs its query once to check it
            e.execute("BANAO VIEW v" + std::to_string(i) + " KAHO DIKHAO * SE v" + std::to_string(i - 1));
        std::vector<Result> out;
        out.push_back(e.runScript("DIKHAO * SE v32")[0]);                     // 32 levels of views: allowed
        out.push_back(e.runScript("DIKHAO * SE v33")[0]);                     // 33 levels: refused
        out.push_back(e.runScript("BANAO VIEW v34 KAHO DIKHAO * SE v33")[0]);  // so v34 cannot even be created
        out.push_back(e.runScript("DIKHAO * SE v1")[0]);                      // the depth counter unwound
        return out;
    });
    CHECK(r[0].error.empty());
    CHECK(r[0].rows.size() == 1);
    const std::string refusal = "[Execution Galti] View 'v1' bahut gehri nested hai (limit 32 views ek ke andar ek)";
    CHECK(r[1].error == refusal);
    CHECK(r[2].error == refusal);
    CHECK(r[3].error.empty());
}
```

`cpp/tests/test_server_hardening.cpp`:

```cpp
// cpp/tests/test_server_hardening.cpp -- a hostile or careless client must not hurt the server or other clients.
#include <catch2/catch_test_macros.hpp>
#include "meradb/client.h"
#include "meradb/engine.h"
#include "server_fixture.h"
#include <filesystem>

using namespace meradb;
using namespace meradb_test;
using protocol::Json;

namespace {

ConnectOptions to(const RunningServer& s) {
    ConnectOptions options;
    options.port = s.port();
    return options;
}

bool serverStillWorks(RunningServer& s) {
    Connection db(to(s));
    return db.runScript("DIKHAO TABLES")[0].error.empty();
}

std::string errorOf(const Json& reply) { return reply.value("error", std::string("(no error)")); }

}  // namespace

TEST_CASE("hardening_server an oversize message is refused and the session survives", "[hardening][server]") {
    RunningServer s("", false, /*maxMessageBytes=*/256);
    RawClient c(s.port());
    c.hello();
    c.sendRaw(std::string(5000, 'x') + "\n");
    CHECK(errorOf(c.receive().value()) == "[Protocol Galti] Message bahut bada hai");
    CHECK(c.request(Json{{"type", "ping"}})["ok"] == true);  // the rest of the long line was skipped, not parsed
    Json big = Json::object();
    big["type"] = "query";
    big["text"] = std::string(400, ' ') + "DIKHAO TABLES";
    c.send(big);
    CHECK(errorOf(c.receive().value()) == "[Protocol Galti] Message bahut bada hai");
    CHECK(c.request(Json{{"type", "ping"}})["ok"] == true);
}

TEST_CASE("hardening_server deeply nested JSON is a protocol error not a crash", "[hardening][server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    c.sendRaw(std::string(200000, '[') + std::string(200000, ']') + "\n");
    std::string error = errorOf(c.receive().value());
    CHECK(error.rfind("[Protocol Galti] Galat message: ", 0) == 0);
    c.sendRaw("{\"type\": \"query\", \"text\": " + std::string(100000, '[') + "}\n");
    CHECK(errorOf(c.receive().value()).rfind("[Protocol Galti] ", 0) == 0);
    CHECK(c.request(Json{{"type", "ping"}})["ok"] == true);
}

TEST_CASE("hardening_server invalid UTF-8 and binary junk are protocol errors", "[hardening][server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    c.sendRaw("{\"type\": \"query\", \"text\": \"\xC3(\"}\n");
    CHECK(errorOf(c.receive().value()) == "[Protocol Galti] Galat message: invalid UTF-8");
    std::string junk;
    for (int i = 0; i < 300; ++i) junk += static_cast<char>((i * 37) % 251 + 1 == '\n' ? 'x' : (i * 37) % 251 + 1);
    c.sendRaw(junk + "\n");
    CHECK(errorOf(c.receive().value()).rfind("[Protocol Galti] ", 0) == 0);
    CHECK(c.request(Json{{"type", "ping"}})["ok"] == true);
}

TEST_CASE("hardening_server a client that vanishes mid-message does no harm", "[hardening][server]") {
    RunningServer s;
    {
        RawClient c(s.port());
        c.hello();
        c.sendRaw("{\"type\": \"query\", \"text\": \"DIKHAO ");  // half a message, then gone
    }
    {
        RawClient c(s.port());  // never completes the handshake either
        c.sendRaw("{\"type\": \"hel");
    }
    CHECK(waitFor([&] { return s.server().sessions() == 0; }));
    CHECK(serverStillWorks(s));
}

TEST_CASE("hardening_server odd hello fields never crash the handshake", "[hardening][server]") {
    RunningServer s;
    s.instance().users().create("5", "pw");
    const std::vector<std::string> hellos = {
        "{\"type\": \"hello\"}",
        "{\"type\": \"hello\", \"user\": 5, \"password\": \"pw\"}",  // str(5) == "5": a valid login
        "{\"type\": \"hello\", \"user\": 0, \"password\": 12}",       // falsy user: not a login at all
        "{\"type\": \"hello\", \"user\": [\"a\"], \"password\": {\"x\": 1}}",
        "{\"type\": \"hello\", \"database\": 7}",
        "{\"type\": \"hello\", \"database\": \"\", \"password\": null, \"user\": null}",
        "{\"type\": [\"hello\"]}",
        "{\"type\": \"HELLO\"}",
        "{}",
    };
    for (const auto& text : hellos) {
        RawClient c(s.port());
        c.sendRaw(text + "\n");
        try {
            c.receive();  // an answer or a plain close: both are fine
        } catch (const net::NetError&) {
        }
    }
    CHECK(serverStillWorks(s));
    RawClient login(s.port());
    CHECK(login.hello("pw", nullptr, 5)["ok"] == true);
}

TEST_CASE("hardening_server hostile queries get an error result and the session goes on", "[hardening][server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    std::string deep = "DIKHAO * SE t JAHAN " + std::string(1000000, '(');
    Json reply = c.query(deep);
    CHECK(reply["ok"] == true);
    CHECK(reply["results"][0]["error"].get<std::string>().rfind("[Parser Galti] Query bahut gehri (nested) hai", 0) == 0);
    std::string chain = "DIKHAO * SE t JAHAN x = 1";
    for (int i = 0; i < 100000; ++i) chain += " + 1";
    reply = c.query(chain);
    CHECK(reply["results"][0]["error"].get<std::string>().rfind("[Parser Galti] Query bahut gehri (nested) hai", 0) == 0);
    CHECK(c.query("DIKHAO TABLES")["results"][0]["error"] == "");
}

TEST_CASE("hardening_server a large script goes through in one message", "[hardening][server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    c.query("BANAO TABLE t (id INT MUKHYA KUNJI, s TEXT)");
    std::string insert = "DAALO MEIN t MAAN ";
    for (int i = 0; i < 3000; ++i) insert += (i ? ", (" : "(") + std::to_string(i) + ", 'row number " + std::to_string(i) + "')";
    Json reply = c.query(insert);
    CHECK(reply["results"][0]["error"] == "");
    CHECK(reply["results"][0]["message"] == "3000 row(s) daal di");
    std::string many;
    for (int i = 0; i < 400; ++i) many += "DIKHAO * SE t JAHAN id = " + std::to_string(i) + ";\n";
    reply = c.query(many);
    CHECK(reply["results"].size() == 400);
}

TEST_CASE("hardening_server pipelined requests are answered in order", "[hardening][server]") {
    RunningServer s;
    RawClient c(s.port());
    c.hello();
    std::string burst;
    for (int i = 0; i < 100; ++i) {
        Json q = Json::object();
        q["type"] = "query";
        q["text"] = "DIKHAO TABLES; DIKHAO * SE gayab_" + std::to_string(i);  // the second statement names its request
        burst += pyjson::dump(q) + "\n";
    }
    c.sendRaw(burst);
    for (int i = 0; i < 100; ++i) {
        Json reply = c.receive().value();
        REQUIRE(reply["results"].size() == 2);
        CHECK(reply["results"][1]["error"] ==
              "[Execution Galti] Table 'gayab_" + std::to_string(i) + "' exist nahi karta");
    }
    CHECK(c.request(Json{{"type", "ping"}})["ok"] == true);
}

TEST_CASE("hardening_server shutdown with a transaction open rolls it back cleanly", "[hardening][server]") {
    TempDir dir;  // outlives the server so the folder can be reopened afterwards
    std::string dataDir;
    {
        // a server on a folder we own: RunningServer makes its own temp dir, so copy the idea by hand
        ServerOptions options;
        options.dataDir = dir.str();
        options.port = 0;
        options.log = [](const std::string&) {};
        Server server(options);
        server.instance().lockTimeoutSeconds = 0.5;
        std::thread runner([&] { server.serveForever(); });
        ConnectOptions connect;
        connect.port = server.port();
        Connection a(connect);
        a.execute("BANAO TABLE t (x INT); DAALO MEIN t MAAN (1); SHURU; DAALO MEIN t MAAN (2); MITAO SE t");
        REQUIRE(a.inTransaction());
        server.requestStop();  // the client is still connected, mid-transaction
        runner.join();         // returns only after the session thread rolled its transaction back
        dataDir = dir.str();
    }
    Engine reopened(dataDir);
    CHECK(reopened.instance().recovered().empty());  // nothing left for crash recovery to undo
    CHECK_FALSE(std::filesystem::exists(dir.path() / ".wapas" / "main"));
    auto rows = reopened.execute("DIKHAO * SE t")[0].rows;
    REQUIRE(rows.size() == 1);
    CHECK(std::get<int64_t>(rows[0][0].data) == 1);  // the transaction's insert and delete were both undone
}

TEST_CASE("hardening_server one busy transaction does not stop the server from stopping", "[hardening][server]") {
    auto s = std::make_unique<RunningServer>();
    Connection holder(to(*s));
    holder.execute("BANAO TABLE t (x INT); SHURU");
    std::thread blocked([&] {
        Connection other(to(*s));
        other.runScript("DIKHAO * SE t");  // waits for the lock (0.5 s), then reports busy
    });
    sleepMs(100);
    auto started = std::chrono::steady_clock::now();
    s->stop();
    blocked.join();
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(took < 4.0);
}

TEST_CASE("hardening_server a hundred idle connections stop promptly", "[hardening][server]") {
    auto s = std::make_unique<RunningServer>();
    std::vector<std::unique_ptr<RawClient>> clients;
    for (int i = 0; i < 100; ++i) {
        clients.push_back(std::make_unique<RawClient>(s->port()));
        clients.back()->hello();
    }
    CHECK(waitFor([&] { return s->server().sessions() == 100; }, 5.0));
    auto started = std::chrono::steady_clock::now();
    s->stop();
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    CHECK(took < 4.0);
    CHECK(s->server().sessions() == 0);
}
```

- [ ] **Step 2: Add both to `cpp/tests/CMakeLists.txt`; verify failure**

```bash
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "hardening"
```
Expected: FAIL — the deep queries crash or hang instead of returning the error (a crash of
the test process counts as failing).

- [ ] **Step 3: `parser.h`** — in `class Parser`

Replace `std::unique_ptr<ast::Statement> parseStatement();` with

```cpp
    std::unique_ptr<ast::Statement> parseStatement();      // guarded: see spend() / the Scope in parser.cpp
    std::unique_ptr<ast::Statement> parseStatementBody();  // the real statement dispatcher
```
and directly after `size_t pos_ = 0;` add

```cpp

    // Hostile-input limits (a deliberate divergence from Python, which has none and
    // dies with RecursionError): every operator and every nesting level of ONE
    // statement spends one unit of a shared budget; a statement inside a
    // TRIGGER/PROCEDURE body gets a fresh budget, and bodies may nest 32 deep.
    static constexpr int kMaxNesting = 400;
    static constexpr int kMaxStatementNesting = 32;
    int nestingUsed_ = 0;
    int statementDepth_ = 0;
    void spend();
```

- [ ] **Step 4: `parser.cpp`** — twelve find/replace edits (each old text occurs once; if the
compiler says a `spend()` is missing, the tests in `test_hardening.cpp` name the construct)

4a. Directly before `[[noreturn]] void Parser::error(const std::string& msg) const {` add:
```cpp
void Parser::spend() {
    if (++nestingUsed_ > kMaxNesting)
        error("Query bahut gehri (nested) hai (limit " + std::to_string(kMaxNesting) + ")");
}
```
4b. `YA`: replace
`    while (matchKeyword("YA")) left = std::make_unique<BinaryOp>("YA", std::move(left), parseAnd());` with
```cpp
    while (matchKeyword("YA")) {
        spend();
        left = std::make_unique<BinaryOp>("YA", std::move(left), parseAnd());
    }
```
4c. `AUR`: the same shape with `parseNot()`:
```cpp
    while (matchKeyword("AUR")) {
        spend();
        left = std::make_unique<BinaryOp>("AUR", std::move(left), parseNot());
    }
```
4d. `NAHI`: replace `    if (matchKeyword("NAHI")) {\n        auto u = std::make_unique<UnaryOp>();` with the same text plus `        spend();` as the first line inside the block.
4e. Additive loop: replace
`        std::string op = advance().textValue;\n        left = std::make_unique<BinaryOp>(op, std::move(left), parseTerm());` with the same text with `        spend();` inserted after the `advance()` line. Multiplicative loop: same, for `parseUnary()`.
4f. Unary minus: replace `    if (checkSymbol("-")) {\n        advance();\n        auto operand = parseUnary();` inserting `        spend();` after `advance();`.
4g. `AGAR`: replace `    if (matchKeyword("AGAR")) return parseCase();` with
```cpp
    if (matchKeyword("AGAR")) {
        spend();
        return parseCase();
    }
```
4h. Function call: replace `        if (matchSymbol("(")) {\n            // function call:` inserting `            spend();` as the first line in the block.
4i. Grouping paren: replace `    if (matchSymbol("(")) {\n        if (checkKeyword("DIKHAO")) {` inserting `        spend();` after the `if (matchSymbol("(")) {` line.
4j. Set operations: replace `        op->op = advance().textValue;\n        expectKeyword("DIKHAO");` inserting `        spend();` after the `advance()` line.
4k. `MEIN (`: replace
`        expectSymbol("(");\n        if (checkKeyword("DIKHAO")) {\n            auto sub = std::make_unique<Subquery>();\n            expectKeyword("DIKHAO");\n            sub->statement = parseSelectBody();\n            expectSymbol(")");\n            auto inSub`
with the same text with `        spend();` inserted after `expectSymbol("(");`; and in the list branch replace
`        while (matchSymbol(",")) {\n            auto leftCopy2 = cloneExpr(*left);` with the same plus
`            spend();  // the list becomes an OR chain: one level per item` as the first line of the loop.
4l. The statement scope: replace
```cpp
std::unique_ptr<Statement> Parser::parseStatement() {
    const Token& tok = peek();
```
with
```cpp
// Every (possibly nested) statement gets its own operator budget, and statements
// nested inside statements (SAMJHAO ..., trigger/procedure bodies) are capped.
std::unique_ptr<Statement> Parser::parseStatement() {
    struct Scope {
        Parser& parser;
        int savedBudget;
        explicit Scope(Parser& p) : parser(p), savedBudget(p.nestingUsed_) {
            if (p.statementDepth_ >= kMaxStatementNesting)
                p.error("Statements bahut gehre nested hain (limit " + std::to_string(kMaxStatementNesting) + ")");
            ++p.statementDepth_;
            p.nestingUsed_ = 0;
        }
        ~Scope() {
            --parser.statementDepth_;
            parser.nestingUsed_ = savedBudget;
        }
    } scope(*this);
    return parseStatementBody();
}

std::unique_ptr<Statement> Parser::parseStatementBody() {
    const Token& tok = peek();
```
(If Batch A's Task 4 changed the first line of `parseStatement`, apply the same rename to
whatever the dispatcher's first lines are: the function keeps its body, only its name
becomes `parseStatementBody`.)

- [ ] **Step 5: Views** — `engine.h`, directly after `std::unique_ptr<Table> resolveSource(const std::string& name);` add

```cpp
    static constexpr int kMaxViewDepth = 32;  // views defined over views (Python: none, RecursionError)
    int viewDepth_ = 0;
```
`engine.cpp`, in `Engine::resolveSource`, replace
`    if (cat.views.count(name)) {\n        auto parsed = parseScript(cat.views.at(name));` with
```cpp
    if (cat.views.count(name)) {
        struct ViewDepth {
            Engine& engine;
            ViewDepth(Engine& e, const std::string& view) : engine(e) {
                if (e.viewDepth_ >= kMaxViewDepth)
                    throw ExecutionError("View '" + view + "' bahut gehri nested hai (limit " +
                                         std::to_string(kMaxViewDepth) + " views ek ke andar ek)");
                ++e.viewDepth_;
            }
            ~ViewDepth() { --engine.viewDepth_; }
        } viewDepth(*this, name);
        auto parsed = parseScript(cat.views.at(name));
```

- [ ] **Step 6: Build and run**

```bash
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "hardening"
ctest --test-dir cpp/build --output-on-failure
```
Expected: 8 `hardening` + 11 `hardening_server` test cases pass; the whole suite still green
(the Batch A golden scripts must not have used more than 400 units in one statement).

- [ ] **Step 7: Re-verify the 400 figure on a small stack (once, by hand)**

`cpp/tests/stack_probe.cpp` is deliberately not in CMake. Build and run it once:

```bash
g++ -std=c++17 -Icpp/include -Icpp/tests -Icpp/build/_deps/json-src/include cpp/tests/stack_probe.cpp $(ls cpp/src/*.cpp | grep -v main.cpp) -lws2_32 -lbcrypt -o cpp/build/stack_probe
./cpp/build/stack_probe 256
```
Expected last line: `done bad=0`. (Linux/macOS: drop `-lws2_32 -lbcrypt`, add `-pthread`.)
If a construct needed by a future phase raises the per-level stack cost, lower `kMaxNesting`
rather than growing thread stacks.

`cpp/tests/stack_probe.cpp`:

```cpp
#include "meradb/engine.h"
#include "test_util.h"
#include <pthread.h>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace meradb;

static std::string parens(int depth, const std::string& core) {
    return std::string(depth, '(') + core + std::string(depth, ')');
}
static std::string repeated(const std::string& unit, int times, const std::string& sep) {
    std::string out;
    for (int i = 0; i < times; ++i) out += (i ? sep : std::string()) + unit;
    return out;
}

static void* work(void*) {
    meradb_test::TempDir dir;
    Engine e(dir.str());
    e.execute("BANAO TABLE t (x INT); DAALO MEIN t MAAN (1), (2)");
    int bad = 0;
    auto run = [&](const std::string& q) { auto r = e.runScript(q)[0]; if (!r.error.empty()) { ++bad; std::printf("ERR %s\n", r.error.substr(0, 100).c_str()); } };
    run("DIKHAO * SE t JAHAN " + parens(390, "x = 1"));
    run("DIKHAO * SE t JAHAN x = " + repeated("1", 390, " * "));
    run("DIKHAO * SE t JAHAN " + repeated("x > 0", 390, " AUR "));
    run("SAMJHAO DIKHAO * SE t JAHAN " + parens(390, "x = 1"));
    run("DIKHAO * SE t JAHAN x MEIN (DIKHAO x SE t JAHAN " + parens(390, "x = 2") + ")");
    run("DIKHAO * SE t JAHAN x MEIN (" + repeated("1", 390, ", ") + ")");
    run("DIKHAO * SE t JAHAN AGAR x = 1 TAB 1 WARNA 0 KHATAM = 1");
    std::printf("done bad=%d\n", bad);
    return nullptr;
}

int main(int argc, char** argv) {
    size_t kb = argc > 1 ? std::atoi(argv[1]) : 1024;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kb * 1024);
    pthread_t t;
    pthread_create(&t, &attr, work, nullptr);
    pthread_join(t, nullptr);
    return 0;
}
```

- [ ] **Step 8: Commit**

```bash
git add cpp/include/meradb/parser.h cpp/src/parser.cpp cpp/include/meradb/engine.h cpp/src/engine.cpp cpp/tests/test_hardening.cpp cpp/tests/test_server_hardening.cpp cpp/tests/stack_probe.cpp cpp/tests/CMakeLists.txt
git commit -m "Cap parse depth, statement nesting and view depth; harden the server against abuse"
```

**Completion checklist:**
- [ ] `docs/CPP.md` (Task 25) lists all three caps as deliberate divergences
- [ ] 100,000-term `+ 1` chains, 1,000,000 `(` and 200,000-deep JSON are all error replies, never crashes
- [ ] shutdown with an open transaction leaves no `.wapas/main` and an empty recovery list
- [ ] 100 idle connections stop in well under 4 s
- [ ] full suite green, zero warnings; `git diff <base> -- meradb` still empty

---



# BATCH C — process control and the command line (Tasks 20-21)

### Task 20: Process control — spawn, kill, `start` / `stop` / `status` logic

**Files:**
- Modify: `cpp/include/meradb/sys_compat.h`, `cpp/src/sys_compat.cpp`
- Create: `cpp/include/meradb/server_control.h`, `cpp/src/server_control.cpp`
- Test: `cpp/tests/test_server_control.cpp`
- Modify: `cpp/CMakeLists.txt` (add `src/server_control.cpp`), `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `Connection` (Task 17), `protocol::runningServer` / pid-file helpers (Task 14), `net::portOpen` (Task 13).
- Produces, in `meradb::sys`:
  ```cpp
  void setEnv(const std::string& name, const std::string& value);
  std::string readHidden(const std::string& prompt);     // password prompt without echo (stderr prompt)
  std::string executablePath();                          // UTF-8 path of the running program
  class DetachedProcess { std::int64_t pid() const; bool exited(); /* never blocks */ };
  std::unique_ptr<DetachedProcess> spawnDetached(exePath, args, logPath);  // stdin = null device, stdout+stderr appended to logPath
  std::string killProcess(std::int64_t pid);             // "" on success, else the OS error text
  ```
  and in `meradb`: `ControlOptions`, `int serverStart(const ControlOptions&)`,
  `serverStop`, `serverStatus` — each prints exactly what `cmd_start` / `cmd_stop` /
  `cmd_status` of `meradb/cli.py` print (results on stdout, notes on stderr) and returns
  the same exit code (`status` returns **3** when nothing runs).
- Behaviour copied from Python: `start` creates the data folder, reports an already-running
  server (exit 0), refuses a busy port with the "Doosra port do" hint (exit 1), starts
  `<this program> server --data D --host H --port P [--verbose]` detached with its output
  appended to `<data>/server.log`, passes `--password` through the `MERADB_PASSWORD`
  environment variable (so it stays out of the process list), and waits up to 15 s for a
  server that answers, reporting the log tail if the child dies first; `stop` removes a stale
  pid file, asks politely (`shutdown` message) and only with `--force` kills, then waits up
  to 10 s for the port to close; `status` prints address / pid / data / started and then the
  version, sessions and databases if it can log in.
- Two platform traps this task exists to avoid (both were hit while prototyping):
  1. **Windows handle inheritance.** `CreateProcess(bInheritHandles=TRUE)` alone makes the
     detached server inherit *every* inheritable handle of the launcher, including the write
     end of a pipe the caller is reading until EOF — so `subprocess.run(meradb start)` never
     returned. The code passes an explicit `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` containing only
     the log and NUL handles.
  2. **POSIX fork safety.** Everything that allocates (argument vector, log fd) is prepared
     *before* `fork()`; the child only calls `setsid`, `dup2`, `execv`, `_exit`.

- [ ] **Step 1: Write the failing tests** — `cpp/tests/test_server_control.cpp`

The `--force` path of `serverStop` is *not* unit-tested (an in-process server's pid file
holds the test program's own pid, so a kill would end the test run); `cli_lifecycle.py` in
Task 21 covers it with real server processes. The kill test spawns this very test program
with the name of a test that only sleeps when `MERADB_TEST_SLEEPER` is set, so an ordinary
run (and ctest's discovery) passes straight through it.

```cpp
// cpp/tests/test_server_control.cpp -- process helpers and start/stop/status.
//
// serverStop's --force path is not tested here: it would kill THIS test process
// (the pid file of an in-process server holds our own pid). The end-to-end
// ctest cli_lifecycle.py covers start / status / stop / --force with real
// server processes.
#include <catch2/catch_test_macros.hpp>
#include "meradb/errors.h"
#include "meradb/server_control.h"
#include "meradb/sys_compat.h"
#include "server_fixture.h"
#include <filesystem>
#include <iostream>
#include <sstream>

using namespace meradb;
using namespace meradb_test;

namespace {

// Captures stdout and stderr while alive.
class Capture {
public:
    Capture() : oldOut_(std::cout.rdbuf(out_.rdbuf())), oldErr_(std::cerr.rdbuf(err_.rdbuf())) {}
    ~Capture() {
        std::cout.rdbuf(oldOut_);
        std::cerr.rdbuf(oldErr_);
    }
    std::string out() const { return out_.str(); }
    std::string err() const { return err_.str(); }

private:
    std::ostringstream out_, err_;
    std::streambuf* oldOut_;
    std::streambuf* oldErr_;
};

// Make an in-process server look like a `meradb start`ed one: a pid file for its folder.
void writeFakePidFile(RunningServer& s) {
    protocol::Json info = protocol::Json::object();
    info["pid"] = 999999;
    info["host"] = "127.0.0.1";
    info["port"] = s.port();
    info["started"] = "2026-01-01 00:00:00";
    protocol::writePidFile(s.dataDir(), info);
}

}  // namespace

TEST_CASE("sys_process setEnv and getEnv round trip", "[sys]") {
    sys::setEnv("MERADB_TEST_ENV_VALUE", "hello world");
    CHECK(sys::getEnv("MERADB_TEST_ENV_VALUE").value() == "hello world");
    sys::setEnv("MERADB_TEST_ENV_VALUE", "");
    auto value = sys::getEnv("MERADB_TEST_ENV_VALUE");
    CHECK((!value || value->empty()));
}

TEST_CASE("sys_process executablePath names this running program", "[sys]") {
    std::string path = sys::executablePath();
    REQUIRE_FALSE(path.empty());
    CHECK(std::filesystem::exists(std::filesystem::u8path(path)));
}

TEST_CASE("sys_process spawnDetached runs a program and reports its exit", "[sys]") {
    TempDir dir;
    auto child = sys::spawnDetached(sys::executablePath(), {"--list-tests"}, dir.file("child.log"));
    REQUIRE(child);
    CHECK(child->pid() > 0);
    CHECK(waitFor([&] { return child->exited(); }, 10.0));
    CHECK(readText(dir.file("child.log")).find("sys_process spawnDetached") != std::string::npos);  // its stdout went to the log
}

TEST_CASE("sys_process spawnDetached passes awkward arguments through intact", "[sys]") {
    TempDir dir;
    // Catch2 lists the tests matching this filter: the argument must arrive as ONE token.
    auto child = sys::spawnDetached(sys::executablePath(), {"--list-tests", "sys_process setEnv and getEnv round trip"},
                                    dir.file("child.log"));
    REQUIRE(waitFor([&] { return child->exited(); }, 10.0));
    std::string log = readText(dir.file("child.log"));
    CHECK(log.find("sys_process setEnv and getEnv round trip") != std::string::npos);
    CHECK(log.find("sys_process executablePath") == std::string::npos);
}

TEST_CASE("sys_process spawnDetached of a missing program fails clearly or exits at once", "[sys]") {
    TempDir dir;
    bool refused = false;
    try {
        auto child = sys::spawnDetached(dir.file("no_such_program.exe"), {}, dir.file("child.log"));
        refused = waitFor([&] { return child->exited(); }, 5.0);  // POSIX: exec fails inside the child
    } catch (const MeraDBError&) {
        refused = true;  // Windows: CreateProcess fails in the parent
    }
    CHECK(refused);
}

TEST_CASE("sys_process killProcess ends a spawned process", "[sys]") {
    TempDir dir;
    // The "sleeper" test below only sleeps when the environment says so, so the child stays alive until killed
    // while an ordinary run of the whole suite (and ctest's per-test discovery) passes straight through it.
    sys::setEnv("MERADB_TEST_SLEEPER", "1");
    auto child = sys::spawnDetached(sys::executablePath(), {"sys_process sleeper for kill test"}, dir.file("child.log"));
    sys::setEnv("MERADB_TEST_SLEEPER", "");
    REQUIRE(child);
    sleepMs(300);
    CHECK_FALSE(child->exited());
    CHECK(sys::killProcess(child->pid()).empty());
    CHECK(waitFor([&] { return child->exited(); }, 10.0));
}

TEST_CASE("sys_process sleeper for kill test", "[sleeper]") {
    auto flag = sys::getEnv("MERADB_TEST_SLEEPER");
    if (flag && !flag->empty()) sleepMs(30000);  // only the child of the kill test sleeps
}

TEST_CASE("sys_process killProcess of a process that does not exist reports an error", "[sys]") {
    CHECK_FALSE(sys::killProcess(2000000000).empty());
}

TEST_CASE("control status: says so when no server runs", "[control]") {
    TempDir dir;
    ControlOptions options;
    options.dataDir = dir.str();
    Capture capture;
    CHECK(serverStatus(options) == 3);
    CHECK(capture.out().find("MeraDB server nahi chal raha  (data: ") == 0);
    CHECK(capture.out().find("Start karne ke liye: meradb start\n") != std::string::npos);
}

TEST_CASE("control status: describes a running server", "[control]") {
    RunningServer s;
    writeFakePidFile(s);
    ControlOptions options;
    options.dataDir = s.dataDir();
    Capture capture;
    CHECK(serverStatus(options) == 0);
    std::string out = capture.out();
    CHECK(out.find("MeraDB server chal raha hai\n  address:   127.0.0.1:" + std::to_string(s.port()) +
                   "\n  pid:       999999\n") == 0);
    CHECK(out.find("  started:   2026-01-01 00:00:00\n  version:   MeraDB 1.0.0\n  sessions:  ") != std::string::npos);
    CHECK(out.find(" connected\n  databases: main\n") != std::string::npos);
}

TEST_CASE("control status: asks for the password when the server has one", "[control]") {
    RunningServer s("sekrit");
    writeFakePidFile(s);
    ControlOptions options;
    options.dataDir = s.dataDir();
    {
        Capture capture;
        CHECK(serverStatus(options) == 0);  // still "running": only the details are missing
        CHECK(capture.out().find("  (details nahi mile: ") != std::string::npos);
    }
    options.password = "sekrit";
    Capture capture;
    CHECK(serverStatus(options) == 0);
    CHECK(capture.out().find("  version:   MeraDB 1.0.0\n") != std::string::npos);
}

TEST_CASE("control stop: with nothing running removes a stale pid file", "[control]") {
    TempDir dir;
    protocol::Json info = protocol::Json::object();
    info["pid"] = 999999;
    info["host"] = "127.0.0.1";
    info["port"] = 1;  // nobody listens there
    protocol::writePidFile(dir.str(), info);
    ControlOptions options;
    options.dataDir = dir.str();
    Capture capture;
    CHECK(serverStop(options) == 0);
    CHECK(capture.out() == "Server nahi chal raha.\n");
    CHECK_FALSE(std::filesystem::exists(protocol::pidFilePath(dir.str())));
}

TEST_CASE("control stop: shuts a running server down", "[control]") {
    RunningServer s;
    writeFakePidFile(s);
    ControlOptions options;
    options.dataDir = s.dataDir();
    Capture capture;
    CHECK(serverStop(options) == 0);
    CHECK(capture.out() == "MeraDB server band ho gaya (pid 999999).\n");
    CHECK(waitFor([&] { return s.stopped(); }));
}

TEST_CASE("control stop: without the password fails and points at --force", "[control]") {
    RunningServer s("sekrit");
    writeFakePidFile(s);
    ControlOptions options;
    options.dataDir = s.dataDir();
    Capture capture;
    CHECK(serverStop(options) == 1);
    CHECK(capture.err().find("Zabardasti band karne ke liye:  meradb stop --force\n") != std::string::npos);
    CHECK_FALSE(s.stopped());
}

TEST_CASE("control start: refuses when a server already runs for the folder", "[control]") {
    RunningServer s;
    writeFakePidFile(s);
    ControlOptions options;
    options.dataDir = s.dataDir();
    Capture capture;
    CHECK(serverStart(options) == 0);
    CHECK(capture.out() == "Server pehle se chal raha hai: 127.0.0.1:" + std::to_string(s.port()) + " (pid 999999)\n");
}

TEST_CASE("control start: refuses a port something else is using", "[control]") {
    RunningServer other;  // holds a port, but has no pid file for the folder we start in
    TempDir dir;
    ControlOptions options;
    options.dataDir = dir.str();
    options.port = other.port();
    Capture capture;
    CHECK(serverStart(options) == 1);
    CHECK(capture.err().find("Port " + std::to_string(other.port()) + " par pehle se kuch aur chal raha hai.") == 0);
}

TEST_CASE("control start: reports a child that dies at once, with its log", "[control]") {
    TempDir dir;
    ControlOptions options;
    options.dataDir = dir.str();
    options.port = 1;  // the child (this test program) rejects the unknown arguments and exits
    options.startTimeoutSeconds = 10.0;
    Capture capture;
    CHECK(serverStart(options) == 1);
    CHECK(capture.err().find("Server start nahi hua. Log (") == 0);
    CHECK(std::filesystem::exists(dir.path() / "server.log"));
}
```

- [ ] **Step 2: Add to `cpp/tests/CMakeLists.txt`; verify it fails**

```bash
cmake --build cpp/build
```
Expected: FAIL — `meradb/server_control.h: No such file or directory`.

- [ ] **Step 3: `sys_compat.h`** — inside `namespace meradb::sys`, after the existing declarations add

```cpp

// nullopt-free setter: overwrites the variable for this process (and its children).
void setEnv(const std::string& name, const std::string& value);

// Reads one line from the terminal WITHOUT echoing it (a password prompt, like
// getpass). The prompt goes to stderr. When stdin is not a terminal the line is
// simply read.
std::string readHidden(const std::string& prompt);

// UTF-8 path of the running executable ("" if the OS will not say).
std::string executablePath();

// A child process started so that it outlives this one (no terminal, no shared
// stdio). Only ever created by spawnDetached().
class DetachedProcess {
public:
    struct Impl;
    explicit DetachedProcess(std::unique_ptr<Impl> impl);
    ~DetachedProcess();
    DetachedProcess(const DetachedProcess&) = delete;
    DetachedProcess& operator=(const DetachedProcess&) = delete;

    std::int64_t pid() const;
    bool exited();  // true once the process has ended; never blocks

private:
    std::unique_ptr<Impl> impl_;
};

// Starts `exePath args...` detached from this process and terminal: stdin is
// the null device, stdout and stderr are APPENDED to `logPath`. Throws
// StorageError if the process cannot be started.
std::unique_ptr<DetachedProcess> spawnDetached(const std::string& exePath, const std::vector<std::string>& args,
                                               const std::string& logPath);

// Asks the OS to end a process (SIGTERM; TerminateProcess on Windows).
// Returns "" on success, otherwise the OS error text.
std::string killProcess(std::int64_t pid);
```

and make sure the header includes `<cstdint>`, `<memory>`, `<string>` and `<vector>`.

- [ ] **Step 4: `sys_compat.cpp`**

4a. Replace the include block at the top so that it also has `<iostream>`, `<memory>`,
`<system_error>` and `<vector>`, and the POSIX branch has `<fcntl.h>`, `<signal.h>`,
`<sys/types.h>`, `<sys/wait.h>`, `<termios.h>`, `<cerrno>` (plus `<mach-o/dyld.h>` on macOS):

```cpp
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <cerrno>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif
```
(keep any other include the Task 1 version already has).

4b. Directly before the closing `}  // namespace meradb::sys` add:

```cpp
// ---------------------------------------------------------------------------
// environment, terminal, processes
// ---------------------------------------------------------------------------

void setEnv(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());
#else
    ::setenv(name.c_str(), value.c_str(), 1);
#endif
}

std::string readHidden(const std::string& prompt) {
    std::cerr << prompt << std::flush;
    std::string line;
#ifdef _WIN32
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool terminal = (in != INVALID_HANDLE_VALUE) && GetConsoleMode(in, &mode);
    if (terminal) SetConsoleMode(in, mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
    std::getline(std::cin, line);
    if (terminal) {
        SetConsoleMode(in, mode);
        std::cerr << "\n";
    }
#else
    termios saved{};
    const bool terminal = ::isatty(STDIN_FILENO) && ::tcgetattr(STDIN_FILENO, &saved) == 0;
    if (terminal) {
        termios quiet = saved;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
    }
    std::getline(std::cin, line);
    if (terminal) {
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
        std::cerr << "\n";
    }
#endif
    if (!line.empty() && line.back() == '\r') line.pop_back();
    return line;
}

#ifdef _WIN32
namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], n);
    return out;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &out[0], n, nullptr, nullptr);
    return out;
}

// The rules of CommandLineToArgvW, run backwards.
std::wstring quoteArgument(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (auto it = arg.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            out.append(backslashes * 2, L'\\');  // a trailing run must not escape the closing quote
            break;
        }
        if (*it == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(*it);
        }
    }
    out.push_back(L'"');
    return out;
}

}  // namespace
#endif

std::string executablePath() {
#ifdef _WIN32
    std::wstring buffer(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, &buffer[0], static_cast<DWORD>(buffer.size()));
    buffer.resize(n);
    return narrow(buffer);
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof buffer;
    if (_NSGetExecutablePath(buffer, &size) != 0) return std::string();
    return std::string(buffer);
#else
    char buffer[4096];
    ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof buffer - 1);
    if (n <= 0) return std::string();
    return std::string(buffer, static_cast<std::size_t>(n));
#endif
}

struct DetachedProcess::Impl {
    std::int64_t pid = 0;
    bool done = false;
#ifdef _WIN32
    HANDLE process = nullptr;
#endif
};

DetachedProcess::DetachedProcess(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

DetachedProcess::~DetachedProcess() {
#ifdef _WIN32
    if (impl_ && impl_->process != nullptr) CloseHandle(impl_->process);
#endif
}

std::int64_t DetachedProcess::pid() const { return impl_->pid; }

bool DetachedProcess::exited() {
    if (impl_->done) return true;
#ifdef _WIN32
    if (WaitForSingleObject(impl_->process, 0) == WAIT_OBJECT_0) impl_->done = true;
#else
    int status = 0;
    pid_t result = ::waitpid(static_cast<pid_t>(impl_->pid), &status, WNOHANG);
    if (result != 0) impl_->done = true;  // > 0: it ended and is reaped now; -1: it is not our child (any more)
#endif
    return impl_->done;
}

std::unique_ptr<DetachedProcess> spawnDetached(const std::string& exePath, const std::vector<std::string>& args,
                                               const std::string& logPath) {
    auto impl = std::make_unique<DetachedProcess::Impl>();
#ifdef _WIN32
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof inheritable;
    inheritable.bInheritHandle = TRUE;
    HANDLE log = CreateFileW(widen(logPath).c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE)
        throw StorageError("Log file nahi khuli: " + logPath + " (" + std::system_category().message(static_cast<int>(GetLastError())) + ")");
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);

    std::wstring commandLine = quoteArgument(widen(exePath));
    for (const auto& arg : args) commandLine += L" " + quoteArgument(widen(arg));

    // Inherit ONLY the log and NUL handles. With plain bInheritHandles the child would also inherit every other
    // inheritable handle of this process -- e.g. the write end of a pipe that our caller reads until EOF, which
    // would then never end for as long as the server lives.
    HANDLE inherited[2] = {nul, log};
    const DWORD inheritedCount = nul != INVALID_HANDLE_VALUE ? 2 : 1;
    HANDLE* inheritedList = nul != INVALID_HANDLE_VALUE ? inherited : inherited + 1;
    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<char> attributeStorage(attributeBytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes) ||
        !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritedList,
                                   inheritedCount * sizeof(HANDLE), nullptr, nullptr)) {
        CloseHandle(log);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        throw StorageError("Process start nahi hua: " + exePath + " (handle list)");
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nul;
    startup.StartupInfo.hStdOutput = log;
    startup.StartupInfo.hStdError = log;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION info{};
    BOOL started = CreateProcessW(widen(exePath).c_str(), &commandLine[0], nullptr, nullptr, TRUE,
                                  DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                                  nullptr, &startup.StartupInfo, &info);
    const DWORD failure = started ? 0 : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(log);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started)
        throw StorageError("Process start nahi hua: " + exePath + " (" + std::system_category().message(static_cast<int>(failure)) + ")");
    CloseHandle(info.hThread);
    impl->process = info.hProcess;
    impl->pid = static_cast<std::int64_t>(info.dwProcessId);
#else
    // Everything that allocates happens BEFORE fork(): the child may only call async-signal-safe functions.
    std::vector<std::string> storage;
    storage.push_back(exePath);
    for (const auto& arg : args) storage.push_back(arg);
    std::vector<char*> argv;
    for (auto& s : storage) argv.push_back(&s[0]);
    argv.push_back(nullptr);

    int logFd = ::open(logPath.c_str(), O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (logFd < 0) throw StorageError("Log file nahi khuli: " + logPath + " (" + std::system_category().message(errno) + ")");
    int nullFd = ::open("/dev/null", O_RDONLY);

    pid_t child = ::fork();
    if (child < 0) {
        int e = errno;
        ::close(logFd);
        if (nullFd >= 0) ::close(nullFd);
        throw StorageError("Process start nahi hua: " + exePath + " (" + std::system_category().message(e) + ")");
    }
    if (child == 0) {
        ::setsid();  // no controlling terminal: survives the parent's shell closing
        if (nullFd >= 0) ::dup2(nullFd, STDIN_FILENO);
        ::dup2(logFd, STDOUT_FILENO);
        ::dup2(logFd, STDERR_FILENO);
        ::execv(exePath.c_str(), argv.data());
        ::_exit(127);  // exec failed
    }
    ::close(logFd);
    if (nullFd >= 0) ::close(nullFd);
    impl->pid = static_cast<std::int64_t>(child);
#endif
    return std::make_unique<DetachedProcess>(std::move(impl));
}

std::string killProcess(std::int64_t pid) {
#ifdef _WIN32
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) return std::system_category().message(static_cast<int>(GetLastError()));
    const BOOL ok = TerminateProcess(process, 1);
    const DWORD failure = ok ? 0 : GetLastError();
    CloseHandle(process);
    return ok ? std::string() : std::system_category().message(static_cast<int>(failure));
#else
    if (::kill(static_cast<pid_t>(pid), SIGTERM) != 0) return std::system_category().message(errno);
    return std::string();
#endif
}
```

- [ ] **Step 5: `server_control`**

`cpp/include/meradb/server_control.h`:

```cpp
// cpp/include/meradb/server_control.h
//
// `meradb start | stop | status` (mirrors cmd_start / cmd_stop / cmd_status in
// meradb/cli.py). Each function prints like Python does -- results on stdout,
// side notes on stderr -- and returns the process exit code.
#pragma once
#include <optional>
#include <string>

namespace meradb {

struct ControlOptions {
    std::string dataDir;
    std::string host = "127.0.0.1";
    int port = 6372;
    std::optional<std::string> password;  // shared password: given to a starting server, or used to reach a running one
    bool verbose = false;                 // start: log every query
    bool force = false;                   // stop: kill the process if a polite shutdown fails
    std::string exePath;                  // start: the program to run as `<exe> server ...` (default: this program)
    double startTimeoutSeconds = 15.0;
    double stopTimeoutSeconds = 10.0;
};

int serverStart(const ControlOptions& options);
int serverStop(const ControlOptions& options);
int serverStatus(const ControlOptions& options);

}  // namespace meradb
```

`cpp/src/server_control.cpp`:

```cpp
// cpp/src/server_control.cpp -- see server_control.h.
#include "meradb/server_control.h"
#include "meradb/client.h"
#include "meradb/errors.h"
#include "meradb/net_compat.h"
#include "meradb/protocol.h"
#include "meradb/sys_compat.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

namespace meradb {
namespace {

using protocol::Json;

void note(const std::string& message) { std::cerr << message << "\n"; }

std::string absolutePath(const std::string& path) {
    std::error_code ec;
    auto absolute = std::filesystem::absolute(std::filesystem::u8path(path), ec);
    return ec ? path : absolute.lexically_normal().u8string();
}

bool isWildcardHost(const std::string& host) { return host == "0.0.0.0" || host == "::" || host.empty(); }

std::string tailOf(const std::string& path, std::size_t lines = 15) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "(log nahi mila)";
    std::vector<std::string> all;
    std::string line;
    while (std::getline(in, line)) all.push_back(line + "\n");
    std::string out;
    for (std::size_t i = all.size() > lines ? all.size() - lines : 0; i < all.size(); ++i) out += all[i];
    return out;
}

std::string passwordFor(const ControlOptions& options) {
    if (options.password) return *options.password;
    return sys::getEnv("MERADB_PASSWORD").value_or("");
}

ConnectOptions connectOptionsFor(const ControlOptions& options, const std::string& host, int port) {
    ConnectOptions connect;
    connect.host = host;
    connect.port = port;
    std::string password = passwordFor(options);
    if (!password.empty()) connect.password = password;
    return connect;
}

double seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void sleepShort() { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }

}  // namespace

int serverStart(const ControlOptions& options) {
    const std::string data = absolutePath(options.dataDir);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::u8path(data), ec);
    if (auto info = protocol::runningServer(data)) {
        std::cout << "Server pehle se chal raha hai: " << info->value("host", std::string("?")) << ":"
                  << (*info)["port"].dump() << " (pid " << (*info)["pid"].dump() << ")\n";
        return 0;
    }
    const std::string checkHost = isWildcardHost(options.host) ? protocol::kDefaultHost : options.host;
    if (net::portOpen(checkHost, options.port, 0.5)) {
        note("Port " + std::to_string(options.port) +
             " par pehle se kuch aur chal raha hai. Doosra port do:  meradb start --port 6373");
        return 1;
    }

    const std::string logPath = (std::filesystem::u8path(data) / "server.log").u8string();
    std::vector<std::string> args = {"server", "--data", data, "--host", options.host, "--port",
                                     std::to_string(options.port)};
    if (options.verbose) args.push_back("--verbose");
    // The password travels in the environment so it does not show in the process list.
    const bool hasPassword = options.password && !options.password->empty();
    std::optional<std::string> savedPassword = sys::getEnv("MERADB_PASSWORD");
    if (hasPassword) sys::setEnv("MERADB_PASSWORD", *options.password);

    std::unique_ptr<sys::DetachedProcess> child;
    try {
        child = sys::spawnDetached(options.exePath.empty() ? sys::executablePath() : options.exePath, args, logPath);
    } catch (const std::exception& e) {
        if (hasPassword) sys::setEnv("MERADB_PASSWORD", savedPassword.value_or(""));
        note(e.what());
        return 1;
    }
    if (hasPassword) sys::setEnv("MERADB_PASSWORD", savedPassword.value_or(""));

    const double deadline = seconds() + options.startTimeoutSeconds;
    while (seconds() < deadline) {
        if (child->exited()) {
            note("Server start nahi hua. Log (" + logPath + "):");
            note(tailOf(logPath));
            return 1;
        }
        // No server served this folder a moment ago, so any that appears now is ours.
        if (auto info = protocol::runningServer(data)) {
            std::cout << "MeraDB server chal gaya: " << options.host << ":" << (*info)["port"].dump() << "  (pid "
                      << (*info)["pid"].dump() << ")\n";
            std::cout << "  data: " << data << "\n";
            std::cout << "  log:  " << logPath << "\n";
            std::cout << "  connect: meradb shell   |   band: meradb stop\n";
            return 0;
        }
        sleepShort();
    }
    note("Server " + std::to_string(static_cast<int>(options.startTimeoutSeconds)) +
         " second mein ready nahi hua. Log dekho: " + logPath);
    return 1;
}

int serverStop(const ControlOptions& options) {
    const std::string data = absolutePath(options.dataDir);
    auto info = protocol::runningServer(data);
    if (!info) {
        if (auto stale = protocol::readPidFile(data))
            protocol::removePidFile(data, stale->value("pid", static_cast<std::int64_t>(-1)));  // left by a crash
        std::cout << "Server nahi chal raha.\n";
        return 0;
    }
    std::string host = info->value("host", std::string(protocol::kDefaultHost));
    if (isWildcardHost(host)) host = protocol::kDefaultHost;
    const int port = static_cast<int>((*info)["port"].get<std::int64_t>());
    const std::int64_t pid = info->value("pid", static_cast<std::int64_t>(0));
    try {
        Connection connection(connectOptionsFor(options, host, port));
        connection.shutdown();
    } catch (const MeraDBError& e) {
        if (!options.force) {
            note(std::string(e.what()) + "\nZabardasti band karne ke liye:  meradb stop --force");
            return 1;
        }
        note(std::string(e.what()) + " -- process " + std::to_string(pid) + " ko kill kar rahe hain (--force)");
        std::string failure = sys::killProcess(pid);
        if (!failure.empty()) {
            note("Kill fail: " + failure);
            return 1;
        }
        protocol::removePidFile(data, pid);
    }
    const double deadline = seconds() + options.stopTimeoutSeconds;
    while (seconds() < deadline) {
        if (!net::portOpen(host, port, 0.5)) {
            std::cout << "MeraDB server band ho gaya (pid " << pid << ").\n";
            return 0;
        }
        sleepShort();
    }
    note("Server abhi bhi chal raha hai -- `meradb stop --force` try karo");
    return 1;
}

int serverStatus(const ControlOptions& options) {
    const std::string data = absolutePath(options.dataDir);
    auto info = protocol::runningServer(data);
    if (!info) {
        std::cout << "MeraDB server nahi chal raha  (data: " << data << ")\n";
        std::cout << "Start karne ke liye: meradb start\n";
        return 3;  // conventional "not running" exit code for database status tools
    }
    const std::string host = info->value("host", std::string(protocol::kDefaultHost));
    const std::string connectHost = isWildcardHost(host) ? protocol::kDefaultHost : host;
    const int port = static_cast<int>((*info)["port"].get<std::int64_t>());
    std::cout << "MeraDB server chal raha hai\n";
    std::cout << "  address:   " << host << ":" << port << "\n";
    std::cout << "  pid:       " << (info->contains("pid") ? (*info)["pid"].dump() : std::string("None")) << "\n";
    std::cout << "  data:      " << data << "\n";
    std::cout << "  started:   " << info->value("started", std::string("?")) << "\n";
    try {
        Connection connection(connectOptionsFor(options, connectHost, port));
        Json status = connection.status();
        std::cout << "  version:   " << status.value("server", std::string("None")) << "\n";
        std::cout << "  sessions:  " << status["sessions"].dump() << " connected\n";
        std::string names;
        for (const auto& name : status["databases"]) names += (names.empty() ? "" : ", ") + name.get<std::string>();
        std::cout << "  databases: " << names << "\n";
    } catch (const MeraDBError& e) {
        std::cout << "  (details nahi mile: " << e.what() << ")\n";
    }
    return 0;
}

}  // namespace meradb
```

- [ ] **Step 6: CMake** — add `src/server_control.cpp` to `meradb_core`.

- [ ] **Step 7: Build and run**

```bash
cmake -S cpp -B cpp/build -G "MinGW Makefiles"
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "sys_process|control"
```
Expected: 8 `sys_process` + 9 `control` test cases pass, no stray `meradb`/test processes left
(`tasklist | findstr meradb` on Windows, `pgrep -f meradb` elsewhere).

- [ ] **Step 8: Commit**

```bash
git add cpp/include/meradb/sys_compat.h cpp/src/sys_compat.cpp cpp/include/meradb/server_control.h cpp/src/server_control.cpp cpp/tests/test_server_control.cpp cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add process control: spawn, kill and the start/stop/status logic"
```

**Completion checklist:**
- [ ] the Windows spawn uses an explicit inherited-handle list (grep `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`)
- [ ] the POSIX child branch calls only async-signal-safe functions
- [ ] argument quoting has a test with spaces (`"sys_process setEnv and getEnv round trip"` arrives as one argument)
- [ ] no test leaves a process running
- [ ] full suite green, zero warnings

---

### Task 21: CLI — option parsing, dispatch, `run` with local fallback, lifecycle ctest

**Files:**
- Create: `cpp/include/meradb/cli.h`, `cpp/src/cli.cpp`, `cpp/tests/cli_lifecycle.py`
- Modify: `cpp/src/main.cpp` (rewritten), `cpp/CMakeLists.txt` (add `src/cli.cpp`), `cpp/tests/CMakeLists.txt`, `cpp/tests/cross_engine_diff.py`
- Test: `cpp/tests/test_cli.cpp`

**Interfaces:**
- Consumes: everything above.
- Produces: `CliArgs parseCliArgs(argv)`, `std::unique_ptr<Backend> openBackend(const CliArgs&)`,
  `bool runFile(Backend&, path)`, `int cliMain(argv)`; `main()` becomes a five-line wrapper.
- Python behaviour mirrored (`meradb/cli.py`): `meradb x.mdb` = `run x.mdb`, `--tui` =
  `workbench`, no or unknown command = `shell`; `-D/--data` (default `protocol::defaultDataDir()`,
  i.e. `MERADB_DATA` or the per-user folder — **a change from Phase 1's `./data` default,
  needed for Python parity; see D7**); server commands take `--host` (default `127.0.0.1`),
  `--port` (default `MERADB_PORT` or 6372), `--password VALUE` (default `MERADB_PASSWORD`), `-v`;
  client commands take `-H/--host`, `-p/--port`, `-d/--database`, `-W` (ask for the password;
  else `MERADB_PASSWORD`), `-U/--user` (default `MERADB_USER`), `--local`; `stop` also has
  `--force`; argparse-style errors print
  `usage: meradb [-h] [--version] COMMAND ...` then `meradb: error: ...` and exit **2**.
- `openBackend` order (exactly Python's): `--local` → embedded engine (a `-U` is ignored with a
  note); an explicit `--host`/`--port` means **no fallback** if the server is down;
  otherwise host = `MERADB_HOST` or 127.0.0.1, port = `MERADB_PORT`, else the port in the data
  folder's pid file, else 6372; if nobody is listening, print
  `(HOST:PORT par server nahi mila -- LOCAL mode: seedha 'DIR' khol rahe hain. Server ke liye: meradb start)`
  on stderr and open the folder directly, printing a `RECOVERY:` note for every database whose
  unfinished transaction was rolled back.
- `shell` and `workbench` print a "not in the C++ version yet" note and exit 1 (Phases 3-4).
- `run` runs **all** files even after a failure; exit 1 if any statement or file failed.

- [ ] **Step 1: Write the failing tests**

`cpp/tests/test_cli.cpp` (argument parsing, environment defaults, argparse-style errors,
`run --local`, the fallback note, explicit `--port` refusing to fall back, `run` through a
real in-process server, a user login honouring privileges):

```cpp
// cpp/tests/test_cli.cpp -- argument parsing, backend selection and `run` output.
#include <catch2/catch_test_macros.hpp>
#include "meradb/cli.h"
#include "meradb/errors.h"
#include "meradb/sys_compat.h"
#include "server_fixture.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

using namespace meradb;
using namespace meradb_test;

namespace {

class Capture {
public:
    Capture() : oldOut_(std::cout.rdbuf(out_.rdbuf())), oldErr_(std::cerr.rdbuf(err_.rdbuf())) {}
    ~Capture() {
        std::cout.rdbuf(oldOut_);
        std::cerr.rdbuf(oldErr_);
    }
    std::string out() const { return out_.str(); }
    std::string err() const { return err_.str(); }

private:
    std::ostringstream out_, err_;
    std::streambuf* oldOut_;
    std::streambuf* oldErr_;
};

// Environment variables the parser reads, cleared for the duration of a test.
class CleanEnv {
public:
    CleanEnv() {
        for (const char* name : names_) {
            saved_.push_back(sys::getEnv(name));
            sys::setEnv(name, "");
        }
    }
    ~CleanEnv() {
        for (std::size_t i = 0; i < saved_.size(); ++i) sys::setEnv(names_[i], saved_[i].value_or(""));
    }

private:
    const char* names_[6] = {"MERADB_DATA", "MERADB_HOST", "MERADB_PORT", "MERADB_PASSWORD", "MERADB_USER", "MERADB_X"};
    std::vector<std::optional<std::string>> saved_;
};

void writeFile(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
}

}  // namespace

TEST_CASE("cli shortcuts: a .mdb file means run, --tui means workbench, nothing means shell", "[cli]") {
    CleanEnv env;
    CHECK(parseCliArgs({"demo.mdb"}).command == "run");
    CHECK(parseCliArgs({"demo.mdb"}).files == std::vector<std::string>{"demo.mdb"});
    CHECK(parseCliArgs({"--tui"}).command == "workbench");
    CHECK(parseCliArgs({"tui"}).command == "workbench");
    CHECK(parseCliArgs({}).command == "shell");
    CHECK(parseCliArgs({"--local"}).command == "shell");  // an unknown first word becomes `shell ...`
    CHECK(parseCliArgs({"--local"}).local);
    CHECK(parseCliArgs({"--version"}).showVersion);
    CHECK(parseCliArgs({"-h"}).showHelp);
    CHECK(parseCliArgs({"status", "--help"}).showHelp);
}

TEST_CASE("cli server options and their defaults", "[cli]") {
    CleanEnv env;
    CliArgs args = parseCliArgs({"server"});
    CHECK(args.host.value() == "127.0.0.1");
    CHECK(args.port.value() == 6372);
    CHECK_FALSE(args.password.has_value());
    CHECK_FALSE(args.verbose);

    args = parseCliArgs({"start", "-D", "d", "--host", "0.0.0.0", "--port", "7000", "--password", "pw", "-v"});
    CHECK(args.command == "start");
    CHECK(args.dataDir == "d");
    CHECK(args.host.value() == "0.0.0.0");
    CHECK(args.port.value() == 7000);
    CHECK(args.password.value() == "pw");
    CHECK(args.verbose);

    args = parseCliArgs({"server", "--data=x", "--port=1234"});
    CHECK(args.dataDir == "x");
    CHECK(args.port.value() == 1234);
}

TEST_CASE("cli environment variables set the defaults", "[cli]") {
    CleanEnv env;
    sys::setEnv("MERADB_PORT", "7001");
    sys::setEnv("MERADB_PASSWORD", "envpw");
    sys::setEnv("MERADB_USER", "envuser");
    CHECK(parseCliArgs({"server"}).port.value() == 7001);
    CHECK(parseCliArgs({"server"}).password.value() == "envpw");
    CHECK(parseCliArgs({"server", "--port", "9"}).port.value() == 9);  // a flag beats the environment
    CHECK(parseCliArgs({"shell"}).user.value() == "envuser");
    CHECK_FALSE(parseCliArgs({"shell"}).port.has_value());  // client: MERADB_PORT is applied later, as a non-explicit default
    sys::setEnv("MERADB_DATA", "envdata");
    CHECK(parseCliArgs({"status"}).dataDir == "envdata");
}

TEST_CASE("cli client options", "[cli]") {
    CleanEnv env;
    CliArgs args = parseCliArgs({"run", "a.mdb", "b.mdb", "-H", "10.0.0.5", "-p", "6400", "-d", "school", "-W", "-U", "asha", "--local"});
    CHECK(args.command == "run");
    CHECK(args.files == std::vector<std::string>{"a.mdb", "b.mdb"});
    CHECK(args.host.value() == "10.0.0.5");
    CHECK(args.port.value() == 6400);
    CHECK(args.database.value() == "school");
    CHECK(args.askPassword);
    CHECK(args.user.value() == "asha");
    CHECK(args.local);
}

TEST_CASE("cli stop and status options", "[cli]") {
    CleanEnv env;
    CliArgs stop = parseCliArgs({"stop", "--force", "-W", "-D", "d"});
    CHECK(stop.force);
    CHECK(stop.askPassword);
    CHECK(stop.dataDir == "d");
    CHECK_FALSE(parseCliArgs({"status"}).force);
}

TEST_CASE("cli bad arguments are reported", "[cli]") {
    CleanEnv env;
    CHECK(parseCliArgs({"run"}).error == "the following arguments are required: files");
    CHECK(parseCliArgs({"server", "--port", "abc"}).error == "argument --port: invalid int value: 'abc'");
    CHECK(parseCliArgs({"server", "--port"}).error == "argument --port: expected one argument");
    CHECK(parseCliArgs({"status", "--nope"}).error == "unrecognized arguments: --nope");
    CHECK(parseCliArgs({"stop", "extra"}).error == "unrecognized arguments: extra");
    CHECK(parseCliArgs({"server", "--force"}).error == "unrecognized arguments: --force");  // stop only
}

TEST_CASE("cli the default data folder is the per-user one", "[cli]") {
    CleanEnv env;
    CHECK(parseCliArgs({"run", "x.mdb"}).dataDir == protocol::defaultDataDir());
}

TEST_CASE("cli cliMain prints version and help", "[cli]") {
    Capture capture;
    CHECK(cliMain({"--version"}) == 0);
    CHECK(capture.out() == "MeraDB 1.0.0\n");
    CHECK(cliMain({"--help"}) == 0);
    CHECK(capture.out().find("usage: meradb") != std::string::npos);
    CHECK(cliMain({"run"}) == 2);
    CHECK(capture.err().find("meradb: error: the following arguments are required: files") != std::string::npos);
}

TEST_CASE("cli run --local executes a script and returns 1 on any failure", "[cli]") {
    CleanEnv env;
    TempDir dir;
    writeFile(dir.file("good.mdb"), "BANAO TABLE t (x INT); DAALO MEIN t MAAN (1); DIKHAO * SE t;");
    writeFile(dir.file("bad.mdb"), "DIKHAO * SE gayab;");
    {
        Capture capture;
        CHECK(cliMain({"run", dir.file("good.mdb"), "--local", "--data", dir.file("data")}) == 0);
        CHECK(capture.out().find("1 row(s)") != std::string::npos);
    }
    {
        Capture capture;
        CHECK(cliMain({"run", dir.file("bad.mdb"), dir.file("good.mdb"), "--local", "--data", dir.file("data2")}) == 1);
        CHECK(capture.out().find("[Execution Galti] Table 'gayab' exist nahi karta") != std::string::npos);
        CHECK(capture.out().find("1 row(s)") != std::string::npos);  // the second file still ran
    }
    {
        Capture capture;
        CHECK(cliMain({"run", dir.file("missing.mdb"), "--local", "--data", dir.file("data3")}) == 1);
        CHECK(capture.out().find("File nahi khuli: [Errno 2] No such file or directory: '") == 0);
    }
}

TEST_CASE("cli --local with a user says it is ignored", "[cli]") {
    CleanEnv env;
    TempDir dir;
    writeFile(dir.file("s.mdb"), "DIKHAO TABLES;");
    Capture capture;
    CHECK(cliMain({"run", dir.file("s.mdb"), "--local", "-U", "asha", "--data", dir.file("d")}) == 0);
    CHECK(capture.err() == "(--local mode mein -U/--user 'asha' ka koi matlab nahi -- ignore kiya, superuser ki tarah chal raha hai)\n");
}

TEST_CASE("cli falls back to local mode when no server is found", "[cli]") {
    CleanEnv env;
    TempDir dir;
    RunningServer other;  // find a free port by borrowing one, then stop the server so nothing listens on it
    int freePort = other.port();
    other.stop();
    sys::setEnv("MERADB_PORT", std::to_string(freePort));
    writeFile(dir.file("s.mdb"), "DIKHAO TABLES;");
    Capture capture;
    CHECK(cliMain({"run", dir.file("s.mdb"), "--data", dir.file("d")}) == 0);
    CHECK(capture.err().find("(127.0.0.1:" + std::to_string(freePort) + " par server nahi mila -- LOCAL mode: seedha '") == 0);
    CHECK(capture.err().find("Server ke liye: meradb start)\n") != std::string::npos);
}

TEST_CASE("cli an explicit --port does not fall back", "[cli]") {
    CleanEnv env;
    TempDir dir;
    RunningServer other;
    int freePort = other.port();
    other.stop();
    writeFile(dir.file("s.mdb"), "DIKHAO TABLES;");
    Capture capture;
    CHECK(cliMain({"run", dir.file("s.mdb"), "--port", std::to_string(freePort), "--data", dir.file("d")}) == 1);
    CHECK(capture.err().find("par MeraDB server nahi mila") != std::string::npos);
}

TEST_CASE("cli run goes through a server when one answers", "[cli]") {
    CleanEnv env;
    TempDir dir;
    RunningServer s;
    writeFile(dir.file("s.mdb"), "BANAO TABLE via_server (x INT); DIKHAO TABLES;");
    Capture capture;
    CHECK(cliMain({"run", dir.file("s.mdb"), "--port", std::to_string(s.port())}) == 0);
    CHECK(capture.out().find("via_server") != std::string::npos);
    CHECK(std::filesystem::exists(std::filesystem::path(s.dataDir()) / "main"));  // it really ran in the server's folder
}

TEST_CASE("cli run with a user logs in and honours privileges", "[cli]") {
    CleanEnv env;
    TempDir dir;
    RunningServer s;
    s.instance().users().create("asha", "pw");
    writeFile(dir.file("s.mdb"), "DIKHAO TABLES;");
    {
        Capture capture;
        sys::setEnv("MERADB_PASSWORD", "pw");
        // logged in as asha: a restricted user, so DDL / admin commands are refused
        CHECK(cliMain({"run", dir.file("s.mdb"), "--port", std::to_string(s.port()), "-U", "asha"}) == 1);
        CHECK(capture.out().find("'asha' superuser nahi hai") != std::string::npos);
        CHECK(capture.err().empty());
    }
    {
        Capture capture;
        sys::setEnv("MERADB_PASSWORD", "wrong");
        CHECK(cliMain({"run", dir.file("s.mdb"), "--port", std::to_string(s.port()), "-U", "asha"}) == 1);
        CHECK_FALSE(capture.err().empty());
    }
}

TEST_CASE("cli shell and workbench say they are not here yet", "[cli]") {
    CleanEnv env;
    Capture capture;
    CHECK(cliMain({"shell"}) == 1);
    CHECK(cliMain({"workbench"}) == 1);
    CHECK(capture.err().find("abhi C++ version mein nahi hai") != std::string::npos);
}
```

`cpp/tests/cli_lifecycle.py` — the end-to-end twin of `CommandLineTest` in
`tests/test_server.py`, driving the real `meradb_cli` as separate processes: start, start
again, status, run through the server, run finding the server through the pid file, busy
port, stop, status exit 3, stop when stopped; a password-protected server (refused without
the password, works with `MERADB_PASSWORD`, `stop` fails and points at `--force`,
`stop --force` kills it); the local fallback; `--local` on a served folder being refused.

```python
"""
End-to-end check of the C++ command line with REAL server processes:
start / status / run through the server / stop, the password and --force paths,
the local fallback, and the "already running" / "port busy" refusals.

    python cpp/tests/cli_lifecycle.py path/to/meradb_cli

Modelled on CommandLineTest in tests/test_server.py. Prints PASS/FAIL per
scenario and exits non-zero if any failed.
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLI = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "cpp" / "build" / "meradb_cli")
failures = []


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def meradb(*args, env=None):
    base = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_")}
    base.update(env or {})
    r = subprocess.run([CLI, *args], capture_output=True, timeout=90, env=base)
    return r.returncode, r.stdout.decode("utf-8").replace("\r\n", "\n"), r.stderr.decode("utf-8").replace("\r\n", "\n")


def check(name, condition, detail=""):
    print(("PASS " if condition else "FAIL ") + name)
    if not condition:
        failures.append(name)
        if detail:
            print("     " + detail.replace("\n", "\n     "))


def scenario_lifecycle():
    data, port = tempfile.mkdtemp(), str(free_port())
    try:
        code, out, err = meradb("start", "--data", data, "--port", port)
        check("start: exit 0", code == 0, out + err)
        check("start: says it is up", out.startswith("MeraDB server chal gaya: 127.0.0.1:" + port), out)
        check("start: pid file written", os.path.exists(os.path.join(data, "meradb.pid")))

        code, out, err = meradb("start", "--data", data, "--port", port)
        check("start twice: already running, exit 0", code == 0 and out.startswith("Server pehle se chal raha hai: 127.0.0.1:" + port), out + err)

        code, out, err = meradb("status", "--data", data)
        check("status: exit 0 and details", code == 0 and "  version:   MeraDB 1.0.0\n" in out and "  databases: main\n" in out, out + err)

        script = os.path.join(data, "t.mdb")
        with open(script, "w", encoding="utf-8") as f:
            f.write("BANAO TABLE t (x INT); DAALO MEIN t MAAN (42); DIKHAO * SE t;")
        code, out, err = meradb("run", "--port", port, script)
        check("run via server: output has 42, no fallback note", code == 0 and "42" in out and "LOCAL mode" not in err, out + err)
        code, out, err = meradb("run", script, "--data", data)  # no --port: found through the pid file
        check("run finds the server through the pid file", "LOCAL mode" not in err, out + err)

        code, out, err = meradb("start", "--data", tempfile.mkdtemp(), "--port", port)
        check("start on a busy port refuses", code == 1 and err.startswith("Port " + port + " par pehle se kuch aur chal raha hai."), out + err)

        code, out, err = meradb("stop", "--data", data)
        check("stop: exit 0", code == 0 and out.startswith("MeraDB server band ho gaya (pid "), out + err)
        check("stop: pid file gone", not os.path.exists(os.path.join(data, "meradb.pid")))
        code, out, err = meradb("status", "--data", data)
        check("status when stopped: exit 3", code == 3 and out.startswith("MeraDB server nahi chal raha"), out + err)
        code, out, err = meradb("stop", "--data", data)
        check("stop when stopped: exit 0", code == 0 and out == "Server nahi chal raha.\n", out + err)
    finally:
        meradb("stop", "--data", data, "--force")
        shutil.rmtree(data, ignore_errors=True)


def scenario_password_and_force():
    data, port = tempfile.mkdtemp(), str(free_port())
    try:
        code, out, err = meradb("start", "--data", data, "--port", port, "--password", "sekrit")
        check("start with a password", code == 0, out + err)
        script = os.path.join(data, "t.mdb")
        with open(script, "w", encoding="utf-8") as f:
            f.write("DIKHAO TABLES;")
        code, out, err = meradb("run", "--port", port, script)
        check("run without the password is refused", code == 1 and "assword" in err, out + err)
        code, out, err = meradb("run", "--port", port, script, env={"MERADB_PASSWORD": "sekrit"})
        check("run with MERADB_PASSWORD works", code == 0, out + err)
        code, out, err = meradb("status", "--data", data)
        check("status without the password: still running, no details", code == 0 and "(details nahi mile:" in out, out + err)
        code, out, err = meradb("stop", "--data", data)
        check("stop without the password fails and points at --force",
              code == 1 and "Zabardasti band karne ke liye:  meradb stop --force" in err, out + err)
        code, out, err = meradb("stop", "--data", data, "--force")
        check("stop --force kills it", code == 0 and "(--force)" in err and out.startswith("MeraDB server band ho gaya"), out + err)
        code, out, err = meradb("status", "--data", data)
        check("after --force: not running", code == 3, out + err)
    finally:
        meradb("stop", "--data", data, "--force")
        shutil.rmtree(data, ignore_errors=True)


def scenario_local_fallback():
    data = tempfile.mkdtemp()
    try:
        env = {"MERADB_PORT": str(free_port())}
        demo = str(ROOT / "examples" / "demo.mdb")
        code, out, err = meradb("run", "--data", data, demo, env=env)
        check("fallback: says LOCAL mode", "LOCAL mode" in err, err)
        check("fallback: the script ran", "Transaction WAPAS" in out, out[-300:])
        fresh = tempfile.mkdtemp()  # the demo creates tables, so it needs an empty folder each time
        try:
            code, out, err = meradb("run", "--data", fresh, demo, "--local", env=env)
            check("--local: no note", "LOCAL mode" not in err and "Transaction WAPAS" in out, err)
        finally:
            shutil.rmtree(fresh, ignore_errors=True)
        code, out, err = meradb("run", "--data", data, demo, "--port", env["MERADB_PORT"])
        check("explicit --port: no fallback", code == 1 and "par MeraDB server nahi mila" in err, out + err)
    finally:
        shutil.rmtree(data, ignore_errors=True)


def scenario_data_lock_message():
    """A folder served by a running server must not be opened directly."""
    data, port = tempfile.mkdtemp(), str(free_port())
    try:
        meradb("start", "--data", data, "--port", port)
        script = os.path.join(data, "t.mdb")
        with open(script, "w", encoding="utf-8") as f:
            f.write("DIKHAO TABLES;")
        code, out, err = meradb("run", script, "--local", "--data", data)
        check("--local on a served folder is refused", code == 1 and "Is data folder par MeraDB server chal raha hai" in err, out + err)
    finally:
        meradb("stop", "--data", data, "--force")
        shutil.rmtree(data, ignore_errors=True)


for scenario in (scenario_lifecycle, scenario_password_and_force, scenario_local_fallback, scenario_data_lock_message):
    scenario()
print("FAILED: " + ", ".join(failures) if failures else "ALL PASSED")
sys.exit(1 if failures else 0)
```

- [ ] **Step 2: Add `test_cli.cpp` to `cpp/tests/CMakeLists.txt`; verify failure**

```bash
cmake --build cpp/build
```
Expected: FAIL — `meradb/cli.h: No such file or directory`.

- [ ] **Step 3: Header** — `cpp/include/meradb/cli.h`

```cpp
// cpp/include/meradb/cli.h
//
// The `meradb` command line (mirrors meradb/cli.py):
//
//   meradb start | stop | status | server | run FILE... | shell | workbench
//
// `meradb x.mdb` means `run`, `meradb --tui` means `workbench`, and no command
// means `shell`. shell and workbench arrive in a later phase; here they say so.
#pragma once
#include "meradb/backend.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace meradb {

struct CliArgs {
    std::string command;  // server start stop status shell workbench run
    std::string dataDir;
    // Server commands: where to listen. Client commands: where to connect; unset = not given.
    std::optional<std::string> host;
    std::optional<int> port;
    std::optional<std::string> password;  // server / start: --password VALUE
    bool askPassword = false;             // client commands and stop / status: -W
    bool verbose = false;
    bool force = false;
    bool local = false;
    std::optional<std::string> database;
    std::optional<std::string> user;
    std::vector<std::string> files;
    bool showHelp = false;
    bool showVersion = false;
    std::string error;  // non-empty: the arguments were bad (exit code 2, like argparse)
};

CliArgs parseCliArgs(std::vector<std::string> argv);

// Opens the backend a client command talks to. Prints side notes on stderr,
// exactly as Python does. Throws MeraDBError.
std::unique_ptr<Backend> openBackend(const CliArgs& args);

// Runs one script file against `backend`, printing each result followed by a
// blank line. Returns false if the file could not be read or any statement failed.
bool runFile(Backend& backend, const std::string& path);

int cliMain(std::vector<std::string> argv);

}  // namespace meradb
```

- [ ] **Step 4: Implementation** — `cpp/src/cli.cpp`

```cpp
// cpp/src/cli.cpp -- see cli.h.
#include "meradb/cli.h"
#include "meradb/ast.h"
#include "meradb/cli_format.h"
#include "meradb/client.h"
#include "meradb/errors.h"
#include "meradb/protocol.h"
#include "meradb/server.h"
#include "meradb/server_control.h"
#include "meradb/sys_compat.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

namespace meradb {
namespace {

const std::set<std::string> kCommands = {"server", "start", "stop", "status", "shell", "workbench", "tui", "run"};

void note(const std::string& message) { std::cerr << message << "\n"; }

std::optional<int> parseInt(const std::string& text) {
    if (text.empty()) return std::nullopt;
    std::size_t used = 0;
    try {
        int value = std::stoi(text, &used);
        if (used != text.size()) return std::nullopt;
        return value;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

int envPort() {
    auto text = sys::getEnv("MERADB_PORT");
    if (text && !text->empty())
        if (auto value = parseInt(*text)) return *value;
    return protocol::kDefaultPort;
}

const char* kUsage =
    "usage: meradb [-h] [--version] COMMAND ...\n"
    "\n"
    "MeraDB -- apna database, apni bhasha.\n"
    "\n"
    "commands:\n"
    "  server       server isi terminal mein chalao (Ctrl+C se band)\n"
    "  start        server background mein chalao\n"
    "  stop         background server band karo\n"
    "  status       server chal raha hai ya nahi\n"
    "  shell        interactive command-line shell\n"
    "  workbench    full-screen UI (MySQL Workbench jaisa)\n"
    "  run          .mdb script files chalao\n"
    "\n"
    "examples:\n"
    "  meradb start                      server ko background mein chalao\n"
    "  meradb run examples/demo.mdb      script chalao\n"
    "  meradb status / meradb stop\n"
    "  meradb run x.mdb --local          bina server ke, seedha data folder par\n";

}  // namespace

CliArgs parseCliArgs(std::vector<std::string> argv) {
    CliArgs args;
    // shortcuts: `meradb demo.mdb` = run, `meradb --tui` = workbench, `meradb` = shell
    auto endsWithMdb = [](const std::string& s) { return s.size() >= 4 && s.compare(s.size() - 4, 4, ".mdb") == 0; };
    if (!argv.empty() && endsWithMdb(argv[0])) argv.insert(argv.begin(), "run");
    else if (!argv.empty() && argv[0] == "--tui") argv[0] = "workbench";
    if (argv.empty() || (!kCommands.count(argv[0]) && argv[0] != "-h" && argv[0] != "--help" && argv[0] != "--version"))
        argv.insert(argv.begin(), "shell");

    if (argv[0] == "-h" || argv[0] == "--help") {
        args.showHelp = true;
        return args;
    }
    if (argv[0] == "--version") {
        args.showVersion = true;
        return args;
    }
    args.command = argv[0] == "tui" ? "workbench" : argv[0];
    args.dataDir = protocol::defaultDataDir();

    const bool isServer = args.command == "server" || args.command == "start";
    const bool isClient = args.command == "shell" || args.command == "workbench" || args.command == "run";
    const bool isControl = args.command == "stop" || args.command == "status";
    if (isServer) {
        args.host = protocol::kDefaultHost;
        args.port = envPort();
        if (auto pw = sys::getEnv("MERADB_PASSWORD"); pw && !pw->empty()) args.password = *pw;
    }
    if (isClient) {
        if (auto user = sys::getEnv("MERADB_USER"); user && !user->empty()) args.user = *user;
    }

    for (std::size_t i = 1; i < argv.size(); ++i) {
        const std::string& arg = argv[i];
        // "--name=value" is accepted like argparse does
        std::string name = arg, inlineValue;
        bool hasInline = false;
        if (arg.rfind("--", 0) == 0 && arg.find('=') != std::string::npos) {
            name = arg.substr(0, arg.find('='));
            inlineValue = arg.substr(arg.find('=') + 1);
            hasInline = true;
        }
        auto value = [&](std::string& out) -> bool {
            if (hasInline) {
                out = inlineValue;
                return true;
            }
            if (i + 1 >= argv.size()) {
                args.error = "argument " + name + ": expected one argument";
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string text;
        if (name == "-h" || name == "--help") {
            args.showHelp = true;
            return args;
        } else if (name == "-D" || name == "--data") {
            if (!value(text)) return args;
            args.dataDir = text;
        } else if (isServer && name == "--host") {
            if (!value(text)) return args;
            args.host = text;
        } else if (isClient && (name == "-H" || name == "--host")) {
            if (!value(text)) return args;
            args.host = text;
        } else if ((isServer && name == "--port") || (isClient && (name == "-p" || name == "--port"))) {
            if (!value(text)) return args;
            auto number = parseInt(text);
            if (!number) {
                args.error = "argument " + name + ": invalid int value: '" + text + "'";
                return args;
            }
            args.port = *number;
        } else if (isServer && name == "--password") {
            if (!value(text)) return args;
            args.password = text;
        } else if ((isClient || isControl) && (name == "-W" || name == "--password")) {
            args.askPassword = true;
        } else if (isServer && (name == "-v" || name == "--verbose")) {
            args.verbose = true;
        } else if (args.command == "stop" && name == "--force") {
            args.force = true;
        } else if (isClient && (name == "-d" || name == "--database")) {
            if (!value(text)) return args;
            args.database = text;
        } else if (isClient && (name == "-U" || name == "--user")) {
            if (!value(text)) return args;
            args.user = text;
        } else if (isClient && name == "--local") {
            args.local = true;
        } else if (args.command == "run" && (arg.empty() || arg[0] != '-' || arg == "-")) {
            args.files.push_back(arg);
        } else {
            args.error = "unrecognized arguments: " + arg;
            return args;
        }
    }
    if (args.command == "run" && args.files.empty()) args.error = "the following arguments are required: files";
    return args;
}

bool runFile(Backend& backend, const std::string& path) {
    std::ifstream file(std::filesystem::u8path(path), std::ios::binary);
    if (!file) {
        // Same wording as Python's OSError text: "[Errno 2] No such file or directory: 'x'"
        std::string quoted;
        for (char c : path) {
            if (c == '\\' || c == '\'') quoted += '\\';
            quoted += c;
        }
        std::error_code ec;
        bool missing = !std::filesystem::exists(std::filesystem::u8path(path), ec);
        std::cout << "File nahi khuli: "
                  << (missing ? "[Errno 2] No such file or directory: '" : "[Errno 13] Permission denied: '")
                  << quoted << "'\n";
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string text = buffer.str();
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);  // utf-8-sig, like Python

    bool ok = true;
    for (const auto& result : backend.runScript(text)) {
        std::string out = formatResult(result);
        if (!out.empty()) std::cout << out << "\n";
        std::cout << "\n";
        if (!result.error.empty()) ok = false;
    }
    return ok;
}

namespace {

std::optional<std::string> clientPassword(const CliArgs& args) {
    if (args.askPassword) return sys::readHidden("Password: ");
    auto env = sys::getEnv("MERADB_PASSWORD");
    if (env) return env;  // Python: os.environ.get -- an empty value stays "" (which means no password)
    return std::nullopt;
}

std::string absolute(const std::string& path) {
    std::error_code ec;
    auto result = std::filesystem::absolute(std::filesystem::u8path(path), ec);
    return ec ? path : result.lexically_normal().u8string();
}

std::unique_ptr<Backend> openLocal(const CliArgs& args) {
    auto backend = std::make_unique<LocalBackend>(args.dataDir);
    for (const auto& db : backend->engine().instance().recovered())
        note("RECOVERY: database '" + db + "' ka adhoora transaction WAPAS kiya (pichli baar crash hua tha)");
    if (args.database) {
        ast::UseDatabase use;
        use.name = *args.database;
        backend->engine().executeStatement(use);
    }
    return backend;
}

}  // namespace

std::unique_ptr<Backend> openBackend(const CliArgs& args) {
    std::optional<std::string> password = clientPassword(args);
    if (args.local) {
        if (args.user)
            note("(--local mode mein -U/--user '" + *args.user +
                 "' ka koi matlab nahi -- ignore kiya, superuser ki tarah chal raha hai)");
        return openLocal(args);
    }

    // Flags are an explicit choice (no fallback to local mode if that server is down);
    // MERADB_HOST / MERADB_PORT only change the defaults.
    const bool explicitTarget = args.host.has_value() || args.port.has_value();
    std::string host = args.host ? *args.host : std::string();
    if (host.empty()) host = sys::getEnv("MERADB_HOST").value_or("");
    if (host.empty()) host = protocol::kDefaultHost;
    int port = 0;
    if (args.port) port = *args.port;
    else if (auto text = sys::getEnv("MERADB_PORT"); text && !text->empty()) {
        auto number = parseInt(*text);
        if (!number) throw MeraDBError("MERADB_PORT ek number hona chahiye: '" + *text + "'");
        port = *number;
    } else {
        auto info = protocol::readPidFile(absolute(args.dataDir));  // a server on another port for this folder?
        port = (info && info->contains("port")) ? static_cast<int>((*info)["port"].get<std::int64_t>())
                                                : protocol::kDefaultPort;
    }

    ConnectOptions options;
    options.host = host;
    options.port = port;
    if (password && !password->empty()) options.password = password;
    options.database = args.database;
    options.user = args.user;
    try {
        return std::make_unique<Connection>(options);
    } catch (const ServerUnavailable&) {
        if (explicitTarget) throw;
        note("(" + host + ":" + std::to_string(port) + " par server nahi mila -- LOCAL mode: seedha '" +
             absolute(args.dataDir) + "' khol rahe hain. Server ke liye: meradb start)");
        return openLocal(args);
    }
}

namespace {

ControlOptions controlOptions(const CliArgs& args) {
    ControlOptions options;
    options.dataDir = args.dataDir;
    if (args.host) options.host = *args.host;
    if (args.port) options.port = *args.port;
    options.password = args.password;
    options.verbose = args.verbose;
    options.force = args.force;
    if (args.askPassword) options.password = sys::readHidden("Password: ");
    return options;
}

}  // namespace

int cliMain(std::vector<std::string> argv) {
    CliArgs args = parseCliArgs(std::move(argv));
    if (args.showHelp) {
        std::cout << kUsage;
        return 0;
    }
    if (args.showVersion) {
        std::cout << protocol::kServerName << "\n";
        return 0;
    }
    if (!args.error.empty()) {
        std::cerr << "usage: meradb [-h] [--version] COMMAND ...\nmeradb: error: " << args.error << "\n";
        return 2;
    }
    try {
        if (args.command == "server") {
            ServerOptions options;
            options.dataDir = args.dataDir;
            options.host = args.host.value_or(protocol::kDefaultHost);
            options.port = args.port.value_or(protocol::kDefaultPort);
            options.password = args.password.value_or("");
            options.verbose = args.verbose;
            return serve(options);
        }
        if (args.command == "start") return serverStart(controlOptions(args));
        if (args.command == "stop") return serverStop(controlOptions(args));
        if (args.command == "status") return serverStatus(controlOptions(args));
        if (args.command == "run") {
            auto backend = openBackend(args);
            bool allOk = true;
            for (const auto& path : args.files)
                if (!runFile(*backend, path)) allOk = false;  // run ALL files, even after a failure
            backend->close();
            return allOk ? 0 : 1;
        }
        // shell / workbench
        note("`meradb " + args.command + "` abhi C++ version mein nahi hai (aage ke phase mein aayega). "
             "Python version istemal karo, ya scripts ke liye:  meradb run FILE");
        return 1;
    } catch (const MeraDBError& e) {
        note(e.what());
        return 1;
    } catch (const std::exception& e) {
        note(e.what());
        return 1;
    }
}

}  // namespace meradb
```

- [ ] **Step 5: `main.cpp`** — replace the whole file (this supersedes the temporary edit of Task 1):

```cpp
// meradb_cli: the `meradb` command (see cli.h).
#include "meradb/cli.h"
#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    return meradb::cliMain(std::move(args));
}
```

- [ ] **Step 6: CMake and the two ctest hooks**

`cpp/CMakeLists.txt`: add `src/cli.cpp` to `meradb_core`.

`cpp/tests/CMakeLists.txt`, inside the existing `if(Python3_Interpreter_FOUND)` block, add

```cmake
  add_test(NAME cli_lifecycle
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/cli_lifecycle.py $<TARGET_FILE:meradb_cli>)
  set_tests_properties(cli_lifecycle PROPERTIES TIMEOUT 240)
```

`cpp/tests/cross_engine_diff.py`, in `main()`, make the C++ side explicitly local (the CLI now
tries a server first, and a server on 6372 must not change a comparison):
replace `cpp_out, cpp_code = run([str(args.cli), "run", args.script, "--data", cpp_dir])` with
`cpp_out, cpp_code = run([str(args.cli), "run", args.script, "--local", "--data", cpp_dir])`.

- [ ] **Step 7: Build and run**

```bash
cmake -S cpp -B cpp/build -G "MinGW Makefiles"
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "^cli "
ctest --test-dir cpp/build --output-on-failure
```
Expected: 15 `cli` test cases pass, `cli_lifecycle` prints only `PASS` lines and `ALL PASSED`,
and the Phase 1 `cross_engine_*` ctests still `MATCH`. The `Transaction WAPAS` check of the
fallback scenario needs Batch A's triggers/procedures (it runs the full `examples/demo.mdb`).

- [ ] **Step 8: Commit**

```bash
git add cpp/include/meradb/cli.h cpp/src/cli.cpp cpp/src/main.cpp cpp/tests/test_cli.cpp cpp/tests/cli_lifecycle.py cpp/tests/cross_engine_diff.py cpp/CMakeLists.txt cpp/tests/CMakeLists.txt
git commit -m "Add the meradb command line: server, start, stop, status, run with local fallback"
```

**Completion checklist:**
- [ ] `meradb_cli run x.mdb` output for a plain script is byte-identical to Phase 1's (the Phase 1 cross-engine ctests still pass)
- [ ] every note (`RECOVERY:`, fallback, `-U` with `--local`) goes to stderr, results to stdout
- [ ] argparse-style error text and exit code 2 verified for: missing files, bad int, missing value, unknown flag
- [ ] `cli_lifecycle` leaves no server process or temp folder behind (it always runs `stop --force` in `finally`)
- [ ] the default data folder is the per-user one (open question 1 in the hand-off notes)
- [ ] full suite green, zero warnings

---



# BATCH D — verification against Python, and documentation (Tasks 22-25)

Batch D adds no engine features. Its scripts are the acceptance test of the whole phase:
the **full** `examples/demo.mdb` and `examples/rdbms_lab_coverage.mdb` (including the
users, triggers and procedures that Phase 1 had to trim), every client against every
server, and both engines on each other's data folders. When one of these disagrees with
Python, **Python is right** unless the difference is one of the deliberate divergences of
D5 / D7 (then record it in `docs/CPP.md` in Task 25 and make the check tolerate exactly it).

### Task 22: Cross-engine — full example scripts, local and via server

**Files:**
- Modify (replace): `cpp/tests/cross_engine_diff.py`
- Modify: `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `meradb_cli` (`run --local`, `server`, `stop`, `run --port`), the Python engine.
- Produces: `cross_engine_diff.py [--via-server] [--cli PATH] SCRIPT` — exits 0 and prints
  `MATCH (local)` / `MATCH (via server)` when stdout and exit code are identical to
  `python -m meradb run --local`. In `--via-server` mode it starts `meradb_cli server` on a
  free port and a fresh folder, runs `meradb_cli run --port P SCRIPT` against it, fails if the
  client silently fell back to local mode, and always stops the server. `MERADB_*` variables of
  the calling shell are removed from both sides' environments so a user's settings cannot
  change a comparison.

- [x] **Step 1: Replace `cpp/tests/cross_engine_diff.py`** with:

```python
"""
Runs the same .mdb script through the Python engine and the C++ CLI and diffs
their raw output. Exits non-zero (printing a unified diff) on any
mismatch, so it can be wired into a CI-style check.

Modes for the C++ side:
  (default)      `meradb_cli run --local`            the engine inside the CLI process
  --via-server   `meradb_cli server` + `meradb_cli run --port P`
                 the same script, but every statement crosses the wire protocol

The Python side is always `python -m meradb run --local` (the oracle).

Usage:
    python cpp/tests/cross_engine_diff.py examples/demo.mdb
    python cpp/tests/cross_engine_diff.py --via-server examples/demo.mdb
    python cpp/tests/cross_engine_diff.py --cli path/to/meradb_cli SCRIPT.mdb
"""
import argparse
import difflib
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.stdout.reconfigure(encoding="utf-8")  # diffs may contain non-ASCII text; never die on a console code page


def default_cli() -> Path:
    build = REPO_ROOT / "cpp" / "build"
    for folder in (build, build / "Release"):
        for name in ("meradb_cli.exe", "meradb_cli"):
            if (folder / name).exists():
                return folder / name
    return REPO_ROOT / "cpp" / "build" / "meradb_cli"


def child_env() -> dict:
    env = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_")}  # no user settings leak in
    env.update(PYTHONIOENCODING="utf-8", PYTHONPATH=str(REPO_ROOT))
    return env


def run(cmd: list[str]) -> tuple[str, str, int]:
    result = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, env=child_env())
    return (result.stdout.decode("utf-8").replace("\r\n", "\n"),
            result.stderr.decode("utf-8").replace("\r\n", "\n"), result.returncode)


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for_port(port: int, seconds: float = 15.0) -> bool:
    deadline = time.time() + seconds
    while time.time() < deadline:
        with socket.socket() as s:
            s.settimeout(0.3)
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return True
        time.sleep(0.1)
    return False


class CppServer:
    """`meradb_cli server` in the foreground of a child process, stopped again on exit."""

    def __init__(self, cli: Path, data: str):
        self.port = free_port()
        self.data = data
        self.cli = cli
        self.proc = subprocess.Popen([str(cli), "server", "--data", data, "--port", str(self.port)], cwd=REPO_ROOT,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=child_env())

    def __enter__(self):
        if not wait_for_port(self.port):
            self.proc.kill()
            raise SystemExit("C++ server did not start")
        return self

    def __exit__(self, *exc):
        subprocess.run([str(self.cli), "stop", "--data", self.data], cwd=REPO_ROOT, capture_output=True, env=child_env(),
                       timeout=30)
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("script")
    parser.add_argument("--cli", type=Path, default=default_cli())
    parser.add_argument("--via-server", action="store_true", help="run the C++ side through a C++ server")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as py_dir, tempfile.TemporaryDirectory() as cpp_dir:
        py_out, _, py_code = run([sys.executable, "-m", "meradb", "run", "--local", "--data", py_dir, args.script])
        if args.via_server:
            with CppServer(args.cli, cpp_dir) as server:
                cpp_out, cpp_err, cpp_code = run([str(args.cli), "run", "--port", str(server.port), args.script])
            if "LOCAL mode" in cpp_err:
                print("MISMATCH: the C++ client fell back to local mode instead of using the server")
                return 1
        else:
            cpp_out, _, cpp_code = run([str(args.cli), "run", args.script, "--local", "--data", cpp_dir])

    label = "via server" if args.via_server else "local"
    # Compare raw text (CRLF already normalised) so trailing-newline differences are not hidden.
    if py_out == cpp_out and py_code == cpp_code:
        print(f"MATCH ({label}): {args.script} ({len(py_out.splitlines())} lines identical, exit={py_code})")
        return 0

    print(f"MISMATCH ({label}): {args.script} (exit python={py_code}, cpp={cpp_code})")
    diff = difflib.unified_diff(py_out.splitlines(keepends=True), cpp_out.splitlines(keepends=True),
                                fromfile="python", tofile="cpp")
    print("".join(diff) or f"(only trailing whitespace differs) python={py_out[-20:]!r} cpp={cpp_out[-20:]!r}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
```

- [x] **Step 2: The trimmed Phase 1 scripts must still match, in both modes**

```bash
python cpp/tests/cross_engine_diff.py --cli cpp/build/meradb_cli.exe cpp/tests/demo_phase1.mdb
python cpp/tests/cross_engine_diff.py --cli cpp/build/meradb_cli.exe --via-server cpp/tests/demo_phase1.mdb
python cpp/tests/cross_engine_diff.py --cli cpp/build/meradb_cli.exe cpp/tests/rdbms_lab_coverage_phase1.mdb
python cpp/tests/cross_engine_diff.py --cli cpp/build/meradb_cli.exe --via-server cpp/tests/rdbms_lab_coverage_phase1.mdb
```
Expected: four `MATCH` lines (548 and 556 lines identical, `exit=1` — the scripts contain
deliberate errors). The `--via-server` results are the proof that every value type survives
the JSON codec and that the server's session behaves like an embedded engine.

- [x] **Step 3: The full examples**

```bash
python cpp/tests/cross_engine_diff.py --cli cpp/build/meradb_cli.exe examples/demo.mdb
python cpp/tests/cross_engine_diff.py --cli cpp/build/meradb_cli.exe --via-server examples/demo.mdb
python cpp/tests/cross_engine_diff.py --cli cpp/build/meradb_cli.exe examples/rdbms_lab_coverage.mdb
python cpp/tests/cross_engine_diff.py --cli cpp/build/meradb_cli.exe --via-server examples/rdbms_lab_coverage.mdb
```
Expected: four `MATCH` lines. A `MISMATCH` prints a unified diff; triage in this order:
(1) a message that differs by a word — a spelling slip in Task 8-11; (2) a row order or a
value — a trigger firing order or a substitution bug (Tasks 9-10); (3) a diff that appears
*only* in `--via-server` — the wire codec (Task 14) or session state (Task 16); (4) `[Internal
Galti]` — an exception type the server did not expect: fix the source, then add a regression
test next to it. Fix the C++ code, never the expected output.

- [x] **Step 4: ctest registration** — in `cpp/tests/CMakeLists.txt`, inside the existing
`if(Python3_Interpreter_FOUND)` block, after the `cross_engine_missing_file` test, add:

```cmake
  set(_examples ${CMAKE_CURRENT_SOURCE_DIR}/../../examples)
  foreach(_script demo rdbms_lab_coverage)
    add_test(NAME cross_engine_full_${_script}
      COMMAND ${Python3_EXECUTABLE} ${_diff} --cli $<TARGET_FILE:meradb_cli> ${_examples}/${_script}.mdb)
    add_test(NAME cross_engine_server_${_script}
      COMMAND ${Python3_EXECUTABLE} ${_diff} --cli $<TARGET_FILE:meradb_cli> --via-server ${_examples}/${_script}.mdb)
  endforeach()
  foreach(_script demo_phase1 rdbms_lab_coverage_phase1)
    add_test(NAME cross_engine_server_${_script}
      COMMAND ${Python3_EXECUTABLE} ${_diff} --cli $<TARGET_FILE:meradb_cli> --via-server
              ${CMAKE_CURRENT_SOURCE_DIR}/${_script}.mdb)
  endforeach()
  set_tests_properties(cross_engine_server_demo cross_engine_server_rdbms_lab_coverage
                       cross_engine_server_demo_phase1 cross_engine_server_rdbms_lab_coverage_phase1
                       PROPERTIES TIMEOUT 240)
```

- [x] **Step 5: Run and commit**

```bash
cmake -S cpp -B cpp/build -G "MinGW Makefiles"
ctest --test-dir cpp/build --output-on-failure -R cross_engine
git add cpp/tests/cross_engine_diff.py cpp/tests/CMakeLists.txt
git commit -m "Compare the full example scripts with Python, locally and through the server"
```
Expected: 3 (Phase 1) + 4 + 4 `cross_engine_*` tests pass.

**Completion checklist:**
- [x] all eight comparisons print `MATCH`
- [x] `git diff <base> -- examples` is empty (the examples were not edited to make them pass)
- [x] no leftover server process after the run (`tasklist | findstr meradb`)
- [x] full suite green

---

### Task 23: Interop matrix — Python and C++ clients x servers

**Files:**
- Modify: `cpp/src/tokenizer.cpp`
- Create: `cpp/tests/test_tokenizer_unicode.cpp`, `cpp/tests/interop_check.py`
- Modify: `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `meradb_cli` as client and server, `python -m meradb` as client and server,
  Python's `UserStore` and `Engine` to prepare data.
- Produces: `interop_check.py --cli PATH [script ...]`, three groups of checks:
  1. **scripts** — each script through all four (client, server) pairs; the three pairs other
     than (Python, Python) must equal the baseline byte for byte;
  2. **raw protocol** — the same message lists sent to a Python server and a C++ server, every
     reply line compared (pid, data folder, start time and the wording of a JSON syntax error
     are normalised): `basics`, `queries` (unicode, an astral character, DEL, floats, dates,
     NULLs, errors, aggregates, schema, status), `bad messages`, `transaction`, hello
     variants, first-message-not-hello, a database in `hello`, an unknown one, unicode in
     values and in error positions, and the shared password (none / wrong / right);
  3. **logins** — users created by the *Python* `UserStore` in the data folder of both servers
     (so the C++ server reads a Python-written `users.json`), logging in over the wire and being
     held to their grants, including a wrong password, an unknown user, and a restricted user
     asking for a database in `hello`.
- **A real Phase 1 bug this matrix found while it was being written** (fixed in Steps 1-3):
  the tokenizer counted UTF-8 **bytes** for `col` (Python counts characters) and printed only
  the first byte of an unexpected non-ASCII character. Any parse error after a non-ASCII string
  literal on the same line therefore reported a different column than Python. Fixing it
  changes no other behaviour. **Not fixed, deliberately** (recorded in Task 25): Python treats
  every Unicode letter as an identifier character (`str.isalpha`), so `BANAO TABLE café` works in
  Python; the C++ tokenizer accepts ASCII letters only and rejects such a name. Equivalent
  behaviour needs Unicode category tables (ICU), which the project does not depend on.

- [x] **Step 1: Write the failing tokenizer tests** — `cpp/tests/test_tokenizer_unicode.cpp`
(add it to `cpp/tests/CMakeLists.txt`):

```cpp
// cpp/tests/test_tokenizer_unicode.cpp -- columns and error characters for non-ASCII source text.
// Found by the interop matrix (Task 23): Python counts characters, Phase 1 counted UTF-8 bytes.
#include <catch2/catch_test_macros.hpp>
#include "meradb/errors.h"
#include "meradb/tokenizer.h"
#include <string>

using namespace meradb;

namespace {
std::string errorOf(const std::string& source) {
    try {
        tokenize(source);
    } catch (const TokenizerError& e) {
        return e.message();
    }
    return "(no error)";
}
}  // namespace

TEST_CASE("tokenizer_unicode columns after a multi-byte string count characters", "[tokenizer][unicode]") {
    // 'caf<e-acute>' @  -> quote, c, a, f, e-acute, quote, space, @ : the @ is character 8 (bytes would say 9)
    CHECK(errorOf("'caf\xC3\xA9' @") == "Ye character samajh nahi aaya: '@' (line 1, col 8)");
    // two 4-byte characters (emoji) are two columns, not eight
    CHECK(errorOf("'\xF0\x9F\x98\x80\xF0\x9F\x98\x80' @") == "Ye character samajh nahi aaya: '@' (line 1, col 6)");
    // a line break resets the column as before
    CHECK(errorOf("'\xC3\xA9'\n  @") == "Ye character samajh nahi aaya: '@' (line 2, col 3)");
}

TEST_CASE("tokenizer_unicode an unexpected non-ASCII character is shown whole", "[tokenizer][unicode]") {
    CHECK(errorOf("DIKHAO \xC2\xA3") == "Ye character samajh nahi aaya: '\xC2\xA3' (line 1, col 8)");        // pound sign
    CHECK(errorOf("x \xE2\x82\xAC") == "Ye character samajh nahi aaya: '\xE2\x82\xAC' (line 1, col 3)");     // euro sign
    CHECK(errorOf("\xF0\x9F\x98\x80") == "Ye character samajh nahi aaya: '\xF0\x9F\x98\x80' (line 1, col 1)");  // emoji
}

TEST_CASE("tokenizer_unicode non-ASCII text inside strings is kept byte for byte", "[tokenizer][unicode]") {
    auto tokens = tokenize("'caf\xC3\xA9 \xE4\xB8\x96\xE7\x95\x8C'");
    REQUIRE(tokens.size() == 2);
    CHECK(tokens[0].textValue == "caf\xC3\xA9 \xE4\xB8\x96\xE7\x95\x8C");
    CHECK(tokens[0].col == 1);
}
```

- [x] **Step 2: Verify they fail**

```bash
cmake --build cpp/build && ctest --test-dir cpp/build --output-on-failure -R tokenizer_unicode
```
Expected: FAIL (`col 9` instead of `col 8`; `'\xC2'` instead of the whole character).

- [x] **Step 3: Fix `cpp/src/tokenizer.cpp`**

3a. In `Tokenizer::advance()` replace
```cpp
    if (c == '\n') { ++line_; col_ = 1; } else { ++col_; }
```
with
```cpp
    if (c == '\n') { ++line_; col_ = 1; }
    else if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++col_;  // count characters, not UTF-8 bytes (Python's str)
```
3b. In `Tokenizer::tokenize()`, in the branch that reports an unknown character, replace
```cpp
                    std::string shown = (c == '\\') ? std::string("'\\\\'") : std::string("'") + c + "'";
```
with
```cpp
                    // A non-ASCII character is shown whole (all of its UTF-8 bytes), as Python's repr does for
                    // printable characters; escaping of unprintable ones (e.g. '\xa0') is not reproduced.
                    unsigned char lead = static_cast<unsigned char>(c);
                    size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
                    std::string character = text_.substr(pos_, length);
                    std::string shown = (c == '\\') ? std::string("'\\\\'") : "'" + character + "'";
```

- [x] **Step 4: Build and run**

```bash
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure -R "tokenizer|golden|parse"
```
Expected: the 3 new test cases pass and every earlier tokenizer/parser/golden test still
passes (the golden scripts were recorded from Python, so any of them that contain a
non-ASCII string before an error now match even more closely).

- [x] **Step 5: Write the matrix** — `cpp/tests/interop_check.py`

```python
"""
Interop matrix: every client against every server.

                    Python server        C++ server
    Python client   (the baseline)       C++ server must behave like Python's
    C++ client      C++ client must work against Python's server

1. SCRIPTS: each script is run through all four client/server pairs with
   `run --port P`; the output of the three other pairs must equal the
   (Python client, Python server) baseline byte for byte.
2. RAW PROTOCOL: the same list of messages is sent to both servers over a plain
   socket and every reply line is compared (after hiding the parts that
   legitimately differ: pid, data folder, start time, and the wording of a
   JSON syntax error).
3. LOGINS: users made with the PYTHON `UserStore` in the data folder of both
   servers -- so the C++ server is reading a Python-written `users.json` --
   log in over the wire and are held to their grants.

Usage:
    python cpp/tests/interop_check.py --cli path/to/meradb_cli [script.mdb ...]
With no script arguments it uses examples/demo.mdb and
examples/rdbms_lab_coverage.mdb. Exit code 0 = everything matched.
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.stdout.reconfigure(encoding="utf-8")  # diffs may contain non-ASCII text; never die on a console code page
sys.path.insert(0, str(REPO_ROOT))


def env() -> dict:
    e = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_")}
    e.update(PYTHONIOENCODING="utf-8", PYTHONPATH=str(REPO_ROOT))
    return e


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def port_open(port: int) -> bool:
    with socket.socket() as s:
        s.settimeout(0.3)
        return s.connect_ex(("127.0.0.1", port)) == 0


class Server:
    """A foreground server process of the given kind ("python" or "cpp") on a fresh data folder."""

    def __init__(self, kind: str, cli: str, data: str, password: str | None = None):
        self.kind, self.cli, self.data, self.port = kind, cli, data, free_port()
        base = ([sys.executable, "-m", "meradb"] if kind == "python" else [cli])
        command = base + ["server", "--data", data, "--port", str(self.port)]
        e = env()
        if password:
            e["MERADB_PASSWORD"] = password
        self.proc = subprocess.Popen(command, cwd=REPO_ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=e)
        deadline = time.time() + 20
        while time.time() < deadline and not port_open(self.port):
            if self.proc.poll() is not None:
                raise SystemExit(f"{kind} server exited early")
            time.sleep(0.1)
        if not port_open(self.port):
            raise SystemExit(f"{kind} server did not start")

    def stop(self):
        try:
            with socket.create_connection(("127.0.0.1", self.port), timeout=3) as s:
                f = s.makefile("rwb")
                f.write(b'{"type": "hello", "version": 1, "password": null, "database": null, "user": null}\n')
                f.flush()
                f.readline()
                f.write(b'{"type": "shutdown"}\n')
                f.flush()
                f.readline()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()


def run_client(kind: str, cli: str, port: int, script: str, extra_env: dict | None = None) -> tuple[str, int]:
    base = ([sys.executable, "-m", "meradb"] if kind == "python" else [cli])
    e = env()
    e.update(extra_env or {})
    r = subprocess.run(base + ["run", "--port", str(port), script], cwd=REPO_ROOT, capture_output=True, env=e, timeout=300)
    return r.stdout.decode("utf-8").replace("\r\n", "\n"), r.returncode


# ---------------------------------------------------------------- 1. scripts

def check_scripts(cli: str, scripts: list[str]) -> list[str]:
    failures = []
    for script in scripts:
        results = {}
        for server_kind in ("python", "cpp"):
            for client_kind in ("python", "cpp"):
                with tempfile.TemporaryDirectory() as data, Server(server_kind, cli, data) as server:
                    results[(client_kind, server_kind)] = run_client(client_kind, cli, server.port, script)
        baseline = results[("python", "python")]
        for pair, got in results.items():
            name = f"{Path(script).name}: {pair[0]} client -> {pair[1]} server"
            if pair == ("python", "python"):
                continue
            if got == baseline:
                print(f"PASS {name} ({len(got[0].splitlines())} lines)")
            else:
                print(f"FAIL {name}")
                failures.append(name)
                import difflib
                print("".join(list(difflib.unified_diff(baseline[0].splitlines(keepends=True), got[0].splitlines(keepends=True),
                                                        "python->python", f"{pair[0]}->{pair[1]}"))[:60]))
    return failures


# ---------------------------------------------------------------- 2. raw protocol

HELLO = {"type": "hello", "version": 1, "password": None, "database": None, "user": None}


def dumps(message) -> bytes:
    return (json.dumps(message) + "\n").encode("utf-8")


def normalise(reply: bytes) -> str:
    text = reply.decode("utf-8", "replace").rstrip("\n")
    text = re.sub(r'"pid": \d+', '"pid": 0', text)
    text = re.sub(r'"started": "[^"]*"', '"started": "T"', text)
    text = re.sub(r'"data_dir": "(?:[^"\\]|\\.)*"', '"data_dir": "D"', text)
    text = re.sub(r'\[Protocol Galti\] Galat message: .*?"\}', '[Protocol Galti] Galat message: X"}', text)  # json's own wording
    return text


def exchange(port: int, lines: list, timeout: float = 5.0) -> list[str]:
    """Sends each item (dict -> JSON line, bytes -> raw) and collects one reply line per item ('' = closed)."""
    replies = []
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        f = s.makefile("rwb")
        for item in lines:
            f.write(item if isinstance(item, bytes) else dumps(item))
            f.flush()
            replies.append(normalise(f.readline()))
    return replies


def query(text: str) -> dict:
    return {"type": "query", "text": text}


PROTOCOL_SCENARIOS = {
    "basics": [HELLO, {"type": "ping"}, {"type": "status"}, {"type": "nope"}, {"type": "schema"}],
    "queries": [HELLO,
                query("BANAO TABLE t (id INT MUKHYA KUNJI, name TEXT, score FLOAT, born DATE, ok BOOL)"),
                query("DAALO MEIN t MAAN (1, 'café 世界 \U0001F600', 1.5, '2024-02-29', SAHI), "
                      "(2, NULL, 100000000000000000000.0, NULL, GALAT), (3, 'del\x7f', 0.00001, '1999-12-31', NULL)"),
                query("DIKHAO * SE t ORDER BY id"),
                query("DIKHAO id, score * 2 SE t; DIKHAO * SE gayab; DIKHAO 1 / 0"),
                query("DIKHAO COUNT(*), AVG(score) SE t"),
                {"type": "schema"}, {"type": "status"}],
    "bad messages": [HELLO, b"not json at all\n", b"[1, 2, 3]\n", b'{"type": "query"}\n', b'{"type": "query", "text": 5}\n',
                     b'{"type": "query", "text": "DIKHAO TABLES"}\n', b"\n", {"type": "ping"}],
    "transaction": [HELLO, query("BANAO TABLE a (x INT); SHURU; DAALO MEIN a MAAN (1)"), query("DIKHAO * SE a"),
                    query("WAPAS"), query("DIKHAO * SE a")],
    "hello variants": [{"type": "hello"}],
    "first message not hello": [{"type": "ping"}],
    "database in hello": [dict(HELLO, database="main"), {"type": "status"}],
    "unknown database in hello": [dict(HELLO, database="nosuchdb")],
    "unicode in values and in error positions": [
        HELLO, query("BANAO TABLE u (name TEXT)"),
        query("DAALO MEIN u MAAN ('यूज़र 😀'); DIKHAO * SE u"),
        query("DAALO MEIN u MAAN ('café') @"),        # the error column counts characters, not bytes
        query("DIKHAO * SE u JAHAN name = 'é' £")],  # an unexpected non-ASCII character is shown whole
    "shutdown from loopback is refused for the wrong first message": [{"type": "shutdown"}],
}


def check_protocol(cli: str) -> list[str]:
    failures = []
    for name, lines in PROTOCOL_SCENARIOS.items():
        got = {}
        for kind in ("python", "cpp"):
            with tempfile.TemporaryDirectory() as data, Server(kind, cli, data) as server:
                try:
                    got[kind] = exchange(server.port, lines)
                except (OSError, ValueError) as e:
                    got[kind] = [f"<{type(e).__name__}>"]
        label = f"protocol: {name}"
        if got["python"] == got["cpp"]:
            print(f"PASS {label}")
        else:
            print(f"FAIL {label}")
            failures.append(label)
            for i, (a, b) in enumerate(zip(got["python"], got["cpp"])):
                if a != b:
                    print(f"  message {i}:\n    python: {a[:400]}\n    cpp:    {b[:400]}")
            if len(got["python"]) != len(got["cpp"]):
                print(f"  reply counts differ: python={len(got['python'])} cpp={len(got['cpp'])}")
    # a shared password
    got = {}
    for kind in ("python", "cpp"):
        with tempfile.TemporaryDirectory() as data, Server(kind, cli, data, password="sekrit") as server:
            got[kind] = (exchange(server.port, [HELLO]), exchange(server.port, [dict(HELLO, password="wrong")]),
                         exchange(server.port, [dict(HELLO, password="sekrit"), {"type": "ping"}]))
    label = "protocol: shared password (none / wrong / right)"
    if got["python"] == got["cpp"]:
        print(f"PASS {label}")
    else:
        print(f"FAIL {label}\n  python: {got['python']}\n  cpp:    {got['cpp']}")
        failures.append(label)
    return failures


# ---------------------------------------------------------------- 3. logins with Python-written users.json

def check_logins(cli: str) -> list[str]:
    from meradb.engine import Engine
    from meradb.users import UserStore

    failures = []
    got = {}
    for kind in ("python", "cpp"):
        with tempfile.TemporaryDirectory() as data:
            engine = Engine(data)
            engine.execute("BANAO TABLE staff (id INT, name TEXT); DAALO MEIN staff MAAN (1, 'a'), (2, 'b'); "
                           "BANAO TABLE secret (x INT); DAALO MEIN secret MAAN (9)")
            engine.close()
            users = UserStore(data)  # the Python module writes users.json; the C++ server must read it
            users.create("asha", "pw-é")
            users.grant("asha", "main", "staff", ["DIKHAO", "DAALO"])
            with Server(kind, cli, data) as server:
                login = dict(HELLO, user="asha", password="pw-é")
                got[kind] = [
                    exchange(server.port, [login, query("DIKHAO * SE staff"), query("DAALO MEIN staff MAAN (3, 'c')"),
                                           query("DIKHAO * SE secret"), query("MITAO SE staff"),
                                           query("BANAO TABLE z (x INT)"), query("DIKHAO TABLES")]),
                    exchange(server.port, [dict(HELLO, user="asha", password="wrong")]),
                    exchange(server.port, [dict(HELLO, user="ghost", password="x")]),
                    exchange(server.port, [dict(login, database="main")]),
                ]
    label = "logins: Python-written users.json, grants enforced"
    if got["python"] == got["cpp"]:
        print(f"PASS {label}")
    else:
        print(f"FAIL {label}")
        failures.append(label)
        for i, (a, b) in enumerate(zip(got["python"], got["cpp"])):
            if a != b:
                print(f"  scenario {i}:\n    python: {a}\n    cpp:    {b}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scripts", nargs="*")
    parser.add_argument("--cli", required=True)
    args = parser.parse_args()
    scripts = args.scripts or [str(REPO_ROOT / "examples" / "demo.mdb"), str(REPO_ROOT / "examples" / "rdbms_lab_coverage.mdb")]
    failures = check_scripts(args.cli, scripts) + check_protocol(args.cli) + check_logins(args.cli)
    print("ALL MATCHED" if not failures else "FAILED: " + "; ".join(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [x] **Step 6: Run it**

```bash
python cpp/tests/interop_check.py --cli cpp/build/meradb_cli.exe
```
Expected: 6 `PASS` lines for the two scripts (three non-baseline pairs each), 11 `PASS` lines for
the raw-protocol scenarios, `PASS logins: ...`, then `ALL MATCHED`. Interpreting a failure:
a raw-protocol diff shows both replies for the first differing message — compare them with the
Python source of that reply (`meradb/server.py`); a scripts diff that exists only for
(Python client, C++ server) is a wire-codec or session problem, only for (C++ client, Python
server) is a client problem (`client.cpp`), only for (C++, C++) is both.

- [x] **Step 7: ctest registration** — in `cpp/tests/CMakeLists.txt`, in the Python block:

```cmake
  add_test(NAME interop_matrix
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/interop_check.py --cli $<TARGET_FILE:meradb_cli>)
  set_tests_properties(interop_matrix PROPERTIES TIMEOUT 900)
```

- [x] **Step 8: Commit**

```bash
git add cpp/src/tokenizer.cpp cpp/tests/test_tokenizer_unicode.cpp cpp/tests/interop_check.py cpp/tests/CMakeLists.txt
git commit -m "Add the client/server interop matrix; count tokenizer columns in characters"
```

**Completion checklist:**
- [x] `ALL MATCHED` from `interop_check.py` with the two full example scripts
- [x] the matrix ran with a Python server AND a C++ server for every group (no group is C++-only)
- [x] the tokenizer fix has its own tests and no golden script changed
- [x] the non-ASCII identifier divergence is written down for Task 25
- [x] full suite green, zero warnings

---

### Task 24: Data-folder interchange and the trigger/procedure differential fuzz

**Files:**
- Create: `cpp/tests/interchange_check.py`, `cpp/tests/fuzz_triggers.py`
- Modify: `cpp/tests/CMakeLists.txt`

**Interfaces:**
- Consumes: `meradb_cli`, the Python engine and `UserStore`.
- Produces:
  - `interchange_check.py --cli PATH`: all 8 chains (builder, runner, verifier) over the two
    engines share ONE data folder in turn. The build script creates tables, three triggers,
    two procedures, two users and grants; the use script fires the triggers, hits a failing
    `PEHLE` guard, calls procedures (right and wrong argument counts, and a missing one),
    drops a trigger; the verify script continues and drops a procedure and a user. Every chain
    must print exactly what the all-Python chain printed and end with the same `catalog.json`
    (compared as ordered JSON, so key order counts) and an equivalent `users.json` (same users
    and grants; each record's keys exactly `salt`, `hash`, `grants` in that order; 32-hex salt,
    64-hex hash; Python's `UserStore.verify` accepts the right password, rejects a wrong one,
    and no longer knows the dropped user).
  - `fuzz_triggers.py [--cli PATH] [--seeds N] [--start S] [--seed N] [--show] [--stats]`:
    a deterministic (seeded) generator of *valid* scripts around a fixed schema
    `acct(id PK, name, bal)`, `log(kind, id, a, b)`, `ctr(n)`: 1-5 random triggers (timing x
    event, bodies written from templates that only touch *other* tables, some with a division
    guard that fails when `NAYA.bal < 0`), up to three procedures (two parameters, a `TEXT`
    parameter, none), then 10-40 random statements (inserts including duplicate keys, updates,
    deletes, upserts, procedure calls with occasionally the wrong argument count, dropping a
    random trigger, creating a late one, selects). Each script runs through both engines with
    `--local`; output and exit code must be identical. A failing seed saves its script to the
    temp folder and prints a diff; `--seed N --show` reproduces it.
  - The generator was checked against the Python engine before this plan was written: over 250
    seeds, 8,958 results, 9 % errors (the intended ones), about 4,800 trigger/procedure log
    lines in the final dumps, and **no** internal error (`--stats` re-runs this check).
- What the generator deliberately avoids: a trigger writing to its own table (Python's
  `RecursionError` against the C++ nesting cap of 32 is a documented divergence, not a bug to
  chase), non-ASCII identifiers (documented divergence), and the scalar functions of
  Phase 3+.

- [x] **Step 1: Write the interchange check** — `cpp/tests/interchange_check.py`

```python
"""
Data-folder interchange: a folder written by one engine must be usable by the other.

Every combination (builder, runner, verifier) of the two engines works on ONE data folder in turn:
  1. builder  runs BUILD  (tables, triggers, procedures, users, grants)
  2. runner   runs USE    (fires the triggers, calls the procedures, hits the guards)
  3. verifier runs VERIFY (more of the same, drops a procedure and a user)
All 8 chains must print exactly what the all-Python chain printed, and end with the same
`catalog.json` (same content and key order) and equivalent `users.json` (same users and grants,
salts/hashes of the right shape, and passwords that Python's UserStore verifies).

    python cpp/tests/interchange_check.py --cli path/to/meradb_cli
"""
import argparse
import itertools
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.stdout.reconfigure(encoding="utf-8")  # diffs may contain non-ASCII text; never die on a console code page
sys.path.insert(0, str(REPO_ROOT))

BUILD = """
BANAO TABLE accounts (id INT MUKHYA KUNJI, naam TEXT, balance INT);
BANAO TABLE audit (naam TEXT, purana INT, naya INT);
BANAO TABLE ledger (note TEXT, amount INT);
DAALO MEIN accounts MAAN (1, 'Ravi', 1000), (2, 'café 世界', 50);
BANAO TRIGGER t_audit BAAD BADLO PAR accounts SHURU
    DAALO MEIN audit MAAN (NAYA.naam, PURANA.balance, NAYA.balance);
KHATAM;
BANAO TRIGGER t_guard PEHLE BADLO PAR accounts SHURU
    DAALO MEIN ledger MAAN ('guard', 1 / (AGAR NAYA.balance < 0 TAB 0 WARNA 1 KHATAM));
KHATAM;
BANAO TRIGGER t_ins BAAD DAALO PAR accounts SHURU
    DAALO MEIN ledger MAAN ('new', NAYA.id);
    DIKHAO * SE ledger;
KHATAM;
BANAO PROCEDURE jama (p_id INT, p_kitna INT) SHURU
    BADLO accounts RAKHO balance = balance + p_kitna JAHAN id = p_id;
    DAALO MEIN ledger MAAN ('jama', p_kitna);
KHATAM;
BANAO PROCEDURE naam_badlo (p_id INT, p_naam TEXT) SHURU
    BADLO accounts RAKHO naam = p_naam JAHAN id = p_id;
KHATAM;
BANAO USER asha GUPT 'pw-sécret';
BANAO USER ravi GUPT 'x';
ADHIKAR DO DIKHAO, DAALO PAR accounts KO asha;
ADHIKAR DO SAB PAR ledger KO ravi;
ADHIKAR WAPAS DAALO PAR accounts SE asha;
"""

USE = """
BADLO accounts RAKHO balance = balance - 100 JAHAN id = 1;
BADLO accounts RAKHO balance = balance - 5000 JAHAN id = 2;
CHALAO jama(1, 250);
CHALAO naam_badlo(2, 'Priya O''Neil');
CHALAO jama(9);
CHALAO nahi_hai(1);
DAALO MEIN accounts MAAN (3, 'Meena', 10);
MITAO SE accounts JAHAN id = 3;
HATAO TRIGGER t_ins;
DAALO MEIN accounts MAAN (4, 'Zed', 1);
DIKHAO * SE accounts KRAM id;
DIKHAO * SE audit;
DIKHAO * SE ledger;
"""

VERIFY = """
CHALAO jama(2, 1);
DIKHAO * SE accounts KRAM id;
DIKHAO * SE audit;
HATAO PROCEDURE naam_badlo;
CHALAO naam_badlo(1, 'x');
HATAO USER ravi;
BANAO TRIGGER t_ins BAAD DAALO PAR accounts SHURU
    DAALO MEIN ledger MAAN ('again', NAYA.id);
KHATAM;
DIKHAO * SE ledger;
"""


def env() -> dict:
    e = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_")}
    e.update(PYTHONIOENCODING="utf-8", PYTHONPATH=str(REPO_ROOT))
    return e


def run_engine(kind: str, cli: str, data: str, script: str) -> tuple[str, int]:
    base = [sys.executable, "-m", "meradb"] if kind == "python" else [cli]
    r = subprocess.run(base + ["run", "--local", "--data", data, script], cwd=REPO_ROOT, capture_output=True, env=env(), timeout=300)
    return r.stdout.decode("utf-8").replace("\r\n", "\n"), r.returncode


def users_shape(data: str) -> dict:
    users = json.load(open(os.path.join(data, "users.json"), encoding="utf-8"))
    shape = {}
    for name, record in users.items():
        assert list(record.keys()) == ["salt", "hash", "grants"], (name, list(record.keys()))
        assert len(record["salt"]) == 32 and len(record["hash"]) == 64, name
        int(record["salt"], 16), int(record["hash"], 16)
        shape[name] = record["grants"]
    return shape


def main() -> int:
    from meradb.users import UserStore

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", required=True)
    args = ap.parse_args()

    results, failures = {}, []
    with tempfile.TemporaryDirectory() as tmp:
        scripts = {}
        for name, text in (("build", BUILD), ("use", USE), ("verify", VERIFY)):
            scripts[name] = os.path.join(tmp, name + ".mdb")
            Path(scripts[name]).write_text(text, encoding="utf-8")
        for builder, runner, verifier in itertools.product(("python", "cpp"), repeat=3):
            chain = f"{builder}>{runner}>{verifier}"
            data = os.path.join(tmp, "data_" + chain.replace(">", "_"))
            outputs = [run_engine(k, args.cli, data, scripts[s]) for k, s in ((builder, "build"), (runner, "use"), (verifier, "verify"))]
            try:
                catalog = json.dumps(json.load(open(os.path.join(data, "main", "catalog.json"), encoding="utf-8")))
                store = UserStore(data)
                passwords_ok = store.verify("asha", "pw-sécret") and not store.verify("asha", "nope") and "ravi" not in store.users
                shape = users_shape(data)
            except (OSError, ValueError, AssertionError) as e:  # a chain whose folder is missing or malformed is a FAIL, not a crash
                catalog, passwords_ok, shape = f"<unreadable: {e}>", False, {}
            results[chain] = (outputs, catalog, shape, passwords_ok)

        baseline = results["python>python>python"]
        for chain, got in results.items():
            problems = []
            for step, (a, b) in enumerate(zip(baseline[0], got[0])):
                if a != b:
                    problems.append(f"step {step} output differs")
                    import difflib
                    problems.append("".join(list(difflib.unified_diff(a[0].splitlines(keepends=True), b[0].splitlines(keepends=True), "python-chain", chain))[:40]))
            if got[1] != baseline[1]:
                problems.append("catalog.json differs from the all-Python chain")
            if got[2] != baseline[2]:
                problems.append(f"users.json shape differs: {got[2]} vs {baseline[2]}")
            if not got[3]:
                problems.append("a password does not verify in Python's UserStore")
            if problems:
                failures.append(chain)
                print(f"FAIL {chain}\n  " + "\n  ".join(problems))
            else:
                print(f"PASS {chain}")
    print("ALL MATCHED" if not failures else "FAILED: " + ", ".join(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [x] **Step 2: Run it**

```bash
python cpp/tests/interchange_check.py --cli cpp/build/meradb_cli.exe
```
Expected: eight `PASS <builder>><runner>><verifier>` lines and `ALL MATCHED`. Typical
findings and where to fix them: a catalog key order difference (Task 7's `addTrigger` /
`addProcedure` building the JSON object in another order than `catalog.py`); a `users.json`
record with keys in another order or a salt of the wrong length (Task 6); a trigger created by
one engine that the other engine cannot fire (Task 7's `body_text` capture — text between
`SHURU` and `KHATAM` including the last `;`, byte for byte); a password Python rejects
(Task 5: the PBKDF2 parameters, or the password not being encoded as UTF-8).

- [x] **Step 3: Write the fuzz** — `cpp/tests/fuzz_triggers.py`

```python
"""
Seeded differential fuzz for triggers and stored procedures: random but valid
scripts run through the Python engine and the C++ CLI (both `--local`), and the
outputs must be identical.

    python cpp/tests/fuzz_triggers.py --cli path/to/meradb_cli [--seeds 200] [--start 0]
    python cpp/tests/fuzz_triggers.py --seed 17 --show          # print the script for one seed
    python cpp/tests/fuzz_triggers.py --stats                   # Python only: how "busy" are the scripts?

A failing seed prints the script's path and a unified diff; re-run just that seed with --seed N.
The generator sticks to constructs whose behaviour is fully defined by the language, and avoids
what is a DELIBERATE divergence: triggers never write to the table they are on (self recursion
hits Python's RecursionError and the C++ nesting cap), and no identifiers are non-ASCII.

Schema (fixed): acct(id PK, name, bal), log(kind, id, a, b), ctr(n) with one row.
Triggers are only ever defined on acct (writing to log and ctr) and on ctr (writing to log).
"""
import argparse
import difflib
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.stdout.reconfigure(encoding="utf-8")  # diffs may contain non-ASCII text; never die on a console code page

NAMES = ["Ravi", "Priya", "O'Neil", "café", "世界", "", "x y"]


def sql_text(s: str) -> str:
    return "'" + s.replace("'", "''") + "'"


def trigger_body(rng: random.Random, event: str, timing: str, tag: str, on_ctr: bool) -> str:
    """One to three statements; only tables other than the trigger's own are written."""
    if on_ctr:
        return f"DAALO MEIN log MAAN ('{tag}', NAYA.n, PURANA.n, 0);" if event == "BADLO" else f"DAALO MEIN log MAAN ('{tag}', 0, 0, 0);"
    choices = []
    if event == "DAALO":
        choices += [f"DAALO MEIN log MAAN ('{tag}', NAYA.id, NAYA.bal, 0);",
                    f"DAALO MEIN log MAAN ('{tag}n', NAYA.id, NAYA.bal + 1, NAYA.bal * 2);",
                    "BADLO ctr RAKHO n = n + 1;"]
    elif event == "BADLO":
        choices += [f"DAALO MEIN log MAAN ('{tag}', NAYA.id, PURANA.bal, NAYA.bal);",
                    f"DAALO MEIN log MAAN ('{tag}d', NAYA.id, NAYA.bal - PURANA.bal, 0);",
                    "BADLO ctr RAKHO n = n + NAYA.bal - PURANA.bal;"]
    else:
        choices += [f"DAALO MEIN log MAAN ('{tag}', PURANA.id, PURANA.bal, 0);",
                    "BADLO ctr RAKHO n = n - 1;"]
    if timing == "PEHLE" and event != "MITAO" and rng.random() < 0.35:  # a guard that fails when bal < 0
        choices.append(f"DAALO MEIN log MAAN ('{tag}g', NAYA.id, 1 / (AGAR NAYA.bal < 0 TAB 0 WARNA 1 KHATAM), 0);")
    if rng.random() < 0.2:
        choices.append("DIKHAO GINO(*) SE log;")
    count = rng.randint(1, min(3, len(choices)))
    return "\n    ".join(rng.sample(choices, count))


def generate(seed: int) -> str:
    rng = random.Random(seed)
    out = ["BANAO TABLE acct (id INT MUKHYA KUNJI, name TEXT, bal INT);",
           "BANAO TABLE log (kind TEXT, id INT, a INT, b INT);",
           "BANAO TABLE ctr (n INT);",
           "DAALO MEIN ctr MAAN (0);"]
    triggers = []
    for i in range(rng.randint(1, 5)):
        on_ctr = rng.random() < 0.2
        timing = rng.choice(["PEHLE", "BAAD"])
        event = rng.choice(["BADLO"] if on_ctr else ["DAALO", "BADLO", "MITAO"])
        name = f"trg{i}"
        body = trigger_body(rng, event, timing, f"{name}", on_ctr)
        table = "ctr" if on_ctr else "acct"
        out.append(f"BANAO TRIGGER {name} {timing} {event} PAR {table} SHURU\n    {body}\nKHATAM;")
        triggers.append(name)
    procs = []
    if rng.random() < 0.8:
        out.append("BANAO PROCEDURE bump (pid INT, amt INT) SHURU\n    BADLO acct RAKHO bal = bal + amt JAHAN id = pid;\n"
                   "    DAALO MEIN log MAAN ('bump', pid, amt, 0);\nKHATAM;")
        procs.append("bump")
    if rng.random() < 0.5:
        out.append("BANAO PROCEDURE rename (pid INT, nm TEXT) SHURU\n    BADLO acct RAKHO name = nm JAHAN id = pid;\nKHATAM;")
        procs.append("rename")
    if rng.random() < 0.3:
        out.append("BANAO PROCEDURE ping () SHURU\n    DAALO MEIN log MAAN ('ping', 0, 0, 0);\nKHATAM;")
        procs.append("ping")

    for _ in range(rng.randint(10, 40)):
        roll = rng.random()
        ident = rng.randint(1, 8)
        if roll < 0.30:
            out.append(f"DAALO MEIN acct MAAN ({ident}, {sql_text(rng.choice(NAMES))}, {rng.randint(-50, 500)});")
        elif roll < 0.45:
            out.append(f"BADLO acct RAKHO bal = bal + {rng.randint(-300, 300)} JAHAN id = {ident};")
        elif roll < 0.52:
            out.append(f"BADLO acct RAKHO bal = bal * 2 JAHAN bal > {rng.randint(0, 300)};")
        elif roll < 0.62:
            out.append(f"MITAO SE acct JAHAN id = {ident};")
        elif roll < 0.66:
            out.append(f"MITAO SE acct JAHAN bal < {rng.randint(0, 100)};")
        elif roll < 0.72:
            out.append(f"DAALO MEIN acct MAAN ({ident}, 'up', {rng.randint(0, 99)}) TAKRAAV PAR BADLO bal = bal + {rng.randint(1, 9)};")
        elif roll < 0.86 and procs:
            name = rng.choice(procs)
            if name == "bump":
                args = f"{ident}, {rng.randint(-200, 200)}" if rng.random() < 0.9 else f"{ident}"  # sometimes a wrong count
            elif name == "rename":
                args = f"{ident}, {sql_text(rng.choice(NAMES))}"
            else:
                args = ""
            out.append(f"CHALAO {name}({args});")
        elif roll < 0.90 and triggers:
            victim = rng.choice(triggers)
            out.append(f"HATAO TRIGGER {victim};")
        elif roll < 0.93:
            out.append(f"BANAO TRIGGER late{rng.randint(0, 3)} BAAD DAALO PAR acct SHURU\n    DAALO MEIN log MAAN ('late', NAYA.id, 0, 0);\nKHATAM;")
        else:
            out.append(rng.choice(["DIKHAO * SE acct KRAM id;", "DIKHAO * SE log;", "DIKHAO * SE ctr;",
                                   "DIKHAO kind, GINO(*) SE log SAMOOH kind KRAM kind;"]))
    out += ["DIKHAO * SE acct KRAM id;", "DIKHAO * SE log;", "DIKHAO * SE ctr;"]
    return "\n".join(out) + "\n"


def env() -> dict:
    e = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_")}
    e.update(PYTHONIOENCODING="utf-8", PYTHONPATH=str(REPO_ROOT))
    return e


def run(cmd: list[str]) -> tuple[str, int]:
    r = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, env=env(), timeout=120)
    return r.stdout.decode("utf-8").replace("\r\n", "\n"), r.returncode


def default_cli() -> str:
    for name in ("meradb_cli.exe", "meradb_cli"):
        p = REPO_ROOT / "cpp" / "build" / name
        if p.exists():
            return str(p)
    return str(REPO_ROOT / "cpp" / "build" / "meradb_cli")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", default=default_cli())
    ap.add_argument("--seeds", type=int, default=200)
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--seed", type=int, help="run exactly this seed")
    ap.add_argument("--show", action="store_true", help="print the script for --seed and exit")
    ap.add_argument("--stats", action="store_true", help="Python only: report how many statements fail / how many rows the triggers wrote")
    args = ap.parse_args()

    if args.show:
        sys.stdout.reconfigure(encoding="utf-8")
        print(generate(args.seed or 0))
        return 0
    seeds = [args.seed] if args.seed is not None else list(range(args.start, args.start + args.seeds))

    if args.stats:
        errors = statements = log_rows = 0
        for seed in seeds:
            with tempfile.TemporaryDirectory() as data:
                script = os.path.join(data, "s.mdb")
                Path(script).write_text(generate(seed), encoding="utf-8")
                out, _ = run([sys.executable, "-m", "meradb", "run", "--local", "--data", os.path.join(data, "d"), script])
            blocks = [b for b in out.split("\n\n") if b.strip()]
            statements += len(blocks)
            errors += sum(1 for b in blocks if "Galti]" in b)
            if "Internal" in out or "Traceback" in out or "RecursionError" in out:
                print(f"seed {seed}: the Python engine hit an internal error -- the generator must avoid this construct")
            log_rows += out.count("\n| trg") + out.count("| bump") + out.count("| ping")
        print(f"{len(seeds)} scripts, {statements} results, {errors} errors ({100 * errors // max(statements, 1)}%), "
              f"~{log_rows} trigger/procedure log lines in the final dumps")
        return 0

    failures = []
    for seed in seeds:
        with tempfile.TemporaryDirectory() as tmp:
            script = os.path.join(tmp, "s.mdb")
            Path(script).write_text(generate(seed), encoding="utf-8")
            py, py_code = run([sys.executable, "-m", "meradb", "run", "--local", "--data", os.path.join(tmp, "py"), script])
            cpp, cpp_code = run([args.cli, "run", "--local", "--data", os.path.join(tmp, "cpp"), script])
            if (py, py_code) != (cpp, cpp_code):
                keep = os.path.join(tempfile.gettempdir(), f"meradb_fuzz_{seed}.mdb")
                Path(keep).write_text(generate(seed), encoding="utf-8")
                failures.append(seed)
                print(f"MISMATCH seed {seed} (exit python={py_code} cpp={cpp_code}); script saved to {keep}")
                print("".join(list(difflib.unified_diff(py.splitlines(keepends=True), cpp.splitlines(keepends=True), "python", "cpp"))[:40]))
    print(f"{len(seeds) - len(failures)}/{len(seeds)} seeds matched" + (f"; FAILED: {failures}" if failures else ""))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [x] **Step 4: Check the generator against Python only, then run the fuzz**

```bash
python cpp/tests/fuzz_triggers.py --stats --seeds 60
python cpp/tests/fuzz_triggers.py --cli cpp/build/meradb_cli.exe --seeds 200
```
Expected: the first prints roughly `60 scripts, ~2200 results, ~9% errors` and no
"internal error" lines; the second ends with `200/200 seeds matched`. If a seed mismatches:
`python cpp/tests/fuzz_triggers.py --seed N --show > bad.mdb`, then shrink it by hand (delete
statements while the diff persists) and turn the shrunk script into a golden script or a unit
test in `test_engine_triggers.cpp` **before** fixing the engine. Run a bigger sweep once before
signing the task off (`--seeds 2000`; about 4 minutes).

- [x] **Step 5: ctest registration** — in the Python block of `cpp/tests/CMakeLists.txt`:

```cmake
  add_test(NAME interchange
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/interchange_check.py --cli $<TARGET_FILE:meradb_cli>)
  add_test(NAME fuzz_triggers
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/fuzz_triggers.py --cli $<TARGET_FILE:meradb_cli> --seeds 100)
  set_tests_properties(interchange fuzz_triggers PROPERTIES TIMEOUT 900)
```

- [x] **Step 6: Commit**

```bash
git add cpp/tests/interchange_check.py cpp/tests/fuzz_triggers.py cpp/tests/CMakeLists.txt
git commit -m "Add data-folder interchange check and the trigger/procedure differential fuzz"
```

**Completion checklist:**
- [x] `interchange_check.py`: 8/8 chains pass, in particular every chain whose builder and runner differ
- [x] `fuzz_triggers.py`: 200/200 (and one 2000-seed sweep) with no mismatch
- [x] every fuzz failure found was turned into a permanent regression test before the fix
- [x] the fuzz's seeds are deterministic (running the same seed twice prints the same script)
- [x] full suite green

---

### Task 25: Documentation, completion checklist, hand-off to Phases 3-5

**Files:**
- Modify: `docs/CPP.md`, `README.md`
- Modify: `docs/cpp-port/plans/2026-09-29-cpp-port-phase2-server-users-triggers.md` (tick the completion checklist below)

**Interfaces:** documentation only; nothing here may change behaviour.

- [x] **Step 1: `docs/CPP.md` — head and scope.** Replace the first heading and the two
sections "What Phase 1 covers" / the "Not in Phase 1" paragraph with:

```markdown
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
```

- [x] **Step 2: `docs/CPP.md` — "Run a script" and new "Server and client".** Replace the
"Run a script" section with the text below (keep the cross-engine section and update it in
Step 3):

````markdown
## Command line

```
meradb_cli run <script.mdb> [more.mdb ...] [--data <dir>] [--local] [-H host] [-p port] [-U user] [-W] [-d database]
meradb_cli server  [--data <dir>] [--host 127.0.0.1] [--port 6372] [--password pw] [-v]
meradb_cli start   [same options]      # background; log in <data>/server.log
meradb_cli status | stop [--force] [-D <dir>] [-W]
```

`meradb_cli x.mdb` means `run x.mdb`. `run` connects to a server if one answers (the port of
the data folder's `meradb.pid`, `MERADB_HOST` / `MERADB_PORT`, else 127.0.0.1:6372) and
otherwise opens the data folder directly ("LOCAL mode", with a note on stderr); an explicit
`-H` / `-p` never falls back. Output matches `meradb run`: each statement's result followed
by a blank line; a failing statement does not stop the script; exit code 1 if any statement
failed. Environment: `MERADB_DATA` (data folder), `MERADB_PORT`, `MERADB_HOST`,
`MERADB_PASSWORD` (the server-wide password), `MERADB_USER`. `--data` defaults to the same
per-user folder as Python (`%LOCALAPPDATA%\MeraDB\data`, or `~/.local/share/MeraDB/data`).

## Server notes

One thread per connection; each connection owns its engine session (current database,
transaction) on that thread. All sessions share one instance and one lock: a statement waits
up to 10 s for another session's transaction ("Database busy hai"). A client that disconnects
in the middle of a transaction is rolled back, and so is every open transaction when the
server is stopped (Python leaves those to crash recovery). A data folder that a server is
serving cannot be opened directly by another process. The protocol is documented in
`docs/SERVER.md` (unchanged: the C++ server speaks exactly that).
```

- [x] **Step 3: `docs/CPP.md` — verification section.** Replace the "Cross-engine
verification" section with:

````markdown
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
````

- [x] **Step 4: `docs/CPP.md` — layout rows.** Add to the layout table:

```markdown
| `users.py`                          | `users.h` / `users.cpp`, `crypto.h` / `crypto.cpp` (in-tree SHA-256, HMAC, PBKDF2) |
| `protocol.py`                       | `protocol.h` / `protocol.cpp`, `pyjson.h` / `pyjson.cpp` (Python-compatible JSON) |
| `server.py`                         | `server.h` / `server.cpp`                         |
| `client.py`                         | `client.h` / `client.cpp`, `backend.h`            |
| `cli.py` (server, start, stop, status, run) | `cli.h` / `cli.cpp`, `server_control.h` / `server_control.cpp`, `main.cpp` |
```
and this paragraph after the "Supporting files" paragraph: "Also without a Python counterpart:
`net_compat.h` / `net_compat.cpp` (Winsock / BSD sockets) and `sys_compat.h` / `sys_compat.cpp`
(environment, time, random bytes, process spawning): the only places that include platform
headers."

- [x] **Step 5: `docs/CPP.md` — divergences.** In "Known divergences", **delete** the "Default
data folder" bullet (it no longer differs) and **add**:

```markdown
- **Parse-depth caps (hostile input)**: Python dies with `RecursionError` on a deeply nested
  query (about 110 nested parentheses). C++ limits ONE statement to 400 operator/nesting
  units (`Query bahut gehri (nested) hai (limit 400)`), statements nested inside statements
  (trigger and procedure bodies, `SAMJHAO`) to 32, and views defined over views to 32. The
  caps exist so that a network client cannot crash the server; ordinary scripts never come
  near them.
- **Trigger/procedure recursion cap**: a trigger that (directly or indirectly) fires itself
  endlessly makes Python raise `RecursionError`; C++ stops at 32 levels with
  `Trigger/procedure bahut gehra chal raha hai (limit 32) -- shayad koi trigger khud ko
  baar-baar chala raha hai`.
- **Non-ASCII identifiers**: Python accepts any Unicode letter in a table or column name;
  C++ accepts ASCII letters, digits and `_` only (Unicode letter classes need tables the
  project does not depend on). Non-ASCII text in string literals and data is fine.
  Unexpected non-ASCII characters in a query are shown whole, but Python's escaping of
  unprintable ones (`'\xa0'`) is not reproduced.
- **Server shutdown**: stopping the server rolls back every open transaction cleanly; Python's
  daemon threads die with the process and rely on crash recovery at the next start. The data
  folder ends up the same.
- **`start`** launches `meradb_cli server` (this program), not `python -m meradb server`.
- **SHA-256 in-tree** (no picosha2 as the design spec suggested; see the plan's D4).
```
and add to "Python behaviours mirrored on purpose":

```markdown
- `Connection.execute` wraps the server's already-tagged error text in a second
  `MeraDBError`, so the `[Stage Galti]` tag stays in the message.
- A user login supersedes the shared server password; a `hello` with a database goes through
  `UseDatabase`, so a restricted user is refused ("superuser nahi hai").
- An unterminated last line on the wire counts as a message; the size limit counts the newline.
- Triggers on a table are not dropped or renamed with it; a `NAYA`/`PURANA` reference is
  substituted into `INSERT`, `UPDATE`, `DELETE`, `SELECT` and `CHALAO` only (subquery bodies are
  left as written).
```

- [x] **Step 6: `README.md`.** Replace the "C++ implementation" paragraph with:

```markdown
### C++ implementation

A C++17 port lives in `cpp/`. It shares the Python engine's grammar, output, on-disk formats
and wire protocol, and is checked against it (example scripts, a client/server interop matrix,
data folders passed between the engines, a trigger/procedure fuzz). It has the full engine,
users and privileges, triggers, stored procedures, the TCP server and the `run` / `start` /
`stop` / `status` commands; the interactive shell and the workbench are Python-only for now.
See [docs/CPP.md](docs/CPP.md) for build instructions and status.
```
and change the last link description in the docs list to `the C++ port (build instructions, layout, verification, status)`.

- [x] **Step 7: Final verification of the whole phase**

```bash
cmake -S cpp -B cpp/build -G "MinGW Makefiles"
cmake --build cpp/build
ctest --test-dir cpp/build --output-on-failure
git diff <base> --stat -- meradb
```
Expected: every ctest test passes (Catch2 cases, `cross_engine_*`, `cli_lifecycle`,
`interop_matrix`, `interchange`, `fuzz_triggers`); the last command prints nothing (the Python
engine is untouched). Then a warning check: `cmake --build cpp/build --clean-first 2>&1 | grep -c warning`
prints `0`.

- [x] **Step 8: Commit**

```bash
git add docs/CPP.md README.md docs/cpp-port/plans/2026-09-29-cpp-port-phase2-server-users-triggers.md
git commit -m "Document the C++ server, client, users, triggers and procedures"
```

**Hand-off notes for the later phases** (put the same text at the end of `docs/CPP.md` under
"Next phases", one bullet each):
- **Phase 3, shell** (`repl.py`): reuse `Backend` (`LocalBackend` / `Connection`), `formatResult`
  and `runFile`; `sys::readHidden` exists for password prompts; needs a line editor (linenoise
  or replxx, vendored or FetchContent) with history; the banner/animation are pure output.
  `openBackend` already implements the shell's connection logic — call it, do not copy it.
  The shell must handle `Connection` errors mid-session (`ConnectionFailed`) like `repl.py`.
- **Phase 4, workbench**: `Backend::schemaTree()` already returns the JSON the sidebar needs
  (both local and remote); FTXUI panels; a query must run off the UI thread — but remember the
  SHURU rule: a transaction's statements must all run on ONE thread, so use a dedicated
  worker thread per session, never a pool.
- **Phase 5, polish**: `docs/REPORT.md`; grow `docs/CPP.md`'s divergence list from the
  hand-off checklists; consider Unicode identifiers (ICU or a small generated table of letter
  ranges) if full parity is wanted; the Windows MSVC build has not been compiled in Phase 2 —
  do it first thing and fix warnings (the code follows the MSVC rules in the plan's Global
  Constraints, but they were only reviewed, not compiled).

**Completion checklist:**
- [x] `docs/CPP.md` no longer says "Phase 1 covers" anywhere and lists every divergence of this phase
- [x] the commands in the docs were copy-pasted and run once
- [x] README paragraph and link text updated
- [x] `git diff <base> -- meradb` still empty
- [x] the Phase 2 completion checklist below is fully ticked

---

# Batches

Tasks are grouped into four batches; each batch ends with a review of the whole batch
(the focus of that review is in the last column) and the full suite green with zero warnings.

| Batch | Tasks | What it delivers | Review focus |
|---|---|---|---|
| A | 1-12 | build groundwork, `sys_compat`, Phase 2 grammar, crypto, `users.json`, privileges, triggers, procedures, goldens | privilege matrix wording and class names (D6); trigger hook order and index-cache safety (D5, D9.2); `users.json` vectors |
| B | 13-19 | sockets, JSON codec, protocol, engine lock helper, server, client, in-process client/server tests, hardening | thread confinement of sessions, shutdown, disconnect mid-transaction (D2, D9.1); wire byte fidelity (D9.3); MSVC review of `net_compat` (D9.5) |
| C | 20-21 | process control (spawn / kill), `start` / `stop` / `status`, the command line, lifecycle ctest | Windows handle inheritance and POSIX fork safety; argparse-style error text; fallback rules |
| D | 22-25 | full-example diffs (local and via server), interop matrix, data-folder interchange, fuzz, docs | every mismatch resolved in C++ or recorded as a divergence; docs match reality |

---

# Phase 2 completion checklist

Tick every line before declaring the phase done (copy the ticked list into the PR description).

**Features**
- [x] `BANAO USER` / `HATAO USER` / `ADHIKAR DO` / `ADHIKAR WAPAS` parse, execute and persist to `users.json`; privilege errors match Python (exact text, class names)
- [x] `BANAO TRIGGER` / `HATAO TRIGGER` fire `PEHLE`/`BAAD` per row for `DAALO`/`BADLO`/`MITAO` and upsert, with `NAYA`/`PURANA`
- [x] `BANAO PROCEDURE` / `HATAO PROCEDURE` / `CHALAO` with parameter coercion and the argument-count error
- [x] server: hello (password, user, database), query, schema, status, ping, shutdown; per-session database and transaction; rollback on disconnect and on server stop
- [x] client `Connection` and `LocalBackend` interchangeable behind `Backend`
- [x] CLI: `server`, `start`, `stop [--force]`, `status` (exit 3 when not running), `run` with local fallback, all client options

**Verification**
- [x] Catch2 suite green (Phase 1's 244 cases plus every new one), zero compiler warnings on a clean rebuild
- [x] `cross_engine_diff.py`: `MATCH` for `examples/demo.mdb` and `examples/rdbms_lab_coverage.mdb`, local and via server (and the two Phase 1 trimmed scripts)
- [x] `interop_check.py`: `ALL MATCHED` (four client/server pairs, raw protocol, Python-written `users.json`)
- [x] `interchange_check.py`: 8/8 chains
- [x] `fuzz_triggers.py`: 200/200 seeds, plus one 2000-seed sweep, no mismatch
- [x] `cli_lifecycle.py`: `ALL PASSED`; no stray processes or temp folders after the whole ctest run
- [x] three consecutive runs of the whole ctest suite pass (thread-timing flakiness check)

**Constraints**
- [x] `git diff <base> -- meradb examples` is empty (Python engine and examples untouched)
- [x] no `getenv` / `localtime` / `strerror` / socket headers outside `sys_compat` / `net_compat`
- [x] every deliberate divergence is in `docs/CPP.md` (depth caps, recursion cap, non-ASCII identifiers, shutdown rollback, in-tree SHA-256)
- [x] no tool or personal references in code, comments, commit messages or docs
- [x] `docs/CPP.md` and README updated (Task 25)

**Known limits carried into later phases** (also in the hand-off notes): the MSVC build is
reviewed but not compiled; no shell or workbench; identifiers are ASCII-only.

