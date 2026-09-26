"""
The MeraDB SERVER: lets many clients use one database at the same time,
the way MySQL or other database servers do.

    meradb server            # run in this terminal (Ctrl+C to stop)
    meradb start             # run in the background

Architecture:

      meradb shell ─┐                        ┌─ Engine (session 1) ─┐
      workbench ────┼── TCP :6372 ── server ─┼─ Engine (session 2) ─┼── Instance ── data/
      your app ─────┘   (JSON lines)         └─ Engine (session 3) ─┘   (shared: catalogs,
                                                                         indexes, the lock)

  * One THREAD per connection (socketserver.ThreadingTCPServer).
  * Each connection gets its OWN Engine -> its own current database and
    transaction. They all share ONE Instance -> one lock, one cache.
  * The protocol is newline-delimited JSON -- see protocol.py.
  * If a client disconnects in the middle of a transaction, it is rolled back.
"""

import hmac
import os
import socketserver
import sys
import threading
import traceback
from datetime import datetime

from . import __version__
from . import ast_nodes as ast
from .engine import Engine, Instance, Result
from .errors import MeraDBError
from .protocol import (
    DEFAULT_HOST,
    DEFAULT_PORT,
    PROTOCOL_VERSION,
    ProtocolError,
    receive,
    remove_pid_file,
    running_server,
    send,
    write_pid_file,
)

LOOPBACK = ("127.0.0.1", "::1", "localhost")


class MeraDBServer(socketserver.ThreadingTCPServer):
    daemon_threads = True  # don't let a hanging client block shutdown
    # On Windows SO_REUSEADDR lets TWO servers bind the same port -- never want that.
    allow_reuse_address = os.name != "nt"

    def __init__(self, data_dir: str, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT,
                 password: str | None = None, verbose: bool = False, log=None):
        self.instance = Instance(data_dir, served=True)
        self.password = password or None
        self.verbose = verbose
        self._log_fn = log or (lambda line: print(line, flush=True))
        self.started = datetime.now()
        self.sessions = 0
        self._sessions_lock = threading.Lock()
        super().__init__((host, port), ClientHandler)

    @property
    def port(self) -> int:
        return self.server_address[1]

    def log(self, message: str) -> None:
        self._log_fn(f"{datetime.now():%Y-%m-%d %H:%M:%S}  {message}")

    def track(self, delta: int) -> None:
        with self._sessions_lock:
            self.sessions += delta


