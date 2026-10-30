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
| `BANAO OR REPLACE VIEW` / `BANAO BADAL VIEW` | CREATE OR REPLACE VIEW | badal = replace |
| `SUDHARO VIEW v KAHO ...` | ALTER VIEW v AS ... | sudharo = alter |
| `... WITH [LOCAL\|CASCADED] CHECK OPTION` | WITH CHECK OPTION | English words, no Hinglish equivalent |
| `SHOW ...` / `DIKHAO GRANTS\|TRIGGERS\|PROCEDURES\|DATABASES` | SHOW GRANTS / TRIGGERS / PROCEDURE STATUS / DATABASES | see "Introspection: SHOW" |
| `SANYUKT`         | UNION           | sanyukt = united/combined |
| `SAAJHA`          | INTERSECT       | saajha = shared/common |
| `CHHODKAR`        | EXCEPT / MINUS  | chhodkar = leaving out |
| `SAMAAN MILAO`    | NATURAL JOIN    | samaan = same/matching |
| `DAHINA MILAO`    | RIGHT JOIN      | dahina = right         |
| `DONO MILAO`      | FULL OUTER JOIN | dono = both            |
| `PEHLA(...)`      | COALESCE(...) / NVL | pehla = first non-empty |
| `SIRF n OFFSET m` / `SIRF m, n` | LIMIT n OFFSET m / LIMIT m, n | skip m rows, then show n |
| `MILAO t USING (c)` | JOIN t USING (c) | join on the shared column(s) `c` |
| `EXISTS (...)` / `ANY` / `SOME` / `ALL` | EXISTS / ANY / SOME / ALL | subquery tests (English words) |
| `SE (DIKHAO ...) t` | FROM (SELECT ...) t | derived table: a query used as a table |
| `DIKHAO NOW(), 1 + 1` (no `SE`) | SELECT NOW(), 1 + 1 | one row computed from the expressions alone |
| `LIMIT n [OFFSET m]` / `LIMIT m, n` | the same | English spelling of `SIRF` (accepted wherever `SIRF` is) |
| `a / b`, `a DIV b` | `/` and `DIV` | `/` is exact (`7 / 2` = 3.5, `6 / 2` = 3); `DIV` truncates (`7 DIV 2` = 3) |
| `AUTO_INCREMENT` / `AUTO_ANK` | AUTO_INCREMENT | an INT column numbers itself 1, 2, 3... |
| `ENUM('a', 'b')` | ENUM('a', 'b') | text that must be one of the listed members |
| `DATETIME` / `TIMESTAMP` | DATETIME / TIMESTAMP | `'YYYY-MM-DD HH:MM:SS'` |
| `BANAO TABLE t KAHO DIKHAO ...` / `... AS DIKHAO ...` | CREATE TABLE t AS SELECT ... | a new table from a query's columns and rows |
| `BANAO TABLE t2 LIKE t1` | CREATE TABLE t2 LIKE t1 | same columns and constraints, no rows |
| `SANDARBH p(c) ON DELETE CASCADE` | FOREIGN KEY ... ON DELETE CASCADE | also `SET NULL`, `RESTRICT`, `NO ACTION`; `ON UPDATE` too |
| `JODO [CONSTRAINT n] FOREIGN KEY (c) SANDARBH p(c)` / `HATAO FOREIGN KEY n` | ADD / DROP FOREIGN KEY | named foreign keys, after the table exists |
| `SUDHARO TABLE t CHANGE old new type` / `MODIFY col type` | ALTER TABLE ... CHANGE / MODIFY | rename + retype / retype a column |
| `JODO col type FIRST` / `AFTER other` | ADD COLUMN ... FIRST / AFTER | where the new column goes |
| `BADAL DAALO MEIN` / `REPLACE INTO` | REPLACE INTO | insert, first deleting rows that collide on a key |
| `UPPER()`, `CONCAT()`, `ROUND()`, `NOW()`, `CAST(x AS t)` ... | the same names | scalar functions (English words) |
| `a \|\| b`        | a \|\| b (string concatenation) | joins two texts |
| `AGAR ... TAB ... WARNA ... KHATAM` | CASE WHEN ... THEN ... ELSE ... END | agar/tab/khatam = if/then/end |
| `TAKRAAV PAR BADLO` | ON CONFLICT DO UPDATE | takraav = collision |
| `ANOKHA (a, b)` / `MUKHYA KUNJI (a, b)` | composite UNIQUE / PRIMARY KEY | multi-column constraint |
| `BANAO USER ... GUPT` | CREATE USER ... IDENTIFIED BY | gupt = secret (password) |
| `HATAO USER`      | DROP USER       | hatao = remove         |
| `ADHIKAR DO ... PAR ... KO` | GRANT ... ON ... TO | adhikar do = give privilege |
| `ADHIKAR WAPAS ... PAR ... SE` | REVOKE ... ON ... FROM | adhikar wapas = take back privilege |
| `SAB` (after `ADHIKAR DO`) | ALL             | sab = all              |
| `BANAO ROLE r` / `HATAO ROLE r` | CREATE ROLE / DROP ROLE | role has no Hinglish word |
| `ADHIKAR DO ROLE r KO u` / `ADHIKAR WAPAS ROLE r SE u` | GRANT r TO u / REVOKE r FROM u | the `ROLE` word is required |
| `SET ROLE r` / `SET ROLE SAB` / `SET ROLE NONE` | SET ROLE | picks the active roles of the session |
| `... WITH GRANT OPTION` | WITH GRANT OPTION | the holder may pass the privilege on |
| `DIKHAO (c1, c2)` inside a grant | SELECT (c1, c2) | column-level privileges |
| `BANAO TRIGGER`   | CREATE TRIGGER  | banao = make            |
| `HATAO TRIGGER`   | DROP TRIGGER    | hatao = remove          |
| `PEHLE` / `BAAD`  | BEFORE / AFTER  | pehle/baad = before/after |
| `NAYA` / `PURANA` (alias inside a trigger body) | NEW / OLD | naya/purana = new/old |
| `BANAO PROCEDURE` | CREATE PROCEDURE | banao = make           |
| `HATAO PROCEDURE` | DROP PROCEDURE  | hatao = remove          |
| `CHALAO`          | CALL            | chalao = run/execute    |

## Data types

