"""
The AST (Abstract Syntax Tree): the *meaning* of a query, as Python objects.

The parser builds these; the executor reads them. Neither the parser nor the
executor needs to know about the other -- this file is the contract between them.

    DIKHAO naam SE students JAHAN umar > 18
        |
        v
    Select(
        columns=[ColumnRef("naam")],
        table="students",
        where=BinaryOp(">", ColumnRef("umar"), Literal(18)),
    )
"""

from dataclasses import dataclass, field
from typing import Optional


# ============================================================================
# Expressions -- things that produce a value (used in WHERE, SELECT list, SET)
# ============================================================================


class Expr:
    pass


@dataclass
class Literal(Expr):
    value: object  # int, float, str, bool, or None (KHALI)


@dataclass
class ColumnRef(Expr):
    name: str
    table: Optional[str] = None  # `s` in `s.naam`; None when not qualified


@dataclass
class Star(Expr):
    """The `*` in `DIKHAO * SE students`, or `s.*` (table="s")."""

    table: Optional[str] = None


@dataclass
class BinaryOp(Expr):
    op: str  # "=", "!=", "<", "+", "AUR", "YA", ...
    left: Expr
    right: Expr


@dataclass
class UnaryOp(Expr):
    op: str  # "-" or "NAHI"
    operand: Expr


@dataclass
class IsNull(Expr):
    """`x HAI KHALI` (negated=False) or `x HAI NAHI KHALI` (negated=True)."""

    expr: Expr
    negated: bool = False


@dataclass
class FuncCall(Expr):
    """An aggregate call like `GINO(*)` or `AUSAT(cgpa)`. `arg` is Star() for `*`."""

    name: str  # upper-case, as typed: "GINO", "COUNT", ...
    arg: Expr


@dataclass
class Subquery(Expr):
    """`(DIKHAO ...)` used as an ordinary value -- scalar or IN-list context.
    See Engine._run_subquery for how it is executed (evaluator.py never runs it
    itself, it only looks up a pre-computed result by id(expr))."""

    statement: "Select"


@dataclass
class InSubquery(Expr):
    """`x MEIN (DIKHAO ...)` -- membership against a subquery's rows, instead of
    the literal-list `x MEIN (1, 2, 3)` which parses as a chain of `=`/`YA`."""

    left: Expr
    subquery: Subquery
    negated: bool = False


@dataclass
class Coalesce(Expr):
    """`PEHLA(a, b, c)` / `COALESCE(a, b, c)`: first non-KHALI argument."""

    args: list[Expr]


@dataclass
class CaseWhen(Expr):
    """`AGAR cond1 TAB val1 AGAR cond2 TAB val2 WARNA elseval KHATAM`."""

    branches: list[tuple[Expr, Expr]]
    else_: Optional[Expr] = None


# ============================================================================
# Statements -- one per kind of command
# ============================================================================


class Statement:
    pass


# ---- database-level ----
@dataclass
class CreateDatabase(Statement):
    name: str


@dataclass
class DropDatabase(Statement):
    name: str


@dataclass
class UseDatabase(Statement):
    name: str


@dataclass
class ShowTables(Statement):
    pass


@dataclass
class Describe(Statement):
    table: str


@dataclass
class CreateView(Statement):
    """BANAO VIEW naam KAHO DIKHAO ...  -- `query_text` is the raw SELECT source,
    reparsed fresh every time the view is used (see docs/ARCHITECTURE.md)."""

    name: str
    query_text: str


@dataclass
class DropView(Statement):
    name: str


@dataclass
class ShowViews(Statement):
    pass


# ---- DDL ----
@dataclass
class ColumnDef:
    name: str
    type_name: str  # already normalised: "INT", "FLOAT", "TEXT", "BOOL", "DATE"
    primary_key: bool = False
    not_null: bool = False
    unique: bool = False
    default: object = None  # WARNA value; None means "no default" (= KHALI)
    max_length: Optional[int] = None  # VARCHAR(n)/CHAR(n): max TEXT length
    ref_table: Optional[str] = None  # SANDARBH table(column): the parent table
    ref_column: Optional[str] = None  # SANDARBH table(column): the parent column
    check: Optional[str] = None  # SHART (...): the constraint's source text


