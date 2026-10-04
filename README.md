# MeraDB

**Apna database, apni bhasha.** A relational database engine built from scratch, queried in
a **Hinglish** language instead of SQL. You run a database **server** in the background and
connect to it with a **workbench** (like MySQL Workbench) or a command-line **shell**.

MeraDB exists twice, and the two versions are interchangeable (same language, same data
files, same network protocol):

- **The C++ version** (`cpp/`) is the main one: build it with CMake, run `meradb_cli`.
  Start with [Building and running the C++ version](#building-and-running-the-c-version).
- **The Python version** (`meradb/`) is the reference implementation it was ported from.
  See [The Python version](#the-python-version).

![C++](https://img.shields.io/badge/C%2B%2B-17-blue)
![Python](https://img.shields.io/badge/python-3.10%2B-blue)
![C++ tests](https://img.shields.io/badge/C%2B%2B%20ctest-780%20tests-brightgreen)
![Python tests](https://img.shields.io/badge/python%20tests-183%20passing-brightgreen)

```sql
BANAO TABLE courses  (id INT MUKHYA KUNJI, title VARCHAR(40) ZAROORI ANOKHA);
BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT ZAROORI, dob DATE,
                      cgpa FLOAT SHART (cgpa BEECH 0 AUR 10), cid INT SANDARBH courses(id));

DAALO MEIN courses  MAAN (10, 'DBMS'), (20, 'Operating Systems');
DAALO MEIN students MAAN (1, 'Ravi', '2004-05-12', 8.4, 10), (2, 'Priya', '2005-01-30', 9.1, 20);

DIKHAO s.naam, c.title SE students s MILAO courses c PAR s.cid = c.id JAHAN s.cgpa > 8;
DIKHAO cid, GINO(*) KAHO total, AUSAT(cgpa) KAHO avg SE students SAMOOH cid KRAM total ULTA;

SHURU;  BADLO students RAKHO cgpa = cgpa + 0.1 JAHAN id = 2;  PAKKA;
```

In SQL, that's `CREATE TABLE`, `INSERT INTO`, `SELECT ... JOIN ... WHERE`, `GROUP BY` and
`BEGIN ... COMMIT`.

---

## Contents

1. [Features](#features)
2. [Building and running the C++ version](#building-and-running-the-c-version)
3. [The Python version](#the-python-version) (requirements, installation, quick start)
4. [Command reference](#command-reference)
5. [Shell commands](#shell-commands)
6. [Workbench keys](#workbench-keys)
7. [Language reference](#language-reference)
8. [Where the data lives](#where-the-data-lives)
9. [How MeraDB works](#how-meradb-works)
10. [Running the tests](#running-the-tests)
11. [Project structure](#project-structure)
12. [Documentation](#documentation)

---

## Features

- **Full DDL:**
  - create, drop and use databases
  - create, drop, truncate, alter (add, drop, rename column) and rename tables
  - describe a table, list tables
- **Constraints:** PRIMARY KEY, NOT NULL, UNIQUE, DEFAULT, FOREIGN KEY (RESTRICT), CHECK,
  composite (multi-column) UNIQUE / PRIMARY KEY
- **Full DML:**
  - multi-row INSERT (including `INSERT ... SELECT`), UPDATE, DELETE
  - a simplified upsert: `ON CONFLICT DO UPDATE`
  - SELECT with `WHERE`, `ORDER BY`, `LIMIT`, `DISTINCT`, column aliases, `GROUP BY` / `HAVING`
  - `COUNT SUM AVG MIN MAX`
  - `LIKE`, `BETWEEN`, `IN`, `IS NULL`, arithmetic, `COALESCE`/NVL, `CASE WHEN`
  - subqueries: scalar, `IN (subquery)`, correlated
  - `UNION` / `INTERSECT` / `EXCEPT`
- **Views:** `CREATE VIEW`/`DROP VIEW`, re-materialized fresh on every read
- **Joins:** INNER, LEFT, RIGHT, FULL OUTER and NATURAL, with table aliases (`s.naam`)
- **Types:** INT, FLOAT, TEXT, BOOL, DATE, `VARCHAR(n)`, `NUMBER(p,s)`
- **Transactions:** `BEGIN / COMMIT / ROLLBACK`, with crash recovery
- **Query plans:** `EXPLAIN` shows index lookup vs full scan and hash join vs nested loop
- **Hash indexes:** automatic on PRIMARY KEY and UNIQUE columns
- **SQL-correct NULL handling:** three-valued logic
- **Users & privileges:** `CREATE USER`/`DROP USER` (hashed passwords), `GRANT`/`REVOKE`
  per `(database, table)`, enforced only for a server session that logged in with a
  username (see `docs/LANGUAGE.md`)
- **Triggers:** `CREATE TRIGGER`/`DROP TRIGGER`, `BEFORE`/`AFTER` × `INSERT`/`UPDATE`/`DELETE`,
  `NEW`/`OLD` row substitution
- **Stored procedures:** `CREATE PROCEDURE`/`DROP PROCEDURE`/`CALL`, typed parameters
- **Client–server:**
  - TCP server, several clients at once
  - one session per client, transactions isolated from each other
  - optional password, or per-user login with privileges
- **Three clients:**
  - `workbench`: full-screen UI, like MySQL Workbench
  - `shell`: command-line shell
  - a client library (C++ and Python)
- **Own storage format:** a binary file format designed from scratch; no SQLite, no database library of any kind
- **Error messages in Hinglish**

---

## Building and running the C++ version

### What you need

- **CMake 3.20 or newer**: [cmake.org/download](https://cmake.org/download/)
- **A C++17 compiler**, one of: MinGW-w64 g++, MSVC (Visual Studio 2019 or newer), g++ or clang
- **Git** and **internet access on the first build**: CMake downloads three libraries by itself
  (nlohmann/json, Catch2 for the tests, and FTXUI for the workbench). Nothing else is installed
  by hand.
- **Python is optional**. Only the cross-check tests (C++ against the Python engine) use it, and
  they are left out of the test run if Python is not found.

### Build

The quickest way is the helper script in the repository root. It finds CMake, configures a
Release build, builds with all cores and prints where the program is:

```powershell
.\build.ps1            # Windows PowerShell (add -Test to run the tests afterwards)
```
```bash
./build.sh             # Linux / macOS (add --test to run the tests afterwards)
```

Or run CMake yourself (all commands from the repository root):

```powershell
# Windows, MinGW-w64 (g++ and cmake must be on PATH)
cmake -S cpp -B cpp/build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build --parallel

# Windows, MSVC (Developer PowerShell for Visual Studio)
cmake -S cpp -B cpp/build -G "Visual Studio 17 2022"
cmake --build cpp/build --config Release --parallel
```
```bash
# Linux / macOS
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build --parallel
```

The program is one executable:

| Build | Path of the program |
|-------|---------------------|
| MinGW, Linux, macOS | `cpp/build/meradb_cli` (`.exe` on Windows) |
| MSVC / Visual Studio | `cpp/build/Release/meradb_cli.exe` |

To type less, give it a short name for the current terminal (adjust the path for MSVC):

```powershell
Set-Alias meradb_cli (Resolve-Path cpp\build\meradb_cli.exe)     # PowerShell
```
```bash
alias meradb_cli="$PWD/cpp/build/meradb_cli"                     # bash / zsh
```

Check it works: `meradb_cli --version` prints `MeraDB 1.0.0`, and `meradb_cli --help` lists the
commands. (The help text calls the program `meradb`; it is the same program.)

**Building without the workbench.** `-DMERADB_WORKBENCH=OFF` (script: `-NoWorkbench` /
`--no-workbench`) leaves out the full-screen workbench, so FTXUI is not downloaded and not
compiled; `meradb_cli workbench` then prints a note and exits. For a machine with no internet,
unpack the FTXUI v5.0.0 source tarball somewhere and add
`-DFETCHCONTENT_SOURCE_DIR_FTXUI=<that folder>`. nlohmann/json and Catch2 are still fetched on
the first configure; copy a finished `cpp/build` folder to such a machine, or see
[docs/CPP.md](docs/CPP.md).

### Run it

The commands and options are the same as the Python version's (see the
[command reference](#command-reference), where `meradb` is the Python program; for C++ type
`meradb_cli` instead).

```bash
meradb_cli start                       # server in the background (port 6372)
meradb_cli status                      # is it running? exit code 0 = yes, 3 = no
meradb_cli shell                       # command-line shell (uses the server if running)
meradb_cli workbench                   # full-screen UI (needs a real terminal, at least 60 x 24)
meradb_cli run examples/demo.mdb       # run a script file
meradb_cli stop                        # stop the background server
meradb_cli server                      # or: run the server in this terminal (Ctrl+C stops it)
```

If no server is running, `shell`, `workbench` and `run` open the data folder directly
("local mode") and print a note on stderr. Add `--local` to ask for that. Inside the shell, type
statements ending with `;`, `.help` for help and `.exit` to leave (see [Shell commands](#shell-commands)).

Where the data goes: see [Where the data lives](#where-the-data-lives). The C++ and Python
versions use the same default folder and the same file formats. Set `MERADB_DATA` (or pass
`-D <folder>`) to use another folder, for example a throw-away one while experimenting:

```powershell
$env:MERADB_DATA = "$env:TEMP\mera-try"        # PowerShell
```
```bash
export MERADB_DATA=/tmp/mera-try                # bash / zsh
```

### A 5-minute demo

1. Build (above) and set a throw-away `MERADB_DATA`.
2. `meradb_cli run examples/demo.mdb`: creates a `college` database and walks through every
   feature top to bottom (tables, constraints, joins, grouping, views, users, triggers,
   procedures, transactions, `SAMJHAO`), then drops it again. Its last statements show
   friendly error messages on purpose, so the exit code is 1; that is expected.
3. `meradb_cli run examples/rdbms_lab_coverage.mdb`: a longer script that covers a standard
   first DBMS course syllabus topic by topic.
4. `meradb_cli start`, then `meradb_cli shell`: type
   `BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT ZAROORI, cgpa FLOAT);`, a
   `DAALO MEIN students MAAN (1, 'Ravi', 8.4), (2, 'Priya', 9.1);` and
   `DIKHAO naam SE students JAHAN cgpa > 9;` (the output is shown in
   [Quick start](#quick-start-python-version) below, and is identical).
5. `meradb_cli workbench` (the server is still running): press **F5** to run the editor text, **F6** for
   the query plan, **F1** for help, **Ctrl+Q** to quit.
6. `meradb_cli stop`.

### Run the tests

```bash
ctest --test-dir cpp/build --output-on-failure       # add  -C Release  for MSVC
```

There are about 780 tests: unit tests for every layer, and the cross-checks that compare the
C++ program with the Python engine (these need Python, and the workbench comparison also
needs the `textual` package, otherwise it is reported as skipped). The first full run takes a
few minutes.

### More about the C++ version

[docs/CPP.md](docs/CPP.md) has the details: the module layout, how it is verified against
Python, the shell and workbench, the manual terminal checklist, and the **list of known
divergences** from the Python engine (all small and deliberate). Platforms: the Windows MinGW
build is the one built and tested so far. For Linux see `docs/CPP.md`; macOS and the MSVC
build have not been verified yet.

---

## The Python version

The Python implementation in `meradb/` is the reference the C++ version was ported from. It is
complete on its own and installs as the `meradb` command.

### Requirements (Python version)

- **Python 3.10 or newer**: [python.org/downloads](https://www.python.org/downloads/).
  On Windows, tick **"Add python.exe to PATH"** in the installer.
- **Git**, to clone the repository.
- The engine, server and shell use **only the Python standard library**. The optional
  workbench needs one package, [Textual](https://textual.textualize.io), which installs automatically.

Check your versions:

```bash
python --version
git --version
```

On macOS/Linux use `python3` instead of `python` if needed.

---

### Installation (Python version)

#### Windows (PowerShell or Command Prompt)

```powershell
git clone https://github.com/SarmaHighOnCode/meradb.git
cd meradb
python -m venv .venv
.venv\Scripts\activate
pip install -e ".[workbench]"
meradb --version
```

> If PowerShell refuses to run `activate`, run this once:
> `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`

#### macOS / Linux

```bash
git clone https://github.com/SarmaHighOnCode/meradb.git
cd meradb
python3 -m venv .venv
source .venv/bin/activate
pip install -e ".[workbench]"
meradb --version
```

`meradb --version` should print `MeraDB 1.0.0`.

**Notes:**
- **Virtual environment:** the `.venv` step is recommended, not required. If you use it,
  activate it (`activate` / `source ...`) in every new terminal before running `meradb`.
- **Without the workbench:** `pip install -e .` installs everything except the full-screen workbench.
- **Without installing at all:** from inside the `meradb` folder, every command also works
  as `python -m meradb <command>`, e.g. `python -m meradb shell`.
- **Update to the latest version:** `git pull`. The `-e` (editable) install picks up the changes
  automatically. Restart the server afterwards: `meradb stop`, then `meradb start`.
- **Uninstall:** `pip uninstall meradb`. Your data stays in the data folder (see below)
  until you delete it.

---

### Quick start (Python version)

```bash
meradb start                      # 1. start the database server in the background
meradb workbench                  # 2. open the workbench (like MySQL Workbench)
meradb shell                      # 3. or type queries in the command-line shell
meradb run examples/demo.mdb      # 4. run the demo script: every feature, top to bottom
meradb status                     # 5. is the server running? who is connected?
meradb stop                       # 6. stop the server
```

Inside the shell:

```
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

meradb:main> .exit
```

**No server running?** `shell`, `workbench` and `run` fall back automatically to **local
mode**: the engine runs inside the client and opens the data folder directly.
They print a note when they do this.

---

## Command reference

The commands below are written for the Python program `meradb`. The C++ program
`meradb_cli` has the same commands and options (`-D`, `--port`, `--local`, ...); differences are
listed in [docs/CPP.md](docs/CPP.md). `meradb` with no command is the same as `meradb shell`. Add `--help` to any command for its options.

| Command | What it does |
|---------|--------------|
| `meradb start` | Start the database server in the background |
| `meradb stop` | Stop the background server cleanly |
| `meradb status` | Show whether it runs: address, pid, sessions, databases |
| `meradb server` | Run the server in this terminal (Ctrl+C stops it) |
| `meradb workbench` (alias `tui`) | Full-screen client, like MySQL Workbench |
| `meradb shell` | Interactive command-line shell |
| `meradb run FILE...` | Run one or more `.mdb` script files |
| `meradb --version` | Print the version |
| `meradb --help` | List all commands |

### Server commands: `start`, `server`

| Option | Default | Meaning |
|--------|---------|---------|
| `-D, --data DIR` | per-user folder (see [below](#where-the-data-lives)) | Data folder |
| `--host HOST` | `127.0.0.1` | Address to listen on. `0.0.0.0` = the whole network (use a password!) |
| `--port PORT` | `6372` | TCP port ("MERA" on a phone keypad) |
| `--password PW` | none | Clients must send this password |
| `-v, --verbose` | off | Log every query to the server log |

```bash
meradb start
meradb start --port 6373 --password s3cret
meradb start --host 0.0.0.0 --password s3cret     # allow other computers on your LAN
meradb server -v                                  # foreground, watch every query arrive
```

The background server writes its log to `server.log` in the data folder.

### Server control: `stop`, `status`

| Option | Meaning |
|--------|---------|
| `-D, --data DIR` | Data folder of the server to stop/inspect |
| `-W, --password` | Ask for the server password |
| `--force` | (`stop` only) kill the process if a normal shutdown fails |

```bash
meradb status          # exit code 0 = running, 3 = not running
meradb stop
meradb stop --force
```

### Client commands: `shell`, `workbench`, `run`

| Option | Default | Meaning |
|--------|---------|---------|
| `-H, --host HOST` | `127.0.0.1` | Server address |
| `-p, --port PORT` | `6372` | Server port |
| `-d, --database DB` | `main` | Database to start in |
| `-W, --password` | | Ask for the password |
| `-U, --user USERNAME` | | Log in as this user (privileges apply -- see `docs/SERVER.md`) |
| `--local` | | Don't use a server: open the data folder directly |
| `-D, --data DIR` | per-user folder | Data folder, for local mode |

```bash
meradb shell -d college                      # start in database 'college'
meradb shell -H 192.168.1.20 -p 6372 -W      # connect to another computer
meradb shell --local                         # no server; open the data folder directly
meradb workbench
meradb run examples/demo.mdb
meradb run setup.mdb data.mdb queries.mdb    # several files, in order
meradb run report.mdb > output.txt           # save the results to a file
```

If you pass `-H` or `-p` and that server isn't reachable, the command fails instead of
falling back to local mode.

### Environment variables

| Variable | Meaning |
|----------|---------|
| `MERADB_DATA` | Default data folder |
| `MERADB_HOST` | Default server address for clients |
| `MERADB_PORT` | Default port, for server and clients |
| `MERADB_PASSWORD` | Password, so you don't need `-W` |
| `MERADB_USER` | Username, so you don't need `-U` |

---

## Shell commands

Everything ending in `;` is a query. A query can span several lines. Lines starting with
`.` are shell commands:

| Command | Meaning |
|---------|---------|
| `.help` | Help and syntax summary |
| `.tables` | List tables (same as `DIKHAO TABLES;`) |
| `.schema <table>` | Show a table's columns (same as `BATAO <table>;`) |
| `.run <file>` | Run a `.mdb` script file |
| `.exit` / `.quit` / `.nikal` | Leave the shell (Ctrl+C / Ctrl+D also work) |

The prompt shows the current database: `meradb:main>`. It gets a `*` while a transaction is
open (`meradb:main*>`).

---

## Workbench keys

| Key | Action |
|-----|--------|
| **F5** / **Ctrl+R** | Run the editor's text, or only the selected text |
| **F6** | Show the query plan (`SAMJHAO`) without running the query |
| **Ctrl+Up / Ctrl+Down** | Previous / next query from history |
| **Ctrl+S** | Export the current results to CSV (`exports/` folder) |
| **Ctrl+O** | Connect to another server, or switch to local mode |
| **Tab / Shift+Tab** | Move between editor, results and schema tree |
| **Enter** on a table | Preview its first 100 rows |
| **Enter** on a database | Switch to it (`ISTEMAL`) |
| **Enter** on a column | Insert its name into the editor |
| **Ctrl+L** | Clear the log |
| **F1** | Help + full language reference (Esc closes it) |
| **Ctrl+Q** | Quit |

The header shows the connection, the current database and whether a transaction is open.

---

## Language reference

Keywords are case-insensitive. Statements end with `;`. Comments start with `--`.
The full reference with the formal grammar is in [docs/LANGUAGE.md](docs/LANGUAGE.md).

### Keywords

| MeraDB | SQL | | MeraDB | SQL |
|--------|-----|-|--------|-----|
| `BANAO` | CREATE | | `DIKHAO` | SELECT |
| `HATAO` | DROP | | `SE` | FROM |
| `SUDHARO` | ALTER | | `JAHAN` | WHERE |
| `JODO` | ADD | | `KRAM` / `SEEDHA` / `ULTA` | ORDER BY / ASC / DESC |
| `NAYA_NAAM` | RENAME TO | | `SIRF` | LIMIT |
| `SAAF` | TRUNCATE | | `ALAG` | DISTINCT |
| `SIKODO` | VACUUM | | `KAHO` | AS (column alias) |
| `ISTEMAL` | USE | | `SAMOOH` / `JINKA` | GROUP BY / HAVING |
| `BATAO` | DESCRIBE | | `MILAO ... PAR` | JOIN ... ON |
| `DAALO MEIN ... MAAN` | INSERT INTO ... VALUES | | `BAAYAN MILAO` | LEFT JOIN |
| `BADLO ... RAKHO` | UPDATE ... SET | | `AUR` / `YA` / `NAHI` | AND / OR / NOT |
| `MITAO SE` | DELETE FROM | | `JAISA` | LIKE |
| `MUKHYA KUNJI` | PRIMARY KEY | | `BEECH ... AUR` | BETWEEN ... AND |
| `ZAROORI` | NOT NULL | | `MEIN (...)` | IN (...) |
| `ANOKHA` | UNIQUE | | `HAI KHALI` | IS NULL |
| `WARNA` | DEFAULT | | `KHALI` | NULL |
| `SANDARBH` | REFERENCES (foreign key) | | `SACH` / `JHOOTH` | TRUE / FALSE |
| `SHART` | CHECK | | `SHURU` / `PAKKA` / `WAPAS` | BEGIN / COMMIT / ROLLBACK |
| `BANAO VIEW ... KAHO` | CREATE VIEW ... AS | | `SAMJHAO` | EXPLAIN |
| `HATAO VIEW` / `DIKHAO VIEWS` | DROP VIEW / SHOW VIEWS | | `SANYUKT` / `SAAJHA` / `CHHODKAR` | UNION / INTERSECT / EXCEPT |
| `DAHINA MILAO` / `DONO MILAO` | RIGHT JOIN / FULL OUTER JOIN | | `SAMAAN MILAO` | NATURAL JOIN |
| `PEHLA(...)` | COALESCE(...) / NVL | | `AGAR ... TAB ... WARNA ... KHATAM` | CASE WHEN ... THEN ... ELSE ... END |
| `TAKRAAV PAR BADLO` | ON CONFLICT DO UPDATE | | `ANOKHA (a,b)` / `MUKHYA KUNJI (a,b)` | composite UNIQUE / PRIMARY KEY |
| `BANAO USER ... GUPT` | CREATE USER ... IDENTIFIED BY | | `HATAO USER` | DROP USER |
| `ADHIKAR DO ... PAR ... KO` | GRANT ... ON ... TO | | `ADHIKAR WAPAS ... PAR ... SE` | REVOKE ... ON ... FROM |
| `SAB` (with `ADHIKAR DO`) | ALL (privileges) | | `BANAO`/`HATAO TRIGGER` | CREATE/DROP TRIGGER |
| `PEHLE` / `BAAD` | BEFORE / AFTER | | `NAYA` / `PURANA` | NEW / OLD |
| `BANAO`/`HATAO PROCEDURE` | CREATE/DROP PROCEDURE | | `CHALAO` | CALL |

Aggregates: `GINO` (COUNT), `KUL` (SUM), `AUSAT` (AVG), `NYUNTAM` (MIN), `ADHIKTAM` (MAX).
The English names `COUNT SUM AVG MIN MAX` work too.

### Data types

| Type | Also written as | Example value |
|------|-----------------|---------------|
| `INT` | `ANK`, `INTEGER` | `42`, `-7` |
| `FLOAT` | `DASHAMLAV`, `REAL`, `NUMBER(p,s)`, `NUMERIC`, `DECIMAL` | `3.14` |
| `TEXT` | `SHABD`, `STRING`, `VARCHAR(n)`, `VARCHAR2(n)`, `CHAR(n)` | `'Ravi'` (`'Ravi''s'` for a quote) |
| `BOOL` | `HAAN_NA`, `BOOLEAN` | `SACH`, `JHOOTH` |
| `DATE` | `TAREEKH` | `'2024-01-31'` |

`VARCHAR(20)` enforces a maximum length of 20 characters. Precision on numeric types is
accepted and ignored.

### Databases

```sql
BANAO DATABASE college;          -- CREATE DATABASE
ISTEMAL college;                 -- USE
HATAO DATABASE college;          -- DROP DATABASE
DIKHAO TABLES;                   -- SHOW TABLES
```

### DDL: tables

```sql
BANAO TABLE courses (
    id     INT          MUKHYA KUNJI,              -- PRIMARY KEY
    title  VARCHAR(40)  ZAROORI ANOKHA             -- NOT NULL UNIQUE
);

BANAO TABLE students (
    id     INT   MUKHYA KUNJI,
    naam   TEXT  ZAROORI,
    umar   INT   WARNA 18  SHART (umar >= 0),      -- DEFAULT 18, CHECK (umar >= 0)
    dob    DATE,
    cid    INT   SANDARBH courses(id)              -- FOREIGN KEY REFERENCES courses(id)
);

BATAO students;                                    -- DESCRIBE

SUDHARO TABLE students JODO email TEXT;            -- ALTER TABLE ADD COLUMN
SUDHARO TABLE students HATAO email;                -- ALTER TABLE DROP COLUMN
SUDHARO TABLE students COLUMN naam NAYA_NAAM full_naam;   -- RENAME COLUMN
SUDHARO TABLE students NAYA_NAAM pupils;           -- RENAME TABLE

SAAF TABLE pupils;                                 -- TRUNCATE
SIKODO TABLE pupils;                               -- VACUUM: reclaim space from deleted rows
HATAO TABLE pupils;                                -- DROP TABLE
```

Foreign keys use **RESTRICT**: you can't delete or change a parent row, or drop or
truncate a parent table, while child rows still point to it.

A CHECK rule rejects a row only when the rule is definitely false; KHALI (NULL) passes,
as in SQL.

### DML

```sql
-- INSERT (the column list is optional; missing columns get their WARNA value or KHALI)
DAALO MEIN students (id, naam, dob, cid) MAAN (1, 'Ravi', '2004-05-12', 10), (2, 'Priya', KHALI, 10);

-- SELECT
DIKHAO * SE students;
DIKHAO naam, umar + 1 SE students JAHAN umar > 18 AUR naam != 'Ravi' KRAM umar ULTA SIRF 10;
DIKHAO naam SE students JAHAN naam JAISA 'R%';            -- LIKE: % = anything, _ = one char
DIKHAO naam SE students JAHAN umar BEECH 18 AUR 25;       -- BETWEEN
DIKHAO naam SE students JAHAN id MEIN (1, 2, 3);          -- IN
DIKHAO naam SE students JAHAN dob HAI KHALI;              -- IS NULL
DIKHAO naam SE students JAHAN dob > '2004-01-01';         -- dates compare with 'YYYY-MM-DD'
DIKHAO ALAG cid SE students;                              -- DISTINCT

-- Aggregates, GROUP BY, HAVING, aliases
DIKHAO GINO(*) KAHO total, AUSAT(umar) KAHO avg_umar SE students;
DIKHAO cid, GINO(*) KAHO total SE students SAMOOH cid JINKA GINO(*) > 1 KRAM total ULTA;

-- Joins
DIKHAO s.naam, c.title SE students s MILAO courses c PAR s.cid = c.id;         -- INNER JOIN
DIKHAO s.naam, c.title SE students s BAAYAN MILAO courses c PAR s.cid = c.id;  -- LEFT JOIN

-- UPDATE / DELETE
BADLO students RAKHO umar = umar + 1 JAHAN id = 1;
MITAO SE students JAHAN umar HAI KHALI;
```

### Transactions

```sql
SHURU;                                               -- BEGIN
BADLO accounts RAKHO paisa = paisa - 500 JAHAN id = 1;
BADLO accounts RAKHO paisa = paisa + 500 JAHAN id = 2;
PAKKA;                                               -- COMMIT (or WAPAS; = ROLLBACK)
```

Other clients never see changes that haven't been committed yet. If the connection
drops or the program crashes before `PAKKA`, the whole transaction is rolled back.

### Query plans

```sql
SAMJHAO DIKHAO * SE students JAHAN id = 3;
--  1. INDEX LOOKUP students PAR id = 3  [hash index, MUKHYA KUNJI]
--  2. FILTER  JAHAN id = 3
--  3. PROJECT  id, naam, umar, dob, cid
```

`SAMJHAO` works with `DIKHAO`, `BADLO` and `MITAO`. It shows the plan without running the query.

---

## Where the data lives

| OS | Default data folder |
|----|---------------------|
| Windows | `%LOCALAPPDATA%\MeraDB\data` |
| macOS / Linux | `~/.local/share/MeraDB/data` |

Override it with `-D <folder>` or `MERADB_DATA`. Inside the folder:

```
data/
  meradb.pid        which process/port serves this folder (only while the server runs)
  server.log        background server log
  main/             the default database
    catalog.json    table schemas
    students.tbl    table rows (binary format)
  college/          another database
```

To back up, stop the server and copy the folder. To reset everything, stop the server and
delete the folder.

---

## How MeraDB works

### Is MeraDB SQL or NoSQL?

**MeraDB is a relational (SQL-family) database. It is not NoSQL.**

| | Relational / SQL databases (MySQL, Oracle...) | NoSQL (MongoDB, Redis...) | **MeraDB** |
|---|---|---|---|
| Data stored as | Tables of rows and columns | Documents, key-value pairs, graphs | **Tables of rows and columns** |
| Schema | Fixed, declared with CREATE TABLE | Flexible or none | **Fixed, declared with `BANAO TABLE`** |
| Constraints (PK, FK, UNIQUE, CHECK) | Yes | Mostly no | **Yes** |
| Joins between tables | Yes | Usually no | **Yes** (`MILAO`) |
| Transactions (COMMIT / ROLLBACK) | Yes | Limited | **Yes** (`PAKKA` / `WAPAS`) |
| Query language | SQL | Its own API | **SQL-style, with Hinglish keywords** |

The query language follows SQL's structure clause for clause. Only the words change:

```
SQL:     SELECT naam FROM students WHERE cgpa > 8 ORDER BY naam;
MeraDB:  DIKHAO naam SE   students JAHAN cgpa > 8 KRAM     naam;
```

So everything you learn about SQL applies here: DDL/DML, keys, normalisation, joins,
GROUP BY, NULL rules and transactions. The one practical difference is that MeraDB speaks
its own language, so standard SQL tools and drivers can't connect to it directly. You use
MeraDB's own workbench and shell instead.

### Server and clients

```
   meradb workbench      meradb shell      your Python program
          |                    |                    |
          +-------- network (TCP port 6372) --------+
                               |
                    +----------v----------+
                    |    MeraDB server    |   meradb start / stop / status
                    |  (one session per   |
                    |  connected client)  |
                    +----------+----------+
                               |
                    data folder: catalog.json + .tbl files
```

1. **The server** (`meradb start`) runs in the background, owns the data files and waits for connections.
2. **Clients** (workbench, shell, Python) connect to it over the network. You can connect
   several at once, even from other computers on your network.
3. **Each connection is its own session**, with its own current database and its own transaction.
4. **Inside the server**, every query passes through the same stages:

   | Stage | What it does |
   |-------|--------------|
   | Tokenizer | Splits the text into words |
   | Parser | Checks the grammar and builds a syntax tree |
   | Planner | Decides index lookup or full scan, and hash join or nested loop |
   | Executor | Filters, joins, groups, sorts |
   | Storage | Reads and writes the binary table files |

5. **Results** go back to the client, which shows them as a table.

How each stage works, down to the bytes on disk, is explained in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

### Coming from MySQL Workbench?

The MeraDB workbench is organised the same way, so the usual workflow carries over:

| In MySQL Workbench | In MeraDB | How |
|---|---|---|
| Start the MySQL server (service) | Start the MeraDB server | `meradb start` |
| Open a connection (host, port, password) | Connect to the server | `meradb workbench`, or **Ctrl+O** for another server |
| Navigator: schemas, tables, columns | Schema tree (left panel) | Enter on a table previews its rows |
| `USE college;` / double-click a schema | Switch database | `ISTEMAL college;` or Enter on the database |
| SQL editor tab | Query editor (bottom) | Type queries, with syntax highlighting |
| Execute (lightning bolt / Ctrl+Enter) | Run | **F5** |
| Execute only the selected statement | Run only the selection | Select it, then **F5** |
| Result grid | Results panel | Shows the last result |
| Output panel (messages, errors, duration) | Log panel | Every message, error and its time in ms |
| Explain / Visual Explain | Query plan | **F6** (or `SAMJHAO` before a query) |
| Export result set to CSV | Export | **Ctrl+S** (saved in `exports/`) |
| Auto-commit, START TRANSACTION / COMMIT / ROLLBACK | Same model | Auto-commit by default; `SHURU` / `PAKKA` / `WAPAS` |
| Previous statements (history) | Query history | **Ctrl+Up / Ctrl+Down** |
| Table inspector / `DESCRIBE t` | Describe | `BATAO t;` (columns, types, constraints) |

What MeraDB's workbench does not have (yet): multiple editor tabs, editing result cells
directly in the grid, and the visual ER-diagram designer.

### Limits to know

MeraDB is a learning-scale database:
- It runs one statement at a time, so clients take turns.
- Indexes are hash indexes on primary-key and unique columns, and they speed up `=` lookups only.
- Triggers have no procedural control flow (no loops), and stored procedures have no
  return value or local variables -- both are simplified, not SQL-standard-complete (see
  `docs/LANGUAGE.md`).
- Privileges are per `(database, table)` only -- no schema-level roles, no column-level
  grants, no `WITH GRANT OPTION`.
- Security is one optional server-wide password, or per-user hashed-password logins with
  privileges -- either way, no TLS encryption.
- It is comfortable up to around 100,000 rows per table.

---

## Running the tests

C++ version: see [Run the tests](#run-the-tests) above (`ctest`, about 780 tests).

Python version:

```bash
python -m unittest
```

183 tests, run in about 15 seconds. They cover:
- the tokenizer and parser
- every DDL/DML command and every constraint
- joins, indexes, transactions and crash recovery
- the real client–server path over TCP, including `meradb start` / `stop`
- the workbench

Run one file with, for example, `python -m unittest tests.test_constraints`.

---

## Project structure

Python version (the C++ version is described after this block):

```
meradb/
  tokenizer.py   text -> tokens
  parser.py      tokens -> syntax tree (recursive descent)
  ast_nodes.py   syntax tree node definitions
  planner.py     name binding, index vs scan, hash join vs nested loop
  engine.py      executor, sessions, constraints, transactions + crash recovery
  evaluator.py   expressions, NULL logic, LIKE
  aggregates.py  COUNT / SUM / AVG / MIN / MAX
  table.py       table = schema + heap file + hash indexes
  catalog.py     table schemas (catalog.json)
  storage.py     binary row format + heap files
  datatypes.py   types and value checking
  server.py      TCP server (one thread + session per client)
  client.py      Python client library
  protocol.py    wire format, pid file, default data folder
  cli.py         the `meradb` command
  repl.py        interactive shell
  tui.py         workbench
  highlight.py   syntax highlighting
tests/           automated tests
examples/        demo.mdb: every feature in one script
docs/            documentation
```

### C++ implementation

```
cpp/
  include/meradb/   headers (one per Python module, plus the shell and workbench)
  src/              sources: engine, storage, server, client, shell, workbench
  tests/            ~780 ctest tests: unit tests, golden files, comparison scripts
  cmake/            helper scripts for the build
build.ps1, build.sh build helpers (Windows / POSIX)
```

The C++ port shares the Python engine's grammar, output, on-disk formats and wire protocol,
and is checked against it (example scripts, a client/server interop matrix, data folders passed
between the engines, a trigger/procedure fuzz). It has the full engine, users and privileges,
triggers, stored procedures, the TCP server, `run` / `start` / `stop` / `status`, the
interactive shell and the full-screen workbench (FTXUI). The module-by-module mapping from
the Python files is in [docs/CPP.md](docs/CPP.md).

### Use from Python (Python version)

```python
from meradb.client import Connection

with Connection("127.0.0.1", 6372) as db:            # a running server
    result = db.execute("DIKHAO * SE students;")[0]
    print(result.columns, result.rows)

from meradb import Engine                            # or embedded, no server
engine = Engine("mydata")
engine.execute("BANAO TABLE t (x INT); DAALO MEIN t MAAN (1);")
```

---

## Documentation

- [docs/LANGUAGE.md](docs/LANGUAGE.md): complete language reference, SQL mapping, formal grammar (EBNF)
- [docs/SERVER.md](docs/SERVER.md): running the server, passwords, network access, autostart, wire protocol, troubleshooting
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how it works inside: storage format byte by byte, planner, indexes, joins, transactions, concurrency
- [docs/ROADMAP.md](docs/ROADMAP.md): how the project was built, week by week
- [docs/REPORT.md](docs/REPORT.md): project report
- [docs/CPP.md](docs/CPP.md): the C++ port (build instructions, layout, verification, known divergences, status)
