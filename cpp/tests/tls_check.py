"""TLS with the real programs, on 127.0.0.1: meradb_devcert, `meradb start`, stop and status through the pid file,
`meradb run`, and the Python client against the C++ server.

usage: tls_check.py --cli meradb_cli --devcert meradb_devcert
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from meradb.client import Connection  # noqa: E402
from meradb.errors import ConnectionFailed, MeraDBError  # noqa: E402

failures = []


def check(name, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else "\n" + str(detail)))
    if not ok:
        failures.append(name)


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    parser.add_argument("--devcert", required=True)
    args = parser.parse_args()

    root = tempfile.mkdtemp(prefix="mdb_tls_")
    env = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_")}
    py_env = dict(env, PYTHONPATH=str(ROOT))
    data = os.path.join(root, "data")
    certs = os.path.join(root, "certs")
    os.makedirs(certs)
    port = free_port()

    def cli(*words, **kw):
        return subprocess.run([args.cli, *words], env=env, cwd=root, capture_output=True, text=True, timeout=60, **kw)

    def python(*words):
        return subprocess.run([sys.executable, "-m", "meradb", *words], env=py_env, cwd=root, capture_output=True, text=True, timeout=60)

    try:
        made = subprocess.run([args.devcert, "--out", certs], capture_output=True, text=True, timeout=60)
        cert, key = os.path.join(certs, "meradb-dev.crt"), os.path.join(certs, "meradb-dev.key")
        check("devcert makes both files", made.returncode == 0 and os.path.exists(cert) and os.path.exists(key), made.stderr)
        again = subprocess.run([args.devcert, "--out", certs], capture_output=True, text=True, timeout=60)
        check("devcert will not overwrite", again.returncode == 1, again.stdout + again.stderr)

        script = os.path.join(root, "a.mdb")
        Path(script).write_text("BANAO TABLE t (a INT);\nDAALO MEIN t MAAN (42);\nDIKHAO * SE t;\n", encoding="utf-8")

        started = cli("start", "--data", data, "--port", str(port), "--tls-cert", cert, "--tls-key", key)
        check("start with a certificate", started.returncode == 0, started.stdout + started.stderr)

        status = cli("status", "--data", data)
        check("status finds its way through the pid file", status.returncode == 0 and "databases" in status.stdout and "main" in status.stdout,
              status.stdout + status.stderr)
        py_status = python("status", "--data", data)
        check("Python status too", py_status.returncode == 0 and "main" in py_status.stdout, py_status.stdout + py_status.stderr)

        ran = cli("run", "-H", "127.0.0.1", "-p", str(port), "--tls-ca", cert, script)
        check("run with --tls-ca", ran.returncode == 0 and "42" in ran.stdout, ran.stdout + ran.stderr)

        plain = cli("run", "-H", "127.0.0.1", "-p", str(port), script)
        check("run without TLS is refused and says why", plain.returncode != 0 and "TLS" in plain.stdout + plain.stderr, plain.stdout + plain.stderr)

        system_roots = cli("run", "-H", "127.0.0.1", "-p", str(port), "--tls", script)
        check("--tls alone does not trust a self-signed certificate", system_roots.returncode != 0, system_roots.stdout + system_roots.stderr)

        via_env = subprocess.run([args.cli, "run", "-H", "127.0.0.1", "-p", str(port), script], env=dict(env, MERADB_TLS_CA=cert),
                                 cwd=root, capture_output=True, text=True, timeout=60)
        check("MERADB_TLS_CA turns it on", via_env.returncode == 0 or "pehle se" in via_env.stdout + via_env.stderr, via_env.stdout + via_env.stderr)

        with Connection("127.0.0.1", port, tls_ca=cert) as conn:
            results = conn.run_script("DIKHAO * SE t")
            check("Python client reads over TLS", not results[0].error and results[0].rows and results[0].rows[0][0] == 42, results)
            check("Python client: status over TLS", "databases" in conn.status())
        with Connection("localhost", port, tls=True, tls_ca=cert) as conn:
            check("Python client: a name in the certificate", not conn.run_script("DIKHAO DATABASES")[0].error)

        def refused(name, **kw):
            try:
                Connection("127.0.0.1", port, **kw).close()
                check(name, False, "connected")
            except ConnectionFailed as e:
                check(name, True)
                return str(e)
            except MeraDBError as e:
                check(name, False, repr(e))
            return ""

        refused("Python client: --tls alone does not trust it", tls=True)
        text = refused("Python client without TLS is told to use it")
        check("the refusal says TLS", "TLS" in text, text)
        other = os.path.join(root, "other")
        os.makedirs(other)
        subprocess.run([args.devcert, "--out", other], capture_output=True, timeout=60, check=True)
        refused("Python client: another certificate", tls_ca=os.path.join(other, "meradb-dev.crt"))
        python_run = python("run", "-H", "127.0.0.1", "-p", str(port), "--tls-ca", cert, script)
        check("Python run with --tls-ca", "42" in python_run.stdout, python_run.stdout + python_run.stderr)

        stopped = cli("stop", "--data", data)
        check("stop through the pid file", stopped.returncode == 0, stopped.stdout + stopped.stderr)

        # A plain server: asking for TLS must fail, not go plain.
        plain_port = free_port()
        plain_data = os.path.join(root, "plain")
        started = cli("start", "--data", plain_data, "--port", str(plain_port))
        check("a plain server still starts", started.returncode == 0, started.stdout + started.stderr)
        downgrade = cli("run", "-H", "127.0.0.1", "-p", str(plain_port), "--tls-ca", cert, script)
        check("C++ client does not fall back to plain", downgrade.returncode != 0 and "42" not in downgrade.stdout, downgrade.stdout + downgrade.stderr)
        try:
            Connection("127.0.0.1", plain_port, tls_ca=cert).close()
            check("Python client does not fall back to plain", False, "connected")
        except ConnectionFailed:
            check("Python client does not fall back to plain", True)
        clean = cli("run", "-H", "127.0.0.1", "-p", str(plain_port), script)
        check("plain client to a plain server still works", clean.returncode == 0 and "42" in clean.stdout, clean.stdout + clean.stderr)
        cli("stop", "--data", plain_data)

        # Nobody listening, TLS asked for: an error, never local mode.
        env_nobody = subprocess.run([args.cli, "run", "--data", os.path.join(root, "x"), "--tls-ca", cert, script],
                                    env=dict(env, MERADB_PORT=str(free_port())), cwd=root, capture_output=True, text=True, timeout=60)
        check("TLS asked, nobody there: no fallback to local mode", env_nobody.returncode != 0 and "LOCAL" not in env_nobody.stderr,
              env_nobody.stdout + env_nobody.stderr)

        py_server = python("server", "--data", os.path.join(root, "py"), "--port", str(free_port()), "--tls-cert", cert, "--tls-key", key)
        check("Python server refuses instead of running plain", py_server.returncode == 1 and "TLS" in py_server.stderr, py_server.stdout + py_server.stderr)
    finally:
        cli("stop", "--data", data, "--force")
        shutil.rmtree(root, ignore_errors=True)
    if failures:
        print("FAILED: " + ", ".join(failures))
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
