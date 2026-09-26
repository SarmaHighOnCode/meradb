"""
Evaluates an expression (from the AST) against ONE row.

    evaluate(BinaryOp(">", ColumnRef("umar"), Literal(18)), {"umar": 20})  ->  True

KHALI (NULL) follows SQL's THREE-VALUED LOGIC. A comparison with KHALI is not
True or False -- it is "unknown" (we return None):

    KHALI = 5          -> KHALI      (unknown)
    KHALI = KHALI      -> KHALI      (still unknown! use `HAI KHALI` instead)
    JHOOTH AUR KHALI   -> JHOOTH     (false AND anything is false)
    SACH YA KHALI      -> SACH       (true OR anything is true)
    SACH AUR KHALI     -> KHALI

JAHAN keeps a row only if the condition is exactly SACH, so "unknown" rows are
filtered out. This is the #1 thing people get wrong about SQL NULLs.
"""

import re
from datetime import date
from functools import lru_cache
from typing import Iterator, Optional

from . import ast_nodes as ast
from .datatypes import format_value, parse_date
from .errors import ExecutionError


def evaluate(expr: ast.Expr, row: dict, subqueries: Optional[dict] = None):
    if isinstance(expr, ast.Literal):
        return expr.value

    if isinstance(expr, (ast.Subquery, ast.InSubquery)):
        # The engine pre-computes every subquery's result for THIS outer row
        # before calling evaluate() (see Engine._run_subquery / find_subqueries)
        # -- evaluate() itself never executes anything, it only looks the
        # answer up by node identity. This keeps evaluator.py free of
        # engine-level concerns (no Catalog/Table/planner imports here).
        if subqueries is None or id(expr) not in subqueries:
            raise ExecutionError("Subquery ka result pehle se ready nahi tha (internal error)")
        value = subqueries[id(expr)]
        if isinstance(expr, ast.Subquery):
            return value  # already reduced to a scalar by the engine
        # InSubquery: `value` is the list of membership values
        left = evaluate(expr.left, row, subqueries)
        if left is None:
            return None
        is_member = any(left == v for v in value if v is not None)
        return (not is_member) if expr.negated else is_member

    if isinstance(expr, ast.Coalesce):
        for arg in expr.args:
            v = evaluate(arg, row, subqueries)
            if v is not None:
                return v
        return None

    if isinstance(expr, ast.CaseWhen):
        for cond, value in expr.branches:
            if is_true(evaluate(cond, row, subqueries)):
                return evaluate(value, row, subqueries)
        return evaluate(expr.else_, row, subqueries) if expr.else_ is not None else None

    if isinstance(expr, ast.ColumnRef):
        if expr.name not in row:
            raise ExecutionError(f"Column '{expr.name}' nahi mila")
        return row[expr.name]

    if isinstance(expr, ast.IsNull):
        is_null = evaluate(expr.expr, row, subqueries) is None
        return not is_null if expr.negated else is_null

    if isinstance(expr, ast.UnaryOp):
        value = evaluate(expr.operand, row, subqueries)
        if value is None:
            return None
        if expr.op == "-":
            _require_number(value, "-")
            return -value
        if expr.op == "NAHI":
            _require_bool(value, "NAHI")
            return not value

    if isinstance(expr, ast.BinaryOp):
        if expr.op == "AUR":
            return _and(expr, row, subqueries)
        if expr.op == "YA":
            return _or(expr, row, subqueries)

        left = evaluate(expr.left, row, subqueries)
        right = evaluate(expr.right, row, subqueries)
        if left is None or right is None:
            return None  # anything combined with KHALI is KHALI
        if expr.op in ("=", "!=", "<", "<=", ">", ">="):
            return _compare(expr.op, left, right)
        if expr.op == "JAISA":
            return _like(left, right)
        return _arithmetic(expr.op, left, right)

    if isinstance(expr, ast.FuncCall):
        # Aggregates are NOT computed here. The engine computes them once per
        # group and stores the answer in the row under agg_key(); we just look it up.
        key = agg_key(expr)
        if key in row:
            return row[key]
        raise ExecutionError(
            f"{expr_label(expr)} yahan nahi chal sakta -- aggregates sirf DIKHAO list, JINKA aur KRAM mein chalte hain"
        )

    if isinstance(expr, ast.Star):
        raise ExecutionError("'*' yahan use nahi ho sakta")

    raise ExecutionError(f"Unknown expression: {expr!r}")


def is_true(value) -> bool:
    """Used by JAHAN: only exactly SACH counts. KHALI (unknown) does not."""
    return value is True


# ----------------------------------------------------------------------------
# helpers
# ----------------------------------------------------------------------------


def _and(expr: ast.BinaryOp, row: dict, subqueries=None):
    left = evaluate(expr.left, row, subqueries)
    _require_bool_or_null(left, "AUR")
    if left is False:
        return False  # short-circuit: no need to look at the right side
    right = evaluate(expr.right, row, subqueries)
    _require_bool_or_null(right, "AUR")
    if right is False:
        return False
    if left is None or right is None:
        return None
    return True


