"""
The CATALOG: the database's "data about data" (metadata).

It remembers which tables exist and what their columns are. Table *rows* live in
.tbl files (see storage.py); the *schemas* live here, in catalog.json.

Real databases store their catalog in special system tables (e.g. SQLite's
`sqlite_schema`). We use a small JSON file because metadata is tiny and being
able to open it in a text editor makes debugging much easier.

Layout on disk:

    data/
      main/                 <- one folder per database
        catalog.json        <- schemas of every table in this database
        students.tbl        <- rows of table `students`
        courses.tbl
      college/
        ...
"""

import json
import os
from dataclasses import asdict, dataclass, field
from datetime import date
from typing import Optional

from .errors import ExecutionError


@dataclass
class Column:
    name: str
    type_name: str
    primary_key: bool = False
    not_null: bool = False
    unique: bool = False
    default: object = None  # WARNA value used when an INSERT leaves this column out
    # Everything below is Optional with a default so OLD catalog.json files
    # (saved before these fields existed) still load fine.
    max_length: Optional[int] = None  # VARCHAR(n)/CHAR(n): max TEXT length, else None
    ref_table: Optional[str] = None  # SANDARBH (FOREIGN KEY): the parent table
    ref_column: Optional[str] = None  # SANDARBH: the parent column (MUKHYA KUNJI/ANOKHA there)
    check: Optional[str] = None  # SHART (CHECK): the constraint expression's SOURCE TEXT

    @property
    def is_unique(self) -> bool:
        return self.primary_key or self.unique

    @property
    def is_required(self) -> bool:
        return self.primary_key or self.not_null

    # ---- JSON (de)serialisation ----
    # dataclasses.asdict() can't turn a `date` WARNA default into JSON on its
    # own (json.dump would crash on it), and catalog.json / schema_tree() both
    # need it as plain data. So Column controls its own (de)serialisation.
    def to_dict(self) -> dict:
        d = asdict(self)
        if isinstance(d["default"], date):
            d["default"] = d["default"].isoformat()
        return d

    @staticmethod
    def from_dict(d: dict) -> "Column":
        d = dict(d)
        if d.get("type_name") == "DATE" and isinstance(d.get("default"), str):
            d["default"] = date.fromisoformat(d["default"])
        return Column(**d)


@dataclass
class TableSchema:
    name: str
    columns: list[Column] = field(default_factory=list)
    # Composite (multi-column) constraints -- a plain single-column ANOKHA/MUKHYA
    # KUNJI still lives on the Column itself (see is_unique above); these are
    # ONLY for the table-level `ANOKHA (a, b)` / `MUKHYA KUNJI (a, b)` form.
    composite_unique: list[list[str]] = field(default_factory=list)
    composite_pk: Optional[list[str]] = None

    @property
    def column_names(self) -> list[str]:
        return [c.name for c in self.columns]

    @property
    def types(self) -> list[str]:
        return [c.type_name for c in self.columns]

    def index_of(self, column: str) -> int:
        for i, c in enumerate(self.columns):
            if c.name == column:
                return i
        raise ExecutionError(f"Table '{self.name}' mein column '{column}' nahi hai")

    def get_column(self, column: str) -> Column:
        return self.columns[self.index_of(column)]

    @staticmethod
    def from_dict(d: dict) -> "TableSchema":
        # `composite_unique`/`composite_pk` are backward-compat: older
        # catalog.json files were written before these existed.
        return TableSchema(
            d["name"],
            [Column.from_dict(c) for c in d["columns"]],
            composite_unique=d.get("composite_unique", []),
            composite_pk=d.get("composite_pk"),
        )


class Catalog:
    FILE_NAME = "catalog.json"

    def __init__(self, db_dir: str):
        self.db_dir = db_dir
        self.path = os.path.join(db_dir, self.FILE_NAME)
        self.tables: dict[str, TableSchema] = {}
        self.views: dict[str, str] = {}  # view name -> its DIKHAO source text
        self._load()

    def _load(self) -> None:
        if not os.path.exists(self.path):
            self.tables = {}
            self.views = {}
            return
        with open(self.path, "r", encoding="utf-8") as f:
            data = json.load(f)
        self.tables = {name: TableSchema.from_dict(t) for name, t in data["tables"].items()}
        self.views = data.get("views", {})  # "views" key: absent in old catalog.json files

    def save(self) -> None:
        # Same temp-file-then-replace trick as HeapFile.rewrite: never leave
        # a half-written catalog behind if we crash mid-write.
        tmp = self.path + ".tmp"
        tables = {
            n: {
                "name": t.name,
                "columns": [c.to_dict() for c in t.columns],
                "composite_unique": t.composite_unique,
                "composite_pk": t.composite_pk,
            }
            for n, t in self.tables.items()
        }
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"tables": tables, "views": self.views}, f, indent=2)
        os.replace(tmp, self.path)

    def table_path(self, table: str) -> str:
        return os.path.join(self.db_dir, f"{table}.tbl")

    def get(self, table: str) -> TableSchema:
        schema = self.find(table)
        if schema is None:
            raise ExecutionError(f"Table '{table}' exist nahi karta")
        return schema

    def find(self, table: str) -> Optional[TableSchema]:
        return self.tables.get(table)

    def add(self, schema: TableSchema) -> None:
        self.tables[schema.name] = schema
        self.save()

    def remove(self, table: str) -> None:
        del self.tables[table]
        self.save()

    def add_view(self, name: str, query_text: str) -> None:
        self.views[name] = query_text
        self.save()

    def remove_view(self, name: str) -> None:
        del self.views[name]
        self.save()
