# 4-Week Plan: Understand It, Extend It, Defend It

## Progress

| Week | Feature | Status |
|------|---------|--------|
| 0 | Core engine: every DDL + DML command, constraints, storage, REPL | ✅ Done |
| 1 | `JAISA` (LIKE), `BEECH` (BETWEEN), `MEIN` (IN) | ✅ Done, **study it** |
| 2 | `WARNA` (DEFAULT), `SIKODO TABLE` (VACUUM) | ✅ Done, **study it** |
| 3 | Aggregates `GINO/KUL/AUSAT/NYUNTAM/ADHIKTAM`, `SAMOOH`, `JINKA`, `ALAG` | ✅ Done, **study it** |
| 3 | Workbench: full-screen terminal UI | ✅ Done |
| 4 | Hash indexes, `MILAO` joins, `SHURU/PAKKA/WAPAS` transactions, `SAMJHAO` | ✅ Done, **study it** |
| 4 | Client–server: `meradb start/stop/status/shell/workbench` | ✅ Done, **study it** |
| 5 | `SANDARBH` (FOREIGN KEY, RESTRICT), `SHART` (CHECK), `DATE`/`TAREEKH`, `VARCHAR(n)`/`NUMBER(p,s)`, `NAYA_NAAM` (RENAME), `KAHO` (output alias) | ✅ Done, **study it** |
| 4 | Project report draft (`docs/REPORT.md`) | ✅ Draft, **add your details** |
| 4 | Prove-it exercises below + demo + viva practice | ⬜ **Yours** |

The features are built, but **your grade depends on explaining them**. Each week below
has reading, a "trace it" task, and a small **prove-it exercise**. Do the exercise
yourself, without help. If you can do it, you understand the layer.

Rule for every change: write the test first (`tests/`), then change
tokenizer → parser → engine in that order. Run the tests with:

```bash
python -m unittest
```

---

## Week 1: The language front-end (tokenizer + parser)

**Read (about 6 hrs):**
- Crafting Interpreters, free online: <https://craftinginterpreters.com>
  - Ch 4 *Scanning* → `tokenizer.py`
  - Ch 5 *Representing Code* → `ast_nodes.py`
  - Ch 6 *Parsing Expressions* → the expression half of `parser.py`
- `docs/LANGUAGE.md`, especially the EBNF grammar at the bottom.

**Trace it:**
```python
from meradb.tokenizer import tokenize; from meradb.parser import parse
print(tokenize("DIKHAO * SE s JAHAN a > 1"))
print(parse("DIKHAO * SE s JAHAN a = 1 YA b = 2 AUR c = 3"))
print(parse("DIKHAO * SE s JAHAN umar BEECH 18 AUR 25"))   # see the sugar become >= AUR <=
```
- Draw the AST for `a = 1 YA b = 2 AUR c = 3` on paper. Why is `AUR` deeper?
- Study `_parse_pattern_range_or_list` in `parser.py`. Why doesn't
  `umar BEECH 18 AUR 25 AUR x = 1` get confused by two `AUR`s?

**Prove-it exercise:** add a power operator `^` (`DIKHAO 2 ^ 10 SE s` gives 1024).
It must bind tighter than `*`. Touches `ONE_CHAR_SYMBOLS`, a new parser level between
`_parse_term` and `_parse_unary`, and `_arithmetic` in the evaluator.

---

## Week 2: Storage + catalog (how data lives on disk)

**Read/watch (about 6 hrs):**
- Python `struct` docs: <https://docs.python.org/3/library/struct.html>
- CMU 15-445 *Intro to Database Systems* (Andy Pavlo), free on YouTube (CMU Database Group channel),
  the **Database Storage** lectures. Course site: <https://15445.courses.cs.cmu.edu>
- "Let's Build a Simple Database" (SQLite clone in C): <https://cstack.github.io/db_tutorial/>, parts 1–5
- `docs/ARCHITECTURE.md` → "On-disk format"

**Trace it:**
1. Insert 2 rows, delete 1, hex-dump the `.tbl` file, and label every byte by hand.
2. Run `SIKODO TABLE` and dump the file again. What disappeared?
3. Create a table with `WARNA`, then open `data/main/catalog.json`. Where is the default stored?
   Why does it survive restarting the program?

**Prove-it exercise:** add a shell command `.hexdump <table>` to `repl.py` that prints the
table file 16 bytes per line and marks each record's status byte as LIVE or DELETED.

---

## Week 3: The executor (the "brains")

**Read (about 4 hrs):**
- Crafting Interpreters Ch 7 *Evaluating Expressions*
- Wikipedia: *Null (SQL)*, the section on three-valued logic
- `engine.py` top to bottom, then `evaluator.py` and `aggregates.py`
- `docs/ARCHITECTURE.md` → "How grouping works"

