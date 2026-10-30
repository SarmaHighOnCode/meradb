"""
Real process kills: start the C++ shell on a data folder, kill it (TerminateProcess / SIGKILL) at varied moments during
inserts, big updates, transactions, schema changes and trigger-firing statements, reopen the folder and check that

    * every statement / PAKKA the shell acknowledged is there,
    * a statement that was not acknowledged is either wholly there or wholly gone, never half,
    * an uncommitted transaction is gone,
    * the files load and take new writes.

    python cpp/tests/crash_kill.py path/to/meradb_cli [--rounds N] [--seed S] [--only NAME]

The kills come in two kinds. Timed ones land anywhere, but a fixed window can be missed; so MERADB_TEST_CRASH_POINT also
stops the process exactly at each step of the journal and commit protocol (exit code 77 from the crash point itself).

A kill loses nothing the operating system has already been given. So this proves the process-crash half of the promise,
and the write ORDER that the power-loss half depends on, but NOT survival of a real power cut: that takes pulling the plug
on real hardware, and no test here can do it. See docs/ARCHITECTURE.md.
"""
import argparse
import os
import queue
import random
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

parser = argparse.ArgumentParser()
parser.add_argument("cli")
parser.add_argument("--rounds", type=int, default=6)
parser.add_argument("--seed", type=int, default=20261030)
parser.add_argument("--only", default="")
ARGS = parser.parse_args()
CLI = ARGS.cli
RNG = random.Random(ARGS.seed)
failures = []
passes = 0


def check(name, condition, detail=""):
    global passes
    if condition:
        passes += 1
        return
    print("FAIL " + name)
    if detail:
        print("     " + str(detail).replace("\n", "\n     "))
    failures.append(name)


