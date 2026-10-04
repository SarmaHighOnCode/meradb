"""
The full-screen workbench on a real pseudo-terminal (POSIX): it is started with --local on a temp data folder, driven
with the bytes a terminal sends, and its screen is rebuilt from what it wrote (ptyutil.Screen, a minimal VT model).

    python cpp/tests/pty_workbench.py path/to/meradb_cli

Checks: the start-up screen, a query run with F5, Ctrl+S (CSV) not freezing the terminal, Ctrl+C only hinting,
bracketed paste with a Tab, a resize, Ctrl+Q (exit 0, terminal modes and screen restored, an open transaction rolled
back), SIGHUP / SIGTERM (clean rollback and restore) and SIGKILL (crash recovery on the next start); with Textual
installed the Python workbench is put through the same exit and restore checks.

Exits 77 (ctest: skipped) where there is no pty (Windows).
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

import ptyutil   # exits 77 (skip) where there is no pty
import fcntl
import struct
from ptyutil import Checker, Pty, Screen, clean_env, flags, python_env

CLI = sys.argv[1]
if not ptyutil.can_open_pty():
    print("SKIP: cannot open a pseudo-terminal here")
    sys.exit(77)

ck = Checker()
ROWS, COLS = 40, 120


class Bench:
    """A workbench on a pty plus the screen model that follows its output."""

    def __init__(self, data, cwd, rows=ROWS, cols=COLS, argv=None, env=None):
        self.pty = Pty(argv or [CLI, "workbench", "--local", "-D", data], env=env or clean_env(), rows=rows, cols=cols,
                       cwd=cwd)
        self.screen = Screen(rows, cols)
        self.seen = 0

    def update(self, wait=0.15):
        self.pty.pump(wait)
        self.screen.feed(self.pty.buf[self.seen:])
        self.seen = len(self.pty.buf)

    def wait_for(self, text, timeout=10.0):
        end = time.time() + timeout
        while time.time() < end:
            self.update(0.1)
            if text in self.screen.text():
                return True
            if self.pty.proc.poll() is not None:
                self.update(0.1)
                return text in self.screen.text()
        return False

    def send(self, data, wait=0.2):
        self.pty.send(data)
        self.update(wait)

    def close(self):
        self.pty.close()


def run_script(data, script):
    """Run `script` through the CLI on the data folder (the workbench must be gone); returns (stdout, stderr)."""
    r = subprocess.run([CLI, "run", "--local", "-D", data, script], capture_output=True, timeout=60, env=clean_env())
    return r.stdout.decode("utf-8", "replace"), r.stderr.decode("utf-8", "replace")


def write_script(root, text):
    path = os.path.join(root, "q.mdb")
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return path


F5 = b"\x1b[15~"
root = tempfile.mkdtemp(prefix="mdb_ptywb_")
benches = []
try:
    # ---- start-up, a query, Ctrl+S, Ctrl+C, paste, resize, Ctrl+Q ------------------------------------------------------
    data = os.path.join(root, "data")
    work = os.path.join(root, "work")
    os.makedirs(work)
    b = Bench(data, work)
    benches.append(b)
    initial = b.pty.initial_attrs
    ck.check("start-up: the header, the panels and the footer are drawn",
             b.wait_for("MeraDB Workbench") and all(w in b.screen.text() for w in ("Schema", "Results", "Log", "Query", "F5 Chalao", "^Q Bahar")),
             b.screen.text())
    ck.check("start-up: header shows local mode and the database", "local (" in b.screen.lines()[0] and "db: main" in b.screen.lines()[0],
             b.screen.lines()[0])
    ck.check("start-up: the alternate screen, bracketed paste and mouse reporting are switched on",
             {1049, 2004, 1000}.issubset(b.screen.modes), str(sorted(b.screen.modes)))
    running = b.pty.attrs()
    isig, ixon, icanon, echo, iexten = flags(running)
    ck.check("running: signals, flow control, canonical mode and echo are off", not isig and not ixon and not icanon and not echo,
             str(flags(running)))

    b.send(b"BANAO TABLE t (id INT, naam TEXT);\nDAALO MEIN t MAAN (1, 'Ravi'), (2, 'Priya');\nDIKHAO * SE t;")
    ck.check("typing: the text appears in the editor with line numbers and the cursor stays in it",
             "3 DIKHAO * SE t;" in b.screen.text(), b.screen.text())
    b.send(F5, 0.8)
    b.wait_for("Results -- 2 row(s)")
    text = b.screen.text()
    ck.check("F5: the results title and the rows are shown", "Results -- 2 row(s)" in text and "Ravi" in text and "Priya" in text, text)
    ck.check("F5: the log has the echo, the messages and a timing", "Table 't' ban gaya" in text and "2 row(s) daal di" in text and "ms)" in text, text)
    ck.check("F5: the schema tree shows the new table", "▶ t" in text, text)

    b.send(b"\x13", 0.5)    # Ctrl+S: write a CSV
    text = b.screen.text()
    ck.check("Ctrl+S: arrives as a key (the terminal does not freeze), the CSV is written and logged",
             "CSV mein save" in text, text[-900:])
    b.send(b"Z", 0.4)
    ck.check("after Ctrl+S the terminal still takes input (no XOFF freeze)", "Z" in b.screen.text(), "")
    b.send(b"\x7f", 0.3)

    b.send(b"\x03", 0.4)    # Ctrl+C
    ck.check("Ctrl+C: a hint in the log, the workbench keeps running", b.pty.proc.poll() is None and "Ctrl+Q" in b.screen.text(),
             b.screen.text()[-900:])

    # bracketed paste with a Tab inside
    b.send(b"\x1b[200~PASTE\tED\x1b[201~", 0.5)
    text = b.screen.text()
    ck.check("bracketed paste: a Tab inside the paste is inserted into the editor as spaces", "PASTE    ED" in text, text[-900:])
    b.send(b"!", 0.3)
    ck.check("bracketed paste: the focus stayed in the editor", "PASTE    ED!" in b.screen.text(), b.screen.text()[-900:])

    # resize: a smaller window, then one below the minimum, then back
    def resize(rows, cols):
        fcntl.ioctl(b.pty.master, ptyutil.termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        b.pty.signal(signal.SIGWINCH)
        b.screen = Screen(rows, cols)
        b.seen = len(b.pty.buf)
        b.update(0.6)

    resize(30, 100)
    lines = b.screen.lines()
    ck.check("resize to 100 x 30: the layout follows (header on the first row, footer on the last)",
             "MeraDB Workbench" in lines[0] and "^Q Bahar" in lines[29], "\n".join(lines))
    resize(20, 50)
    ck.check("resize below the minimum: the too-small message", "bahut chhota" in b.screen.text(), b.screen.text())
    resize(ROWS, COLS)
    ck.check("resize back: the full layout returns", "MeraDB Workbench" in b.screen.text() and "Results" in b.screen.text(), b.screen.text())

    # open a transaction and quit with Ctrl+Q: rolled back, terminal restored
    b.send(b"\x01", 0.2)    # Ctrl+A: select all
    b.send(b"SHURU;\nDAALO MEIN t MAAN (3, 'Neha');", 0.3)
    b.send(F5, 0.8)
    b.wait_for("TRANSACTION")
    ck.check("an open transaction shows the marker in the header", "TRANSACTION" in b.screen.lines()[0], b.screen.lines()[0])
    b.send(b"\x11", 0.3)
    rc = b.pty.wait(15)
    b.update(0.2)
    ck.check("Ctrl+Q: exit code 0", rc == 0, "exit %r" % (rc,))
    after = b.pty.attrs()
    ck.check("Ctrl+Q: the terminal modes (ISIG, IXON, ICANON, ECHO, IEXTEN) are back as they were",
             after is None or flags(after) == flags(initial), "before %r after %r" % (flags(initial), after and flags(after)))
    out = b.pty.text()
    ck.check("Ctrl+Q: the alternate screen, mouse reporting and bracketed paste are switched off again",
             "\x1b[?1049l" in out and "\x1b[?2004l" in out and "\x1b[?1000l" in out, repr(out[-200:]))
    ck.check("Ctrl+Q: the cursor is shown again", out.rfind("\x1b[?25h") > out.rfind("\x1b[?25l"), repr(out[-200:]))
    so, se = run_script(data, write_script(root, "DIKHAO * SE t;"))
    ck.check("Ctrl+Q with a transaction open: it was rolled back (2 rows, no RECOVERY note)",
             "2 row(s)" in so and "Neha" not in so and "RECOVERY" not in se, so + se)
    b.close()

    # ---- signals ----------------------------------------------------------------------------------------------------
    for sig, name, expect_recovery in ((signal.SIGHUP, "SIGHUP", False), (signal.SIGTERM, "SIGTERM", False),
                                       (signal.SIGKILL, "SIGKILL", True)):
        sdata = os.path.join(root, "data_" + name)
        sb = Bench(sdata, work)
        benches.append(sb)
        sb.wait_for("MeraDB Workbench")
        sb.send(b"BANAO TABLE s (id INT);\nDAALO MEIN s MAAN (1);", 0.2)
        sb.send(F5, 0.6)
        sb.send(b"\x01SHURU;\nDAALO MEIN s MAAN (2);", 0.3)
        sb.send(F5, 0.8)
        sb.wait_for("TRANSACTION")
        started = sb.pty.initial_attrs
        sb.pty.signal(sig)
        rc = sb.pty.wait(15)
        sb.update(0.2)
        ck.check("%s with a transaction open: the process ends (no hang)" % name, rc is not None, "still running")
        if sig == signal.SIGHUP:
            ck.check("SIGHUP: exit code 0 (the loop ended like Ctrl+Q)", rc == 0, "exit %r" % (rc,))
        if sig != signal.SIGKILL:
            after = sb.pty.attrs()
            ck.check("%s: the terminal modes are restored" % name, after is None or flags(after) == flags(started),
                     "before %r after %r" % (flags(started), after and flags(after)))
            ck.check("%s: the alternate screen is left" % name, "\x1b[?1049l" in sb.pty.text(), repr(sb.pty.text()[-150:]))
        so, se = run_script(sdata, write_script(root, "DIKHAO * SE s;"))
        if expect_recovery:
            ck.check("SIGKILL: the next start recovers the transaction (RECOVERY note) and shows 1 row",
                     "RECOVERY" in se and "1 row(s)" in so, so + se)
        else:
            ck.check("%s: the open transaction was rolled back cleanly (1 row, no RECOVERY note)" % name,
                     "1 row(s)" in so and "RECOVERY" not in se, so + se)
        sb.close()

    # ---- the Python workbench on the same kind of terminal (needs Textual) ----------------------------------------------
    try:
        import textual  # noqa: F401
        have_textual = True
    except ImportError:
        have_textual = False
    if have_textual:
        pdata = os.path.join(root, "data_py")
        pb = Bench(pdata, work, argv=[sys.executable, "-m", "meradb", "workbench", "--local", "-D", pdata], env=python_env())
        benches.append(pb)
        started = pb.pty.initial_attrs
        shown = pb.wait_for("Workbench", 20) or pb.wait_for("Schema", 5)
        pb.send(b"\x11", 0.5)
        rc = pb.pty.wait(15)
        pb.update(0.2)
        after = pb.pty.attrs()
        ck.check("Python workbench (reference): Ctrl+Q exits 0 and restores the terminal",
                 rc == 0 and (after is None or flags(after) == flags(started)),
                 "exit %r before %r after %r" % (rc, flags(started), after and flags(after)))
        ck.check("Python workbench (reference): uses the alternate screen as well", "\x1b[?1049h" in pb.pty.text(), "")
        pb.close()
    else:
        print("SKIP the Python workbench comparison: Textual is not installed")
finally:
    for bench in benches:
        bench.close()
    shutil.rmtree(root, ignore_errors=True)

sys.exit(ck.finish())
