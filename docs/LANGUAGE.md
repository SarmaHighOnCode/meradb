# MeraDB Query Language (MQL): Reference

Keywords are **case-insensitive** (`dikhao` = `DIKHAO`). Table and column names
are also case-insensitive. Statements end with `;`. Comments start with `--`.

## Keyword ↔ SQL cheat sheet

| MeraDB            | SQL             | Meaning (Hindi)       |
|-------------------|-----------------|-----------------------|
| `BANAO`           | CREATE          | banao = make          |
| `HATAO`           | DROP            | hatao = remove        |
| `SUDHARO`         | ALTER           | sudharo = fix/modify  |
| `JODO`            | ADD             | jodo = join/add       |
| `SAAF`            | TRUNCATE        | saaf = clean          |
| `ISTEMAL`         | USE             | istemal = use         |
| `BATAO`           | DESCRIBE        | batao = tell          |
| `DAALO MEIN`      | INSERT INTO     | daalo mein = put into |
| `MAAN`            | VALUES          | maan = value          |
| `DIKHAO`          | SELECT          | dikhao = show         |
| `SE`              | FROM            | se = from             |
| `JAHAN`           | WHERE           | jahan = where         |
| `BADLO`           | UPDATE          | badlo = change        |
| `RAKHO`           | SET             | rakho = keep/put      |
| `MITAO SE`        | DELETE FROM     | mitao = erase         |
| `KRAM`            | ORDER BY        | kram = sequence       |
| `SEEDHA` / `ULTA` | ASC / DESC      | straight / reverse    |
| `SIRF`            | LIMIT           | sirf = only           |
| `ALAG`            | DISTINCT        | alag = separate       |
| `SAMOOH`          | GROUP BY        | samooh = group        |
| `JINKA`           | HAVING          | jinka = whose         |
| `JAISA`           | LIKE            | jaisa = like          |
| `BEECH ... AUR`   | BETWEEN ... AND | beech = between       |
| `MEIN (...)`      | IN (...)        | mein = in             |
| `WARNA`           | DEFAULT         | warna = otherwise     |
| `SIKODO TABLE`    | VACUUM          | sikodo = shrink       |
| `MILAO ... PAR`   | JOIN ... ON     | milao = combine, par = on |
| `BAAYAN MILAO`    | LEFT JOIN       | baayan = left         |
| `KAHO`            | AS              | kaho = call/say (output column alias) |
| `SHURU`           | BEGIN           | shuru = start         |
| `PAKKA`           | COMMIT          | pakka = confirmed     |
| `WAPAS`           | ROLLBACK        | wapas = back          |
| `SAMJHAO`         | EXPLAIN         | samjhao = explain     |
| `AUR` / `YA` / `NAHI` | AND / OR / NOT | and / or / not     |
| `HAI KHALI`       | IS NULL         | is empty              |
| `KHALI`           | NULL            | empty                 |
| `SACH` / `JHOOTH` | TRUE / FALSE    | true / false          |
| `MUKHYA KUNJI`    | PRIMARY KEY     | main key              |
| `ZAROORI`         | NOT NULL        | required              |
| `ANOKHA`          | UNIQUE          | unique                |
| `SANDARBH`        | REFERENCES (FOREIGN KEY) | sandarbh = reference |
| `SHART`           | CHECK           | shart = condition     |
| `NAYA_NAAM`       | RENAME TO       | naya naam = new name  |
| `BANAO VIEW ... KAHO` | CREATE VIEW ... AS | banao view = make a view |
| `HATAO VIEW`      | DROP VIEW       | hatao = remove        |
| `DIKHAO VIEWS`    | SHOW VIEWS (informal SQL) | dikhao = show |
| `SANYUKT`         | UNION           | sanyukt = united/combined |
| `SAAJHA`          | INTERSECT       | saajha = shared/common |
| `CHHODKAR`        | EXCEPT / MINUS  | chhodkar = leaving out |
| `SAMAAN MILAO`    | NATURAL JOIN    | samaan = same/matching |
| `DAHINA MILAO`    | RIGHT JOIN      | dahina = right         |
| `DONO MILAO`      | FULL OUTER JOIN | dono = both            |
| `PEHLA(...)`      | COALESCE(...) / NVL | pehla = first non-empty |
| `AGAR ... TAB ... WARNA ... KHATAM` | CASE WHEN ... THEN ... ELSE ... END | agar/tab/khatam = if/then/end |
| `TAKRAAV PAR BADLO` | ON CONFLICT DO UPDATE | takraav = collision |
| `ANOKHA (a, b)` / `MUKHYA KUNJI (a, b)` | composite UNIQUE / PRIMARY KEY | multi-column constraint |

