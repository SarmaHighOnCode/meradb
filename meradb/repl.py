"""
The SHELL (a REPL: Read-Eval-Print Loop), MeraDB's command-line client.

    meradb shell                     # connect to the server (or run locally if none)
    meradb run examples/demo.mdb     # run a script file, then exit

It works with any "backend" that has run_script / current_db / in_transaction /
description / close -- an embedded Engine or a client Connection to a server.

A statement can span many lines; it runs once you type a `;`.
Lines starting with `.` are shell commands (not part of the language): .help, .exit ...
"""

import os
import sys

from . import __version__
from .datatypes import format_value
from .engine import Result
from .errors import MeraDBError

# ============================================================================
# Colour (zero dependencies): plain ANSI escape codes, used only when we are
# confident the terminal understands them. NO_COLOR (https://no-color.org)
# and a non-terminal (piped/redirected output, as in `meradb run x.mdb > out`)
# both turn it off, so scripted output and test captures stay plain text.
# ============================================================================


def _enable_conhost_ansi() -> bool:
    """
    Ask the classic Windows console host (conhost.exe, used by a plain cmd.exe
    or "Windows PowerShell" window) to render ANSI escape codes. Windows 10+
    supports this; older Windows and non-console terminals do not, so every
    failure here just means "leave colour off", never a crash.
    """
    try:
        if sys.getwindowsversion().major < 10:
            return False
        import ctypes

        kernel32 = ctypes.windll.kernel32
        handle = kernel32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
        mode = ctypes.c_uint32()
        if not kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            return False
        return bool(kernel32.SetConsoleMode(handle, mode.value | 0x0004))  # ENABLE_VIRTUAL_TERMINAL_PROCESSING
    except Exception:
        return False


def _supports_color() -> bool:
    if os.environ.get("NO_COLOR") or not sys.stdout.isatty():
        return False
    if os.name != "nt":
        return True  # real terminals on Linux/macOS support ANSI as standard
    # Modern terminal emulators on Windows (Windows Terminal, Git Bash/mintty,
    # ConEmu...) already understand ANSI with no extra step -- and calling the
    # conhost-only API below on one of them can wrongly report "unsupported".
    if os.environ.get("WT_SESSION") or os.environ.get("TERM") or os.environ.get("ConEmuANSI") == "ON":
        return True
    # Otherwise this is the classic console host: ask it to turn ANSI on.
    return _enable_conhost_ansi()


_COLOR = _supports_color()


def _c(text: str, *codes: int) -> str:
    """Wrap `text` in ANSI SGR codes (1=bold 2=dim 31=red 32=green 33=yellow 35=magenta 36=cyan)."""
    if not _COLOR or not codes:
        return text
    return f"\x1b[{';'.join(map(str, codes))}m{text}\x1b[0m"


def _can_encode(text: str) -> bool:
    """
    Whether stdout's actual encoding can render `text`. Some Windows consoles
    (and any redirected/piped output, e.g. `meradb run x.mdb > out.txt`) fall
    back to a legacy codepage like cp1252 that has no box-drawing characters
    -- encoding there would crash, so we check first and use plain ASCII
    instead when it can't, the same rule format_table already follows.
    """
    try:
        text.encode(sys.stdout.encoding or "utf-8", errors="strict")
        return True
    except (UnicodeEncodeError, LookupError):
        return False


# A middle dot where the terminal supports it, a plain asterisk otherwise.
_DOT = "·" if _can_encode("·") else "*"

# Solid block glyph for the logo, where encodable; a hash for legacy codepages
# and piped/redirected output. Same rule format_table already follows.
_BLOCK = "█" if _can_encode("█") else "#"

