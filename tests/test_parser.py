import unittest

from meradb import ast_nodes as ast
from meradb.errors import ParseError
from meradb.parser import parse


def one(text):
    statements = parse(text)
    assert len(statements) == 1
    return statements[0]


class ParserTest(unittest.TestCase):
    def test_create_table(self):
        stmt = one("BANAO TABLE t (id INT MUKHYA KUNJI, naam SHABD ZAROORI ANOKHA, ok BOOL)")
        self.assertEqual(stmt.name, "t")
        self.assertEqual(
            stmt.columns,
            [
                ast.ColumnDef("id", "INT", primary_key=True),
                ast.ColumnDef("naam", "TEXT", not_null=True, unique=True),
                ast.ColumnDef("ok", "BOOL"),
            ],
        )

    def test_insert_multiple_rows(self):
        stmt = one("DAALO MEIN t (a, b) MAAN (1, 'x'), (-2, KHALI)")
        self.assertEqual(stmt.columns, ["a", "b"])
        self.assertEqual(
            stmt.rows,
            [[ast.Literal(1), ast.Literal("x")], [ast.Literal(-2), ast.Literal(None)]],
        )

    def test_select_full(self):
        stmt = one("DIKHAO a, b SE t JAHAN a > 1 KRAM b ULTA, a SIRF 5")
        self.assertIsInstance(stmt, ast.Select)
        self.assertEqual(stmt.columns, [ast.ColumnRef("a"), ast.ColumnRef("b")])
        self.assertEqual(stmt.where, ast.BinaryOp(">", ast.ColumnRef("a"), ast.Literal(1)))
        self.assertEqual(
            stmt.order_by,
            [ast.OrderItem(ast.ColumnRef("b"), True), ast.OrderItem(ast.ColumnRef("a"), False)],
        )
        self.assertEqual(stmt.limit, 5)

    def test_aur_binds_tighter_than_ya(self):
        # a YA b AUR c  ==  a YA (b AUR c)
        stmt = one("DIKHAO * SE t JAHAN a = 1 YA b = 2 AUR c = 3")
        self.assertEqual(stmt.where.op, "YA")
        self.assertEqual(stmt.where.right.op, "AUR")

    def test_arithmetic_precedence(self):
        # 1 + 2 * 3  ==  1 + (2 * 3)
        expr = one("DIKHAO 1 + 2 * 3 SE t").columns[0]
        self.assertEqual(expr.op, "+")
        self.assertEqual(expr.right.op, "*")

    def test_is_null(self):
        stmt = one("MITAO SE t JAHAN a HAI NAHI KHALI")
        self.assertEqual(stmt.where, ast.IsNull(ast.ColumnRef("a"), negated=True))

    def test_update(self):
        stmt = one("BADLO t RAKHO a = a + 1, b = 'z' JAHAN id = 1")
        self.assertEqual([c for c, _ in stmt.assignments], ["a", "b"])

    def test_alter(self):
        self.assertIsInstance(one("SUDHARO TABLE t JODO COLUMN x INT"), ast.AlterAddColumn)
        self.assertIsInstance(one("SUDHARO TABLE t HATAO x"), ast.AlterDropColumn)

    def test_multiple_statements(self):
        self.assertEqual(len(parse("DIKHAO TABLES; BATAO t; SAAF TABLE t;")), 3)

    def test_errors(self):
        for bad in [
            "DIKHAO SE t",  # no columns
            "BANAO TABLE t (id)",  # no type
            "BANAO TABLE t (id BLOB)",  # unknown type
            "DAALO t MAAN (1)",  # missing MEIN
            "DIKHAO * SE t SIRF 'x'",
            "naam DIKHAO",  # doesn't start with a command
            "DIKHAO * SE t DIKHAO * SE t",  # missing ;
        ]:
            with self.subTest(bad=bad), self.assertRaises(ParseError):
                parse(bad)


if __name__ == "__main__":
    unittest.main()
