"""
Smoke test of the real binary's `workbench` command without a terminal: it must say so and exit 1 before opening
anything. (The screen itself is checked by the C++ tests and by hand; see the manual checklist.)

    python cpp/tests/wb_cli_smoke.py path/to/meradb_cli

With the workbench built in the message is "Workbench ke liye terminal chahiye ..."; built with
-DMERADB_WORKBENCH=OFF it is the "abhi C++ version mein nahi hai" note. Either way: exit 1.
"""
import os
import shutil
import subprocess
import sys
import tempfile

CLI = sys.argv[1]
failures = []


def run(*args):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_")}
    work = tempfile.mkdtemp()
    try:
        r = subprocess.run([CLI, *args], stdin=subprocess.DEVNULL, capture_output=True, timeout=60, env=env, cwd=work)
        listing = os.listdir(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    text = lambda b: b.decode("utf-8").replace("\r\n", "\n")
    return r.returncode, text(r.stdout), text(r.stderr), listing


def check(name, condition, detail=""):
    print(("PASS " if condition else "FAIL ") + name)
    if not condition:
        failures.append(name)
        if detail:
            print("     " + detail.replace("\n", "\n     "))


data = tempfile.mkdtemp()
try:
    for command in (["workbench"], ["tui"], ["--tui"]):
        code, out, err, listing = run(*command, "--local", "--data", os.path.join(data, "never"))
        label = " ".join(command)
        check(label + ": exit 1", code == 1, f"{code} {out!r} {err!r}")
        check(label + ": says why on stderr", "terminal chahiye" in err or "abhi C++ version mein nahi hai" in err, err)
        check(label + ": nothing on stdout", out == "", out)
        check(label + ": the data folder was not created", not os.path.exists(os.path.join(data, "never")))
        check(label + ": nothing written to the current folder", listing == [], str(listing))

    code, out, err, _ = run("workbench", "--bogus")
    check("workbench --bogus: exit 2 with usage", code == 2 and "usage: meradb" in err, f"{code} {err!r}")

    code, out, err, _ = run("workbench", "-h")
    check("workbench -h: exit 0 and the usage", code == 0 and out.startswith("usage: meradb workbench"), f"{code} {out!r}")
finally:
    shutil.rmtree(data, ignore_errors=True)

if failures:
    print(f"\n{len(failures)} check(s) failed")
    sys.exit(1)
print("\nall checks passed")
