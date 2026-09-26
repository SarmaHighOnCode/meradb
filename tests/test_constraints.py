"""
Tests for the six constraint/DDL features added on top of the core engine:

    SANDARBH  FOREIGN KEY            CHECK       SHART
    DATE / TAREEKH                   VARCHAR(n) / NUMBER(p,s)
    NAYA_NAAM (RENAME table/column)  KAHO (output column alias)

Each section below has its own TestCase, including error cases and, where the
feature touches catalog.json, persistence across a restart (a fresh Engine on
the same temp directory -- nothing here touches the real server data folder).
"""

import shutil
import tempfile
import unittest
from datetime import date

from meradb.engine import Engine
from meradb.errors import ExecutionError, ParseError


class ConstraintsTestCase(unittest.TestCase):
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
        """Simulate a restart: close this session, open a brand new Engine on the same folder."""
        self.db.close()
        self.db = Engine(self.dir)
        return self.db

    def assertFails(self, sql, message_fragment=None):
        with self.assertRaises(ExecutionError) as ctx:
            self.db.execute(sql)
        if message_fragment:
            self.assertIn(message_fragment, str(ctx.exception))


# ============================================================================
# 1. SANDARBH (FOREIGN KEY)
# ============================================================================


class ForeignKeyTest(ConstraintsTestCase):
    def setUp(self):
        super().setUp()
        self.run_sql(
            """
            BANAO TABLE courses (id INT MUKHYA KUNJI, title TEXT ANOKHA);
            BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT, cid INT SANDARBH courses(id));
            DAALO MEIN courses MAAN (10, 'DBMS'), (20, 'OS');
            """
        )

    def test_parent_must_exist(self):
        self.assertFails("BANAO TABLE t (x INT SANDARBH nahi_hai(id))", "exist nahi karta")

    def test_parent_column_must_exist(self):
        self.assertFails("BANAO TABLE t (x INT SANDARBH courses(nope))", "column 'nope' nahi hai")

    def test_parent_column_must_be_unique(self):
        self.run_sql("BANAO TABLE plain (a INT, b INT)")
        self.assertFails("BANAO TABLE t (x INT SANDARBH plain(b))", "MUKHYA KUNJI ya ANOKHA")

    def test_types_must_match(self):
        self.assertFails("BANAO TABLE t (x TEXT SANDARBH courses(id))", "types match nahi")

    def test_insert_valid_fk_value(self):
        self.run_sql("DAALO MEIN students (id, naam, cid) MAAN (1, 'Ravi', 10)")
        self.assertEqual(self.rows("DIKHAO cid SE students"), [[10]])

    def test_khali_fk_value_is_allowed(self):
        self.run_sql("DAALO MEIN students (id, naam) MAAN (1, 'Ravi')")
        self.assertEqual(self.rows("DIKHAO cid SE students"), [[None]])

    def test_insert_bad_fk_value_fails(self):
        self.assertFails("DAALO MEIN students (id, naam, cid) MAAN (1, 'Ravi', 999)", "SANDARBH")

    def test_update_bad_fk_value_fails(self):
        self.run_sql("DAALO MEIN students (id, naam, cid) MAAN (1, 'Ravi', 10)")
        self.assertFails("BADLO students RAKHO cid = 999 JAHAN id = 1", "SANDARBH")

    def test_delete_parent_restricted_by_child(self):
        self.run_sql("DAALO MEIN students (id, naam, cid) MAAN (1, 'Ravi', 10)")
        self.assertFails("MITAO SE courses JAHAN id = 10", "students")
        self.assertEqual(self.rows("DIKHAO * SE courses"), [[10, "DBMS"], [20, "OS"]])

    def test_delete_parent_allowed_once_child_gone(self):
        self.run_sql("DAALO MEIN students (id, naam, cid) MAAN (1, 'Ravi', 10)")
        self.run_sql("MITAO SE students JAHAN id = 1")
        self.run_sql("MITAO SE courses JAHAN id = 10")
        self.assertEqual(self.rows("DIKHAO * SE courses"), [[20, "OS"]])

    def test_update_parent_key_restricted_by_child(self):
        self.run_sql("DAALO MEIN students (id, naam, cid) MAAN (1, 'Ravi', 10)")
        self.assertFails("BADLO courses RAKHO id = 99 JAHAN id = 10", "students")

    def test_drop_parent_table_restricted(self):
        self.assertFails("HATAO TABLE courses", "students")

    def test_truncate_parent_restricted_by_live_child_value(self):
        self.run_sql("DAALO MEIN students (id, naam, cid) MAAN (1, 'Ravi', 10)")
        self.assertFails("SAAF TABLE courses", "SANDARBH")

    def test_truncate_parent_allowed_when_no_live_fk_values(self):
        self.run_sql("DAALO MEIN students (id, naam) MAAN (1, 'Ravi')")  # cid is KHALI
        self.run_sql("SAAF TABLE courses")  # no non-KHALI child value -> allowed
        self.assertEqual(self.rows("DIKHAO * SE courses"), [])

    def test_alter_drop_referenced_column_restricted(self):
        self.assertFails("SUDHARO TABLE courses HATAO id", "students.cid")

    def test_self_reference(self):
        self.run_sql("BANAO TABLE employee (id INT MUKHYA KUNJI, manager_id INT SANDARBH employee(id))")
        self.run_sql("DAALO MEIN employee (id, manager_id) MAAN (1, KHALI), (2, 1)")
        self.assertEqual(self.rows("DIKHAO * SE employee KRAM id"), [[1, None], [2, 1]])
        # both a manager and its report in the SAME statement must be accepted
        self.run_sql("DAALO MEIN employee (id, manager_id) MAAN (3, 4), (4, 1)")
        self.assertFails("DAALO MEIN employee (id, manager_id) MAAN (5, 999)")

    def test_self_reference_does_not_block_drop(self):
        self.run_sql("BANAO TABLE employee (id INT MUKHYA KUNJI, manager_id INT SANDARBH employee(id))")
        self.run_sql("DAALO MEIN employee (id, manager_id) MAAN (1, KHALI)")
        self.run_sql("HATAO TABLE employee")  # must NOT raise
        self.assertEqual(self.rows("DIKHAO TABLES"), [["courses"], ["students"]])

    def test_batao_shows_sandarbh(self):
        self.assertIn(
            "SANDARBH courses(id)",
            [row[2] for row in self.rows("BATAO students") if row[0] == "cid"][0],
        )

    def test_survives_restart(self):
        self.run_sql("DAALO MEIN students (id, naam, cid) MAAN (1, 'Ravi', 10)")
        self.reopen()
        self.assertFails("DAALO MEIN students (id, naam, cid) MAAN (2, 'X', 999)", "SANDARBH")
        self.assertFails("MITAO SE courses JAHAN id = 10", "students")


