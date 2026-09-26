"""
MeraDB WORKBENCH: a full-screen terminal client (like MySQL Workbench),
built with Textual (https://textual.textualize.io).

    meradb workbench                 # connects to the server (or local mode)

 +-- Header: MeraDB Workbench -- 127.0.0.1:6372 | db: main -------------+
 | Schema tree        | Results (DataTable)                             |
 |  > main            |                                                 |
 |    > students      |                                                 |
 |        id int PK   +-------------------------------------------------+
 |  > college         | Log: queries, messages, errors, timings         |
 |                    +-------------------------------------------------+
 |                    | Query editor (syntax highlighted)               |
 +-- Footer: F5 Chalao  F6 Samjhao  Ctrl+S CSV  Ctrl+O Connect  ... ----+

This file is ONLY presentation. It talks to the database through a "backend"
-- a client Connection to a server, or an embedded Engine -- using the same small
API as the shell. The engine knows nothing about the workbench.
"""

import csv
import os
import time
from datetime import datetime
from pathlib import Path
from typing import Optional

from rich.text import Text
from textual.app import App, ComposeResult
from textual.binding import Binding
from textual.containers import Horizontal, Vertical, VerticalScroll
from textual.screen import ModalScreen
from textual.widgets import Button, DataTable, Footer, Header, Input, Label, Markdown, RichLog, TextArea, Tree

from .datatypes import format_value
from .engine import Engine, Result
from .errors import MeraDBError
from .highlight import RICH_STYLES, spans
from .parser import parse
from .protocol import DEFAULT_HOST, DEFAULT_PORT, default_data_dir

LANGUAGE_DOC = Path(__file__).resolve().parent.parent / "docs" / "LANGUAGE.md"

KEYS_HELP = """\
# MeraDB Workbench: Madad

| Key | Kaam |
|-----|------|
| **F5** / **Ctrl+R** | Query chalao (selected text only, if something is selected) |
| **F6** | SAMJHAO: query plan dikhao (index / scan / join), bina chalaye |
| **Ctrl+Up / Ctrl+Down** | Pichli / agli query (history) |
| **Ctrl+S** | Results ko CSV file mein save karo (`exports/` folder) |
| **Ctrl+O** | Doosre server se connect karo, ya local mode |
| **Tab / Shift+Tab** | Editor, results aur schema tree ke beech jao |
| **Enter** on a table in the tree | Uske pehle 100 rows dikhao |
| **Enter** on a database | Us database ko ISTEMAL karo |
| **Enter** on a column | Column ka naam editor mein daalo |
| **Ctrl+L** | Log saaf karo |
| **F1** / **Esc** | Ye madad kholo / band karo |
| **Ctrl+Q** | Bahar niklo |

"""


# ============================================================================
# Widgets
# ============================================================================


class QueryEditor(TextArea):
    """
    A TextArea that colours MeraDB syntax.

    Textual's built-in highlighting needs a tree-sitter grammar, which our
    Hinglish language doesn't have. Instead we override the method that fills
    the highlight map and feed it spans from our own highlighter.
    NOTE: `_build_highlight_map` is a private Textual method -- that's why
    pyproject.toml pins Textual to 8.x.
    """

    def _build_highlight_map(self) -> None:
        self._line_cache.clear()
        highlights = self._highlights
        highlights.clear()
        for row, line in enumerate(self.document.lines):
            for start, end, kind in spans(line):
                # Textual expects BYTE offsets (UTF-8), not character offsets
                highlights[row].append((_byte_len(line[:start]), _byte_len(line[:end]), kind))


def _byte_len(text: str) -> int:
    return len(text.encode("utf-8"))


def highlighted(text: str) -> Text:
    """The same highlighting, as a Rich Text for the log panel."""
    out = Text()
    for i, line in enumerate(text.strip().splitlines()):
        if i:
            out.append("\n")
        rich_line = Text(line)
        for start, end, kind in spans(line):
            style = RICH_STYLES.get(kind)
            if style:
                rich_line.stylize(style, start, end)
        out.append_text(rich_line)
    return out


