"""
The MeraDB CLIENT library: talk to a running server from Python.

    from meradb.client import Connection

    with Connection("127.0.0.1", 6372) as db:
        for result in db.run_script("DIKHAO * SE students;"):
            print(result.columns, result.rows)

A Connection has the same methods as an embedded Engine (run_script,
schema_tree, current_db, in_transaction, close), so the shell and the
workbench can use either one without knowing which it is.
"""

import socket

from .engine import Result
from .errors import ConnectionFailed, MeraDBError, ServerUnavailable
from .protocol import DEFAULT_HOST, DEFAULT_PORT, PROTOCOL_VERSION, ProtocolError, receive, send


class Connection:
    def __init__(self, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT, password: str | None = None,
                 database: str | None = None, timeout: float = 60.0, user: str | None = None):
        self.host, self.port = host, port
        try:
            self._sock = socket.create_connection((host, port), timeout=5)
        except OSError as e:
            raise ServerUnavailable(f"{host}:{port} par MeraDB server nahi mila ({e.strerror or e})") from e
        # queries may legitimately wait (up to ~10 s) for another client's transaction
        self._sock.settimeout(timeout)
        self._rfile = self._sock.makefile("rb")
        self._wfile = self._sock.makefile("wb")

        # `user`, if given, authenticates as that SPECIFIC user (checked against
        # users.json, see users.py) instead of the single shared server password
        # -- see docs/SERVER.md "no username = superuser".
        reply = self._request({"type": "hello", "version": PROTOCOL_VERSION,
                               "password": password, "database": database, "user": user})
        if not reply.get("ok"):
            self.close()
            raise ConnectionFailed(reply.get("error", "Server ne connection mana kar diya"))
        self.server_version = reply.get("server", "MeraDB")
        self.current_db = reply.get("database", "main")
        self.in_transaction = False

    @property
    def description(self) -> str:
        return f"{self.host}:{self.port}"

    def _request(self, message: dict) -> dict:
        try:
            send(self._wfile, message)
            reply = receive(self._rfile)
        except (OSError, ProtocolError) as e:
            raise ConnectionFailed(f"Server se connection toot gaya: {e}") from e
        if reply is None:
            raise ConnectionFailed("Server ne connection band kar diya")
        return reply

    # ---- same API as Engine ----
    def run_script(self, text: str) -> list[Result]:
        reply = self._request({"type": "query", "text": text})
        if not reply.get("ok"):
            return [Result(error=reply.get("error", "Unknown error"))]
        self.current_db = reply.get("database", self.current_db)
        self.in_transaction = reply.get("in_transaction", False)
        return [Result.from_dict(r) for r in reply["results"]]

    def execute(self, text: str) -> list[Result]:
        """Like run_script, but raises the first error (same as Engine.execute)."""
        results = self.run_script(text)
        for r in results:
            if r.error:
                raise MeraDBError(r.error)
        return results

    def schema_tree(self) -> list[dict]:
        reply = self._request({"type": "schema"})
        if not reply.get("ok"):
            raise ConnectionFailed(reply.get("error", "schema nahi mila"))
        return reply["tree"]

    def status(self) -> dict:
        return self._request({"type": "status"})

    def shutdown(self) -> None:
        reply = self._request({"type": "shutdown"})
        if not reply.get("ok"):
            raise ConnectionFailed(reply.get("error", "shutdown fail"))

    def close(self) -> None:
        for f in (getattr(self, "_rfile", None), getattr(self, "_wfile", None), getattr(self, "_sock", None)):
            try:
                if f is not None:
                    f.close()
            except OSError:
                pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
