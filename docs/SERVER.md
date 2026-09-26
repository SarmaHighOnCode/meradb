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

Opens the server to your LAN. Always use a password when you do this.

```bash
meradb shell -H 192.168.1.20 -W
```

Connects from another machine on the network.

The password travels as plain text (there is no encryption).
That's fine on your own machine or LAN, but not across the internet.

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
-> {"type": "hello", "version": 1, "password": null, "database": "main"}
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
