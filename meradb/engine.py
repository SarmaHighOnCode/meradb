"""
STAGE 3: the EXECUTOR. Takes a parsed statement and actually does it, using
the planner (how to run it), the catalog (schemas) and storage (rows on disk).

The full life of a query:

    text --tokenizer--> tokens --parser--> AST --planner--> plan --engine--> Result
                                                                     |
                                                   catalog.json  +  <table>.tbl files

Two classes live here:

  Instance -- the SHARED state for one data folder: cached catalogs, cached
              indexes, and the LOCK. A server has exactly one Instance.
  Engine   -- one SESSION (one connected client): its current database and its
              open transaction, if any. The server creates one Engine per client.

    engine = Engine("mydata")                    # embedded: private Instance
    results = engine.execute("DIKHAO * SE students;")

CONCURRENCY MODEL: one global lock. Every statement runs while holding it, so
statements from different clients never interleave. A transaction (SHURU) keeps
holding the lock until PAKKA/WAPAS, so other clients wait for it to finish.
This is the simplest correct approach ("serial execution"); real databases use
row-level locks or MVCC to let clients work in parallel.

---------------------------------------------------------------------------
PHASE B NOTES (users/privileges/triggers/procedures build on top of this):

- Statement dispatch is `execute_statement()` below: it looks up `_exec_<ClassName>`
  by the AST node's class name and calls it while holding `self.instance.lock`
  (see `_locked()`). A new statement type (e.g. `ast.CreateTrigger`) just needs
  a matching `_exec_CreateTrigger` method here -- no dispatch table to edit.
- Row-level hooks for triggers: `_exec_Insert` builds `new_rows` (validated,
  ready to write) right before `table.insert_many(new_rows)` -- a BEFORE/AFTER
  INSERT trigger point goes right around that call. Same shape in `_exec_Update`
  around `table.delete_many(targets); table.insert_many(new_rows)`, and in
  `_exec_Delete` around `table.delete_many(doomed)`. All three already collect
  the "old" and "new" row dicts you'd want to pass to a trigger body.
- Privileges: the natural place to check "can this session do X" is the top of
  each `_exec_*` method, or once in `execute_statement()` if a single
  (statement-class -> required-privilege) table is enough -- current dispatch
  already isolates one statement per handler, so per-statement privilege checks
  don't need restructuring.
- Views (this phase) are stored as RAW SOURCE TEXT in `Catalog.views` (name ->
  SELECT text), re-parsed and re-planned fresh every time they're used --
  reuse the same pattern for stored procedure bodies if you go that route.
- Users/sessions: `Engine.__init__` takes a data dir or shared `Instance`; a
  session-identity field (current user) would live on `Engine`, next to
  `self.current_db`, and would need to survive whatever the REPL/server does
  to keep one `Engine` per connected client (see server.py).
- Subqueries (this phase) are pre-computed by the ENGINE (not the evaluator)
  into a `subqueries: dict[id(node) -> value]` passed into every `evaluate()`
  call for a row -- see `_run_subquery`/`_correlate` below and
  `evaluator.find_subqueries`. If Phase B needs its own per-row side data
  (e.g. an audit trigger reading "current user"), the same
  pre-compute-then-pass-a-dict shape is reusable rather than threading a new
  positional parameter through every `evaluate()` call site.
---------------------------------------------------------------------------
"""

import os
import shutil
import threading
from contextlib import contextmanager
from dataclasses import asdict, dataclass, field
from datetime import date
from typing import Optional

from . import aggregates
from . import ast_nodes as ast
from . import planner
from .catalog import Catalog, Column, TableSchema
from .datatypes import coerce, format_value
from .errors import ExecutionError, MeraDBError
from .evaluator import (
    agg_key,
    column_ref_nodes,
    column_refs,
    evaluate,
    expr_label,
    find_aggregates,
    find_subqueries,
    is_true,
)
from .parser import parse, parse_expression
from .planner import Scope, bind, natural_join_condition
from .protocol import default_data_dir, running_server
from .storage import HeapFile, encode_row
from .table import MaterializedTable, Table

DEFAULT_DATABASE = "main"
SNAPSHOT_DIR = ".wapas"  # where transactions keep their "before" copy


@dataclass
class Result:
    """What every statement returns. SELECT fills columns+rows; others a message; failures `error`."""

    columns: list[str] = field(default_factory=list)
    rows: list[list] = field(default_factory=list)
    message: str = ""
    error: str = ""

    def to_dict(self) -> dict:
        # json.dumps can't handle a `date` value directly, so every DATE cell
        # travels over the wire as {"$date": "YYYY-MM-DD"} instead.
        return {
            "columns": self.columns,
            "rows": [[_wire_encode(v) for v in row] for row in self.rows],
            "message": self.message,
            "error": self.error,
        }

    @staticmethod
    def from_dict(d: dict) -> "Result":
        rows = [[_wire_decode(v) for v in row] for row in d.get("rows", [])]
        return Result(d.get("columns", []), rows, d.get("message", ""), d.get("error", ""))


def _wire_encode(value):
    return {"$date": value.isoformat()} if isinstance(value, date) else value


def _wire_decode(value):
    if isinstance(value, dict) and "$date" in value:
        return date.fromisoformat(value["$date"])
    return value


def _infer_column_type(rows: list[list], position: int) -> str:
    """A VIEW's MaterializedTable has no declared column types -- infer one
    from the FIRST non-KHALI value in that column, defaulting to TEXT (see
    docs/ARCHITECTURE.md's view-materialization section)."""
    for row in rows:
        value = row[position]
        if value is not None:
            if isinstance(value, bool):
                return "BOOL"
            if isinstance(value, int):
                return "INT"
            if isinstance(value, float):
                return "FLOAT"
            if isinstance(value, date):
                return "DATE"
            return "TEXT"
    return "TEXT"


# ============================================================================
# Instance: shared state for one data folder
# ============================================================================


class Instance:
    lock_timeout = 10.0  # seconds a statement waits for another session's transaction

    def __init__(self, data_dir: str, served: bool = False):
        self.data_dir = os.path.abspath(data_dir)
        os.makedirs(self.data_dir, exist_ok=True)
        if not served:
            info = running_server(self.data_dir)
            if info:
                raise MeraDBError(
                    f"Is data folder par MeraDB server chal raha hai (port {info.get('port')}). "
                    f"Seedha files mat kholo -- `meradb shell` se server se connect karo."
                )
        self.lock = threading.RLock()
        self.catalogs: dict[str, Catalog] = {}
        self.indexes: dict[tuple, dict] = {}  # (database, table) -> {column: {value: row_id}}
        self.recovered = self._recover()
        os.makedirs(self.db_dir(DEFAULT_DATABASE), exist_ok=True)

    def db_dir(self, name: str) -> str:
        return os.path.join(self.data_dir, name)

    def databases(self) -> list[str]:
        return sorted(
            d for d in os.listdir(self.data_dir) if not d.startswith(".") and os.path.isdir(self.db_dir(d))
        )

    def catalog(self, db: str) -> Catalog:
        if db not in self.catalogs:
            self.catalogs[db] = Catalog(self.db_dir(db))
        return self.catalogs[db]

    def forget(self, db: str) -> None:
        """Drop everything cached for a database (after DROP DATABASE or ROLLBACK)."""
        self.catalogs.pop(db, None)
        for key in [k for k in self.indexes if k[0] == db]:
            del self.indexes[key]

    # ------------------------------------------------------------------
    # Transactions use SHADOW COPIES:
    #   SHURU  copies  data/<db>  ->  data/.wapas/<db>        (the "before" image)
    #   PAKKA  deletes the copy                                (changes are kept)
    #   WAPAS  puts the copy back over data/<db>               (changes are undone)
    # Every step goes through a rename (os.replace), which is ATOMIC, so a crash
    # at any moment leaves something recoverable -- see _recover().
    # ------------------------------------------------------------------
    def _snapshot(self, db: str, suffix: str = "") -> str:
        return os.path.join(self.data_dir, SNAPSHOT_DIR, db + suffix)

    def take_snapshot(self, db: str) -> None:
        tmp = self._snapshot(db, ".tmp")
        shutil.rmtree(tmp, ignore_errors=True)
        shutil.copytree(self.db_dir(db), tmp)
        os.replace(tmp, self._snapshot(db))  # only a COMPLETE copy ever gets the real name

    def discard_snapshot(self, db: str) -> None:
        done = self._snapshot(db, ".done")
        os.replace(self._snapshot(db), done)  # <- the COMMIT POINT: after this, no rollback
        shutil.rmtree(done, ignore_errors=True)

    def restore_snapshot(self, db: str) -> None:
        shutil.rmtree(self.db_dir(db), ignore_errors=True)
        os.replace(self._snapshot(db), self.db_dir(db))
        self.forget(db)

    def _recover(self) -> list[str]:
        """
        CRASH RECOVERY, run at startup. A snapshot still lying around means the
        process died in the middle of a transaction that never reached PAKKA,
        so we roll it back. `.tmp` = SHURU never finished; `.done` = PAKKA
        already happened -- both are just deleted.
        """
        root = os.path.join(self.data_dir, SNAPSHOT_DIR)
        if not os.path.isdir(root):
            return []
        recovered = []
        for name in sorted(os.listdir(root)):
            path = os.path.join(root, name)
            if name.endswith((".tmp", ".done")):
                shutil.rmtree(path, ignore_errors=True)
            else:
                self.restore_snapshot(name)
                recovered.append(name)
        return recovered