# ============================================================================
# 2. SHART (CHECK)
# ============================================================================


class CheckConstraintTest(ConstraintsTestCase):
    def setUp(self):
        super().setUp()
        self.run_sql("BANAO TABLE students (id INT MUKHYA KUNJI, umar INT SHART (umar >= 0 AUR umar < 150))")

    def test_unknown_column_in_check_fails_at_create(self):
        self.assertFails("BANAO TABLE t (x INT SHART (y > 0))", "column 'y'")

    def test_qualified_column_in_check_fails_at_create(self):
        self.assertFails("BANAO TABLE t (x INT SHART (t.x > 0))", "table.column")

    def test_aggregate_in_check_fails_at_create(self):
        self.assertFails("BANAO TABLE t (x INT SHART (GINO(x) > 0))", "aggregate")

    def test_valid_row_is_accepted(self):
        self.run_sql("DAALO MEIN students MAAN (1, 20)")
        self.assertEqual(self.rows("DIKHAO umar SE students"), [[20]])

    def test_violating_row_is_rejected(self):
        self.assertFails("DAALO MEIN students MAAN (1, -5)", "SHART toot gayi")
        self.assertFails("DAALO MEIN students MAAN (2, 200)", "SHART toot gayi")

    def test_khali_passes_check_unknown_not_false(self):
        # umar is KHALI -> `umar >= 0` is KHALI (unknown), not JHOOTH, so SQL accepts it
        self.run_sql("DAALO MEIN students (id) MAAN (1)")
        self.assertEqual(self.rows("DIKHAO umar SE students"), [[None]])

    def test_update_enforces_check(self):
        self.run_sql("DAALO MEIN students MAAN (1, 20)")
        self.assertFails("BADLO students RAKHO umar = -1 JAHAN id = 1", "SHART toot gayi")

    def test_check_can_use_another_column(self):
        self.run_sql("BANAO TABLE range_t (lo INT, hi INT SHART (hi > lo))")
        self.run_sql("DAALO MEIN range_t MAAN (1, 5)")
        self.assertFails("DAALO MEIN range_t MAAN (5, 1)", "SHART toot gayi")

    def test_alter_add_check_validates_existing_rows(self):
        self.run_sql("BANAO TABLE t (x INT)")
        self.run_sql("DAALO MEIN t MAAN (5), (10)")
        # new column's WARNA default (0) must satisfy its own SHART for existing rows
        self.assertFails("SUDHARO TABLE t JODO y INT SHART (y > 0) WARNA 0", "SHART toot gayi")
        self.run_sql("SUDHARO TABLE t JODO z INT SHART (z > 0) WARNA 1")
        self.assertEqual(self.rows("DIKHAO z SE t"), [[1], [1]])

    def test_alter_drop_column_used_by_check_is_blocked(self):
        self.assertFails("SUDHARO TABLE students HATAO umar", "SHART")

    def test_batao_shows_shart(self):
        self.assertIn(
            "SHART (umar >= 0 AUR umar < 150)",
            [row[2] for row in self.rows("BATAO students") if row[0] == "umar"][0],
        )

    def test_survives_restart(self):
        self.run_sql("DAALO MEIN students MAAN (1, 20)")
        self.reopen()
        self.assertFails("DAALO MEIN students MAAN (2, -1)", "SHART toot gayi")


