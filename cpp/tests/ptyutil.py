"""
Helpers for the pseudo-terminal tests (pty_shell.py, pty_workbench.py, pty_keys.py): run a program on a real
pseudo-terminal, send it keystrokes, read what it prints, look at the terminal attributes, and rebuild the screen
of a full-screen program from the bytes it wrote (a minimal VT100 / xterm screen model).

POSIX only: `import ptyutil` raises Skip on a system without pty / termios (Windows), and the tests exit 77
(ctest's skip code) then.
"""
import os
import re
import shutil
import signal
import sys
import tempfile
import time
import unicodedata

try:
    import fcntl
    import pty
    import select
    import struct
    import subprocess
    import termios
except ImportError:  # Windows
    print("SKIP: no pty / termios on this platform")
    sys.exit(77)

if not hasattr(os, "openpty"):
    print("SKIP: no os.openpty")
    sys.exit(77)

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def clean_env(**extra):
    """The environment without MERADB_* variables (and without colour switches), plus `extra`."""
    env = {k: v for k, v in os.environ.items()
           if not k.startswith("MERADB_") and k not in ("NO_COLOR", "FORCE_COLOR", "PYTHONSTARTUP")}
    env["TERM"] = "xterm-256color"
    env["PYTHONIOENCODING"] = "utf-8"
    env["LANG"] = env.get("LANG", "C.UTF-8")
    env.update(extra)
    return env


def python_shell_argv(*args):
    return [sys.executable, "-m", "meradb", *args]


def python_env(**extra):
    env = clean_env(**extra)
    env["PYTHONPATH"] = ROOT
    return env


def can_open_pty():
    try:
        master, slave = os.openpty()
    except OSError:
        return False
    os.close(master)
    os.close(slave)
    return True


class Pty:
    """A child process whose stdin, stdout and stderr are one pseudo-terminal."""

    def __init__(self, argv, env=None, rows=40, cols=120, cwd=None, utf8_erase=True):
        self.argv = argv
        self.buf = b""
        self.returncode = None
        self.master, slave = os.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        if utf8_erase and hasattr(termios, "IUTF8"):
            # terminal emulators and ssh set IUTF8, so that Backspace erases a whole multi-byte character
            attrs = termios.tcgetattr(slave)
            attrs[0] |= termios.IUTF8
            termios.tcsetattr(slave, termios.TCSANOW, attrs)
        self.initial_attrs = termios.tcgetattr(slave)   # what the program finds when it starts

        def become_controlling():
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)

        self.proc = subprocess.Popen(argv, stdin=slave, stdout=slave, stderr=slave, env=env or clean_env(), cwd=cwd,
                                     start_new_session=True, preexec_fn=become_controlling, close_fds=True)
        os.close(slave)

    # ---- output ----
    def pump(self, timeout=0.05):
        """Read whatever is available within `timeout` seconds; returns the new bytes."""
        got = b""
        end = time.time() + timeout
        while True:
            left = max(0.0, end - time.time())
            ready, _, _ = select.select([self.master], [], [], left)
            if not ready:
                break
            try:
                chunk = os.read(self.master, 65536)
            except OSError:  # EIO: the child side is closed
                break
            if not chunk:
                break
            got += chunk
            end = time.time() + 0.02  # keep draining while data keeps arriving
        self.buf += got
        return got

    def expect(self, needle, timeout=10.0, start=0):
        """Wait until `needle` (bytes or a compiled bytes regex) appears in the output from offset `start`."""
        end = time.time() + timeout
        while True:
            self.pump(0.05)
            hay = self.buf[start:]
            if isinstance(needle, bytes):
                if needle in hay:
                    return True
            elif needle.search(hay):
                return True
            if time.time() > end or self.proc.poll() is not None and not self._more():
                self.pump(0.05)
                hay = self.buf[start:]
                if isinstance(needle, bytes):
                    return needle in hay
                return bool(needle.search(hay))

    def _more(self):
        ready, _, _ = select.select([self.master], [], [], 0)
        return bool(ready)

    def text(self):
        return self.buf.decode("utf-8", "replace")

    # ---- input ----
    def send(self, data):
        if isinstance(data, str):
            data = data.encode("utf-8")
        os.write(self.master, data)

    # ---- state ----
    def attrs(self):
        """The terminal attributes as the child sees them (None if they cannot be read)."""
        try:
            return termios.tcgetattr(self.master)
        except termios.error:
            return None

    def wait(self, timeout=10.0):
        end = time.time() + timeout
        while self.proc.poll() is None and time.time() < end:
            self.pump(0.05)
        self.pump(0.05)
        self.returncode = self.proc.poll()
        return self.returncode

    def signal(self, sig):
        os.kill(self.proc.pid, sig)

    def close(self):
        """Kill the whole process group (the child and anything it started) and release the terminal."""
        try:
            if self.proc.poll() is None:
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                except (ProcessLookupError, PermissionError):
                    pass
            self.proc.wait(timeout=10)
        except Exception:
            pass
        try:
            os.close(self.master)
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


def flags(attrs):
    """(isig, ixon, icanon, echo, iexten) of a termios.tcgetattr() result."""
    iflag, _, _, lflag = attrs[0], attrs[1], attrs[2], attrs[3]
    return (bool(lflag & termios.ISIG), bool(iflag & termios.IXON), bool(lflag & termios.ICANON),
            bool(lflag & termios.ECHO), bool(lflag & termios.IEXTEN))


# ---------------------------------------------------------------------------------------------------------------
# a minimal screen model
# ---------------------------------------------------------------------------------------------------------------

