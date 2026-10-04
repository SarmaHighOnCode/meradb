"""
End-to-end check of the C++ command line with REAL server processes:
start / status / run through the server / stop, the password and --force paths,
the local fallback, and the "already running" / "port busy" refusals.

    python cpp/tests/cli_lifecycle.py path/to/meradb_cli

Modelled on CommandLineTest in tests/test_server.py. Prints PASS/FAIL per
scenario and exits non-zero if any failed.
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLI = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "cpp" / "build" / "meradb_cli")
failures = []


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def meradb(*args, env=None):
    base = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_")}
    base.update(env or {})
    r = subprocess.run([CLI, *args], capture_output=True, timeout=90, env=base)
    return r.returncode, r.stdout.decode("utf-8").replace("\r\n", "\n"), r.stderr.decode("utf-8").replace("\r\n", "\n")


def check(name, condition, detail=""):
    print(("PASS " if condition else "FAIL ") + name)
    if not condition:
        failures.append(name)
        if detail:
            print("     " + detail.replace("\n", "\n     "))


def scenario_lifecycle():
    data, port = tempfile.mkdtemp(), str(free_port())
    try:
        code, out, err = meradb("start", "--data", data, "--port", port)
        check("start: exit 0", code == 0, out + err)
        check("start: says it is up", out.startswith("MeraDB server chal gaya: 127.0.0.1:" + port), out)
        check("start: pid file written", os.path.exists(os.path.join(data, "meradb.pid")))

        code, out, err = meradb("start", "--data", data, "--port", port)
        check("start twice: already running, exit 0", code == 0 and out.startswith("Server pehle se chal raha hai: 127.0.0.1:" + port), out + err)

        code, out, err = meradb("status", "--data", data)
        check("status: exit 0 and details", code == 0 and "  version:   MeraDB 1.0.0\n" in out and "  databases: main\n" in out, out + err)

        script = os.path.join(data, "t.mdb")
        with open(script, "w", encoding="utf-8") as f:
            f.write("BANAO TABLE t (x INT); DAALO MEIN t MAAN (42); DIKHAO * SE t;")
        code, out, err = meradb("run", "--port", port, script)
        check("run via server: output has 42, no fallback note", code == 0 and "42" in out and "LOCAL mode" not in err, out + err)
        code, out, err = meradb("run", script, "--data", data)  # no --port: found through the pid file
        check("run finds the server through the pid file", "LOCAL mode" not in err, out + err)

        other = tempfile.mkdtemp()  # a second folder for the busy-port start; removed below (it used to be left behind)
        try:
            code, out, err = meradb("start", "--data", other, "--port", port)
        finally:
            shutil.rmtree(other, ignore_errors=True)
        check("start on a busy port refuses", code == 1 and err.startswith("Port " + port + " par pehle se kuch aur chal raha hai."), out + err)

        code, out, err = meradb("stop", "--data", data)
        check("stop: exit 0", code == 0 and out.startswith("MeraDB server band ho gaya (pid "), out + err)
        check("stop: pid file gone", not os.path.exists(os.path.join(data, "meradb.pid")))
        code, out, err = meradb("status", "--data", data)
        check("status when stopped: exit 3", code == 3 and out.startswith("MeraDB server nahi chal raha"), out + err)
        code, out, err = meradb("stop", "--data", data)
        check("stop when stopped: exit 0", code == 0 and out == "Server nahi chal raha.\n", out + err)
    finally:
        meradb("stop", "--data", data, "--force")
        shutil.rmtree(data, ignore_errors=True)


def scenario_password_and_force():
    data, port = tempfile.mkdtemp(), str(free_port())
    try:
        code, out, err = meradb("start", "--data", data, "--port", port, "--password", "sekrit")
        check("start with a password", code == 0, out + err)
        script = os.path.join(data, "t.mdb")
        with open(script, "w", encoding="utf-8") as f:
            f.write("DIKHAO TABLES;")
        code, out, err = meradb("run", "--port", port, script)
        check("run without the password is refused", code == 1 and "assword" in err, out + err)
        code, out, err = meradb("run", "--port", port, script, env={"MERADB_PASSWORD": "sekrit"})
        check("run with MERADB_PASSWORD works", code == 0, out + err)
        code, out, err = meradb("status", "--data", data)
        check("status without the password: still running, no details", code == 0 and "(details nahi mile:" in out, out + err)
        code, out, err = meradb("stop", "--data", data)
        check("stop without the password fails and points at --force",
              code == 1 and "Zabardasti band karne ke liye:  meradb stop --force" in err, out + err)
        code, out, err = meradb("stop", "--data", data, "--force")
        check("stop --force kills it", code == 0 and "(--force)" in err and out.startswith("MeraDB server band ho gaya"), out + err)
        code, out, err = meradb("status", "--data", data)
        check("after --force: not running", code == 3, out + err)
    finally:
        meradb("stop", "--data", data, "--force")
        shutil.rmtree(data, ignore_errors=True)


def scenario_local_fallback():
    data = tempfile.mkdtemp()
    try:
        env = {"MERADB_PORT": str(free_port())}
        demo = str(ROOT / "examples" / "demo.mdb")
        code, out, err = meradb("run", "--data", data, demo, env=env)
        check("fallback: says LOCAL mode", "LOCAL mode" in err, err)
        check("fallback: the script ran", "Transaction WAPAS" in out, out[-300:])
        fresh = tempfile.mkdtemp()  # the demo creates tables, so it needs an empty folder each time
        try:
            code, out, err = meradb("run", "--data", fresh, demo, "--local", env=env)
            check("--local: no note", "LOCAL mode" not in err and "Transaction WAPAS" in out, err)
        finally:
            shutil.rmtree(fresh, ignore_errors=True)
        code, out, err = meradb("run", "--data", data, demo, "--port", env["MERADB_PORT"])
        check("explicit --port: no fallback", code == 1 and "par MeraDB server nahi mila" in err, out + err)
    finally:
        shutil.rmtree(data, ignore_errors=True)


def scenario_data_lock_message():
    """A folder served by a running server must not be opened directly."""
    data, port = tempfile.mkdtemp(), str(free_port())
    try:
        meradb("start", "--data", data, "--port", port)
        script = os.path.join(data, "t.mdb")
        with open(script, "w", encoding="utf-8") as f:
            f.write("DIKHAO TABLES;")
        code, out, err = meradb("run", script, "--local", "--data", data)
        check("--local on a served folder is refused", code == 1 and "Is data folder par MeraDB server chal raha hai" in err, out + err)
    finally:
        meradb("stop", "--data", data, "--force")
        shutil.rmtree(data, ignore_errors=True)


for scenario in (scenario_lifecycle, scenario_password_and_force, scenario_local_fallback, scenario_data_lock_message):
    scenario()
print("FAILED: " + ", ".join(failures) if failures else "ALL PASSED")
sys.exit(1 if failures else 0)
