"""
The `meradb` command: one entry point for everything.

    meradb start      start the server in the background
    meradb stop       stop it
    meradb status     is it running?
    meradb server     run the server in this terminal
    meradb shell      interactive command-line client
    meradb workbench  full-screen client (like MySQL Workbench)
    meradb run FILE   run a script file

`meradb` with no command = `meradb shell`.

Clients (shell / workbench / run) connect to the server if one is running.
If none is running they fall back to LOCAL mode: the engine runs inside the
client process and opens the data folder directly (like SQLite).
"""

import argparse
import getpass
import os
import signal
import subprocess
import sys
import time

from . import __version__
from . import ast_nodes as ast
from .errors import MeraDBError, ServerUnavailable
from .protocol import (DEFAULT_HOST, DEFAULT_PORT, default_data_dir, port_open, read_pid_file,
                       remove_pid_file, running_server)

COMMANDS = {"server", "start", "stop", "status", "shell", "workbench", "tui", "run"}

EXAMPLES = """
examples:
  meradb start                      server ko background mein chalao
  meradb shell                      server se connect karo (command-line shell)
  meradb workbench                  full-screen UI (MySQL Workbench jaisa)
  meradb run examples/demo.mdb      script chalao
  meradb status / meradb stop
  meradb shell --local              bina server ke, seedha data folder par
"""


def note(message: str) -> None:
    """Side messages go to stderr so that `meradb run x.mdb > out.txt` stays clean."""
    print(message, file=sys.stderr)


# ============================================================================
# argument parsing
# ============================================================================


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="meradb",
        description="MeraDB -- apna database, apni bhasha.",
        epilog=EXAMPLES,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--version", action="version", version=f"MeraDB {__version__}")
    sub = parser.add_subparsers(dest="command", metavar="COMMAND")

    def data_arg(p):
        p.add_argument("-D", "--data", default=default_data_dir(),
                       help=f"data folder (default: {default_data_dir()}, ya MERADB_DATA)")

    def server_args(p):
        data_arg(p)
        p.add_argument("--host", default=DEFAULT_HOST,
                       help=f"kis address par suno (default {DEFAULT_HOST}; LAN ke liye 0.0.0.0)")
        p.add_argument("--port", type=int, default=int(os.environ.get("MERADB_PORT", DEFAULT_PORT)),
                       help=f"TCP port (default {DEFAULT_PORT}, ya MERADB_PORT)")
        p.add_argument("--password", default=os.environ.get("MERADB_PASSWORD"),
                       help="clients ko ye password dena hoga (ya MERADB_PASSWORD)")
        p.add_argument("-v", "--verbose", action="store_true", help="har query log karo")

    def client_args(p):
        data_arg(p)
        p.add_argument("-H", "--host", help=f"server ka address (default {DEFAULT_HOST}, ya MERADB_HOST)")
        p.add_argument("-p", "--port", type=int, help=f"server ka port (default {DEFAULT_PORT}, ya MERADB_PORT)")
        p.add_argument("-d", "--database", help="shuru mein ye database istemal karo")
        p.add_argument("-W", "--password", action="store_true", help="password poocho (ya MERADB_PASSWORD)")
        p.add_argument("-U", "--user", default=os.environ.get("MERADB_USER"),
                       help="is USERNAME se login karo (ya MERADB_USER) -- privileges ke saath, "
                            "server-wide password ki jagah")
        p.add_argument("--local", action="store_true", help="server ke bina, seedha data folder kholo")

    p = sub.add_parser("server", help="server isi terminal mein chalao (Ctrl+C se band)")
    server_args(p)
    p = sub.add_parser("start", help="server background mein chalao")
    server_args(p)
    p = sub.add_parser("stop", help="background server band karo")
    data_arg(p)
    p.add_argument("-W", "--password", action="store_true", help="password poocho (ya MERADB_PASSWORD)")
    p.add_argument("--force", action="store_true", help="agar normal shutdown na ho to process kill karo")
    p = sub.add_parser("status", help="server chal raha hai ya nahi")
    data_arg(p)
    p.add_argument("-W", "--password", action="store_true", help="password poocho (ya MERADB_PASSWORD)")
    p = sub.add_parser("shell", help="interactive command-line shell")
    client_args(p)
    p = sub.add_parser("workbench", aliases=["tui"], help="full-screen UI (MySQL Workbench jaisa)")
    client_args(p)
    p = sub.add_parser("run", help=".mdb script files chalao")
    client_args(p)
    p.add_argument("files", nargs="+", help="script files")
    return parser


