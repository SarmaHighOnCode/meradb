"""
Tests for the Phase A "query-language gap" features:

    subqueries (scalar, IN-list, correlated)      BANAO VIEW / HATAO VIEW
    SANYUKT / SAAJHA / CHHODKAR (UNION/INTERSECT/EXCEPT)
    SAMAAN / DAHINA / DONO MILAO (NATURAL/RIGHT/FULL OUTER JOIN)
    PEHLA/COALESCE and AGAR...TAB...WARNA...KHATAM (CASE WHEN)
    INSERT ... DIKHAO and TAKRAAV PAR BADLO (upsert)
    composite (multi-column) ANOKHA / MUKHYA KUNJI

Same style as tests/test_constraints.py: one TestCase per feature, including
error cases and (where the feature touches catalog.json) persistence across a
restart -- always in a tempfile.mkdtemp() directory, never the real data folder.
"""

import shutil
import tempfile
import unittest

from meradb.engine import Engine
from meradb.errors import ExecutionError, ParseError


class GapFeaturesTestCase(unittest.TestCase):
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

    def assertFails(self, sql, message_fragment=None):
        with self.assertRaises(ExecutionError) as ctx:
            self.db.execute(sql)
        if message_fragment:
            self.assertIn(message_fragment, str(ctx.exception))


# ============================================================================
# 1. Subqueries
# ============================================================================


class SubqueryTest(GapFeaturesTestCase):
    def setUp(self):
        super().setUp()
        self.db.execute(
            """
            BANAO TABLE dept (id INT MUKHYA KUNJI, naam TEXT);
            BANAO TABLE emp (id INT MUKHYA KUNJI, naam TEXT, dept_id INT, salary FLOAT);
            DAALO MEIN dept MAAN (1, 'CS'), (2, 'EE'), (3, 'ME');
            DAALO MEIN emp MAAN (1, 'Ravi', 1, 50000), (2, 'Priya', 2, 60000), (3, 'Aman', 1, 55000);
            """
        )

    def test_scalar_subquery(self):
        # AUSAT(salary) = (50000 + 60000 + 55000) / 3 = 55000 -- only Priya's
        # 60000 is strictly greater
        self.assertEqual(
            self.rows("DIKHAO naam SE emp JAHAN salary > (DIKHAO AUSAT(salary) SE emp) KRAM id"),
            [["Priya"]],
        )

    def test_in_subquery(self):
        self.assertEqual(
            self.rows("DIKHAO naam SE dept JAHAN id MEIN (DIKHAO dept_id SE emp) KRAM id"),
            [["CS"], ["EE"]],
        )

    def test_not_in_subquery(self):
        self.assertEqual(self.rows("DIKHAO naam SE dept JAHAN id NAHI MEIN (DIKHAO dept_id SE emp)"), [["ME"]])

    def test_in_subquery_empty_result_means_never_a_member(self):
        self.assertEqual(
            self.rows("DIKHAO naam SE dept JAHAN id MEIN (DIKHAO dept_id SE emp JAHAN dept_id = -1)"), []
        )

    def test_correlated_subquery(self):
        self.assertEqual(
            self.rows(
                "DIKHAO e.naam SE emp e JAHAN salary > "
                "(DIKHAO AUSAT(salary) SE emp e2 JAHAN e2.dept_id = e.dept_id) KRAM e.id"
            ),
            [["Aman"]],
        )

    def test_scalar_subquery_must_return_one_column(self):
        self.assertFails(
            "DIKHAO naam SE emp JAHAN id = (DIKHAO id, naam SE emp)",
            "sirf 1 column",
        )

    def test_scalar_subquery_must_return_at_most_one_row(self):
        self.assertFails(
            "DIKHAO naam SE emp JAHAN id = (DIKHAO id SE emp)",
            "ek se zyada rows",
        )

    def test_in_subquery_must_return_one_column(self):
        self.assertFails(
            "DIKHAO naam SE dept JAHAN id MEIN (DIKHAO id, naam SE emp)",
            "sirf 1 column",
        )

    def test_scalar_subquery_zero_rows_is_khali(self):
        self.assertEqual(
            self.rows("DIKHAO naam SE dept JAHAN id = (DIKHAO dept_id SE emp JAHAN dept_id = -1) KRAM id"), []
        )

    def test_samjhao_shows_subquery_correlation(self):
        plan = "\n".join(
            r[0] for r in self.rows("SAMJHAO DIKHAO naam SE emp JAHAN salary > (DIKHAO AUSAT(salary) SE emp)")
        )
        self.assertIn("SUBQUERY (uncorrelated)", plan)

        plan = "\n".join(
            r[0]
            for r in self.rows(
                "SAMJHAO DIKHAO e.naam SE emp e JAHAN salary > "
                "(DIKHAO AUSAT(salary) SE emp e2 JAHAN e2.dept_id = e.dept_id)"
            )
        )
        self.assertIn("SUBQUERY (correlated)", plan)

    def test_update_where_subquery(self):
        self.db.execute("BADLO emp RAKHO salary = salary + 1000 JAHAN dept_id = (DIKHAO id SE dept JAHAN naam = 'CS')")
        self.assertEqual(self.rows("DIKHAO salary SE emp JAHAN naam = 'Ravi'"), [[51000.0]])

    def test_delete_where_subquery(self):
        self.db.execute("MITAO SE emp JAHAN dept_id MEIN (DIKHAO id SE dept JAHAN naam = 'EE')")
        self.assertEqual(self.rows("DIKHAO naam SE emp KRAM id"), [["Ravi"], ["Aman"]])


