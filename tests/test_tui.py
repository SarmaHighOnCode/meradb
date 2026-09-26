"""Drives the terminal UI headlessly (no real terminal needed). Skipped if Textual isn't installed."""

import asyncio
import shutil
import tempfile
import unittest

try:
    from textual.widgets import DataTable, RichLog, Tree

    from meradb.engine import Engine
    from meradb.tui import MeraDBApp, QueryEditor
except ImportError:  # pragma: no cover
    MeraDBApp = None


@unittest.skipIf(MeraDBApp is None, "textual not installed")
class TuiTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        # unittest runs the event loop in debug mode, which floods the output
        # with "slow callback" warnings and makes Textual much slower
        asyncio.get_running_loop().set_debug(False)

    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.dir)

    async def run_query(self, app, pilot, text):
        app.query_one(QueryEditor).text = text
        await pilot.press("f5")
        await pilot.pause()

    async def test_run_query_shows_results_and_schema(self):
        app = MeraDBApp(Engine(self.dir))
        async with app.run_test(size=(120, 40)) as pilot:
            await self.run_query(
                app, pilot,
                "BANAO TABLE s (id INT MUKHYA KUNJI, naam TEXT); DAALO MEIN s MAAN (1, 'Ravi'), (2, KHALI); DIKHAO * SE s;",
            )
            table = app.query_one("#results", DataTable)
            self.assertEqual(table.row_count, 2)
            self.assertEqual(len(table.columns), 2)

            tree = app.query_one("#schema", Tree)
            main_db = tree.root.children[0]
            self.assertEqual(main_db.data, ("db", "main"))
            self.assertEqual(main_db.children[0].data, ("table", "main", "s"))

    async def test_errors_go_to_log_and_app_survives(self):
        app = MeraDBApp(Engine(self.dir))
        async with app.run_test() as pilot:
            await self.run_query(app, pilot, "DIKHAO * SE nahi_hai;")
            log_text = "\n".join(line.text for line in app.query_one("#log", RichLog).lines)
            self.assertIn("exist nahi karta", log_text)

    async def test_history(self):
        app = MeraDBApp(Engine(self.dir))
        async with app.run_test() as pilot:
            await self.run_query(app, pilot, "DIKHAO TABLES;")
            await self.run_query(app, pilot, "BATAO nahi_hai;")
            await pilot.press("ctrl+up")
            self.assertEqual(app.query_one(QueryEditor).text, "BATAO nahi_hai;")
            await pilot.press("ctrl+up")
            self.assertEqual(app.query_one(QueryEditor).text, "DIKHAO TABLES;")

    async def test_selecting_table_in_tree_previews_it(self):
        app = MeraDBApp(Engine(self.dir))
        async with app.run_test() as pilot:
            await self.run_query(app, pilot, "BANAO TABLE t (x INT); DAALO MEIN t MAAN (1), (2), (3);")
            tree = app.query_one("#schema", Tree)
            tree.select_node(tree.root.children[0].children[0])
            await pilot.pause()
            self.assertEqual(app.query_one("#results", DataTable).row_count, 3)

    async def test_highlighting(self):
        app = MeraDBApp(Engine(self.dir))
        async with app.run_test() as pilot:
            editor = app.query_one(QueryEditor)
            editor.text = "DIKHAO naam SE s JAHAN umar > 18 -- comment"
            await pilot.pause()
            kinds = {kind for _, _, kind in editor._highlights[0]}
            self.assertTrue({"keyword", "number", "operator", "comment"} <= kinds)


if __name__ == "__main__":
    unittest.main()
