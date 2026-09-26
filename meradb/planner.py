"""
The PLANNER: decides HOW to run a query, before a single row is read.

    parser --> AST --> PLANNER --> plan --> engine executes it

It has three jobs:

1. BINDING (name resolution). In
       DIKHAO naam SE students s MILAO courses c PAR s.cid = c.id
   which table does `naam` belong to? The binder rewrites every column
   reference into its full "alias.column" key (`naam` -> `s.naam`), and fails
   early for unknown columns or AMBIGUOUS ones (an `id` in both tables).
   After binding, every row is a dict like {"s.id": 1, "s.naam": "Ravi", "c.id": 7}.

2. ACCESS PATH. For the first table: can an INDEX answer `JAHAN id = 5`
   (O(1)), or must we do a FULL SCAN (O(n))?

3. JOIN STRATEGY. For each MILAO: if the PAR condition is an equality
   (`s.cid = c.id`) use a HASH JOIN (O(n + m)), otherwise a NESTED LOOP (O(n * m)).

`SAMJHAO <query>` prints these decisions without running the query.
"""

from dataclasses import dataclass
from typing import Optional

from . import ast_nodes as ast
from .catalog import TableSchema
from .datatypes import coerce
from .errors import ExecutionError
from .evaluator import column_refs, expr_label
from .table import Table


# ============================================================================
# 1. Binding
# ============================================================================


class Scope:
    """The tables visible in one query: [(alias, schema), ...] in FROM/MILAO order."""

    def __init__(self, sources: list[tuple[str, TableSchema]]):
        self.sources = sources
        self.aliases: dict[str, TableSchema] = {}
        self.by_column: dict[str, list[str]] = {}  # "naam" -> ["s.naam"]
        for alias, schema in sources:
            if alias in self.aliases:
                raise ExecutionError(
                    f"'{alias}' query mein do baar hai -- alag alias do (jaise: SE students s MILAO students t ...)"
                )
            self.aliases[alias] = schema
            for column in schema.column_names:
                self.by_column.setdefault(column, []).append(f"{alias}.{column}")

    # ---- keys and rows ----
    def keys(self, i: int) -> list[str]:
        alias, schema = self.sources[i]
        return [f"{alias}.{c}" for c in schema.column_names]

    def row(self, i: int, values: list) -> dict:
        return dict(zip(self.keys(i), values))

    def null_row(self, i: int) -> dict:
        """All-KHALI row for source i (used by BAAYAN MILAO when nothing matches)."""
        return dict.fromkeys(self.keys(i))

    def all_keys(self) -> list[str]:
        return [key for i in range(len(self.sources)) for key in self.keys(i)]

    def source_index(self, key: str) -> int:
        alias = key.split(".", 1)[0]
        return next(i for i, (a, _) in enumerate(self.sources) if a == alias)

    def display(self, key: str) -> str:
        """`s.naam` -> `naam` when there's only one table (friendlier error messages)."""
        return key.split(".", 1)[1] if len(self.sources) == 1 else key

    # ---- resolution ----
    def resolve(self, ref: ast.ColumnRef) -> str:
        if ref.table is not None:
            schema = self.aliases.get(ref.table)
            if schema is None:
                raise ExecutionError(f"'{ref.table}' is query mein koi table ya alias nahi hai")
            schema.index_of(ref.name)  # raises if the column doesn't exist
            return f"{ref.table}.{ref.name}"

        keys = self.by_column.get(ref.name, [])
        if not keys:
            if len(self.sources) == 1:
                self.sources[0][1].index_of(ref.name)  # raises the usual "column nahi hai" error
            raise ExecutionError(f"Column '{ref.name}' kisi bhi table mein nahi hai")
        if len(keys) > 1:
            raise ExecutionError(f"Column '{ref.name}' ek se zyada tables mein hai -- {' ya '.join(keys)} likho")
        return keys[0]

    def expand_star(self, star: ast.Star) -> list[tuple[str, ast.ColumnRef]]:
        """`*` or `s.*` -> [(header label, column ref), ...]"""
        out = []
        for alias, schema in self.sources:
            if star.table is not None and alias != star.table:
                continue
            for column in schema.column_names:
                # plain header unless the name exists in two tables (then `s.id`, `c.id`)
                label = column if len(self.by_column[column]) == 1 else f"{alias}.{column}"
                out.append((label, ast.ColumnRef(column, alias)))
        if star.table is not None and not out:
            raise ExecutionError(f"'{star.table}' is query mein koi table ya alias nahi hai")
        return out


def bind(expr, scope: Scope):
    """Return a copy of `expr` where every ColumnRef is replaced by its full key."""
    if expr is None or isinstance(expr, (ast.Literal, ast.Star)):
        return expr
    if isinstance(expr, ast.ColumnRef):
        return ast.ColumnRef(scope.resolve(expr))
    if isinstance(expr, ast.BinaryOp):
        return ast.BinaryOp(expr.op, bind(expr.left, scope), bind(expr.right, scope))
    if isinstance(expr, ast.UnaryOp):
        return ast.UnaryOp(expr.op, bind(expr.operand, scope))
    if isinstance(expr, ast.IsNull):
        return ast.IsNull(bind(expr.expr, scope), expr.negated)
    if isinstance(expr, ast.FuncCall):
        return ast.FuncCall(expr.name, bind(expr.arg, scope))
    if isinstance(expr, ast.Coalesce):
        return ast.Coalesce([bind(a, scope) for a in expr.args])
    if isinstance(expr, ast.CaseWhen):
        return ast.CaseWhen(
            [(bind(c, scope), bind(v, scope)) for c, v in expr.branches],
            bind(expr.else_, scope) if expr.else_ is not None else None,
        )
    if isinstance(expr, ast.Subquery):
        # The subquery's OWN columns are bound separately, against its OWN
        # scope, when it is planned (see Engine._run_subquery) -- not here.
        # We only need to bind the OUTER references that _correlate() leaves
        # behind after substitution, which by then are no longer ColumnRefs.
        return expr
    if isinstance(expr, ast.InSubquery):
        return ast.InSubquery(bind(expr.left, scope), expr.subquery, expr.negated)
    raise ExecutionError(f"Unknown expression: {expr!r}")