def main(argv: list[str] | None = None) -> int:
    argv = list(sys.argv[1:] if argv is None else argv)
    # shortcuts: `meradb demo.mdb` = run, `meradb --tui` = workbench, `meradb` = shell
    if argv and argv[0].endswith(".mdb"):
        argv.insert(0, "run")
    elif argv and argv[0] == "--tui":
        argv[0] = "workbench"
    if not argv or (argv[0] not in COMMANDS and argv[0] not in ("-h", "--help", "--version")):
        argv.insert(0, "shell")

    args = build_parser().parse_args(argv)
    handlers = {
        "server": cmd_server, "start": cmd_start, "stop": cmd_stop, "status": cmd_status,
        "shell": cmd_shell, "workbench": cmd_workbench, "tui": cmd_workbench, "run": cmd_run,
    }
    try:
        return handlers[args.command](args)
    except MeraDBError as e:
        note(str(e))
        return 1
    except KeyboardInterrupt:
        return 130


# ============================================================================
# connecting (shared by shell / workbench / run)
# ============================================================================


def get_password(args) -> str | None:
    if getattr(args, "password", False) is True:
        return getpass.getpass("Password: ")
    return os.environ.get("MERADB_PASSWORD")


def open_backend(args, password: str | None = None):
    """A server Connection, or an embedded Engine (local mode). Raises MeraDBError."""
    from .client import Connection
    from .engine import Engine

    if password is None:
        password = get_password(args)
    if args.local:
        if getattr(args, "user", None):
            # --local means "trust the person running this process completely"
            # -- there is no server and no authentication, so a username has
            # nothing to check against. Note it and proceed as superuser,
            # rather than erroring (see docs/SERVER.md).
            note(f"(--local mode mein -U/--user '{args.user}' ka koi matlab nahi -- ignore kiya, superuser ki tarah chal raha hai)")
        return open_local(args)

    # Flags are an explicit choice (no fallback to local mode if that server is down);
    # MERADB_HOST / MERADB_PORT only change the defaults.
    explicit = args.host is not None or args.port is not None
    host = args.host or os.environ.get("MERADB_HOST") or DEFAULT_HOST
    port = args.port
    if port is None and os.environ.get("MERADB_PORT"):
        port = int(os.environ["MERADB_PORT"])
    if port is None:  # a server started on another port for this data folder? use that
        info = read_pid_file(os.path.abspath(args.data))
        port = int(info["port"]) if info and "port" in info else DEFAULT_PORT
    try:
        return Connection(host, port, password, args.database, user=getattr(args, "user", None))
    except ServerUnavailable:
        if explicit:
            raise
        note(f"({host}:{port} par server nahi mila -- LOCAL mode: seedha '{os.path.abspath(args.data)}' khol rahe hain. "
             f"Server ke liye: meradb start)")
        return open_local(args)


def open_local(args):
    from .engine import Engine

    engine = Engine(args.data)
    for db in engine.instance.recovered:
        note(f"RECOVERY: database '{db}' ka adhoora transaction WAPAS kiya (pichli baar crash hua tha)")
    if args.database:
        engine.execute_statement(ast.UseDatabase(args.database))
    return engine


# ============================================================================
# client commands
# ============================================================================


def cmd_shell(args) -> int:
    from .repl import repl

    backend = open_backend(args)
    try:
        repl(backend)
    finally:
        backend.close()
    return 0


def cmd_run(args) -> int:
    from .repl import run_file

    backend = open_backend(args)
    try:
        ok = [run_file(backend, path) for path in args.files]  # run ALL files, even after a failure
    finally:
        backend.close()
    return 0 if all(ok) else 1


def cmd_workbench(args) -> int:
    try:
        from .tui import run_workbench
    except ImportError:
        note("Workbench ke liye Textual chahiye. Install karo:  pip install -e .[workbench]"
             "   (ya: pip install -r requirements.txt)")
        return 1
    return run_workbench(open_backend(args), args)


# ============================================================================
# server commands
# ============================================================================


def cmd_server(args) -> int:
    from .server import serve

    return serve(args.data, args.host, args.port, args.password, args.verbose)


