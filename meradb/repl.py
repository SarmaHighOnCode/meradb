"""
The SHELL (a REPL: Read-Eval-Print Loop), MeraDB's command-line client.

    meradb shell                     # connect to the server (or run locally if none)
    meradb run examples/demo.mdb     # run a script file, then exit

It works with any "backend" that has run_script / current_db / in_transaction /
description / close -- an embedded Engine or a client Connection to a server.

A statement can span many lines; it runs once you type a `;`.
Lines starting with `.` are shell commands (not part of the language): .help, .exit ...
"""

from . import __version__
from .datatypes import format_value
from .engine import Result
from .errors import MeraDBError

BANNER = r"""
  __  __                 ____  ____
 |  \/  | ___ _ __ __ _|  _ \| __ )
 | |\/| |/ _ \ '__/ _` | | | |  _ \
 | |  | |  __/ | | (_| | |_| | |_) |
 |_|  |_|\___|_|  \__,_|____/|____/

 MeraDB {version} -- apna database, apni bhasha.
 Connected: {where}
 Type .help for help, .exit to quit. Statements end with ;
"""

HELP = """
Shell commands:
  .help            ye message
  .tables          saari tables dikhao
  .schema <table>  table ka structure
  .run <file>      ek .mdb script file chalao
  .exit / .nikal   bahar niklo

Language (full reference: docs/LANGUAGE.md):
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
  SHURU;  ...  PAKKA;   (or WAPAS; to undo)       SAMJHAO DIKHAO * SE students JAHAN id = 1;
"""


def format_table(result: Result) -> str:
    """Render a Result as an ASCII table (ASCII only, so it works in every Windows console)."""
    headers = result.columns
    body = [[format_value(v) for v in row] for row in result.rows]
    widths = [len(h) for h in headers]
    for row in body:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(cell))

    sep = "+" + "+".join("-" * (w + 2) for w in widths) + "+"

    def line(cells):
        return "| " + " | ".join(c.ljust(w) for c, w in zip(cells, widths)) + " |"

    out = [sep, line(headers), sep]
    out += [line(row) for row in body]
    out.append(sep)
    return "\n".join(out)


def print_result(result: Result) -> None:
    if result.error:
        print(result.error)
        return
    if result.columns:
        print(format_table(result))
    if result.message:
        print(result.message)


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
        print(HELP)
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
    return f"meradb:{backend.current_db}{'*' if backend.in_transaction else ''}> "


def repl(backend) -> None:
    print(BANNER.format(version=__version__, where=backend.description))
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