def _or(expr: ast.BinaryOp, row: dict, subqueries=None):
    left = evaluate(expr.left, row, subqueries)
    _require_bool_or_null(left, "YA")
    if left is True:
        return True
    right = evaluate(expr.right, row, subqueries)
    _require_bool_or_null(right, "YA")
    if right is True:
        return True
    if left is None or right is None:
        return None
    return False


def _kind(value) -> str:
    if isinstance(value, bool):
        return "bool"
    if isinstance(value, (int, float)):
        return "number"
    if isinstance(value, date):
        return "date"
    return "text"


def _compare(op: str, left, right) -> bool:
    # `JAHAN dob > '2005-01-01'` -- a string literal compared to a DATE column
    # is coerced to a date first, so users don't have to write a DATE() cast.
    if isinstance(left, date) and isinstance(right, str):
        right = parse_date(right)
    elif isinstance(right, date) and isinstance(left, str):
        left = parse_date(left)

    if _kind(left) != _kind(right):
        raise ExecutionError(
            f"{format_value(left)!r} aur {format_value(right)!r} ko compare nahi kar sakte (alag types)"
        )
    if op == "=":
        return left == right
    if op == "!=":
        return left != right
    if op == "<":
        return left < right
    if op == "<=":
        return left <= right
    if op == ">":
        return left > right
    return left >= right


def _like(text, pattern) -> bool:
    """naam JAISA 'R%'  --  % = any characters (even none), _ = exactly one character."""
    if not isinstance(text, str) or not isinstance(pattern, str):
        raise ExecutionError("JAISA sirf TEXT ke saath chalta hai")
    return _like_regex(pattern).fullmatch(text) is not None


@lru_cache(maxsize=128)
def _like_regex(pattern: str) -> re.Pattern:
    # Turn the LIKE pattern into a regular expression. Everything except % and _
    # is escaped so characters like '.' or '(' are matched literally.
    # Compiled once per pattern and cached -- a WHERE runs this for every row.
    parts = []
    for ch in pattern:
        if ch == "%":
            parts.append(".*")
        elif ch == "_":
            parts.append(".")
        else:
            parts.append(re.escape(ch))
    return re.compile("".join(parts), re.IGNORECASE | re.DOTALL)


def _arithmetic(op: str, left, right):
    if op == "+" and isinstance(left, str) and isinstance(right, str):
        return left + right  # 'Ra' + 'vi' = 'Ravi'
    _require_number(left, op)
    _require_number(right, op)
    if op == "+":
        return left + right
    if op == "-":
        return left - right
    if op == "*":
        return left * right
    if right == 0 and op in ("/", "%"):
        raise ExecutionError("Zero se divide nahi kar sakte")
    if op == "/":
        if isinstance(left, int) and isinstance(right, int):
            return int(left / right)  # integer division truncates, like SQL
        return left / right
    if op == "%":
        return left % right
    raise ExecutionError(f"Unknown operator {op}")


def _require_number(value, op: str) -> None:
    if _kind(value) != "number":
        raise ExecutionError(f"'{op}' sirf numbers ke saath chalta hai, {format_value(value)!r} mila")


def _require_bool(value, op: str) -> None:
    if not isinstance(value, bool):
        raise ExecutionError(f"'{op}' ko SACH/JHOOTH condition chahiye, {format_value(value)!r} mila")


def _require_bool_or_null(value, op: str) -> None:
    if value is not None:
        _require_bool(value, op)


# ----------------------------------------------------------------------------
# utilities used by the engine
# ----------------------------------------------------------------------------


def column_refs(expr, skip_aggregates: bool = False) -> Iterator[str]:
    """
    Every column name mentioned inside an expression (used for validation).
    With skip_aggregates=True, columns inside GINO(...)/KUL(...) etc. are ignored.
    """
    if expr is None:
        return
    if isinstance(expr, ast.ColumnRef):
        yield expr.name
    elif isinstance(expr, ast.BinaryOp):
        yield from column_refs(expr.left, skip_aggregates)
        yield from column_refs(expr.right, skip_aggregates)
    elif isinstance(expr, ast.UnaryOp):
        yield from column_refs(expr.operand, skip_aggregates)
    elif isinstance(expr, ast.IsNull):
        yield from column_refs(expr.expr, skip_aggregates)
    elif isinstance(expr, ast.FuncCall) and not skip_aggregates:
        yield from column_refs(expr.arg, skip_aggregates)
    elif isinstance(expr, ast.Coalesce):
        for arg in expr.args:
            yield from column_refs(arg, skip_aggregates)
    elif isinstance(expr, ast.CaseWhen):
        for cond, value in expr.branches:
            yield from column_refs(cond, skip_aggregates)
            yield from column_refs(value, skip_aggregates)
        yield from column_refs(expr.else_, skip_aggregates)
    # NOTE: no case for Subquery/InSubquery on purpose -- a subquery's own
    # internal columns are not "this query's" columns for grouping validation
    # (they get bound and validated separately when the subquery itself is planned).