# ============================================================================
# Logo: a hand-authored 5x7 dot-matrix font, just for the letters M E R A D B
# (not a general-purpose font -- it only needs to spell one word).
#   1 = filled pixel, . = empty. Each glyph is 7 rows of 5 columns.
# ============================================================================
_FONT = {
    "M": ["1...1", "11.11", "1.1.1", "1.1.1", "1...1", "1...1", "1...1"],
    "E": ["11111", "1....", "1....", "1111.", "1....", "1....", "11111"],
    "R": ["1111.", "1...1", "1...1", "1111.", "1.1..", "1..1.", "1...1"],
    "A": [".111.", "1...1", "1...1", "11111", "1...1", "1...1", "1...1"],
    "D": ["1111.", "1...1", "1...1", "1...1", "1...1", "1...1", "1111."],
    "B": ["1111.", "1...1", "1...1", "1111.", "1...1", "1...1", "1111."],
}


def _render_logo() -> tuple[list[str], int]:
    """
    "MERADB" as 7 lines of block-letter art, split and coloured as MERA + DB
    -- mera = "my" in Hindi/Urdu, so the wordmark itself reads "my DB".
    Each pixel is drawn 2 characters wide, because a terminal cell is taller
    than it is wide, and a single-character-wide pixel would look squeezed.

    Returns (coloured_lines, plain_width). The width is measured separately
    from the coloured lines, because ANSI escape codes are invisible on
    screen but not to len() -- centering text under the logo needs the
    on-screen width, not the string length.
    """
    halves = [("MERA", (1, 36)), ("DB", (1, 35))]  # (letters, colour codes)
    rows = [""] * 7
    width = 0
    for half_i, (letters, style) in enumerate(halves):
        for letter_i, letter in enumerate(letters):
            glyph = _FONT[letter]
            for r in range(7):
                pixels = "".join(_BLOCK * 2 if px == "1" else "  " for px in glyph[r])
                rows[r] += _c(pixels, *style)
            width += 10  # each glyph is 5 pixels, drawn 2 characters wide
            if letter_i != len(letters) - 1:
                for r in range(7):
                    rows[r] += " "  # gap between letters in the same half
                width += 1
        if half_i != len(halves) - 1:
            for r in range(7):
                rows[r] += "   "  # wider gap between MERA and DB
            width += 3
    return rows, width


# ============================================================================
# Banner
# ============================================================================


