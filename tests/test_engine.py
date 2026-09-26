"""End-to-end tests: text in, results out, real files on disk (in a temp folder)."""

import shutil
import tempfile
import unittest

from meradb.engine import Engine
from meradb.errors import ExecutionError


class EngineTestCase(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.db = Engine(self.dir)
        self.run_sql(
            """
            BANAO TABLE s (id INT MUKHYA KUNJI, naam TEXT ZAROORI, umar INT, email TEXT ANOKHA);
            DAALO MEIN s MAAN (1, 'Ravi', 20, 'r@x.in'), (2, 'Priya', 19, KHALI), (3, 'Aman', KHALI, KHALI);
            """
        )

    def tearDown(self):
        shutil.rmtree(self.dir)

    def run_sql(self, text):
        return self.db.execute(text)[-1]

    def rows(self, text):
        return self.run_sql(text).rows


class DDLTest(EngineTestCase):
    def test_show_and_describe(self):
        self.assertEqual(self.rows("DIKHAO TABLES"), [["s"]])
        self.assertEqual(self.rows("BATAO s")[0], ["id", "INT", "MUKHYA KUNJI"])

    def test_create_duplicate_table_fails(self):
        with self.assertRaises(ExecutionError):
            self.run_sql("BANAO TABLE s (x INT)")

    def test_drop_table(self):
        self.run_sql("HATAO TABLE s")
        self.assertEqual(self.rows("DIKHAO TABLES"), [])
        with self.assertRaises(ExecutionError):
            self.run_sql("DIKHAO * SE s")

    def test_truncate(self):
        self.run_sql("SAAF TABLE s")
        self.assertEqual(self.rows("DIKHAO * SE s"), [])

    def test_alter_add_and_drop_column(self):
        self.run_sql("SUDHARO TABLE s JODO shehar TEXT")
        self.assertEqual(self.rows("DIKHAO shehar SE s JAHAN id = 1"), [[None]])
        self.run_sql("SUDHARO TABLE s HATAO umar")
        self.assertEqual(self.rows("DIKHAO * SE s JAHAN id = 1"), [[1, "Ravi", "r@x.in", None]])

    def test_alter_add_required_column_to_nonempty_table_fails(self):
        with self.assertRaises(ExecutionError):
            self.run_sql("SUDHARO TABLE s JODO x INT ZAROORI")

    def test_databases(self):
        self.run_sql("BANAO DATABASE college; ISTEMAL college")
        self.assertEqual(self.rows("DIKHAO TABLES"), [])
        self.run_sql("ISTEMAL main")
        self.assertEqual(self.rows("DIKHAO TABLES"), [["s"]])
        self.run_sql("HATAO DATABASE college")
        with self.assertRaises(ExecutionError):
            self.run_sql("ISTEMAL college")


class DMLTest(EngineTestCase):
    def test_select_star(self):
        self.assertEqual(len(self.rows("DIKHAO * SE s")), 3)

    def test_where_and_order_and_limit(self):
        self.assertEqual(self.rows("DIKHAO naam SE s KRAM naam SIRF 2"), [["Aman"], ["Priya"]])
        self.assertEqual(self.rows("DIKHAO naam SE s JAHAN umar >= 20"), [["Ravi"]])

    def test_null_logic(self):
        # Aman's umar is KHALI: `umar < 100` is unknown, so the row is NOT returned
        self.assertEqual(len(self.rows("DIKHAO * SE s JAHAN umar < 100")), 2)
        self.assertEqual(self.rows("DIKHAO naam SE s JAHAN umar HAI KHALI"), [["Aman"]])

    def test_expressions(self):
        self.assertEqual(self.rows("DIKHAO umar * 2 + 1 SE s JAHAN id = 1"), [[41]])
        self.assertEqual(self.rows("DIKHAO naam + '!' SE s JAHAN id = 2"), [["Priya!"]])

    def test_update(self):
        self.assertEqual(self.run_sql("BADLO s RAKHO umar = umar + 1 JAHAN umar HAI NAHI KHALI").message[0], "2")
        self.assertEqual(self.rows("DIKHAO umar SE s KRAM id"), [[21], [20], [None]])

    def test_update_does_not_touch_rows_twice(self):
        # Halloween problem guard: every row is updated exactly once
        self.run_sql("BADLO s RAKHO id = id + 10")
        self.assertEqual(self.rows("DIKHAO id SE s KRAM id"), [[11], [12], [13]])

    def test_delete(self):
        self.run_sql("MITAO SE s JAHAN id = 2")
        self.assertEqual(self.rows("DIKHAO id SE s KRAM id"), [[1], [3]])
        self.run_sql("MITAO SE s")
        self.assertEqual(self.rows("DIKHAO * SE s"), [])

    def test_data_survives_restart(self):
        self.run_sql("MITAO SE s JAHAN id = 2")
        fresh = Engine(self.dir)  # a brand new engine reading the same files
        self.assertEqual(fresh.execute("DIKHAO id SE s KRAM id")[0].rows, [[1], [3]])


class ConstraintTest(EngineTestCase):
    def assertFails(self, sql):
        with self.assertRaises(ExecutionError):
            self.run_sql(sql)

    def test_primary_key_unique(self):
        self.assertFails("DAALO MEIN s (id, naam) MAAN (1, 'Dup')")

    def test_primary_key_not_null(self):
        self.assertFails("DAALO MEIN s (id, naam) MAAN (KHALI, 'X')")

    def test_not_null(self):
        self.assertFails("DAALO MEIN s (id) MAAN (9)")

    def test_unique_allows_many_nulls(self):
        self.run_sql("DAALO MEIN s (id, naam) MAAN (9, 'X'), (10, 'Y')")

    def test_unique_within_same_insert(self):
        self.assertFails("DAALO MEIN s (id, naam) MAAN (9, 'X'), (9, 'Y')")

    def test_failed_insert_writes_nothing(self):
        self.assertFails("DAALO MEIN s (id, naam) MAAN (9, 'X'), (1, 'Dup')")
        self.assertEqual(self.rows("DIKHAO * SE s JAHAN id = 9"), [])

    def test_update_cannot_create_duplicate(self):
        self.assertFails("BADLO s RAKHO id = 1 JAHAN id = 2")

    def test_type_checks(self):
        self.assertFails("DAALO MEIN s (id, naam) MAAN ('abc', 'X')")
        self.assertFails("DAALO MEIN s (id, naam) MAAN (9, 42)")
        self.assertFails("DIKHAO * SE s JAHAN naam > 5")

    def test_unknown_column(self):
        self.assertFails("DIKHAO nope SE s")
        self.assertFails("DAALO MEIN s (id, nope) MAAN (9, 1)")


if __name__ == "__main__":
    unittest.main()
