# Running the MeraDB server

MeraDB is a client–server database, the same setup as MySQL with MySQL Workbench:
a **server** process owns the data files, and **clients** connect to it over TCP.

| Command | What it does |
|---------|--------------|
| `meradb start` | Start the server in the background |
| `meradb stop` | Stop it cleanly |
| `meradb status` | Is it running? Who is connected? |
| `meradb server` | Run the server in this terminal (Ctrl+C stops it) |
| `meradb workbench` | Full-screen client with schema browser (like MySQL Workbench) |
| `meradb shell` | Interactive command-line client |
| `meradb run file.mdb` | Run a script |

| Setting | Default | Change it with |
|---------|---------|----------------|
| Port | **6372** ("MERA" on a phone keypad) | `--port` / `MERADB_PORT` |
| Data folder | per-user folder (below) | `-D` / `MERADB_DATA` |
| Password | none | `--password` on the server; `-W` / `MERADB_PASSWORD` on clients |
| Server address for clients | `127.0.0.1` | `-H` / `MERADB_HOST` |
| Username (per-user login) | none | `-U` / `MERADB_USER` on clients; required once users exist, see below |

## Install once

```bash
pip install -e .[workbench]
```

Run this from the project folder. It puts a `meradb` command on your PATH. `-e` means
"editable": when you change the code, the command uses the new code immediately.
Without the install, every command also works as `python -m meradb ...` from the project folder.

## Everyday use

```bash
meradb start
```

```bash
meradb shell
```

```bash
meradb workbench
```

```bash
meradb status
```

```bash
meradb stop
```

Output of `meradb start`:

```
MeraDB server chal gaya: 127.0.0.1:6372  (pid 11052)
  data: C:\Users\<you>\AppData\Local\MeraDB\data
  log:  C:\Users\<you>\AppData\Local\MeraDB\data\server.log
  connect: meradb shell   |   band: meradb stop
```

## Where is my data?

One fixed folder per user, the way database servers keep a data directory:

- Windows: `%LOCALAPPDATA%\MeraDB\data`
- Linux/macOS: `~/.local/share/MeraDB/data`

Change it with `-D <folder>` on any command, or set `MERADB_DATA`. Inside the folder:

```
data/
  meradb.pid       which process/port serves this folder (exists only while running)
  server.log       what the background server printed
  main/            the default database
    catalog.json   table schemas
    students.tbl   rows of `students` (binary)
  college/         another database (BANAO DATABASE college)
  .wapas/          transaction snapshots (only while a transaction is open)
```

## Client options

All of `shell`, `workbench` and `run` accept:

| Option | Meaning |
|--------|---------|
| `-H HOST` / `-p PORT` | Which server (default `127.0.0.1:6372`) |
| `-d DATABASE` | Start in this database instead of `main` |
| `-W` | Ask for a password (or set `MERADB_PASSWORD`) |
| `-U USERNAME` | Log in as this user (or set `MERADB_USER`) -- see "Users and privileges" below |
| `--local` | Don't use a server: open the data folder directly (embedded, like SQLite) |
| `-D FOLDER` | The data folder, for `--local` |

**Automatic fallback:** if you don't pass `-H`/`-p` and no server is running, the
client prints a note and runs in **local mode** instead. So `meradb shell` always
works, with or without a server. If a server *is* running on a data folder, local mode
refuses to open that folder, because two processes writing the same files would corrupt them.

## Password and network access

By default the server only listens on `127.0.0.1`, so only your own computer can connect.

```bash
meradb start --password s3cret
```

Starts the server with a password.

```bash
meradb shell -W
```

Asks for the password when connecting.

```bash
meradb start --host 0.0.0.0 --password s3cret
```

Opens the server to your LAN. A password is required for this: without one the server refuses
to start, because until the first user exists a connection with no password is a superuser. (`MERADB_ALLOW_NO_PASSWORD=1`
overrides that if you really mean it.) Prefer `MERADB_PASSWORD` to `--password` on shared
machines: command-line arguments are visible in the process list.

```bash
meradb shell -H 192.168.1.20 -W
```

Connects from another machine on the network.

Passwords, queries and results all travel as plain text (there is no encryption), so anyone
who can watch the network can read them. Use it on your own machine or a LAN you trust; to
cross anything else, put it inside an SSH tunnel or a VPN.

