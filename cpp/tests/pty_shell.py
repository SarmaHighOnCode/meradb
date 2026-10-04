"""
The interactive shell on a real pseudo-terminal (POSIX), the C++ one next to `python -m meradb shell` on the same
kind of terminal: prompts and continuation, Ctrl+D, Ctrl+C at a prompt / mid-statement / during a statement (exit
130), line editing (done by the terminal driver), colour only on a terminal, the hidden password prompt, and the
terminal modes after exit.

    python cpp/tests/pty_shell.py path/to/meradb_cli

Exits 77 (ctest: skipped) where there is no pty (Windows).
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

import ptyutil
from ptyutil import Checker, Pty, clean_env, python_env, python_shell_argv, strip_ansi

CLI = sys.argv[1]
if not ptyutil.can_open_pty():
    print("SKIP: cannot open a pseudo-terminal here")
    sys.exit(77)

ck = Checker()
ROWS = 1500   # two tables of this many rows: their cross join takes a second or more


def banner_end(text):
    marker = "se khatam hote hain"
    at = text.find(marker)
    return text[at + len(marker):] if at >= 0 else text


def normalise(text, *folders):
    text = text.replace("\r\n", "\n")
    for f in folders:
        text = text.replace(f, "<D>")
    return text


def session(argv, env, steps, timeout=20):
    """Start `argv` on a pty, wait for the first prompt, play `steps` (bytes to send, or a float to sleep) and
    return (exit code or None, output after the banner, the Pty's termios attributes before and after)."""
    with Pty(argv, env=env) as p:
        ok = p.expect(b"main> ", timeout)
        before = p.attrs()
        mark = len(p.buf)
        for step in steps:
            if isinstance(step, (int, float)):
                time.sleep(step)
                p.pump(0.05)
            else:
                p.send(step)
                p.pump(0.15)
        rc = p.wait(timeout)
        out = p.buf[mark:].decode("utf-8", "replace")
        after = p.attrs()
        if rc is None:
            p.close()
        return ok, rc, out, before, after


def both(label, steps, setup=None, timeout=20, colour=False, compare=True, data_script=None):
    """Run the same keystrokes through the C++ shell and the Python shell, each with its own fresh data folder
    (made by `setup`, if any), and return both results; with `compare` check that they printed the same."""
    results = {}
    for name in ("cpp", "py"):
        root = tempfile.mkdtemp(prefix="mdb_ptysh_")
        try:
            data = os.path.join(root, "data")
            if setup:
                setup(data)
            if name == "cpp":
                argv, env = [CLI, "shell", "--local", "-D", data], clean_env()
            else:
                argv, env = python_shell_argv("shell", "--local", "-D", data), python_env()
            if not colour:
                env["NO_COLOR"] = "1"
            ok, rc, out, before, after = session(argv, env, steps, timeout)
            results[name] = (ok, rc, normalise(out if colour else strip_ansi(out), root, data), before, after)
        finally:
            shutil.rmtree(root, ignore_errors=True)
    cpp, py = results["cpp"], results["py"]
    if compare:
        ck.check(label + ": same exit code and output as Python", cpp[1] == py[1] and cpp[2] == py[2],
                 "C++:    %r %r\nPython: %r %r" % (cpp[1], cpp[2], py[1], py[2]))
    return cpp, py


# ---- prompts, statements, continuation ----------------------------------------------------------------------------
cpp, py = both("statement over two lines, then .exit",
               [b"BANAO TABLE t (id INT, naam TEXT);\n", b"DAALO MEIN t MAAN\n", b"(1, 'Ravi');\n", b"DIKHAO * SE t;\n", b".exit\n"])
ck.check("prompts: meradb:main> then the continuation prompt",
         "meradb:main> DAALO MEIN t MAAN\n      ...> (1, 'Ravi');" in cpp[2] and cpp[2].endswith("Phir milenge!\n") and cpp[1] == 0,
         repr(cpp[2]))
ck.check("a table result is drawn", "| id | naam |" in cpp[2] and "| 1  | Ravi |" in cpp[2], repr(cpp[2]))

cpp, py = both("pasted block (several lines in one write)",
               [b"BANAO TABLE p (id INT);\nDAALO MEIN p MAAN (7);\nDIKHAO * SE p;\n.exit\n"])
ck.check("a pasted block runs every statement", "| 7  |" in cpp[2] and cpp[1] == 0, repr(cpp[2]))

# ---- Ctrl+D --------------------------------------------------------------------------------------------------------
cpp, py = both("Ctrl+D at an empty prompt", [b"\x04"])
ck.check("Ctrl+D at an empty prompt: exit 0 after a newline and 'Phir milenge!'",
         cpp[1] == 0 and cpp[2] == "\nPhir milenge!\n", repr(cpp[2]))  # output after the first prompt
ck.check("Ctrl+D at an empty prompt: the terminal modes are unchanged", cpp[3] is None or cpp[4] is None or cpp[3] == cpp[4], "")

# ---- Ctrl+C --------------------------------------------------------------------------------------------------------
cpp, py = both("Ctrl+C at an empty prompt", [b"\x03"])
ck.check("Ctrl+C at a prompt: '^C', newline, 'Phir milenge!', exit 0",
         cpp[1] == 0 and cpp[2] == "^C\nPhir milenge!\n", repr(cpp[2]))  # the driver echoes ^C, then the shell's goodbye
cpp, py = both("Ctrl+C in the middle of a statement", [b"DIKHAO\n", b"\x03"])
ck.check("Ctrl+C on a continuation line: leaves like at a prompt, exit 0",
         cpp[1] == 0 and cpp[2].endswith("      ...> ^C\nPhir milenge!\n"), repr(cpp[2]))
cpp, py = both("Ctrl+C after half a typed line", [b"DIKHAO GIN", b"\x03"])
ck.check("Ctrl+C with typed text: the text is discarded, exit 0", cpp[1] == 0 and "Phir milenge!" in cpp[2], repr(cpp[2]))

# ---- line editing is the terminal driver's ---------------------------------------------------------------------------
both("Backspace", [b"DIKHAX\x7f O TABLES;\n", b".exit\n"])
both("Ctrl+U kills the line", [b"garbage text", b"\x15", b"DIKHAO TABLES;\n", b".exit\n"])
both("Ctrl+W deletes a word", [b"DIKHAO junk", b"\x17", b"\x17", b"DIKHAO TABLES;\n", b".exit\n"])
cpp, py = both("Backspace erases a whole multi-byte character",
               [("DIKHAO 'é\U0001F600'".encode() + b"\x7f\x7f\x7f1';\n"), b".exit\n"])
ck.check("UTF-8 text typed and erased: still valid text", "Tokenizer Galti" not in cpp[2] and cpp[1] == 0, repr(cpp[2]))
cpp, py = both("Unicode statement round trip",
               [b"BANAO TABLE u (s TEXT);\n", "DAALO MEIN u MAAN ('é\U0001F600नमस्ते');\n".encode(), b"DIKHAO * SE u;\n", b".exit\n"])
ck.check("the Unicode text comes back intact", "é\U0001F600नमस्ते" in cpp[2], repr(cpp[2]))

# Ctrl+D after typed text is the kernel's: it hands over what was typed. The shells differ in how many presses they
# need to treat the unfinished line as entered (documented in docs/CPP.md).
cpp, py = both("Ctrl+D after text, three times", [b"DIKHAO TABLES", b"\x04", b"\x04", b"\x04", b";\n", b".exit\n"], compare=False)
ck.check("Ctrl+D after text does not end the shell by itself",
         cpp[1] == 0 and "Phir milenge!" in cpp[2], repr(cpp[2]))

# ---- Ctrl+C while a statement runs: it finishes, the shell exits 130 ----------------------------------------------------


def make_big(data):
    rows = ",".join("(%d)" % i for i in range(ROWS))
    script = data + "_setup.mdb"
    with open(script, "w") as f:
        f.write("BANAO TABLE a (id INT);\nBANAO TABLE b (id INT);\nDAALO MEIN a MAAN %s;\nDAALO MEIN b MAAN %s;\n" % (rows, rows))
    subprocess.run([CLI, "run", "--local", "-D", data, script], capture_output=True, check=True, timeout=120)
    os.remove(script)


cpp, py = both("Ctrl+C during a slow statement", [b"DIKHAO GINO(*) SE a MILAO b PAR 1 = 1;\n", 0.4, b"\x03"],
               setup=make_big, timeout=120, compare=False)
ck.check("Ctrl+C during a statement: exit 130", cpp[1] == 130, "exit %r" % (cpp[1],))
ck.check("Ctrl+C during a statement: the statement finished and its result is printed (never abandoned half-way)",
         ("| %d |" % (ROWS * ROWS)) in cpp[2], repr(cpp[2][-300:]))
ck.check("Ctrl+C during a statement: nothing after the result (no goodbye, no new prompt)",
         cpp[2].rstrip("\n").endswith("1 row(s)"), repr(cpp[2][-200:]))
ck.check("Python also exits 130 there", py[1] == 130, "exit %r" % (py[1],))

# Ctrl+C racing with Enter (checklist row 16): never a hang, and the exit code is one of the two documented ones.
codes = {}
hung = 0
for i in range(30):
    root = tempfile.mkdtemp(prefix="mdb_ptysh_")
    try:
        with Pty([CLI, "shell", "--local", "-D", os.path.join(root, "d")], env=clean_env(NO_COLOR="1")) as p:
            p.expect(b"main> ", 10)
            p.send(b"DIKHAO TABLES;\n")
            time.sleep((i % 6) * 0.003)
            p.send(b"\x03")
            rc = p.wait(15)
            if rc is None:
                hung += 1
                p.send(b"\x04")
                p.wait(5)
            codes[rc] = codes.get(rc, 0) + 1
    finally:
        shutil.rmtree(root, ignore_errors=True)
ck.check("Ctrl+C racing with Enter: no hang in 30 tries", hung == 0, "hung %d, exit codes %r" % (hung, codes))
ck.check("Ctrl+C racing with Enter: exit 0 (at the prompt) or 130 (after the statement)", set(codes) <= {0, 130}, repr(codes))

# ---- colour only on a terminal -----------------------------------------------------------------------------------------
for name, argv_for in (("C++", lambda d: [CLI, "shell", "--local", "-D", d]),
                       ("Python", lambda d: python_shell_argv("shell", "--local", "-D", d))):
    root = tempfile.mkdtemp(prefix="mdb_ptysh_")
    try:
        env = clean_env() if name == "C++" else python_env()
        ok, rc, out, _, _ = session(argv_for(os.path.join(root, "d")), env, [b"DIKHAO TABLES;\n", b".exit\n"])
        ck.check("%s shell on a terminal uses colour" % name, "\x1b[" in out, repr(out[:120]))
        # NO_COLOR on a terminal
        env["NO_COLOR"] = "1"
        ok, rc, out, _, _ = session(argv_for(os.path.join(root, "d2")), env, [b"DIKHAO TABLES;\n", b".exit\n"])
        ck.check("%s shell with NO_COLOR on a terminal is plain" % name, "\x1b[" not in out, repr(out[:120]))
        # stdin piped: plain, whatever TERM says
        env = clean_env() if name == "C++" else python_env()
        r = subprocess.run(argv_for(os.path.join(root, "d3")), input=b"DIKHAO TABLES;\n", capture_output=True, env=env, timeout=60)
        ck.check("%s shell with piped stdin / stdout is plain" % name, b"\x1b[" not in r.stdout + r.stderr, repr(r.stdout[:80]))
    finally:
        shutil.rmtree(root, ignore_errors=True)

# the coloured session of both shells is byte for byte the same
cpp, py = both("coloured transcript on a terminal", [b"BANAO TABLE c (id INT);\n", b"DIKHAO * SE nahi_hai;\n", b"DIKHAO * SE c;\n", b".exit\n"],
               colour=True)

# ---- hidden password prompt (-W) against a server -----------------------------------------------------------------------


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


root = tempfile.mkdtemp(prefix="mdb_ptysh_")
server = None
try:
    port = free_port()
    data = os.path.join(root, "data")
    server = subprocess.Popen([CLI, "server", "-D", data, "--port", str(port), "--password", "geheim"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=clean_env())
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.2).close()
            break
        except OSError:
            time.sleep(0.1)
    with Pty([CLI, "shell", "-W", "-H", "127.0.0.1", "-p", str(port)], env=clean_env(NO_COLOR="1")) as p:
        asked = p.expect(b"assword", 10)
        attrs = p.attrs()
        echo_off = attrs is not None and not (attrs[3] & ptyutil.termios.ECHO)
        ck.check("-W: asks for the password", asked, p.text()[-200:])
        ck.check("-W: the terminal does not echo while the password is typed", echo_off or attrs is None, "")
        p.send(b"geheim\n")
        got_prompt = p.expect(b"main> ", 10)
        text = p.text()
        ck.check("-W: the typed password is not shown", "geheim" not in text, repr(text[-200:]))
        ck.check("-W: a right password reaches the shell prompt", got_prompt, repr(text[-200:]))
        attrs = p.attrs()
        ck.check("-W: echo is back on after the prompt", attrs is None or bool(attrs[3] & ptyutil.termios.ECHO), "")
        p.send(b".exit\n")
        p.wait(10)
    with Pty([CLI, "shell", "-W", "-H", "127.0.0.1", "-p", str(port)], env=clean_env(NO_COLOR="1")) as p:
        p.expect(b"assword", 10)
        p.send(b"falsch\n")
        rc = p.wait(15)
        text = p.text()
        ck.check("-W: a wrong password exits 1 without the banner", rc == 1 and "MERA" not in text and "falsch" not in text,
                 "%r %r" % (rc, text[-300:]))
finally:
    if server is not None:
        server.kill()
        server.wait()
    shutil.rmtree(root, ignore_errors=True)

sys.exit(ck.finish())