class Screen:
    """Enough of an xterm for FTXUI's full-screen output: cursor movement, erase, SGR (ignored), private modes
    (ignored), UTF-8 text with double-width characters. The text of each row is what a person would read."""

    def __init__(self, rows=40, cols=120):
        self.rows, self.cols = rows, cols
        self.grid = [[" "] * cols for _ in range(rows)]
        self.x = self.y = 0
        self.modes = set()            # DEC private modes that are switched on (e.g. 1049, 2004, 1000)
        self._pending = b""
        self._csi = None

    def feed(self, data):
        data = self._pending + data
        self._pending = b""
        # keep an incomplete UTF-8 tail for the next call
        text = None
        for cut in range(0, 4):
            try:
                text = data[:len(data) - cut].decode("utf-8")
                self._pending = data[len(data) - cut:]
                break
            except UnicodeDecodeError:
                continue
        if text is None:
            text = data.decode("utf-8", "replace")
        for ch in text:
            self._char(ch)

    def _put(self, ch):
        width = 2 if unicodedata.east_asian_width(ch) in ("W", "F") else (0 if unicodedata.combining(ch) else 1)
        if width == 0:
            return
        if self.x + width > self.cols:
            self.x = 0
            self._down()
        self.grid[self.y][self.x] = ch
        if width == 2 and self.x + 1 < self.cols:
            self.grid[self.y][self.x + 1] = ""
        self.x += width

    def _down(self):
        if self.y == self.rows - 1:
            self.grid.pop(0)
            self.grid.append([" "] * self.cols)
        else:
            self.y += 1

    def _char(self, ch):
        state = self._csi
        if state is not None:
            kind, body = state
            if kind == "E":                      # just after ESC
                if ch == "[":
                    self._csi = ("C", "")
                elif ch == "]":
                    self._csi = ("O", "")
                elif ch in "()#%":
                    self._csi = ("X", "")
                else:                            # ESC 7, ESC 8, ESC =, ESC > ...
                    self._csi = None
            elif kind == "C":                    # CSI: parameters, then a final byte
                if "@" <= ch <= "~":
                    self._run_csi(body, ch)
                    self._csi = None
                else:
                    self._csi = ("C", body + ch)
            elif kind == "O":                    # OSC: ends with BEL or ST (ESC \)
                if ch == "\x07" or (ch == "\\" and body.endswith("\x1b")):
                    self._csi = None
                else:
                    self._csi = ("O", body + ch)
            else:                                # ESC ( B and friends: one more character
                self._csi = None
            return
        if ch == "\x1b":
            self._csi = ("E", "")
        elif ch == "\r":
            self.x = 0
        elif ch == "\n":
            self._down()
        elif ch == "\b":
            self.x = max(0, self.x - 1)
        elif ch == "\t":
            self.x = min(self.cols - 1, (self.x // 8 + 1) * 8)
        elif ch < " " or ch == "\x7f":
            pass
        else:
            self._put(ch)

    def _run_csi(self, params, final):
        private = params[:1] in ("?", ">", "<", "=")
        body = params[1:] if private else params
        nums = [int(p) if p.isdigit() else 0 for p in body.split(";")] if body else []

        def n(i, default=1):
            return nums[i] if i < len(nums) and nums[i] > 0 else default

        if private:
            if final in ("h", "l"):
                for mode in nums:
                    (self.modes.add if final == "h" else self.modes.discard)(mode)
            return
        if final in ("H", "f"):
            self.y = min(self.rows - 1, n(0) - 1)
            self.x = min(self.cols - 1, n(1) - 1)
        elif final == "A":
            self.y = max(0, self.y - n(0))
        elif final == "B":
            self.y = min(self.rows - 1, self.y + n(0))
        elif final == "C":
            self.x = min(self.cols - 1, self.x + n(0))
        elif final == "D":
            self.x = max(0, self.x - n(0))
        elif final == "G":
            self.x = min(self.cols - 1, n(0) - 1)
        elif final == "d":
            self.y = min(self.rows - 1, n(0) - 1)
        elif final == "J":
            mode = nums[0] if nums else 0
            if mode == 2 or mode == 3:
                self.grid = [[" "] * self.cols for _ in range(self.rows)]
            elif mode == 0:
                self.grid[self.y][self.x:] = [" "] * (self.cols - self.x)
                for r in range(self.y + 1, self.rows):
                    self.grid[r] = [" "] * self.cols
            elif mode == 1:
                self.grid[self.y][:self.x + 1] = [" "] * (self.x + 1)
                for r in range(0, self.y):
                    self.grid[r] = [" "] * self.cols
        elif final == "K":
            mode = nums[0] if nums else 0
            if mode == 0:
                self.grid[self.y][self.x:] = [" "] * (self.cols - self.x)
            elif mode == 1:
                self.grid[self.y][:self.x + 1] = [" "] * (self.x + 1)
            else:
                self.grid[self.y] = [" "] * self.cols
        # m (colours), r, s, u, t, c, n ... change nothing that is visible text

    def lines(self):
        return ["".join(c for c in row).rstrip() for row in self.grid]

    def text(self):
        return "\n".join(self.lines()).rstrip("\n")

    def contains(self, needle):
        return needle in self.text()


def make_data_dir(prefix="mdb_pty_"):
    return tempfile.mkdtemp(prefix=prefix)


def remove(path):
    shutil.rmtree(path, ignore_errors=True)


class Checker:
    def __init__(self):
        self.failures = []

    def check(self, name, condition, detail=""):
        print(("PASS " if condition else "FAIL ") + name)
        if not condition:
            self.failures.append(name)
            if detail:
                print("     " + str(detail).replace("\n", "\n     "))
        return condition

    def finish(self):
        if self.failures:
            print("\nFAILED: " + ", ".join(self.failures))
            return 1
        print("\nall checks passed")
        return 0


def strip_ansi(text):
    return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", text)