def column_ref_nodes(expr) -> Iterator[ast.ColumnRef]:
    """
    Like column_refs, but yields the ColumnRef NODE itself, not just its name --
    so a caller can also see whether it's qualified (`s.naam`, table="s") or
    not. Used to validate SHART (CHECK): it may only use plain column names.
    """
    if expr is None:
        return
    if isinstance(expr, ast.ColumnRef):
        yield expr
    elif isinstance(expr, ast.BinaryOp):
        yield from column_ref_nodes(expr.left)
        yield from column_ref_nodes(expr.right)
    elif isinstance(expr, ast.UnaryOp):
        yield from column_ref_nodes(expr.operand)
    elif isinstance(expr, ast.IsNull):
        yield from column_ref_nodes(expr.expr)
    elif isinstance(expr, ast.FuncCall):
        yield from column_ref_nodes(expr.arg)


def find_aggregates(expr) -> Iterator[ast.FuncCall]:
    """Every aggregate call inside an expression, e.g. both calls in `KUL(a) / GINO(*)`."""
    if isinstance(expr, ast.FuncCall):
        yield expr  # don't look inside: aggregates can't be nested
    elif isinstance(expr, ast.BinaryOp):
        yield from find_aggregates(expr.left)
        yield from find_aggregates(expr.right)
    elif isinstance(expr, ast.UnaryOp):
        yield from find_aggregates(expr.operand)
    elif isinstance(expr, ast.IsNull):
        yield from find_aggregates(expr.expr)
    elif isinstance(expr, ast.Coalesce):
        for arg in expr.args:
            yield from find_aggregates(arg)
    elif isinstance(expr, ast.CaseWhen):
        for cond, value in expr.branches:
            yield from find_aggregates(cond)
            yield from find_aggregates(value)
        yield from find_aggregates(expr.else_)


def find_subqueries(expr) -> "Iterator[ast.Subquery | ast.InSubquery]":
    """Every Subquery/InSubquery node inside an expression (same shape as
    find_aggregates). Used by the engine to pre-compute each subquery's result
    for the current outer row before evaluate() runs. Does NOT look inside a
    subquery's own SELECT -- that gets planned/run separately."""
    if isinstance(expr, ast.Subquery):
        yield expr
    elif isinstance(expr, ast.InSubquery):
        yield expr
        yield from find_subqueries(expr.left)
    elif isinstance(expr, ast.BinaryOp):
        yield from find_subqueries(expr.left)
        yield from find_subqueries(expr.right)
    elif isinstance(expr, ast.UnaryOp):
        yield from find_subqueries(expr.operand)
    elif isinstance(expr, ast.IsNull):
        yield from find_subqueries(expr.expr)
    elif isinstance(expr, ast.FuncCall):
        yield from find_subqueries(expr.arg)
    elif isinstance(expr, ast.Coalesce):
        for arg in expr.args:
            yield from find_subqueries(arg)
    elif isinstance(expr, ast.CaseWhen):
        for cond, value in expr.branches:
            yield from find_subqueries(cond)
            yield from find_subqueries(value)
        yield from find_subqueries(expr.else_)


def agg_key(func: ast.FuncCall) -> tuple:
    """
    Where a group's aggregate result is stored in the row dict. A tuple can
    never clash with a real column name (those are plain strings).
    """
    return ("agg", expr_label(func))


def expr_label(expr: ast.Expr) -> str:
    """A readable column header for a SELECT expression, e.g. `umar + 1`."""
    if isinstance(expr, ast.ColumnRef):
        return f"{expr.table}.{expr.name}" if expr.table else expr.name
    if isinstance(expr, ast.Literal):
        return repr(expr.value) if isinstance(expr.value, str) else format_value(expr.value)
    if isinstance(expr, ast.BinaryOp):
        return f"{expr_label(expr.left)} {expr.op} {expr_label(expr.right)}"
    if isinstance(expr, ast.UnaryOp):
        return f"{expr.op} {expr_label(expr.operand)}" if expr.op == "NAHI" else f"-{expr_label(expr.operand)}"
    if isinstance(expr, ast.IsNull):
        return f"{expr_label(expr.expr)} HAI {'NAHI ' if expr.negated else ''}KHALI"
    if isinstance(expr, ast.FuncCall):
        return f"{expr.name}({expr_label(expr.arg)})"
    if isinstance(expr, ast.Star):
        return f"{expr.table}.*" if expr.table else "*"
    if isinstance(expr, ast.Coalesce):
        return f"PEHLA({', '.join(expr_label(a) for a in expr.args)})"
    if isinstance(expr, ast.CaseWhen):
        return "AGAR ... KHATAM"
    if isinstance(expr, ast.Subquery):
        return "(DIKHAO ...)"
    if isinstance(expr, ast.InSubquery):
        return f"{expr_label(expr.left)} {'NAHI ' if expr.negated else ''}MEIN (DIKHAO ...)"
    return "?"