def render_banner(version: str, where: str) -> str:
    logo, width = _render_logo()

    def centered(plain_text: str, *style: int) -> str:
        pad = max(0, (width - len(plain_text)) // 2)
        return " " * pad + _c(plain_text, *style)

    lines = (
        [""]
        + logo
        + [
            "",
            centered(f"meraDB {version}", 1),
            centered("apna database, apni bhasha", 2),
            "",
            f"  connected: {where}",
            f"  {_c('.help', 1, 33)} commands  {_c(_DOT, 2)}  {_c('.exit', 1, 33)} bahar niklo  {_c(_DOT, 2)}  "
            f"statements {_c(';', 1, 33)} se khatam hote hain",
        ]
    )
    return "\n".join(lines)


# ============================================================================
# Help: a keyword reference (like the README's table) plus worked examples.
# Kept as plain data so `.help <word>` can search it -- e.g. `.help sandarbh`
# or `.help join` shows just the matching rows instead of the whole thing.
# ============================================================================

# category -> [(MeraDB syntax, SQL equivalent, what it does), ...]
HELP_REFERENCE: list[tuple[str, list[tuple[str, str, str]]]] = [
    (
        "Databases",
        [
            ("BANAO DATABASE naam", "CREATE DATABASE", "naya database banao"),
            ("ISTEMAL naam", "USE", "database switch karo"),
            ("HATAO DATABASE naam", "DROP DATABASE", "database hatao"),
            ("DIKHAO TABLES", "SHOW TABLES", "current database ki tables list karo"),
        ],
    ),
    (
        "Tables (DDL)",
        [
            ("BANAO TABLE t (...)", "CREATE TABLE", "naya table banao"),
            ("BATAO t", "DESCRIBE t", "table ka structure dikhao"),
            ("SUDHARO TABLE t JODO col type", "ALTER TABLE ADD COLUMN", "column jodo"),
            ("SUDHARO TABLE t HATAO col", "ALTER TABLE DROP COLUMN", "column hatao"),
            ("SUDHARO TABLE t NAYA_NAAM naya", "ALTER TABLE RENAME TO", "table ka naam badlo"),
            ("SUDHARO TABLE t COLUMN c NAYA_NAAM naya", "ALTER ... RENAME COLUMN", "column ka naam badlo"),
            ("SAAF TABLE t", "TRUNCATE TABLE", "saari rows hatao, table rakho"),
            ("SIKODO TABLE t", "VACUUM", "deleted rows ki jagah wapas lo"),
            ("HATAO TABLE t", "DROP TABLE", "table hamesha ke liye hatao"),
        ],
    ),
    (
        "Constraints",
        [
            ("MUKHYA KUNJI", "PRIMARY KEY", "unique + not null, ek hi column"),
            ("ZAROORI", "NOT NULL", "KHALI (NULL) allowed nahi"),
            ("ANOKHA", "UNIQUE", "duplicate values allowed nahi"),
            ("WARNA value", "DEFAULT", "column chhod do to ye value milegi"),
            ("SANDARBH t(col)", "FOREIGN KEY", "doosre table ke column ko refer karo (RESTRICT)"),
            ("SHART (expr)", "CHECK", "har row ke liye ek condition true honi chahiye"),
        ],
    ),
    (
        "Data types",
        [
            ("INT / ANK", "INTEGER", "whole number"),
            ("FLOAT / DASHAMLAV", "REAL", "decimal number"),
            ("TEXT / SHABD", "TEXT", "string, jaise 'Ravi'"),
            ("VARCHAR(n) / CHAR(n)", "VARCHAR(n)", "TEXT jiski max length n ho"),
            ("BOOL / HAAN_NA", "BOOLEAN", "SACH ya JHOOTH"),
            ("DATE / TAREEKH", "DATE", "'YYYY-MM-DD' string ki tarah likha jata hai"),
            ("NUMBER(p,s) / DECIMAL", "NUMERIC(p,s)", "FLOAT ka alias; p,s accept hote hain, ignore hote hain"),
        ],
    ),
    (
        "Insert / update / delete",
        [
            ("DAALO MEIN t (...) MAAN (...)", "INSERT INTO ... VALUES", "ek ya zyada rows daalo"),
            ("BADLO t RAKHO col = expr JAHAN cond", "UPDATE ... SET ... WHERE", "rows badlo"),
            ("MITAO SE t JAHAN cond", "DELETE FROM ... WHERE", "rows hatao"),
        ],
    ),
    (
        "Select: filtering & sorting",
        [
            ("DIKHAO cols SE t", "SELECT ... FROM", "rows dikhao"),
            ("JAHAN cond", "WHERE", "sirf matching rows"),
            ("KRAM col [ULTA]", "ORDER BY [DESC]", "sort karo (SEEDHA = ASC, ULTA = DESC)"),
            ("SIRF n", "LIMIT", "sirf pehli n rows"),
            ("ALAG", "DISTINCT", "duplicate rows hatao"),
            ("naam JAISA 'R%'", "LIKE 'R%'", "pattern match (% = kuch bhi, _ = ek character)"),
            ("umar BEECH a AUR b", "BETWEEN a AND b", "a aur b ke beech, dono included"),
            ("id MEIN (a, b, ...)", "IN (a, b, ...)", "list mein hai kya"),
            ("col HAI [NAHI] KHALI", "IS [NOT] NULL", "KHALI (NULL) check"),
        ],
    ),
    (
        "Aggregates & grouping",
        [
            ("GINO(*) / GINO(col)", "COUNT(*) / COUNT(col)", "kitni rows (ya non-KHALI values)"),
            ("KUL(col)", "SUM(col)", "total"),
            ("AUSAT(col)", "AVG(col)", "average"),
            ("NYUNTAM(col)", "MIN(col)", "sabse chhota"),
            ("ADHIKTAM(col)", "MAX(col)", "sabse bada"),
            ("SAMOOH col", "GROUP BY", "rows ko group karo"),
            ("JINKA cond", "HAVING", "groups ko filter karo"),
            ("expr KAHO alias", "expr AS alias", "output column ka naya naam"),
        ],
    ),
    (
        "Joins",
        [
            ("SE t1 a MILAO t2 b PAR cond", "JOIN ... ON", "sirf matching rows (INNER JOIN)"),
            ("SE t1 a BAAYAN MILAO t2 b PAR cond", "LEFT JOIN ... ON", "har t1 row; match na mile to KHALI"),
        ],
    ),
    (
        "Transactions & tools",
        [
            ("SHURU", "BEGIN", "transaction shuru karo"),
            ("PAKKA", "COMMIT", "transaction ke changes save karo"),
            ("WAPAS", "ROLLBACK", "transaction ke changes undo karo"),
            ("SAMJHAO query", "EXPLAIN", "query chalaye bina uska plan dikhao"),
        ],
    ),
]

SHELL_COMMANDS_HELP = f"""{_c("Shell commands", 1, 35)}
  .help [khoj]     ye reference dikhao (poora, ya sirf matching keywords -- jaise: .help join)
  .tables          saari tables dikhao
  .schema <table>  table ka structure
  .run <file>      ek .mdb script file chalao
  .exit / .nikal   bahar niklo"""

EXAMPLES_HELP = """Quick examples (full reference: docs/LANGUAGE.md):
  BANAO DATABASE college;   ISTEMAL college;   HATAO DATABASE college;
  BANAO TABLE courses (id INT MUKHYA KUNJI, title TEXT ANOKHA);
  BANAO TABLE students (id INT MUKHYA KUNJI, naam VARCHAR(30) ZAROORI, cid INT SANDARBH courses(id),
                         umar INT WARNA 18 SHART (umar >= 0 AUR umar < 150), dob DATE);
  SUDHARO TABLE students JODO email TEXT;       SUDHARO TABLE students HATAO email;
  SUDHARO TABLE students NAYA_NAAM pupils;      SUDHARO TABLE pupils COLUMN naam NAYA_NAAM full_naam;
  SAAF TABLE students;   SIKODO TABLE students;   HATAO TABLE students;
  DIKHAO TABLES;         BATAO students;
  DAALO MEIN students (id, naam, umar) MAAN (1, 'Ravi', 20), (2, 'Priya', 19);
  DIKHAO * SE students JAHAN umar > 18 AUR naam != 'Ravi' KRAM umar ULTA SIRF 10;
  DIKHAO * SE students JAHAN naam JAISA 'R%' YA umar BEECH 18 AUR 21 YA id MEIN (1, 2);
  DIKHAO * SE students JAHAN dob > '2003-01-01';                -- DATE compared to a string
  DIKHAO umar, GINO(*) KAHO total SE students SAMOOH umar JINKA GINO(*) > 1 KRAM total ULTA;
  DIKHAO ALAG umar SE students;
  DIKHAO s.naam, c.title SE students s MILAO courses c PAR s.cid = c.id;   (BAAYAN MILAO = LEFT JOIN)
  BADLO students RAKHO umar = umar + 1 JAHAN id = 1;
  MITAO SE students JAHAN umar HAI KHALI;
  SHURU;  ...  PAKKA;   (or WAPAS; to undo)       SAMJHAO DIKHAO * SE students JAHAN id = 1;"""


def _render_reference(query: str = "") -> str:
    """
    The keyword reference, optionally filtered to rows whose syntax, SQL name
    or description contain `query` (case-insensitive) -- e.g. `.help foreign`.
    """
    query = query.strip().lower()
    matched = []
    for category, rows in HELP_REFERENCE:
        rows = [r for r in rows if not query or query in " ".join(r).lower()]
        if rows:
            matched.append((category, rows))

    if query and not matched:
        return f"'{query}' ke liye kuch nahi mila. Poora reference ke liye sirf .help likho."

    kw_width = max(len(r[0]) for _, rows in matched for r in rows)
    sql_width = max(len(r[1]) for _, rows in matched for r in rows)

    out = []
    for category, rows in matched:
        out.append(_c(category, 1, 35))
        for keyword, sql, desc in rows:
            out.append(f"  {_c(keyword.ljust(kw_width), 1, 36)}  {_c(sql.ljust(sql_width), 2)}  {desc}")
        out.append("")
    return "\n".join(out).rstrip("\n")


# ============================================================================
# Result rendering
# ============================================================================


def format_table(result: Result) -> str:
    """Render a Result as an ASCII table (ASCII borders, so it works in every Windows console)."""
    headers = result.columns
    body = [[format_value(v) for v in row] for row in result.rows]
    widths = [len(h) for h in headers]
    for row in body:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(cell))

    sep = _c("+" + "+".join("-" * (w + 2) for w in widths) + "+", 2)

    def line(cells, bold_row: bool = False) -> str:
        text = "| " + " | ".join(c.ljust(w) for c, w in zip(cells, widths)) + " |"
        return _c(text, 1) if bold_row else text

    out = [sep, line(headers, bold_row=True), sep]
    out += [line(row) for row in body]
    out.append(sep)
    return "\n".join(out)