# ============================================================================
# 2. VIEWs
# ============================================================================


class ViewTest(GapFeaturesTestCase):
    def setUp(self):
        super().setUp()
        self.db.execute(
            """
            BANAO TABLE dept (id INT MUKHYA KUNJI, naam TEXT);
            BANAO TABLE emp (id INT MUKHYA KUNJI, naam TEXT, dept_id INT, salary FLOAT);
            DAALO MEIN dept MAAN (1, 'CS'), (2, 'EE');
            DAALO MEIN emp MAAN (1, 'Ravi', 1, 50000), (2, 'Priya', 2, 60000), (3, 'Aman', 1, 55000);
            BANAO VIEW cs_emp KAHO DIKHAO naam, salary SE emp JAHAN dept_id = 1;
            """
        )

    def test_view_used_in_se(self):
        self.assertEqual(
            self.rows("DIKHAO naam SE cs_emp KRAM naam"), [["Aman"], ["Ravi"]]
        )

    def test_view_used_in_milao(self):
        self.db.execute("BANAO VIEW dept_v KAHO DIKHAO id, naam SE dept;")
        self.assertEqual(
            self.rows("DIKHAO e.naam, d.naam SE cs_emp e MILAO dept_v d PAR 1 = 1 JAHAN d.naam = 'CS' KRAM e.naam"),
            [["Aman", "CS"], ["Ravi", "CS"]],
        )

    def test_view_reflects_current_schema(self):
        self.db.execute("DAALO MEIN emp MAAN (4, 'Neha', 1, 45000);")
        self.assertEqual(len(self.rows("DIKHAO * SE cs_emp")), 3)

    def test_cannot_insert_into_view(self):
        self.assertFails("DAALO MEIN cs_emp MAAN ('X', 1)", "VIEW hai")

    def test_cannot_update_or_delete_view(self):
        self.assertFails("BADLO cs_emp RAKHO naam = 'x'", "VIEW hai")
        self.assertFails("MITAO SE cs_emp", "VIEW hai")

    def test_show_views(self):
        self.db.execute("BANAO VIEW another KAHO DIKHAO naam SE dept;")
        self.assertEqual(self.rows("DIKHAO VIEWS"), [["another"], ["cs_emp"]])

    def test_batao_view_shows_definition(self):
        rows = self.rows("BATAO cs_emp")
        self.assertEqual(len(rows), 1)
        self.assertIn("DIKHAO", rows[0][0])

    def test_drop_view(self):
        self.db.execute("HATAO VIEW cs_emp;")
        self.assertEqual(self.rows("DIKHAO VIEWS"), [])
        self.assertFails("DIKHAO * SE cs_emp")

    def test_drop_view_unknown(self):
        self.assertFails("HATAO VIEW nope;", "exist nahi")

    def test_view_survives_restart(self):
        self.reopen()
        self.assertEqual(len(self.rows("DIKHAO * SE cs_emp")), 2)

    def test_view_name_collides_with_table(self):
        self.assertFails("BANAO VIEW emp KAHO DIKHAO naam SE dept;", "pehle se hai")

    def test_table_name_collides_with_view(self):
        self.assertFails("BANAO TABLE cs_emp (id INT);", "VIEW")


