"""
The MeraDB WIRE PROTOCOL: how a client and the server talk over TCP.

Big databases use compact binary protocols; ours is deliberately simple and readable:
**one JSON object per line** ("newline-delimited JSON"). You could even talk to
the server by hand with a raw TCP tool.

    client -> server   {"type": "hello", "version": 1, "password": "...", "database": "main",
                        "user": null}
    server -> client   {"ok": true, "server": "MeraDB 1.0.0", "database": "main"}

Optional "user" (Phase B): if present, the server authenticates that SPECIFIC user
against users.json (see users.py) instead of the single shared server password, and
that session becomes subject to privilege checks from then on. Absent/null "user" =
today's behaviour exactly (shared-password check only, unrestricted superuser session)
-- see docs/SERVER.md "Users and privileges".

    client -> server   {"type": "query", "text": "DIKHAO * SE s;"}
    server -> client   {"ok": true, "database": "main", "in_transaction": false,
                        "results": [{"columns": [...], "rows": [[...]], "message": "...", "error": ""}]}

Other request types: "schema" (for the workbench sidebar), "status", "ping",
"shutdown" (only accepted from the same machine).

Every connection starts with "hello". If the server has a password and it is
wrong, the server answers {"ok": false, "error": ...} and closes the connection.

This file also manages the server's PID FILE (`<data>/meradb.pid`), the same
idea real database servers use: it records which process/port is serving a
data folder, so a second server (or an embedded engine) can't open the same
files at the same time and corrupt them.
"""

import json
import os
import socket
from typing import Optional

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 6372  # M-E-R-A on a phone keypad
PROTOCOL_VERSION = 1
MAX_MESSAGE_BYTES = 64 * 1024 * 1024  # refuse absurdly large lines
PID_FILE = "meradb.pid"


class ProtocolError(Exception):
    pass


def send(wfile, message: dict) -> None:
    # json.dumps never emits a raw newline, so "\n" safely marks the end of a message
    wfile.write((json.dumps(message) + "\n").encode("utf-8"))
    wfile.flush()


def receive(rfile) -> Optional[dict]:
    """Read one message. None means the other side closed the connection."""
    line = rfile.readline(MAX_MESSAGE_BYTES + 1)
    if not line:
        return None
    if len(line) > MAX_MESSAGE_BYTES:
        raise ProtocolError("Message bahut bada hai")
    try:
        message = json.loads(line.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        raise ProtocolError(f"Galat message: {e}") from e
    if not isinstance(message, dict):
        raise ProtocolError("Message ek JSON object hona chahiye")
    return message


def default_data_dir() -> str:
    """
    Where databases live unless -D / --data says otherwise. Like a real database server, this is
    ONE fixed place per user (not "wherever you happen to run the command"):
        Windows: %LOCALAPPDATA%\\MeraDB\\data      others: ~/.local/share/MeraDB/data
    The MERADB_DATA environment variable overrides it.
    """
    if os.environ.get("MERADB_DATA"):
        return os.environ["MERADB_DATA"]
    base = os.environ.get("LOCALAPPDATA") if os.name == "nt" else None
    base = base or os.path.join(os.path.expanduser("~"), ".local", "share")
    return os.path.join(base, "MeraDB", "data")


# ----------------------------------------------------------------------------
# PID file
# ----------------------------------------------------------------------------


def pid_file_path(data_dir: str) -> str:
    return os.path.join(data_dir, PID_FILE)


def read_pid_file(data_dir: str) -> Optional[dict]:
    try:
        with open(pid_file_path(data_dir), "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def write_pid_file(data_dir: str, info: dict) -> None:
    with open(pid_file_path(data_dir), "w", encoding="utf-8") as f:
        json.dump(info, f, indent=2)


def remove_pid_file(data_dir: str, pid: int) -> None:
    """Remove the pid file, but only if it still belongs to process `pid`."""
    info = read_pid_file(data_dir)
    if info and info.get("pid") == pid:
        try:
            os.remove(pid_file_path(data_dir))
        except OSError:
            pass


def port_open(host: str, port: int, timeout: float = 0.5) -> bool:
    # We check "is something listening?" rather than "is the pid alive?" because
    # checking a pid is not portable (on Windows, os.kill(pid, 0) KILLS the process!).
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def running_server(data_dir: str) -> Optional[dict]:
    """The pid-file info if a server is really serving this data folder, else None."""
    info = read_pid_file(data_dir)
    if not info:
        return None
    host = info.get("host", DEFAULT_HOST)
    if host in ("0.0.0.0", "", "::"):
        host = DEFAULT_HOST
    if port_open(host, int(info.get("port", DEFAULT_PORT))):
        return info
    return None  # stale pid file left behind by a crashed server
