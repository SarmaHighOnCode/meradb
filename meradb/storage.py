"""
STAGE 4 (bottom layer): STORAGE. How rows actually live on disk.

We do NOT use JSON/CSV/pickle for table data. We define our own binary format,
because designing that is what a real database does.

There are two pieces:

1. RECORD CODEC  -- turns one row (a Python list) into bytes and back.

   For each column value we write a 1-byte tag, then the value:

       tag 0x00                 -> KHALI (NULL), nothing follows
       tag 0x01 + 8 bytes       -> INT   (signed, little-endian)       struct '<q'
       tag 0x01 + 8 bytes       -> FLOAT (IEEE-754 double)             struct '<d'
       tag 0x01 + 1 byte        -> BOOL  (0 or 1)
       tag 0x01 + 4 bytes + N   -> TEXT  (length N, then N UTF-8 bytes)
       tag 0x01 + 4 bytes       -> DATE  (signed, proleptic Gregorian ordinal) struct '<i'

   The column TYPES are not stored in the row -- they come from the catalog.
   That's why reading a row needs the schema.

2. HEAP FILE  -- one file per table (`<table>.tbl`). "Heap" = rows in no
   particular order, new rows are appended at the end.

       +------------------+
       | "MERADB01"       |  8-byte magic header: proves this is our file
       +------------------+
       | status | length | payload ... |   record 1
       | status | length | payload ... |   record 2
       | ...                             |
       +------------------+

       status : 1 byte   1 = live, 0 = deleted (a "tombstone")
       length : 4 bytes  size of payload
       payload: the encoded row

   A row's ROW ID is simply its byte offset in the file.
   DELETE only flips the status byte to 0 -- that's 1 byte written, very fast.
   The dead bytes stay in the file until `compact()` rewrites it.

Learn more:
  - Python struct module: https://docs.python.org/3/library/struct.html
  - CMU 15-445 lectures 3-4 (Database Storage) on YouTube
  - SQLite file format: https://www.sqlite.org/fileformat.html
"""

import os
import struct
from datetime import date
from typing import Iterator

from .errors import StorageError

MAGIC = b"MERADB01"
HEADER_SIZE = len(MAGIC)

STATUS_LIVE = 1
STATUS_DELETED = 0
RECORD_HEADER = struct.Struct("<BI")  # status (1 byte) + payload length (4 bytes)

TAG_NULL = 0
TAG_VALUE = 1


# ============================================================================
# 1. Record codec
# ============================================================================


def encode_row(values: list, types: list[str]) -> bytes:
    out = bytearray()
    for value, type_name in zip(values, types):
        if value is None:
            out.append(TAG_NULL)
            continue
        out.append(TAG_VALUE)
        if type_name == "INT":
            out += struct.pack("<q", value)
        elif type_name == "FLOAT":
            out += struct.pack("<d", value)
        elif type_name == "BOOL":
            out.append(1 if value else 0)
        elif type_name == "TEXT":
            data = value.encode("utf-8")
            out += struct.pack("<I", len(data))
            out += data
        elif type_name == "DATE":
            out += struct.pack("<i", value.toordinal())
        else:
            raise StorageError(f"Unknown type {type_name}")
    return bytes(out)


def decode_row(payload: bytes, types: list[str]) -> list:
    values = []
    i = 0
    for type_name in types:
        tag = payload[i]
        i += 1
        if tag == TAG_NULL:
            values.append(None)
        elif type_name == "INT":
            values.append(struct.unpack_from("<q", payload, i)[0])
            i += 8
        elif type_name == "FLOAT":
            values.append(struct.unpack_from("<d", payload, i)[0])
            i += 8
        elif type_name == "BOOL":
            values.append(payload[i] == 1)
            i += 1
        elif type_name == "TEXT":
            (length,) = struct.unpack_from("<I", payload, i)
            i += 4
            values.append(payload[i : i + length].decode("utf-8"))
            i += length
        elif type_name == "DATE":
            values.append(date.fromordinal(struct.unpack_from("<i", payload, i)[0]))
            i += 4
        else:
            raise StorageError(f"Unknown type {type_name}")
    return values


# ============================================================================
# 2. Heap file
# ============================================================================


class HeapFile:
    def __init__(self, path: str):
        self.path = path

    # ---- lifecycle ----
    def create(self) -> None:
        with open(self.path, "wb") as f:
            f.write(MAGIC)

    def destroy(self) -> None:
        if os.path.exists(self.path):
            os.remove(self.path)

    def truncate(self) -> None:
        """Remove every row but keep the (empty) file."""
        self.create()

    # ---- row operations ----
    def insert(self, payload: bytes) -> int:
        """Append a record at the end. Returns its row id (byte offset)."""
        return self.insert_many([payload])[0]

    def insert_many(self, payloads: list[bytes]) -> list[int]:
        """
        Append many records while opening the file only ONCE. Opening a file is
        slow compared to writing a few bytes: 50,000 single inserts took ~27 s,
        one batch takes well under a second.
        """
        offsets = []
        with open(self.path, "ab") as f:
            f.seek(0, os.SEEK_END)  # tell() in append mode is only reliable after this
            for payload in payloads:
                offsets.append(f.tell())
                f.write(RECORD_HEADER.pack(STATUS_LIVE, len(payload)))
                f.write(payload)
        return offsets

    def delete(self, offset: int) -> None:
        """Mark the record at `offset` as deleted by overwriting its status byte."""
        self.delete_many([offset])

    def delete_many(self, offsets: list[int]) -> None:
        with open(self.path, "r+b") as f:
            for offset in offsets:
                f.seek(offset)
                f.write(bytes([STATUS_DELETED]))

    def read(self, offset: int):
        """Read ONE record directly by its row id (used by index lookups). None if deleted."""
        with open(self.path, "rb") as f:
            f.seek(offset)
            header = f.read(RECORD_HEADER.size)
            if len(header) < RECORD_HEADER.size:
                raise StorageError(f"Row id {offset} file ke bahar hai")
            status, length = RECORD_HEADER.unpack(header)
            return f.read(length) if status == STATUS_LIVE else None

    def scan(self) -> Iterator[tuple[int, bytes]]:
        """Yield (row_id, payload) for every LIVE record, in file order."""
        with open(self.path, "rb") as f:
            if f.read(HEADER_SIZE) != MAGIC:
                raise StorageError(f"{self.path} MeraDB ki file nahi hai (magic header galat)")
            while True:
                offset = f.tell()
                header = f.read(RECORD_HEADER.size)
                if not header:
                    return  # clean end of file
                if len(header) < RECORD_HEADER.size:
                    raise StorageError(f"{self.path} corrupt hai: record header adhoora hai")
                status, length = RECORD_HEADER.unpack(header)
                payload = f.read(length)
                if len(payload) < length:
                    raise StorageError(f"{self.path} corrupt hai: record adhoora hai")
                if status == STATUS_LIVE:
                    yield offset, payload

    def rewrite(self, payloads: list[bytes]) -> None:
        """
        Replace the whole file with exactly these records.

        We write to a temporary file first and then atomically swap it in with
        os.replace(). If the program crashes halfway, the old file is untouched.
        This trick is used by ALTER TABLE and compact().
        """
        tmp_path = self.path + ".tmp"
        with open(tmp_path, "wb") as f:
            f.write(MAGIC)
            for payload in payloads:
                f.write(RECORD_HEADER.pack(STATUS_LIVE, len(payload)))
                f.write(payload)
        os.replace(tmp_path, self.path)

    def compact(self) -> None:
        """Throw away tombstones (deleted records) to reclaim disk space."""
        self.rewrite([payload for _, payload in self.scan()])