| Type          | Hinglish alias | Example      |
|---------------|-----------------|--------------|
| `INT`         | `ANK`          | `42`, `-7`   |
| `FLOAT`       | `DASHAMLAV`    | `3.14`       |
| `TEXT`        | `SHABD`        | `'Ravi'` (use `''` for a quote inside: `'Ravi''s'`) |
| `BOOL`        | `HAAN_NA`      | `SACH`, `JHOOTH` |
| `DATE`        | `TAREEKH`      | `'2024-01-31'` -- written as a `'YYYY-MM-DD'` string literal |
| `DATETIME`    | `TIMESTAMP`    | `'2024-01-31 09:30:00'` (`'2024-01-31'` means midnight) -- see below |
| `ENUM('a','b')` | (none)       | `kind ENUM('open','closed')` -- TEXT that must be one of the members |
| `VARCHAR(n)`  | `CHAR(n)`, `VARCHAR2(n)`, `SHABD(n)` | `naam VARCHAR(20)` -- TEXT with a max length |
| `NUMBER(p,s)` | `NUMERIC(p,s)`, `DECIMAL(p,s)` | `score NUMBER(5,2)` -- an alias of FLOAT; `p`/`s` accepted, ignored |

Every type accepts an optional `(n)` or `(n, n)` after its name (`INT(11)`, `TEXT(20)`,
`NUMBER(5,2)`...). Only the first number on a TEXT-family type is kept, as its max length;
everywhere else the number(s) are parsed for familiarity with SQL and then ignored.

**`DATETIME`** is checked when stored and always shown as `'YYYY-MM-DD HH:MM:SS'`. A value may be
written `'2024-01-31'` (midnight), `'2024-01-31 09:30'` or with a `T` instead of the space;
anything that is not a real date and time is an error. `NOW()` can be inserted straight into a
`DATETIME` column, and `YEAR()`, `MONTH()`, `DAY()`, `DATEDIFF()`, `DATE_FORMAT()` and
`CAST(x AS DATETIME)` accept `DATETIME` values. It is kept as text of that one shape, so
`KRAM`, `NYUNTAM`/`ADHIKTAM`, `SAMOOH`, `ANOKHA` and comparisons all work; a date-only
`'2024-01-31'` next to a `DATETIME` means midnight of that day.

**`ENUM('a', 'b', ...)`** is stored as text; any other value is an error that lists the members.
`KHALI` is allowed unless the column is `ZAROORI`. It is a TEXT column underneath (comparisons are
text comparisons), and `BATAO` shows it as `ENUM('a','b')`.

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

### AUTO_INCREMENT, ENUM, DATETIME

```sql
BANAO TABLE tickets (
    id      INT AUTO_INCREMENT MUKHYA KUNJI,          -- AUTO_ANK works too
    status  ENUM('open', 'closed') WARNA 'open',
    opened  DATETIME
);
DAALO MEIN tickets (opened) MAAN (NOW());             -- id = 1, status = 'open'
DAALO MEIN tickets (id, opened) MAAN (KHALI, '2024-05-01 10:30:00');   -- KHALI id: next number
DAALO MEIN tickets (status) MAAN ('stuck');           -- error: Column 'status' ENUM('open', 'closed') hai ...
```

An `AUTO_INCREMENT` column must be `INT`, there is one per table, and it can never be `KHALI`
(it implies `ZAROORI`; combine it with `MUKHYA KUNJI` as above). When an `INSERT` leaves the column
out or gives `KHALI`, the row gets the largest value in the table + 1 (1 for an empty table); a
value you give yourself is kept and later rows continue after it. Numbers are not remembered after
the top row is deleted: the largest remaining value + 1 is used.

### CREATE TABLE ... AS SELECT / LIKE

```sql
BANAO TABLE toppers KAHO DIKHAO id, naam, cgpa * 10 KAHO score SE students JAHAN cgpa > 9;
-- AS works instead of KAHO; the table takes the query's column names, types and rows, no constraints
BANAO TABLE students_copy LIKE students;     -- the same columns AND constraints, no rows
```

`KAHO DIKHAO` names the columns after the query's output, so each must be a plain name: give an
expression an alias (`COUNT(*)` alone is refused -- write `COUNT(*) KAHO total`) and no two may be
the same. A column keeps its type from its source (an expression's type comes from its values; `NOW()`
is a `DATETIME`). `LIKE` copies `MUKHYA KUNJI`, `ANOKHA`, `ZAROORI`, `WARNA`, `SHART`, `ENUM`,
`AUTO_INCREMENT` and the `SANDARBH` definitions as written; the rows are not copied.

### Foreign key actions

```sql
BANAO TABLE emp (
    id  INT MUKHYA KUNJI,
    did INT SANDARBH depts(id) ON DELETE CASCADE ON UPDATE CASCADE
);
-- ON DELETE / ON UPDATE take CASCADE | SET NULL | RESTRICT | NO ACTION (ON UPDATE too); RESTRICT is the default
BANAO TABLE logs (
    id INT MUKHYA KUNJI, did INT,
    CONSTRAINT fk_logs_dept FOREIGN KEY (did) SANDARBH depts(id) ON DELETE SET NULL   -- table-level form
);

SUDHARO TABLE emp HATAO FOREIGN KEY emp_ibfk_1;                    -- or: HATAO CONSTRAINT emp_ibfk_1
SUDHARO TABLE emp JODO CONSTRAINT fk_dept FOREIGN KEY (did) SANDARBH depts(id) ON DELETE CASCADE;
SUDHARO TABLE emp JODO FOREIGN KEY (did) SANDARBH depts(id);       -- name made up: emp_ibfk_<n>
```

* `CASCADE` on delete deletes the child rows too; on update the child rows follow the parent's new
  value. Cascades go on through further tables and self-references. `SET NULL` sets the child column
  to `KHALI` and needs a column that may be `KHALI` (not `ZAROORI`, not `MUKHYA KUNJI`); this is checked
  when the foreign key is defined. `RESTRICT` and `NO ACTION` refuse while a child row still points at the value.
* A statement either does all of it or nothing: the knock-on changes are worked out and checked
  first (a failure anywhere, such as a cascaded row breaking its own `SHART`, changes no table).
  Triggers fire for the rows you named, not for the cascaded ones (as in MySQL).
