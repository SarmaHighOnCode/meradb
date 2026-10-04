"""
The workbench's key table against the bytes a terminal sends (POSIX pseudo-terminal): wb_keyprobe is run with the
workbench's terminal-mode guard, fed the standard xterm / tmux / rxvt / Linux-console byte sequences, and the line it
shows for each key (the raw bytes FTXUI delivered, and what wb_keys.h makes of them) is compared with what the
workbench is meant to do. Also: the guard lets Ctrl+C / Ctrl+S / Ctrl+Q / Ctrl+O / Ctrl+R through as ordinary bytes
without ending or freezing the program, and the terminal modes are back as they were afterwards.

    python cpp/tests/pty_keys.py path/to/wb_keyprobe

Exits 77 (ctest: skipped) where there is no pty (Windows).
"""
import re
import sys
import time

import ptyutil
from ptyutil import Checker, Pty, Screen, clean_env, flags

PROBE = sys.argv[1]
if not ptyutil.can_open_pty():
    print("SKIP: cannot open a pseudo-terminal here")
    sys.exit(77)

ck = Checker()

# (label, bytes sent, the bytes FTXUI must hand over (hex), what wb_keys.h must make of them; "" = nothing)
ESC = "\x1b"
KEYS = [
    # function keys
    ("F1 (xterm)", ESC + "OP", "action Help"),
    ("F1 (linux console / rxvt)", ESC + "[11~", "action Help", "1B 4F 50"),   # FTXUI rewrites it to ESC O P
    ("F5", ESC + "[15~", "action Run"),
    ("F6", ESC + "[17~", "action Explain"),
    # Ctrl + arrows
    ("Ctrl+Up", ESC + "[1;5A", "action HistoryPrev"),
    ("Ctrl+Down", ESC + "[1;5B", "action HistoryNext"),
    ("Ctrl+Left", ESC + "[1;5D", "editor WordLeft"),
    ("Ctrl+Right", ESC + "[1;5C", "editor WordRight"),
    ("Ctrl+Home", ESC + "[1;5H", "editor DocStart"),
    ("Ctrl+End", ESC + "[1;5F", "editor DocEnd"),
    # Shift + movement
    ("Shift+Up", ESC + "[1;2A", "editor Up +select"),
    ("Shift+Down", ESC + "[1;2B", "editor Down +select"),
    ("Shift+Right", ESC + "[1;2C", "editor Right +select"),
    ("Shift+Left", ESC + "[1;2D", "editor Left +select"),
    ("Shift+Home", ESC + "[1;2H", "editor Home +select"),
    ("Shift+End", ESC + "[1;2F", "editor End +select"),
    ("Ctrl+Shift+Left", ESC + "[1;6D", "editor WordLeft +select"),
    ("Ctrl+Shift+Right", ESC + "[1;6C", "editor WordRight +select"),
    ("Shift+Up (rxvt)", ESC + "[a", "editor Up +select"),
    ("Shift+Down (rxvt)", ESC + "[b", "editor Down +select"),
    ("Shift+Right (rxvt)", ESC + "[c", "editor Right +select"),
    ("Shift+Left (rxvt)", ESC + "[d", "editor Left +select"),
    # plain movement
    ("Up", ESC + "[A", "editor Up"),
    ("Down", ESC + "[B", "editor Down"),
    ("Right", ESC + "[C", "editor Right"),
    ("Left", ESC + "[D", "editor Left"),
    ("Home (xterm)", ESC + "[H", "editor Home"),
    ("End (xterm)", ESC + "[F", "editor End"),
    ("Home (tmux / linux console)", ESC + "[1~", "editor Home"),
    ("End (tmux / linux console)", ESC + "[4~", "editor End"),
    ("Home (rxvt)", ESC + "[7~", "editor Home"),
    ("End (rxvt)", ESC + "[8~", "editor End"),
    ("Home (application cursor mode)", ESC + "OH", "editor Home", "1B 5B 48"),   # FTXUI rewrites it to ESC [ H
    ("End (application cursor mode)", ESC + "OF", "editor End", "1B 5B 46"),
    ("PageUp", ESC + "[5~", "editor PageUp"),
    ("PageDown", ESC + "[6~", "editor PageDown"),
    # control keys (ISIG / IXON / IEXTEN off through the guard: all arrive as bytes)
    ("Ctrl+R", "\x12", "action Run"),
    ("Ctrl+P", "\x10", "action HistoryPrev"),
    ("Ctrl+N", "\x0e", "action HistoryNext"),
    ("Ctrl+S", "\x13", "action Export"),
    ("Ctrl+O", "\x0f", "action Connect"),
    ("Ctrl+L", "\x0c", "action ClearLog"),
    ("Ctrl+Q", "\x11", "action Quit"),
    ("Ctrl+C", "\x03", "action Interrupt"),
    ("Ctrl+A", "\x01", "action SelectAll"),
    ("Tab", "\t", ""),
    ("Shift+Tab", ESC + "[Z", ""),
    ("Escape", ESC, ""),
]


def hexof(text):
    return " ".join("%02X" % b for b in text.encode("latin-1"))


def newest_line(screen):
    """The first 'bytes:' row on the screen (the probe shows the newest key first)."""
    for line in screen.lines():
        m = re.search(r"bytes: (.*)", line)
        if m:
            return m.group(1).rstrip(" │").rstrip()
    return None


with Pty([PROBE, "--guard"], env=clean_env(), rows=30, cols=100) as p:
    initial = p.initial_attrs
    screen = Screen(30, 100)
    p.expect(b"Key probe", 10)
    screen.feed(p.buf)
    seen = len(p.buf)

    def feed():
        global seen
        screen.feed(p.buf[seen:])
        seen = len(p.buf)

    running = p.attrs()
    isig, ixon, icanon, echo, iexten = flags(running)
    ck.check("the guard switches ISIG, IXON and IEXTEN off while the program runs", not isig and not ixon and not iexten,
             str(flags(running)))

    previous = None
    for label, sent, meaning, *rewritten in KEYS:
        p.send(sent)
        deadline = time.time() + (1.5 if sent == ESC else 3.0)
        line = previous
        while time.time() < deadline:
            p.pump(0.05)
            feed()
            line = newest_line(screen)
            if line != previous:
                break
        # the same key twice in a row shows the same text: compare through a separator row instead
        got_hex = None if line is None else re.match(r"((?:[0-9A-F]{2} )+)", line)
        got = got_hex.group(1).strip() if got_hex else None
        want = rewritten[0] if rewritten else hexof(sent)
        ok = got == want and (meaning in (line or "") if meaning else "->" not in (line or ""))
        ck.check("%s: %s %s%s" % (label, "arrives as" if rewritten else "bytes", want,
                                  (" and means '%s'" % meaning) if meaning else ""), ok,
                 "shown: %r" % (line,))
        previous = line

    ck.check("still running after Ctrl+C, Ctrl+S, Ctrl+Q, Ctrl+O and Ctrl+R (no exit, no freeze)", p.proc.poll() is None, "")
    p.send(b"x")
    rc = p.wait(10)
    ck.check("'x' ends the probe with exit 0", rc == 0, "exit %r" % (rc,))
    after = p.attrs()
    ck.check("the terminal modes are back as they were before the program ran",
             initial is None or after is None or (after[0] == initial[0] and after[3] == initial[3]),
             "before %r after %r" % (initial and flags(initial), after and flags(after)))
    out = p.text()
    ck.check("the alternate screen is left again", "\x1b[?1049l" in out, repr(out[-120:]))

sys.exit(ck.finish())
