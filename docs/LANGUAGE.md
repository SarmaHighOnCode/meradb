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
```

## DML: Data Manipulation Language

```sql
-- INSERT (column list optional; missing columns become KHALI)
DAALO MEIN students (id, naam, umar) MAAN (1, 'Ravi', 20), (2, 'Priya', 19);
DAALO MEIN students MAAN (3, 'Aman', 'aman@x.in', 22);

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
            | truncate | compact | describe | show_tables
            | insert | select | update | delete
            | "SHURU" | "PAKKA" | "WAPAS" | "SAMJHAO" statement ;

create_db   = "BANAO" "DATABASE" IDENT ;
drop_db     = "HATAO" "DATABASE" IDENT ;
use_db      = "ISTEMAL" [ "DATABASE" ] IDENT ;
create_tbl  = "BANAO" "TABLE" IDENT "(" column_def { "," column_def } ")" ;
column_def  = IDENT TYPE [ "(" INTEGER [ "," INTEGER ] ")" ]
              { "MUKHYA" "KUNJI" | "ZAROORI" | "ANOKHA" | "WARNA" literal
              | "SANDARBH" IDENT "(" IDENT ")" | "SHART" "(" expr ")" } ;
drop_tbl    = "HATAO" "TABLE" IDENT ;
alter_tbl   = "SUDHARO" "TABLE" IDENT
              ( "JODO" [ "COLUMN" ] column_def
              | "HATAO" [ "COLUMN" ] IDENT
              | "NAYA_NAAM" IDENT                              (* rename the table *)
              | "COLUMN" IDENT "NAYA_NAAM" IDENT ) ;            (* rename a column *)
truncate    = "SAAF" "TABLE" IDENT ;
compact     = "SIKODO" "TABLE" IDENT ;
describe    = "BATAO" [ "TABLE" ] IDENT ;
show_tables = "DIKHAO" "TABLES" ;

insert      = "DAALO" "MEIN" IDENT [ "(" IDENT { "," IDENT } ")" ]
              "MAAN" tuple { "," tuple } ;
tuple       = "(" expr { "," expr } ")" ;
select      = "DIKHAO" [ "ALAG" ] item { "," item } "SE" IDENT [ IDENT ]
              { [ "BAAYAN" ] "MILAO" IDENT [ IDENT ] "PAR" expr }
              [ "JAHAN" expr ]
              [ "SAMOOH" expr { "," expr } ]
              [ "JINKA" expr ]
              [ "KRAM" order_item { "," order_item } ]
              [ "SIRF" INTEGER ] ;
item        = "*" | IDENT "." "*" | expr [ "KAHO" IDENT ] ;   (* KAHO = output column alias *)
order_item  = expr [ "SEEDHA" | "ULTA" ] ;
update      = "BADLO" IDENT "RAKHO" IDENT "=" expr { "," IDENT "=" expr } [ "JAHAN" expr ] ;
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
                       | [ "NAHI" ] "MEIN" "(" expr { "," expr } ")" ] ;
additive    = term { ( "+" | "-" ) term } ;
term        = unary { ( "*" | "/" | "%" ) unary } ;
unary       = "-" unary | primary ;
primary     = NUMBER | STRING | "SACH" | "JHOOTH" | "KHALI"
            | IDENT "(" ( "*" | expr ) ")"               (* aggregate call *)
            | IDENT [ "." IDENT ]                        (* column, or alias.column *)
            | "(" expr ")" ;
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
