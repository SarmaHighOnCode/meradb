"""
Interop matrix: every client against every server.

                    Python server        C++ server
    Python client   (the baseline)       C++ server must behave like Python's
    C++ client      C++ client must work against Python's server

1. SCRIPTS: each script is run through all four client/server pairs with
   `run --port P`; the output of the three other pairs must equal the
   (Python client, Python server) baseline byte for byte.
2. RAW PROTOCOL: the same list of messages is sent to both servers over a plain
   socket and every reply line is compared (after hiding the parts that
   legitimately differ: pid, data folder, start time, and the wording of a
   JSON syntax error).
3. LOGINS: users made with the PYTHON `UserStore` in the data folder of both
   servers -- so the C++ server is reading a Python-written `users.json` --
   log in over the wire and are held to their grants.

Usage:
    python cpp/tests/interop_check.py --cli path/to/meradb_cli [script.mdb ...]
With no script arguments it uses examples/demo.mdb and
examples/rdbms_lab_coverage.mdb. Exit code 0 = everything matched.
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.stdout.reconfigure(encoding="utf-8")  # diffs may contain non-ASCII text; never die on a console code page
sys.path.insert(0, str(REPO_ROOT))


def env() -> dict:
    e = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_")}
    e.update(PYTHONIOENCODING="utf-8", PYTHONPATH=str(REPO_ROOT))
    return e


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def port_open(port: int) -> bool:
    with socket.socket() as s:
        s.settimeout(0.3)
        return s.connect_ex(("127.0.0.1", port)) == 0


class Server:
    """A foreground server process of the given kind ("python" or "cpp") on a fresh data folder."""

    def __init__(self, kind: str, cli: str, data: str, password: str | None = None):
        self.kind, self.cli, self.data, self.port = kind, cli, data, free_port()
        base = ([sys.executable, "-m", "meradb"] if kind == "python" else [cli])
        command = base + ["server", "--data", data, "--port", str(self.port)]
        e = env()
        if password:
            e["MERADB_PASSWORD"] = password
        self.proc = subprocess.Popen(command, cwd=REPO_ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=e)
        deadline = time.time() + 20
        while time.time() < deadline and not port_open(self.port):
            if self.proc.poll() is not None:
                raise SystemExit(f"{kind} server exited early")
            time.sleep(0.1)
        if not port_open(self.port):
            raise SystemExit(f"{kind} server did not start")

    def stop(self):
        try:
            with socket.create_connection(("127.0.0.1", self.port), timeout=3) as s:
                f = s.makefile("rwb")
                f.write(b'{"type": "hello", "version": 1, "password": null, "database": null, "user": null}\n')
                f.flush()
                f.readline()
                f.write(b'{"type": "shutdown"}\n')
                f.flush()
                f.readline()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            self.proc.kill()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stop()


def run_client(kind: str, cli: str, port: int, script: str, extra_env: dict | None = None) -> tuple[str, int]:
    base = ([sys.executable, "-m", "meradb"] if kind == "python" else [cli])
    e = env()
    e.update(extra_env or {})
    r = subprocess.run(base + ["run", "--port", str(port), script], cwd=REPO_ROOT, capture_output=True, env=e, timeout=300)
    return r.stdout.decode("utf-8").replace("\r\n", "\n"), r.returncode


# ---------------------------------------------------------------- 1. scripts

def check_scripts(cli: str, scripts: list[str]) -> list[str]:
    failures = []
    for script in scripts:
        results = {}
        for server_kind in ("python", "cpp"):
            for client_kind in ("python", "cpp"):
                with tempfile.TemporaryDirectory() as data, Server(server_kind, cli, data) as server:
                    results[(client_kind, server_kind)] = run_client(client_kind, cli, server.port, script)
        baseline = results[("python", "python")]
        for pair, got in results.items():
            name = f"{Path(script).name}: {pair[0]} client -> {pair[1]} server"
            if pair == ("python", "python"):
                continue
            if got == baseline:
                print(f"PASS {name} ({len(got[0].splitlines())} lines)")
            else:
                print(f"FAIL {name}")
                failures.append(name)
                import difflib
                print("".join(list(difflib.unified_diff(baseline[0].splitlines(keepends=True), got[0].splitlines(keepends=True),
                                                        "python->python", f"{pair[0]}->{pair[1]}"))[:60]))
    return failures


# ---------------------------------------------------------------- 2. raw protocol

HELLO = {"type": "hello", "version": 1, "password": None, "database": None, "user": None}


def dumps(message) -> bytes:
    return (json.dumps(message) + "\n").encode("utf-8")


def normalise(reply: bytes) -> str:
    text = reply.decode("utf-8", "replace").rstrip("\n")
    text = re.sub(r'"pid": \d+', '"pid": 0', text)
    text = re.sub(r'"started": "[^"]*"', '"started": "T"', text)
    text = re.sub(r'"data_dir": "(?:[^"\\]|\\.)*"', '"data_dir": "D"', text)
    text = re.sub(r'\[Protocol Galti\] Galat message: .*?"\}', '[Protocol Galti] Galat message: X"}', text)  # json's own wording
    return text


def exchange(port: int, lines: list, timeout: float = 5.0) -> list[str]:
    """Sends each item (dict -> JSON line, bytes -> raw) and collects one reply line per item ('' = closed)."""
    replies = []
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        f = s.makefile("rwb")
        for item in lines:
            f.write(item if isinstance(item, bytes) else dumps(item))
            f.flush()
            replies.append(normalise(f.readline()))
    return replies


def query(text: str) -> dict:
    return {"type": "query", "text": text}


PROTOCOL_SCENARIOS = {
    "basics": [HELLO, {"type": "ping"}, {"type": "status"}, {"type": "nope"}, {"type": "schema"}],
    "queries": [HELLO,
                query("BANAO TABLE t (id INT MUKHYA KUNJI, name TEXT, score FLOAT, born DATE, ok BOOL)"),
                query("DAALO MEIN t MAAN (1, 'café 世界 \U0001F600', 1.5, '2024-02-29', SAHI), "
                      "(2, NULL, 100000000000000000000.0, NULL, GALAT), (3, 'del\x7f', 0.00001, '1999-12-31', NULL)"),
                query("DIKHAO * SE t ORDER BY id"),
                query("DIKHAO id, score * 2 SE t; DIKHAO * SE gayab; DIKHAO 1 / 0"),
                query("DIKHAO COUNT(*), AVG(score) SE t"),
                {"type": "schema"}, {"type": "status"}],
    "bad messages": [HELLO, b"not json at all\n", b"[1, 2, 3]\n", b'{"type": "query"}\n', b'{"type": "query", "text": 5}\n',
                     b'{"type": "query", "text": "DIKHAO TABLES"}\n', b"\n", {"type": "ping"}],
    "transaction": [HELLO, query("BANAO TABLE a (x INT); SHURU; DAALO MEIN a MAAN (1)"), query("DIKHAO * SE a"),
                    query("WAPAS"), query("DIKHAO * SE a")],
    "hello variants": [{"type": "hello"}],
    "first message not hello": [{"type": "ping"}],
    "database in hello": [dict(HELLO, database="main"), {"type": "status"}],
    "unknown database in hello": [dict(HELLO, database="nosuchdb")],
    "unicode in values and in error positions": [
        HELLO, query("BANAO TABLE u (name TEXT)"),
        query("DAALO MEIN u MAAN ('यूज़र 😀'); DIKHAO * SE u"),
        query("DAALO MEIN u MAAN ('café') @"),        # the error column counts characters, not bytes
        query("DIKHAO * SE u JAHAN name = 'é' £")],  # an unexpected non-ASCII character is shown whole
    "shutdown from loopback is refused for the wrong first message": [{"type": "shutdown"}],
}


def check_protocol(cli: str) -> list[str]:
    failures = []
    for name, lines in PROTOCOL_SCENARIOS.items():
        got = {}
        for kind in ("python", "cpp"):
            with tempfile.TemporaryDirectory() as data, Server(kind, cli, data) as server:
                try:
                    got[kind] = exchange(server.port, lines)
                except (OSError, ValueError) as e:
                    got[kind] = [f"<{type(e).__name__}>"]
        label = f"protocol: {name}"
        if got["python"] == got["cpp"]:
            print(f"PASS {label}")
        else:
            print(f"FAIL {label}")
            failures.append(label)
            for i, (a, b) in enumerate(zip(got["python"], got["cpp"])):
                if a != b:
                    print(f"  message {i}:\n    python: {a[:400]}\n    cpp:    {b[:400]}")
            if len(got["python"]) != len(got["cpp"]):
                print(f"  reply counts differ: python={len(got['python'])} cpp={len(got['cpp'])}")
    # a shared password
    got = {}
    for kind in ("python", "cpp"):
        with tempfile.TemporaryDirectory() as data, Server(kind, cli, data, password="sekrit") as server:
            got[kind] = (exchange(server.port, [HELLO]), exchange(server.port, [dict(HELLO, password="wrong")]),
                         exchange(server.port, [dict(HELLO, password="sekrit"), {"type": "ping"}]))
    label = "protocol: shared password (none / wrong / right)"
    if got["python"] == got["cpp"]:
        print(f"PASS {label}")
    else:
        print(f"FAIL {label}\n  python: {got['python']}\n  cpp:    {got['cpp']}")
        failures.append(label)
    return failures


# ---------------------------------------------------------------- 3. logins with Python-written users.json

def check_logins(cli: str) -> list[str]:
    from meradb.engine import Engine
    from meradb.users import UserStore

    failures = []
    got = {}
    for kind in ("python", "cpp"):
        with tempfile.TemporaryDirectory() as data:
            engine = Engine(data)
            engine.execute("BANAO TABLE staff (id INT, name TEXT); DAALO MEIN staff MAAN (1, 'a'), (2, 'b'); "
                           "BANAO TABLE secret (x INT); DAALO MEIN secret MAAN (9)")
            engine.close()
            users = UserStore(data)  # the Python module writes users.json; the C++ server must read it
            users.create("asha", "pw-é")
            users.grant("asha", "main", "staff", ["DIKHAO", "DAALO"])
            with Server(kind, cli, data) as server:
                login = dict(HELLO, user="asha", password="pw-é")
                got[kind] = [
                    exchange(server.port, [login, query("DIKHAO * SE staff"), query("DAALO MEIN staff MAAN (3, 'c')"),
                                           query("DIKHAO * SE secret"), query("MITAO SE staff"),
                                           query("BANAO TABLE z (x INT)"), query("DIKHAO TABLES")]),
                    exchange(server.port, [dict(HELLO, user="asha", password="wrong")]),
                    exchange(server.port, [dict(HELLO, user="ghost", password="x")]),
                    exchange(server.port, [dict(login, database="main")]),
                ]
    label = "logins: Python-written users.json, grants enforced"
    if got["python"] == got["cpp"]:
        print(f"PASS {label}")
    else:
        print(f"FAIL {label}")
        failures.append(label)
        for i, (a, b) in enumerate(zip(got["python"], got["cpp"])):
            if a != b:
                print(f"  scenario {i}:\n    python: {a}\n    cpp:    {b}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scripts", nargs="*")
    parser.add_argument("--cli", required=True)
    args = parser.parse_args()
    args.cli = str(Path(args.cli).resolve())  # children run with cwd=REPO_ROOT, so a relative path would break
    scripts = args.scripts or [str(REPO_ROOT / "examples" / "demo.mdb"), str(REPO_ROOT / "examples" / "rdbms_lab_coverage.mdb")]
    failures = check_scripts(args.cli, scripts) + check_protocol(args.cli) + check_logins(args.cli)
    print("ALL MATCHED" if not failures else "FAILED: " + "; ".join(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