# ============================================================================
# 3. Set operations
# ============================================================================


class SetOpTest(GapFeaturesTestCase):
    def setUp(self):
        super().setUp()
        self.db.execute(
            """
            BANAO TABLE students (naam TEXT, dept_id INT);
            BANAO TABLE teachers (naam TEXT, dept_id INT);
            DAALO MEIN students MAAN ('Ravi', 1), ('Priya', 1), ('Ravi', 1);
            DAALO MEIN teachers MAAN ('Ravi', 1), ('Kavita', 2);
            """
        )

    def test_union_dedupes(self):
        self.assertEqual(
            sorted(self.rows("DIKHAO naam SE students SANYUKT DIKHAO naam SE teachers")),
            sorted([["Ravi"], ["Priya"], ["Kavita"]]),
        )

    def test_intersect(self):
        self.assertEqual(self.rows("DIKHAO naam SE students SAAJHA DIKHAO naam SE teachers"), [["Ravi"]])

    def test_except(self):
        self.assertEqual(self.rows("DIKHAO naam SE students CHHODKAR DIKHAO naam SE teachers"), [["Priya"]])

    def test_column_count_mismatch(self):
        self.assertFails(
            "DIKHAO naam SE students SANYUKT DIKHAO naam, dept_id SE teachers",
            "columns chahiye",
        )

    def test_chained_union(self):
        self.db.execute("BANAO TABLE alumni (naam TEXT, dept_id INT); DAALO MEIN alumni MAAN ('Zoya', 3);")
        self.assertEqual(
            len(self.rows("DIKHAO naam SE students SANYUKT DIKHAO naam SE teachers SANYUKT DIKHAO naam SE alumni")),
            4,
        )


# ============================================================================
# 4. NATURAL / RIGHT / FULL OUTER JOIN
# ============================================================================


class JoinKindsTest(GapFeaturesTestCase):
    def setUp(self):
        super().setUp()
        self.db.execute(
            """
            BANAO TABLE dept (dept_id INT MUKHYA KUNJI, dname TEXT);
            BANAO TABLE emp (id INT MUKHYA KUNJI, naam TEXT, dept_id INT);
            DAALO MEIN dept MAAN (1, 'CS'), (2, 'EE'), (3, 'ME');
            DAALO MEIN emp MAAN (1, 'Ravi', 1), (2, 'Priya', 2), (3, 'Aman', KHALI);
            """
        )

    def test_natural_join(self):
        self.assertEqual(
            sorted(self.rows("DIKHAO naam, dname SE emp SAMAAN MILAO dept")),
            sorted([["Ravi", "CS"], ["Priya", "EE"]]),
        )

    def test_natural_join_zero_shared_columns_errors(self):
        self.db.execute("BANAO TABLE nodup (x INT);")
        self.assertFails("DIKHAO * SE emp SAMAAN MILAO nodup", "SAMAAN MILAO fail")

    def test_right_join_pads_unmatched_right_rows(self):
        self.assertEqual(
            sorted(
                self.rows("DIKHAO naam, dname SE emp DAHINA MILAO dept PAR emp.dept_id = dept.dept_id"),
                key=lambda r: (r[1], r[0] or ""),
            ),
            [["Ravi", "CS"], ["Priya", "EE"], [None, "ME"]],
        )

    def test_full_join_pads_both_sides(self):
        result = self.rows("DIKHAO naam, dname SE emp DONO MILAO dept PAR emp.dept_id = dept.dept_id")
        self.assertIn(["Aman", None], result)
        self.assertIn([None, "ME"], result)
        self.assertIn(["Ravi", "CS"], result)
        self.assertEqual(len(result), 4)


