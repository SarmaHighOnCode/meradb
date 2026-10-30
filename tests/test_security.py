"""Proofs of concept for security flaws found in review. Each test failed before its fix."""

import hashlib
import json
import os
import shutil
import socket
import tempfile
import threading
import time
import unittest

from meradb import ast_nodes as ast
from meradb.catalog import Catalog
from meradb.engine import Engine
from meradb.errors import ExecutionError, StorageError
from meradb.protocol import MAX_HELLO_BYTES
from meradb.server import MeraDBServer, serve
from meradb.users import PBKDF2_ITERATIONS, PBKDF2_LEGACY_ITERATIONS, UserStore


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


class PasswordTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.users = UserStore(self.dir)

    def test_empty_password_is_refused(self):
        with self.assertRaises(ExecutionError):
            self.users.create("blank", "")
        self.assertFalse(self.users.exists("blank"))

    def write_users(self, entries: dict) -> UserStore:
        with open(os.path.join(self.dir, "users.json"), "w", encoding="utf-8") as f:
            json.dump(entries, f)
        return UserStore(self.dir)

    def test_old_account_still_logs_in_and_is_upgraded_by_the_login(self):
        salt = bytes(range(16))
        old = {"salt": salt.hex(), "hash": hashlib.pbkdf2_hmac("sha256", b"hunter2", salt, PBKDF2_LEGACY_ITERATIONS).hex(),
               "grants": {"main.t": ["DIKHAO"]}}
        users = self.write_users({"old": old})

        self.assertFalse(users.verify("old", "wrong"))
        self.assertNotIn("iterations", UserStore(self.dir).users["old"])  # a failed login changes nothing

        self.assertTrue(users.verify("old", "hunter2"))
        upgraded = UserStore(self.dir).users["old"]
        self.assertEqual(upgraded["iterations"], PBKDF2_ITERATIONS)
        self.assertNotEqual(upgraded["hash"], old["hash"])
        self.assertEqual(upgraded["grants"], old["grants"])
        self.assertTrue(UserStore(self.dir).verify("old", "hunter2"))
        self.assertFalse(UserStore(self.dir).verify("old", "wrong"))

    def test_new_accounts_use_the_stronger_work_factor(self):
        self.users.create("ravi", "pw")
        self.assertEqual(self.users.users["ravi"]["iterations"], PBKDF2_ITERATIONS)
        self.assertGreaterEqual(PBKDF2_ITERATIONS, 600_000)
        self.assertTrue(self.users.verify("ravi", "pw"))

    def test_damaged_entries_never_log_in_and_never_raise(self):
        for entry in [{"salt": "zz", "hash": "00"}, {"salt": 5, "hash": []}, 7, {"role": True}, {"salt": "00"},
                      {"salt": "00", "hash": "é"}, {"salt": "00", "hash": "00", "iterations": -1},
                      {"salt": "00", "hash": "00", "iterations": 0}, {"salt": "00", "hash": "00", "iterations": "many"},
                      {"salt": "00", "hash": "00", "iterations": True},
                      {"salt": "00", "hash": "00", "iterations": 99_999_999_999}]:
            with self.subTest(entry=entry):
                self.assertFalse(self.write_users({"r": entry}).verify("r", "x"))

    def test_a_password_json_cannot_encode_is_only_a_wrong_password(self):
        self.users.create("ravi", "pw")
        self.assertFalse(self.users.verify("ravi", "\ud800"))


class SharedPasswordTests(ServerCase):
    password = "s3cret"

    def test_non_ascii_guess_is_refused_cleanly(self):
        reply = self.client().request(type="hello", password="päss")
        self.assertEqual(reply, {"ok": False, "error": "Password galat hai"})
        self.assertTrue(self.client().request(type="hello", password="s3cret").get("ok"))


class LogTests(ServerCase):
    def setUp(self):
        super().setUp()
        self.server.verbose = True

    def test_passwords_in_queries_never_reach_the_log(self):
        c = self.client()
        c.request(type="hello")
        for text in ["BANAO USER asha GUPT 'TopSecret99';", "banao  user\n asha gupt 'TopSecret99';",
                     "BANAO USER asha 'TopSecret99';"]:
            c.request(type="query", text=text)
        self.assertNotIn("TopSecret99", "\n".join(self.log))
        c.request(type="query", text="DIKHAO 'still logged';")
        self.assertIn("still logged", "\n".join(self.log))


class LimitTests(ServerCase):
    def closed_within(self, client: RawClient, seconds: float) -> bool:
        client.sock.settimeout(seconds)
        try:
            return client.rfile.readline() == b""
        except socket.timeout:
            return False
        except OSError:
            return True

    def test_a_peer_that_never_says_hello_is_dropped(self):
        self.server.handshake_timeout = 0.2
        self.assertTrue(self.closed_within(self.client(), 3.0))

    def test_a_slow_hello_does_not_extend_the_deadline(self):
        self.server.handshake_timeout = 0.3
        c = self.client()
        try:
            for _ in range(30):  # 1.5 s in all: five times the deadline
                c.sock.sendall(b" ")
                time.sleep(0.05)
        except OSError:
            return  # the server hung up while we were still sending
        self.assertTrue(self.closed_within(c, 1.0))

    def test_a_logged_in_session_has_no_deadline(self):
        self.server.handshake_timeout = 0.2
        c = self.client()
        self.assertTrue(c.request(type="hello").get("ok"))
        time.sleep(0.5)
        self.assertTrue(c.request(type="ping").get("ok"))

    def test_a_hello_is_limited_in_size(self):
        c = self.client()
        try:
            self.assertFalse(c.request(type="hello", padding="x" * MAX_HELLO_BYTES).get("ok"))
        except OSError:
            pass  # closed with bytes still unread, which the OS reports as a reset

    def test_connections_beyond_the_limit_are_refused(self):
        self.server.max_connections = 2
        held = [self.client(), self.client()]
        for c in held:
            self.assertTrue(c.request(type="hello").get("ok"))
        extra = self.client()
        try:
            self.assertFalse(extra.request(type="hello").get("ok"))
        except OSError:
            pass  # the refusal can lose the race with our hello
        for c in held:
            c.close()
        self.clients = [extra]
        for _ in range(100):  # the slots come back once the handler threads end
            if self.server._connections == 0:
                break
            time.sleep(0.02)
        self.assertTrue(self.client().request(type="hello").get("ok"))


