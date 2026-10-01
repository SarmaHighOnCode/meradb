"""
Runs the same .mdb script through the Python engine and the C++ CLI and diffs
their raw output. Exits non-zero (printing a unified diff) on any
mismatch, so it can be wired into a CI-style check.

Usage:
    python cpp/tests/cross_engine_diff.py cpp/tests/demo_phase1.mdb
    python cpp/tests/cross_engine_diff.py cpp/tests/rdbms_lab_coverage_phase1.mdb
    python cpp/tests/cross_engine_diff.py --cli path/to/meradb_cli SCRIPT.mdb
"""
import argparse
import difflib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


def default_cli() -> Path:
    build = REPO_ROOT / "cpp" / "build"
    for folder in (build, build / "Release"):
        for name in ("meradb_cli.exe", "meradb_cli"):
            if (folder / name).exists():
                return folder / name
    return REPO_ROOT / "cpp" / "build" / "meradb_cli"


def run(cmd: list[str]) -> tuple[str, int]:
    env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONPATH=str(REPO_ROOT))
    result = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, env=env)
    return result.stdout.decode("utf-8").replace("\r\n", "\n"), result.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("script")
    parser.add_argument("--cli", type=Path, default=default_cli())
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as py_dir, tempfile.TemporaryDirectory() as cpp_dir:
        py_out, py_code = run([sys.executable, "-m", "meradb", "run", "--local", "--data", py_dir, args.script])
        cpp_out, cpp_code = run([str(args.cli), "run", args.script, "--local", "--data", cpp_dir])

    # Compare raw text (CRLF already normalised) so trailing-newline differences are not hidden.
    if py_out == cpp_out and py_code == cpp_code:
        print(f"MATCH: {args.script} ({len(py_out.splitlines())} lines identical, exit={py_code})")
        return 0

    print(f"MISMATCH: {args.script} (exit python={py_code}, cpp={cpp_code})")
    diff = difflib.unified_diff(py_out.splitlines(keepends=True), cpp_out.splitlines(keepends=True),
                                fromfile="python", tofile="cpp")
    print("".join(diff) or f"(only trailing whitespace differs) python={py_out[-20:]!r} cpp={cpp_out[-20:]!r}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