def clean_env(extra=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_")}
    env.update(extra or {})
    return env


def run_sql(data, sql, extra=None, timeout=240):
    """A fresh process running a script: returns (exit code, stdout + stderr)."""
    fd, path = tempfile.mkstemp(suffix=".mdb", dir=os.path.dirname(data))
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(sql)
    try:
        r = subprocess.run([CLI, "run", "--local", "-D", data, path], capture_output=True, timeout=timeout,
                           env=clean_env(extra))
        return r.returncode, (r.stdout + r.stderr).decode("utf-8", "replace").replace("\r\n", "\n")
    finally:
        os.unlink(path)


def ints(data, sql):
    """Every integer cell of the result of one DIKHAO, in order; raises if the folder would not load."""
    code, out = run_sql(data, sql)
    if code != 0 or "Error" in out or "error" in out or "corrupt" in out:
        raise RuntimeError(f"query failed ({code}): {sql}\n{out}")
    cells = []
    for line in out.splitlines():
        if line.startswith("|") or line.startswith("│"):
            cells += [int(c) for c in re.findall(r"(?<=[|│])\s*(-?\d+)\s*(?=[|│])", line)]
    return cells


def count(data, table, where=""):
    return ints(data, f"DIKHAO GINO(*) SE {table}{' JAHAN ' + where if where else ''};")[0]


class Shell:
    """The real shell over pipes. A statement counts as acknowledged once its message has been printed."""

    def __init__(self, data, extra=None):
        self.proc = subprocess.Popen([CLI, "shell", "--local", "-D", data], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, env=clean_env(extra))
        self.lines = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        try:
            for raw in self.proc.stdout:
                self.lines.put(raw.decode("utf-8", "replace"))
        finally:
            self.lines.put(None)

    def send(self, sql):
        self.proc.stdin.write((sql + "\n").encode("utf-8"))
        self.proc.stdin.flush()

    def ack(self, needle, timeout=120):
        """Wait for a line holding needle. False when the process died first."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if line is None:
                self.lines.put(None)
                return False
            if needle in line:
                return True
        raise RuntimeError(f"no '{needle}' from the shell in {timeout}s")

    def do(self, sql, needle):
        try:
            self.send(sql)
        except (OSError, ValueError):  # the pipe closed: the process is gone
            return False
        return self.ack(needle)

    def kill(self):
        try:
            self.proc.kill()
        except OSError:
            pass
        self.proc.wait(timeout=60)
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        self.proc.stdout.close()


class Folder:
    def __init__(self):
        self.root = tempfile.mkdtemp(prefix="meradb-kill-")
        self.n = 0

    def template(self, sql):
        path = os.path.join(self.root, "template")
        code, out = run_sql(path, sql)
        if code != 0:
            raise RuntimeError(out)
        return path

    def fresh(self, template=None):
        self.n += 1
        path = os.path.join(self.root, f"d{self.n}")
        if template:
            shutil.copytree(template, path)
        else:
            os.makedirs(path)
        return path

    def close(self):
        shutil.rmtree(self.root, ignore_errors=True)


def after_kill_still_works(name, data, table_sql="BANAO TABLE zinda (x INT); DAALO MEIN zinda MAAN (1);"):
    """The reopened folder must take new work, and survive a second reopen."""
    code, out = run_sql(data, table_sql + " DIKHAO GINO(*) SE zinda;")
    check(f"{name}: takes new writes after recovery", code == 0 and "Error" not in out and "corrupt" not in out, out)
    code, out = run_sql(data, "DIKHAO TABLES;")
    check(f"{name}: second reopen is clean", code == 0 and "RECOVERY" not in out, out)


def kill_after(shell, delay):
    timer = threading.Timer(delay, shell.kill)
    timer.start()
    return timer


def timed_statement(name, template_sql, statement, verify, rounds=None, ack="row(s)"):
    """Kills one long statement at moments spread over the time it takes; verify(data, acked) checks each result."""
    folder = Folder()
    try:
        template = folder.template(template_sql)
        # how long does it take when nothing kills it?
        data = folder.fresh(template)
        started = time.monotonic()
        code, out = run_sql(data, statement)
        whole = time.monotonic() - started
        check(f"{name}: runs when not killed", code == 0, out)
        hits = 0
        total = rounds or ARGS.rounds
        for i in range(total):
            data = folder.fresh(template)
            shell = Shell(data)
            shell.do("ISTEMAL main;", "istemal")  # up and running: the kill clock starts now
            delay = whole * (i + RNG.random()) / total
            timer = kill_after(shell, delay)
            finished = shell.do(statement, ack)  # a kill can land while the statement is still being sent
            timer.join()
            hits += 0 if finished else 1
            try:
                verify(f"{name}[{i}]", data, finished)
                after_kill_still_works(f"{name}[{i}]", data)
            except RuntimeError as e:
                check(f"{name}[{i}]: folder loads", False, e)
        print(f"     {name}: {hits}/{total} kills landed before the acknowledgement (statement takes {whole * 1000:.0f} ms)")
    finally:
        folder.close()


def rows_sql(n, v=0, start=0):
    return "DAALO MEIN t MAAN " + ",".join(f"({i}, {v})" for i in range(start, start + n)) + ";\n"


# ---- autocommit inserts: everything acknowledged is there, at most the one in flight besides
def scenario_autocommit_inserts():
    for i in range(ARGS.rounds):
        folder = Folder()
        try:
            data = folder.fresh()
            run_sql(data, "BANAO TABLE t (id INT MUKHYA KUNJI, v INT);")
            shell = Shell(data)
            acked = 0
            box = {"go": True}
            timer = threading.Timer(0.02 + RNG.random() * 0.25, lambda: (box.update(go=False), shell.kill()))
            timer.start()
            while box["go"]:
                if not shell.do(f"DAALO MEIN t MAAN ({acked}, 1);", "row(s)"):
                    break
                acked += 1
            timer.join()
            have = sorted(ints(data, "DIKHAO id SE t KRAM id;"))
            check(f"autocommit[{i}]: all {acked} acknowledged rows there", have[:acked] == list(range(acked)), have[:10])
            check(f"autocommit[{i}]: at most the statement in flight on top", len(have) <= acked + 1, f"{len(have)} vs {acked}")
            after_kill_still_works(f"autocommit[{i}]", data)
        finally:
            folder.close()


# ---- one statement over many rows: all or nothing
def scenario_multi_row_insert():
    def verify(name, data, finished):
        n = count(data, "t")
        check(f"{name}: rows are all there or all gone", n in (1000, 31000), n)
        if finished:
            check(f"{name}: acknowledged means there", n == 31000, n)
    timed_statement("multi_row_insert", "BANAO TABLE t (id INT, v INT);\n" + rows_sql(1000), rows_sql(30000, start=1000), verify)


def scenario_big_update():
    def verify(name, data, finished):
        n = count(data, "t")
        moved = count(data, "t", "v = 1")
        check(f"{name}: no row lost or doubled", n == 40000, n)
        check(f"{name}: the update is whole or absent", moved in (0, 40000), moved)
        if finished:
            check(f"{name}: acknowledged means there", moved == 40000, moved)
    timed_statement("big_update", "BANAO TABLE t (id INT, v INT);\n" + rows_sql(40000), "BADLO t RAKHO v = 1;", verify)


def scenario_big_delete():
    def verify(name, data, finished):
        n = count(data, "t")
        check(f"{name}: delete is whole or absent", n in (5000, 40000), n)
        if finished:
            check(f"{name}: acknowledged means there", n == 5000, n)
    timed_statement("big_delete", "BANAO TABLE t (id INT, v INT);\n" + rows_sql(40000), "MITAO SE t JAHAN id >= 5000;", verify)


def scenario_alter():
    def verify(name, data, finished):
        n = count(data, "t")
        check(f"{name}: no row lost", n == 30000, n)
        code, out = run_sql(data, "DIKHAO GINO(naya) SE t;")
        has = code == 0 and "nahi" not in out and "Error" not in out
        code2, out2 = run_sql(data, "DAALO MEIN t MAAN (" + ("-1, -1, 5" if has else "-1, -1") + "); DIKHAO GINO(*) SE t JAHAN id = -1;")
        check(f"{name}: catalog and rows agree", code2 == 0 and "corrupt" not in out2, out2)
    timed_statement("alter", "BANAO TABLE t (id INT, v INT);\n" + rows_sql(30000), "SUDHARO TABLE t JODO naya INT WARNA 7;", verify, ack="naya")


def scenario_trigger_statement():
    def verify(name, data, finished):
        n, audit = count(data, "t", "v = 1"), count(data, "audit")
        check(f"{name}: rows and trigger writes go together", (n, audit) in ((0, 0), (1500, 1500)), (n, audit))
        if finished:
            check(f"{name}: acknowledged means there", (n, audit) == (1500, 1500), (n, audit))
    setup = ("BANAO TABLE t (id INT, v INT);\nBANAO TABLE audit (id INT);\n" + rows_sql(1500) +
             "BANAO TRIGGER tr BAAD BADLO PAR t SHURU DAALO MEIN audit MAAN (NAYA.id); KHATAM;\n")
    timed_statement("trigger", setup, "BADLO t RAKHO v = 1;", verify, rounds=min(ARGS.rounds, 4))


# ---- transactions
def scenario_open_transaction():
    for i in range(ARGS.rounds):
        folder = Folder()
        try:
            data = folder.fresh()
            run_sql(data, "BANAO TABLE t (id INT, v INT);\nDAALO MEIN t MAAN (0, 0);")
            shell = Shell(data)
            shell.do("SHURU;", "SHURU")
            n = RNG.randint(1, 40)
            for k in range(n):
                shell.do(f"DAALO MEIN t MAAN ({k + 1}, 1);", "row(s)")
            shell.do("BADLO t RAKHO v = 9 JAHAN id = 0;", "row(s)")
            shell.kill()
            have = ints(data, "DIKHAO id, v SE t;")
            check(f"open_txn[{i}]: uncommitted work is gone", have == [0, 0], have)
            after_kill_still_works(f"open_txn[{i}]", data)
        finally:
            folder.close()


def scenario_commits():
    for i in range(ARGS.rounds):
        folder = Folder()
        try:
            data = folder.fresh()
            run_sql(data, "BANAO TABLE t (batch INT, k INT);")
            shell = Shell(data)
            box = {"go": True}
            timer = threading.Timer(0.05 + RNG.random() * 0.5, lambda: (box.update(go=False), shell.kill()))
            timer.start()
            committed = 0
            while box["go"]:
                ok = shell.do("SHURU;", "SHURU")
                for k in range(5):
                    ok = ok and shell.do(f"DAALO MEIN t MAAN ({committed}, {k});", "row(s)")
                if not ok or not shell.do("PAKKA;", "PAKKA"):
                    break
                committed += 1
            timer.join()
            rows = ints(data, "DIKHAO batch, k SE t;")
            batches = {}
            for b, k in zip(rows[::2], rows[1::2]):
                batches[b] = batches.get(b, 0) + 1
            check(f"commits[{i}]: every acknowledged PAKKA is there",
                  all(batches.get(b) == 5 for b in range(committed)), (committed, batches))
            check(f"commits[{i}]: every transaction is whole", all(v == 5 for v in batches.values()), batches)
            check(f"commits[{i}]: at most the one in flight on top", len(batches) <= committed + 1, (committed, batches))
            after_kill_still_works(f"commits[{i}]", data)
        finally:
            folder.close()


# ---- each step of the protocol, exactly. (name, setup, statement, what must hold afterwards)
BASE = "BANAO TABLE t (id INT, v INT);\nBANAO TABLE u (id INT);\n" + rows_sql(50)


def rows_are(data, n, moved=None):
    if count(data, "t") != n:
        return f"t has {count(data, 't')} rows, wanted {n}"
    if moved is not None and count(data, "t", "v = 1") != moved:
        return f"{count(data, 't', 'v = 1')} rows moved, wanted {moved}"
    return None


def tables(data):
    code, out = run_sql(data, "DIKHAO TABLES;")
    return out


def is_gone(table):
    return lambda data: None if f"| {table} " not in tables(data) and f"│ {table} " not in tables(data) else f"{table} still there"


def undone(data):
    return rows_are(data, 50, 0)


INSERT = rows_sql(20, v=1, start=100)
TXN = "SHURU;\n" + rows_sql(20, v=1, start=100) + "BADLO t RAKHO v = 1;\nPAKKA;\n"
EXACT = [
    # one statement outside a transaction: undone, up to and including the last step before its commit point
    ("append: journal written, nothing else", "stmt-after-log", INSERT, undone),
    ("append: just before the write", "stmt-before-write", INSERT, undone),
    ("append: half a record written", "stmt-torn-write", INSERT, undone),
    ("append: written, not synced", "stmt-after-write", INSERT, undone),
    ("append: synced, commit point not reached", "stmt-after-sync", INSERT, undone),
    ("append: after the commit point", "stmt-after-commit", INSERT, lambda d: rows_are(d, 70, 20)),
    ("update: half the tombstones", "stmt-mid-tombstones", "BADLO t RAKHO v = 1;", undone),
    ("update: tombstones done, rows not added", "stmt-after-write", "BADLO t RAKHO v = 1;", undone),
    ("update: journal's second record", "stmt-after-log:2", "BADLO t RAKHO v = 1;", undone),
    ("update: after the commit point", "stmt-after-commit", "BADLO t RAKHO v = 1;", lambda d: rows_are(d, 50, 50)),
    ("delete: half the tombstones", "stmt-mid-tombstones", "MITAO SE t;", undone),
    # schema changes
    ("create table: catalog not swapped", "catalog-before-rename", "BANAO TABLE naya (x INT);", is_gone("naya")),
    ("create table: not committed", "stmt-after-sync", "BANAO TABLE naya (x INT);", is_gone("naya")),
    ("drop table: not committed", "stmt-after-sync", "HATAO TABLE t;", undone),
    ("truncate: not committed", "stmt-after-sync", "SAAF TABLE t;", undone),
    ("compact: not committed", "stmt-after-sync", "SIKODO TABLE t;", undone),
    ("add column: not committed", "stmt-after-sync", "SUDHARO TABLE t JODO naya INT WARNA 7;", undone),
    ("rename table: not committed", "stmt-after-sync", "SUDHARO TABLE t NAYA_NAAM t2;", undone),
    ("rename table: after the commit point", "stmt-after-commit", "SUDHARO TABLE u NAYA_NAAM u2;",
     lambda d: None if "u2" in tables(d) else "u2 missing"),
    # transactions
    ("SHURU: copy done, not named", "snapshot-before-rename", TXN, undone),
    ("SHURU: named", "snapshot-after-rename", TXN, undone),
    ("PAKKA: data synced, marker not moved", "commit-before-marker", TXN, undone),
    ("PAKKA: marker moved", "commit-after-marker", TXN, lambda d: rows_are(d, 70, 70)),
    ("WAPAS: live folder deleted", "rollback-after-remove", "SHURU;\n" + INSERT + "WAPAS;\n", undone),
    ("savepoint rollback: between the two renames", "restore-copy-between-renames",
     "SHURU;\nSAVEPOINT a;\n" + INSERT + "ROLLBACK TO a;\nPAKKA;\n", undone),
    # a statement before these must not be disturbed, and its longer journal must not be read as this one's
    ("after an update whose journal was longer", "stmt-after-log:3", "BADLO t RAKHO v = 1;\nDAALO MEIN t MAAN (100, 1);\n",
     lambda d: rows_are(d, 50, 50)),
    ("second statement of two", "stmt-after-log:2", INSERT + rows_sql(5, v=1, start=200), lambda d: rows_are(d, 70, 20)),
]


def scenario_exact_points():
    folder = Folder()
    try:
        template = folder.template(BASE)
        for name, point, statement, verify in EXACT:
            data = folder.fresh(template)
            code, out = run_sql(data, statement, {"MERADB_TEST_CRASH_POINT": point})
            check(f"exact '{name}': the crash point was reached", code == 77, f"exit {code}\n{out}")
            try:
                problem = verify(data)
                check(f"exact '{name}'", problem is None, problem)
                after_kill_still_works(f"exact '{name}'", data)
            except RuntimeError as e:
                check(f"exact '{name}': folder loads", False, e)
    finally:
        folder.close()


def scenario_trigger_exact():
    """Crash inside a trigger-firing statement, on the second row's trigger: the rows and the trigger's writes go."""
    folder = Folder()
    try:
        setup = (BASE + "BANAO TABLE audit (id INT);\n"
                 "BANAO TRIGGER tr BAAD BADLO PAR t SHURU DAALO MEIN audit MAAN (NAYA.id); KHATAM;\n")
        template = folder.template(setup)
        for n in (1, 2, 3, 10):
            data = folder.fresh(template)
            code, out = run_sql(data, "BADLO t RAKHO v = 1;", {"MERADB_TEST_CRASH_POINT": f"stmt-after-write:{n}"})
            check(f"trigger exact {n}: the crash point was reached", code == 77, out)
            check(f"trigger exact {n}: all undone", (count(data, "t", "v = 1"), count(data, "audit")) == (0, 0),
                  (count(data, "t", "v = 1"), count(data, "audit")))
            check(f"trigger exact {n}: no row lost", count(data, "t") == 50)
            after_kill_still_works(f"trigger exact {n}", data)
    finally:
        folder.close()


def scenario_crash_during_recovery():
    """Recovery itself is killed after each of its steps; the next start finishes the job."""
    folder = Folder()
    try:
        template = folder.template(BASE)
        cases = [("update", "BADLO t RAKHO v = 1;"), ("drop", "HATAO TABLE t;"), ("add column", "SUDHARO TABLE t JODO naya INT WARNA 7;")]
        for name, sql in cases:
            for step in (1, 2):
                data = folder.fresh(template)
                code, out = run_sql(data, sql, {"MERADB_TEST_CRASH_POINT": "stmt-after-sync"})
                check(f"recovery {name}/{step}: first crash", code == 77, out)
                code, out = run_sql(data, "DIKHAO TABLES;", {"MERADB_TEST_CRASH_POINT": f"recovery-after-undo:{step}"})
                # (a statement with fewer steps than this finishes recovery instead of crashing: both are fine)
                if step == 1:
                    check(f"recovery {name}: the crash point was reached", code == 77, out)
                check(f"recovery {name}/{step}: state after the second start", undone(data) is None, undone(data))
                after_kill_still_works(f"recovery {name}/{step}", data)
    finally:
        folder.close()


def scenario_relaxed():
    """MERADB_SYNC=relaxed gives up the power cut, not the kill."""
    folder = Folder()
    try:
        template = folder.template(BASE)
        for name, point, statement, verify in EXACT[:8] + EXACT[19:22]:
            data = folder.fresh(template)
            code, out = run_sql(data, statement, {"MERADB_TEST_CRASH_POINT": point, "MERADB_SYNC": "relaxed"})
            check(f"relaxed '{name}': the crash point was reached", code == 77, out)
            problem = verify(data)
            check(f"relaxed '{name}'", problem is None, problem)
    finally:
        folder.close()


SCENARIOS = {
    "exact": scenario_exact_points,
    "trigger_exact": scenario_trigger_exact,
    "during_recovery": scenario_crash_during_recovery,
    "relaxed": scenario_relaxed,
    "autocommit": scenario_autocommit_inserts,
    "multi_row": scenario_multi_row_insert,
    "big_update": scenario_big_update,
    "big_delete": scenario_big_delete,
    "alter": scenario_alter,
    "trigger": scenario_trigger_statement,
    "open_txn": scenario_open_transaction,
    "commits": scenario_commits,
}


def main():
    for name, scenario in SCENARIOS.items():
        if ARGS.only and ARGS.only != name:
            continue
        started = time.monotonic()
        before = len(failures)
        scenario()
        print(f"{'ok  ' if len(failures) == before else 'FAIL'} {name} ({time.monotonic() - started:.1f}s)")
    print(f"{passes} checks passed, {len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