## Data types

| Type          | Hinglish alias | Example      |
|---------------|-----------------|--------------|
| `INT`         | `ANK`          | `42`, `-7`   |
| `FLOAT`       | `DASHAMLAV`    | `3.14`       |
| `TEXT`        | `SHABD`        | `'Ravi'` (use `''` for a quote inside: `'Ravi''s'`) |
| `BOOL`        | `HAAN_NA`      | `SACH`, `JHOOTH` |
| `DATE`        | `TAREEKH`      | `'2024-01-31'` -- written as a `'YYYY-MM-DD'` string literal |
| `VARCHAR(n)`  | `CHAR(n)`, `VARCHAR2(n)`, `SHABD(n)` | `naam VARCHAR(20)` -- TEXT with a max length |
| `NUMBER(p,s)` | `NUMERIC(p,s)`, `DECIMAL(p,s)` | `score NUMBER(5,2)` -- an alias of FLOAT; `p`/`s` accepted, ignored |

Every type accepts an optional `(n)` or `(n, n)` after its name (`INT(11)`, `TEXT(20)`,
`NUMBER(5,2)`...). Only the first number on a TEXT-family type is kept, as its max length;
everywhere else the number(s) are parsed for familiarity with SQL and then ignored.

## DDL: Data Definition Language

```sql
BANAO DATABASE college;
ISTEMAL college;                -- switch database (default is `main`)
HATAO DATABASE college;

BANAO TABLE courses (
    id    INT  MUKHYA KUNJI,
    title TEXT ZAROORI ANOKHA
);

BANAO TABLE students (
    id     INT  MUKHYA KUNJI,
    naam   VARCHAR(30) ZAROORI,                -- TEXT with a max length
    email  TEXT ANOKHA,
    umar   ANK  WARNA 18                       -- used when an INSERT leaves umar out
           SHART (umar >= 0 AUR umar < 150),   -- CHECK: rejected only if exactly JHOOTH
    dob    DATE,                               -- 'YYYY-MM-DD' string literals
    cid    INT SANDARBH courses(id)            -- FOREIGN KEY -> courses.id
);

SUDHARO TABLE students JODO shehar TEXT;       -- ALTER TABLE ... ADD COLUMN
SUDHARO TABLE students JODO saal INT ZAROORI WARNA 1;  -- existing rows get 1
SUDHARO TABLE students HATAO COLUMN shehar;    -- ALTER TABLE ... DROP COLUMN
SUDHARO TABLE students NAYA_NAAM pupils;                    -- RENAME TABLE
SUDHARO TABLE pupils COLUMN naam NAYA_NAAM full_naam;        -- RENAME COLUMN
SAAF TABLE pupils;                             -- TRUNCATE: delete all rows, keep table
SIKODO TABLE pupils;                           -- VACUUM: reclaim space from deleted rows
HATAO TABLE pupils;                            -- DROP TABLE
HATAO TABLE courses;

DIKHAO TABLES;                                 -- list tables
BATAO students;                                -- describe a table

-- composite (multi-column) UNIQUE / PRIMARY KEY: a table-level constraint,
-- written as its own item in the column list (not attached to one column)
BANAO TABLE enrollments (
    id         INT MUKHYA KUNJI,
    student_id INT SANDARBH students(id),
    course_id  INT SANDARBH courses(id),
    grade      TEXT,
    ANOKHA (student_id, course_id)             -- a student can take a course only once
);
-- MUKHYA KUNJI (a, b) works the same way (composite PRIMARY KEY -- only one
-- PRIMARY KEY total per table, single- or multi-column)
SUDHARO TABLE enrollments JODO ANOKHA (student_id, grade);   -- ALTER TABLE ... ADD
```

## VIEWs

A view is a **named, saved `DIKHAO` query** -- it has no storage of its own. Every time
it's used, MeraDB re-runs its stored query against the CURRENT tables, so it always
reflects the latest data and schema (this is called "materializing" the view; see
`docs/ARCHITECTURE.md`).