def cell(value) -> Text:
    """How one value looks inside the results table."""
    if value is None:
        return Text("KHALI", style="dim italic")
    if isinstance(value, bool):
        return Text(format_value(value), style="green" if value else "red")
    if isinstance(value, (int, float)):
        return Text(format_value(value), style="#bd93f9", justify="right")
    return Text(format_value(value))


class HelpScreen(ModalScreen):
    BINDINGS = [Binding("escape,f1,q", "dismiss", "Band karo")]

    DEFAULT_CSS = """
    HelpScreen { align: center middle; }
    #help-box {
        width: 90%; height: 90%;
        border: thick $primary; background: $surface; padding: 0 2;
    }
    """

    def compose(self) -> ComposeResult:
        try:
            reference = LANGUAGE_DOC.read_text(encoding="utf-8")
        except OSError:
            reference = "*(docs/LANGUAGE.md nahi mila)*"
        with VerticalScroll(id="help-box"):
            yield Markdown(KEYS_HELP + reference)


class ConnectScreen(ModalScreen[Optional[dict]]):
    """The "new connection" dialog. Returns a dict of choices, or None if cancelled."""

    BINDINGS = [Binding("escape", "cancel", "Band karo")]

    DEFAULT_CSS = """
    ConnectScreen { align: center middle; }
    #connect-box {
        width: 64; height: auto;
        border: thick $primary; background: $surface; padding: 1 2;
    }
    #connect-box Input { margin-bottom: 1; }
    #connect-buttons { height: auto; margin-top: 1; }
    #connect-buttons Button { margin-right: 1; }
    """

    def __init__(self, host: str, port: int):
        super().__init__()
        self.host, self.port = host, port

    def compose(self) -> ComposeResult:
        with Vertical(id="connect-box"):
            yield Label(Text("MeraDB server se connect karo", style="bold"))
            yield Label("Host")
            yield Input(value=self.host, id="host")
            yield Label("Port")
            yield Input(value=str(self.port), id="port", type="integer")
            yield Label("Password (agar server par hai)")
            yield Input(password=True, id="password")
            yield Label("Database (optional)")
            yield Input(id="database")
            with Horizontal(id="connect-buttons"):
                yield Button("Connect", variant="primary", id="server")
                yield Button("Local mode", id="local")
                yield Button("Cancel", id="cancel")

    def _choice(self, mode: str) -> dict:
        def value(name):
            return self.query_one(f"#{name}", Input).value.strip()

        return {"mode": mode, "host": value("host") or DEFAULT_HOST, "port": value("port") or str(DEFAULT_PORT),
                "password": value("password") or None, "database": value("database") or None}

    def on_button_pressed(self, event: Button.Pressed) -> None:
        self.dismiss(None if event.button.id == "cancel" else self._choice(event.button.id))

    def on_input_submitted(self, event: Input.Submitted) -> None:
        self.dismiss(self._choice("server"))

    def action_cancel(self) -> None:
        self.dismiss(None)


# ============================================================================
# The app
# ============================================================================