def print_result(result: Result) -> None:
    if result.error:
        print(_c(result.error, 31))
        return
    if result.columns:
        print(format_table(result))
    if result.message:
        print(_c(result.message, 32))


def run_text(backend, text: str) -> bool:
    """
    Execute text and print results. Returns False if any error happened.
    A failing statement prints its error and the NEXT statements still run.
    """
    try:
        results = backend.run_script(text)
    except MeraDBError as e:  # e.g. the server connection dropped
        print(e)
        return False
    for result in results:
        print_result(result)
        print()
    return not any(r.error for r in results)


def run_file(backend, path: str) -> bool:
    try:
        # utf-8-sig: also accepts files saved WITH a byte-order mark, which
        # Notepad and PowerShell 5 (Out-File -Encoding utf8) add on Windows
        with open(path, "r", encoding="utf-8-sig") as f:
            text = f.read()
    except OSError as e:
        print(f"File nahi khuli: {e}")
        return False
    return run_text(backend, text)


def handle_dot_command(backend, line: str) -> bool:
    """Returns False when the user wants to exit."""
    parts = line.split()
    cmd, args = parts[0].lower(), parts[1:]
    if cmd in (".exit", ".quit", ".nikal"):
        return False
    if cmd == ".help":
        topic = line[len(parts[0]) :].strip()  # everything after ".help", so multi-word topics work too
        if topic:
            print(_render_reference(topic))
        else:
            print(SHELL_COMMANDS_HELP)
            print()
            print(_render_reference())
            print()
            print(EXAMPLES_HELP)
    elif cmd == ".tables":
        run_text(backend, "DIKHAO TABLES;")
    elif cmd == ".schema" and args:
        run_text(backend, f"BATAO {args[0]};")
    elif cmd == ".run" and args:
        run_file(backend, args[0])
    else:
        print(f"Ye shell command nahi pata: {line}  (.help dekho)")
    return True


def prompt_for(backend) -> str:
    # a `*` in the prompt means a transaction is open
    marker = _c("*", 1, 33) if backend.in_transaction else ""
    return f"meradb:{_c(backend.current_db, 1, 36)}{marker}> "


def repl(backend) -> None:
    print()
    print(render_banner(__version__, backend.description))
    print()
    buffer = ""
    while True:
        prompt = prompt_for(backend) if not buffer else "      ...> "
        try:
            line = input(prompt)
        except (EOFError, KeyboardInterrupt):
            print("\nPhir milenge!")
            return

        if not buffer and line.strip().startswith("."):
            if not handle_dot_command(backend, line.strip()):
                print("Phir milenge!")
                return
            continue

        buffer += line + "\n"
        if buffer.rstrip().endswith(";"):
            run_text(backend, buffer)
            buffer = ""