Limits the server applies:

| Limit | Value |
|---|---|
| Time to send the first message (the hello) | 10 seconds in all, slow sending does not extend it |
| Size of the hello | 64 KB |
| Size of any later message | 64 MB |
| Connections at once | 256; further ones are told so and closed |
| Connections at once from one address | 64 (`MERADB_MAX_CONNECTIONS_PER_ADDRESS`) |
| A session that does nothing | closed after 8 hours (`MERADB_IDLE_TIMEOUT`, in seconds) |
| A session that does nothing inside a transaction | rolled back and closed after 5 minutes (`MERADB_IDLE_TRANSACTION_TIMEOUT`, in seconds) |
| A client that does not read its reply | dropped after 2 minutes without progress |
| Logins hashing a password at once | 4; the others wait up to 5 seconds, then hear the server is busy |
| Wrong passwords in a row from one address | 10, then that address is refused for 30 seconds |

What one query request (one message, however many statements it holds) may use:

| Limit | Value |
|---|---|
| Time | 120 seconds (`MERADB_MAX_QUERY_SECONDS`) |
| Rows held in memory, estimated | 2048 MB (`MERADB_MAX_QUERY_MEMORY_MB`) |
| Rows in one result | 5,000,000 (`MERADB_MAX_RESULT_ROWS`) |
| One text value (a `CONCAT` that keeps doubling) | 64 MB (`MERADB_MAX_VALUE_MB`); all of a session's `@variables` together four times that |
| The reply | 64 MB, which is the most the clients can read |

Past a limit the statement gets an ordinary error and the session carries on, with its transaction
still open if it had one. The error says which limit it was. Every session counts here, the superuser
too. `--local` and an embedded `Engine` have none of these limits: there the only one who suffers is
the one who asked.

The time limit is checked while expressions are evaluated and the memory figure is an estimate, so a
query can run a little past them. Because one lock lets only one statement run at a time, one query's
limit is close to the limit for the whole server; the replies waiting to be sent to slow clients come
on top of that.

The settings in brackets are read from the environment when the server starts; `0` turns a limit
off. They apply to the C++ server (`meradb_cli`), not to the older Python one.

The per-address limits count everyone behind one address together, which matters behind a proxy or
an SSH tunnel: there, raise the connection limit or set it to `0`.

Only a query or a schema request counts as doing something: a `ping` does not keep a session open.
A session with an open transaction gets the short limit because it holds the one lock everyone else
is waiting for. It is closed as well as rolled back, so that a client which thinks it is inside a
transaction cannot carry on outside one without noticing. Whatever it sends next gets the reason as
its answer, then the closed connection.

