"""The C++ test suite replays cpp/tests/golden_engine.h: the Python engine's own answers, statement by statement. If this
fails, the Python engine no longer answers a golden script the way the header (and so the C++ engine) says it should."""

import os
import subprocess
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


class GoldenInSyncTest(unittest.TestCase):
    def test_python_engine_still_gives_every_golden_answer(self):
        script = os.path.join(ROOT, "cpp", "tests", "golden", "gen_golden.py")
        done = subprocess.run([sys.executable, script, "--check"], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)


if __name__ == "__main__":
    unittest.main()
