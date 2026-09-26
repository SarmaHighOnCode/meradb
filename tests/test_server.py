"""Client <-> server tests over real TCP sockets, plus the `meradb start/stop/status` commands."""

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import unittest

from meradb.client import Connection
from meradb.engine import Engine
from meradb.errors import ConnectionFailed, MeraDBError, ServerUnavailable
from meradb.protocol import running_server
from meradb.server import MeraDBServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class ServerTestCase(unittest.TestCase):
    password = None

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.server = MeraDBServer(self.dir, "127.0.0.1", 0, password=self.password, log=lambda line: None)
        self.server.instance.lock_timeout = 0.5
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        self.thread.start()
        self.connections = []

    def tearDown(self):
        for c in self.connections:
            c.close()
        self.server.shutdown()
        self.server.server_close()
        shutil.rmtree(self.dir, ignore_errors=True)

    def connect(self, **kwargs) -> Connection:
        conn = Connection("127.0.0.1", self.server.port, password=self.password, **kwargs)
        self.connections.append(conn)
        return conn


class ClientServerTest(ServerTestCase):
    def test_query_round_trip_keeps_types(self):
        db = self.connect()
        db.execute("BANAO TABLE t (i INT, f FLOAT, s TEXT, b BOOL); DAALO MEIN t MAAN (1, 2.5, 'Ravi''s', SACH), "
                   "(KHALI, 7.0, KHALI, JHOOTH)")
        result = db.execute("DIKHAO * SE t")[0]
        self.assertEqual(result.columns, ["i", "f", "s", "b"])
        self.assertEqual(result.rows, [[1, 2.5, "Ravi's", True], [None, 7.0, None, False]])
        self.assertIsInstance(result.rows[1][1], float)

    def test_query_round_trip_keeps_dates(self):
        # a `date` value can't go straight into JSON -- protocol.py/engine.py encode it
        # as {"$date": "YYYY-MM-DD"} on the wire, and the client must decode it back.
        import datetime

        db = self.connect()
        db.execute("BANAO TABLE t (dob DATE); DAALO MEIN t MAAN ('2005-06-15'), (KHALI)")
        result = db.execute("DIKHAO * SE t KRAM dob")[0]
        self.assertEqual(result.rows, [[None], [datetime.date(2005, 6, 15)]])
        self.assertIsInstance(result.rows[1][0], datetime.date)

    def test_errors_come_back_per_statement(self):
        results = self.connect().run_script("DIKHAO * SE nahi_hai; DIKHAO TABLES;")
        self.assertIn("exist nahi karta", results[0].error)
        self.assertEqual(results[1].error, "")

    def test_parse_error(self):
        results = self.connect().run_script("DIKHAO SE")
        self.assertIn("Parser", results[0].error)

    def test_sessions_have_their_own_current_database(self):
        a, b = self.connect(), self.connect()
        a.execute("BANAO DATABASE college; ISTEMAL college; BANAO TABLE t (x INT)")
        self.assertEqual(a.current_db, "college")
        self.assertEqual(b.current_db, "main")
        self.assertEqual(b.execute("DIKHAO TABLES")[0].rows, [])
        self.assertEqual(self.connect(database="college").execute("DIKHAO TABLES")[0].rows, [["t"]])

    def test_writes_are_visible_to_other_clients(self):
        a, b = self.connect(), self.connect()
        a.execute("BANAO TABLE t (id INT MUKHYA KUNJI); DAALO MEIN t MAAN (1)")
        self.assertEqual(b.execute("DIKHAO * SE t JAHAN id = 1")[0].rows, [[1]])
        with self.assertRaises(MeraDBError):
            b.execute("DAALO MEIN t MAAN (1)")  # uniqueness is shared too

    def test_transaction_isolation_between_clients(self):
        a, b = self.connect(), self.connect()
        a.execute("BANAO TABLE t (x INT)")
        a.execute("SHURU; DAALO MEIN t MAAN (1)")
        self.assertTrue(a.in_transaction)
        self.assertIn("busy", b.run_script("DIKHAO * SE t")[0].error)  # b must wait for a
        a.execute("WAPAS")
        self.assertEqual(b.execute("DIKHAO * SE t")[0].rows, [])

    def test_disconnect_rolls_back(self):
        a = self.connect()
        a.execute("BANAO TABLE t (x INT); DAALO MEIN t MAAN (1); SHURU; MITAO SE t")
        a.close()
        self.assertEqual(self.connect().execute("DIKHAO * SE t")[0].rows, [[1]])

    def test_schema_and_status(self):
        db = self.connect()
        db.execute("BANAO TABLE t (id INT MUKHYA KUNJI)")
        tree = db.schema_tree()
        self.assertEqual(tree[0]["name"], "main")
        self.assertEqual(tree[0]["tables"][0]["columns"][0]["primary_key"], True)
        self.assertEqual(db.status()["sessions"], 1)

    def test_embedded_engine_refuses_a_served_folder(self):
        from meradb.protocol import write_pid_file

        write_pid_file(self.dir, {"pid": os.getpid(), "host": "127.0.0.1", "port": self.server.port})
        with self.assertRaises(MeraDBError):
            Engine(self.dir)

    def test_no_server(self):
        with self.assertRaises(ServerUnavailable):
            Connection("127.0.0.1", free_port())


