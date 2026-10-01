"""
Runs the same .mdb script through the Python engine and the C++ CLI and diffs
their raw output. Exits non-zero (printing a unified diff) on any
mismatch, so it can be wired into a CI-style check.

Modes for the C++ side:
  (default)      `meradb_cli run --local`            the engine inside the CLI process
  --via-server   `meradb_cli server` + `meradb_cli run --port P`
                 the same script, but every statement crosses the wire protocol

The Python side is always `python -m meradb run --local` (the oracle).

Usage:
    python cpp/tests/cross_engine_diff.py examples/demo.mdb
    python cpp/tests/cross_engine_diff.py --via-server examples/demo.mdb
    python cpp/tests/cross_engine_diff.py --cli path/to/meradb_cli SCRIPT.mdb
"""
import argparse
import difflib
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.stdout.reconfigure(encoding="utf-8")  # diffs may contain non-ASCII text; never die on a console code page


def default_cli() -> Path:
    build = REPO_ROOT / "cpp" / "build"
    for folder in (build, build / "Release"):
        for name in ("meradb_cli.exe", "meradb_cli"):
            if (folder / name).exists():
                return folder / name
    return REPO_ROOT / "cpp" / "build" / "meradb_cli"


def child_env() -> dict:
    env = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_")}  # no user settings leak in
    env.update(PYTHONIOENCODING="utf-8", PYTHONPATH=str(REPO_ROOT))
    return env


def run(cmd: list[str]) -> tuple[str, str, int]:
    result = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, env=child_env())
    return (result.stdout.decode("utf-8").replace("\r\n", "\n"),
            result.stderr.decode("utf-8").replace("\r\n", "\n"), result.returncode)


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for_port(port: int, seconds: float = 15.0) -> bool:
    deadline = time.time() + seconds
    while time.time() < deadline:
        with socket.socket() as s:
            s.settimeout(0.3)
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return True
        time.sleep(0.1)
    return False


class CppServer:
    """`meradb_cli server` in the foreground of a child process, stopped again on exit."""

    def __init__(self, cli: Path, data: str):
        self.port = free_port()
        self.data = data
        self.cli = cli
        self.proc = subprocess.Popen([str(cli), "server", "--data", data, "--port", str(self.port)], cwd=REPO_ROOT,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=child_env())

    def __enter__(self):
        if not wait_for_port(self.port):
            self.proc.kill()
            raise SystemExit("C++ server did not start")
        return self

    def __exit__(self, *exc):
        subprocess.run([str(self.cli), "stop", "--data", self.data], cwd=REPO_ROOT, capture_output=True, env=child_env(),
                       timeout=30)
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("script")
    parser.add_argument("--cli", type=Path, default=default_cli())
    parser.add_argument("--via-server", action="store_true", help="run the C++ side through a C++ server")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as py_dir, tempfile.TemporaryDirectory() as cpp_dir:
        py_out, _, py_code = run([sys.executable, "-m", "meradb", "run", "--local", "--data", py_dir, args.script])
        if args.via_server:
            with CppServer(args.cli, cpp_dir) as server:
                cpp_out, cpp_err, cpp_code = run([str(args.cli), "run", "--port", str(server.port), args.script])
            if "LOCAL mode" in cpp_err:
                print("MISMATCH: the C++ client fell back to local mode instead of using the server")
                return 1
        else:
            cpp_out, _, cpp_code = run([str(args.cli), "run", args.script, "--local", "--data", cpp_dir])

    label = "via server" if args.via_server else "local"
    # Compare raw text (CRLF already normalised) so trailing-newline differences are not hidden.
    if py_out == cpp_out and py_code == cpp_code:
        print(f"MATCH ({label}): {args.script} ({len(py_out.splitlines())} lines identical, exit={py_code})")
        return 0

    print(f"MISMATCH ({label}): {args.script} (exit python={py_code}, cpp={cpp_code})")
    diff = difflib.unified_diff(py_out.splitlines(keepends=True), cpp_out.splitlines(keepends=True),
                                fromfile="python", tofile="cpp")
    print("".join(diff) or f"(only trailing whitespace differs) python={py_out[-20:]!r} cpp={cpp_out[-20:]!r}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
