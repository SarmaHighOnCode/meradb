"""
USERS & PRIVILEGES: a small, SERVER-WIDE user store (mirrors catalog.py's shape).

Real databases separate "server-level" concepts (users, logins) from
"database-level" objects (tables, views) -- a user can connect to a server and
then be granted access to specific things inside specific databases. We model
that same split: one users.json file lives at the TOP of the data directory
(not inside any one database folder), because a MeraDB server serves one
Instance (one data dir) that can hold several databases, and a user is a
property of the SERVER, not of any single database.

Layout on disk -- `<data_dir>/users.json`:

    {
      "ravi": {
        "salt": "<hex>", "hash": "<hex>",
        "grants": {"main.students": ["DIKHAO", "DAALO"]}
      }
    }

Passwords are NEVER stored in the clear: `hashlib.pbkdf2_hmac` (stdlib,
no bcrypt/passlib needed) with a random per-user salt and 100,000 iterations,
the same technique Django/Flask use under the hood. See docs/ARCHITECTURE.md
for why this is "good enough for a teaching project" but not production-grade
(no salt pepper, no configurable work factor, no account lockout, ...).

HONEST SCOPE (see docs/LANGUAGE.md "Users & privileges" for the full list):
grants are per (database, table) -- not schema-level roles, not column-level
grants, no WITH GRANT OPTION, no password rotation/expiry.
"""

import hashlib
import json
import os
import secrets
from typing import Optional

from .errors import ExecutionError

FILE_NAME = "users.json"
PBKDF2_ITERATIONS = 100_000
ALL_PRIVILEGES = ["DIKHAO", "DAALO", "BADLO", "MITAO"]


class UserStore:
    def __init__(self, data_dir: str):
        self.path = os.path.join(data_dir, FILE_NAME)
        self.users: dict[str, dict] = {}
        self.load()

    # ---- persistence (same temp-file-then-os.replace() atomic pattern as Catalog.save()) ----
    def load(self) -> None:
        if not os.path.exists(self.path):
            self.users = {}
            return
        with open(self.path, "r", encoding="utf-8") as f:
            self.users = json.load(f)

    def save(self) -> None:
        tmp = self.path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(self.users, f, indent=2)
        os.replace(tmp, self.path)

    # ---- user management ----
    def exists(self, name: str) -> bool:
        return name in self.users

    def create(self, name: str, password: str) -> None:
        if name in self.users:
            raise ExecutionError(f"User '{name}' pehle se hai")
        salt = secrets.token_bytes(16)
        self.users[name] = {
            "salt": salt.hex(),
            "hash": self._hash(password, salt).hex(),
            "grants": {},
        }
        self.save()

    def drop(self, name: str) -> None:
        if name not in self.users:
            raise ExecutionError(f"User '{name}' exist nahi karta")
        del self.users[name]
        self.save()

    def verify(self, name: str, password: str) -> bool:
        user = self.users.get(name)
        if user is None:
            return False
        salt = bytes.fromhex(user["salt"])
        return self._hash(password or "", salt).hex() == user["hash"]

    @staticmethod
    def _hash(password: str, salt: bytes) -> bytes:
        return hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, PBKDF2_ITERATIONS)

    # ---- grants: keyed by "database.table" (a VIEW shares this same keyspace, see docs) ----
    def grant(self, name: str, db: str, table: str, privileges: list[str]) -> None:
        if name not in self.users:
            raise ExecutionError(f"User '{name}' exist nahi karta")
        key = f"{db}.{table}"
        current = set(self.users[name]["grants"].get(key, []))
        current.update(privileges)
        self.users[name]["grants"][key] = sorted(current, key=ALL_PRIVILEGES.index)
        self.save()

    def revoke(self, name: str, db: str, table: str, privileges: list[str]) -> None:
        if name not in self.users:
            raise ExecutionError(f"User '{name}' exist nahi karta")
        key = f"{db}.{table}"
        current = set(self.users[name]["grants"].get(key, []))
        current.difference_update(privileges)
        if current:
            self.users[name]["grants"][key] = sorted(current, key=ALL_PRIVILEGES.index)
        else:
            self.users[name]["grants"].pop(key, None)
        self.save()

    def has_privilege(self, name: str, db: str, table: str, privilege: str) -> bool:
        user = self.users.get(name)
        if user is None:
            return False
        return privilege in user["grants"].get(f"{db}.{table}", [])