* Every foreign key has a name -- the one you give, or `<table>_ibfk_<n>` (1, 2, ... in column order; also
  for tables written before names existed). `BATAO` shows a name you chose in brackets (`SANDARBH depts(id) [fk_dept]`; the automatic ones stay hidden)
  and `HATAO FOREIGN KEY name` uses it. Names are unique in a database, and a column holds one foreign key.
  `JODO FOREIGN KEY` checks the rows already in the table.

### ALTER TABLE: CHANGE, MODIFY, AFTER / FIRST

```sql
SUDHARO TABLE students CHANGE naam full_naam VARCHAR(60) ZAROORI;   -- rename + new definition
SUDHARO TABLE students MODIFY umar FLOAT;                           -- same name, new type
SUDHARO TABLE students JODO shehar TEXT AFTER naam;                 -- column goes after naam
SUDHARO TABLE students JODO roll INT FIRST;                         -- ...or first
```

`CHANGE` / `MODIFY` convert every existing value to the new type (like `CAST`: `'12'` becomes `12`,
a number becomes its text) and then check the whole table against the new definition: a value
that cannot convert (`'abc'` to `INT`), a `KHALI` in a column that is now `ZAROORI`, two equal values in a
column that is `ANOKHA`, a value outside the new `ENUM`, a broken `SHART` -- any of these refuses the
statement and leaves the table unchanged. The new definition decides the type, `ZAROORI`, `WARNA`, length,
`ENUM` and `AUTO_INCREMENT`; `MUKHYA KUNJI`, `ANOKHA`, `SANDARBH` and `SHART` stay as they were unless the
new definition gives its own `SANDARBH` / `SHART`. Composite `ANOKHA (...)` / `MUKHYA KUNJI (...)` groups and
`SANDARBH` columns elsewhere that name the column follow a rename. Refused with a message: renaming a
column that a `SHART` mentions (like `COLUMN ... NAYA_NAAM`), and changing the type of a column another
table's `SANDARBH` points at. `JODO` carries the table's composite `ANOKHA` / `MUKHYA KUNJI` groups along.

### Indexes

```sql
BANAO INDEX ix_dept PAR employees (dept_id);              -- CREATE INDEX
BANAO INDEX ix_dt PAR employees (dept_id, job_title);     -- composite
BANAO ANOKHA INDEX ux PAR assignments (emp_id, project_id, role);   -- CREATE UNIQUE INDEX: also enforces uniqueness
SHOW INDEX FROM employees;      -- (also SHOW INDEXES / SHOW KEYS) Table, Non_unique, Key_name, Seq_in_index, Column_name
HATAO INDEX ix_dept PAR employees;                        -- DROP INDEX ... ON
```

`BANAO [ANOKHA] INDEX IF NOT EXISTS` and `HATAO INDEX IF EXISTS` do nothing when the answer is "already so". A unique index
checks the rows already in the table, and `HATAO INDEX` lifts the uniqueness again. A plain index is a catalogue entry:
`SHOW INDEX` lists it and it follows a column rename, but a lookup on it still scans the table (only `MUKHYA KUNJI` / `ANOKHA`
columns have the hash index that `SAMJHAO` reports). Index statements are superuser-only.

### IF [NOT] EXISTS

`BANAO DATABASE | TABLE | USER | ROLE IF NOT EXISTS name` and `HATAO DATABASE | TABLE | VIEW | USER | ROLE IF EXISTS name`
leave things alone instead of failing. (`BANAO TRIGGER/PROCEDURE/FUNCTION IF NOT EXISTS` and the matching `HATAO ... IF EXISTS` already existed.)

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

Redefining a view:

```sql
BANAO OR REPLACE VIEW cs_students KAHO DIKHAO naam SE students;   -- create, or replace if it exists
BANAO BADAL VIEW cs_students KAHO DIKHAO naam SE students;        -- same thing, Hinglish spelling
SUDHARO VIEW cs_students KAHO DIKHAO naam, cgpa SE students;      -- ALTER VIEW: the view must already exist
```

Plain `BANAO VIEW` still refuses a name that is taken. A view may not select from itself.

### Updatable views

`DAALO MEIN` / `BADLO` / `MITAO SE` work **through a simple view** by being rewritten onto its
one base table. A view is updatable when it is a plain `DIKHAO` of ONE real table with plain
column references: no `ALAG`, no joins, no `GROUP BY`/`HAVING`, aggregates, `LIMIT`/`OFFSET`,
set operations or sub-select source, no computed columns, and not a view over another view. Then:

- the view's `JAHAN` is ANDed into every `BADLO`/`MITAO` filter, so only visible rows are touched;
- view column names and aliases are mapped back to the base columns;
- base columns the view does not expose get their `WARNA` default, NULL or AUTO_INCREMENT value
  (a `ZAROORI` column with no default makes the insert fail, exactly as on the table).

```sql
BANAO VIEW cs_students KAHO DIKHAO naam, cgpa SE students JAHAN dept = 'CS'
    WITH CHECK OPTION;
DAALO MEIN cs_students MAAN ('Asha', 8.1);     -- ok, if the table's other columns allow NULL/default
BADLO cs_students SET cgpa = 9 JAHAN naam = 'Asha';
MITAO SE cs_students JAHAN naam = 'Asha';
```

`WITH CHECK OPTION` (`WITH LOCAL CHECK OPTION` and `WITH CASCADED CHECK OPTION` are accepted and
behave the same, since views over views are not updatable) rejects any inserted or updated
row that would not satisfy the view's `JAHAN`. It can only be put on an updatable view.
A view that is not updatable keeps the clear error
`'v' ek VIEW hai, table nahi -- isme DAALO/BADLO/MITAO nahi kar sakte`, and `SAAF`/`SIKODO`/
`SUDHARO TABLE`/`HATAO TABLE` always refuse a view's name. Writing through a view needs the
matching privilege **on the view**, not on the base table. `SHOW CREATE VIEW` and `BATAO` still
work on any view.

If the tables a view selects from change shape or disappear, the view simply fails the next
time it's read (same as most real databases without a dependency check at `BANAO VIEW` time
beyond a first sanity run).

## Introspection: SHOW

`SHOW` is a *soft* word (a column or table may still be called `show`). Everything here is
read-only; Hinglish spellings are given where they exist.

