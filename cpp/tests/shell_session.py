"""
Shell behaviours that a single piped script cannot show, checked against the Python shell where Python has
the behaviour and on their own where it does not:

  1. logins        -U / MERADB_PASSWORD against a Python and a C++ server (users made by the Python
                   UserStore): same transcript from both clients, grants enforced, a wrong password is
                   refused before any banner (stderr + exit code equal to Python's)
  2. dropped       the server goes away in the middle of a session: the shell prints the connection error
                   on stdout, keeps going, and still says goodbye (Python and C++ clients, both servers)
  3. fallback      no server is listening: the same note on stderr, then a local shell
  4. hardening     C++ only: a very long line, deeply nested input, invalid UTF-8 and a NUL byte on stdin
                   never crash the shell or stop it from answering the next statement

Usage:
    python cpp/tests/shell_session.py --cli path/to/meradb_cli
Exit code 0 = everything held.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(REPO_ROOT))
sys.stdout.reconfigure(encoding="utf-8")

from interop_check import Server, free_port  # noqa: E402
from shell_diff import child_env, show_diff  # noqa: E402

PROMPT = "meradb:main> "


def command(kind: str, cli: str, *args: str) -> list[str]:
    return ([sys.executable, "-m", "meradb"] if kind == "python" else [cli]) + list(args)


def normalise(text: str, port: int) -> str:
    return text.replace(f"127.0.0.1:{port}", "127.0.0.1:PORT").replace(str(port), "PORT")


# ---------------------------------------------------------------- an interactive session (stdin kept open)

class Session:
    """A shell whose stdin we keep open, so a test can act (stop a server) between two lines."""

    def __init__(self, cmd: list[str], env: dict):
        self.proc = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     cwd=REPO_ROOT, env=env)
        self.out, self.err = bytearray(), bytearray()
        self.threads = [threading.Thread(target=self._pump, args=(self.proc.stdout, self.out), daemon=True),
                        threading.Thread(target=self._pump, args=(self.proc.stderr, self.err), daemon=True)]
        for thread in self.threads:
            thread.start()

    @staticmethod
    def _pump(stream, sink):
        while True:
            chunk = os.read(stream.fileno(), 4096)
            if not chunk:
                return
            sink.extend(chunk)

    def text(self) -> str:
        return bytes(self.out).decode("utf-8", "replace").replace("\r\n", "\n")

    def wait_for(self, needle: str, count: int = 1, seconds: float = 30.0) -> None:
        deadline = time.time() + seconds
        while time.time() < deadline:
            if self.text().count(needle) >= count:
                return
            if self.proc.poll() is not None and self.text().count(needle) < count:
                break
            time.sleep(0.05)
        raise SystemExit(f"timed out waiting for {needle!r} x{count}; got:\n{self.text()}")

    def send(self, text: str) -> None:
        self.proc.stdin.write(text.encode("utf-8"))
        self.proc.stdin.flush()

    def finish(self) -> tuple[str, str, int]:
        try:
            self.proc.stdin.close()
        except OSError:
            pass
        try:
            code = self.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            raise SystemExit("the shell did not end after its input closed")
        for thread in self.threads:
            thread.join(timeout=5)
        return self.text(), bytes(self.err).decode("utf-8", "replace").replace("\r\n", "\n"), code


# ---------------------------------------------------------------- 1. logins

def check_logins(cli: str) -> list[str]:
    from meradb.engine import Engine
    from meradb.users import UserStore

    failures = []
    for server_kind in ("python", "cpp"):
        results = {}
        for client_kind in ("python", "cpp"):
            with tempfile.TemporaryDirectory() as data:
                engine = Engine(data)
                engine.execute("BANAO TABLE staff (id INT, name TEXT); DAALO MEIN staff MAAN (1, 'a'); "
                               "BANAO TABLE secret (x INT)")
                engine.close()
                users = UserStore(data)
                users.create("asha", "pw-é")
                users.grant("asha", "main", "staff", ["DIKHAO"])
                with Server(server_kind, cli, data) as server:
                    script = "DIKHAO * SE staff;\nDIKHAO * SE secret;\nBANAO TABLE z (x INT);\n"
                    good = run_user(client_kind, cli, server.port, "asha", {"MERADB_PASSWORD": "pw-é"}, script)
                    bad = run_user(client_kind, cli, server.port, "asha", {"MERADB_PASSWORD": "wrong"}, script)
                    ghost = run_user(client_kind, cli, server.port, "ghost", {"MERADB_PASSWORD": "x"}, script)
                    results[client_kind] = (good, bad, ghost)
        for index, what in enumerate(("login as asha", "wrong password", "unknown user")):
            label = f"logins: {what} on a {server_kind} server"
            python, cpp = results["python"][index], results["cpp"][index]
            if python == cpp:
                print(f"PASS {label} (exit={python[2]})")
            else:
                print(f"FAIL {label} (exit python={python[2]}, cpp={cpp[2]})")
                failures.append(label)
                show_diff(python[0], cpp[0], "python stdout", "cpp stdout")
                show_diff(python[1], cpp[1], "python stderr", "cpp stderr")
    return failures


def run_user(kind, cli, port, user, extra_env, script):
    env = child_env()
    env.update(extra_env)
    done = subprocess.run(command(kind, cli, "shell", "--port", str(port), "-U", user), input=script.encode("utf-8"),
                          cwd=REPO_ROOT, capture_output=True, env=env, timeout=120)
    return (normalise(done.stdout.decode("utf-8").replace("\r\n", "\n"), port),
            normalise(done.stderr.decode("utf-8").replace("\r\n", "\n"), port), done.returncode)


# ---------------------------------------------------------------- 2. the server goes away mid-session

def dropped_session(client_kind: str, server_kind: str, cli: str) -> tuple[str, str, int]:
    with tempfile.TemporaryDirectory() as data:
        server = Server(server_kind, cli, data)
        try:
            session = Session(command(client_kind, cli, "shell", "--port", str(server.port)), child_env())
            session.wait_for(PROMPT, 1)
            session.send("BANAO TABLE t (x INT);\n")
            session.wait_for(PROMPT, 2)
            server.stop()  # the server shuts down while the shell is connected and idle
            session.send("DIKHAO TABLES;\n")
            session.wait_for("[Connection Galti]", 1)
            session.wait_for(PROMPT, 3)
            session.send("DIKHAO TABLES;\n.exit\n")  # still alive: the same error again, then goodbye
            out, err, code = session.finish()
        finally:
            server.stop()
    # The operating system's own wording after "toot gaya:" differs by platform and by language runtime
    # ("[WinError 10054] ..." from Python, a bare message from C++): everything before it must match.
    out = re.sub(r"(toot gaya: ).*", r"\1<os error>", out)
    return normalise(out, server.port), normalise(err, server.port), code


def check_dropped(cli: str) -> list[str]:
    failures = []
    for server_kind in ("python", "cpp"):
        results = {client: dropped_session(client, server_kind, cli) for client in ("python", "cpp")}
        label = f"dropped: the {server_kind} server stops mid-session"
        python, cpp = results["python"], results["cpp"]
        errors = cpp[0].count("[Connection Galti]")
        if python == cpp and errors >= 2 and cpp[0].endswith("Phir milenge!\n") and cpp[2] == 0:
            print(f"PASS {label} ({errors} errors printed, exit={cpp[2]})")
        else:
            print(f"FAIL {label} (exit python={python[2]}, cpp={cpp[2]}, errors printed by cpp={errors})")
            failures.append(label)
            show_diff(python[0], cpp[0], "python stdout", "cpp stdout")
            show_diff(python[1], cpp[1], "python stderr", "cpp stderr")
    return failures


# ---------------------------------------------------------------- 3. no server: fall back to local

def check_fallback(cli: str) -> list[str]:
    script = "BANAO TABLE f (x INT);\n.tables\n"
    with tempfile.TemporaryDirectory() as root:
        data = os.path.join(root, "data")
        port = free_port()  # nothing listens here
        results = []
        for kind in ("python", "cpp"):
            env = child_env()
            env["MERADB_PORT"] = str(port)
            done = subprocess.run(command(kind, cli, "shell", "--data", data), input=script.encode("utf-8"), cwd=REPO_ROOT,
                                  capture_output=True, env=env, timeout=120)
            results.append((done.stdout.decode("utf-8").replace("\r\n", "\n"), done.stderr.decode("utf-8").replace("\r\n", "\n"),
                            done.returncode))
            shutil.rmtree(data, ignore_errors=True)
    python, cpp = results
    label = "fallback: no server, local shell with a note on stderr"
    if python == cpp and "LOCAL mode" in cpp[1] and "Phir milenge!" in cpp[0]:
        print(f"PASS {label}")
        return []
    print(f"FAIL {label}")
    show_diff(python[0], cpp[0], "python stdout", "cpp stdout")
    show_diff(python[1], cpp[1], "python stderr", "cpp stderr")
    return [label]


# ---------------------------------------------------------------- 4. hardening (C++ only)

def hardening_cases() -> list[tuple[str, bytes, str]]:
    """(name, stdin, something that must be in stdout). Every case ends with a statement that must still work."""
    head = b"BANAO TABLE alive (x INT);\n"
    tail = b"DIKHAO TABLES;\n"  # answers "1 table(s) in 'main'" only if the shell and the database are still sound
    alive = "1 table(s) in 'main'"
    return [
        ("a very long line", head + b"DIKHAO '" + b"x" * 300_000 + b"' SE alive;\n" + tail, alive),
        ("deeply nested parentheses", head + b"DIKHAO " + b"(" * 60_000 + b"1" + b")" * 60_000 + b" SE alive;\n" + tail, alive),
        ("a long chain of operators", head + b"DIKHAO " + b"1 + " * 30_000 + b"1 SE alive;\n" + tail, alive),
        ("many tiny statements", head + b"DIKHAO 1 SE alive;\n" * 3_000 + tail, alive),
        ("invalid UTF-8", head + b"DIKHAO '\xff\xfe' SE alive;\n" + tail, alive),
        ("a NUL byte", head + b"DIKHAO 'a\x00b' SE alive;\n" + tail, alive),
        # a blank line opens a statement, so the dot is statement text (the shell quirk) and ends in a parser error
        ("a blank line then a lone dot", head + b"   \n.\n" + tail, "par '.' mila"),
    ]


def check_hardening(cli: str) -> list[str]:
    failures = []
    for name, data_in, expect in hardening_cases():
        label = f"hardening: {name}"
        with tempfile.TemporaryDirectory() as root:
            done = subprocess.run([cli, "shell", "--local", "--data", os.path.join(root, "d")], input=data_in, cwd=REPO_ROOT,
                                  capture_output=True, env=child_env(), timeout=180)
        out = done.stdout.decode("utf-8", "replace").replace("\r\n", "\n")
        if done.returncode == 0 and expect in out and out.endswith("Phir milenge!\n"):
            print(f"PASS {label} ({len(out)} bytes of output)")
        else:
            print(f"FAIL {label} (exit={done.returncode}, tail={out[-120:]!r}, stderr={done.stderr[-200:]!r})")
            failures.append(label)
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cli", required=True)
    args = parser.parse_args()
    cli = str(Path(args.cli).resolve())
    failures = check_logins(cli) + check_dropped(cli) + check_fallback(cli) + check_hardening(cli)
    print("ALL HELD" if not failures else "FAILED: " + "; ".join(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
