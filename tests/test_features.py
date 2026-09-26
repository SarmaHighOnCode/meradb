"""Tests for the roadmap features: JAISA, BEECH, MEIN, WARNA, SIKODO, aggregates, SAMOOH, JINKA, ALAG."""

import os
import shutil
import tempfile
import unittest

from meradb.datatypes import format_value
from meradb.engine import Engine
from meradb.errors import ExecutionError, ParseError


class FeatureTestCase(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.db = Engine(self.dir)
        self.db.execute(
            """
            BANAO TABLE s (id INT MUKHYA KUNJI, naam TEXT, shehar TEXT WARNA 'Delhi', cgpa FLOAT);
            DAALO MEIN s (id, naam, cgpa) MAAN (1, 'Ravi', 8.0), (2, 'Rohit', 7.0), (3, 'Priya', 9.0);
            DAALO MEIN s MAAN (4, 'Sneha', 'Pune', 8.0), (5, 'Aman', 'Pune', KHALI);
            """
        )

    def tearDown(self):
        shutil.rmtree(self.dir)

    def rows(self, text):
        return self.db.execute(text)[-1].rows

    def assertFails(self, sql, error=ExecutionError):
        with self.assertRaises(error):
            self.db.execute(sql)


class Week1PredicatesTest(FeatureTestCase):
    def test_jaisa(self):
        self.assertEqual(self.rows("DIKHAO id SE s JAHAN naam JAISA 'r%' KRAM id"), [[1], [2]])
        self.assertEqual(self.rows("DIKHAO id SE s JAHAN naam JAISA '_man'"), [[5]])
        self.assertEqual(self.rows("DIKHAO id SE s JAHAN naam NAHI JAISA '%a%' KRAM id"), [[2]])

    def test_jaisa_escapes_regex_characters(self):
        self.db.execute("DAALO MEIN s (id, naam) MAAN (6, 'a.b')")
        self.assertEqual(self.rows("DIKHAO id SE s JAHAN naam JAISA 'a.b'"), [[6]])
        self.assertEqual(self.rows("DIKHAO id SE s JAHAN naam JAISA 'r.vi'"), [])

    def test_beech(self):
        self.assertEqual(self.rows("DIKHAO id SE s JAHAN cgpa BEECH 7.5 AUR 8.5 KRAM id"), [[1], [4]])
        self.assertEqual(self.rows("DIKHAO id SE s JAHAN cgpa NAHI BEECH 7.5 AUR 8.5 KRAM id"), [[2], [3]])

    def test_beech_combined_with_aur(self):
        self.assertEqual(self.rows("DIKHAO id SE s JAHAN cgpa BEECH 7 AUR 9 AUR shehar = 'Pune'"), [[4]])

    def test_mein(self):
        self.assertEqual(self.rows("DIKHAO id SE s JAHAN id MEIN (1, 3, 99) KRAM id"), [[1], [3]])
        self.assertEqual(self.rows("DIKHAO id SE s JAHAN id NAHI MEIN (1, 2, 3) KRAM id"), [[4], [5]])


class Week2DefaultAndCompactTest(FeatureTestCase):
    def test_warna_used_when_column_left_out(self):
        self.assertEqual(self.rows("DIKHAO shehar SE s JAHAN id = 1"), [["Delhi"]])

    def test_explicit_khali_overrides_warna(self):
        self.db.execute("DAALO MEIN s (id, shehar) MAAN (9, KHALI)")
        self.assertEqual(self.rows("DIKHAO shehar SE s JAHAN id = 9"), [[None]])

    def test_warna_survives_restart(self):
        fresh = Engine(self.dir)
        fresh.execute("DAALO MEIN s (id) MAAN (9)")
        self.assertEqual(fresh.execute("DIKHAO shehar SE s JAHAN id = 9")[0].rows, [["Delhi"]])

    def test_warna_type_checked_at_create(self):
        self.assertFails("BANAO TABLE t (x INT WARNA 'abc')")
        self.assertFails("BANAO TABLE t (x INT WARNA naam)", ParseError)

    def test_negative_warna(self):
        self.db.execute("BANAO TABLE t (x INT WARNA -5, y INT); DAALO MEIN t (y) MAAN (1)")
        self.assertEqual(self.rows("DIKHAO x SE t"), [[-5]])

    def test_alter_add_column_with_warna_fills_existing_rows(self):
        self.db.execute("SUDHARO TABLE s JODO saal INT ZAROORI WARNA 1")
        self.assertEqual(self.rows("DIKHAO ALAG saal SE s"), [[1]])

    def test_alter_add_unique_warna_on_many_rows_fails(self):
        self.assertFails("SUDHARO TABLE s JODO code INT ANOKHA WARNA 7")

    def test_sikodo_shrinks_file(self):
        path = os.path.join(self.dir, "main", "s.tbl")
        self.db.execute("MITAO SE s JAHAN id < 5")
        before = os.path.getsize(path)
        self.db.execute("SIKODO TABLE s")
        self.assertLess(os.path.getsize(path), before)
        self.assertEqual(self.rows("DIKHAO id SE s"), [[5]])


class Week3AggregateTest(FeatureTestCase):
    def test_count_sum_avg_min_max(self):
        self.assertEqual(
            self.rows("DIKHAO GINO(*), GINO(cgpa), KUL(cgpa), AUSAT(cgpa), NYUNTAM(cgpa), ADHIKTAM(naam) SE s"),
            [[5, 4, 32.0, 8.0, 7.0, "Sneha"]],
        )

    def test_english_aliases(self):
        self.assertEqual(self.rows("DIKHAO COUNT(*), max(id) SE s"), [[5, 5]])

    def test_aggregate_on_empty_result(self):
        self.assertEqual(self.rows("DIKHAO GINO(*), KUL(cgpa) SE s JAHAN id > 100"), [[0, None]])

    def test_group_by(self):
        self.assertEqual(
            self.rows("DIKHAO shehar, GINO(*), AUSAT(cgpa) SE s SAMOOH shehar KRAM shehar"),
            [["Delhi", 3, 8.0], ["Pune", 2, 8.0]],
        )

    def test_group_by_with_where(self):
        self.assertEqual(self.rows("DIKHAO shehar, GINO(*) SE s JAHAN id > 2 SAMOOH shehar KRAM shehar"),
                         [["Delhi", 1], ["Pune", 2]])

    def test_group_by_on_empty_table_returns_nothing(self):
        self.assertEqual(self.rows("DIKHAO shehar, GINO(*) SE s JAHAN id > 100 SAMOOH shehar"), [])

    def test_having(self):
        self.assertEqual(self.rows("DIKHAO shehar SE s SAMOOH shehar JINKA GINO(*) > 2"), [["Delhi"]])

    def test_order_by_aggregate(self):
        self.assertEqual(self.rows("DIKHAO shehar SE s SAMOOH shehar KRAM GINO(*)"), [["Pune"], ["Delhi"]])

    def test_expression_of_aggregates(self):
        self.assertEqual(self.rows("DIKHAO KUL(cgpa) / GINO(cgpa) SE s"), [[8.0]])

    def test_distinct(self):
        self.assertEqual(self.rows("DIKHAO ALAG shehar SE s KRAM shehar"), [["Delhi"], ["Pune"]])
        self.assertEqual(self.rows("DIKHAO ALAG shehar SE s KRAM shehar SIRF 1"), [["Delhi"]])

    def test_grouping_errors(self):
        self.assertFails("DIKHAO naam, GINO(*) SE s")  # naam not grouped
        self.assertFails("DIKHAO naam SE s SAMOOH shehar")
        self.assertFails("DIKHAO * SE s JAHAN GINO(*) > 1")  # aggregate in WHERE
        self.assertFails("BADLO s RAKHO cgpa = AUSAT(cgpa)")
        self.assertFails("DIKHAO BLAH(id) SE s")
        self.assertFails("DIKHAO KUL(*) SE s")
        self.assertFails("DIKHAO KUL(naam) SE s")


class FormatTest(unittest.TestCase):
    def test_float_formatting(self):
        self.assertEqual(format_value(8.166666666666666), "8.166666667")
        self.assertEqual(format_value(7.0), "7.0")
        self.assertEqual(format_value(1e20), "1e+20")


if __name__ == "__main__":
    unittest.main()