```sql
SHOW TABLES;                    -- = DIKHAO TABLES
SHOW VIEWS;                     -- = DIKHAO VIEWS
SHOW DATABASES;                 -- = DIKHAO DATABASES
SHOW TRIGGERS;                  -- = DIKHAO TRIGGERS: trigger, timing, event, table, body
SHOW PROCEDURE STATUS;          -- = DIKHAO PROCEDURES: name, database, created (all databases)
SHOW COLUMNS FROM students;     -- = BATAO students   (FIELDS / SE / IN also accepted)
SHOW CREATE TABLE students;     -- a re-runnable BANAO TABLE: types, MUKHYA KUNJI, ANOKHA, ZAROORI,
                                -- WARNA, AUTO_INCREMENT, ENUM, SHART, SANDARBH with ON DELETE/UPDATE
SHOW CREATE VIEW cs_students;   -- a re-runnable BANAO VIEW (with its CHECK OPTION)
SHOW GRANTS;                    -- = DIKHAO GRANTS: your own grants
SHOW GRANTS FOR ravi;           -- = DIKHAO GRANTS FOR ravi (other users: superuser only)
```

`SHOW GRANTS` returns one column, `Grants for ravi`, with rows such as
`GRANT SELECT, INSERT ON college.students TO ravi`, `GRANT SELECT (naam, cgpa) ON college.students TO ravi`
and `GRANT staff TO ravi` (a role grant); `WITH GRANT OPTION` is appended where it applies.
`SHOW TRIGGERS` lists the current database's triggers. Procedure creation times are kept in a small
`procedures.meta.json` beside `catalog.json` (procedures made before it existed show KHALI).
The workbench tree and plain `DIKHAO TABLES` / `DIKHAO VIEWS` behave as before.

## Users & privileges

A small, **server-wide** user store (one `users.json` at the top of the data folder, not
one per database -- see `docs/ARCHITECTURE.md`). Passwords are hashed
(`hashlib.pbkdf2_hmac`, stdlib only), never stored in the clear.

```sql
BANAO USER ravi GUPT 'secret123';         -- CREATE USER ravi, password 'secret123'
HATAO USER ravi;                          -- DROP USER

ADHIKAR DO DIKHAO, DAALO PAR students KO ravi;   -- GRANT SELECT, INSERT ON students TO ravi
ADHIKAR DO SAB PAR students KO ravi;             -- GRANT ALL (all 4 privileges) ...
ADHIKAR WAPAS DIKHAO PAR students SE ravi;       -- REVOKE SELECT ON students FROM ravi
```

The four grantable privileges are named after the statement verbs themselves: `DIKHAO`
(SELECT), `DAALO` (INSERT), `BADLO` (UPDATE), `MITAO` (DELETE). A view participates in the
exact same `database.name` grant keyspace as a table -- granting `DIKHAO` on a view's name
lets a restricted user read it **without** needing direct grants on the tables the view
itself queries (same trick real databases use views for).

**How a session becomes "restricted":** an embedded `Engine` (local mode, every test in
this project, `--local` on the CLI) never has a username at all, so it is always a
**superuser** -- unaffected by any of this, 100% backward compatible. Only a *server*
client that connects with `-U/--user` (see `docs/SERVER.md`) authenticates as a specific
user and becomes subject to privilege checks from that point on.

### Roles, grant option and column grants

```sql
BANAO ROLE staff;                                  -- CREATE ROLE (nobody can log in as a role)
ADHIKAR DO DIKHAO, DAALO PAR students KO staff;    -- grant to the role
ADHIKAR DO ROLE staff KO ravi;                     -- GRANT staff TO ravi   (ROLE is required)
ADHIKAR WAPAS ROLE staff SE ravi;
HATAO ROLE staff;

SET ROLE staff;     -- only staff's grants (plus ravi's own) are in force for this session
SET ROLE SAB;       -- every role granted to ravi (the default)
SET ROLE NONE;      -- only ravi's own grants

ADHIKAR DO DIKHAO PAR students KO ravi WITH GRANT OPTION;
ADHIKAR DO DIKHAO (naam, cgpa), BADLO (cgpa) PAR students KO ravi;   -- column-level
```

- A session's effective privileges are the user's own grants plus those of its active roles
  (by default every granted role; `SET ROLE` narrows that for the session). `SET ROLE` can only
  name roles granted to the user, and is open to every user.
- English privilege names are accepted everywhere: `SELECT`, `INSERT`, `UPDATE`, `DELETE`,
  `ALL [PRIVILEGES]`. `MITAO` cannot be column-level.
- `WITH GRANT OPTION`: a non-superuser who holds a privilege with the option may grant it on to
  others (users or roles), never more than they hold. Revoke stays superuser-only and does not
  cascade; a table-wide revoke also removes the column grants on that table.
- Column grants: with only `DIKHAO (c1, c2)`, the select list, `JAHAN`, `KRAM` and the other
  clauses may reference only those columns (`*` is refused); `BADLO` checks its `SET` targets and
  `DAALO MEIN t (cols)` its column list. Sub-selects and derived tables are checked the same way.
- `users.json` stays backward compatible: roles and column grants are extra keys written only when
  used (`roles`, `grant_option`, `column_grants`, `column_grant_option`; a role is an entry with
  `"role": true` and no password hash). Older files load unchanged.

