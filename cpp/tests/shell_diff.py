"""
Drives the Python shell (`python -m meradb shell`) and the C++ shell (`meradb_cli shell`) with the same
piped script and diffs everything they print -- stdout, stderr and the exit code.

Modes:
  local     both shells open a fresh data folder (--local); every script in shell_scripts.py
  server    the same scripts through every client/server pair: a Python or a C++ client against a Python
            or a C++ server (the output of the three other pairs must equal the Python-to-Python baseline)

Usage:
    python cpp/tests/shell_diff.py --cli path/to/meradb_cli                 # both modes, every script
    python cpp/tests/shell_diff.py --cli path/to/meradb_cli --mode local basic errors
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

from interop_check import Server  # noqa: E402  (a foreground server of either kind on a free port)
from shell_scripts import KNOWN_REPR_DIVERGENCE, SCRIPTS, SERVER_SCRIPTS  # noqa: E402


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


# ---------------------------------------------------------------- server mode

def check_servers(cli: str, names: list[str], strict: bool) -> list[str]:
    failures = []
    pool = {n: SCRIPTS[n] for n in SERVER_SCRIPTS}
    if names and not strict:  # both modes were asked for: take only the named scripts that also run through servers
        names = [n for n in names if n in pool]
        if not names:
            return failures
    for name in selected(names, pool):
        script = SCRIPTS[name]
        results = {}
        for server_kind in ("python", "cpp"):
            for client_kind in ("python", "cpp"):
                with tempfile.TemporaryDirectory() as data, Server(server_kind, cli, data) as server:
                    out, err, code = run_shell(client_kind, cli, ["--port", str(server.port)], script)
                    # the port differs per server; nothing else about the banner may
                    results[(client_kind, server_kind)] = (out.replace(f"127.0.0.1:{server.port}", "127.0.0.1:PORT"),
                                                           err.replace(str(server.port), "PORT"), code)
        baseline = results[("python", "python")]
        for pair, got in results.items():
            if pair == ("python", "python"):
                continue
            label = f"server: {name}: {pair[0]} client -> {pair[1]} server"
            if got == baseline:
                print(f"PASS {label} ({len(got[0].splitlines())} lines)")
            else:
                print(f"FAIL {label}")
                failures.append(label)
                show_diff(baseline[0], got[0], "python->python", f"{pair[0]}->{pair[1]}")
                if baseline[1] != got[1]:
                    show_diff(baseline[1], got[1], "python->python stderr", "stderr")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("names", nargs="*", help="script names from shell_scripts.py (default: all)")
    parser.add_argument("--cli", required=True)
    parser.add_argument("--mode", choices=["local", "server", "all"], default="all")
    args = parser.parse_args()
    cli = str(Path(args.cli).resolve())  # children run with cwd=REPO_ROOT, so a relative path would break
    failures = []
    if args.mode in ("local", "all"):
        failures += check_local(cli, args.names)
    if args.mode in ("server", "all"):
        failures += check_servers(cli, args.names, strict=args.mode == "server")
    print("ALL MATCHED" if not failures else "FAILED: " + "; ".join(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
