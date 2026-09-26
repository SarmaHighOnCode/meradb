"""
Tests for the Phase B "systems features":

    users & privileges       BANAO USER / HATAO USER / ADHIKAR DO / ADHIKAR WAPAS
    triggers                 BANAO TRIGGER / HATAO TRIGGER, PEHLE/BAAD, NAYA/PURANA
    stored procedures        BANAO PROCEDURE / HATAO PROCEDURE / CHALAO

Same style as tests/test_gap_features.py: one TestCase per feature, including
error cases and persistence across a restart, always in a tempfile.mkdtemp()
directory. The client/server test (authenticated restricted user) uses real
TCP sockets, same style as tests/test_server.py.
"""

import shutil
import socket
import tempfile
import threading
import unittest

from meradb.client import Connection
from meradb.engine import Engine
from meradb.errors import ExecutionError, ParseError
from meradb.server import MeraDBServer


class EngineTestCase(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.db = Engine(self.dir)

    def tearDown(self):
        self.db.close()
        shutil.rmtree(self.dir, ignore_errors=True)

    def run_sql(self, text, engine=None):
        return (engine or self.db).execute(text)[-1]

    def rows(self, text, engine=None):
        return self.run_sql(text, engine).rows

    def reopen(self) -> Engine:
        self.db.close()
        self.db = Engine(self.dir)
        return self.db

    def assertFails(self, sql, message_fragment=None, engine=None):
        with self.assertRaises(ExecutionError) as ctx:
            (engine or self.db).execute(sql)
        if message_fragment:
            self.assertIn(message_fragment, str(ctx.exception))


# ============================================================================
# Users & privileges
# ============================================================================


class UsersTestCase(EngineTestCase):
    def test_create_drop_and_verify_password(self):
        self.run_sql("BANAO USER ravi GUPT 'secret123'")
        self.assertTrue(self.db.instance.users.exists("ravi"))
        self.assertTrue(self.db.instance.users.verify("ravi", "secret123"))
        self.assertFalse(self.db.instance.users.verify("ravi", "wrong"))
        # password is NEVER stored in the clear
        with open(self.db.instance.users.path, "r", encoding="utf-8") as f:
            self.assertNotIn("secret123", f.read())

        self.run_sql("HATAO USER ravi")
        self.assertFalse(self.db.instance.users.exists("ravi"))

    def test_duplicate_user_fails(self):
        self.run_sql("BANAO USER ravi GUPT 'x'")
        self.assertFails("BANAO USER ravi GUPT 'y'", "pehle se hai")

    def test_drop_unknown_user_fails(self):
        self.assertFails("HATAO USER ghost", "exist nahi karta")

    def test_grant_and_revoke(self):
        self.run_sql("BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT)")
        self.run_sql("BANAO USER ravi GUPT 'x'")
        self.run_sql("ADHIKAR DO DIKHAO, DAALO PAR students KO ravi")
        self.assertTrue(self.db.instance.users.has_privilege("ravi", "main", "students", "DIKHAO"))
        self.assertTrue(self.db.instance.users.has_privilege("ravi", "main", "students", "DAALO"))
        self.assertFalse(self.db.instance.users.has_privilege("ravi", "main", "students", "MITAO"))

        self.run_sql("ADHIKAR WAPAS DIKHAO PAR students SE ravi")
        self.assertFalse(self.db.instance.users.has_privilege("ravi", "main", "students", "DIKHAO"))
        self.assertTrue(self.db.instance.users.has_privilege("ravi", "main", "students", "DAALO"))

    def test_grant_sab_expands_to_all_four(self):
        self.run_sql("BANAO TABLE students (id INT)")
        self.run_sql("BANAO USER ravi GUPT 'x'")
        self.run_sql("ADHIKAR DO SAB PAR students KO ravi")
        for priv in ("DIKHAO", "DAALO", "BADLO", "MITAO"):
            self.assertTrue(self.db.instance.users.has_privilege("ravi", "main", "students", priv))

    def test_grant_unknown_user_fails(self):
        self.run_sql("BANAO TABLE students (id INT)")
        self.assertFails("ADHIKAR DO DIKHAO PAR students KO ghost", "exist nahi karta")

    def test_no_username_session_is_unrestricted_superuser(self):
        """THE most important test in this file: a session that never
        authenticates with a username must keep working EXACTLY as before --
        every existing embedded Engine, and every one of the 235 pre-Phase-B
        tests, is exactly this kind of session."""
        self.run_sql("BANAO USER ravi GUPT 'x'")  # users existing at all changes nothing
        self.run_sql("BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT)")
        self.run_sql("DAALO MEIN students MAAN (1, 'Ravi')")
        self.run_sql("BADLO students RAKHO naam = 'Ravi Kumar' JAHAN id = 1")
        self.run_sql("MITAO SE students JAHAN id = 1")
        self.assertEqual(self.db.user, None)  # never set -> always superuser

    def test_restricted_session_enforces_privileges(self):
        self.run_sql("BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT)")
        self.run_sql("DAALO MEIN students MAAN (1, 'Ravi')")
        self.run_sql("BANAO USER ravi GUPT 'x'")
        self.run_sql("ADHIKAR DO DIKHAO PAR students KO ravi")

        restricted = Engine(self.db.instance)
        restricted.user = "ravi"
        self.assertEqual(self.rows("DIKHAO * SE students", restricted), [[1, "Ravi"]])
        self.assertFails("DAALO MEIN students MAAN (2, 'Simran')", "adhikar nahi hai", engine=restricted)
        self.assertFails("BADLO students RAKHO naam = 'x' JAHAN id = 1", "adhikar nahi hai", engine=restricted)
        self.assertFails("MITAO SE students JAHAN id = 1", "adhikar nahi hai", engine=restricted)
        self.assertFails("BANAO TABLE t2 (a INT)", "superuser nahi hai", engine=restricted)
        self.assertFails("BANAO USER doosra GUPT 'x'", "superuser nahi hai", engine=restricted)
        self.assertFails("SHURU", "superuser nahi hai", engine=restricted)

    def test_granting_a_view_lets_restricted_user_read_it_without_table_grants(self):
        self.run_sql("BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT, umar INT)")
        self.run_sql("DAALO MEIN students MAAN (1, 'Ravi', 20), (2, 'Simran', 17)")
        self.run_sql("BANAO VIEW adults KAHO DIKHAO naam SE students JAHAN umar >= 18")
        self.run_sql("BANAO USER ravi GUPT 'x'")
        self.run_sql("ADHIKAR DO DIKHAO PAR adults KO ravi")

        restricted = Engine(self.db.instance)
        restricted.user = "ravi"
        self.assertEqual(self.rows("DIKHAO * SE adults", restricted), [["Ravi"]])
        # no DIRECT grant on students -- only via the view
        self.assertFails("DIKHAO * SE students", "adhikar nahi hai", engine=restricted)


# ============================================================================
# Triggers
# ============================================================================


class TriggersTestCase(EngineTestCase):
    def setUp(self):
        super().setUp()
        self.run_sql("BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT, umar INT)")
        self.run_sql("BANAO TABLE log_table (naam TEXT MUKHYA KUNJI, events INT)")
        self.run_sql("DAALO MEIN log_table MAAN ('insert_count', 0)")

    def test_after_insert_trigger_fires_with_naya(self):
        self.run_sql(
            "BANAO TRIGGER t1 BAAD DAALO PAR students SHURU "
            "BADLO log_table RAKHO events = events + 1 JAHAN naam = 'insert_count'; "
            "KHATAM"
        )
        self.run_sql("DAALO MEIN students MAAN (1, 'Ravi', 20)")
        self.run_sql("DAALO MEIN students MAAN (2, 'Simran', 22)")
        self.assertEqual(self.rows("DIKHAO * SE log_table"), [["insert_count", 2]])

    def test_naya_and_purana_substitution_on_update(self):
        self.run_sql("BANAO TABLE audit (naam TEXT, old_umar INT, new_umar INT)")
        self.run_sql(
            "BANAO TRIGGER audit_umar BAAD BADLO PAR students SHURU "
            "DAALO MEIN audit MAAN (NAYA.naam, PURANA.umar, NAYA.umar); "
            "KHATAM"
        )
        self.run_sql("DAALO MEIN students MAAN (1, 'Ravi', 20)")
        self.run_sql("BADLO students RAKHO umar = umar + 1 JAHAN id = 1")
        self.assertEqual(self.rows("DIKHAO * SE audit"), [["Ravi", 20, 21]])

    def test_purana_only_on_delete(self):
        self.run_sql("BANAO TABLE deleted_log (naam TEXT)")
        self.run_sql(
            "BANAO TRIGGER dl BAAD MITAO PAR students SHURU "
            "DAALO MEIN deleted_log MAAN (PURANA.naam); "
            "KHATAM"
        )
        self.run_sql("DAALO MEIN students MAAN (1, 'Ravi', 20)")
        self.run_sql("MITAO SE students JAHAN id = 1")
        self.assertEqual(self.rows("DIKHAO * SE deleted_log"), [["Ravi"]])

    def test_pehle_trigger_aborting_aborts_outer_statement(self):
        self.run_sql(
            "BANAO TRIGGER guard PEHLE DAALO PAR students SHURU "
            "BADLO log_table RAKHO events = events / 0 JAHAN naam = 'insert_count'; "
            "KHATAM"
        )
        self.assertFails("DAALO MEIN students MAAN (1, 'Ravi', 20)")
        # the outer INSERT itself never committed
        self.assertEqual(self.rows("DIKHAO * SE students"), [])

    def test_multiple_triggers_fire_in_creation_order(self):
        self.run_sql("BANAO TABLE trace (step TEXT)")
        self.run_sql(
            "BANAO TRIGGER first_t BAAD DAALO PAR students SHURU "
            "DAALO MEIN trace MAAN ('first'); "
            "KHATAM"
        )
        self.run_sql(
            "BANAO TRIGGER second_t BAAD DAALO PAR students SHURU "
            "DAALO MEIN trace MAAN ('second'); "
            "KHATAM"
        )
        self.run_sql("DAALO MEIN students MAAN (1, 'Ravi', 20)")
        self.assertEqual(self.rows("DIKHAO * SE trace"), [["first"], ["second"]])

    def test_drop_trigger(self):
        self.run_sql(
            "BANAO TRIGGER t1 BAAD DAALO PAR students SHURU "
            "BADLO log_table RAKHO events = events + 1 JAHAN naam = 'insert_count'; "
            "KHATAM"
        )
        self.run_sql("HATAO TRIGGER t1")
        self.run_sql("DAALO MEIN students MAAN (1, 'Ravi', 20)")
        self.assertEqual(self.rows("DIKHAO * SE log_table"), [["insert_count", 0]])
        self.assertFails("HATAO TRIGGER t1", "exist nahi karta")

    def test_duplicate_trigger_name_fails(self):
        sql = (
            "BANAO TRIGGER t1 BAAD DAALO PAR students SHURU "
            "BADLO log_table RAKHO events = events + 1 JAHAN naam = 'insert_count'; "
            "KHATAM"
        )
        self.run_sql(sql)
        self.assertFails(sql, "pehle se hai")

    def test_trigger_on_unknown_table_fails(self):
        self.assertFails(
            "BANAO TRIGGER bad PEHLE DAALO PAR ghost SHURU DAALO MEIN log_table MAAN ('x', 1); KHATAM",
            "exist nahi karta",
        )

    def test_persistence_across_restart(self):
        self.run_sql(
            "BANAO TRIGGER t1 BAAD DAALO PAR students SHURU "
            "BADLO log_table RAKHO events = events + 1 JAHAN naam = 'insert_count'; "
            "KHATAM"
        )
        self.reopen()
        self.run_sql("DAALO MEIN students MAAN (1, 'Ravi', 20)")
        self.assertEqual(self.rows("DIKHAO * SE log_table"), [["insert_count", 1]])


# ============================================================================
# Stored procedures
# ============================================================================


class ProceduresTestCase(EngineTestCase):
    def setUp(self):
        super().setUp()
        self.run_sql("BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT, umar INT)")
        self.run_sql("DAALO MEIN students MAAN (1, 'Ravi', 20)")

    def test_call_with_type_coercion(self):
        self.run_sql(
            "BANAO PROCEDURE badhao_umar (p_id INT, p_kitna FLOAT) SHURU "
            "BADLO students RAKHO umar = umar + p_kitna JAHAN id = p_id; "
            "KHATAM"
        )
        # p_kitna is FLOAT; passing the INT literal 5 must coerce cleanly
        self.run_sql("CHALAO badhao_umar(1, 5)")
        self.assertEqual(self.rows("DIKHAO umar SE students"), [[25]])

    def test_call_type_mismatch_gives_clean_error(self):
        self.run_sql(
            "BANAO PROCEDURE badhao_umar (p_id INT, p_kitna INT) SHURU "
            "BADLO students RAKHO umar = umar + p_kitna JAHAN id = p_id; "
            "KHATAM"
        )
        self.assertFails("CHALAO badhao_umar(1, 'oops')", "type ka hai")

    def test_wrong_arg_count_fails(self):
        self.run_sql(
            "BANAO PROCEDURE badhao_umar (p_id INT, p_kitna INT) SHURU "
            "BADLO students RAKHO umar = umar + p_kitna JAHAN id = p_id; "
            "KHATAM"
        )
        self.assertFails("CHALAO badhao_umar(1)", "argument")

    def test_unknown_procedure_fails(self):
        self.assertFails("CHALAO ghost(1, 2)", "exist nahi karta")

    def test_drop_procedure(self):
        self.run_sql(
            "BANAO PROCEDURE badhao_umar (p_id INT, p_kitna INT) SHURU "
            "BADLO students RAKHO umar = umar + p_kitna JAHAN id = p_id; "
            "KHATAM"
        )
        self.run_sql("HATAO PROCEDURE badhao_umar")
        self.assertFails("CHALAO badhao_umar(1, 5)", "exist nahi karta")
        self.assertFails("HATAO PROCEDURE badhao_umar", "exist nahi karta")

    def test_persistence_across_restart(self):
        self.run_sql(
            "BANAO PROCEDURE badhao_umar (p_id INT, p_kitna INT) SHURU "
            "BADLO students RAKHO umar = umar + p_kitna JAHAN id = p_id; "
            "KHATAM"
        )
        self.reopen()
        self.run_sql("CHALAO badhao_umar(1, 5)")
        self.assertEqual(self.rows("DIKHAO umar SE students"), [[25]])

    def test_multi_statement_procedure(self):
        self.run_sql("BANAO TABLE log_table (msg TEXT)")
        self.run_sql(
            "BANAO PROCEDURE bump (p_id INT, p_kitna INT) SHURU "
            "BADLO students RAKHO umar = umar + p_kitna JAHAN id = p_id; "
            "DAALO MEIN log_table MAAN ('bumped'); "
            "KHATAM"
        )
        self.run_sql("CHALAO bump(1, 3)")
        self.assertEqual(self.rows("DIKHAO umar SE students"), [[23]])
        self.assertEqual(self.rows("DIKHAO * SE log_table"), [["bumped"]])


# ============================================================================
# Client/server: an authenticated restricted user, over real TCP
# ============================================================================


class UsersOverTheWireTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.server = MeraDBServer(self.dir, "127.0.0.1", 0, log=lambda line: None)
        self.server.instance.lock_timeout = 0.5
        self.thread = threading.Thread(target=self.server.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        self.thread.start()
        self.connections = []

        admin = self.connect()
        admin.execute("BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT)")
        admin.execute("DAALO MEIN students MAAN (1, 'Ravi')")
        admin.execute("BANAO USER ravi GUPT 'secret123'")
        admin.execute("ADHIKAR DO DIKHAO PAR students KO ravi")

    def tearDown(self):
        for c in self.connections:
            c.close()
        self.server.shutdown()
        self.server.server_close()
        shutil.rmtree(self.dir, ignore_errors=True)

    def connect(self, **kwargs) -> Connection:
        conn = Connection("127.0.0.1", self.server.port, **kwargs)
        self.connections.append(conn)
        return conn

    def test_wrong_password_refused(self):
        with self.assertRaises(Exception):
            self.connect(user="ravi", password="wrong")

    def test_unknown_user_refused(self):
        with self.assertRaises(Exception):
            self.connect(user="ghost", password="whatever")

    def test_authenticated_user_can_do_what_is_granted(self):
        conn = self.connect(user="ravi", password="secret123")
        result = conn.execute("DIKHAO * SE students")[0]
        self.assertEqual(result.rows, [[1, "Ravi"]])

    def test_authenticated_user_cannot_do_what_is_not_granted(self):
        conn = self.connect(user="ravi", password="secret123")
        results = conn.run_script("DAALO MEIN students MAAN (2, 'Simran')")
        self.assertTrue(results[0].error)
        self.assertIn("adhikar nahi hai", results[0].error)

    def test_authenticated_non_superuser_cannot_run_ddl(self):
        conn = self.connect(user="ravi", password="secret123")
        results = conn.run_script("BANAO TABLE t2 (a INT)")
        self.assertTrue(results[0].error)
        self.assertIn("superuser nahi hai", results[0].error)

    def test_no_username_connection_is_still_unrestricted(self):
        conn = self.connect()  # no user= at all -- exactly today's behaviour
        conn.execute("DAALO MEIN students MAAN (3, 'Anjali')")
        result = conn.execute("DIKHAO * SE students")[0]
        self.assertEqual(len(result.rows), 2)


if __name__ == "__main__":
    unittest.main()
