"""
Seeded differential fuzz for triggers and stored procedures: random but valid
scripts run through the Python engine and the C++ CLI (both `--local`), and the
outputs must be identical.

    python cpp/tests/fuzz_triggers.py --cli path/to/meradb_cli [--seeds 200] [--start 0]
    python cpp/tests/fuzz_triggers.py --seed 17 --show          # print the script for one seed
    python cpp/tests/fuzz_triggers.py --stats                   # Python only: how "busy" are the scripts?

A failing seed prints the script's path and a unified diff; re-run just that seed with --seed N.
The generator sticks to constructs whose behaviour is fully defined by the language, and avoids
what is a DELIBERATE divergence: triggers never write to the table they are on (self recursion
hits Python's RecursionError and the C++ nesting cap), and no identifiers are non-ASCII.

Schema (fixed): acct(id PK, name, bal), log(kind, id, a, b), ctr(n) with one row.
Triggers are only ever defined on acct (writing to log and ctr) and on ctr (writing to log).
"""
import argparse
import difflib
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.stdout.reconfigure(encoding="utf-8")  # diffs may contain non-ASCII text; never die on a console code page

NAMES = ["Ravi", "Priya", "O'Neil", "café", "世界", "", "x y"]


def sql_text(s: str) -> str:
    return "'" + s.replace("'", "''") + "'"


def trigger_body(rng: random.Random, event: str, timing: str, tag: str, on_ctr: bool) -> str:
    """One to three statements; only tables other than the trigger's own are written."""
    if on_ctr:
        return f"DAALO MEIN log MAAN ('{tag}', NAYA.n, PURANA.n, 0);" if event == "BADLO" else f"DAALO MEIN log MAAN ('{tag}', 0, 0, 0);"
    choices = []
    if event == "DAALO":
        choices += [f"DAALO MEIN log MAAN ('{tag}', NAYA.id, NAYA.bal, 0);",
                    f"DAALO MEIN log MAAN ('{tag}n', NAYA.id, NAYA.bal + 1, NAYA.bal * 2);",
                    "BADLO ctr RAKHO n = n + 1;"]
    elif event == "BADLO":
        choices += [f"DAALO MEIN log MAAN ('{tag}', NAYA.id, PURANA.bal, NAYA.bal);",
                    f"DAALO MEIN log MAAN ('{tag}d', NAYA.id, NAYA.bal - PURANA.bal, 0);",
                    "BADLO ctr RAKHO n = n + NAYA.bal - PURANA.bal;"]
    else:
        choices += [f"DAALO MEIN log MAAN ('{tag}', PURANA.id, PURANA.bal, 0);",
                    "BADLO ctr RAKHO n = n - 1;"]
    if timing == "PEHLE" and event != "MITAO" and rng.random() < 0.35:  # a guard that fails when bal < 0
        choices.append(f"DAALO MEIN log MAAN ('{tag}g', NAYA.id, 1 / (AGAR NAYA.bal < 0 TAB 0 WARNA 1 KHATAM), 0);")
    if rng.random() < 0.2:
        choices.append("DIKHAO GINO(*) SE log;")
    count = rng.randint(1, min(3, len(choices)))
    return "\n    ".join(rng.sample(choices, count))


def generate(seed: int) -> str:
    rng = random.Random(seed)
    out = ["BANAO TABLE acct (id INT MUKHYA KUNJI, name TEXT, bal INT);",
           "BANAO TABLE log (kind TEXT, id INT, a INT, b INT);",
           "BANAO TABLE ctr (n INT);",
           "DAALO MEIN ctr MAAN (0);"]
    triggers = []
    for i in range(rng.randint(1, 5)):
        on_ctr = rng.random() < 0.2
        timing = rng.choice(["PEHLE", "BAAD"])
        event = rng.choice(["BADLO"] if on_ctr else ["DAALO", "BADLO", "MITAO"])
        name = f"trg{i}"
        body = trigger_body(rng, event, timing, f"{name}", on_ctr)
        table = "ctr" if on_ctr else "acct"
        out.append(f"BANAO TRIGGER {name} {timing} {event} PAR {table} SHURU\n    {body}\nKHATAM;")
        triggers.append(name)
    procs = []
    if rng.random() < 0.8:
        out.append("BANAO PROCEDURE bump (pid INT, amt INT) SHURU\n    BADLO acct RAKHO bal = bal + amt JAHAN id = pid;\n"
                   "    DAALO MEIN log MAAN ('bump', pid, amt, 0);\nKHATAM;")
        procs.append("bump")
    if rng.random() < 0.5:
        out.append("BANAO PROCEDURE rename (pid INT, nm TEXT) SHURU\n    BADLO acct RAKHO name = nm JAHAN id = pid;\nKHATAM;")
        procs.append("rename")
    if rng.random() < 0.3:
        out.append("BANAO PROCEDURE ping () SHURU\n    DAALO MEIN log MAAN ('ping', 0, 0, 0);\nKHATAM;")
        procs.append("ping")

    for _ in range(rng.randint(10, 40)):
        roll = rng.random()
        ident = rng.randint(1, 8)
        if roll < 0.30:
            out.append(f"DAALO MEIN acct MAAN ({ident}, {sql_text(rng.choice(NAMES))}, {rng.randint(-50, 500)});")
        elif roll < 0.45:
            out.append(f"BADLO acct RAKHO bal = bal + {rng.randint(-300, 300)} JAHAN id = {ident};")
        elif roll < 0.52:
            out.append(f"BADLO acct RAKHO bal = bal * 2 JAHAN bal > {rng.randint(0, 300)};")
        elif roll < 0.62:
            out.append(f"MITAO SE acct JAHAN id = {ident};")
        elif roll < 0.66:
            out.append(f"MITAO SE acct JAHAN bal < {rng.randint(0, 100)};")
        elif roll < 0.72:
            out.append(f"DAALO MEIN acct MAAN ({ident}, 'up', {rng.randint(0, 99)}) TAKRAAV PAR BADLO bal = bal + {rng.randint(1, 9)};")
        elif roll < 0.86 and procs:
            name = rng.choice(procs)
            if name == "bump":
                args = f"{ident}, {rng.randint(-200, 200)}" if rng.random() < 0.9 else f"{ident}"  # sometimes a wrong count
            elif name == "rename":
                args = f"{ident}, {sql_text(rng.choice(NAMES))}"
            else:
                args = ""
            out.append(f"CHALAO {name}({args});")
        elif roll < 0.90 and triggers:
            victim = rng.choice(triggers)
            out.append(f"HATAO TRIGGER {victim};")
        elif roll < 0.93:
            out.append(f"BANAO TRIGGER late{rng.randint(0, 3)} BAAD DAALO PAR acct SHURU\n    DAALO MEIN log MAAN ('late', NAYA.id, 0, 0);\nKHATAM;")
        else:
            out.append(rng.choice(["DIKHAO * SE acct KRAM id;", "DIKHAO * SE log;", "DIKHAO * SE ctr;",
                                   "DIKHAO kind, GINO(*) SE log SAMOOH kind KRAM kind;"]))
    out += ["DIKHAO * SE acct KRAM id;", "DIKHAO * SE log;", "DIKHAO * SE ctr;"]
    return "\n".join(out) + "\n"


