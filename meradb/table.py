"""
A Table = its schema (from the catalog) + its heap file (on disk) + its INDEXES.

INDEXES
-------
Without an index, `DIKHAO * SE s JAHAN id = 50000` must read EVERY row (a full
scan, O(n)). With an index it jumps straight to the right row (O(1)).

MeraDB automatically keeps a HASH INDEX on every MUKHYA KUNJI and ANOKHA column,
just like real databases automatically index primary keys and unique columns.
An index here is simply a Python dict:

        {value -> row_id}        e.g.  {1: 8, 2: 27, 3: 49}
                                        ^ id   ^ byte offset in students.tbl

Design choices (good viva material):
  * The index lives in MEMORY only. It is built the first time it is needed,
    with one full scan, and then kept up to date on every INSERT / DELETE.
  * Anything that rewrites the whole file (ALTER, SIKODO, SAAF, ROLLBACK)
    changes row ids, so it throws the index away. The next query rebuilds it.
  * Hash indexes answer `=` only. `<` / `>` would need a SORTED index (a B-tree),
    which is how real databases do it -- see docs/ARCHITECTURE.md.
  * The same index makes MUKHYA KUNJI / ANOKHA checks O(1) instead of a scan.
"""

from typing import Iterator, Optional

from .catalog import TableSchema
from .storage import HeapFile, decode_row, encode_row


class Table:
    def __init__(self, schema: TableSchema, path: str, index_cache: dict, cache_key: tuple):
        self.schema = schema
        self.heap = HeapFile(path)
        # The cache is shared by every session of the server, so an index built
        # by one client's query is reused by everyone.
        self._cache = index_cache
        self._key = cache_key

    # ---- reading ----
    def rows(self) -> Iterator[tuple[int, list]]:
        """Yield (row_id, values) for every live row: a FULL SCAN."""
        for row_id, payload in self.heap.scan():
            yield row_id, decode_row(payload, self.schema.types)

    def get(self, row_id: int) -> Optional[list]:
        payload = self.heap.read(row_id)
        return None if payload is None else decode_row(payload, self.schema.types)

    @staticmethod
    def _index_key(col, values: list):
        """`col` is either a single column POSITION (single-column ANOKHA/MUKHYA
        KUNJI) or a tuple of positions (a composite constraint) -- see
        `_composite_keys`. Returns the value/tuple to use as the index key, or
        None if it shouldn't be indexed (KHALI never participates)."""
        if isinstance(col, tuple):
            key = tuple(values[p] for p in col)
            return None if None in key else key
        return values[col] if values[col] is not None else None

    # ---- writing (keeps the indexes in sync) ----
    def insert_many(self, rows: list[list]) -> None:
        row_ids = self.heap.insert_many([encode_row(values, self.schema.types) for values in rows])
        indexes = self._cache.get(self._key)
        if indexes is not None:  # if not built yet, it will be built from the file later
            for values, row_id in zip(rows, row_ids):
                for col, index in indexes.items():
                    key = self._index_key(col, values)
                    if key is not None:
                        index[key] = row_id

    def delete_many(self, rows: list[tuple[int, list]]) -> None:
        """Delete (row_id, values) pairs. The values are needed to find the index entries."""
        self.heap.delete_many([row_id for row_id, _ in rows])
        indexes = self._cache.get(self._key)
        if indexes is not None:
            for row_id, values in rows:
                for col, index in indexes.items():
                    key = self._index_key(col, values)
                    # only remove the entry if it still points at THIS row (see UPDATE)
                    if key is not None and index.get(key) == row_id:
                        del index[key]

    # ---- indexes ----
    def _composite_keys(self) -> list[tuple[int, ...]]:
        """Every composite constraint (ANOKHA/MUKHYA KUNJI over 2+ columns) as a
        tuple of column POSITIONS, e.g. (1, 2) for `ANOKHA (student_id, course_id)`."""
        schema = self.schema
        groups = list(schema.composite_unique)
        if schema.composite_pk:
            groups.append(schema.composite_pk)
        return [tuple(schema.index_of(c) for c in group) for group in groups]

    def indexes(self) -> dict[int, dict]:
        """{column position -> {value -> row_id}} for every unique column, PLUS
        one entry per composite constraint keyed by a tuple of column positions
        (e.g. (1, 2)) whose dict is in turn keyed by a TUPLE of values instead
        of a single value. Built lazily."""
        indexes = self._cache.get(self._key)
        if indexes is None:
            single = {i: {} for i, c in enumerate(self.schema.columns) if c.is_unique}
            composite = {positions: {} for positions in self._composite_keys()}
            indexes = {**single, **composite}
            if indexes:
                for row_id, values in self.rows():
                    for col, index in single.items():
                        if values[col] is not None:
                            index[values[col]] = row_id
                    for positions, index in composite.items():
                        key = tuple(values[p] for p in positions)
                        if None not in key:  # KHALI never participates in a uniqueness violation
                            index[key] = row_id
            self._cache[self._key] = indexes
        return indexes

    def lookup(self, col: int, value) -> list[tuple[int, list]]:
        """Index lookup: the (row_id, values) with this value, or [] -- without a scan."""
        row_id = self.indexes()[col].get(value)
        if row_id is None:
            return []
        values = self.get(row_id)
        return [] if values is None else [(row_id, values)]

    def invalidate_indexes(self) -> None:
        self._cache.pop(self._key, None)


# ============================================================================
# MaterializedTable: a VIEW's result set, dressed up just enough to be used
# wherever a Table is used in SE/MILAO (see Engine._plan_select). It has no
# real storage -- a row_id is just its position in the materialized rows --
# and it deliberately does NOT support insert/delete/indexes: planner.choose_access
# guards against it (see `is_view`), so a view as the first FROM source always
# does a full scan of its (already computed) rows.
# ============================================================================


class MaterializedTable:
    is_view = True

    def __init__(self, schema: TableSchema, rows: list[list]):
        self.schema = schema
        self._rows = rows

    def rows(self) -> Iterator[tuple[int, list]]:
        return iter(enumerate(self._rows))

    def get(self, row_id: int) -> Optional[list]:
        return self._rows[row_id] if 0 <= row_id < len(self._rows) else None
