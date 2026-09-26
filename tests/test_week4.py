"""Week 4 stretch features: JOINs (MILAO), hash indexes, transactions (SHURU/PAKKA/WAPAS), SAMJHAO."""

import os
import shutil
import tempfile
import threading
import unittest

from meradb import ast_nodes as ast
from meradb.engine import Engine, Instance
from meradb.errors import ExecutionError


class Week4TestCase(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.db = Engine(self.dir)
        self.db.execute(
            """
            BANAO TABLE courses (id INT MUKHYA KUNJI, title TEXT ANOKHA);
            BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT, cid INT);
            DAALO MEIN courses MAAN (10, 'DBMS'), (20, 'OS'), (30, 'CN');
            DAALO MEIN students MAAN (1, 'Ravi', 10), (2, 'Priya', 20), (3, 'Aman', 10), (4, 'Sneha', KHALI);
            """
        )

    def tearDown(self):
        self.db.close()
        shutil.rmtree(self.dir, ignore_errors=True)

    def result(self, text, engine=None):
        return (engine or self.db).execute(text)[-1]

    def rows(self, text, engine=None):
        return self.result(text, engine).rows

    def plan(self, text):
        return "\n".join(r[0] for r in self.rows("SAMJHAO " + text))

    def assertFails(self, sql, engine=None):
        with self.assertRaises(ExecutionError):
            (engine or self.db).execute(sql)


class JoinTest(Week4TestCase):
    def test_inner_join(self):
        self.assertEqual(
            self.rows("DIKHAO s.naam, c.title SE students s MILAO courses c PAR s.cid = c.id KRAM s.id"),
            [["Ravi", "DBMS"], ["Priya", "OS"], ["Aman", "DBMS"]],
        )

    def test_left_join_keeps_unmatched_rows(self):
        self.assertEqual(
            self.rows("DIKHAO s.naam, c.title SE students s BAAYAN MILAO courses c PAR s.cid = c.id KRAM s.id"),
            [["Ravi", "DBMS"], ["Priya", "OS"], ["Aman", "DBMS"], ["Sneha", None]],
        )

    def test_join_without_aliases_and_unambiguous_names(self):
        self.assertEqual(
            self.rows("DIKHAO naam, title SE students MILAO courses PAR cid = courses.id JAHAN title = 'OS'"),
            [["Priya", "OS"]],
        )

    def test_star_labels_disambiguate_clashing_columns(self):
        result = self.result("DIKHAO * SE students s MILAO courses c PAR s.cid = c.id")
        self.assertEqual(result.columns, ["s.id", "naam", "cid", "c.id", "title"])
        self.assertEqual(self.result("DIKHAO c.* SE students s MILAO courses c PAR s.cid = c.id").columns,
                         ["c.id", "title"])

    def test_join_with_group_by(self):
        self.assertEqual(
            self.rows("DIKHAO c.title, GINO(s.id) SE courses c BAAYAN MILAO students s PAR s.cid = c.id "
                      "SAMOOH c.title KRAM c.title"),
            [["CN", 0], ["DBMS", 2], ["OS", 1]],
        )

    def test_non_equi_join_uses_nested_loop(self):
        # Ravi(10) < 20, 30 | Priya(20) < 30 | Aman(10) < 20, 30 | Sneha(KHALI) never matches
        self.assertEqual(len(self.rows("DIKHAO s.id SE students s MILAO courses c PAR s.cid < c.id")), 5)
        self.assertIn("NESTED LOOP JOIN", self.plan("DIKHAO * SE students s MILAO courses c PAR s.cid < c.id"))
        self.assertIn("HASH JOIN", self.plan("DIKHAO * SE students s MILAO courses c PAR s.cid = c.id"))

    def test_three_way_self_join(self):
        rows = self.rows("DIKHAO a.naam, b.naam SE students a MILAO students b PAR a.cid = b.cid "
                         "JAHAN a.id < b.id")
        self.assertEqual(rows, [["Ravi", "Aman"]])

    def test_join_errors(self):
        self.assertFails("DIKHAO id SE students MILAO courses PAR cid = courses.id")  # ambiguous id
        self.assertFails("DIKHAO * SE students MILAO students PAR cid = cid")  # same alias twice
        self.assertFails("DIKHAO x.naam SE students s")  # unknown alias
        self.assertFails("DIKHAO * SE students s MILAO courses c PAR s.cid = z.id")


class IndexTest(Week4TestCase):
    def test_equality_on_primary_key_uses_index(self):
        self.assertIn("INDEX LOOKUP students PAR id = 2", self.plan("DIKHAO * SE students JAHAN id = 2"))
        self.assertIn("INDEX LOOKUP", self.plan("DIKHAO * SE students JAHAN naam = 'x' AUR 2 = id"))
        self.assertIn("INDEX LOOKUP courses PAR title = 'OS'", self.plan("DIKHAO * SE courses JAHAN title = 'OS'"))

    def test_other_conditions_use_full_scan(self):
        for where in ("id > 2", "id = 1 YA id = 2", "naam = 'Ravi'", "id = '2'"):
            self.assertIn("FULL SCAN", self.plan(f"DIKHAO * SE students JAHAN {where}"), where)

    def test_index_answers_match_scan(self):
        self.assertEqual(self.rows("DIKHAO naam SE students JAHAN id = 3"), [["Aman"]])
        self.assertEqual(self.rows("DIKHAO naam SE students JAHAN id = 3 AUR naam = 'x'"), [])
        self.assertEqual(self.rows("DIKHAO naam SE students JAHAN id = 99"), [])

    def test_index_stays_correct_after_writes(self):
        self.rows("DIKHAO * SE students JAHAN id = 1")  # builds the index
        self.db.execute("BADLO students RAKHO id = 100 JAHAN id = 1")
        self.assertEqual(self.rows("DIKHAO naam SE students JAHAN id = 100"), [["Ravi"]])
        self.assertEqual(self.rows("DIKHAO naam SE students JAHAN id = 1"), [])
        self.db.execute("MITAO SE students JAHAN id = 100")
        self.assertEqual(self.rows("DIKHAO naam SE students JAHAN id = 100"), [])
        self.db.execute("DAALO MEIN students MAAN (100, 'New', KHALI)")
        self.assertEqual(self.rows("DIKHAO naam SE students JAHAN id = 100"), [["New"]])

    def test_index_rebuilt_after_file_rewrites(self):
        self.rows("DIKHAO * SE students JAHAN id = 1")
        self.db.execute("MITAO SE students JAHAN id = 1; SIKODO TABLE students")  # row ids change
        self.assertEqual(self.rows("DIKHAO naam SE students JAHAN id = 2"), [["Priya"]])
        self.db.execute("SUDHARO TABLE students HATAO cid")
        self.assertEqual(self.rows("DIKHAO naam SE students JAHAN id = 3"), [["Aman"]])

    def test_swapping_unique_values_in_one_update(self):
        self.db.execute("BADLO students RAKHO id = 3 - id JAHAN id MEIN (1, 2)")
        self.assertEqual(self.rows("DIKHAO naam SE students JAHAN id = 1"), [["Priya"]])
        self.assertEqual(self.rows("DIKHAO naam SE students JAHAN id = 2"), [["Ravi"]])

    def test_uniqueness_still_enforced(self):
        self.assertFails("DAALO MEIN students MAAN (1, 'Dup', KHALI)")
        self.assertFails("BADLO courses RAKHO title = 'OS' JAHAN id = 10")

    def test_update_and_delete_use_index(self):
        self.assertIn("INDEX LOOKUP", self.plan("MITAO SE students JAHAN id = 1"))
        self.assertIn("INDEX LOOKUP", self.plan("BADLO students RAKHO naam = 'x' JAHAN id = 1"))


class TransactionTest(Week4TestCase):
    def test_rollback_undoes_everything(self):
        self.db.execute("SHURU")
        self.assertTrue(self.db.in_transaction)
        self.db.execute("""
            DAALO MEIN students MAAN (9, 'Temp', KHALI);
            MITAO SE courses;
            HATAO TABLE students;
            BANAO TABLE junk (x INT);
        """)
        self.db.execute("WAPAS")
        self.assertFalse(self.db.in_transaction)
        self.assertEqual(self.rows("DIKHAO GINO(*) SE students"), [[4]])
        self.assertEqual(self.rows("DIKHAO GINO(*) SE courses"), [[3]])
        self.assertEqual(self.rows("DIKHAO TABLES"), [["courses"], ["students"]])
        self.assertEqual(self.rows("DIKHAO naam SE students JAHAN id = 9"), [])  # index rebuilt too

    def test_commit_keeps_changes(self):
        self.db.execute("SHURU; DAALO MEIN students MAAN (9, 'Kept', KHALI); PAKKA")
        self.assertEqual(self.rows("DIKHAO naam SE students JAHAN id = 9"), [["Kept"]])
        self.assertFalse(os.listdir(os.path.join(self.dir, ".wapas")))  # snapshot cleaned up

    def test_close_rolls_back_open_transaction(self):
        self.db.execute("SHURU; MITAO SE students")
        self.db.close()
        self.assertEqual(self.rows("DIKHAO GINO(*) SE students", Engine(self.dir)), [[4]])

    def test_transaction_errors(self):
        self.assertFails("PAKKA")
        self.assertFails("WAPAS")
        self.db.execute("SHURU")
        self.assertFails("SHURU")
        self.assertFails("ISTEMAL main")
        self.assertFails("BANAO DATABASE x")
        self.db.execute("WAPAS")

    def test_crash_recovery(self):
        """Simulate a crash mid-transaction: the process dies without PAKKA/WAPAS."""
        self.db.execute("SHURU; MITAO SE students")
        self.db.instance.lock.release()  # the "dead" process's lock and session are gone too
        self.db.txn_db = None
        # "restart": a brand-new Instance over the same folder runs recovery
        restarted = Engine(self.dir)
        self.assertEqual(restarted.instance.recovered, ["main"])
        self.assertEqual(self.rows("DIKHAO GINO(*) SE students", restarted), [[4]])

    def test_other_sessions_wait_for_a_transaction(self):
        instance = Instance(tempfile.mkdtemp())
        instance.lock_timeout = 0.3
        alice, bob = Engine(instance), Engine(instance)
        alice.execute("BANAO TABLE t (x INT); SHURU; DAALO MEIN t MAAN (1)")

        outcome = {}

        def bob_reads():
            try:
                outcome["rows"] = bob.execute("DIKHAO * SE t")[0].rows
            except ExecutionError as e:
                outcome["error"] = str(e)

        thread = threading.Thread(target=bob_reads)  # a different thread = a different client
        thread.start()
        thread.join()
        self.assertIn("busy", outcome.get("error", ""))  # Bob could not see Alice's uncommitted row

        alice.execute("PAKKA")
        thread = threading.Thread(target=bob_reads)
        thread.start()
        thread.join()
        self.assertEqual(outcome.get("rows"), [[1]])
        shutil.rmtree(instance.data_dir, ignore_errors=True)


class ExplainTest(Week4TestCase):
    def test_explain_does_not_run_the_query(self):
        self.db.execute("SAMJHAO MITAO SE students")
        self.assertEqual(self.rows("DIKHAO GINO(*) SE students"), [[4]])

    def test_explain_shows_pipeline(self):
        plan = self.plan("DIKHAO ALAG c.title, GINO(*) SE students s MILAO courses c PAR s.cid = c.id "
                         "JAHAN s.id > 0 SAMOOH c.title JINKA GINO(*) > 0 KRAM c.title ULTA SIRF 2")
        for step in ("FULL SCAN students s", "HASH JOIN", "FILTER  JAHAN", "GROUP  SAMOOH", "FILTER GROUPS",
                     "SORT", "PROJECT", "DISTINCT", "LIMIT"):
            self.assertIn(step, plan)

    def test_explain_rejects_ddl(self):
        self.assertFails("SAMJHAO HATAO TABLE students")


class MiscTest(unittest.TestCase):
    def test_huge_int_is_a_clean_error(self):
        db = Engine(tempfile.mkdtemp())
        db.execute("BANAO TABLE t (x INT)")
        with self.assertRaises(ExecutionError):
            db.execute("DAALO MEIN t MAAN (99999999999999999999)")
        shutil.rmtree(db.instance.data_dir)

    def test_database_dropped_by_another_session(self):
        instance = Instance(tempfile.mkdtemp())
        a, b = Engine(instance), Engine(instance)
        a.execute("BANAO DATABASE x")
        b.execute("ISTEMAL x")
        a.execute("HATAO DATABASE x")
        with self.assertRaises(ExecutionError):
            b.execute("DIKHAO TABLES")
        self.assertEqual(b.current_db, "main")
        b.execute_statement(ast.ShowTables())
        shutil.rmtree(instance.data_dir)


if __name__ == "__main__":
    unittest.main()