# ============================================================================
# Engine: one session
# ============================================================================


class Engine:
    def __init__(self, data: "str | Instance | None" = None):
        if data is None:
            data = default_data_dir()
        self.instance = data if isinstance(data, Instance) else Instance(data)
        self.current_db = DEFAULT_DATABASE
        self.txn_db: Optional[str] = None  # database of the open transaction, if any

    # ------------------------------------------------------------------
    # public API (the REPL, workbench, server and tests all use only these)
    # ------------------------------------------------------------------
    @property
    def description(self) -> str:
        return f"local ({self.instance.data_dir})"

    @property
    def in_transaction(self) -> bool:
        return self.txn_db is not None

    @property
    def catalog(self) -> Catalog:
        return self.instance.catalog(self.current_db)

    def execute(self, text: str) -> list[Result]:
        """Run `;`-separated statements. Raises on the first error (handy in tests)."""
        return [self.execute_statement(stmt) for stmt in parse(text)]

    def run_script(self, text: str) -> list[Result]:
        """Run `;`-separated statements. Never raises: a failing statement gets
        Result(error=...) and the following statements still run."""
        try:
            statements = parse(text)
        except MeraDBError as e:
            return [Result(error=str(e))]
        results = []
        for stmt in statements:
            try:
                results.append(self.execute_statement(stmt))
            except MeraDBError as e:
                results.append(Result(error=str(e)))
        return results

    def execute_statement(self, stmt: ast.Statement) -> Result:
        # Dispatch by class name: Select -> self._exec_Select(stmt)
        handler = getattr(self, f"_exec_{type(stmt).__name__}", None)
        if handler is None:
            raise ExecutionError(f"{type(stmt).__name__} abhi supported nahi hai")
        with self._locked():
            if not os.path.isdir(self.instance.db_dir(self.current_db)):
                gone, self.current_db = self.current_db, DEFAULT_DATABASE
                raise ExecutionError(f"Database '{gone}' ab exist nahi karta. Ab '{DEFAULT_DATABASE}' use ho raha hai.")
            return handler(stmt)

    def schema_tree(self) -> list[dict]:
        """Every database -> table -> column, as plain dicts (sent as JSON by the server)."""
        with self._locked(timeout=2.0):
            return [
                {
                    "name": db,
                    "current": db == self.current_db,
                    "tables": [
                        {"name": t.name, "columns": [c.to_dict() for c in t.columns]}
                        for t in (self.instance.catalog(db).tables[n] for n in sorted(self.instance.catalog(db).tables))
                    ],
                }
                for db in self.instance.databases()
            ]

    def close(self) -> None:
        """End the session. An unfinished transaction is rolled back (standard database behaviour)."""
        if self.in_transaction:
            try:
                self.execute_statement(ast.Rollback())
            except MeraDBError:
                pass

    @contextmanager
    def _locked(self, timeout: Optional[float] = None):
        """Hold the instance lock for the duration of a `with` block (wait at most `timeout`)."""
        lock = self.instance.lock
        if not lock.acquire(timeout=self.instance.lock_timeout if timeout is None else timeout):
            raise ExecutionError(
                "Database busy hai -- kisi aur session ka transaction chal raha hai. Thodi der baad try karo."
            )
        try:
            yield
        finally:
            lock.release()

    def _no_transaction(self, command: str) -> None:
        if self.in_transaction:
            raise ExecutionError(f"{command} transaction ke andar nahi chal sakta -- pehle PAKKA ya WAPAS karo")

    # ------------------------------------------------------------------
    # databases
    # ------------------------------------------------------------------
    def _exec_CreateDatabase(self, stmt: ast.CreateDatabase) -> Result:
        self._no_transaction("BANAO DATABASE")
        path = self.instance.db_dir(stmt.name)
        if os.path.isdir(path):
            raise ExecutionError(f"Database '{stmt.name}' pehle se hai")
        os.makedirs(path)
        return Result(message=f"Database '{stmt.name}' ban gaya")

    def _exec_DropDatabase(self, stmt: ast.DropDatabase) -> Result:
        self._no_transaction("HATAO DATABASE")
        if stmt.name == DEFAULT_DATABASE:
            raise ExecutionError(f"'{DEFAULT_DATABASE}' default database hai, use hata nahi sakte")
        path = self.instance.db_dir(stmt.name)
        if not os.path.isdir(path):
            raise ExecutionError(f"Database '{stmt.name}' exist nahi karta")
        shutil.rmtree(path)
        self.instance.forget(stmt.name)
        if self.current_db == stmt.name:
            self.current_db = DEFAULT_DATABASE
        return Result(message=f"Database '{stmt.name}' hata diya")

    def _exec_UseDatabase(self, stmt: ast.UseDatabase) -> Result:
        self._no_transaction("ISTEMAL")
        if not os.path.isdir(self.instance.db_dir(stmt.name)):
            raise ExecutionError(f"Database '{stmt.name}' exist nahi karta")
        self.current_db = stmt.name
        return Result(message=f"Ab database '{stmt.name}' istemal ho raha hai")

    def _exec_ShowTables(self, stmt: ast.ShowTables) -> Result:
        names = sorted(self.catalog.tables)
        return Result(["table"], [[n] for n in names], f"{len(names)} table(s) in '{self.current_db}'")

    def _exec_Describe(self, stmt: ast.Describe) -> Result:
        if self.catalog.find(stmt.table) is None and stmt.table in self.catalog.views:
            return Result(["definition"], [[self.catalog.views[stmt.table]]], f"VIEW '{stmt.table}'")
        schema = self.catalog.get(stmt.table)
        rows = []
        for c in schema.columns:
            type_display = f"{c.type_name}({c.max_length})" if c.max_length is not None else c.type_name
            flags = []
            if c.primary_key:
                flags.append("MUKHYA KUNJI")
            if c.not_null:
                flags.append("ZAROORI")
            if c.unique:
                flags.append("ANOKHA")
            if c.default is not None:
                flags.append(f"WARNA {expr_label(ast.Literal(c.default))}")
            if c.ref_table:
                flags.append(f"SANDARBH {c.ref_table}({c.ref_column})")
            if c.check:
                flags.append(f"SHART ({c.check})")
            rows.append([c.name, type_display, " ".join(flags)])
        for group in schema.composite_unique:
            rows.append([f"({', '.join(group)})", "", "ANOKHA"])
        if schema.composite_pk:
            rows.append([f"({', '.join(schema.composite_pk)})", "", "MUKHYA KUNJI"])
        return Result(["column", "type", "constraints"], rows, f"Table '{schema.name}'")

    def _exec_ShowViews(self, stmt: ast.ShowViews) -> Result:
        names = sorted(self.catalog.views)
        return Result(["view"], [[n] for n in names], f"{len(names)} view(s) in '{self.current_db}'")

    def _exec_CreateView(self, stmt: ast.CreateView) -> Result:
        if self.catalog.find(stmt.name) is not None:
            raise ExecutionError(f"Table '{stmt.name}' pehle se hai -- VIEW usi naam se nahi ban sakti")
        if stmt.name in self.catalog.views:
            raise ExecutionError(f"View '{stmt.name}' pehle se hai")
        select_stmt = parse(stmt.query_text)[0]
        self._exec_Select(select_stmt)  # sanity check: must run cleanly against the CURRENT schema
        self.catalog.add_view(stmt.name, stmt.query_text)
        return Result(message=f"View '{stmt.name}' ban gaya")

    def _exec_DropView(self, stmt: ast.DropView) -> Result:
        if stmt.name not in self.catalog.views:
            raise ExecutionError(f"View '{stmt.name}' exist nahi karta")
        self.catalog.remove_view(stmt.name)
        return Result(message=f"View '{stmt.name}' hata diya")

    # ------------------------------------------------------------------
    # transactions
    # ------------------------------------------------------------------
    def _exec_Begin(self, stmt: ast.Begin) -> Result:
        if self.in_transaction:
            raise ExecutionError("Transaction pehle se chal raha hai (PAKKA ya WAPAS karo)")
        self.instance.take_snapshot(self.current_db)
        # Take the lock ONE EXTRA time and keep it until PAKKA/WAPAS: other
        # sessions now wait, so nobody sees our half-finished changes.
        self.instance.lock.acquire()
        self.txn_db = self.current_db
        return Result(message="Transaction SHURU. PAKKA se save karo, WAPAS se sab undo.")

    def _exec_Commit(self, stmt: ast.Commit) -> Result:
        if not self.in_transaction:
            raise ExecutionError("Koi transaction nahi chal raha (SHURU se shuru karo)")
        self.instance.discard_snapshot(self.txn_db)
        self.txn_db = None
        self.instance.lock.release()
        return Result(message="Transaction PAKKA -- saare changes save ho gaye")

    def _exec_Rollback(self, stmt: ast.Rollback) -> Result:
        if not self.in_transaction:
            raise ExecutionError("Koi transaction nahi chal raha (SHURU se shuru karo)")
        self.instance.restore_snapshot(self.txn_db)
        self.txn_db = None
        self.instance.lock.release()
        return Result(message="Transaction WAPAS -- saare changes undo ho gaye")

    # ------------------------------------------------------------------
    # DDL
    # ------------------------------------------------------------------
    def _table(self, name: str) -> Table:
        """A REAL table only -- used by DAALO/BADLO/MITAO/SUDHARO/SAAF/SIKODO/HATAO
        TABLE, which a VIEW can never be the target of."""
        if self.catalog.find(name) is None and name in self.catalog.views:
            raise ExecutionError(f"'{name}' ek VIEW hai, table nahi -- isme DAALO/BADLO/MITAO nahi kar sakte")
        return Table(
            self.catalog.get(name), self.catalog.table_path(name), self.instance.indexes, (self.current_db, name)
        )

    def _exec_CreateTable(self, stmt: ast.CreateTable) -> Result:
        if self.catalog.find(stmt.name):
            raise ExecutionError(f"Table '{stmt.name}' pehle se hai")
        if stmt.name in self.catalog.views:
            raise ExecutionError(f"'{stmt.name}' ek VIEW hai -- table usi naam se nahi ban sakti")
        names = [c.name for c in stmt.columns]
        dupes = {n for n in names if names.count(n) > 1}
        if dupes:
            raise ExecutionError(f"Column naam do baar diya: {', '.join(sorted(dupes))}")
        if sum(c.primary_key for c in stmt.columns) > 1:
            raise ExecutionError("Ek table mein sirf ek MUKHYA KUNJI ho sakti hai")
        if stmt.composite_pk is not None and any(c.primary_key for c in stmt.columns):
            raise ExecutionError("Ek table mein sirf ek MUKHYA KUNJI ho sakti hai (single- ya multi-column, dono nahi)")
        self._check_composite_columns(stmt.name, names, stmt.composite_unique, stmt.composite_pk)

        schema = TableSchema(stmt.name, [self._make_column(c) for c in stmt.columns])
        # a composite PRIMARY KEY's columns must all be NOT NULL, exactly like
        # a normal (single-column) PK -- mark this ONCE, here, rather than
        # re-deriving it every validation (see docs/ARCHITECTURE.md)
        if stmt.composite_pk:
            for name in stmt.composite_pk:
                schema.get_column(name).not_null = True
        schema.composite_unique = [list(g) for g in stmt.composite_unique]
        schema.composite_pk = list(stmt.composite_pk) if stmt.composite_pk else None
        for col in schema.columns:
            if col.ref_table:
                self._check_fk_target(col, self_schema=schema)
            if col.check:
                self._check_shart_expr(col, schema)
        HeapFile(self.catalog.table_path(stmt.name)).create()
        self.catalog.add(schema)
        return Result(message=f"Table '{stmt.name}' ban gaya ({len(schema.columns)} columns)")

    @staticmethod
    def _check_composite_columns(table: str, column_names: list[str], composite_unique: list[list[str]], composite_pk: Optional[list[str]]) -> None:
        for group in composite_unique + ([composite_pk] if composite_pk else []):
            for name in group:
                if name not in column_names:
                    raise ExecutionError(f"Table '{table}': composite constraint mein column '{name}' nahi hai")
            if len(set(group)) != len(group):
                raise ExecutionError(f"Table '{table}': composite constraint mein ek column do baar diya hai")

    @staticmethod
    def _make_column(col_def: ast.ColumnDef) -> Column:
        column = Column(**vars(col_def))
        # check the WARNA value fits the column type (and VARCHAR(n) length) now, not at the first INSERT
        column.default = coerce(column.default, column.type_name, column.name)
        Engine._check_length(column, column.default)
        return column

    def _check_fk_target(self, col: Column, self_schema: TableSchema) -> TableSchema:
        """
        SANDARBH parent(column): the parent table must exist (or be the table
        being created/altered itself -- a self-reference), the parent column
        must exist, be MUKHYA KUNJI/ANOKHA (so a hash index answers lookups in
        O(1) -- see table.py), and have the same type as this column.
        """
        target = self_schema if col.ref_table == self_schema.name else self.catalog.find(col.ref_table)
        if target is None:
            raise ExecutionError(f"Column '{col.name}': SANDARBH table '{col.ref_table}' exist nahi karta")
        if col.ref_column not in target.column_names:
            raise ExecutionError(
                f"Column '{col.name}': SANDARBH table '{col.ref_table}' mein column '{col.ref_column}' nahi hai"
            )
        ref_col = target.get_column(col.ref_column)
        if not ref_col.is_unique:
            raise ExecutionError(
                f"Column '{col.name}': SANDARBH '{col.ref_table}.{col.ref_column}' MUKHYA KUNJI ya ANOKHA nahi hai"
            )
        if ref_col.type_name != col.type_name:
            raise ExecutionError(
                f"Column '{col.name}' ({col.type_name}) aur SANDARBH '{col.ref_table}.{col.ref_column}' "
                f"({ref_col.type_name}) ke types match nahi karte"
            )
        return target

    def _check_shart_expr(self, col: Column, schema: TableSchema) -> None:
        """SHART (...) may only use plain (unqualified) columns of THIS table, and no aggregates."""
        expr = parse_expression(col.check)
        agg = next(find_aggregates(expr), None)
        if agg is not None:
            raise ExecutionError(f"Column '{col.name}': SHART mein aggregate function ('{agg.name}') nahi chal sakta")
        for ref in column_ref_nodes(expr):
            if ref.table is not None:
                raise ExecutionError(f"Column '{col.name}': SHART mein sirf column ka naam likho, 'table.column' nahi")
            if ref.name not in schema.column_names:
                raise ExecutionError(f"Column '{col.name}': SHART mein column '{ref.name}' table '{schema.name}' mein nahi hai")

    def _exec_DropTable(self, stmt: ast.DropTable) -> Result:
        children = sorted(
            other_name
            for other_name, other in self.catalog.tables.items()
            if other_name != stmt.name and any(c.ref_table == stmt.name for c in other.columns)
        )
        if children:
            raise ExecutionError(
                f"Table '{stmt.name}' hata nahi sakte -- {', '.join(children)} ise SANDARBH karte hain"
            )
        table = self._table(stmt.name)
        table.heap.destroy()
        table.invalidate_indexes()
        self.catalog.remove(stmt.name)
        return Result(message=f"Table '{stmt.name}' hata diya")

    def _exec_TruncateTable(self, stmt: ast.TruncateTable) -> Result:
        table = self._table(stmt.name)
        schema = table.schema
        for other_name, other in self.catalog.tables.items():
            for child_col in other.columns:
                if child_col.ref_table != schema.name:
                    continue
                child_pos = other.index_of(child_col.name)
                if any(v[child_pos] is not None for _, v in self._table(other_name).rows()):
                    raise ExecutionError(
                        f"Table '{schema.name}' SAAF nahi kar sakte -- table '{other_name}' ka column "
                        f"'{child_col.name}' (SANDARBH) abhi bhi values use karta hai"
                    )
        count = sum(1 for _ in table.rows())
        table.heap.truncate()
        table.invalidate_indexes()
        return Result(message=f"Table '{stmt.name}' saaf -- {count} row(s) hataye")

    def _exec_CompactTable(self, stmt: ast.CompactTable) -> Result:
        table = self._table(stmt.name)
        before = os.path.getsize(table.heap.path)
        table.heap.compact()
        table.invalidate_indexes()  # compaction moves rows, so every row id changed
        after = os.path.getsize(table.heap.path)
        return Result(message=f"Table '{stmt.name}' sikod diya: {before} -> {after} bytes ({before - after} bytes bache)")

    def _exec_AlterAddColumn(self, stmt: ast.AlterAddColumn) -> Result:
        table = self._table(stmt.table)
        schema = table.schema
        new_col = self._make_column(stmt.column)
        if new_col.name in schema.column_names:
            raise ExecutionError(f"Column '{new_col.name}' pehle se hai")
        if new_col.primary_key and any(c.primary_key for c in schema.columns):
            raise ExecutionError("Table mein pehle se MUKHYA KUNJI hai")

        old_rows = [values for _, values in table.rows()]
        if len(old_rows) > 1 and new_col.is_unique and new_col.default is not None:
            # every existing row would get the same WARNA value -> duplicates
            raise ExecutionError(f"Naya ANOKHA column '{new_col.name}' sab rows mein ek hi WARNA value nahi le sakta")
        if old_rows and new_col.is_required and new_col.default is None:
            # every existing row would get KHALI in the new column, breaking ZAROORI
            raise ExecutionError(f"Table khali nahi hai; naya ZAROORI column '{new_col.name}' ko WARNA value chahiye")

        if new_col.ref_table:
            parent = self._check_fk_target(new_col, self_schema=schema)
            if new_col.default is not None:
                parent_table = table if new_col.ref_table == schema.name else self._table(new_col.ref_table)
                parent_pos = parent.index_of(new_col.ref_column)
                if new_col.default not in parent_table.indexes().get(parent_pos, {}):
                    raise ExecutionError(
                        f"Column '{new_col.name}': WARNA value {format_value(new_col.default)} table "
                        f"'{new_col.ref_table}' ke column '{new_col.ref_column}' mein nahi mila (SANDARBH)"
                    )

        new_schema = TableSchema(schema.name, schema.columns + [new_col])
        if new_col.check:
            self._check_shart_expr(new_col, new_schema)
            expr = parse_expression(new_col.check)
            for v in old_rows:
                row = dict(zip(new_schema.column_names, v + [new_col.default]))
                if evaluate(expr, row) is False:
                    raise ExecutionError(
                        f"SHART toot gayi: ({new_col.check}) -- column '{new_col.name}' "
                        f"(table ki maujooda rows ke liye, WARNA value ke saath)"
                    )

        # existing rows get the WARNA value (or KHALI if there is none)
        table.heap.rewrite([encode_row(v + [new_col.default], new_schema.types) for v in old_rows])
        table.invalidate_indexes()
        self.catalog.add(new_schema)
        return Result(message=f"Column '{new_col.name}' '{schema.name}' mein jod diya")

    def _exec_AlterAddComposite(self, stmt: ast.AlterAddComposite) -> Result:
        table = self._table(stmt.table)
        schema = table.schema
        self._check_composite_columns(stmt.table, schema.column_names, [stmt.columns], None)
        if stmt.kind == "MUKHYA":
            if schema.composite_pk is not None or any(c.primary_key for c in schema.columns):
                raise ExecutionError("Ek table mein sirf ek MUKHYA KUNJI ho sakti hai")
            for name in stmt.columns:
                schema.get_column(name).not_null = True
            schema.composite_pk = list(stmt.columns)
        else:
            schema.composite_unique.append(list(stmt.columns))

        # existing data must already satisfy the new constraint
        table.invalidate_indexes()
        positions = tuple(schema.index_of(c) for c in stmt.columns)
        seen = set()
        for _, values in table.rows():
            key = tuple(values[p] for p in positions)
            if stmt.kind == "MUKHYA" and None in key:
                raise ExecutionError(
                    f"Table '{stmt.table}' mein columns {list(stmt.columns)!r} ki maujooda rows mein KHALI hai -- "
                    f"MUKHYA KUNJI ke liye ZAROORI (NOT NULL) chahiye"
                )
            if None in key:
                continue
            if key in seen:
                raise ExecutionError(
                    f"Duplicate value {key!r} columns {list(stmt.columns)!r} mein -- maujooda data ye constraint todta hai"
                )
            seen.add(key)

        self.catalog.add(schema)
        kind_label = "MUKHYA KUNJI" if stmt.kind == "MUKHYA" else "ANOKHA"
        return Result(message=f"Table '{stmt.table}' mein {kind_label} ({', '.join(stmt.columns)}) jod diya")

    def _exec_AlterDropColumn(self, stmt: ast.AlterDropColumn) -> Result:
        table = self._table(stmt.table)
        schema = table.schema
        idx = schema.index_of(stmt.column)
        if len(schema.columns) == 1:
            raise ExecutionError("Table ka aakhri column nahi hata sakte -- HATAO TABLE use karo")

        referenced_by = sorted(
            f"{other_name}.{c.name}"
            for other_name, other in self.catalog.tables.items()
            for c in other.columns
            if c.ref_table == schema.name and c.ref_column == stmt.column
        )
        if referenced_by:
            raise ExecutionError(
                f"Column '{stmt.column}' hata nahi sakte -- {', '.join(referenced_by)} (SANDARBH) ise use karte hain"
            )
        used_by_shart = sorted(
            c.name
            for c in schema.columns
            if c.check and stmt.column in column_refs(parse_expression(c.check))
        )
        if used_by_shart:
            raise ExecutionError(
                f"Column '{stmt.column}' hata nahi sakte -- column {', '.join(used_by_shart)} ka SHART ise use karta hai"
            )

        new_schema = TableSchema(schema.name, schema.columns[:idx] + schema.columns[idx + 1 :])
        new_rows = [v[:idx] + v[idx + 1 :] for _, v in table.rows()]
        table.heap.rewrite([encode_row(v, new_schema.types) for v in new_rows])
        table.invalidate_indexes()
        self.catalog.add(new_schema)
        return Result(message=f"Column '{stmt.column}' '{schema.name}' se hata diya")

    def _exec_RenameTable(self, stmt: ast.RenameTable) -> Result:
        schema = self.catalog.get(stmt.table)  # raises if it doesn't exist
        if stmt.new_name == stmt.table:
            raise ExecutionError(f"Naya naam purane naam '{stmt.table}' jaisa hi hai")
        if self.catalog.find(stmt.new_name) is not None:
            raise ExecutionError(f"Table '{stmt.new_name}' pehle se hai")

        # the row DATA doesn't change, only the file's name -- os.replace is atomic
        os.replace(self.catalog.table_path(stmt.table), self.catalog.table_path(stmt.new_name))
        del self.catalog.tables[stmt.table]
        schema.name = stmt.new_name
        self.catalog.tables[stmt.new_name] = schema
        # every SANDARBH (FK) pointing at the old name must follow it, INCLUDING
        # a self-reference (e.g. employee.manager_id -> employee.id)
        for other in self.catalog.tables.values():
            for col in other.columns:
                if col.ref_table == stmt.table:
                    col.ref_table = stmt.new_name
        self.catalog.save()
        # row ids didn't change, but the CACHE KEY (db, table) did
        self.instance.indexes.pop((self.current_db, stmt.table), None)
        return Result(message=f"Table '{stmt.table}' ka naam ab '{stmt.new_name}' hai")

    def _exec_RenameColumn(self, stmt: ast.RenameColumn) -> Result:
        schema = self.catalog.get(stmt.table)
        idx = schema.index_of(stmt.column)  # raises if the column doesn't exist
        if stmt.new_name == stmt.column:
            raise ExecutionError(f"Naya naam purane naam '{stmt.column}' jaisa hi hai")
        if stmt.new_name in schema.column_names:
            raise ExecutionError(f"Column '{stmt.new_name}' pehle se hai")

        # A SHART's catalog entry is only SOURCE TEXT (see parser.parse_expression),
        # not an AST -- so it can't be "renamed" in place. Refuse instead of
        # silently leaving a CHECK that mentions a column that no longer exists.
        for col in schema.columns:
            if col.check and stmt.column in column_refs(parse_expression(col.check)):
                raise ExecutionError(
                    f"Column '{stmt.column}' rename nahi kar sakte -- column '{col.name}' ka SHART "
                    f"'({col.check})' ise use karta hai. Pehle wo SHART hatao (SHART text round-trip "
                    f"nahi hoti -- dekho docs/LANGUAGE.md)"
                )

        schema.columns[idx].name = stmt.new_name
        # every SANDARBH (FK) pointing at this column must follow it
        for other in self.catalog.tables.values():
            for col in other.columns:
                if col.ref_table == stmt.table and col.ref_column == stmt.column:
                    col.ref_column = stmt.new_name
        self.catalog.save()
        return Result(message=f"Column '{stmt.column}' ka naam ab '{stmt.new_name}' hai")

    # ------------------------------------------------------------------
    # DML
    # ------------------------------------------------------------------
    def _exec_Insert(self, stmt: ast.Insert) -> Result:
        table = self._table(stmt.table)
        schema = table.schema
        target_cols = stmt.columns or schema.column_names
        for col in target_cols:
            schema.index_of(col)  # raises if the column doesn't exist
        if len(set(target_cols)) != len(target_cols):
            raise ExecutionError("Ek column do baar diya hai")

        # Validate EVERY row before writing ANY -- so a bad 3rd row doesn't
        # leave rows 1 and 2 half-inserted. (A tiny taste of atomicity.)
        new_rows = []
        if stmt.select is not None:
            # INSERT ... SELECT: values already come out typed (real Python
            # values, not Expr) -- coerce() in _validate_row still widens them
            # the same way it would widen a literal (e.g. INT into a FLOAT column).
            select_result = self._exec_Select(stmt.select)
            if len(select_result.columns) != len(target_cols):
                raise ExecutionError(
                    f"{len(target_cols)} values chahiye thi, DIKHAO ne {len(select_result.columns)} columns di"
                )
            for row in select_result.rows:
                values = [c.default for c in schema.columns]
                for col, value in zip(target_cols, row):
                    values[schema.index_of(col)] = value
                new_rows.append(self._validate_row(schema, values))
        else:
            for tuple_ in stmt.rows:
                if len(tuple_) != len(target_cols):
                    raise ExecutionError(f"{len(target_cols)} values chahiye thi, {len(tuple_)} mili")
                values = [c.default for c in schema.columns]  # WARNA values (KHALI if none)
                for col, expr in zip(target_cols, tuple_):
                    values[schema.index_of(col)] = evaluate(expr, {})
                new_rows.append(self._validate_row(schema, values))

        if stmt.on_conflict_update is None:
            self._check_unique(table, new_rows)
            self._check_fk(table, new_rows)
            table.insert_many(new_rows)
            return Result(message=f"{len(new_rows)} row(s) daal di")

        # TAKRAAV PAR BADLO (simplified upsert): rows colliding with an EXISTING
        # row (by any unique/PK column, single or composite) get UPDATEd instead
        # of inserted. A collision against another row IN THIS SAME BATCH is
        # still a hard error, exactly like today -- only pre-existing rows are rescued.
        to_insert, to_update = [], []
        for new in new_rows:
            existing_row_id = self._find_conflict(table, new)
            if existing_row_id is None:
                to_insert.append(new)
            else:
                to_update.append((existing_row_id, new))

        self._check_unique(table, to_insert)
        self._check_fk(table, to_insert)

        updated_targets, updated_new_rows = [], []
        assignments = [(schema.index_of(col), expr) for col, expr in stmt.on_conflict_update]
        for row_id, attempted in to_update:
            old_values = table.get(row_id)
            # assignments see the ATTEMPTED (incoming) row's values, not the
            # existing row's -- so `TAKRAAV PAR BADLO naam = naam` means
            # "keep inserting naam" (see docs/LANGUAGE.md)
            env = dict(zip(schema.column_names, attempted))
            new_values = list(old_values)
            for position, expr in assignments:
                new_values[position] = evaluate(expr, env)
            updated_targets.append((row_id, old_values))
            updated_new_rows.append(self._validate_row(schema, new_values))
        if updated_new_rows:
            self._check_unique(table, updated_new_rows, ignore_row_ids={rid for rid, _ in updated_targets})
            self._check_fk(table, updated_new_rows)
            table.delete_many(updated_targets)
            table.insert_many(updated_new_rows)
        table.insert_many(to_insert)
        return Result(message=f"{len(to_insert)} row(s) daali, {len(updated_new_rows)} row(s) TAKRAAV par badli")

    def _find_conflict(self, table: Table, values: list) -> Optional[int]:
        """Does `values` collide with an EXISTING row on any unique/PK column
        (single or composite)? Returns that row's id, or None."""
        indexes = table.indexes()
        for col, index in indexes.items():
            if isinstance(col, tuple):  # composite constraint
                key = tuple(values[p] for p in col)
                if None in key:
                    continue
                row_id = index.get(key)
            else:
                v = values[col]
                if v is None:
                    continue
                row_id = index.get(v)
            if row_id is not None:
                return row_id
        return None

    def _candidates(self, table: Table, access: Optional[planner.IndexLookup]) -> list[tuple[int, list]]:
        """The rows worth looking at: one index lookup, or every row (full scan)."""
        if access is not None:
            return table.lookup(access.column, access.value)
        return list(table.rows())

    def _exec_Select(self, stmt: ast.Select) -> Result:
        plan = self._plan_select(stmt)
        scope = plan.scope

        # The pipeline, in the same order a real database runs it:
        #   scan/index -> MILAO -> JAHAN -> SAMOOH + aggregates -> JINKA -> KRAM
        #   -> project -> ALAG -> SIRF

        # 1. READ the first table (index lookup or full scan)
        rows = [scope.row(0, values) for _, values in self._candidates(plan.tables[0], plan.access)]

        # 2. JOIN (MILAO) each further table
        for i, (join, on, hash_keys) in enumerate(plan.joins, start=1):
            right_rows = [scope.row(i, values) for _, values in plan.tables[i].rows()]
            null_left = {}
            for j in range(i):
                null_left.update(scope.null_row(j))
            rows = _join(rows, right_rows, on, hash_keys, join.kind, null_left, scope.null_row(i))

        # 3. FILTER (JAHAN) -- any WHERE subquery is pre-computed per outer row
        #    (once, if uncorrelated; per row, if correlated -- see _precompute_subqueries)
        if plan.where is not None:
            subq = self._precompute_subqueries([plan.where], rows)
            rows = [r for r, sq in zip(rows, subq) if is_true(evaluate(plan.where, r, sq))]

        # 4. GROUP (SAMOOH) + compute aggregates, then filter groups (JINKA)
        if plan.grouped:
            rows = self._group(rows, plan.group_by, plan.aggregates, scope)
            if plan.having is not None:
                subq = self._precompute_subqueries([plan.having], rows)
                rows = [r for r, sq in zip(rows, subq) if is_true(evaluate(plan.having, r, sq))]

        # 5. SORT (KRAM). Python's sort is *stable*, so sorting by the LAST key
        #    first and the FIRST key last gives a correct multi-column sort.
        for item in reversed(plan.order_by):
            subq = self._precompute_subqueries([item.expr], rows)
            paired = sorted(zip(rows, subq), key=lambda p: _sort_key(evaluate(item.expr, p[0], p[1])), reverse=item.descending)
            rows = [r for r, _ in paired]

        # 6. PROJECT (pick / compute the output columns)
        subq = self._precompute_subqueries(plan.outputs, rows)
        out_rows = [[evaluate(e, r, sq) for e in plan.outputs] for r, sq in zip(rows, subq)]

        # 7. DISTINCT (ALAG): keep the first copy of each row, preserving order
        if stmt.distinct:
            seen = set()
            unique_rows = []
            for row in out_rows:
                if tuple(row) not in seen:
                    seen.add(tuple(row))
                    unique_rows.append(row)
            out_rows = unique_rows

        # 8. LIMIT (SIRF)
        if stmt.limit is not None:
            out_rows = out_rows[: stmt.limit]

        return Result(plan.labels, out_rows, f"{len(out_rows)} row(s)")

    def _resolve_source(self, name: str) -> "Table | MaterializedTable":
        """A SE/MILAO source: a real Table, or -- if `name` isn't a table --
        a VIEW, materialized fresh by re-running its stored DIKHAO text (so it
        always reflects the CURRENT schema of whatever it selects from)."""
        if self.catalog.find(name) is not None:
            return self._table(name)
        if name in self.catalog.views:
            view_stmt = parse(self.catalog.views[name])[0]
            result = self._exec_Select(view_stmt)
            columns = [Column(label, _infer_column_type(result.rows, i)) for i, label in enumerate(result.columns)]
            return MaterializedTable(TableSchema(name, columns), result.rows)
        raise ExecutionError(f"Table '{name}' exist nahi karta")

    def _select_sources_scope(self, stmt: ast.Select) -> tuple[list, list, Scope]:
        sources = [(stmt.alias or stmt.table, stmt.table)] + [(j.alias, j.table) for j in stmt.joins]
        tables = [self._resolve_source(name) for _, name in sources]
        scope = Scope([(alias, table.schema) for (alias, _), table in zip(sources, tables)])
        return sources, tables, scope

    def _plan_select(self, stmt: ast.Select) -> "SelectPlan":
        sources, tables, scope = self._select_sources_scope(stmt)

        # KAHO: an output alias becomes the column header, and (only) KRAM may
        # refer back to it by name -- JAHAN/JINKA do not (standard SQL: they
        # run before the output list exists). We collect {alias: unbound expr}
        # here so KRAM can be rewritten to that expr BEFORE binding.
        labels, outputs, alias_exprs = [], [], {}
        for e, alias in zip(stmt.columns, stmt.aliases):
            if isinstance(e, ast.Star):
                for label, ref in scope.expand_star(e):
                    labels.append(label)
                    outputs.append(bind(ref, scope))
            else:
                labels.append(alias or expr_label(e))  # KAHO wins over the default header
                outputs.append(bind(e, scope))
                if alias:
                    alias_exprs[alias] = e

        def _order_expr(e: ast.Expr) -> ast.Expr:
            # a bare `naam` in KRAM that matches an alias (and isn't a real
            # column) means the output expression, not a column lookup
            if isinstance(e, ast.ColumnRef) and e.table is None and e.name in alias_exprs and e.name not in scope.by_column:
                return alias_exprs[e.name]
            return e

        plan = SelectPlan(
            tables=tables,
            scope=scope,
            labels=labels,
            outputs=outputs,
            where=bind(stmt.where, scope),
            group_by=[bind(g, scope) for g in stmt.group_by],
            having=bind(stmt.having, scope),
            order_by=[ast.OrderItem(bind(_order_expr(o.expr), scope), o.descending) for o in stmt.order_by],
        )
        plan.aggregates = self._unique_aggregates(plan.outputs + [plan.having] + [o.expr for o in plan.order_by])
        plan.grouped = bool(plan.group_by or plan.aggregates or plan.having is not None)
        if plan.grouped:
            self._check_grouping(plan, scope)

        plan.access = planner.choose_access(tables[0], scope, plan.where)
        for i, join in enumerate(stmt.joins, start=1):
            if join.kind == "NATURAL":
                # no PAR written by the user -- synthesise `earlier.col = new.col`
                # for every column name shared with an already-joined table
                on = natural_join_condition(scope, i)
            else:
                on = bind(join.on, scope)
            planner.check_join_condition(on, scope, i)
            plan.joins.append((join, on, planner.choose_join(on, scope, i)))
        return plan

    # ------------------------------------------------------------------
    # subqueries (WHERE-clause scalar / IN-list, correlated or not)
    # ------------------------------------------------------------------
    def _run_subquery(self, subquery: ast.Subquery, outer_row: Optional[dict] = None) -> tuple[Result, bool]:
        """
        Runs `subquery` and returns (its Result, whether it turned out to be
        CORRELATED). With outer_row=None it's simply executed as-is (used for
        BANAO VIEW's sanity check and other non-row contexts). With an
        outer_row, every ColumnRef inside the subquery's own WHERE/columns/etc
        that its OWN scope can't resolve is replaced by the matching value
        from outer_row (see _correlate) -- if that substitution never fires,
        the subquery is uncorrelated and its result doesn't depend on
        outer_row's VALUES at all (only its KEYS, which are the same for
        every row of the same outer query -- see _precompute_subqueries).
        """
        stmt = subquery.statement
        if outer_row is None:
            return self._exec_Select(stmt), False
        _, _, subquery_scope = self._select_sources_scope(stmt)
        new_stmt, fired = self._correlate_select(stmt, subquery_scope, outer_row)
        return self._exec_Select(new_stmt), fired

    def _correlate_select(self, stmt: ast.Select, subquery_scope: Scope, outer_row: dict) -> tuple[ast.Select, bool]:
        fired = [False]

        def corr(e):
            return self._correlate(e, subquery_scope, outer_row, fired)

        new_stmt = ast.Select(
            columns=[corr(c) for c in stmt.columns],
            table=stmt.table,
            alias=stmt.alias,
            joins=[ast.Join(j.table, j.alias, corr(j.on) if j.on is not None else None, j.kind) for j in stmt.joins],
            where=corr(stmt.where) if stmt.where is not None else None,
            group_by=[corr(g) for g in stmt.group_by],
            having=corr(stmt.having) if stmt.having is not None else None,
            order_by=[ast.OrderItem(corr(o.expr), o.descending) for o in stmt.order_by],
            limit=stmt.limit,
            distinct=stmt.distinct,
            aliases=list(stmt.aliases),
        )
        return new_stmt, fired[0]

    def _correlate(self, expr, subquery_scope: Scope, outer_row: dict, fired: list) -> ast.Expr:
        """Replace every ColumnRef in `expr` that does NOT resolve against the
        subquery's OWN scope with a Literal of the matching outer_row value.
        Recurses through every expression shape exactly like planner.bind()."""
        if expr is None or isinstance(expr, (ast.Literal, ast.Star)):
            return expr
        if isinstance(expr, ast.ColumnRef):
            try:
                subquery_scope.resolve(expr)
                return expr  # resolves locally -- this is NOT a correlation
            except ExecutionError:
                pass
            if expr.table is not None:
                key = f"{expr.table}.{expr.name}"
                if key not in outer_row:
                    raise ExecutionError(f"'{key}' na is subquery mein na outer query mein mila")
            else:
                matches = [k for k in outer_row if k.endswith(f".{expr.name}")]
                if not matches:
                    raise ExecutionError(f"Column '{expr.name}' na is subquery mein na outer query mein mila")
                if len(matches) > 1:
                    raise ExecutionError(
                        f"Column '{expr.name}' outer query mein ek se zyada tables mein hai -- "
                        f"{' ya '.join(matches)} likho"
                    )
                key = matches[0]
            fired[0] = True
            return ast.Literal(outer_row[key])
        if isinstance(expr, ast.BinaryOp):
            return ast.BinaryOp(expr.op, self._correlate(expr.left, subquery_scope, outer_row, fired), self._correlate(expr.right, subquery_scope, outer_row, fired))
        if isinstance(expr, ast.UnaryOp):
            return ast.UnaryOp(expr.op, self._correlate(expr.operand, subquery_scope, outer_row, fired))
        if isinstance(expr, ast.IsNull):
            return ast.IsNull(self._correlate(expr.expr, subquery_scope, outer_row, fired), expr.negated)
        if isinstance(expr, ast.FuncCall):
            return ast.FuncCall(expr.name, self._correlate(expr.arg, subquery_scope, outer_row, fired))
        if isinstance(expr, ast.Coalesce):
            return ast.Coalesce([self._correlate(a, subquery_scope, outer_row, fired) for a in expr.args])
        if isinstance(expr, ast.CaseWhen):
            return ast.CaseWhen(
                [(self._correlate(c, subquery_scope, outer_row, fired), self._correlate(v, subquery_scope, outer_row, fired)) for c, v in expr.branches],
                self._correlate(expr.else_, subquery_scope, outer_row, fired) if expr.else_ is not None else None,
            )
        if isinstance(expr, (ast.Subquery, ast.InSubquery)):
            return expr  # a NESTED subquery correlates against ITS OWN nesting when IT runs
        raise ExecutionError(f"Unknown expression: {expr!r}")

    def _reduce_subquery_result(self, node, result: Result):
        """Scalar context (ast.Subquery): exactly 1 column, 0 or 1 row.
        List context (ast.InSubquery): exactly 1 column, any number of rows."""
        if len(result.columns) != 1:
            raise ExecutionError("Subquery sirf 1 column return kar sakti hai is jagah")
        if isinstance(node, ast.Subquery):
            if len(result.rows) > 1:
                raise ExecutionError("Subquery ek se zyada rows return kar rahi hai -- sirf 1 row honi chahiye")
            return result.rows[0][0] if result.rows else None
        return [r[0] for r in result.rows]

    def _precompute_subqueries(self, exprs: list, rows: list[dict]) -> list[dict]:
        """One `subqueries` dict per row, ready to pass into evaluate(). A
        subquery is structurally either correlated or not -- whether its
        columns resolve locally depends only on which KEYS the outer row has,
        not their values, and every row here has the same keys -- so ONE dry
        run (against the first row) decides correlated-vs-not for ALL rows:
        uncorrelated results are computed once and shared; correlated ones are
        recomputed per row."""
        nodes, seen = [], set()
        for e in exprs:
            for node in find_subqueries(e):
                if id(node) not in seen:
                    seen.add(id(node))
                    nodes.append(node)
        if not nodes or not rows:
            return [{} for _ in rows]

        uncorrelated_value = {}
        correlated = set()
        probe = rows[0]
        for node in nodes:
            sub = node if isinstance(node, ast.Subquery) else node.subquery
            result, fired = self._run_subquery(sub, probe)
            if fired:
                correlated.add(id(node))
            else:
                uncorrelated_value[id(node)] = self._reduce_subquery_result(node, result)

        out = []
        for row in rows:
            d = dict(uncorrelated_value)
            for node in nodes:
                if id(node) in correlated:
                    sub = node if isinstance(node, ast.Subquery) else node.subquery
                    result, _fired = self._run_subquery(sub, row)
                    d[id(node)] = self._reduce_subquery_result(node, result)
            out.append(d)
        return out

    # ------------------------------------------------------------------
    # set operations: SANYUKT (UNION), SAAJHA (INTERSECT), CHHODKAR (EXCEPT)
    # ------------------------------------------------------------------
    def _exec_SetOp(self, stmt: ast.SetOp) -> Result:
        op_name = {"SANYUKT": "UNION", "SAAJHA": "INTERSECT", "CHHODKAR": "EXCEPT"}[stmt.op]
        left = self.execute_statement(stmt.left)
        right = self.execute_statement(stmt.right)
        if len(left.columns) != len(right.columns):
            raise ExecutionError(
                f"{stmt.op} ({op_name}) ke dono taraf {len(left.columns)} columns chahiye, "
                f"{len(left.columns)} aur {len(right.columns)} mile"
            )
        left_tuples = [tuple(r) for r in left.rows]
        right_tuples = [tuple(r) for r in right.rows]

        if stmt.op == "SANYUKT":  # UNION: dedupe, preserving first-occurrence order
            seen, out = set(), []
            for row in left_tuples + right_tuples:
                if row not in seen:
                    seen.add(row)
                    out.append(row)
        elif stmt.op == "SAAJHA":  # INTERSECT: in BOTH, deduped, left's order
            right_set = set(right_tuples)
            seen, out = set(), []
            for row in left_tuples:
                if row in right_set and row not in seen:
                    seen.add(row)
                    out.append(row)
        else:  # CHHODKAR: EXCEPT -- in left but NOT right, deduped, left's order
            right_set = set(right_tuples)
            seen, out = set(), []
            for row in left_tuples:
                if row not in right_set and row not in seen:
                    seen.add(row)
                    out.append(row)

        return Result(left.columns, [list(r) for r in out], f"{len(out)} row(s)")

    def _exec_Update(self, stmt: ast.Update) -> Result:
        table = self._table(stmt.table)
        schema = table.schema
        scope = Scope([(stmt.table, schema)])
        assignments = [(schema.index_of(col), bind(expr, scope)) for col, expr in stmt.assignments]
        where = bind(stmt.where, scope)

        # Collect matching rows FIRST, then modify. If we updated while scanning,
        # the re-inserted rows (appended at the end of the file) would be scanned
        # again and updated twice -- the famous "Halloween problem".
        candidates = self._candidates(table, planner.choose_access(table, scope, where))
        if where is not None:
            candidate_rows = [scope.row(0, values) for _, values in candidates]
            subq = self._precompute_subqueries([where], candidate_rows)
            targets = [c for c, r, sq in zip(candidates, candidate_rows, subq) if is_true(evaluate(where, r, sq))]
        else:
            targets = list(candidates)

        old_rows = [scope.row(0, values) for _, values in targets]  # SET expressions see the OLD values
        set_subq = self._precompute_subqueries([expr for _, expr in assignments], old_rows)
        new_rows = []
        for (_, values), old, sq in zip(targets, old_rows, set_subq):
            new = list(values)
            for position, expr in assignments:
                new[position] = evaluate(expr, old, sq)
            new_rows.append(self._validate_row(schema, new))

        self._check_unique(table, new_rows, ignore_row_ids={row_id for row_id, _ in targets})
        self._check_fk(table, new_rows)  # child side: new FK values must exist in the parent

        # parent side (RESTRICT): if a referenced column's value is CHANGING,
        # no child row may still be pointing at the value that is disappearing
        changed_by_column: dict[int, set] = {}
        for (_, old_values), new_values in zip(targets, new_rows):
            for pos in range(len(schema.columns)):
                if old_values[pos] is not None and old_values[pos] != new_values[pos]:
                    changed_by_column.setdefault(pos, set()).add(old_values[pos])
        if changed_by_column:
            overrides = {row_id: new for (row_id, _), new in zip(targets, new_rows)}
            self._check_no_children(schema, changed_by_column, overrides=overrides)

        # An update = delete old versions + insert new versions. All deletes
        # happen first, so an index entry moved from one row to another (e.g.
        # swapping two ids) is never removed by mistake.
        table.delete_many(targets)
        table.insert_many(new_rows)
        return Result(message=f"{len(new_rows)} row(s) badal di")

    def _exec_Delete(self, stmt: ast.Delete) -> Result:
        table = self._table(stmt.table)
        schema = table.schema
        scope = Scope([(stmt.table, schema)])
        where = bind(stmt.where, scope)

        candidates = self._candidates(table, planner.choose_access(table, scope, where))
        if where is not None:
            candidate_rows = [scope.row(0, values) for _, values in candidates]
            subq = self._precompute_subqueries([where], candidate_rows)
            doomed = [c for c, r, sq in zip(candidates, candidate_rows, subq) if is_true(evaluate(where, r, sq))]
        else:
            doomed = list(candidates)

        # RESTRICT: refuse if any child row still references a value about to be deleted
        deleted_by_column: dict[int, set] = {}
        for _, values in doomed:
            for pos in range(len(schema.columns)):
                if values[pos] is not None:
                    deleted_by_column.setdefault(pos, set()).add(values[pos])
        if deleted_by_column:
            self._check_no_children(schema, deleted_by_column, exempt_row_ids={rid for rid, _ in doomed})

        table.delete_many(doomed)
        return Result(message=f"{len(doomed)} row(s) mita di")

    # ------------------------------------------------------------------
    # SAMJHAO (EXPLAIN)
    # ------------------------------------------------------------------
    def _exec_Explain(self, stmt: ast.Explain) -> Result:
        inner = stmt.statement
        if isinstance(inner, ast.Select):
            lines = self._explain_select(inner, self._plan_select(inner))
        elif isinstance(inner, ast.SetOp):
            op_name = {"SANYUKT": "UNION", "SAAJHA": "INTERSECT", "CHHODKAR": "EXCEPT"}[inner.op]
            lines = [f"{inner.op} ({op_name}) of:"]
            for side, label in ((inner.left, "LEFT"), (inner.right, "RIGHT")):
                sub_lines = self._explain_one(side)
                lines.append(f"  {label}:")
                lines.extend(f"    {line}" for line in sub_lines)
        elif isinstance(inner, (ast.Update, ast.Delete)):
            table = self._table(inner.table)
            scope = Scope([(inner.table, table.schema)])
            access = planner.choose_access(table, scope, bind(inner.where, scope))
            verb = "BADLO (update)" if isinstance(inner, ast.Update) else "MITAO (delete)"
            lines = [
                access.describe(table) if access else f"FULL SCAN {inner.table}",
                *([f"FILTER  JAHAN {expr_label(inner.where)}"] if inner.where is not None else []),
                f"{verb} matching rows",
            ]
        else:
            raise ExecutionError("SAMJHAO sirf DIKHAO, BADLO aur MITAO ke saath chalta hai")
        numbered = [[f"{i}. {line}"] for i, line in enumerate(lines, start=1)]
        return Result(["plan"], numbered, "Query plan (query chalayi nahi gayi)")

    def _explain_one(self, stmt: ast.Statement) -> list[str]:
        """One side of a SetOp -- itself a Select or (recursively) a SetOp."""
        if isinstance(stmt, ast.SetOp):
            op_name = {"SANYUKT": "UNION", "SAAJHA": "INTERSECT", "CHHODKAR": "EXCEPT"}[stmt.op]
            lines = [f"{stmt.op} ({op_name}) of:"]
            for side in (stmt.left, stmt.right):
                lines.extend(f"  {line}" for line in self._explain_one(side))
            return lines
        return self._explain_select(stmt, self._plan_select(stmt))

    JOIN_LABEL = {"INNER": "", "LEFT": "LEFT ", "RIGHT": "RIGHT ", "FULL": "FULL ", "NATURAL": "NATURAL "}

    def _explain_select(self, stmt: ast.Select, plan: "SelectPlan") -> list[str]:
        first = plan.tables[0]
        alias = f" {stmt.alias}" if stmt.alias else ""
        lines = [plan.access.describe(first) if plan.access else f"FULL SCAN {stmt.table}{alias}"]
        for join, on, hash_keys in plan.joins:
            kind = self.JOIN_LABEL[join.kind] + ("HASH JOIN" if hash_keys else "NESTED LOOP JOIN")
            name = join.table if join.alias == join.table else f"{join.table} {join.alias}"
            lines.append(f"{kind} {name} PAR {expr_label(on)}")
        if stmt.where is not None:
            lines.append(f"FILTER  JAHAN {expr_label(stmt.where)}")
        lines.extend(self._explain_subqueries([stmt.where], plan.scope))
        if plan.grouped:
            aggs = ", ".join(expr_label(a) for a in plan.aggregates) or "-"
            if stmt.group_by:
                lines.append(f"GROUP  SAMOOH {', '.join(expr_label(g) for g in stmt.group_by)}  [aggregates: {aggs}]")
            else:
                lines.append(f"AGGREGATE saari rows ek group  [aggregates: {aggs}]")
        if stmt.having is not None:
            lines.append(f"FILTER GROUPS  JINKA {expr_label(stmt.having)}")
            lines.extend(self._explain_subqueries([stmt.having], plan.scope))
        if stmt.order_by:
            keys = ", ".join(expr_label(o.expr) + (" ULTA" if o.descending else "") for o in stmt.order_by)
            lines.append(f"SORT  KRAM {keys}")
        lines.append(f"PROJECT  {', '.join(plan.labels)}")
        lines.extend(self._explain_subqueries(plan.outputs, plan.scope))
        if stmt.distinct:
            lines.append("DISTINCT  ALAG")
        if stmt.limit is not None:
            lines.append(f"LIMIT  SIRF {stmt.limit}")
        return lines

    def _explain_subqueries(self, exprs: list, scope: Scope) -> list[str]:
        """One line per DISTINCT subquery node found in `exprs`, naming
        whether it's correlated -- see docs/ARCHITECTURE.md. Deliberately
        shallow: only the subquery's OWN first plan line is shown, not its
        whole nested plan tree."""
        lines, seen = [], set()
        dummy_outer = {k: None for k in scope.all_keys()}
        for e in exprs:
            for node in find_subqueries(e):
                sub = node if isinstance(node, ast.Subquery) else node.subquery
                if id(sub) in seen:
                    continue
                seen.add(id(sub))
                _, _, subquery_scope = self._select_sources_scope(sub.statement)
                substituted, fired = self._correlate_select(sub.statement, subquery_scope, dummy_outer)
                inner_plan = self._plan_select(substituted)
                first_line = self._explain_select(substituted, inner_plan)[0]
                kind = "correlated" if fired else "uncorrelated"
                lines.append(f"SUBQUERY ({kind}): {first_line}")
        return lines

    # ------------------------------------------------------------------
    # grouping helpers
    # ------------------------------------------------------------------
    @staticmethod
    def _unique_aggregates(exprs: list) -> list[ast.FuncCall]:
        found: dict[tuple, ast.FuncCall] = {}
        for expr in exprs:
            for func in find_aggregates(expr):
                aggregates.canonical_name(func)  # fail early on unknown functions
                found.setdefault(agg_key(func), func)
        return list(found.values())

    @staticmethod
    def _check_grouping(plan: "SelectPlan", scope: Scope) -> None:
        """
        In a grouped query each output row stands for a whole GROUP of rows, so a
        bare column like `naam` has no single value -- unless we grouped by it.
        """
        grouped = {name for g in plan.group_by for name in column_refs(g)}
        for expr in plan.outputs + [plan.having] + [o.expr for o in plan.order_by]:
            for name in column_refs(expr, skip_aggregates=True):
                if name not in grouped:
                    raise ExecutionError(
                        f"Column '{scope.display(name)}' SAMOOH mein nahi hai -- ise SAMOOH mein daalo "
                        f"ya kisi aggregate (GINO, KUL, AUSAT...) ke andar use karo"
                    )

    @staticmethod
    def _group(rows: list[dict], group_by: list, aggs: list[ast.FuncCall], scope: Scope) -> list[dict]:
        """
        Bucket rows by their SAMOOH values, then turn each bucket into ONE row:
        the bucket's first row (for the grouped columns) plus every aggregate's
        result stored under agg_key(). evaluate() then finds them there.
        """
        buckets: dict[tuple, list[dict]] = {}
        for r in rows:
            key = tuple(evaluate(g, r) for g in group_by)
            buckets.setdefault(key, []).append(r)
        if not group_by and not buckets:
            buckets[()] = []  # `DIKHAO GINO(*) SE empty_table` must still return one row: 0

        out = []
        for members in buckets.values():
            group_row = dict(members[0]) if members else dict.fromkeys(scope.all_keys())
            for func in aggs:
                group_row[agg_key(func)] = aggregates.compute(func, members)
            out.append(group_row)
        return out

    # ------------------------------------------------------------------
    # constraint checks
    # ------------------------------------------------------------------
    @staticmethod
    def _validate_row(schema: TableSchema, values: list) -> list:
        """Type-check each value and enforce ZAROORI / MUKHYA KUNJI (not null) / VARCHAR(n) / SHART."""
        out = []
        for col, value in zip(schema.columns, values):
            value = coerce(value, col.type_name, col.name)
            if value is None and col.is_required:
                raise ExecutionError(f"Column '{col.name}' ZAROORI hai, KHALI nahi ho sakta")
            Engine._check_length(col, value)
            out.append(value)
        Engine._check_shart(schema, out)
        return out

    @staticmethod
    def _check_length(col: Column, value) -> None:
        if value is not None and col.max_length is not None and len(value) > col.max_length:
            raise ExecutionError(f"Column '{col.name}' mein zyada se zyada {col.max_length} characters ho sakte hain")

    @staticmethod
    def _check_shart(schema: TableSchema, values: list) -> None:
        """
        SHART (CHECK), SQL semantics: the row is rejected ONLY when the
        expression is exactly JHOOTH. KHALI (unknown, e.g. because a column it
        uses is KHALI) passes, same as SQL. ColumnRefs look up `row[name]`
        directly, which is exactly what evaluate() already does for an
        unbound (table=None) reference -- no binding needed here.
        """
        row = dict(zip(schema.column_names, values))
        for col in schema.columns:
            if col.check and evaluate(parse_expression(col.check), row) is False:
                raise ExecutionError(f"SHART toot gayi: ({col.check}) -- column '{col.name}'")

    @staticmethod
    def _check_unique(table: Table, new_rows: list[list], ignore_row_ids: frozenset = frozenset()) -> None:
        """
        Enforce ANOKHA / MUKHYA KUNJI (single-column AND composite) using the
        hash indexes: O(1) per value instead of scanning the whole table.
        `ignore_row_ids` are rows that are being replaced (UPDATE), so their
        old values don't count. A composite constraint's index is keyed by a
        tuple of column POSITIONS (see Table._composite_keys), so its `seen`
        set and its value are both tuples instead of single values.
        """
        indexes = table.indexes()
        seen: dict = {col: set() for col in indexes}  # values within this statement
        for values in new_rows:
            for col, index in indexes.items():
                if isinstance(col, tuple):  # composite constraint
                    v = tuple(values[p] for p in col)
                    if None in v:
                        continue  # KHALI never participates in a uniqueness violation
                    existing = index.get(v)
                    if (existing is not None and existing not in ignore_row_ids) or v in seen[col]:
                        names = [table.schema.columns[p].name for p in col]
                        raise ExecutionError(f"Duplicate value {v!r} columns {names!r} mein -- ye combination alag hona chahiye")
                else:
                    v = values[col]
                    if v is None:
                        continue  # like SQL: many KHALI values are allowed in an ANOKHA column
                    existing = index.get(v)
                    if (existing is not None and existing not in ignore_row_ids) or v in seen[col]:
                        name = table.schema.columns[col].name
                        raise ExecutionError(
                            f"Duplicate value {v!r} column '{name}' mein -- is column mein har value alag honi chahiye"
                        )
                seen[col].add(v)

    def _check_fk(self, table: Table, new_rows: list[list]) -> None:
        """
        SANDARBH (FOREIGN KEY), child side: every non-KHALI value must already
        exist in the parent's column. Uses the parent's hash index (see
        table.py Table.indexes()) for an O(1) check per value -- possible only
        because _check_fk_target() required the parent column to be MUKHYA
        KUNJI/ANOKHA, so it always has one. For a self-reference, a value is
        also accepted if it appears among the OTHER rows of this very
        statement (e.g. inserting a whole org chart in one DAALO).
        """
        schema = table.schema
        for pos, col in enumerate(schema.columns):
            if not col.ref_table:
                continue
            is_self = col.ref_table == schema.name
            parent = table if is_self else self._table(col.ref_table)
            parent_pos = parent.schema.index_of(col.ref_column)
            parent_index = parent.indexes().get(parent_pos, {})
            local_values = {r[parent_pos] for r in new_rows if r[parent_pos] is not None} if is_self else ()
            for row in new_rows:
                v = row[pos]
                if v is None or v in parent_index or v in local_values:
                    continue
                raise ExecutionError(
                    f"Column '{col.name}': value {format_value(v)} table '{col.ref_table}' ke column "
                    f"'{col.ref_column}' mein nahi mila (SANDARBH)"
                )

    def _check_no_children(
        self, schema: TableSchema, changed_by_column: dict, exempt_row_ids: frozenset = frozenset(),
        overrides: Optional[dict] = None,
    ) -> None:
        """
        SANDARBH (FOREIGN KEY), parent side: RESTRICT. Refuse a DELETE/UPDATE
        on `schema` if any child row still points at a value that is
        disappearing from it. No CASCADE (future work -- see docs/REPORT.md).

        changed_by_column: {parent column position -> {values going away}}.
        exempt_row_ids (DELETE): these rows of `schema` are themselves being
        removed, so a self-referencing child among them doesn't count.
        overrides (UPDATE): {row_id -> new values} for rows THIS statement is
        also updating, so a self-referencing child's about-to-change value is
        checked, not its about-to-be-stale one.
        """
        overrides = overrides or {}
        for other_name, other in self.catalog.tables.items():
            for child_col in other.columns:
                if child_col.ref_table != schema.name:
                    continue
                parent_pos = schema.index_of(child_col.ref_column)
                removed = changed_by_column.get(parent_pos)
                if not removed:
                    continue
                is_self = other_name == schema.name
                child_pos = other.index_of(child_col.name)
                for row_id, values in self._table(other_name).rows():
                    if is_self and row_id in exempt_row_ids:
                        continue
                    current = overrides.get(row_id, values) if is_self else values
                    v = current[child_pos]
                    if v is not None and v in removed:
                        raise ExecutionError(
                            f"Table '{schema.name}' mein ye value(s) hata/badal nahi sakte -- table "
                            f"'{other_name}' ka column '{child_col.name}' (SANDARBH {schema.name}."
                            f"{child_col.ref_column}) abhi bhi inhe use karta hai"
                        )


