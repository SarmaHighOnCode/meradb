"""
Drives the Python shell (`python -m meradb shell`) and the C++ shell (`meradb_cli shell`) with the same
piped script and diffs everything they print -- stdout, stderr and the exit code.

Every script in shell_scripts.py runs with `--local` on a fresh data folder (server mode is added in the
next task).

Usage:
    python cpp/tests/shell_diff.py --cli path/to/meradb_cli                 # every script
    python cpp/tests/shell_diff.py --cli path/to/meradb_cli basic errors    # just these
Exit code 0 = everything matched.
"""
import argparse
import difflib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.stdout.reconfigure(encoding="utf-8")  # diffs may contain non-ASCII text; never die on a console code page

from shell_scripts import KNOWN_REPR_DIVERGENCE, SCRIPTS  # noqa: E402


def child_env() -> dict:
    env = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_") and k != "NO_COLOR"}
    env.update(PYTHONIOENCODING="utf-8", PYTHONPATH=str(REPO_ROOT), NO_COLOR="1")
    return env


def run_shell(kind: str, cli: str, args: list[str], script: str) -> tuple[str, str, int]:
    """One shell session: `script` on stdin, CRLF normalised in what comes back."""
    base = [sys.executable, "-m", "meradb"] if kind == "python" else [cli]
    done = subprocess.run(base + ["shell"] + args, input=script.encode("utf-8"), cwd=REPO_ROOT, capture_output=True,
                          env=child_env(), timeout=120)
    return (done.stdout.decode("utf-8").replace("\r\n", "\n"), done.stderr.decode("utf-8").replace("\r\n", "\n"),
            done.returncode)


def show_diff(left: str, right: str, left_name: str, right_name: str) -> None:
    diff = difflib.unified_diff(left.splitlines(keepends=True), right.splitlines(keepends=True), left_name, right_name)
    print("".join(list(diff)[:80]) or f"(only whitespace differs) {left[-30:]!r} vs {right[-30:]!r}")


def selected(names: list[str], pool: dict) -> list[str]:
    """The scripts to run: all of `pool`, or the named ones."""
    for name in names:
        if name not in pool:
            raise SystemExit(f"unknown script {name!r}; known: {', '.join(pool)}")
    return names or list(pool)


# ---------------------------------------------------------------- local mode

def check_local(cli: str, names: list[str]) -> list[str]:
    failures = []
    for name in selected(names, SCRIPTS):
        label = f"local: {name}"
        if name in KNOWN_REPR_DIVERGENCE:
            print(f"SKIP {label} (the tokenizer shows an unprintable character raw, Python escapes it)")
            continue
        script = SCRIPTS[name]
        with tempfile.TemporaryDirectory() as root:
            data = os.path.join(root, "data")  # the same path for both, so the banner's "connected:" line matches
            python = run_shell("python", cli, ["--local", "--data", data], script)
            shutil.rmtree(data, ignore_errors=True)
            cpp = run_shell("cpp", cli, ["--local", "--data", data], script)
        if python == cpp:
            print(f"PASS {label} ({len(python[0].splitlines())} lines, exit={python[2]})")
        else:
            print(f"FAIL {label} (exit python={python[2]}, cpp={cpp[2]})")
            failures.append(label)
            show_diff(python[0], cpp[0], "python stdout", "cpp stdout")
            if python[1] != cpp[1]:
                show_diff(python[1], cpp[1], "python stderr", "cpp stderr")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("names", nargs="*", help="script names from shell_scripts.py (default: all)")
    parser.add_argument("--cli", required=True)
    args = parser.parse_args()
    cli = str(Path(args.cli).resolve())  # children run with cwd=REPO_ROOT, so a relative path would break
    failures = check_local(cli, args.names)
    print("ALL MATCHED" if not failures else "FAILED: " + "; ".join(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