def cmd_start(args) -> int:
    data = os.path.abspath(args.data)
    os.makedirs(data, exist_ok=True)
    info = running_server(data)
    if info:
        print(f"Server pehle se chal raha hai: {info['host']}:{info['port']} (pid {info['pid']})")
        return 0
    check_host = DEFAULT_HOST if args.host in ("0.0.0.0", "::", "") else args.host
    if port_open(check_host, args.port):
        note(f"Port {args.port} par pehle se kuch aur chal raha hai. Doosra port do:  meradb start --port 6373")
        return 1

    log_path = os.path.join(data, "server.log")
    env = dict(os.environ)
    if args.password:
        env["MERADB_PASSWORD"] = args.password  # via env, so it doesn't show in the process list
    package_parent = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    env["PYTHONPATH"] = package_parent + os.pathsep + env.get("PYTHONPATH", "")
    cmd = [sys.executable, "-m", "meradb", "server", "--data", data, "--host", args.host, "--port", str(args.port)]
    if args.verbose:
        cmd.append("--verbose")

    # Detach the server from this terminal so it keeps running after we exit.
    kwargs = {}
    if os.name == "nt":
        kwargs["creationflags"] = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        kwargs["start_new_session"] = True
    with open(log_path, "a", encoding="utf-8") as log:
        proc = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                env=env, close_fds=True, **kwargs)

    deadline = time.time() + 15
    while time.time() < deadline:
        if proc.poll() is not None:
            note(f"Server start nahi hua. Log ({log_path}):")
            note(_tail(log_path))
            return 1
        # We checked above that no server was serving this folder, so any server
        # that appears now is ours. Don't compare proc.pid with the pid file:
        # inside a Windows virtual environment, .venv\Scripts\python.exe is a small
        # launcher that starts the real Python as a CHILD process, so the pids differ.
        info = running_server(data)
        if info:
            print(f"MeraDB server chal gaya: {args.host}:{info['port']}  (pid {info['pid']})")
            print(f"  data: {data}")
            print(f"  log:  {log_path}")
            print("  connect: meradb shell   |   band: meradb stop")
            return 0
        time.sleep(0.2)
    note(f"Server 15 second mein ready nahi hua. Log dekho: {log_path}")
    return 1


def cmd_stop(args) -> int:
    from .client import Connection

    data = os.path.abspath(args.data)
    info = running_server(data)
    if not info:
        stale = read_pid_file(data)
        if stale:
            remove_pid_file(data, stale.get("pid"))  # left behind by a crash
        print("Server nahi chal raha.")
        return 0

    host = info.get("host", DEFAULT_HOST)
    if host in ("0.0.0.0", "::", ""):
        host = DEFAULT_HOST
    port, pid = int(info["port"]), info.get("pid")
    try:
        with Connection(host, port, get_password(args)) as conn:
            conn.shutdown()
    except MeraDBError as e:
        if not args.force:
            note(f"{e}\nZabardasti band karne ke liye:  meradb stop --force")
            return 1
        note(f"{e} -- process {pid} ko kill kar rahe hain (--force)")
        try:
            os.kill(pid, signal.SIGTERM)  # on Windows this is TerminateProcess
        except OSError as kill_error:
            note(f"Kill fail: {kill_error}")
            return 1
        remove_pid_file(data, pid)

    deadline = time.time() + 10
    while time.time() < deadline:
        if not port_open(host, port):
            print(f"MeraDB server band ho gaya (pid {pid}).")
            return 0
        time.sleep(0.2)
    note("Server abhi bhi chal raha hai -- `meradb stop --force` try karo")
    return 1


def cmd_status(args) -> int:
    from .client import Connection

    data = os.path.abspath(args.data)
    info = running_server(data)
    if not info:
        print(f"MeraDB server nahi chal raha  (data: {data})")
        print("Start karne ke liye: meradb start")
        return 3  # conventional "not running" exit code for database status tools

    host = info.get("host", DEFAULT_HOST)
    connect_host = DEFAULT_HOST if host in ("0.0.0.0", "::", "") else host
    print(f"MeraDB server chal raha hai")
    print(f"  address:   {host}:{info['port']}")
    print(f"  pid:       {info.get('pid')}")
    print(f"  data:      {data}")
    print(f"  started:   {info.get('started', '?')}")
    try:
        with Connection(connect_host, int(info["port"]), get_password(args)) as conn:
            status = conn.status()
        print(f"  version:   {status.get('server')}")
        print(f"  sessions:  {status.get('sessions')} connected")
        print(f"  databases: {', '.join(status.get('databases', []))}")
    except MeraDBError as e:
        print(f"  (details nahi mile: {e})")
    return 0


def _tail(path: str, lines: int = 15) -> str:
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return "".join(f.readlines()[-lines:])
    except OSError:
        return "(log nahi mila)"
