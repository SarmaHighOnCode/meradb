#!/usr/bin/env python3
"""Runs one scenario of workbench_scenarios.json against meradb/tui.py with Textual's Pilot; prints observables as JSON."""
import argparse
import asyncio
import glob
import json
import os
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

LOG_CLASSES = {"bold red": "error", "#50fa7b": "message", "bold #50fa7b": "connected", "dim": "dim",
               "yellow": "warn", "bold": "bold"}
CELL_CLASSES = {"dim italic": "null", "green": "true", "red": "false", "#bd93f9": "number", "": "plain"}


def log_class(text):
    kind = LOG_CLASSES.get(str(text.style), "plain")
    if kind == "dim" and re.match(r"^\S+> ", text.plain):  # Text("<db>> ", dim) + highlighted(...) keeps the dim base style
        kind = "echo"
    return kind


def label_of(node):
    return node.label.plain if hasattr(node.label, "plain") else str(node.label)


def walk(node, depth=0):
    yield {"depth": depth, "label": label_of(node), "expanded": bool(node.is_expanded)}
    for child in node.children:
        yield from walk(child, depth + 1)


def find_node(tree, path):
    node = tree.root
    assert label_of(node) == path[0], path
    for label in path[1:]:
        node = next(c for c in node.children if label_of(c) == label)
    return node


def column_label(column):
    label = column.label
    return label.plain if hasattr(label, "plain") else str(label)


def observe(app, logged):
    from meradb.tui import QueryEditor
    from textual.widgets import DataTable, Tree

    table = app.query_one("#results", DataTable)
    out = {
        "subtitle": app.sub_title,
        "editor": app.query_one(QueryEditor).text,
        "history": list(app.history),
        "results_title": table.border_title,
        "columns": [column_label(c) for c in table.columns.values()],
        "rows": [[{"t": c.plain, "k": CELL_CLASSES.get(str(c.style), "plain")} for c in table.get_row_at(i)]
                 for i in range(table.row_count)],
        "log": [{"k": log_class(t), "t": t.plain} for t in logged],
        "tree": list(walk(app.query_one("#schema", Tree).root)),
    }
    files = glob.glob(os.path.join("exports", "*.csv"))
    if files:
        with open(max(files, key=os.path.getmtime), newline="", encoding="utf-8") as f:
            out["csv"] = f.read()
    return out


async def run(scenario, data_dir):
    from meradb.engine import Engine
    from meradb.tui import MeraDBApp, QueryEditor
    from textual.widgets import Tree
    from textual.widgets.text_area import Selection

    logged = []
    app = MeraDBApp(Engine(data_dir), data_dir=data_dir)
    original = app.log_line
    app.log_line = lambda text: (logged.append(text), original(text))[1]
    clear = app.action_clear_log
    app.action_clear_log = lambda: (logged.clear(), clear())[1]
    snaps = {}
    async with app.run_test(size=(220, 60)) as pilot:
        await pilot.pause()
        for step in scenario["steps"]:
            editor = app.query_one(QueryEditor)
            if "set_text" in step:
                editor.text = step["set_text"]
            elif "select" in step:
                r1, c1, r2, c2 = step["select"]
                editor.selection = Selection(start=(r1, c1), end=(r2, c2))
            elif "run" in step:
                app.action_run_query()
            elif "explain" in step:
                app.action_explain()
            elif "history" in step:
                app.action_history(step["history"])
            elif "export" in step:
                app.action_export_csv()
            elif "tree_enter" in step:
                tree = app.query_one("#schema", Tree)
                tree.select_node(find_node(tree, step["tree_enter"]))
            elif "clear_log" in step:
                app.action_clear_log()
            elif "connect" in step:
                app._switch_backend({"mode": "local", "host": "127.0.0.1", "port": "6372", "password": None, "database": None})
            elif "snapshot" in step:
                snaps[step["snapshot"]] = observe(app, logged)
            await pilot.pause()
    app.backend.close()
    return snaps


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("scenarios")
    parser.add_argument("name")
    parser.add_argument("--data", required=True)
    parser.add_argument("--cwd", required=True)
    args = parser.parse_args()
    scenario = next(s for s in json.load(open(args.scenarios, encoding="utf-8")) if s["name"] == args.name)
    os.chdir(args.cwd)
    sys.stdout.reconfigure(encoding="utf-8")
    print(json.dumps(asyncio.run(run(scenario, args.data)), ensure_ascii=False))


if __name__ == "__main__":
    main()