# ============================================================================
# 3. DATE / TAREEKH
# ============================================================================


class DateTypeTest(ConstraintsTestCase):
    def setUp(self):
        super().setUp()
        self.run_sql(
            "BANAO TABLE students (id INT MUKHYA KUNJI, dob DATE, joined TAREEKH WARNA '2020-01-01')"
        )

    def test_insert_and_select(self):
        self.run_sql("DAALO MEIN students (id, dob) MAAN (1, '2005-06-15')")
        self.assertEqual(self.rows("DIKHAO dob, joined SE students"), [[date(2005, 6, 15), date(2020, 1, 1)]])

    def test_khali_date_allowed(self):
        self.run_sql("DAALO MEIN students (id) MAAN (1)")
        self.assertEqual(self.rows("DIKHAO dob SE students"), [[None]])

    def test_bad_date_string_fails(self):
        self.assertFails("DAALO MEIN students (id, dob) MAAN (1, '2024-13-40')", "valid DATE nahi hai")
        self.assertFails("DAALO MEIN students (id, dob) MAAN (1, 'not-a-date')", "valid DATE nahi hai")

    def test_compare_date_column_to_string_literal(self):
        self.run_sql("DAALO MEIN students (id, dob) MAAN (1, '2005-06-15'), (2, '2010-01-01')")
        self.assertEqual(self.rows("DIKHAO id SE students JAHAN dob > '2008-01-01' KRAM id"), [[2]])
        self.assertEqual(self.rows("DIKHAO id SE students JAHAN dob < '2008-01-01' KRAM id"), [[1]])

    def test_arithmetic_on_dates_fails(self):
        self.run_sql("DAALO MEIN students (id, dob) MAAN (1, '2005-06-15')")
        self.assertFails("DIKHAO dob + 1 SE students")

    def test_jaisa_on_dates_fails(self):
        self.run_sql("DAALO MEIN students (id, dob) MAAN (1, '2005-06-15')")
        self.assertFails("DIKHAO * SE students JAHAN dob JAISA '2005%'")

    def test_min_max_work_on_dates(self):
        self.run_sql("DAALO MEIN students (id, dob) MAAN (1, '2005-06-15'), (2, '2001-01-01')")
        self.assertEqual(
            self.rows("DIKHAO NYUNTAM(dob), ADHIKTAM(dob) SE students"),
            [[date(2001, 1, 1), date(2005, 6, 15)]],
        )

    def test_sum_avg_reject_dates(self):
        self.run_sql("DAALO MEIN students (id, dob) MAAN (1, '2005-06-15')")
        with self.assertRaises(ExecutionError):
            self.db.execute("DIKHAO KUL(dob) SE students")
        with self.assertRaises(ExecutionError):
            self.db.execute("DIKHAO AUSAT(dob) SE students")

    def test_batao_shows_date_type(self):
        self.assertEqual([row[1] for row in self.rows("BATAO students") if row[0] == "dob"], ["DATE"])

    def test_survives_restart_including_default(self):
        self.run_sql("DAALO MEIN students (id, dob) MAAN (1, '2005-06-15')")
        self.reopen()
        self.assertEqual(self.rows("DIKHAO dob, joined SE students"), [[date(2005, 6, 15), date(2020, 1, 1)]])
        self.run_sql("DAALO MEIN students (id) MAAN (2)")  # WARNA default still works after reload
        self.assertEqual(self.rows("DIKHAO joined SE students JAHAN id = 2"), [[date(2020, 1, 1)]])