class MeraDBApp(App):
    TITLE = "MeraDB Workbench"

    CSS = """
    #body { height: 1fr; }
    #schema { width: 32; border: round $primary; }
    #main { width: 1fr; }
    #results { height: 1fr; border: round $accent; }
    #log { height: 10; border: round $secondary; }
    #editor { height: 9; border: round $success; }
    #schema:focus-within, #results:focus, #editor:focus { border: heavy $warning; }
    """

    # priority=True: these keys work even while the editor has focus
    BINDINGS = [
        Binding("f5", "run_query", "Chalao", priority=True),
        Binding("ctrl+r", "run_query", "Chalao", show=False, priority=True),
        Binding("f6", "explain", "Samjhao", priority=True),
        Binding("ctrl+up", "history(-1)", "Pichli", priority=True),
        Binding("ctrl+down", "history(1)", "Agli", priority=True),
        Binding("ctrl+s", "export_csv", "CSV", priority=True),
        Binding("ctrl+o", "connect", "Connect", priority=True),
        Binding("ctrl+l", "clear_log", "Log saaf"),
        Binding("f1", "help", "Madad"),
        Binding("ctrl+q", "quit", "Bahar", priority=True),
    ]

    def __init__(self, backend, data_dir: Optional[str] = None):
        super().__init__()
        self.backend = backend  # a client Connection or an embedded Engine
        self.data_dir = data_dir or default_data_dir()
        self.history: list[str] = []
        self.history_pos = 0
        self.last_result: Optional[Result] = None

    # ---- layout ----
    def compose(self) -> ComposeResult:
        yield Header()
        with Horizontal(id="body"):
            yield Tree("Databases", id="schema")
            with Vertical(id="main"):
                yield DataTable(id="results", zebra_stripes=True, cursor_type="row")
                yield RichLog(id="log", wrap=True, markup=False, highlight=False)
                yield QueryEditor(
                    id="editor",
                    theme="dracula",
                    show_line_numbers=True,
                    placeholder="Yahan query likho, jaise:  DIKHAO * SE students;   (F5 se chalao)",
                )
        yield Footer()

    def on_mount(self) -> None:
        self.theme = "dracula"
        self.query_one("#schema").border_title = "Schema"
        self.query_one("#results").border_title = "Results"
        self.query_one("#log").border_title = "Log"
        self.query_one("#editor").border_title = "Query  [F5 = chalao, F6 = samjhao]"
        self.log_line(Text(f"Namaste! Connected: {self.backend.description}", style="bold"))
        self.log_line(Text("F1 dabao madad ke liye.", style="dim"))
        self.refresh_schema()
        self.update_subtitle()
        self.query_one(QueryEditor).focus()

    # ---- helpers ----
    def log_line(self, text: Text) -> None:
        self.query_one("#log", RichLog).write(text)

    def update_subtitle(self) -> None:
        subtitle = f"{self.backend.description}  |  db: {self.backend.current_db}"
        if self.backend.in_transaction:
            subtitle += "  |  TRANSACTION (PAKKA / WAPAS)"
        self.sub_title = subtitle

    def show_result(self, result: Result) -> None:
        self.last_result = result
        table = self.query_one("#results", DataTable)
        table.clear(columns=True)
        table.add_columns(*result.columns)
        table.add_rows([[cell(v) for v in row] for row in result.rows])
        table.border_title = f"Results -- {len(result.rows)} row(s)"

    def refresh_schema(self) -> None:
        """Rebuild the sidebar tree, keeping open whatever the user had opened."""
        try:
            databases = self.backend.schema_tree()
        except MeraDBError as e:
            self.log_line(Text(f"(schema refresh nahi hua: {e})", style="dim"))
            return
        tree = self.query_one("#schema", Tree)
        open_nodes = {node.data for node in _walk(tree.root) if node.is_expanded and node.data}

        tree.clear()
        tree.root.expand()
        for db in databases:
            name = db["name"]
            label = Text(name, style="bold #50fa7b") if db["current"] else Text(name)
            db_node = tree.root.add(label, data=("db", name), expand=db["current"] or ("db", name) in open_nodes)
            for table in db["tables"]:
                key = ("table", name, table["name"])
                table_node = db_node.add(Text(table["name"], style="bold"), data=key, expand=key in open_nodes)
                for col in table["columns"]:
                    label = Text.assemble(col["name"], " ", (col["type_name"].lower(), "#8be9fd"))
                    if col["primary_key"]:
                        label.append(" PK", style="#ffb86c")
                    elif col["unique"]:
                        label.append(" UQ", style="#ffb86c")
                    if col["not_null"]:
                        label.append(" NN", style="dim")
                    table_node.add_leaf(label, data=("column", name, table["name"], col["name"]))

    # ---- running queries ----
    def run_text(self, text: str) -> None:
        text = text.strip()
        if not text:
            return
        if not self.history or self.history[-1] != text:
            self.history.append(text)
        self.history_pos = len(self.history)

        self.log_line(Text(f"{self.backend.current_db}> ", style="dim") + highlighted(text))
        started = time.perf_counter()
        try:
            results = self.backend.run_script(text)
        except MeraDBError as e:  # e.g. the server went away
            self.log_line(Text(str(e), style="bold red"))
            self.log_line(Text("Ctrl+O se dobara connect karo.", style="dim"))
            return

        last_table = None
        for result in results:
            if result.error:
                self.log_line(Text(result.error, style="bold red"))
                continue
            if result.columns:
                last_table = result
            if result.message:
                self.log_line(Text(result.message, style="#50fa7b"))

        elapsed_ms = (time.perf_counter() - started) * 1000
        self.log_line(Text(f"({elapsed_ms:.1f} ms)", style="dim"))
        if last_table is not None:
            self.show_result(last_table)
        self.refresh_schema()
        self.update_subtitle()

    def _editor_text(self) -> str:
        editor = self.query_one(QueryEditor)
        return editor.selected_text or editor.text

    # ---- actions (bound to keys above) ----
    def action_run_query(self) -> None:
        self.run_text(self._editor_text())

    def action_explain(self) -> None:
        text = self._editor_text().strip()
        try:
            statements = parse(text)
        except MeraDBError as e:
            self.log_line(Text(str(e), style="bold red"))
            return
        if len(statements) != 1:
            self.log_line(Text("SAMJHAO ek hi query par chalta hai -- ek query select karke F6 dabao", style="yellow"))
            return
        self.run_text("SAMJHAO " + text)

    def action_history(self, step: int) -> None:
        if not self.history:
            return
        self.history_pos = max(0, min(len(self.history), self.history_pos + step))
        text = self.history[self.history_pos] if self.history_pos < len(self.history) else ""
        editor = self.query_one(QueryEditor)
        editor.text = text
        editor.move_cursor(editor.document.end)
        editor.focus()

    def action_export_csv(self) -> None:
        result = self.last_result
        if result is None:
            self.log_line(Text("Pehle koi DIKHAO query chalao, phir Ctrl+S", style="yellow"))
            return
        os.makedirs("exports", exist_ok=True)
        path = os.path.abspath(os.path.join("exports", f"meradb-{datetime.now():%Y%m%d-%H%M%S}.csv"))
        with open(path, "w", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            writer.writerow(result.columns)
            for row in result.rows:
                writer.writerow(["" if v is None else format_value(v) for v in row])
        self.log_line(Text(f"{len(result.rows)} row(s) CSV mein save: {path}", style="#50fa7b"))

    def action_connect(self) -> None:
        host = getattr(self.backend, "host", DEFAULT_HOST)
        port = getattr(self.backend, "port", DEFAULT_PORT)
        self.push_screen(ConnectScreen(host, port), self._switch_backend)

    def _switch_backend(self, choice: Optional[dict]) -> None:
        if not choice:
            return
        from .client import Connection

        try:
            if choice["mode"] == "local":
                new_backend = Engine(self.data_dir)
            else:
                new_backend = Connection(choice["host"], int(choice["port"]), choice["password"], choice["database"])
        except (MeraDBError, ValueError) as e:
            self.log_line(Text(f"Connect nahi hua: {e}", style="bold red"))
            return
        old, self.backend = self.backend, new_backend
        old.close()
        self.log_line(Text(f"Connected: {new_backend.description}", style="bold #50fa7b"))
        self.refresh_schema()
        self.update_subtitle()

    def action_clear_log(self) -> None:
        self.query_one("#log", RichLog).clear()

    def action_help(self) -> None:
        self.push_screen(HelpScreen())

    # ---- schema tree clicks ----
    def on_tree_node_selected(self, event: Tree.NodeSelected) -> None:
        data = event.node.data
        if not data:
            return
        kind, db = data[0], data[1]
        if kind == "column":
            editor = self.query_one(QueryEditor)
            editor.insert(data[3])
            editor.focus()
            return
        if db != self.backend.current_db:
            self.run_text(f"ISTEMAL {db};")
        if kind == "table":
            self.run_text(f"DIKHAO * SE {data[2]} SIRF 100;")


def _walk(node):
    yield node
    for child in node.children:
        yield from _walk(child)


def run_workbench(backend, args=None) -> int:
    app = MeraDBApp(backend, data_dir=getattr(args, "data", None))
    try:
        app.run()
    finally:
        app.backend.close()  # rolls back an unfinished transaction, closes the connection
    return 0
