"""
Non-ASCII command-line arguments and environment values reach the C++ command
line intact (on Windows the narrow argv is in the ANSI code page), and the
results match `python -m meradb`.

    python cpp/tests/cli_unicode.py path/to/meradb_cli

Everything happens in temp folders with MERADB_* removed from the environment;
a started server is always stopped (and killed if need be) before exiting.
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ROOT = Path(__file__).resolve().parents[2]
CLI = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "cpp" / "build" / "meradb_cli")
failures = []


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def base_env(extra=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_")}
    env["PYTHONIOENCODING"] = "utf-8"
    env.update(extra or {})
    return env


def run(cmd, env=None, cwd=None):
    r = subprocess.run(cmd, capture_output=True, timeout=90, env=base_env(env), cwd=cwd)
    return r.returncode, r.stdout.decode("utf-8").replace("\r\n", "\n"), r.stderr.decode("utf-8").replace("\r\n", "\n")


def cpp(*args, env=None):
    return run([CLI, *args], env)


def py(*args, env=None):
    return run([sys.executable, "-m", "meradb", *args], env, cwd=str(ROOT))


def check(name, condition, detail=""):
    print(("PASS " if condition else "FAIL ") + name)
    if not condition:
        failures.append(name)
        if detail:
            print("     " + detail.replace("\n", "\n     "))


def main():
    root = tempfile.mkdtemp(prefix="mdb_uni_")
    data = os.path.join(root, "déta 日本")  # e-acute and CJK, with a space
    pydata = os.path.join(root, "py_déta 日本")
    script = os.path.join(root, "é.mdb")
    port = str(free_port())
    try:
        with open(script, "w", encoding="utf-8") as f:
            f.write("BANAO TABLE t (naam TEXT); DAALO MEIN t MAAN ('café'); DIKHAO * SE t;")

        c = cpp("run", "--local", "--data", data, script)
        p = py("run", "--local", "--data", pydata, script)
        check("run --local with a non-ASCII data folder and script name works", c[0] == 0 and "café" in c[1], str(c))
        check("run --local output matches Python", c == p, str(c) + "\n" + str(p))
        check("the data folder was created under its real name", os.path.isdir(data) and os.path.isdir(os.path.join(data, "main")))
        check("Python reads the folder C++ created",
              py("run", "--local", "--data", data, script)[1].count("café") >= 1)

        c = cpp("status", "--data", data)
        p = py("status", "--data", data)
        check("status --data prints the folder like Python", c == p and data in c[1], str(c) + "\n" + str(p))

        c = cpp("status", env={"MERADB_DATA": data})
        p = py("status", env={"MERADB_DATA": data})
        check("MERADB_DATA with non-ASCII text is read as UTF-8", c == p and data in c[1], str(c) + "\n" + str(p))

        # a real server in that folder, with a non-ASCII password handed over through the environment
        password = "pässwörd"
        code, out, err = cpp("start", "--data", data, "--port", port, "--password", password)
        check("start with a non-ASCII data folder and password", code == 0 and out.startswith("MeraDB server chal gaya: 127.0.0.1:" + port), out + err)
        check("pid file and log are in the non-ASCII folder",
              os.path.exists(os.path.join(data, "meradb.pid")) and os.path.exists(os.path.join(data, "server.log")))
        code, out, err = cpp("status", "--data", data, env={"MERADB_PASSWORD": password})
        check("status with the matching non-ASCII password", code == 0 and "  databases: main\n" in out, out + err)
        wrong = {"MERADB_PASSWORD": "pässwört"}
        c, p = cpp("status", "--data", data, env=wrong), py("status", "--data", data, env=wrong)
        check("status with a wrong non-ASCII password answers like Python", c == p and "databases" not in c[1], str(c) + "\n" + str(p))
        select = os.path.join(root, "select.mdb")  # the table already exists from the local runs above
        with open(select, "w", encoding="utf-8") as f:
            f.write("DIKHAO * SE t;")
        code, out, err = cpp("run", "--port", port, select, env={"MERADB_PASSWORD": password})
        check("run through the server with the non-ASCII password", code == 0 and "café" in out and "LOCAL mode" not in err, out + err)
        code, out, err = cpp("stop", "--data", data, env={"MERADB_PASSWORD": password})
        check("stop", code == 0 and out.startswith("MeraDB server band ho gaya (pid "), out + err)
        check("pid file gone", not os.path.exists(os.path.join(data, "meradb.pid")))
    finally:
        cpp("stop", "--data", data, "--force", env={"MERADB_PASSWORD": "pässwörd"})
        shutil.rmtree(root, ignore_errors=True)
    if failures:
        print("FAILED: " + ", ".join(failures))
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