# ============================================================================
# 4. VARCHAR(n) / NUMBER(p,s)
# ============================================================================


class TypeLengthTest(ConstraintsTestCase):
    def setUp(self):
        super().setUp()
        self.run_sql("BANAO TABLE t (naam VARCHAR(5), score NUMBER(5,2), code CHAR(3))")

    def test_value_within_length_ok(self):
        self.run_sql("DAALO MEIN t (naam) MAAN ('abcde')")
        self.assertEqual(self.rows("DIKHAO naam SE t"), [["abcde"]])

    def test_value_over_length_fails_on_insert(self):
        self.assertFails("DAALO MEIN t (naam) MAAN ('abcdef')", "zyada se zyada 5 characters")

    def test_value_over_length_fails_on_update(self):
        self.run_sql("DAALO MEIN t (naam) MAAN ('ab')")
        self.assertFails("BADLO t RAKHO naam = 'abcdefgh'", "zyada se zyada 5 characters")

    def test_default_over_length_fails_at_create(self):
        self.assertFails("BANAO TABLE bad (x VARCHAR(3) WARNA 'toolong')", "zyada se zyada 3 characters")

    def test_number_precision_and_scale_are_ignored(self):
        # NUMBER/NUMERIC/DECIMAL map to FLOAT; the (p, s) is accepted but not enforced
        self.run_sql("DAALO MEIN t (score) MAAN (123.456789)")
        self.assertEqual(self.rows("DIKHAO score SE t"), [[123.456789]])

    def test_int_length_is_accepted_and_ignored(self):
        self.run_sql("BANAO TABLE ids (x INT(11))")
        self.run_sql("DAALO MEIN ids (x) MAAN (12345678901)")
        self.assertEqual(self.rows("DIKHAO x SE ids"), [[12345678901]])

    def test_batao_shows_length(self):
        rows = {row[0]: row[1] for row in self.rows("BATAO t")}
        self.assertEqual(rows["naam"], "TEXT(5)")
        self.assertEqual(rows["code"], "TEXT(3)")
        self.assertEqual(rows["score"], "FLOAT")  # NUMBER(5,2) is just FLOAT: no length shown

    def test_alter_add_column_with_length(self):
        self.run_sql("SUDHARO TABLE t JODO city VARCHAR(4)")
        self.assertFails("DAALO MEIN t (city) MAAN ('toolong')", "zyada se zyada 4 characters")

    def test_survives_restart(self):
        self.reopen()
        self.assertFails("DAALO MEIN t (naam) MAAN ('abcdef')", "zyada se zyada 5 characters")


# ============================================================================
# 5. NAYA_NAAM (RENAME table / column)
# ============================================================================