class ClientHandler(socketserver.StreamRequestHandler):
    server: MeraDBServer

    def handle(self) -> None:
        server = self.server
        peer = f"{self.client_address[0]}:{self.client_address[1]}"

        # ---- 1. handshake ----
        try:
            hello = receive(self.rfile)
        except (ProtocolError, OSError):
            return
        if not hello or hello.get("type") != "hello":
            return

        user = hello.get("user")
        if user:
            # A per-user login SUPERSEDES the single shared server password --
            # the whole-server password check below is skipped entirely for
            # this connection (see docs/SERVER.md "no username = superuser").
            if not server.instance.users.verify(str(user), str(hello.get("password") or "")):
                # bare message: the client wraps this in a ConnectionFailed exception,
                # whose own __str__ already adds "[Connection Galti] " -- adding it here
                # too would print it twice
                send(self.wfile, {"ok": False, "error": "User ya password galat hai"})
                server.log(f"{peer}  login fail (galat user/password: {user!r})")
                return
        elif server.password and not hmac.compare_digest(str(hello.get("password") or ""), server.password):
            send(self.wfile, {"ok": False, "error": "Password galat hai"})  # bare: see note above
            server.log(f"{peer}  login fail (galat password)")
            return

        session = Engine(server.instance)
        if user:
            session.user = str(user)
        server.track(+1)
        server.log(f"{peer}  connected{f' as {user!r}' if user else ''}  (active sessions: {server.sessions})")
        try:
            database = hello.get("database")
            if database:
                try:
                    session.execute_statement(ast.UseDatabase(str(database)))
                except MeraDBError as e:
                    # .message not str(e): the client wraps this in ConnectionFailed,
                    # which would double the "[Stage Galti] " tag otherwise
                    send(self.wfile, {"ok": False, "error": e.message})
                    return
            send(self.wfile, {"ok": True, "server": f"MeraDB {__version__}", "protocol": PROTOCOL_VERSION,
                              "database": session.current_db})

            # ---- 2. request loop ----
            while True:
                try:
                    request = receive(self.rfile)
                except ProtocolError as e:
                    send(self.wfile, {"ok": False, "error": f"[Protocol Galti] {e}"})
                    continue
                if request is None:
                    break  # client closed the connection
                reply, stop = self.dispatch(session, request, peer)
                send(self.wfile, reply)
                if stop:
                    break
        except (ConnectionError, OSError):
            pass  # client vanished
        finally:
            session.close()  # rolls back an unfinished transaction
            server.track(-1)
            server.log(f"{peer}  disconnected  (active sessions: {server.sessions})")

    def dispatch(self, session: Engine, request: dict, peer: str) -> tuple[dict, bool]:
        server = self.server
        kind = request.get("type")

        if kind == "query":
            text = str(request.get("text", ""))
            if server.verbose:
                server.log(f"{peer}  [{session.current_db}]  {' '.join(text.split())[:200]}")
            try:
                results = session.run_script(text)
            except Exception as e:  # a bug must not kill the whole server
                server.log(f"{peer}  INTERNAL ERROR\n{traceback.format_exc()}")
                results = [Result(error=f"[Internal Galti] {e!r}")]
            for r in results:
                if r.error:
                    server.log(f"{peer}  {r.error}")
            return {
                "ok": True,
                "results": [r.to_dict() for r in results],
                "database": session.current_db,
                "in_transaction": session.in_transaction,
            }, False

        if kind == "schema":
            try:
                return {"ok": True, "tree": session.schema_tree()}, False
            except MeraDBError as e:
                return {"ok": False, "error": e.message}, False  # see note above: schema_tree() also wraps in ConnectionFailed

        if kind == "status":
            return {
                "ok": True,
                "server": f"MeraDB {__version__}",
                "pid": os.getpid(),
                "data_dir": server.instance.data_dir,
                "started": server.started.isoformat(timespec="seconds"),
                "sessions": server.sessions,
                "databases": server.instance.databases(),
            }, False

        if kind == "ping":
            return {"ok": True}, False

        if kind == "shutdown":
            if self.client_address[0] not in LOOPBACK:
                return {"ok": False, "error": "Shutdown sirf usi computer se ho sakta hai jahan server chal raha hai"}, False
            server.log(f"{peer}  shutdown requested")
            # shutdown() waits for serve_forever() to stop, so call it from another thread
            threading.Thread(target=server.shutdown, daemon=True).start()
            return {"ok": True}, True

        return {"ok": False, "error": f"Unknown request type: {kind!r}"}, False


def serve(data_dir: str, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT,
          password: str | None = None, verbose: bool = False) -> int:
    """Run the server in the foreground until Ctrl+C or a `meradb stop`."""
    data_dir = os.path.abspath(data_dir)
    existing = running_server(data_dir) if os.path.isdir(data_dir) else None
    if existing:
        print(f"Is data folder ka server pehle se chal raha hai: {existing['host']}:{existing['port']} "
              f"(pid {existing['pid']})", file=sys.stderr)
        return 1
    try:
        server = MeraDBServer(data_dir, host, port, password, verbose)
    except OSError as e:
        print(f"Server start nahi hua ({host}:{port}): {e}", file=sys.stderr)
        return 1
    except MeraDBError as e:
        print(e, file=sys.stderr)
        return 1

    pid = os.getpid()
    write_pid_file(data_dir, {"pid": pid, "host": host, "port": server.port,
                              "started": server.started.isoformat(timespec="seconds")})
    server.log(f"MeraDB {__version__} server chal raha hai  ->  {host}:{server.port}")
    server.log(f"data folder: {data_dir}")
    for db in server.instance.recovered:
        server.log(f"RECOVERY: database '{db}' ka adhoora transaction WAPAS kiya (pichli baar crash hua tha)")
    if not password:
        server.log("password: nahi (koi bhi connect kar sakta hai)")
        if host not in LOOPBACK:
            server.log("WARNING: bina password ke network par khula hai! --password use karo")
    server.log("band karne ke liye: Ctrl+C  ya  meradb stop")
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        server.log("Ctrl+C -- band ho raha hai")
    finally:
        server.server_close()
        remove_pid_file(data_dir, pid)
        server.log("server band")
    return 0
