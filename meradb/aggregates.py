"""
Aggregate functions: they turn MANY rows into ONE value.

    GINO(*)        COUNT(*)   number of rows
    GINO(x)        COUNT(x)   number of rows where x is not KHALI
    KUL(x)         SUM(x)
    AUSAT(x)       AVG(x)
    NYUNTAM(x)     MIN(x)
    ADHIKTAM(x)    MAX(x)

Like SQL, every aggregate except GINO(*) IGNORES KHALI values, and on zero
(non-KHALI) values KUL/AUSAT/NYUNTAM/ADHIKTAM return KHALI (GINO returns 0).

The English SQL names also work: COUNT, SUM, AVG, MIN, MAX.
"""

from datetime import date

from . import ast_nodes as ast
from .errors import ExecutionError
from .evaluator import evaluate, expr_label

ALIASES = {
    "GINO": "GINO",
    "COUNT": "GINO",
    "KUL": "KUL",
    "SUM": "KUL",
    "AUSAT": "AUSAT",
    "AVG": "AUSAT",
    "NYUNTAM": "NYUNTAM",
    "MIN": "NYUNTAM",
    "ADHIKTAM": "ADHIKTAM",
    "MAX": "ADHIKTAM",
}


def canonical_name(func: ast.FuncCall) -> str:
    name = ALIASES.get(func.name)
    if name is None:
        raise ExecutionError(f"Function '{func.name}' nahi pata. Ye chalte hain: GINO, KUL, AUSAT, NYUNTAM, ADHIKTAM")
    if isinstance(func.arg, ast.Star) and name != "GINO":
        raise ExecutionError(f"{func.name}(*) nahi chalta -- '*' sirf GINO(*) mein")
    return name


def compute(func: ast.FuncCall, rows: list[dict]):
    """Compute one aggregate over the rows of one group."""
    name = canonical_name(func)
    if isinstance(func.arg, ast.Star):
        return len(rows)

    values = [v for v in (evaluate(func.arg, r) for r in rows) if v is not None]

    if name == "GINO":
        return len(values)
    if not values:
        return None
    if name in ("KUL", "AUSAT"):
        if any(isinstance(v, (bool, str, date)) for v in values):
            raise ExecutionError(f"{expr_label(func)}: {name} sirf numbers ke saath chalta hai")
        total = sum(values)
        return total if name == "KUL" else total / len(values)
    if name == "NYUNTAM":
        return min(values)
    return max(values)