# ============================================================================
# 5. PEHLA/COALESCE and CASE WHEN
# ============================================================================


class CoalesceCaseTest(GapFeaturesTestCase):
    def setUp(self):
        super().setUp()
        self.db.execute(
            """
            BANAO TABLE emp (naam TEXT, dept_id INT, salary FLOAT);
            DAALO MEIN emp MAAN ('Ravi', 1, 50000), ('Priya', KHALI, 60000), ('Aman', 1, 55000);
            """
        )

    def test_coalesce_mix_of_khali_and_values(self):
        self.assertEqual(
            self.rows("DIKHAO naam, PEHLA(dept_id, -1) SE emp KRAM naam"),
            [["Aman", 1], ["Priya", -1], ["Ravi", 1]],
        )

    def test_coalesce_alias_and_all_khali(self):
        self.assertEqual(self.rows("DIKHAO COALESCE(KHALI, KHALI) SE emp SIRF 1"), [[None]])

    def test_case_when_multiple_branches(self):
        self.assertEqual(
            self.rows(
                "DIKHAO naam, AGAR salary > 55000 TAB 'high' AGAR salary = 55000 TAB 'mid' WARNA 'low' KHATAM "
                "SE emp KRAM naam"
            ),
            [["Aman", "mid"], ["Priya", "high"], ["Ravi", "low"]],
        )

    def test_case_when_khali_condition_is_not_true(self):
        # dept_id HAI KHALI is the only way to test the KHALI row here; a
        # KHALI condition elsewhere (e.g. dept_id = KHALI) must be treated as
        # false, never raise, matching JAHAN's own KHALI handling
        self.assertEqual(
            self.rows("DIKHAO naam, AGAR dept_id = 1 TAB 'yes' WARNA 'no' KHATAM SE emp KRAM naam"),
            [["Aman", "yes"], ["Priya", "no"], ["Ravi", "yes"]],
        )

    def test_case_when_no_match_no_else_is_khali(self):
        self.assertEqual(self.rows("DIKHAO AGAR JHOOTH TAB 1 KHATAM SE emp SIRF 1"), [[None]])


# ============================================================================
# 6. INSERT ... DIKHAO and TAKRAAV PAR BADLO (upsert)
# ============================================================================


class InsertSelectAndUpsertTest(GapFeaturesTestCase):
    def setUp(self):
        super().setUp()
        self.db.execute(
            """
            BANAO TABLE src (id INT, naam TEXT);
            BANAO TABLE dst (id INT MUKHYA KUNJI, naam TEXT);
            DAALO MEIN src MAAN (1, 'Ravi'), (2, 'Priya');
            """
        )

    def test_insert_select(self):
        self.db.execute("DAALO MEIN dst (id, naam) DIKHAO id, naam SE src;")
        self.assertEqual(self.rows("DIKHAO * SE dst KRAM id"), [[1, "Ravi"], [2, "Priya"]])

    def test_insert_select_column_count_mismatch(self):
        self.assertFails("DAALO MEIN dst (id) DIKHAO id, naam SE src;", "values chahiye")

    def test_upsert_hits_conflict(self):
        self.db.execute("DAALO MEIN dst MAAN (1, 'Ravi');")
        result = self.run_sql("DAALO MEIN dst MAAN (1, 'Ravi Updated') TAKRAAV PAR BADLO naam = naam;")
        self.assertEqual(self.rows("DIKHAO * SE dst"), [[1, "Ravi Updated"]])
        self.assertIn("1 row(s) TAKRAAV par badli", result.message)

    def test_upsert_no_conflict_inserts_normally(self):
        self.db.execute("DAALO MEIN dst MAAN (1, 'Ravi');")
        self.db.execute("DAALO MEIN dst MAAN (2, 'Priya') TAKRAAV PAR BADLO naam = naam;")
        self.assertEqual(self.rows("DIKHAO * SE dst KRAM id"), [[1, "Ravi"], [2, "Priya"]])

    def test_upsert_batch_internal_duplicate_still_fails(self):
        self.assertFails("DAALO MEIN dst MAAN (1, 'A'), (1, 'B') TAKRAAV PAR BADLO naam = naam;")