```sql
BANAO VIEW cs_students KAHO DIKHAO naam, cgpa SE students JAHAN dept = 'CS';

DIKHAO * SE cs_students;                       -- use it just like a table in SE
DIKHAO v.naam, c.title SE cs_students v MILAO courses c PAR v.cid = c.id;  -- or in MILAO

DIKHAO VIEWS;                                  -- list views
BATAO cs_students;                             -- show its stored DIKHAO text

HATAO VIEW cs_students;
```

A view can be read from, but never written to: `DAALO`/`BADLO`/`MITAO`/`SUDHARO`/`SAAF`/
`SIKODO`/`HATAO TABLE` all refuse a view's name with a clear error. If the tables a view
selects from change shape or disappear, the view simply fails the next time it's read
(same as most real databases without a dependency check at `BANAO VIEW` time beyond a
first sanity run).

## DML: Data Manipulation Language

```sql
-- INSERT (column list optional; missing columns become KHALI)
DAALO MEIN students (id, naam, umar) MAAN (1, 'Ravi', 20), (2, 'Priya', 19);
DAALO MEIN students MAAN (3, 'Aman', 'aman@x.in', 22);

-- INSERT ... SELECT ("multi-table insert"): rows come from a query instead of MAAN
DAALO MEIN alumni (id, naam) DIKHAO id, naam SE students JAHAN umar > 25;

-- Simplified upsert: TAKRAAV PAR BADLO ("on conflict, update") -- if a row
-- collides with an EXISTING row on any UNIQUE/PRIMARY KEY column (single or
-- composite), that row is UPDATEd instead of rejected. The assignment
-- expressions see the INCOMING (attempted) row's own values, so
-- `naam = naam` means "keep the value I tried to insert".
DAALO MEIN students (id, naam) MAAN (1, 'Ravi Verma') TAKRAAV PAR BADLO naam = naam;
-- A conflict against ANOTHER ROW IN THE SAME STATEMENT is still a hard error --
-- only conflicts against rows already on disk are rescued this way.

-- SELECT
DIKHAO * SE students;
DIKHAO naam, umar + 1 SE students
    JAHAN umar >= 18 AUR (naam != 'Ravi' YA email HAI NAHI KHALI)
    KRAM umar ULTA, naam
    SIRF 10;

-- pattern / range / list
DIKHAO * SE students JAHAN naam JAISA 'R%';          -- % = any characters, _ = exactly one
DIKHAO * SE students JAHAN umar BEECH 18 AUR 25;     -- inclusive on both ends
DIKHAO * SE students JAHAN id MEIN (1, 2, 3);
DIKHAO * SE students JAHAN naam NAHI JAISA '%a%';    -- NAHI works before JAISA, BEECH, MEIN

-- aggregates: GINO (COUNT), KUL (SUM), AUSAT (AVG), NYUNTAM (MIN), ADHIKTAM (MAX)
DIKHAO GINO(*), AUSAT(umar), ADHIKTAM(umar) SE students;
DIKHAO shehar, GINO(*) SE students
    SAMOOH shehar                                    -- GROUP BY
    JINKA GINO(*) > 1                                -- HAVING: filter groups
    KRAM GINO(*) ULTA;
DIKHAO ALAG shehar SE students;                      -- DISTINCT

-- PEHLA / COALESCE: first non-KHALI argument (any number of arguments)
DIKHAO naam, PEHLA(umar, 0) SE students;

-- CASE WHEN, written AGAR ... TAB ... [AGAR ... TAB ...] [WARNA ...] KHATAM
DIKHAO naam,
    AGAR umar >= 60 TAB 'senior'
    AGAR umar >= 18 TAB 'adult'
    WARNA 'minor'
    KHATAM KAHO age_group
    SE students;
-- a KHALI condition is treated as false (like JAHAN), never raises an error

-- Subqueries: scalar (exactly 1 column, 0 or 1 row for this outer row),
-- IN-list (exactly 1 column, any number of rows), correlated or not
DIKHAO naam SE students JAHAN umar > (DIKHAO AUSAT(umar) SE students);        -- scalar
DIKHAO naam SE students JAHAN dept_id MEIN (DIKHAO id SE depts JAHAN active); -- IN-list
DIKHAO s.naam SE students s
    JAHAN s.umar > (DIKHAO AUSAT(s2.umar) SE students s2 JAHAN s2.dept_id = s.dept_id);  -- correlated:
    -- s2's subquery references the OUTER row's s.dept_id, so it reruns once per outer row

-- KAHO: rename an output column (like SQL's AS). KRAM may refer back to the alias;
-- JAHAN and JINKA cannot -- they run before the output list exists.
DIKHAO GINO(*) KAHO total, AUSAT(umar) KAHO avg_umar
    SE students
    KRAM total ULTA;

-- JOINS: give tables a short alias after their name, then use alias.column
DIKHAO s.naam, c.title
    SE students s
    MILAO courses c PAR s.cid = c.id          -- INNER JOIN: only students with a course
    JAHAN c.title != 'OS';
DIKHAO s.naam, c.title
    SE students s
    BAAYAN MILAO courses c PAR s.cid = c.id;  -- LEFT JOIN: every student; KHALI if no course
DIKHAO c.* SE students s MILAO courses c PAR s.cid = c.id;   -- all columns of one table
-- A plain `naam` works when only one table has that column; otherwise write s.naam.

DIKHAO s.naam, d.dname
    SE students s
    DAHINA MILAO depts d PAR s.dept_id = d.id;   -- RIGHT JOIN: every dept, KHALI if no students
DIKHAO s.naam, d.dname
    SE students s
    DONO MILAO depts d PAR s.dept_id = d.id;     -- FULL OUTER JOIN: unmatched rows from BOTH sides
DIKHAO naam, dname
    SE students SAMAAN MILAO depts;              -- NATURAL JOIN: no PAR -- matches every column
    -- name shared between the two tables (here: dept_id in both); errors if none are shared

-- Set operations: SANYUKT (UNION), SAAJHA (INTERSECT), CHHODKAR (EXCEPT/MINUS).
-- All three dedupe (like SQL's bare UNION/INTERSECT/EXCEPT -- no ALL variant).
-- Both sides must have the same number of columns; names don't need to match.
DIKHAO naam SE students JAHAN dept_id = 1
SANYUKT
DIKHAO naam SE teachers JAHAN dept_id = 1;

-- UPDATE
BADLO students RAKHO umar = umar + 1, email = KHALI JAHAN id = 1;

-- DELETE
MITAO SE students JAHAN umar HAI KHALI;
MITAO SE students;                             -- deletes every row
```