@dataclass
class CreateTable(Statement):
    name: str
    columns: list[ColumnDef]
    # table-level constraints (ANOKHA (a, b) / MUKHYA KUNJI (a, b)), see docs/LANGUAGE.md
    composite_unique: list[list[str]] = field(default_factory=list)
    composite_pk: Optional[list[str]] = None


@dataclass
class AlterAddComposite(Statement):
    """SUDHARO TABLE t JODO ANOKHA (a, b)  /  JODO MUKHYA KUNJI (a, b)"""

    table: str
    kind: str  # "ANOKHA" | "MUKHYA"
    columns: list[str]


@dataclass
class DropTable(Statement):
    name: str


@dataclass
class AlterAddColumn(Statement):
    table: str
    column: ColumnDef


@dataclass
class AlterDropColumn(Statement):
    table: str
    column: str


@dataclass
class RenameTable(Statement):
    """SUDHARO TABLE old_name NAYA_NAAM new_name"""

    table: str
    new_name: str


@dataclass
class RenameColumn(Statement):
    """SUDHARO TABLE table_name COLUMN old_name NAYA_NAAM new_name"""

    table: str
    column: str
    new_name: str


@dataclass
class TruncateTable(Statement):
    name: str


@dataclass
class CompactTable(Statement):
    name: str


# ---- transactions ----
@dataclass
class Begin(Statement):
    pass


@dataclass
class Commit(Statement):
    pass


@dataclass
class Rollback(Statement):
    pass


@dataclass
class Explain(Statement):
    """SAMJHAO <statement>: show HOW it would run, without running it."""

    statement: Statement


# ---- DML ----
@dataclass
class Insert(Statement):
    table: str
    columns: Optional[list[str]]  # None means "all columns, in table order"
    # Exactly one of these two is populated by the parser: literal MAAN tuples,
    # or an INSERT ... DIKHAO (multi-table insert / "INSERT ... SELECT").
    rows: Optional[list[list[Expr]]] = None
    select: Optional["Select"] = None
    # TAKRAAV PAR BADLO col = expr, ...: simplified upsert (ON CONFLICT DO UPDATE)
    on_conflict_update: Optional[list[tuple[str, Expr]]] = None


@dataclass
class OrderItem:
    expr: Expr
    descending: bool = False


@dataclass
class Join:
    """`[BAAYAN|DAHINA|DONO|SAMAAN] MILAO courses c [PAR s.cid = c.id]`

    kind: "INNER" (plain MILAO) | "LEFT" (BAAYAN) | "RIGHT" (DAHINA) |
          "FULL" (DONO) | "NATURAL" (SAMAAN, no PAR -- synthesised at bind time).
    `on` is None only for a NATURAL join (parser never writes a PAR clause
    there); the planner fills it in during `_plan_select`.
    """

    table: str
    alias: str
    on: Optional[Expr]
    kind: str = "INNER"


@dataclass
class Select(Statement):
    columns: list[Expr]
    table: str
    alias: Optional[str] = None  # `s` in `SE students s`
    joins: list[Join] = field(default_factory=list)
    where: Optional[Expr] = None
    group_by: list[Expr] = field(default_factory=list)
    having: Optional[Expr] = None
    order_by: list[OrderItem] = field(default_factory=list)
    limit: Optional[int] = None
    distinct: bool = False
    # `columns[i] KAHO aliases[i]` -- None where there is no alias. Same
    # length as `columns`. Kept as a parallel list (not wrapped into `columns`)
    # so `columns` stays a plain list[Expr], like before KAHO existed.
    aliases: list[Optional[str]] = field(default_factory=list)


@dataclass
class SetOp(Statement):
    """`left SANYUKT|SAAJHA|CHHODKAR right` -- UNION/INTERSECT/EXCEPT, dedupe-only
    (no ALL variant, see docs/LANGUAGE.md). Left-associative chaining nests these
    (`a SANYUKT b SANYUKT c` -> SetOp(SetOp(a, b), c))."""

    op: str  # "SANYUKT" | "SAAJHA" | "CHHODKAR"
    left: Statement
    right: Statement


@dataclass
class Update(Statement):
    table: str
    assignments: list[tuple[str, Expr]]
    where: Optional[Expr] = None


@dataclass
class Delete(Statement):
    table: str
    where: Optional[Expr] = None