class PasswordTest(ServerTestCase):
    password = "s3cret"

    def test_right_password(self):
        self.assertEqual(self.connect().execute("DIKHAO TABLES")[0].rows, [])

    def test_wrong_password(self):
        with self.assertRaises(ConnectionFailed) as ctx:
            Connection("127.0.0.1", self.server.port, password="nope")
        self.assertNotIsInstance(ctx.exception, ServerUnavailable)


class CommandLineTest(unittest.TestCase):
    """Runs the real `python -m meradb start / status / run / stop` as separate processes."""

    def meradb(self, *args, env=None):
        return subprocess.run([sys.executable, "-m", "meradb", *args], cwd=ROOT, capture_output=True,
                              text=True, timeout=60, env=env)

    def test_start_query_stop(self):
        data = tempfile.mkdtemp()
        port = str(free_port())
        try:
            started = self.meradb("start", "--data", data, "--port", port)
            self.assertEqual(started.returncode, 0, started.stdout + started.stderr)
            self.assertIsNotNone(running_server(data))

            self.assertEqual(self.meradb("status", "--data", data).returncode, 0)

            script = os.path.join(data, "t.mdb")
            with open(script, "w", encoding="utf-8") as f:
                f.write("BANAO TABLE t (x INT); DAALO MEIN t MAAN (42); DIKHAO * SE t;")
            ran = self.meradb("run", "--port", port, script)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("42", ran.stdout)
            self.assertNotIn("LOCAL mode", ran.stderr)  # it really went through the server

            stopped = self.meradb("stop", "--data", data)
            self.assertEqual(stopped.returncode, 0, stopped.stdout + stopped.stderr)
            self.assertIsNone(running_server(data))
            self.assertEqual(self.meradb("status", "--data", data).returncode, 3)
        finally:
            self.meradb("stop", "--data", data, "--force")
            shutil.rmtree(data, ignore_errors=True)

    def test_local_fallback_when_no_server(self):
        data = tempfile.mkdtemp()
        try:
            # point the default port somewhere empty, even if a real server runs on 6372
            env = dict(os.environ, MERADB_PORT=str(free_port()))
            ran = self.meradb("run", "--data", data, os.path.join(ROOT, "examples", "demo.mdb"), env=env)
            self.assertIn("LOCAL mode", ran.stderr)
            self.assertIn("Transaction WAPAS", ran.stdout)
        finally:
            shutil.rmtree(data, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
