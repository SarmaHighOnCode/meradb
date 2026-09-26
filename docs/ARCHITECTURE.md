# How MeraDB Works

## The big picture

Every relational database has the same layers. MeraDB has them too,
just smaller. Here's the whole system, from a client typing a query down to bytes on disk:

```
  meradb shell      meradb workbench      your Python code
        │                  │                     │
        └──────── TCP :6372, one JSON object per line ────────┘      client.py / protocol.py
                                   │
   ┌───────────────────────────────▼──────────────────────────────┐
   │ SERVER  server.py   one thread + one Engine (session) per client │
   └───────────────────────────────┬──────────────────────────────┘
                                   │  "DIKHAO naam SE s JAHAN id = 5;"
   ┌───────────────────────────────▼──────────────────────────────┐
   │ 1. TOKENIZER  tokenizer.py      text -> tokens                │
   │ 2. PARSER     parser.py         tokens -> AST (ast_nodes.py)  │
   │ 3. PLANNER    planner.py        names -> alias.column,        │
   │                                 index or scan? hash join?      │
   │ 4. EXECUTOR   engine.py         runs the plan                  │
   │               evaluator.py      JAHAN/RAKHO per row, NULL logic│
   │               aggregates.py     GINO / KUL / AUSAT ...         │
   └──────────┬─────────────────────────────────┬─────────────────┘
              │                                 │
   ┌──────────▼──────────┐          ┌───────────▼───────────────────┐
   │ 5a. CATALOG         │          │ 5b. TABLES + STORAGE          │
   │ catalog.py          │          │ table.py   hash indexes       │
   │ catalog.json        │          │ storage.py binary heap files  │
   └─────────────────────┘          └───────────────────────────────┘
```

| File | Responsibility |
|------|----------------|
| `errors.py`     | One exception class per layer, so you know *where* a query failed |
| `tokenizer.py`  | Characters → tokens. Knows keywords, numbers, strings, symbols |
| `ast_nodes.py`  | Dataclasses describing every statement and expression |
| `parser.py`     | Recursive-descent parser. One method per grammar rule |
| `planner.py`    | Binding (name resolution), index-vs-scan choice, hash-join-vs-loop choice |
| `evaluator.py`  | Evaluates an expression against a row; SQL three-valued NULL logic; `JAISA` |
| `aggregates.py` | GINO / KUL / AUSAT / NYUNTAM / ADHIKTAM over the rows of one group |
| `engine.py`     | `Instance` (shared state + lock + transactions) and `Engine` (one session) |
| `table.py`      | A table = schema + heap file + in-memory hash indexes |
| `catalog.py`    | Table schemas, saved in `catalog.json` |
| `storage.py`    | Binary row encoding + heap file (append, scan, read, tombstone delete) |
| `datatypes.py`  | INT/FLOAT/TEXT/BOOL, type checking, display formatting |
| `protocol.py`   | Wire format (JSON lines), pid file, default data folder |
| `server.py`     | The TCP server |
| `client.py`     | `Connection`: talk to a server from Python |
| `cli.py`        | The `meradb` command (start/stop/status/server/shell/workbench/run) |
| `repl.py`       | The interactive shell |
| `highlight.py`  | Lenient regex scanner that colours queries (for the workbench) |
| `tui.py`        | The full-screen workbench (Textual). Presentation only |

**Front-ends don't know the engine's insides.** The shell, the workbench and the server
all use the same small API: `run_script`, `schema_tree`, `current_db`, `in_transaction`
and `close`. An embedded `Engine` and a network `Connection` both provide it, so the shell
and workbench work identically in local mode and against a server.

## On-disk format: a real example

After running:

```sql
BANAO TABLE s (id INT, naam TEXT);
DAALO MEIN s MAAN (1, 'Ravi'), (2, KHALI);
MITAO SE s JAHAN id = 1;
```

`main/s.tbl` is exactly 46 bytes:

```
4d 45 52 41 44 42 30 31                    "MERADB01"   magic header
00 12 00 00 00                             status=0 (DELETED!), length=18
   01 01 00 00 00 00 00 00 00              id:   tag=1, INT 1 (8 bytes, little-endian)
   01 04 00 00 00 52 61 76 69              naam: tag=1, len=4, "Ravi"
01 0a 00 00 00                             status=1 (live), length=10
   01 02 00 00 00 00 00 00 00              id:   tag=1, INT 2
   00                                      naam: tag=0 -> KHALI
```

Deleting Ravi flipped **one byte** (`01` → `00`), but the data is still physically there.
That's a *tombstone*. `SIKODO TABLE` rewrites the file without tombstones.
A row's **row id** is simply its byte offset in the file (Ravi = 8, the second row = 31).

## How each statement executes

| Statement | Algorithm |
|-----------|-----------|
| `DAALO`   | Evaluate values → type-check + VARCHAR(n) → ZAROORI → SHART → ANOKHA + SANDARBH via hash index (O(1)) → append all rows |
| `DIKHAO`  | Index lookup or full scan → MILAO → JAHAN → SAMOOH + aggregates → JINKA → KRAM (KAHO aliases resolved first) → project → ALAG → SIRF |
| `BADLO`   | Index/scan & collect matching rows **first** → compute new rows → validate (incl. SHART, SANDARBH) → RESTRICT check if a SANDARBH'd value changed → tombstone old + append new |
| `MITAO`   | Index/scan & collect matching rows → RESTRICT check (any child still SANDARBH-ing a value?) → tombstone each |
| `SUDHARO ... JODO/HATAO` | Read all rows → re-encode with new schema → atomic rewrite (temp file + `os.replace`) |
| `SUDHARO ... NAYA_NAAM` | Catalog-only: rename the `.tbl` file with `os.replace`, no row rewrite (row ids don't change) |
| `SAAF`    | RESTRICT check (as a parent) → rewrite file with just the header |
| `SIKODO`  | Rewrite file with only the live records (drops tombstones) |
| `SHURU` / `PAKKA` / `WAPAS` | Snapshot the database folder / delete the snapshot / put it back |

## Planning: binding, indexes, joins (`planner.py`)

**Binding.** Before running, every column name is resolved to `alias.column`.
In `DIKHAO naam SE students s MILAO courses c PAR s.cid = c.id`, `naam` becomes `s.naam`.
An unknown name fails here. So does a name found in two tables (`id`): the user must
write `s.id` or `c.id`. After binding, every row is a dict such as
`{"s.id": 1, "s.naam": "Ravi", "c.id": 10, "c.title": "DBMS"}`.

**Indexes (`table.py`).** Every `MUKHYA KUNJI` / `ANOKHA` column gets a hash index:
a Python `dict` from value to row id. It is built lazily with one scan the first time it
is needed, updated on every insert and delete, and thrown away whenever a file is
rewritten (ALTER, SIKODO, SAAF, WAPAS), because row ids change. The planner uses it when
`JAHAN` contains `unique_column = constant` among its `AUR`-ed parts. The full `JAHAN` is
still checked afterwards, so the index can only make a query faster, never change its answer.
Uniqueness checks also use it: O(1) per value instead of a full scan.

**Joins.** For each `MILAO`, if `PAR` contains `left.x = right.y`, MeraDB does a
**hash join**: it puts the right table in a dict keyed by `y`, then each left row finds
its partners instantly (O(n + m)). Otherwise it does a **nested loop** that tries every
pair (O(n × m)). `BAAYAN MILAO` (LEFT JOIN) keeps left rows with no partner, padding
the right side with KHALI.

`SAMJHAO` prints these decisions without running the query.

**Joins beyond INNER/LEFT.** `DAHINA MILAO` (RIGHT) is built symmetrically to LEFT --
the hash index goes on the LEFT rows instead, and the loop is driven by the RIGHT rows,
but the output columns still appear in the order written (left columns, then right). `DONO
MILAO` (FULL OUTER) runs LEFT semantics first (every left row, matched or KHALI-padded),
then separately finds right rows that matched nothing at all and appends those too,
KHALI-padded on the left side -- i.e. FULL = LEFT ∪ (unmatched RIGHT rows). `SAMAAN MILAO`
(NATURAL JOIN) has no `PAR` clause at all: the planner looks at every column name the new
table shares with any table already joined so far and ANDs together `earlier.col =
new.col` for each one (the leftmost earlier table wins if more than one shares a name);
zero shared names is an error, since there is nothing to join on.

## Subqueries: uncorrelated-once vs correlated-per-row

A `(DIKHAO ...)` used as an ordinary value -- as a scalar (`x = (DIKHAO ...)`) or as an
`x MEIN (DIKHAO ...)` membership list -- is deliberately NOT handled by a general derived-table
planner. Instead, `Engine._run_subquery` runs the subquery's OWN `_exec_Select` fresh, and the
result is reduced to a scalar or list by `Engine._reduce_subquery_result` (scalar: exactly 1
column, 0 or 1 row for THIS outer row; list: exactly 1 column, any number of rows).

The tricky part is a CORRELATED subquery, where an inner column reference (like `s2.dept_id` in
`JAHAN s2.dept_id = s.dept_id`) actually means the OUTER query's current row, not anything in the
subquery's own tables. MeraDB does this with an AST rewrite rather than threading a parent scope
through the whole planner: `Engine._correlate` walks the subquery's WHERE/columns/etc, and for
every `ColumnRef` that does NOT resolve against the subquery's OWN scope, it substitutes an
`ast.Literal` holding that value from the current outer row (found by matching `alias.column`, or
by column name alone if unambiguous). The rewritten statement is then planned and run completely
normally -- the planner and evaluator have no idea a correlation ever happened.

Whether a subquery is correlated is a STRUCTURAL fact (it depends only on which column names the
outer row's keys have, not on their values), so `Engine._precompute_subqueries` decides it with
ONE dry run against the first outer row: if the substitution never fires, the subquery is
uncorrelated and its single result is shared across every row; if it does fire, the subquery is
re-run once per outer row. Either way, the ENGINE computes a `subqueries: dict[id(node) -> value]`
before calling `evaluate()` -- the evaluator itself never executes anything, it only looks the
precomputed answer up by AST node identity (`evaluator.find_subqueries` mirrors `find_aggregates`
for this purpose). This keeps `evaluator.py` free of any Engine/Catalog/planner imports.

## Views: materialized fresh on every read

`BANAO VIEW naam KAHO DIKHAO ...` stores only the SELECT's raw SOURCE TEXT in
`Catalog.views` (same idea as a `SHART`/CHECK constraint) -- never a parsed AST, since
`catalog.json` is plain JSON. Every time the view's name is used in `SE`/`MILAO`,
`Engine._resolve_source` re-parses that text and runs it via `_exec_Select` right then,
wrapping the resulting rows in a small `table.MaterializedTable` (schema inferred from the
first non-KHALI value in each output column, defaulting to TEXT) so the rest of
`_plan_select`/`_join`/etc can treat it exactly like a `Table`. This means a view always
reflects the CURRENT schema and data of whatever it selects from -- there is no cached,
stale copy -- at the cost of re-running the whole query every time (no view is ever
materialized once and reused across statements). `planner.choose_access` refuses to build
an index lookup against a `MaterializedTable` (`table.is_view`), so a view as the first
`SE` source is always a full scan of its just-computed rows; this is correct, just not
index-accelerated, exactly like a real view over a complex query would be.

## How grouping works (`SAMOOH`)

```sql
DIKHAO shehar, GINO(*), AUSAT(cgpa) SE s SAMOOH shehar;
```

1. **Bucket:** walk the filtered rows and put each into a `dict` keyed by its `SAMOOH`
   values: `{("Delhi",): [row, row, row], ("Pune",): [row, row]}`.
2. **Collapse:** each bucket becomes ONE row. It is the bucket's first row plus each
   aggregate's answer, stored under a special key:
   `{"s.shehar": "Delhi", ..., ("agg", "GINO(*)"): 3, ("agg", "AUSAT(s.cgpa)"): 8.17}`.
3. **Evaluate:** when `evaluate()` meets `GINO(*)` it simply looks up `("agg", "GINO(*)")`
   in the row. So `JINKA`, `KRAM` and the output columns all work on groups with **no
   extra code**. A tuple key can never clash with a real column name, which is always a string.

With no `SAMOOH` but an aggregate (`DIKHAO GINO(*) SE s`), all rows form one group.
That group still exists when the table is empty, so the answer is `0`, not "no rows".

## Transactions and crash recovery

MeraDB uses **shadow copies**:

| Step | What happens on disk |
|------|----------------------|
| `SHURU` | copy `data/<db>` → `data/.wapas/<db>.tmp`, then rename to `.wapas/<db>` |
| work    | changes go to `data/<db>` as usual |
| `PAKKA` | rename `.wapas/<db>` → `.wapas/<db>.done` (**the commit point**), then delete it |
| `WAPAS` | delete `data/<db>`, rename `.wapas/<db>` back to `data/<db>` |

Every step goes through a **rename**, which the operating system does atomically.
At startup, `Instance._recover()` looks in `.wapas/`:

- `<db>` still there → the process died **before** `PAKKA`, so roll back (restore it).
- `<db>.tmp` → `SHURU` never finished, so delete it.
- `<db>.done` → `PAKKA` already happened, so delete it.

That gives **atomicity** (all or nothing) and **durability** (committed = on disk).
It costs one full copy of the database per transaction, which is fine for a small
database. Real systems write a *write-ahead log* (WAL) of changes instead.

## Concurrency: many clients, one lock

The server gives each connection its own thread and its own `Engine` (session). All
sessions share one `Instance`, which holds the catalogs, the indexes and **one lock**:

- Every statement holds the lock while it runs, so statements never interleave.
- `SHURU` takes the lock and keeps it until `PAKKA`/`WAPAS`. Other clients wait
  (10 s, then "Database busy"), so nobody sees uncommitted data. That is **isolation**,
  at the strictest level (serializable).
- Disconnecting mid-transaction rolls it back.
- `meradb.pid` in the data folder stops a second server, or a `--local` client, from
  opening the same files at the same time.

## Constraints: SANDARBH (FOREIGN KEY) and SHART (CHECK)

**SANDARBH reuses the same hash index as MUKHYA KUNJI/ANOKHA.** A FK's parent column is
*required* to be MUKHYA KUNJI or ANOKHA (checked once, when the FK is declared), so it
already has a `{value -> row_id}` hash index (`table.py`). Checking a child's FK value on
INSERT/UPDATE is then just one dict lookup per value -- O(1), the same trick used for
uniqueness checks.

```
students.cid --SANDARBH--> courses.id (MUKHYA KUNJI)
   DAALO MEIN students (cid) MAAN (10)
       -> courses.indexes()[id_position].get(10) is not None?  O(1)
```

There is no reverse index (child rows pointing at a parent), so the RESTRICT check on
DELETE/UPDATE/HATAO TABLE/SAAF TABLE of a *parent* scans the catalog for every column whose
`ref_table` names this table, then scans that child table's rows -- O(children), same cost a
real database pays without a foreign-key index on the child column either. A self-reference
(`employee.manager_id -> employee.id`) is the same code path with parent == child; it is
special-cased only to (a) also accept values inserted earlier in the *same* statement, and
(b) not count a row against itself when it is the one being deleted/renamed away.

**SHART stores the expression's SOURCE TEXT, not its AST**, because `catalog.json` is JSON
and a parsed `BinaryOp`/`ColumnRef` tree isn't JSON-serialisable (unlike a `WARNA` default,
which is already a plain value). `tokenizer.py` therefore tracks each token's character
`start`/`end` offset, and the parser slices the original query text between the `SHART`
clause's parentheses. `parser.parse_expression()` re-parses that text back into an AST
whenever it needs to be checked or evaluated, and is `functools.lru_cache`d since the same
CHECK text is parsed again on every INSERT/UPDATE. Enforcement then reuses the *existing*
`evaluate()` engine unchanged: the row's values are put in a `{column: value}` dict, and an
unbound `ColumnRef` already looks itself up there. Like `JAHAN`, a `SHART` follows SQL's
three-valued logic: the row is rejected only if the result is exactly `JHOOTH`; `KHALI`
(unknown) passes, same as a `WHERE` clause lets an unknown row through when negated.

Because the catalog only has text, not a tree, a `SHART` can't be automatically "rewritten"
when a column it uses is renamed -- `SUDHARO TABLE ... COLUMN ... NAYA_NAAM` therefore
refuses the rename instead of silently leaving a CHECK that mentions a column that no
longer exists.

## Users, privileges, triggers, procedures (Phase B)

**None of these three are being built SQL-standard-complete** -- doing so would be many
times the size of the rest of the project. Each is a genuinely useful, clearly-simplified
version, and knowing exactly WHERE the simplification is matters more for a viva than the
feature working at all:

- **Privileges are one shared, server-wide user table with per-`(database, table)`
  grants** -- not schema-level roles, not column-level grants, not `WITH GRANT OPTION`.
  A user is stored in `users.py`'s `UserStore`, one JSON file (`users.json`) at the TOP of
  the data directory (not inside any one database's folder), because a MeraDB *server*
  (one `Instance`) can hold several databases, and a login is a property of the server, not
  of any one database -- exactly the split real databases make between "server-level
  logins" and "database-level objects". Passwords are hashed with
  `hashlib.pbkdf2_hmac("sha256", ..., 100_000 iterations)` and a random per-user salt --
  stdlib only, no bcrypt/passlib, "good enough for a teaching project", explicitly not
  production-grade (no configurable work factor, no pepper, no lockout).
- **Backward compatibility is the whole design constraint here.** `Engine.user` defaults
  to `None`, meaning superuser/unrestricted -- `Engine._check_privileges()` (in
  `engine.py`, called once from `execute_statement()` before every statement) returns
  immediately whenever `self.user is None`. Every embedded `Engine`, every `--local`
  session, and every one of the 235 tests that existed before this phase never sets
  `self.user` at all, so privilege checking is invisible to all of them. Only a *server*
  client that authenticates with `-U`/`--user` (see `docs/SERVER.md`) gets `Engine.user`
  set by `server.py`'s handshake, and only THAT session is ever checked.
- **Triggers fire once per affected row**, running a fixed list of stored statements with
  `NAYA`/`PURANA` substituted in as literals -- no procedural control flow beyond what
  `AGAR...KHATAM` expressions already give. A trigger's body is stored as raw SOURCE TEXT
  in `Catalog.triggers` (`name -> {timing, event, table, body_text}`), exactly like a
  `BANAO VIEW`'s query text -- reparsed fresh every time it fires (`_fire_triggers()` in
  `engine.py`), rather than caching an AST that can't round-trip through `catalog.json`.
  `NAYA.col`/`PURANA.col` substitution reuses the SAME recursive-AST-rewrite shape as
  `Engine._correlate()` (written for correlated subqueries) -- see `Engine._substitute()`,
  a generic "replace this ColumnRef with a Literal" walk shared by triggers AND procedures,
  so the recursion logic exists exactly once in the codebase.
- **Stored procedures are a named, parameterised macro, not a language**: no loops, no
  local variables beyond the parameters, no return value. Bodies are stored as raw source
  text too (`Catalog.procedures`), reparsed on every `CHALAO`. Parameter substitution is
  the same `Engine._substitute()` walk, matching bare (unqualified) `ColumnRef`s by name --
  which is also why a procedure touching a table with a same-named column is a real
  (documented) footgun: there's no separate namespace, hence the `p_` prefix convention.
- **A trigger/procedure body executes via `self.execute_statement()`**, so it is subject to
  the SAME privilege checks as anything else the session runs -- i.e., "invoker rights",
  not "definer rights" (a real database sometimes offers the latter as an option; MeraDB
  keeps only the simpler, stricter one).

## Design decisions (good viva material)

1. **Why a binary format and not JSON/CSV?** Fixed-size numbers, no parsing, and
   deletes are a 1-byte in-place write. JSON would need rewriting the whole file per delete.
2. **Why is UPDATE = delete + insert?** Text values change length, so the new row
   might not fit where the old one was. Appending is always safe.
3. **Why collect rows before updating?** If we updated while scanning, the new
   versions appended at the end would be scanned again → updated twice (the "Halloween problem").
4. **Why validate all INSERT rows before writing any?** So a failing row 3 doesn't
   leave rows 1–2 behind.
5. **Why temp file + `os.replace` for ALTER and the catalog?** `os.replace` is atomic:
   a crash leaves either the old file or the new one, never half of each.
6. **Why are `BEECH` and `MEIN` handled only in the parser?** They are *syntactic sugar*.
   `x BEECH a AUR b` means exactly `x >= a AUR x <= b`, so the parser builds that tree.
   The evaluator gets them for free, with correct KHALI behaviour.
7. **Why keep indexes only in memory?** Rebuilding costs one scan, which is cheap at this
   size, and there's no second file that could get out of sync with the table after a crash.
8. **Why hash indexes and not B-trees?** Hash lookup is O(1) and trivial to build from a
   `dict`, but it only answers `=`. A B-tree keeps keys sorted, so it also answers `<`, `>`
   and `KRAM`. That's why every real database defaults to B-trees.
9. **Why one global lock?** It's the simplest design that is provably correct: statements
   run one after another (*serial execution*). The cost is no parallelism. Large databases use
   MVCC so readers and writers don't block each other.
10. **Why JSON lines for the protocol?** It's human-readable, trivial to parse in any
    language, and `json.dumps` never emits a newline, so a newline cleanly ends each message.
11. **Why is SANDARBH RESTRICT-only, no CASCADE?** RESTRICT is one `if` (refuse if a child
    still points here); CASCADE means finding and deleting/repointing every child row too,
    recursively, which is real extra machinery for a feature this project doesn't otherwise
    need. Explicit `MITAO`/`BADLO` on the children first keeps the rule easy to explain in
    a viva: "if something still points at it, you must move it or remove it first."
12. **Why store SHART as text instead of an AST?** `catalog.json` is JSON; a `BinaryOp`/
    `ColumnRef` tree isn't JSON-serialisable, and teaching a custom (de)serialiser for every
    AST node is a lot of machinery for one feature. Text round-trips through JSON for free,
    and `parser.parse_expression()` (cached) turns it back into a tree whenever it's needed.
    The cost: a renamed column can't be "patched" inside a saved CHECK, so the rename is
    refused instead -- a deliberate, documented trade-off, not an oversight.
13. **Why must SANDARBH's parent column be MUKHYA KUNJI/ANOKHA?** So it is guaranteed to
    already have a hash index (see "Constraints" above) -- the FK check is then a plain
    dict lookup, not a table scan, with no extra index-maintenance code required.
14. **What's still missing vs. a real DB?** B-tree indexes, MVCC, a write-ahead log, a
    cost-based optimizer, pages and a buffer pool, schema-level roles and column-level
    grants, TLS encryption, and `ON DELETE CASCADE` for foreign keys.
15. **Why is `users.json` server-wide instead of per-database, like `catalog.json`?** A
    MeraDB server (one `Instance`, one data directory) can serve several databases; a login
    is a property of the SERVER you connect to, not of any one database inside it -- the
    same split MySQL/Postgres make between server-level logins and database-level objects.
16. **Why do triggers/procedures store raw SOURCE TEXT instead of an AST, again?** The same
    reason as `SHART` and `BANAO VIEW`: `catalog.json` is JSON, and a parsed `BinaryOp`/
    `ColumnRef` tree isn't JSON-serialisable. Reparsing on every fire/call is simplicity over
    micro-optimisation -- trigger/procedure bodies are small, and `parser.parse()` is fast.
17. **Why do triggers/procedures run with invoker rights, not definer rights?** Simplicity:
    "run with whatever the calling session is already checked against" needs zero new
    machinery (it's just `self.execute_statement()`), whereas definer rights would need a
    second identity threaded through every check. The honest cost: a restricted user needs
    direct grants on every table a trigger/procedure they invoke touches, not just the one
    they wrote to -- documented plainly in `docs/LANGUAGE.md` rather than hidden.