Operators: `=  !=  <>  <  <=  >  >=  +  -  *  /  %`, where `+` also joins two texts.

## Transactions

```sql
SHURU;                                     -- BEGIN
BADLO accounts RAKHO paisa = paisa - 500 JAHAN id = 1;
BADLO accounts RAKHO paisa = paisa + 500 JAHAN id = 2;
PAKKA;                                     -- COMMIT: keep both changes
-- or WAPAS; to undo EVERYTHING since SHURU (ROLLBACK)
```

- Everything outside `SHURU ... PAKKA` is saved immediately ("autocommit").
- A transaction covers the current database. `ISTEMAL`, `BANAO DATABASE` and
  `HATAO DATABASE` are not allowed inside one.
- While your transaction is open, other clients wait. They never see half-finished work.
- If the connection drops or the program crashes before `PAKKA`, everything is rolled back.
- The shell prompt shows `*` during a transaction: `meradb:main*>`.

## Query plans: SAMJHAO

Put `SAMJHAO` in front of a `DIKHAO`, `BADLO` or `MITAO` to see **how** MeraDB would run
it. The query itself is not run.

```sql
SAMJHAO DIKHAO * SE students JAHAN id = 3;
--  1. INDEX LOOKUP students PAR id = 3  [hash index, MUKHYA KUNJI]
--  2. FILTER  JAHAN id = 3
--  3. PROJECT  id, naam, ...

SAMJHAO DIKHAO s.naam, c.title SE students s MILAO courses c PAR s.cid = c.id;
--  1. FULL SCAN students s
--  2. HASH JOIN courses c PAR s.cid = c.id
--  3. PROJECT  s.naam, c.title
```

Every `MUKHYA KUNJI` and `ANOKHA` column automatically gets a hash index. It is used when
`JAHAN` contains `column = value` joined by `AUR`. Conditions like `>` or `YA` still scan.

## Formal grammar (EBNF)

This is exactly what `meradb/parser.py` implements. One grammar rule ≈ one method.

