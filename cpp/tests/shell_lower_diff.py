"""
`.help <topic>` and dot-command words with non-ASCII text print exactly what `python -m meradb shell` prints
(Python's str.lower() on user text: other alphabets, final sigma, U+0130, the Kelvin sign).

    python cpp/tests/shell_lower_diff.py path/to/meradb_cli

Both shells read the same piped script in the same temp data folder (MERADB_* removed from the environment).
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CLI = sys.argv[1] if len(sys.argv) > 1 else str(ROOT / "cpp" / "build" / "meradb_cli")

K = "K"  # KELVIN SIGN, lower() is a plain "k"
TOPICS = [
    "ÉCOLE",  # prints the lower-cased echo
    "ΣΑΣ",  # final sigma at the end
    "ΑΣ Α",
    "ΑΣ.",
    "Σ",
    "İ",  # i + combining dot above
    "İSTANBUL",
    f"{K}unji",  # matches the MUKHYA KUNJI rows
    f"mukhya {K}unji",
    f"FOREIGN {K}EY",
    "É",
    "ЖУК",  # Cyrillic
    "नमस्ते",  # Devanagari (no case)
    "\U00010400",  # Deseret, outside the BMP
    "ẞ",
    "ǅ",
    "JOIN",
    "foreign key",
]
COMMANDS = [".HELP join", f".N{chr(0x130)}KAL", f".NI{K}AL", ".ÉXIT", f".{K}", ".EXIT"]


def script():
    lines = [f".help {t}" for t in TOPICS]
    return "\n".join(lines) + "\nDIKHAO TABLES;\n"


def run(cmd, data, text, cwd=None):
    env = {k: v for k, v in os.environ.items() if not k.startswith("MERADB_")}
    env["PYTHONIOENCODING"] = "utf-8"
    r = subprocess.run(cmd + ["shell", "--local", "--data", data], input=text.encode("utf-8"), capture_output=True,
                       timeout=120, env=env, cwd=cwd)
    return r.returncode, r.stdout.decode("utf-8").replace("\r\n", "\n"), r.stderr.decode("utf-8").replace("\r\n", "\n")


def main():
    root = tempfile.mkdtemp(prefix="mdb_lower_")
    failures = 0
    try:
        # each command word gets its own run: some of them end the shell
        cases = [("topics", script())] + [(f"command {ascii(c)}", c + "\nDIKHAO TABLES;\n") for c in COMMANDS]
        for name, text in cases:
            data = os.path.join(root, "data")
            c = run([CLI], data, text)
            p = run([sys.executable, "-m", "meradb"], data, text, cwd=str(ROOT))
            ok = c == p
            print(("PASS " if ok else "FAIL ") + name)
            if not ok:
                failures += 1
                print("--- C++ ---\n" + ascii(c) + "\n--- Python ---\n" + ascii(p))
    finally:
        shutil.rmtree(root, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