Protect the data folder with ordinary file permissions. `users.json` holds password hashes, so
only the user who runs MeraDB can read it: mode `600` on Linux and macOS, and on Windows an access
list with that one user and nothing inherited from the folder. A `users.json` from an older version
gets the same rights the next time the C++ engine opens it. (So a server that runs as another Windows
account than the one you use for `--local` needs that account's rights: an administrator can take
ownership. The older Python engine does not set the Windows list, and a save by it puts the file back
on the folder's rights until the C++ engine next opens it.)

Errors sent to clients say `<data folder>` where the path would be, and only a client on the server's
own computer gets the folder from `meradb status`; the server's log keeps the full text. One event
is always one line in the log: a line break or control character a client sent shows as `\n` or `\x1b`.

## Users and privileges

There are two completely separate ways to authenticate:

1. **The single shared server password** (`--password` / `-W` above) -- anyone who knows
   it connects as an unrestricted **superuser**. This is what every example above uses.
2. **A per-user login** (`-U`/`--user`, or `MERADB_USER`) -- the server checks the given
   username + password against `users.json` (created with `BANAO USER ...` -- see
   `docs/LANGUAGE.md` "Users & privileges"). A per-user login **supersedes** the shared
   password check entirely for that connection.

```bash
meradb shell -U ravi -W        # log in as ravi (asks for ravi's password)
```

**The rule that matters: a server connection without a username is a superuser only if
nothing else protects the server.** What it gets depends on the server:

| The server has | A connection without a username |
|---|---|
| no users, no shared password | superuser (this is how the first user gets made) |
| a shared password (`--password`) | superuser, if it gives that password |
| users, and no shared password | **refused**, with a message saying how to log in |

So as soon as `BANAO USER` has run on a server without a shared password, everyone has to log in:

```bash
meradb shell -U ravi -W          # the shell: asks for ravi's password
meradb run -U ravi -W script.mdb # a script
meradb workbench -U ravi -W      # the terminal workbench
meradb gui -U ravi -W            # the desktop window (its Connect dialog has a Username box too)
```

Set `MERADB_USER` and `MERADB_PASSWORD` instead of the options to save typing. A role never
counts as a user here (a role cannot log in), and a session that was already open when the
first user was made stays open.

**Getting superuser back.** A user made with `BANAO USER` is restricted, so with users and no
shared password nobody on the server is a superuser. There are two ways back, and neither can be
locked out, because both need access to the computer and not to any account:

```bash
meradb stop --force              # `meradb stop` needs a superuser too, so it says the same
meradb shell --local             # full access, straight to the data folder
```

or restart the server with `meradb start --password s3cret`, then `meradb shell -W`.

A username is meaningless with `--local` (there's no server, so nothing to authenticate
against) -- passing `-U` there is noted and ignored, not an error, since `--local` has
always meant "trust the person running this process completely". The same holds for an
embedded `Engine`.

Once a session HAS authenticated as a specific user, it can only run `DIKHAO`/`DAALO`/
`BADLO`/`MITAO` against tables/views it has been granted the matching privilege on via
`ADHIKAR DO` -- every DDL statement, user/grant management, and every transaction command
becomes superuser-only from that point. See `docs/LANGUAGE.md` for the full grant syntax
and its honest-scope limitations.

## Troubleshooting

| Problem | Fix |
|---------|-----|
| `Port 6372 par pehle se kuch aur chal raha hai` | Another program uses the port: `meradb start --port 6373`, then `meradb shell -p 6373` |
| `meradb stop` says the password is wrong | `meradb stop -W`, or `meradb stop --force` to kill it |
| The server doesn't start | Read `server.log` in the data folder (`meradb start` prints its path and last lines) |
| `Is data folder par MeraDB server chal raha hai` | You used `--local` while a server runs. Drop `--local`, or `meradb stop` first |
| You changed the code but nothing changed | The server runs the code it started with: `meradb stop` then `meradb start` |
| `meradb` is not recognised | Python's `Scripts` folder isn't on PATH. Use `python -m meradb ...` instead |

## Start automatically when Windows starts (optional)

MySQL installs itself as a Windows service. For MeraDB, a Task Scheduler entry
is enough. Run this once in a terminal:

```bash
schtasks /Create /TN MeraDB /SC ONLOGON /TR "meradb start"
```

To remove it again:

```bash
schtasks /Delete /TN MeraDB /F
```

## The wire protocol

Clients and server exchange **one JSON object per line** over TCP. It's readable enough
to debug by hand:

```
-> {"type": "hello", "version": 1, "password": null, "database": "main", "user": null}
<- {"ok": true, "server": "MeraDB 1.0.0", "protocol": 1, "database": "main"}
-> {"type": "query", "text": "DIKHAO * SE t;"}
<- {"ok": true, "database": "main", "in_transaction": false,
    "results": [{"columns": ["x"], "rows": [[42]], "message": "1 row(s)", "error": ""}]}
```

Other requests: `schema`, `status`, `ping`, `shutdown` (only accepted from the same machine).
The full description is in `meradb/protocol.py`. From Python:

```python
from meradb.client import Connection

with Connection("127.0.0.1", 6372) as db:
    print(db.execute("DIKHAO * SE students;")[0].rows)
```

## How several clients share one server

- Each client connection gets its **own thread** and its **own session**, with its own
  current database and transaction.
- All sessions share **one lock**. A statement runs only while holding it, so two
  statements never interleave.
- `SHURU` (BEGIN) keeps the lock until `PAKKA` / `WAPAS`. Other clients wait (up to
  10 s, then they get a "Database busy" error), so nobody ever sees half-finished changes.
- If a client disconnects mid-transaction, the server rolls it back.

This "one statement at a time" design is the simplest correct approach. Real databases
use row locks or MVCC so that many clients can work at the same moment.