```ebnf
script      = statement { ";" statement } [ ";" ] ;

statement   = create_db | drop_db | use_db | create_tbl | drop_tbl | alter_tbl
            | truncate | compact | describe | show_tables | show_views
            | create_view | drop_view
            | insert | select_stmt | update | delete
            | "SHURU" | "PAKKA" | "WAPAS" | "SAMJHAO" statement ;

create_db   = "BANAO" "DATABASE" IDENT ;
drop_db     = "HATAO" "DATABASE" IDENT ;
use_db      = "ISTEMAL" [ "DATABASE" ] IDENT ;
create_tbl  = "BANAO" "TABLE" IDENT "(" table_item { "," table_item } ")" ;
table_item  = column_def | composite_constraint ;
column_def  = IDENT TYPE [ "(" INTEGER [ "," INTEGER ] ")" ]
              { "MUKHYA" "KUNJI" | "ZAROORI" | "ANOKHA" | "WARNA" literal
              | "SANDARBH" IDENT "(" IDENT ")" | "SHART" "(" expr ")" } ;
composite_constraint = ( "ANOKHA" | "MUKHYA" "KUNJI" ) "(" IDENT { "," IDENT } ")" ;
                       (* table-level: 2+ columns, distinguished from column_def by
                          lookahead -- a column_def always starts with the column's own IDENT *)
drop_tbl    = "HATAO" "TABLE" IDENT ;
alter_tbl   = "SUDHARO" "TABLE" IDENT
              ( "JODO" [ "COLUMN" ] column_def
              | "JODO" composite_constraint
              | "HATAO" [ "COLUMN" ] IDENT
              | "NAYA_NAAM" IDENT                              (* rename the table *)
              | "COLUMN" IDENT "NAYA_NAAM" IDENT ) ;            (* rename a column *)
truncate    = "SAAF" "TABLE" IDENT ;
compact     = "SIKODO" "TABLE" IDENT ;
describe    = "BATAO" [ "TABLE" ] IDENT ;                      (* also shows a VIEW's definition *)
show_tables = "DIKHAO" "TABLES" ;
show_views  = "DIKHAO" "VIEWS" ;
create_view = "BANAO" "VIEW" IDENT "KAHO" select ;              (* select's SOURCE TEXT is stored *)
drop_view   = "HATAO" "VIEW" IDENT ;

insert      = "DAALO" "MEIN" IDENT [ "(" IDENT { "," IDENT } ")" ]
              ( "MAAN" tuple { "," tuple } | select )
              [ "TAKRAAV" "PAR" "BADLO" assignment { "," assignment } ] ;
tuple       = "(" expr { "," expr } ")" ;
assignment  = IDENT "=" expr ;

select_stmt = select { set_op select } ;                        (* left-associative chaining *)
set_op      = "SANYUKT" | "SAAJHA" | "CHHODKAR" ;                (* UNION / INTERSECT / EXCEPT *)
select      = "DIKHAO" [ "ALAG" ] item { "," item } "SE" IDENT [ IDENT ]
              { join }
              [ "JAHAN" expr ]
              [ "SAMOOH" expr { "," expr } ]
              [ "JINKA" expr ]
              [ "KRAM" order_item { "," order_item } ]
              [ "SIRF" INTEGER ] ;
join        = ( [ "BAAYAN" | "DAHINA" | "DONO" ] "MILAO" IDENT [ IDENT ] "PAR" expr )
            | ( "SAMAAN" "MILAO" IDENT [ IDENT ] ) ;   (* NATURAL: no PAR -- synthesised at bind time *)
item        = "*" | IDENT "." "*" | expr [ "KAHO" IDENT ] ;   (* KAHO = output column alias *)
order_item  = expr [ "SEEDHA" | "ULTA" ] ;
update      = "BADLO" IDENT "RAKHO" assignment { "," assignment } [ "JAHAN" expr ] ;
delete      = "MITAO" "SE" IDENT [ "JAHAN" expr ] ;

(* expressions, lowest precedence first *)
expr        = or_expr ;
or_expr     = and_expr { "YA" and_expr } ;
and_expr    = not_expr { "AUR" not_expr } ;
not_expr    = "NAHI" not_expr | comparison ;
comparison  = additive [ ( "=" | "!=" | "<" | "<=" | ">" | ">=" ) additive
                       | "HAI" [ "NAHI" ] "KHALI"
                       | [ "NAHI" ] "JAISA" additive
                       | [ "NAHI" ] "BEECH" additive "AUR" additive
                       | [ "NAHI" ] "MEIN" "(" ( expr { "," expr } | "DIKHAO" select_body ) ")" ] ;
additive    = term { ( "+" | "-" ) term } ;
term        = unary { ( "*" | "/" | "%" ) unary } ;
unary       = "-" unary | primary ;
primary     = NUMBER | STRING | "SACH" | "JHOOTH" | "KHALI"
            | case_expr | coalesce_expr
            | IDENT "(" ( "*" | expr ) ")"               (* aggregate call *)
            | IDENT [ "." IDENT ]                        (* column, or alias.column *)
            | "(" ( expr | "DIKHAO" select_body ) ")" ;   (* the 2nd form: a scalar subquery *)
case_expr   = "AGAR" expr "TAB" expr { "AGAR" expr "TAB" expr } [ "WARNA" expr ] "KHATAM" ;
coalesce_expr = ( "PEHLA" | "COALESCE" ) "(" expr { "," expr } ")" ;
literal     = [ "-" ] NUMBER | STRING | "SACH" | "JHOOTH" | "KHALI" ;
```