**Honest scope (see the top of this doc's intro and `docs/ARCHITECTURE.md`):**
- Grants are per `(database, table)`, optionally narrowed to columns. There are no schema-level
  grants, no `REVOKE ... CASCADE`, and no role hierarchy (a role cannot be granted to a role).
- Once a session HAS authenticated as a (non-super) user, it can run **only**
  `DIKHAO`/`DAALO`/`BADLO`/`MITAO` against tables/views it holds the matching privilege
  on, plus `ISTEMAL` (switch database), `SET ROLE`, `SHOW GRANTS` for itself and `ADHIKAR DO` where
  it holds the grant option. Every DDL statement (`BANAO`/`HATAO`/`SUDHARO`/`SAAF`/`SIKODO TABLE`,
  `VIEW`, `TRIGGER`, `PROCEDURE`), user/role management, `ADHIKAR WAPAS`, schema listing
  (`DIKHAO TABLES` / `SHOW ...` and friends) and every transaction command
  (`SHURU`/`PAKKA`/`WAPAS`) is **superuser-only** from that point -- kept simple and strict
  rather than a fine-grained "who can ALTER what" model.
- A trigger or stored procedure body runs with the **invoking session's own privileges**
  (no "definer rights") -- a restricted user needs grants on every table a trigger/procedure
  they trigger touches, not just the one they directly wrote to.
- There is no password rotation/expiry and no account lockout -- this is a teaching-project
  user store, not a production one.

`SET DEFAULT ROLE ALL | NONE | r [, r] TO user` chooses the roles a session of that user starts with (kept in `users.json`;
the default is every granted role). The session can still change them with `SET ROLE`.

## Triggers

```sql
BANAO TRIGGER before_insert_students PEHLE DAALO PAR students SHURU
    BADLO log_table RAKHO events = events + 1 JAHAN naam = 'insert_count';
KHATAM;

HATAO TRIGGER before_insert_students;
```

`PEHLE`/`BAAD` (BEFORE/AFTER) combine with `DAALO`/`BADLO`/`MITAO` (INSERT/UPDATE/DELETE)
on one table. The body is one or more `;`-terminated statements between `SHURU` and
`KHATAM` (the same BEGIN/END keywords used for transactions -- a different grammar
position, no ambiguity), parse-validated when the trigger is created.

Inside the body, `NAYA.column`/`PURANA.column` (NEW/OLD) refer to the affected row's new
and old values -- exactly like a table alias, substituted with the actual value right
before the statement runs:

```sql
BANAO TRIGGER audit_umar BAAD BADLO PAR students SHURU
    DAALO MEIN audit MAAN (NAYA.naam, PURANA.umar, NAYA.umar);
KHATAM;
```

- A `PEHLE DAALO`/`BAAD DAALO` trigger only has `NAYA` (there is no old row yet).
- A `PEHLE MITAO`/`BAAD MITAO` trigger only has `PURANA` (the row is going away).
- A `BADLO` trigger has both.
- Multiple triggers on the same `(timing, event, table)` fire **in the order they were
  created**.

**Procedural bodies.** A trigger body may use everything a procedure body can (see
[Stored procedures](#stored-procedures)): `DECLARE`, `SET`, `IF`, loops, `SIGNAL`.

```sql
BANAO TRIGGER neg PEHLE DAALO PAR t SHURU
    IF NAYA.v < 0 THEN SIGNAL 'v negative nahi ho sakta'; END IF;      -- AGAR ... TAB ... KHATAM AGAR also works
KHATAM;

BANAO TRIGGER clamp PEHLE DAALO PAR t SHURU
    IF NAYA.v > 100 THEN RAKHO NAYA.v = 100; END IF;                   -- SET NAYA.v = 100 is the same
KHATAM;
```

- **`SIGNAL 'message'`** stops the statement with that message. Everything the statement did
  is undone, including rows already written and what earlier `BAAD` triggers logged. (The
  data change is undone; so is the rest of a multi-row `DAALO`.)
- **`RAKHO NAYA.col = expr`** in a `PEHLE DAALO`/`PEHLE BADLO` trigger changes the row about
  to be written. The new value is type-checked and checked for `NOT NULL`/unique like any
  other. `PURANA` is read-only; `BAAD` triggers cannot change `NAYA`.
- Several triggers on the same `(timing, event, table)` fire in creation order.
- `BANAO TRIGGER IF NOT EXISTS name ...` does nothing if the trigger exists;
  `HATAO TRIGGER IF EXISTS name` does nothing if it does not.
- A trigger fires **once per affected row**.
- A `PEHLE` (BEFORE) trigger's error aborts the whole outer `DAALO`/`BADLO`/`MITAO`.
- `TAKRAAV PAR BADLO` (upsert): a row that collides and gets updated fires `BADLO` triggers;
  a row freshly inserted fires `DAALO` triggers.
- `SAMJHAO` (EXPLAIN) is trigger-unaware.
- A trigger runs with the invoking session's own privileges.

## Stored procedures

```sql
BANAO PROCEDURE badhao_umar (p_id INT, p_kitna INT) SHURU
    BADLO students RAKHO umar = umar + p_kitna JAHAN id = p_id;
KHATAM;

CHALAO badhao_umar(1, 2);

HATAO PROCEDURE badhao_umar;
```

A stored procedure is a **named, parameterised sequence of statements** -- a macro, not a
procedural language. Parameters are declared with a type (any of the usual column types),
purely so `CHALAO`'s arguments get `coerce()`d to that type -- a clean type-mismatch error
instead of a confusing one deep inside the first statement that uses the wrong value.
`CHALAO`'s arguments must be constant expressions (literals, arithmetic on literals, ...):
there is no outer row to reference a column against at a bare `CHALAO` call site.

### Procedure bodies

A body can be a plain list of statements (as above) or a small program. Keywords come in
English and Hinglish forms (`AGAR`/`IF`, `TAB`/`THEN`, `WARNA`/`ELSE`, `JAB TAK`/`WHILE`).

```sql
BANAO PROCEDURE jod (IN n INT, OUT total INT) SHURU
    DECLARE i INT DEFAULT 1;
    SET total = 0;
    WHILE i <= n DO
        SET total = total + i;
        SET i = i + 1;
    END WHILE;
KHATAM;

CHALAO jod(10, @t);        -- returns the OUT values as a row
DIKHAO @t;                 -- 55
```

| Construct | Notes |
|---|---|
| `DECLARE x TYPE [DEFAULT expr];` | local variable |
| `SET x = expr;` / `RAKHO x = expr;` | local, `@user` variable or parameter |
| `IF c THEN ... ELSEIF c THEN ... ELSE ... END IF;` | |
| `WHILE c DO ... END WHILE;` | also `JAB TAK c KARO ... KHATAM JAB TAK;` |
| `REPEAT ... UNTIL c END REPEAT;` | body runs at least once |
| `label: LOOP ... END LOOP label;` | leave with `LEAVE label;`, skip with `ITERATE label;` |
| `DIKHAO a, b INTO x, y SE t ...;` | exactly one row, else an error |
| `DECLARE c CURSOR FOR DIKHAO ...;` `OPEN c;` `FETCH c INTO x;` `CLOSE c;` | |
| `DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;` | runs when `FETCH` / `SELECT INTO` finds no row |
| `SIGNAL 'message';` | stops with an error |

- Parameters have a mode: `IN` (default), `OUT` (starts `KHALI`) or `INOUT`. For `OUT` and
  `INOUT`, `CHALAO` needs a `@variable` (or a declared local variable inside another body).
  `IN` arguments can be any expression, including `@variables`.
- Loops stop with an error after a safety limit of iterations.
- `CHALAO` reports `N statement(s)`: the SQL statements the body ran.
- A body runs with the caller's own privileges.
- `HATAO PROCEDURE IF EXISTS name` is allowed.
- Name collision: inside a plain-statement body, a bare column name equal to a parameter
  name is taken as the parameter. Prefix parameters with `p_` to avoid this.

### Stored functions

```sql
BANAO FUNCTION bonus (sal INT, pct INT) RETURNS INT SHURU
    RETURN sal * pct / 100;
KHATAM;

DIKHAO naam, bonus(salary, 10) SE emp;
SHOW FUNCTION STATUS;
HATAO FUNCTION bonus;           -- HATAO FUNCTION IF EXISTS bonus also works
```

- Used inside any expression (`JAHAN`, `KRAM`, `DAALO` values, `BADLO ... RAKHO`, other bodies).
- Arguments and the result are converted to the declared types like MySQL does
  (`'1000'` becomes `1000` for an `INT`, `5` becomes `'5'` for a `TEXT`); a `KHALI` argument goes through.
- A function **may read but not change anything**: `DAALO`/`BADLO`/`MITAO`, DDL and `CHALAO`
  inside a function are errors. `RETURN` is only allowed in functions (a procedure with `RETURN` is refused when created).
- A function that ends without `RETURN` is an error; recursion has a depth limit.
- Reading a table inside a function needs `DIKHAO` on it; only a superuser can create functions.

### Session variables

`SET @x = expr, @y = expr;` stores a value for the connection. `DIKHAO @x;` reads it
(`KHALI` if never set) -- no `SE` needed. Variables are per connection and are not saved.
`SHOW VARIABLES [LIKE 'pattern']` shows the server settings (`autocommit`, `transaction_isolation`); read a `@variable` with `DIKHAO @x`.

### Prepared statements

```sql
PREPARE p FROM 'DIKHAO naam SE t JAHAN id = ?';
EXECUTE p USING 2;
DEALLOCATE PREPARE p;           -- HATAO PREPARE p is the same
```

`PREPARE ... FROM` takes a string or a `@variable` holding one, with exactly one statement.
The SQL is checked when prepared. `?` marks inside string literals are not placeholders; each value from
`USING` is inserted as a safe literal, so a value containing quotes cannot change the statement.
Dynamic table names work by building the text first. `EXECUTE` checks privileges for the statement it runs.

## Transactions

```sql
SHURU;                                     -- BEGIN
BADLO accounts RAKHO paisa = paisa - 500 JAHAN id = 1;
BADLO accounts RAKHO paisa = paisa + 500 JAHAN id = 2;
PAKKA;                                     -- COMMIT: keep both changes
-- or WAPAS; to undo EVERYTHING since SHURU (ROLLBACK)
```

- Everything outside `SHURU ... PAKKA` is saved immediately ("autocommit").
- `SET autocommit = 0` (also `OFF`): the first statement that changes anything opens a
  transaction that stays open until `PAKKA` or `WAPAS`. `SET autocommit = 1` (or `ON`) commits
  an open transaction. `SHURU` while such a transaction is open commits it first, like MySQL's `START TRANSACTION`.
  While it is open, other connections wait and get "Database busy hai". Closing the session rolls it back.
- Savepoints, inside a transaction:

  ```sql
  SAVEPOINT a;
  ROLLBACK TO a;            -- also WAPAS TAK a; the savepoint stays and can be used again
  RELEASE SAVEPOINT a;
  ```

  `ROLLBACK TO` undoes everything after the savepoint and drops later savepoints.
  `SAVEPOINT` outside a transaction is an error.
- Restricted users (`-U`) cannot run `SET autocommit` or `SAVEPOINT`; those are superuser commands.
- A transaction covers the current database. `ISTEMAL`, `BANAO DATABASE` and
  `HATAO DATABASE` are not allowed inside one.
- While your transaction is open, other clients wait. They never see half-finished work.
- If the connection drops or the program crashes before `PAKKA`, everything is rolled back.
- The shell prompt shows `*` during a transaction: `meradb:main*>`.

### Isolation level and locking

```sql
SET [SESSION | GLOBAL] TRANSACTION ISOLATION LEVEL READ UNCOMMITTED | READ COMMITTED | REPEATABLE READ | SERIALIZABLE;
DIKHAO * SE t JAHAN id = 1 FOR UPDATE;        -- also FOR SHARE, LOCK IN SHARE MODE, NOWAIT, SKIP LOCKED
SHOW VARIABLES LIKE 'transaction_isolation';
```

The level is remembered and shown, but MeraDB keeps the whole database locked while a transaction is open, so every
transaction really behaves as `SERIALIZABLE`. `FOR UPDATE` / `FOR SHARE` are accepted and read normally for the same
reason: there are no row locks to take, the transaction already holds the lot.

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
            | create_view | alter_view | drop_view | show_stmt
            | create_user | drop_user | grant_stmt | revoke_stmt
            | create_role | drop_role | grant_role | revoke_role | set_role
            | create_trigger | drop_trigger | create_procedure | drop_procedure | call_proc
            | insert | select_stmt | update | delete
            | "SHURU" | "PAKKA" | "WAPAS" | "SAMJHAO" statement ;

create_db   = "BANAO" "DATABASE" IDENT ;
drop_db     = "HATAO" "DATABASE" IDENT ;
use_db      = "ISTEMAL" [ "DATABASE" ] IDENT ;
create_tbl  = "BANAO" "TABLE" IDENT
              ( "(" table_item { "," table_item } ")"
              | ( "KAHO" | "AS" ) "DIKHAO" select_stmt_body       (* CREATE TABLE ... AS SELECT *)
              | "LIKE" IDENT ) ;                                  (* copy columns + constraints, no rows *)
table_item  = column_def | composite_constraint | foreign_key ;
column_def  = IDENT col_type
              { "MUKHYA" "KUNJI" | "ZAROORI" | "ANOKHA" | "WARNA" literal
              | "AUTO_INCREMENT" | "AUTO_ANK"
              | "SANDARBH" IDENT "(" IDENT ")" { fk_action } | "SHART" "(" expr ")" } ;
col_type    = TYPE [ "(" INTEGER [ "," INTEGER ] ")" ]             (* TYPE includes DATETIME / TIMESTAMP *)
            | "ENUM" "(" STRING { "," STRING } ")" ;
fk_action   = "ON" ( "DELETE" | "UPDATE" )
              ( "CASCADE" | "SET" "NULL" | "RESTRICT" | "NO" "ACTION" ) ;   (* SET NULL may also be written SET KHALI *)
foreign_key = [ "CONSTRAINT" IDENT ] "FOREIGN" "KEY" "(" IDENT ")"
              ( "SANDARBH" | "REFERENCES" ) IDENT "(" IDENT ")" { fk_action } ;
composite_constraint = ( "ANOKHA" | "MUKHYA" "KUNJI" ) "(" IDENT { "," IDENT } ")" ;
                       (* table-level: 2+ columns, distinguished from column_def by
                          lookahead -- a column_def always starts with the column's own IDENT *)
drop_tbl    = "HATAO" "TABLE" IDENT ;
alter_tbl   = "SUDHARO" "TABLE" IDENT
              ( "JODO" [ "COLUMN" ] column_def [ "FIRST" | "AFTER" IDENT ]
              | "JODO" composite_constraint
              | "JODO" foreign_key                              (* named, or <table>_ibfk_<n> *)
              | "CHANGE" [ "COLUMN" ] IDENT column_def          (* rename + new definition *)
              | "MODIFY" [ "COLUMN" ] column_def                (* same name, new definition *)
              | "HATAO" ( "FOREIGN" "KEY" | "CONSTRAINT" ) IDENT
              | "HATAO" [ "COLUMN" ] IDENT
              | "NAYA_NAAM" IDENT                              (* rename the table *)
              | "COLUMN" IDENT "NAYA_NAAM" IDENT ) ;            (* rename a column *)
truncate    = "SAAF" "TABLE" IDENT ;
compact     = "SIKODO" "TABLE" IDENT ;
describe    = "BATAO" [ "TABLE" ] IDENT ;                      (* also shows a VIEW's definition *)
show_tables = "DIKHAO" "TABLES" ;
show_views  = "DIKHAO" "VIEWS" ;
create_view = "BANAO" [ "OR" "REPLACE" | "BADAL" ] "VIEW" IDENT "KAHO" select [ check_opt ] ;
                                                                (* select's SOURCE TEXT is stored *)
alter_view  = "SUDHARO" "VIEW" IDENT "KAHO" select [ check_opt ] ;
check_opt   = "WITH" [ "LOCAL" | "CASCADED" ] "CHECK" "OPTION" ; (* updatable views only *)
show_stmt   = "SHOW" ( "TABLES" | "VIEWS" | "DATABASES" | "TRIGGERS" | "PROCEDURE" "STATUS"
                     | "GRANTS" [ "FOR" IDENT ]
                     | "CREATE" ( "TABLE" | "VIEW" ) IDENT
                     | ( "COLUMNS" | "FIELDS" ) ( "FROM" | "SE" | "IN" ) [ "TABLE" ] IDENT )
            | "DIKHAO" ( "GRANTS" [ "FOR" IDENT ] | "TRIGGERS" | "PROCEDURES" | "DATABASES" ) ;
drop_view   = "HATAO" "VIEW" IDENT ;

create_user = "BANAO" "USER" IDENT "GUPT" STRING ;
drop_user   = "HATAO" "USER" IDENT ;
privilege   = "DIKHAO" | "DAALO" | "BADLO" | "MITAO"
            | "SELECT" | "INSERT" | "UPDATE" | "DELETE" ;
priv_item   = privilege [ "(" IDENT { "," IDENT } ")" ] ;       (* columns: not with MITAO / DELETE *)
priv_list   = "SAB" | "ALL" [ "PRIVILEGES" ] | priv_item { "," priv_item } ;
grant_stmt  = "ADHIKAR" "DO" priv_list "PAR" IDENT "KO" IDENT [ "WITH" "GRANT" "OPTION" ] ;
revoke_stmt = "ADHIKAR" "WAPAS" priv_list "PAR" IDENT "SE" IDENT ;
create_role = "BANAO" "ROLE" IDENT ;
drop_role   = "HATAO" "ROLE" IDENT ;
grant_role  = "ADHIKAR" "DO" "ROLE" IDENT { "," IDENT } "KO" IDENT ;
revoke_role = "ADHIKAR" "WAPAS" "ROLE" IDENT { "," IDENT } "SE" IDENT ;
set_role    = "SET" "ROLE" ( IDENT { "," IDENT } | "SAB" | "NONE" ) ;

block_body  = "SHURU" statement ";" { statement ";" } "KHATAM" ;   (* 1+ statements; raw SOURCE
                                                                       TEXT between SHURU/KHATAM
                                                                       is what's actually stored *)
create_trigger = "BANAO" "TRIGGER" IDENT ( "PEHLE" | "BAAD" ) ( "DAALO" | "BADLO" | "MITAO" )
                 "PAR" IDENT block_body ;
drop_trigger   = "HATAO" "TRIGGER" IDENT ;

proc_param     = IDENT TYPE ;
create_procedure = "BANAO" "PROCEDURE" IDENT "(" [ proc_param { "," proc_param } ] ")" block_body ;
drop_procedure   = "HATAO" "PROCEDURE" IDENT ;
call_proc        = "CHALAO" IDENT "(" [ expr { "," expr } ] ")" ;

insert      = ( "DAALO" "MEIN" | "REPLACE" "INTO" | "BADAL" "DAALO" "MEIN" ) IDENT [ "(" IDENT { "," IDENT } ")" ]
              ( "MAAN" tuple { "," tuple } | select )
              [ "TAKRAAV" "PAR" "BADLO" assignment { "," assignment } ] ;   (* not with REPLACE / BADAL *)
tuple       = "(" expr { "," expr } ")" ;
assignment  = IDENT "=" expr ;

select_stmt = select { set_op select } ;                        (* left-associative chaining *)
set_op      = "SANYUKT" | "SAAJHA" | "CHHODKAR" ;                (* UNION / INTERSECT / EXCEPT *)
select      = "DIKHAO" select_body ;
select_body = [ "ALAG" ] item { "," item } [ "SE" source           (* no SE: one row of constants *)
              { join } ]
              [ "JAHAN" expr ]
              [ "SAMOOH" expr { "," expr } ]
              [ "JINKA" expr ]
              [ "KRAM" order_item { "," order_item } ]
              [ ( "SIRF" | "LIMIT" ) INTEGER [ "OFFSET" INTEGER | "," INTEGER ] ] ;   (* SIRF m, n = SIRF n OFFSET m *)
select_stmt_body = select_body { set_op "DIKHAO" select_body } ;  (* what follows DIKHAO in create_tbl *)
source      = IDENT [ IDENT ]
            | "(" "DIKHAO" select_body ")" [ "KAHO" | "AS" ] IDENT ;   (* derived table: alias is required *)
join        = ( [ "BAAYAN" | "DAHINA" | "DONO" ] "MILAO" source
                ( "PAR" expr | "USING" "(" IDENT { "," IDENT } ")" ) )
            | ( "SAMAAN" "MILAO" source ) ;   (* NATURAL: no PAR -- synthesised at bind time *)
item        = "*" | IDENT "." "*" | expr [ "KAHO" IDENT ] ;   (* KAHO = output column alias *)
order_item  = expr [ "SEEDHA" | "ULTA" ] ;
update      = "BADLO" IDENT "RAKHO" assignment { "," assignment } [ "JAHAN" expr ] ;
delete      = "MITAO" "SE" IDENT [ "JAHAN" expr ] ;

(* expressions, lowest precedence first *)
expr        = or_expr ;
or_expr     = and_expr { "YA" and_expr } ;
and_expr    = not_expr { "AUR" not_expr } ;
not_expr    = ( "NAHI" | "NOT" "EXISTS" ) not_expr | comparison ;   (* NOT is only taken before EXISTS *)
comparison  = additive [ ( "=" | "!=" | "<" | "<=" | ">" | ">=" ) additive
                       | ( "=" | "!=" | "<" | "<=" | ">" | ">=" ) ( "ANY" | "SOME" | "ALL" ) "(" "DIKHAO" select_body ")"
                       | "HAI" [ "NAHI" ] "KHALI"
                       | [ "NAHI" ] "JAISA" additive
                       | [ "NAHI" ] "BEECH" additive "AUR" additive
                       | [ "NAHI" ] "MEIN" "(" ( expr { "," expr } | "DIKHAO" select_body ) ")" ] ;
additive    = term { ( "+" | "-" | "||" ) term } ;
term        = unary { ( "*" | "/" | "DIV" | "%" ) unary } ;
unary       = "-" unary | primary ;
primary     = NUMBER | STRING | "SACH" | "JHOOTH" | "KHALI"
            | case_expr | coalesce_expr | cast_expr
            | "EXISTS" "(" "DIKHAO" select_body ")"
            | IDENT "(" [ expr { "," expr } ] ")"        (* scalar function: UPPER(x), NOW(), ... *)
            | "CURRENT_DATE"
            | IDENT "(" ( "*" | expr ) ")"               (* aggregate call *)
            | IDENT [ "." IDENT ]                        (* column, or alias.column *)
            | "(" ( expr | "DIKHAO" select_body ) ")" ;   (* the 2nd form: a scalar subquery *)
case_expr   = "AGAR" expr "TAB" expr { "AGAR" expr "TAB" expr } [ "WARNA" expr ] "KHATAM" ;
coalesce_expr = ( "PEHLA" | "COALESCE" ) "(" expr { "," expr } ")" | "IFNULL" "(" expr "," expr ")" ;
cast_expr   = "CAST" "(" expr ( "KAHO" | "AS" ) TYPE ")" ;
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
- **`SANDARBH` (FOREIGN KEY) is RESTRICT unless you say otherwise.** Deleting/changing a parent row that a
  child still points at is an error -- you must remove or repoint the child rows first -- unless the
  foreign key has `ON DELETE` / `ON UPDATE` `CASCADE` or `SET NULL` (see "Foreign key actions"). The
  parent column must be `MUKHYA KUNJI`/`ANOKHA`, so the child-side check is an O(1) hash-index lookup,
  same as uniqueness checks.
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
- **`MILAO ... USING (c)` shows the shared column once.** In `*` it appears once (for a
  `DAHINA`/`DONO` join it takes whichever side has a value), and a plain `c` anywhere in the
  query is not ambiguous. `t.*` still lists all of `t`'s own columns.
- **`OFFSET`, `USING`, `EXISTS`, `ANY`, `SOME`, `ALL`, `CAST` and the scalar function
  names are not reserved words.** They keep working as column or table names; they only mean
  something in the places shown above. The same goes for the words added with the newer syntax:
  `LIMIT` (English `SIRF`; only taken before a number), `DIV`, `AS`, `LIKE`, `REPLACE`, `INTO`, `BADAL`,
  `AUTO_INCREMENT`, `AUTO_ANK`, `ENUM`, `FOREIGN`, `KEY`, `CONSTRAINT`, `REFERENCES`, `ON`, `DELETE`,
  `UPDATE`, `CASCADE`, `RESTRICT`, `NO`, `ACTION`, `SET`, `CHANGE`, `MODIFY`, `FIRST` and `AFTER`.
  (The workbench highlights them wherever they appear.)
- **`DIKHAO` without `SE` has no columns to read.** `DIKHAO naam` is an error ("Column 'naam' ke liye
  table chahiye"); only constants, functions and subqueries make sense there.
- **A derived table is run first and then used like a view.** It cannot refer to columns of the
  query around it (no correlation), it needs an alias, and its column names must be unique.
- **A view is not a table.** It has no rows of its own, no index, and always does a full scan of
  its freshly re-run query -- see the VIEWs section above and `docs/ARCHITECTURE.md`.
- **No username = superuser.** Privilege checking only ever applies to a session that
  authenticated with `-U/--user` against a running server -- every embedded/`--local`
  session, and every test in this project, is unaffected. A *server* refuses a connection
  without a username once users exist and no shared password is set. See "Users & privileges"
  above and `docs/SERVER.md`.
- **Triggers and stored procedures run with the invoking session's own privileges**, not
  "definer rights" -- a restricted user needs grants on every table their trigger/procedure
  touches, not just the one they directly wrote to.