def env() -> dict:
    e = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_")}
    e.update(PYTHONIOENCODING="utf-8", PYTHONPATH=str(REPO_ROOT))
    return e


def run(cmd: list[str]) -> tuple[str, int]:
    r = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, env=env(), timeout=120)
    return r.stdout.decode("utf-8").replace("\r\n", "\n"), r.returncode


def default_cli() -> str:
    for name in ("meradb_cli.exe", "meradb_cli"):
        p = REPO_ROOT / "cpp" / "build" / name
        if p.exists():
            return str(p)
    return str(REPO_ROOT / "cpp" / "build" / "meradb_cli")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cli", default=default_cli())
    ap.add_argument("--seeds", type=int, default=200)
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--seed", type=int, help="run exactly this seed")
    ap.add_argument("--show", action="store_true", help="print the script for --seed and exit")
    ap.add_argument("--stats", action="store_true", help="Python only: report how many statements fail / how many rows the triggers wrote")
    args = ap.parse_args()
    args.cli = str(Path(args.cli).resolve())  # children run with cwd=REPO_ROOT, so a relative path would break

    if args.show:
        sys.stdout.reconfigure(encoding="utf-8")
        print(generate(args.seed or 0))
        return 0
    seeds = [args.seed] if args.seed is not None else list(range(args.start, args.start + args.seeds))

    if args.stats:
        errors = statements = log_rows = 0
        for seed in seeds:
            with tempfile.TemporaryDirectory() as data:
                script = os.path.join(data, "s.mdb")
                Path(script).write_text(generate(seed), encoding="utf-8")
                out, _ = run([sys.executable, "-m", "meradb", "run", "--local", "--data", os.path.join(data, "d"), script])
            blocks = [b for b in out.split("\n\n") if b.strip()]
            statements += len(blocks)
            errors += sum(1 for b in blocks if "Galti]" in b)
            if "Internal" in out or "Traceback" in out or "RecursionError" in out:
                print(f"seed {seed}: the Python engine hit an internal error -- the generator must avoid this construct")
            log_rows += out.count("\n| trg") + out.count("| bump") + out.count("| ping")
        print(f"{len(seeds)} scripts, {statements} results, {errors} errors ({100 * errors // max(statements, 1)}%), "
              f"~{log_rows} trigger/procedure log lines in the final dumps")
        return 0

    failures = []
    for seed in seeds:
        with tempfile.TemporaryDirectory() as tmp:
            script = os.path.join(tmp, "s.mdb")
            Path(script).write_text(generate(seed), encoding="utf-8")
            py, py_code = run([sys.executable, "-m", "meradb", "run", "--local", "--data", os.path.join(tmp, "py"), script])
            cpp, cpp_code = run([args.cli, "run", "--local", "--data", os.path.join(tmp, "cpp"), script])
            if (py, py_code) != (cpp, cpp_code):
                keep = os.path.join(tempfile.gettempdir(), f"meradb_fuzz_{seed}.mdb")
                Path(keep).write_text(generate(seed), encoding="utf-8")
                failures.append(seed)
                print(f"MISMATCH seed {seed} (exit python={py_code} cpp={cpp_code}); script saved to {keep}")
                print("".join(list(difflib.unified_diff(py.splitlines(keepends=True), cpp.splitlines(keepends=True), "python", "cpp"))[:40]))
    print(f"{len(seeds) - len(failures)}/{len(seeds)} seeds matched" + (f"; FAILED: {failures}" if failures else ""))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
