"""Proofs of concept for security flaws found in review. Each test failed before its fix."""

import json
import os
import shutil
import socket
import tempfile
import threading
import unittest

from meradb import ast_nodes as ast
from meradb.catalog import Catalog
from meradb.engine import Engine
from meradb.errors import ExecutionError, StorageError
from meradb.server import MeraDBServer


class RawClient:
    """The protocol by hand: send whatever you like, read back what the server sent."""

    def __init__(self, port: int):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.rfile = self.sock.makefile("rb")

    def request(self, **message) -> dict:
        self.sock.sendall((json.dumps(message) + "\n").encode())
        line = self.rfile.readline()
        return json.loads(line) if line else {}

    def close(self):
        self.rfile.close()
        self.sock.close()


class ServerCase(unittest.TestCase):
    password = None

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.data = os.path.join(self.dir, "data")
        self.log = []
        self.server = MeraDBServer(self.data, "127.0.0.1", 0, password=self.password, log=self.log.append)
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        self.thread.start()
        self.clients = []

    def tearDown(self):
        for c in self.clients:
            c.close()
        self.server.shutdown()
        self.server.server_close()
        shutil.rmtree(self.dir, ignore_errors=True)

    def client(self) -> RawClient:
        c = RawClient(self.server.port)
        self.clients.append(c)
        return c


class PathTests(ServerCase):
    def test_database_name_cannot_leave_the_data_folder(self):
        os.makedirs(os.path.join(self.dir, "outside"))
        reply = self.client().request(type="hello", database="../outside")
        self.assertFalse(reply.get("ok"))

        engine = Engine(self.server.instance)
        for name in ["../outside", "..\\outside", "..", ".", ".wapas", "main:stream", "main.", "main ", "a\nb"]:
            with self.subTest(name=name), self.assertRaises(ExecutionError):
                engine.execute_statement(ast.UseDatabase(name))

    def test_table_name_in_a_catalog_cannot_leave_the_database_folder(self):
        catalog = Catalog(os.path.join(self.data, "main"))
        for name in ["../../evil", "t:stream", "..", ""]:
            with self.subTest(name=name), self.assertRaises(StorageError):
                catalog.table_path(name)


class RestrictedRequestTests(ServerCase):
    def setUp(self):
        super().setUp()
        self.server.instance.users.create("ravi", "pw")
        self.admin = self.client()
        self.assertTrue(self.admin.request(type="hello").get("ok"))
        self.ravi = self.client()
        self.assertTrue(self.ravi.request(type="hello", user="ravi", password="pw").get("ok"))

    def test_restricted_user_cannot_shut_the_server_down(self):
        self.assertFalse(self.ravi.request(type="shutdown").get("ok"))
        self.assertTrue(self.ravi.request(type="ping").get("ok"))

    def test_restricted_user_does_not_get_status_details(self):
        reply = self.ravi.request(type="status")
        self.assertTrue(reply.get("ok"))
        for key in ["data_dir", "databases", "pid", "sessions", "started"]:
            self.assertNotIn(key, reply)
        self.assertIn("databases", self.admin.request(type="status"))

    def test_schema_shows_a_restricted_user_only_what_it_was_granted(self):
        self.admin.request(type="query", text="BANAO TABLE topsecret (salary INT); BANAO TABLE mine (id INT);"
                                              "ADHIKAR DO DAALO PAR mine KO ravi; BANAO DATABASE otherdb;")
        tree = json.dumps(self.ravi.request(type="schema"))
        self.assertIn("mine", tree)
        for hidden in ["topsecret", "salary", "otherdb"]:
            self.assertNotIn(hidden, tree)
        self.assertIn("topsecret", json.dumps(self.admin.request(type="schema")))


if __name__ == "__main__":
    unittest.main()
