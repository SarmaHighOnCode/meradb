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
no bcrypt/passlib needed) with a random per-user salt and 600,000 iterations,
the same technique Django/Flask use under the hood. An entry's work factor is
kept in its "iterations" key (none = 100,000, written by an older version) and an
account on a lower one is rehashed at its next successful login. See
docs/ARCHITECTURE.md for why this is "good enough for a teaching project" but
not production-grade (no salt pepper, no password rotation, ...).

HONEST SCOPE (see docs/LANGUAGE.md "Users & privileges" for the full list):
grants are per (database, table) -- not schema-level roles, not column-level
grants, no WITH GRANT OPTION, no password rotation/expiry.
"""

import hashlib
import hmac
import json
import os
import secrets
import tempfile
import threading
from typing import Optional

from .errors import ExecutionError

FILE_NAME = "users.json"
PBKDF2_ITERATIONS = 600_000
PBKDF2_LEGACY_ITERATIONS = 100_000  # an entry with no "iterations" key
PBKDF2_MAX_ITERATIONS = 100_000_000  # more than this in a file is damage: it would hang a login
ALL_PRIVILEGES = ["DIKHAO", "DAALO", "BADLO", "MITAO"]


class UserStore:
    def __init__(self, data_dir: str):
        self.path = os.path.join(data_dir, FILE_NAME)
        self.users: dict[str, dict] = {}
        self._save_lock = threading.RLock()  # a login's rehash runs outside the engine's statement lock
        self.load()

    # ---- persistence (same temp-file-then-os.replace() atomic pattern as Catalog.save()) ----
    def load(self) -> None:
        if not os.path.exists(self.path):
            self.users = {}
            return
        with open(self.path, "r", encoding="utf-8") as f:
            self.users = json.load(f)

    def save(self) -> None:
        with self._save_lock:
            # mkstemp: owner only on POSIX from the first byte, under a name nobody can prepare a link for. (The Windows
            # access list is the C++ engine's work, see docs/SERVER.md.)
            fd, tmp = tempfile.mkstemp(prefix="users.json.", suffix=".tmp", dir=os.path.dirname(self.path) or ".")
            try:
                with os.fdopen(fd, "w", encoding="utf-8") as f:
                    json.dump(self.users, f, indent=2)
                os.replace(tmp, self.path)
            except BaseException:
                try:
                    os.unlink(tmp)
                except OSError:
                    pass
                raise

    # ---- user management ----
    def exists(self, name: str) -> bool:
        return name in self.users

    def has_logins(self) -> bool:
        """Is there any account that can log in? (A role written by the C++ engine cannot.)"""
        return any(not (isinstance(u, dict) and u.get("role") is True) for u in self.users.values())

    def create(self, name: str, password: str) -> None:
        if name in self.users:
            raise ExecutionError(f"User '{name}' pehle se hai")
        if not password:
            raise ExecutionError(f"User '{name}' ka password khali nahi ho sakta")
        salt = secrets.token_bytes(16)
        self.users[name] = {
            "salt": salt.hex(),
            "hash": self._hash(password, salt).hex(),
            "iterations": PBKDF2_ITERATIONS,
            "grants": {},
        }
        self.save()

    def drop(self, name: str) -> None:
        if name not in self.users:
            raise ExecutionError(f"User '{name}' exist nahi karta")
        del self.users[name]
        self.save()

    def verify(self, name: str, password: str) -> bool:
        password = password or ""
        stored = self._stored_hash(name)
        if stored is None:  # no such login: do the same work, so timing does not tell which names exist
            self._hash(password, bytes(16))
            return False
        salt, expected, iterations = stored
        if not hmac.compare_digest(self._hash(password, salt, iterations).hex().encode(), expected.encode("utf-8", "replace")):
            return False
        if iterations < PBKDF2_ITERATIONS:
            self._upgrade(name, expected, password)
        return True

    def _stored_hash(self, name: str) -> Optional[tuple[bytes, str, int]]:
        """(salt, hash, iterations), or None for a missing or malformed entry: those never authenticate."""
        user = self.users.get(name)
        if not isinstance(user, dict):
            return None
        iterations = user.get("iterations", PBKDF2_LEGACY_ITERATIONS)
        if type(iterations) is not int or not 1 <= iterations <= PBKDF2_MAX_ITERATIONS:
            return None
        try:
            return bytes.fromhex(user["salt"]), str(user["hash"]), iterations
        except (KeyError, TypeError, ValueError):
            return None

    def _upgrade(self, name: str, verified_hash: str, password: str) -> None:
        """The one moment the password is at hand: bring the account up to today's work factor. The login has
        already succeeded, so a failure here only leaves the old hash in place for next time."""
        salt = secrets.token_bytes(16)
        upgraded = self._hash(password, salt).hex()
        with self._save_lock:
            user = self.users.get(name)
            if not isinstance(user, dict) or user.get("hash") != verified_hash:
                return  # a concurrent DROP or another login's upgrade wins
            before = dict(user)
            user.update(salt=salt.hex(), hash=upgraded, iterations=PBKDF2_ITERATIONS)
            try:
                self.save()
            except OSError:
                user.clear()
                user.update(before)

    @staticmethod
    def _hash(password: str, salt: bytes, iterations: int = PBKDF2_ITERATIONS) -> bytes:
        return hashlib.pbkdf2_hmac("sha256", password.encode("utf-8", "surrogatepass"), salt, iterations)

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