**Trace it:**
- Put a `print(rows)` after `_group(...)` in `_exec_Select`, and run
  `DIKHAO shehar, GINO(*) SE s SAMOOH shehar`. Find the `("agg", "GINO(*)")` keys.
- Why does `DIKHAO GINO(*) SE s JAHAN id > 999` return one row with `0`, but the
  same query with `SAMOOH shehar` returns no rows?
- Trace `BADLO` step by step and explain the Halloween problem.

**Prove-it exercise:** add `GINO(ALAG x)` (COUNT DISTINCT). You need an optional `ALAG`
inside the call in `_parse_primary`, a `distinct` field on `FuncCall`, and a `set()` in
`aggregates.compute`. Don't forget `expr_label` so the column header reads `GINO(ALAG x)`.

---

## Week 4: Indexes, joins, transactions, client–server

**Read (about 6 hrs):**
- `docs/ARCHITECTURE.md`: "Planning", "Transactions and crash recovery", "Concurrency"
- `docs/SERVER.md`: how to run it, and the wire protocol
- CMU 15-445: the lectures on **Hash Tables**, **Joins** and **Concurrency Control**
- `planner.py`, `table.py`, then the transaction part of `engine.py` (`Instance`), then `server.py`

**Trace it:**
- `SAMJHAO DIKHAO * SE students JAHAN id = 3` vs `... JAHAN id > 3`. Why is only one an index lookup?
- Time `DIKHAO * SE big JAHAN id = 49999` on a 50,000-row table, with the index (`id`) and
  without it (a non-unique column). Put the numbers in your report.
- Run `SHURU;` in one `meradb shell`, then `DIKHAO TABLES;` in a second shell. What happens and why?
- During a transaction, open `%LOCALAPPDATA%\MeraDB\data\.wapas`. What is inside?
- Start the server with `meradb server -v` and watch each query arrive from the workbench.

**Then (last 3–4 days):**
- Fill in `docs/REPORT.md` (your name, college, screenshots, the timing numbers).
- Demo: `meradb start`, then `meradb workbench`. Use `examples/demo.mdb` as your script,
  show a JOIN, a `SAMJHAO`, and a `SHURU ... WAPAS` from two clients at once.
- Practise the viva questions below out loud.

---

## Likely viva questions

1. Walk me through what happens when I type `DIKHAO * SE s JAHAN id = 1;`.
2. What's the difference between a tokenizer and a parser? Why separate them?
3. Why does `AUR` bind tighter than `YA`? Show me where in the code.
4. Why does `JAHAN umar = KHALI` return no rows?
5. How is a row stored on disk? What happens to the bytes on DELETE? What does SIKODO do?
6. How do you enforce MUKHYA KUNJI? What's its time complexity? How would you make it faster?
7. What happens if the program crashes during `SUDHARO TABLE`?
8. Is MeraDB ACID? Which parts yes, which no?
9. What is DDL vs DML? Give examples of each in your language.
10. Why is UPDATE implemented as delete + insert?
11. How does `SAMOOH` work? Why is `DIKHAO naam, GINO(*) SE s` an error?
12. What is syntactic sugar? Give an example from MeraDB.
13. What's the difference between `JAHAN` and `JINKA`?
14. The workbench was added without changing the engine. How is that possible?
15. How does a hash index work? Why can't it answer `JAHAN id > 5`? What would?
16. Hash join vs nested loop join: when does MeraDB use which, and what are their costs?
17. What does WAPAS do on disk? What happens if the power goes off in the middle of a transaction?
18. Two clients run queries at the same time. What stops them corrupting each other's data?
19. What is `meradb.pid` for? What goes wrong without it?
20. Walk me through what travels over the network when the shell runs one query.
21. Why is `SANDARBH` (FOREIGN KEY) RESTRICT and not CASCADE? What has to change to add CASCADE?
22. `SHART (umar >= 0)` and `umar` is `KHALI` -- is the row accepted or rejected? Why, in terms
    of three-valued logic?
23. How is a DATE value stored on disk, and why can't a `date` object go straight into the
    JSON wire protocol or `catalog.json`?
24. Why is a `SHART` constraint's expression saved as TEXT in the catalog instead of as a
    parsed tree (an AST)? What does that cost you, and where do you see the cost?

## More resources (optional, for deeper curiosity)

- SQLite architecture overview: <https://www.sqlite.org/arch.html>
- *Build Your Own Database From Scratch* (James Smith): <https://build-your-own.org/database/>
- *Database Internals*, Alex Petrov (O'Reilly), the storage-engine chapters
- *Architecture of a Database System*, Hellerstein, Stonebraker & Hamilton (free PDF paper)
- Textual docs (for the TUI): <https://textual.textualize.io>