`BEECH` and `MEIN` are **syntactic sugar**: the parser rewrites them into things the
evaluator already knows (`x >= a AUR x <= b`, and `x = a YA x = b ...`).

## Things to know

- **KHALI is not a value, it's "unknown".** `umar = KHALI` is never true. Use `umar HAI KHALI`.
- **Rows have no guaranteed order.** After `BADLO`, an updated row moves to the end of the
  file. If order matters, use `KRAM`.
- **`MUKHYA KUNJI`** = `ZAROORI` + `ANOKHA`. Only one per table.
- **`ANOKHA` allows many KHALI values**, like SQL.
- **`JAISA` ignores upper/lower case**: `'r%'` matches `Ravi`.
- **Aggregates skip KHALI.** `GINO(*)` counts rows, `GINO(cgpa)` counts non-KHALI cgpa values.
  On zero rows `GINO` gives 0 and the others give KHALI. The English names
  `COUNT/SUM/AVG/MIN/MAX` also work.
- **In a grouped query, every plain column must be in `SAMOOH`.** `DIKHAO naam, GINO(*) SE s`
  is an error, because one output row stands for many rows, each with its own `naam`.
- **`SANDARBH` (FOREIGN KEY) is RESTRICT, not CASCADE.** Deleting/changing a parent row that a
  child still points at is an error -- you must remove or repoint the child rows first. There is
  no `ON DELETE CASCADE` (future work). The parent column must be `MUKHYA KUNJI`/`ANOKHA`, so
  the child-side check is an O(1) hash-index lookup, same as uniqueness checks.
- **`SHART` (CHECK) uses NULL semantics, not boolean ones.** A row is rejected only when the
  expression is exactly `JHOOTH`. If it's `KHALI` (unknown -- e.g. because a column it reads is
  `KHALI`), the row is accepted, exactly like a `JAHAN` clause treats `KHALI`.
- **`SHART`'s text is stored as-is, not as a parsed tree**, because `catalog.json` is JSON and an
  AST isn't JSON-serialisable. That's also why renaming a column that a `SHART` uses is refused:
  the saved text can't be rewritten, only re-parsed.
- **Dates are written as strings.** `DATE`/`TAREEKH` columns take a `'YYYY-MM-DD'` string
  literal; MeraDB converts it to a real date and validates the format. Comparing a `DATE` column
  to a string literal (`JAHAN dob > '2005-01-01'`) also works. Arithmetic and `JAISA` on dates
  are errors; `NYUNTAM`/`ADHIKTAM` work, `KUL`/`AUSAT` don't (same as for TEXT/BOOL).
- **`SANYUKT`/`SAAJHA`/`CHHODKAR` only have the dedupe form** (like bare SQL
  `UNION`/`INTERSECT`/`EXCEPT`) -- there is no `ALL` variant that keeps duplicates.
- **`SAMAAN MILAO` (NATURAL JOIN) may repeat shared columns in `*`.** Getting the JOIN itself
  correct (rows filtered by the synthesised condition) matters far more than `*` cosmetics, so
  `DIKHAO * SE a SAMAAN MILAO b` currently lists a shared column once per side rather than once
  -- write the columns out explicitly if you want each name only once.
- **A view is not a table.** It has no rows of its own, no index, and always does a full scan of
  its freshly re-run query -- see the VIEWs section above and `docs/ARCHITECTURE.md`.