class RenameTest(ConstraintsTestCase):
    def setUp(self):
        super().setUp()
        self.run_sql(
            """
            BANAO TABLE courses (id INT MUKHYA KUNJI, title TEXT);
            BANAO TABLE students (id INT MUKHYA KUNJI, naam TEXT, cid INT SANDARBH courses(id));
            DAALO MEIN courses MAAN (10, 'DBMS');
            DAALO MEIN students MAAN (1, 'Ravi', 10);
            """
        )

    def test_rename_table(self):
        self.run_sql("SUDHARO TABLE courses NAYA_NAAM subjects")
        self.assertEqual(self.rows("DIKHAO * SE subjects"), [[10, "DBMS"]])
        self.assertFails("DIKHAO * SE courses")

    def test_rename_table_to_existing_name_fails(self):
        self.assertFails("SUDHARO TABLE courses NAYA_NAAM students", "pehle se hai")

    def test_rename_table_to_same_name_fails(self):
        self.assertFails("SUDHARO TABLE courses NAYA_NAAM courses")

    def test_rename_table_updates_child_fk(self):
        self.run_sql("SUDHARO TABLE courses NAYA_NAAM subjects")
        self.assertIn(
            "SANDARBH subjects(id)",
            [row[2] for row in self.rows("BATAO students") if row[0] == "cid"][0],
        )
        # the FK must still work correctly against the renamed parent
        self.assertFails("DAALO MEIN students (id, naam, cid) MAAN (2, 'X', 999)", "SANDARBH")
        self.run_sql("DAALO MEIN students (id, naam, cid) MAAN (2, 'X', 10)")

    def test_rename_table_indexes_still_work(self):
        self.run_sql("SUDHARO TABLE courses NAYA_NAAM subjects")
        # MUKHYA KUNJI index must still enforce uniqueness after the rename
        self.assertFails("DAALO MEIN subjects MAAN (10, 'Duplicate')")
        self.assertEqual(self.rows("DIKHAO title SE subjects JAHAN id = 10"), [["DBMS"]])

    def test_rename_column(self):
        self.run_sql("SUDHARO TABLE students COLUMN naam NAYA_NAAM full_naam")
        self.assertEqual(self.rows("DIKHAO full_naam SE students"), [["Ravi"]])
        self.assertFails("DIKHAO naam SE students")

    def test_rename_column_to_existing_name_fails(self):
        self.assertFails("SUDHARO TABLE students COLUMN naam NAYA_NAAM id", "pehle se hai")

    def test_rename_column_updates_fk_ref_column(self):
        self.run_sql("SUDHARO TABLE courses COLUMN id NAYA_NAAM course_id")
        self.assertIn(
            "SANDARBH courses(course_id)",
            [row[2] for row in self.rows("BATAO students") if row[0] == "cid"][0],
        )

    def test_rename_column_used_by_check_is_blocked(self):
        self.run_sql("BANAO TABLE t (umar INT SHART (umar >= 0))")
        self.assertFails("SUDHARO TABLE t COLUMN umar NAYA_NAAM age", "SHART")

    def test_survives_restart(self):
        self.run_sql("SUDHARO TABLE courses NAYA_NAAM subjects")
        self.run_sql("SUDHARO TABLE students COLUMN naam NAYA_NAAM full_naam")
        self.reopen()
        self.assertEqual(self.rows("DIKHAO full_naam SE students"), [["Ravi"]])
        self.assertEqual(self.rows("DIKHAO * SE subjects"), [[10, "DBMS"]])
        self.assertFails("DAALO MEIN students (id, full_naam, cid) MAAN (2, 'X', 999)", "SANDARBH")


# ============================================================================
# 6. KAHO (output column alias)
# ============================================================================


class OutputAliasTest(ConstraintsTestCase):
    def setUp(self):
        super().setUp()
        self.run_sql("BANAO TABLE students (id INT MUKHYA KUNJI, cgpa FLOAT)")
        self.run_sql("DAALO MEIN students MAAN (1, 8.4), (2, 9.1), (3, 7.0)")

    def test_alias_becomes_header(self):
        result = self.run_sql("DIKHAO GINO(*) KAHO total, AUSAT(cgpa) KAHO avg SE students")
        self.assertEqual(result.columns, ["total", "avg"])
        self.assertEqual(result.rows, [[3, (8.4 + 9.1 + 7.0) / 3]])

    def test_plain_column_alias(self):
        result = self.run_sql("DIKHAO cgpa KAHO score SE students JAHAN id = 1")
        self.assertEqual(result.columns, ["score"])
        self.assertEqual(result.rows, [[8.4]])

    def test_order_by_can_use_alias(self):
        result = self.run_sql("DIKHAO cgpa KAHO score SE students KRAM score ULTA")
        self.assertEqual(result.rows, [[9.1], [8.4], [7.0]])

    def test_order_by_alias_on_aggregate(self):
        result = self.run_sql(
            "DIKHAO id, GINO(*) KAHO n SE students SAMOOH id KRAM n ULTA, id"
        )
        self.assertEqual(len(result.rows), 3)

    def test_real_column_wins_over_alias_of_same_name(self):
        # `id` is a REAL column, so `KRAM id` must sort by the column, not the alias
        result = self.run_sql("DIKHAO cgpa KAHO id SE students KRAM id")
        self.assertEqual(result.rows, [[8.4], [9.1], [7.0]])  # sorted by real id (1,2,3), not by cgpa

    def test_star_kaho_is_a_parse_error(self):
        with self.assertRaises(ParseError):
            self.db.execute("DIKHAO * KAHO x SE students")

    def test_where_and_having_do_not_see_aliases(self):
        with self.assertRaises(ExecutionError):
            self.db.execute("DIKHAO cgpa KAHO score SE students JAHAN score > 8")

    def test_samjhao_project_line_shows_alias(self):
        plan_rows = self.rows("SAMJHAO DIKHAO GINO(*) KAHO total SE students")
        self.assertTrue(any("PROJECT" in r[0] and "total" in r[0] for r in plan_rows))


if __name__ == "__main__":
    unittest.main()