class LoginLockTests(ServerCase):
    def attempt(self, password: str) -> dict:
        c = self.client()
        return c.request(type="hello", user="ravi", password=password)

    def test_guessing_from_one_address_is_locked_out_then_recovers(self):
        self.server.instance.users.create("ravi", "pw")
        self.server.login_failure_limit = 3
        self.server.login_lock_seconds = 1.0
        self.assertFalse(self.attempt("guess1").get("ok"))
        self.assertTrue(self.attempt("pw").get("ok"))  # a success clears the count
        for guess in ["guess2", "guess3", "guess4"]:
            self.assertFalse(self.attempt(guess).get("ok"))
        self.assertEqual(self.attempt("pw"), {"ok": False, "error": "User ya password galat hai"})
        time.sleep(1.1)
        self.assertTrue(self.attempt("pw").get("ok"))


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


class NetworkBindTests(unittest.TestCase):
    def test_no_password_on_a_network_address_is_refused_before_binding(self):
        directory = tempfile.mkdtemp()
        os.environ.pop("MERADB_ALLOW_NO_PASSWORD", None)
        self.assertEqual(serve(directory, host="0.0.0.0", port=0), 1)
        self.assertFalse(os.path.exists(os.path.join(directory, "meradb.pid")))


class UsersFileTests(unittest.TestCase):
    @unittest.skipIf(os.name == "nt", "POSIX permission bits")
    def test_users_file_is_readable_by_its_owner_only(self):
        directory = tempfile.mkdtemp()
        users = UserStore(directory)
        users.create("ravi", "pw")
        self.assertEqual(os.stat(users.path).st_mode & 0o077, 0)


class DeviceNameTests(unittest.TestCase):
    def test_windows_device_names_cannot_be_created(self):
        directory = tempfile.mkdtemp()
        engine = Engine(directory)
        engine.execute("BANAO TABLE ok_table (id INT);")
        for name in ["nul", "NUL", "con", "prn", "aux", "com1", "Com9", "lpt1", "LPT9"]:
            for sql in [f"BANAO DATABASE {name};", f"BANAO TABLE {name} (id INT);", f"SUDHARO TABLE ok_table NAYA_NAAM {name};"]:
                with self.subTest(sql=sql), self.assertRaises(ExecutionError):
                    engine.execute(sql)
        engine.execute("BANAO TABLE console (id INT);")
        engine.execute("BANAO TABLE com10 (id INT);")


class PrivilegeTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.admin = Engine(self.dir)
        self.ravi = Engine(self.admin.instance)
        self.ravi.user = "ravi"
        self.admin.execute("BANAO USER ravi GUPT 'pw';"
                           "BANAO TABLE pub (id INT MUKHYA KUNJI, name TEXT);"
                           "BANAO TABLE hidden (id INT);"
                           "DAALO MEIN pub MAAN (1, 'a');"
                           "DAALO MEIN hidden MAAN (1);")

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def denied(self, text: str):
        with self.assertRaises(ExecutionError):
            self.ravi.execute(text)

    def name_of_row_one(self):
        return self.admin.execute("DIKHAO name SE pub JAHAN id = 1;")[0].rows[0][0]

    def test_sub_selects_need_dikhao_wherever_they_are_written(self):
        self.admin.execute("ADHIKAR DO SAB PAR pub KO ravi;")
        self.denied("DIKHAO id SE pub JAHAN id MEIN (DIKHAO id SE hidden);")
        self.denied("DIKHAO (DIKHAO id SE hidden) SE pub;")
        self.denied("BADLO pub RAKHO name = 'x' JAHAN id MEIN (DIKHAO id SE hidden);")
        self.denied("MITAO SE pub JAHAN id MEIN (DIKHAO id SE hidden);")
        self.assertEqual(self.name_of_row_one(), "a")
        self.ravi.execute("DIKHAO id SE pub JAHAN id MEIN (DIKHAO id SE pub);")

    def test_upsert_needs_badlo(self):
        self.admin.execute("ADHIKAR DO DAALO PAR pub KO ravi;")
        self.denied("DAALO MEIN pub MAAN (1, 'x') TAKRAAV PAR BADLO name = 'hacked';")
        self.assertEqual(self.name_of_row_one(), "a")
        self.ravi.execute("DAALO MEIN pub MAAN (2, 'new');")

    def test_explain_of_a_set_operation_needs_dikhao(self):
        self.denied("SAMJHAO DIKHAO id SE hidden SANYUKT DIKHAO id SE hidden;")


class RestrictedRequestTests(ServerCase):
    password = "adminpw"  # with users present, the superuser session needs the shared password

    def setUp(self):
        super().setUp()
        self.server.instance.users.create("ravi", "pw")
        self.admin = self.client()
        self.assertTrue(self.admin.request(type="hello", password="adminpw").get("ok"))
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
