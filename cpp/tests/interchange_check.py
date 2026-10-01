"""
Data-folder interchange: a folder written by one engine must be usable by the other.

Every combination (builder, runner, verifier) of the two engines works on ONE data folder in turn:
  1. builder  runs BUILD  (tables, triggers, procedures, users, grants)
  2. runner   runs USE    (fires the triggers, calls the procedures, hits the guards)
  3. verifier runs VERIFY (more of the same, drops a procedure and a user)
All 8 chains must print exactly what the all-Python chain printed, and end with the same
`catalog.json` (same content and key order) and equivalent `users.json` (same users and grants,
salts/hashes of the right shape, and passwords that Python's UserStore verifies).

    python cpp/tests/interchange_check.py --cli path/to/meradb_cli
"""
import argparse
import itertools
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.stdout.reconfigure(encoding="utf-8")  # diffs may contain non-ASCII text; never die on a console code page
sys.path.insert(0, str(REPO_ROOT))

BUILD = """
BANAO TABLE accounts (id INT MUKHYA KUNJI, naam TEXT, balance INT);
BANAO TABLE audit (naam TEXT, purana INT, naya INT);
BANAO TABLE ledger (note TEXT, amount INT);
DAALO MEIN accounts MAAN (1, 'Ravi', 1000), (2, 'café 世界', 50);
BANAO TRIGGER t_audit BAAD BADLO PAR accounts SHURU
    DAALO MEIN audit MAAN (NAYA.naam, PURANA.balance, NAYA.balance);
KHATAM;
BANAO TRIGGER t_guard PEHLE BADLO PAR accounts SHURU
    DAALO MEIN ledger MAAN ('guard', 1 / (AGAR NAYA.balance < 0 TAB 0 WARNA 1 KHATAM));
KHATAM;
BANAO TRIGGER t_ins BAAD DAALO PAR accounts SHURU
    DAALO MEIN ledger MAAN ('new', NAYA.id);
    DIKHAO * SE ledger;
KHATAM;
BANAO PROCEDURE jama (p_id INT, p_kitna INT) SHURU
    BADLO accounts RAKHO balance = balance + p_kitna JAHAN id = p_id;
    DAALO MEIN ledger MAAN ('jama', p_kitna);
KHATAM;
BANAO PROCEDURE naam_badlo (p_id INT, p_naam TEXT) SHURU
    BADLO accounts RAKHO naam = p_naam JAHAN id = p_id;
KHATAM;
BANAO USER asha GUPT 'pw-sécret';
BANAO USER ravi GUPT 'x';
ADHIKAR DO DIKHAO, DAALO PAR accounts KO asha;
ADHIKAR DO SAB PAR ledger KO ravi;
ADHIKAR WAPAS DAALO PAR accounts SE asha;
"""

USE = """
BADLO accounts RAKHO balance = balance - 100 JAHAN id = 1;
BADLO accounts RAKHO balance = balance - 5000 JAHAN id = 2;
CHALAO jama(1, 250);
CHALAO naam_badlo(2, 'Priya O''Neil');
CHALAO jama(9);
CHALAO nahi_hai(1);
DAALO MEIN accounts MAAN (3, 'Meena', 10);
MITAO SE accounts JAHAN id = 3;
HATAO TRIGGER t_ins;
DAALO MEIN accounts MAAN (4, 'Zed', 1);
DIKHAO * SE accounts KRAM id;
DIKHAO * SE audit;
DIKHAO * SE ledger;
"""

VERIFY = """
CHALAO jama(2, 1);
DIKHAO * SE accounts KRAM id;
DIKHAO * SE audit;
HATAO PROCEDURE naam_badlo;
CHALAO naam_badlo(1, 'x');
HATAO USER ravi;
BANAO TRIGGER t_ins BAAD DAALO PAR accounts SHURU
    DAALO MEIN ledger MAAN ('again', NAYA.id);
KHATAM;
DIKHAO * SE ledger;
"""


def env() -> dict:
    e = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_")}
    e.update(PYTHONIOENCODING="utf-8", PYTHONPATH=str(REPO_ROOT))
    return e


def run_engine(kind: str, cli: str, data: str, script: str) -> tuple[str, int]:
    base = [sys.executable, "-m", "meradb"] if kind == "python" else [cli]
    r = subprocess.run(base + ["run", "--local", "--data", data, script], cwd=REPO_ROOT, capture_output=True, env=env(), timeout=300)
    return r.stdout.decode("utf-8").replace("\r\n", "\n"), r.returncode


def users_shape(data: str) -> dict:
    users = json.load(open(os.path.join(data, "users.json"), encoding="utf-8"))
    shape = {}
    for name, record in users.items():
        assert list(record.keys()) == ["salt", "hash", "grants"], (name, list(record.keys()))
        assert len(record["salt"]) == 32 and len(record["hash"]) == 64, name
        int(record["salt"], 16), int(record["hash"], 16)
        shape[name] = record["grants"]
    return shape


def main() -> int:
    from meradb.users import UserStore

    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", required=True)
    args = ap.parse_args()
    args.cli = str(Path(args.cli).resolve())  # children run with cwd=REPO_ROOT, so a relative path would break

    results, failures = {}, []
    with tempfile.TemporaryDirectory() as tmp:
        scripts = {}
        for name, text in (("build", BUILD), ("use", USE), ("verify", VERIFY)):
            scripts[name] = os.path.join(tmp, name + ".mdb")
            Path(scripts[name]).write_text(text, encoding="utf-8")
        for builder, runner, verifier in itertools.product(("python", "cpp"), repeat=3):
            chain = f"{builder}>{runner}>{verifier}"
            data = os.path.join(tmp, "data_" + chain.replace(">", "_"))
            outputs = [run_engine(k, args.cli, data, scripts[s]) for k, s in ((builder, "build"), (runner, "use"), (verifier, "verify"))]
            try:
                catalog = json.dumps(json.load(open(os.path.join(data, "main", "catalog.json"), encoding="utf-8")))
                store = UserStore(data)
                passwords_ok = store.verify("asha", "pw-sécret") and not store.verify("asha", "nope") and "ravi" not in store.users
                shape = users_shape(data)
            except (OSError, ValueError, AssertionError) as e:  # a chain whose folder is missing or malformed is a FAIL, not a crash
                catalog, passwords_ok, shape = f"<unreadable: {e}>", False, {}
            results[chain] = (outputs, catalog, shape, passwords_ok)

        baseline = results["python>python>python"]
        for chain, got in results.items():
            problems = []
            for step, (a, b) in enumerate(zip(baseline[0], got[0])):
                if a != b:
                    problems.append(f"step {step} output differs")
                    import difflib
                    problems.append("".join(list(difflib.unified_diff(a[0].splitlines(keepends=True), b[0].splitlines(keepends=True), "python-chain", chain))[:40]))
            if got[1] != baseline[1]:
                problems.append("catalog.json differs from the all-Python chain")
            if got[2] != baseline[2]:
                problems.append(f"users.json shape differs: {got[2]} vs {baseline[2]}")
            if not got[3]:
                problems.append("a password does not verify in Python's UserStore")
            if problems:
                failures.append(chain)
                print(f"FAIL {chain}\n  " + "\n  ".join(problems))
            else:
                print(f"PASS {chain}")
    print("ALL MATCHED" if not failures else "FAILED: " + ", ".join(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