def natural_join_condition(scope: Scope, right_index: int) -> ast.Expr:
    """
    SAMAAN MILAO (NATURAL JOIN): the ON condition the user did NOT write.
    For every column name shared between the RIGHT table and any table already
    joined so far (0..right_index-1), AND together `earlier.col = right.col`.
    If a name is shared by more than one earlier table, the FIRST (leftmost)
    one wins -- kept simple on purpose (see docs/LANGUAGE.md).
    """
    right_alias, right_schema = scope.sources[right_index]
    first_match: dict[str, str] = {}  # column name -> "alias.column" of the FIRST earlier source that has it
    for j in range(right_index):
        alias, schema = scope.sources[j]
        for col in schema.column_names:
            first_match.setdefault(col, f"{alias}.{col}")

    shared = [col for col in right_schema.column_names if col in first_match]
    if not shared:
        other = scope.sources[0][0] if right_index > 0 else ""
        raise ExecutionError(
            f"'{right_alias}' ka SAMAAN MILAO fail hua -- '{other}' se koi column naam match nahi karta"
        )
    conds = [ast.BinaryOp("=", ast.ColumnRef(first_match[col]), ast.ColumnRef(f"{right_alias}.{col}")) for col in shared]
    result = conds[0]
    for c in conds[1:]:
        result = ast.BinaryOp("AUR", result, c)
    return result


def conjuncts(expr) -> list:
    """Split `a AUR b AUR c` into [a, b, c]. Every one of them must be true."""
    if expr is None:
        return []
    if isinstance(expr, ast.BinaryOp) and expr.op == "AUR":
        return conjuncts(expr.left) + conjuncts(expr.right)
    return [expr]


# ============================================================================
# 2. Access path: index lookup or full scan?
# ============================================================================


@dataclass
class IndexLookup:
    column: int  # position of the column in the table
    column_name: str
    value: object

    def describe(self, table: Table) -> str:
        col = table.schema.columns[self.column]
        kind = "MUKHYA KUNJI" if col.primary_key else "ANOKHA"
        value = expr_label(ast.Literal(self.value))  # 5 stays 5, 'Ravi' keeps its quotes
        return f"INDEX LOOKUP {table.schema.name} PAR {self.column_name} = {value}  [hash index, {kind}]"


def choose_access(table: Table, scope: Scope, where) -> Optional[IndexLookup]:
    """
    Look for a condition `unique_column = constant` that MUST be true (i.e. it
    is one of the AUR-ed parts of JAHAN). If found, one index lookup replaces the
    scan. The full JAHAN is still checked on the row afterwards, so this can
    only make the query faster, never change its answer. None = full scan.
    """
    if getattr(table, "is_view", False):
        return None  # a VIEW has no real index -- always a full scan of its materialized rows
    alias, schema = scope.sources[0]
    for cond in conjuncts(where):
        if not (isinstance(cond, ast.BinaryOp) and cond.op == "="):
            continue
        for col_side, val_side in ((cond.left, cond.right), (cond.right, cond.left)):
            if not (isinstance(col_side, ast.ColumnRef) and isinstance(val_side, ast.Literal)):
                continue
            key_alias, _, name = col_side.name.partition(".")
            if key_alias != alias or val_side.value is None:
                continue
            position = schema.index_of(name)
            column = schema.columns[position]
            if not column.is_unique:
                continue
            try:
                value = coerce(val_side.value, column.type_name, name)
            except ExecutionError:
                continue  # type mismatch: let the normal scan report the error
            return IndexLookup(position, name, value)
    return None


# ============================================================================
# 3. Join strategy: hash join or nested loop?
# ============================================================================


def check_join_condition(on, scope: Scope, right: int) -> None:
    """PAR may only use tables joined so far (0..right)."""
    for key in column_refs(on):
        if scope.source_index(key) > right:
            raise ExecutionError(f"PAR mein '{key}' abhi use nahi ho sakta -- wo table baad mein MILAO hoti hai")


def choose_join(on, scope: Scope, right: int) -> Optional[tuple[str, str]]:
    """
    If PAR contains `left_table.x = right_table.y`, return (left_key, right_key)
    so the engine can do a HASH JOIN. None means nested loop.
    """
    for cond in conjuncts(on):
        if not (isinstance(cond, ast.BinaryOp) and cond.op == "="):
            continue
        if not (isinstance(cond.left, ast.ColumnRef) and isinstance(cond.right, ast.ColumnRef)):
            continue
        a, b = scope.source_index(cond.left.name), scope.source_index(cond.right.name)
        if a < right and b == right:
            return cond.left.name, cond.right.name
        if b < right and a == right:
            return cond.right.name, cond.left.name
    return None