@dataclass
class SelectPlan:
    """Everything the planner decided about one DIKHAO (also what SAMJHAO prints)."""

    tables: list[Table]
    scope: Scope
    labels: list[str]
    outputs: list[ast.Expr]
    where: Optional[ast.Expr]
    group_by: list[ast.Expr]
    having: Optional[ast.Expr]
    order_by: list[ast.OrderItem]
    aggregates: list[ast.FuncCall] = field(default_factory=list)
    grouped: bool = False
    access: Optional[planner.IndexLookup] = None
    joins: list[tuple] = field(default_factory=list)  # (Join, bound PAR, hash keys or None)


def _join(left_rows: list[dict], right_rows: list[dict], on, hash_keys, kind: str, null_left: dict, null_right: dict):
    """
    Combine every left row with the right rows that satisfy PAR.

    HASH JOIN (when PAR has `left.x = right.y`): put the right rows in a dict
    keyed by y, then each left row finds its partners in O(1).  O(n + m)
    NESTED LOOP (anything else): try every pair.                  O(n * m)

    kind:
      INNER/NATURAL  -- only matched rows (NATURAL just has a synthesised `on`)
      LEFT   (BAAYAN MILAO)  -- every left row kept once, KHALI-padded if unmatched
      RIGHT  (DAHINA MILAO)  -- every right row kept once, KHALI-padded if unmatched
      FULL   (DONO MILAO)    -- both: LEFT semantics, PLUS any right row that
                                 matched nothing, KHALI-padded on the left side
    """
    if kind == "RIGHT":
        # Symmetric to LEFT but on the OTHER side: build the hash index on
        # left_rows, drive the loop from right_rows -- but the OUTPUT still
        # looks like "left columns then right columns" (dict order doesn't
        # matter for evaluate(), only the keys do).
        if hash_keys is not None:
            left_key, right_key = hash_keys
            buckets: dict = {}
            for l in left_rows:
                if l[left_key] is not None:
                    buckets.setdefault(l[left_key], []).append(l)

            def partners(r_row):
                return buckets.get(r_row[right_key], []) if r_row[right_key] is not None else []

        else:

            def partners(r_row):
                return left_rows

        out = []
        for r_row in right_rows:
            matched = False
            for l_row in partners(r_row):
                combined = {**l_row, **r_row}
                if is_true(evaluate(on, combined)):
                    out.append(combined)
                    matched = True
            if not matched:
                out.append({**null_left, **r_row})
        return out

    keep_left_unmatched = kind in ("LEFT", "FULL")
    keep_right_unmatched = kind == "FULL"

    if hash_keys is not None:
        left_key, right_key = hash_keys
        buckets: dict = {}
        for r in right_rows:
            if r[right_key] is not None:  # KHALI never equals anything, so it never joins
                buckets.setdefault(r[right_key], []).append(r)

        def partners(l_row):
            return buckets.get(l_row[left_key], []) if l_row[left_key] is not None else []

    else:

        def partners(l_row):
            return right_rows

    out = []
    matched_right_ids: set = set()
    for l_row in left_rows:
        matched = False
        for r_row in partners(l_row):
            combined = {**l_row, **r_row}
            if is_true(evaluate(on, combined)):  # re-check the full PAR (it may have more conditions)
                out.append(combined)
                matched = True
                if keep_right_unmatched:
                    matched_right_ids.add(id(r_row))
        if keep_left_unmatched and not matched:
            out.append({**l_row, **null_right})

    if keep_right_unmatched:
        # FULL = LEFT (matched + left-unmatched-padded) UNION right-only-unmatched
        for r_row in right_rows:
            if id(r_row) not in matched_right_ids:
                out.append({**null_left, **r_row})
    return out


def _sort_key(value):
    # KHALI sorts before every real value (in SEEDHA / ascending order)
    return (0, 0) if value is None else (1, value)