# ============================================================================
# 7. Composite UNIQUE / PRIMARY KEY
# ============================================================================


class CompositeConstraintTest(GapFeaturesTestCase):
    def setUp(self):
        super().setUp()
        self.db.execute(
            """
            BANAO TABLE students (id INT MUKHYA KUNJI);
            BANAO TABLE courses (id INT MUKHYA KUNJI);
            BANAO TABLE enrollments (
                id INT MUKHYA KUNJI,
                student_id INT SANDARBH students(id),
                course_id INT SANDARBH courses(id),
                grade TEXT,
                ANOKHA (student_id, course_id)
            );
            DAALO MEIN students MAAN (1), (2);
            DAALO MEIN courses MAAN (10), (20);
            """
        )

    def test_composite_unique_allows_distinct_combinations(self):
        self.db.execute("DAALO MEIN enrollments MAAN (1, 1, 10, 'A'), (2, 1, 20, 'B'), (3, 2, 10, 'C');")
        self.assertEqual(len(self.rows("DIKHAO * SE enrollments")), 3)

    def test_composite_unique_rejects_duplicate_combination(self):
        self.db.execute("DAALO MEIN enrollments MAAN (1, 1, 10, 'A');")
        self.assertFails("DAALO MEIN enrollments MAAN (2, 1, 10, 'B');", "alag hona chahiye")

    def test_composite_pk(self):
        self.db.execute("BANAO TABLE pk_test (a INT, b INT, c TEXT, MUKHYA KUNJI (a, b));")
        self.db.execute("DAALO MEIN pk_test MAAN (1, 1, 'x');")
        self.assertFails("DAALO MEIN pk_test MAAN (1, 1, 'y');", "alag hona chahiye")

    def test_composite_pk_columns_become_not_null(self):
        self.db.execute("BANAO TABLE pk_test2 (a INT, b INT, MUKHYA KUNJI (a, b));")
        self.assertFails("DAALO MEIN pk_test2 (a) MAAN (1);", "ZAROORI")

    def test_only_one_primary_key_total(self):
        # a column-level MUKHYA KUNJI plus a table-level composite one: refused
        self.assertFails(
            "BANAO TABLE bad (a INT MUKHYA KUNJI, b INT, MUKHYA KUNJI (a, b));",
            "sirf ek MUKHYA KUNJI",
        )
        # two composite MUKHYA KUNJI clauses in the same table: refused at parse time
        with self.assertRaises(ParseError):
            self.db.execute("BANAO TABLE bad2 (a INT, b INT, c INT, MUKHYA KUNJI (a, b), MUKHYA KUNJI (b, c));")

    def test_alter_add_composite_unique(self):
        self.db.execute("BANAO TABLE alt (a INT, b INT); DAALO MEIN alt MAAN (1, 1);")
        self.db.execute("SUDHARO TABLE alt JODO ANOKHA (a, b);")
        self.assertFails("DAALO MEIN alt MAAN (1, 1);", "alag hona chahiye")

    def test_alter_add_composite_rejects_existing_violation(self):
        self.db.execute("BANAO TABLE alt2 (a INT, b INT); DAALO MEIN alt2 MAAN (1, 1), (1, 1);")
        self.assertFails("SUDHARO TABLE alt2 JODO ANOKHA (a, b);")

    def test_composite_constraint_survives_restart(self):
        self.db.execute("DAALO MEIN enrollments MAAN (1, 1, 10, 'A');")
        self.reopen()
        self.assertFails("DAALO MEIN enrollments MAAN (2, 1, 10, 'B');", "alag hona chahiye")

    def test_batao_shows_composite_constraint(self):
        rows = self.rows("BATAO enrollments")
        self.assertTrue(any("student_id, course_id" in r[0] for r in rows))
