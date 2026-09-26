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
NUMBER(p,s) length limits, RENAME and output column aliases, and is verified by 183
automated tests.

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

MeraDB has **183 automated tests** (`python -m unittest`):

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
