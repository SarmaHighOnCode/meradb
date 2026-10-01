"""
Argument handling of the C++ command line against `python -m meradb`: every case
is run through both and must give the same exit code, stdout and stderr
(`--`, files after an option, trailing separators and "" in --data, ...), plus
the stale-pid-file and failed-start behaviour that has no Python twin to diff.

    python cpp/tests/cli_args_diff.py path/to/meradb_cli

Runs in temp folders with MERADB_* removed; nothing touches the real data folder.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ROOT = Path(__file__).resolve().parents[2]
CLI = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "cpp" / "build" / "meradb_cli")
failures = []


def check(name, condition, detail=""):
    print(("PASS " if condition else "FAIL ") + name)
    if not condition:
        failures.append(name)
        if detail:
            print("     " + detail.replace("\n", "\n     "))


def run(cmd, env, cwd, raw=False):
    r = subprocess.run(cmd, capture_output=True, env=env, cwd=cwd, timeout=90)
    if raw:
        return r.returncode, r.stdout, r.stderr
    text = lambda b: b.decode("utf-8", "replace").replace("\r\n", "\n")
    return r.returncode, text(r.stdout), text(r.stderr)


def main():
    root = tempfile.mkdtemp(prefix="mdb_args_")
    sep = os.sep
    data = os.path.join(root, "data")
    os.makedirs(data)
    os.makedirs(os.path.join(root, "sub"))
    a, b = os.path.join(root, "a.mdb"), os.path.join(root, "b.mdb")
    for path in (a, b):
        Path(path).write_text("DIKHAO TABLES;", encoding="utf-8")
    env = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_")}
    env["MERADB_DATA"] = os.path.join(root, "unused")
    py_env = dict(env, PYTHONPATH=str(ROOT))
    try:
        cases = [
            ["run", "--local", "--data", data, "--", a],
            ["run", "--local", "--data", data, a, "--", b],
            ["run", "--local", "--data", data, "--", a, b],
            ["run", "--local", "--data", data, "--", "--weird.mdb"],
            ["run", "--local", "--data", data, "--", "-x"],
            ["run", "--", "--local", a],
            ["run", "--", a, "--", b],
            ["run", a, "--", "--"],
            ["run", a, "--data", data, b, "--local"],
            ["run", a, "--local", b, "--data", data],
            ["run", a, "--local", "--", b],
            ["run", a, "--local", "--"],
            ["run", "--local", a, b, "--data", data],
            ["run", "--local", "--data", data, a, "--"],
            ["run", "--local", "--data", data, "--"],
            ["run", "--"],
            ["status", "--data", data, "--"],
            ["status", "--", "--data", data],
            ["status", "--data", data, "extra"],
            ["status", "--data", data, "--", "extra"],
            ["stop", "--data", data, "--", "--force"],
            ["status", "--data", os.path.join(root, "sub") + sep],
            ["status", "--data", os.path.join(root, "sub") + "/"],
            ["status", "--data", os.path.join(root, "sub") + sep + sep],
            ["status", "--data", ""],
            ["status", "--data", "."],
            ["status", "--data", "sub"],
            ["status", "--data", os.path.join(root, "sub", "..", "sub")],
            ["run", "--local", "--data", "", a],
            ["run", "--local", "--data", data + sep, a],
        ]
        for args in cases:
            mine = run([CLI, *args], env, root)
            theirs = run([sys.executable, "-m", "meradb", *args], py_env, root)
            label = " ".join(x.replace(root, "<tmp>") for x in args)
            check("same as Python: " + label, mine == theirs, "C++:    %r\nPython: %r" % (mine, theirs))

        # a pid file without a pid (a crash leftover): stop says so and removes it, like Python
        for name, runner in (("C++", lambda: run([CLI, "stop", "--data", data], env, root)),
                             ("Python", lambda: run([sys.executable, "-m", "meradb", "stop", "--data", data], py_env, root))):
            pid_file = os.path.join(data, "meradb.pid")
            Path(pid_file).write_text(json.dumps({"host": "127.0.0.1", "port": 1}), encoding="utf-8")
            code, out, err = runner()
            check("stop with a pid-less stale pid file (%s)" % name,
                  code == 0 and out == "Server nahi chal raha.\n" and not os.path.exists(pid_file), out + err)

        # a failed `start` shows the log tail with plain line endings (no doubled carriage returns)
        code, out, err = run([CLI, "start", "--data", data, "--host", "999.1.1.1", "--port", "47999"], env, root, raw=True)
        check("failed start: exit 1 and the log tail follows", code == 1 and b"Server start nahi hua." in err, repr(err))
        check("failed start: no doubled carriage return", b"\r\r" not in err, repr(err))
        check("failed start: no process left behind", not os.path.exists(os.path.join(data, "meradb.pid")))
    finally:
        shutil.rmtree(root, ignore_errors=True)
    if failures:
        print("FAILED: " + ", ".join(failures))
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
