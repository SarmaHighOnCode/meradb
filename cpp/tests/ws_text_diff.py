"""
Text that is stored or interpreted by the shells, with every character Python's str.isspace() accepts (and
U+FEFF, which the tokenizer also skips) in the places where Python strips or splits it: the end and the start of
CHECK (SHART) text, view definitions, trigger and procedure bodies, a statement's last character, and dot-command
arguments (.help<c>topic, <c>.tables, .schema<c>t<c>).

For each character the same script is typed into `python -m meradb shell --local` and `meradb_cli shell --local`:
what they print and the catalog.json they write must be identical, and each engine must read (and print the same
for) the folder the other one wrote.

    python cpp/tests/ws_text_diff.py path/to/meradb_cli
"""
import difflib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
sys.stdout.reconfigure(encoding="utf-8", errors="replace")
CLI = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else str(REPO_ROOT / "cpp" / "build" / "meradb_cli")

SPACES = [chr(i) for i in range(0x110000) if chr(i).isspace()] + ["﻿"]


def env() -> dict:
    e = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_") and k != "NO_COLOR"}
    e.update(PYTHONIOENCODING="utf-8", PYTHONPATH=str(REPO_ROOT), NO_COLOR="1")
    return e


def shell(kind: str, data: str, script: str):
    base = [sys.executable, "-m", "meradb"] if kind == "python" else [CLI]
    done = subprocess.run(base + ["shell", "--local", "--data", data], input=script.encode("utf-8"), cwd=REPO_ROOT,
                          capture_output=True, env=env(), timeout=120)
    return (done.stdout.decode("utf-8").replace("\r\n", "\n"), done.stderr.decode("utf-8").replace("\r\n", "\n"),
            done.returncode)


def writer_script(c: str) -> str:
    return (
        f"BANAO TABLE t (a INT SHART (a > 0{c}), b INT SHART ({c}b < 9{c}{c}));\n"
        f"BATAO t;\n"
        f"BANAO VIEW v KAHO DIKHAO{c}a{c}SE t{c};\n"
        f"BANAO TRIGGER tr BAAD DAALO PAR t SHURU{c}DAALO MEIN t MAAN (1, 2);{c}KHATAM;\n"
        f"BANAO PROCEDURE p (x INT) SHURU{c}DIKHAO * SE t;{c}KHATAM;\n"
        f"DIKHAO TABLES;{c}\n"
        f".help{c}join\n"
        f".help{c}foreign{c}key{c}\n"
        f"{c}.tables\n"
        f".schema{c}t{c}\n"
        f".tables{c}extra\n"
        f".exit\n"
    )


READER_SCRIPT = "BATAO t;\n.schema t\n.tables\nDIKHAO * SE v;\n.exit\n"


def catalog_of(data: str) -> str:
    found = sorted(Path(data).rglob("catalog.json"))
    return "\n".join(p.relative_to(data).as_posix() + ":" + p.read_bytes().decode("utf-8") for p in found)


def show(a: str, b: str, an: str, bn: str) -> None:
    diff = difflib.unified_diff(a.splitlines(keepends=True), b.splitlines(keepends=True), an, bn)
    print("".join(list(diff)[:40]) or f"(differs) {a[-40:]!r} vs {b[-40:]!r}")


def main() -> int:
    failures = []
    for c in SPACES:
        label = f"U+{ord(c):04X}"
        with tempfile.TemporaryDirectory() as root:
            data = os.path.join(root, "data")  # the same path every time: the banner prints it
            kept = {k: os.path.join(root, "kept_" + k) for k in ("python", "cpp")}
            wrote, catalogs = {}, {}
            for k in kept:
                wrote[k] = shell(k, data, writer_script(c))
                catalogs[k] = catalog_of(data)
                os.rename(data, kept[k])
            ok = wrote["python"] == wrote["cpp"] and catalogs["python"] == catalogs["cpp"]
            if not ok:
                print(f"FAIL {label}: writer output or catalog.json differs")
                show(wrote["python"][0], wrote["cpp"][0], "python stdout", "cpp stdout")
                show(wrote["python"][1], wrote["cpp"][1], "python stderr", "cpp stderr")
                show(catalogs["python"], catalogs["cpp"], "python catalog", "cpp catalog")
            reads = {}
            for writer in kept:
                for reader in kept:
                    shutil.copytree(kept[writer], data)  # a copy, so reading never changes the writer's folder
                    reads[(writer, reader)] = shell(reader, data, READER_SCRIPT)
                    shutil.rmtree(data)
            baseline = reads[("python", "python")]
            for pair, got in reads.items():
                if got != baseline:
                    ok = False
                    print(f"FAIL {label}: {pair[1]} reading what {pair[0]} wrote differs from python/python")
                    show(baseline[0], got[0], "python reads python", f"{pair[1]} reads {pair[0]}")
                    show(baseline[1], got[1], "python stderr", "stderr")
        print(("PASS " if ok else "FAIL ") + label)
        if not ok:
            failures.append(label)
    print(f"{len(SPACES) - len(failures)}/{len(SPACES)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
