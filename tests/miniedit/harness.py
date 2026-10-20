"""
pty-based harness for driving the dietlibc dash build interactively.

Spawns dash -i on one end of a pty, sends keystrokes, and captures
everything it writes back.  It answers the "\\x1b[6n" cursor position
query the way a real terminal would, because Ctrl+L still asks for the
cursor's row.  Ordinary editing never asks: the tests check that
(see test_no_cursor_query_while_editing), so a session that does not
answer would not hide anything else.

Each Session gets its own isolated $HOME (so ~/.bash_history tests
don't cross-contaminate), and is torn down with SIGKILL + waitpid
rather than relying on the shell's own `exit` - sending `exit` as
cleanup interacts badly with whatever is currently sitting in the
edit buffer (e.g. a just-recalled history entry) and previously
corrupted the very history file being tested. A per-session hard
wall-clock budget guards against a hung child wedging the whole
suite (this bit us once already, from leaked pty fds across many
back-to-back sessions in one process).
"""

import contextlib
import fcntl
import os
import pty
import select
import struct
import sys
import tempfile
import termios
import time

from vtsim import simulate

DEFAULT_DASH = os.path.join(
    os.path.dirname(__file__), "..", "..", "src", "dash"
)


class Session:
    def __init__(self, dash_path=None, width=80, height=24, ps1="",
                 home=None, budget=5.0, cwd=None):
        self.width = width
        self.height = height
        self.dash_path = dash_path or os.environ.get("MINIEDIT_DASH",
                                                       DEFAULT_DASH)
        self._tmphome = None
        if home is None:
            self._tmphome = tempfile.mkdtemp(prefix="miniedit-test-")
            home = self._tmphome
        self.home = home
        self.deadline = time.time() + budget
        self.data = b""

        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ,
                    struct.pack("HHHH", height, width, 0, 0))
        pid = os.fork()
        if pid == 0:
            os.setsid()
            os.close(master)
            os.dup2(slave, 0)
            os.dup2(slave, 1)
            os.dup2(slave, 2)
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)
            if cwd:
                os.chdir(cwd)
            env = {
                "PATH": os.environ.get("PATH", ""),
                "LANG": "en_US.UTF-8",
                "PS1": ps1,
                "HOME": home,
            }
            try:
                os.execve(self.dash_path, ["dash", "-i"], env)
            finally:
                os._exit(127)
        os.close(slave)
        self.master = master
        self.pid = pid
        self._pump(settle=0.3)

    def send(self, s):
        """Write `s` (str) to the pty, then drain and process
        whatever dash writes back in response (including answering
        any cursor-position query)."""
        os.write(self.master, s.encode("utf-8"))
        self._pump()

    def _pump(self, settle=0.15):
        while time.time() < self.deadline:
            r, _, _ = select.select([self.master], [], [], settle)
            if not r:
                return
            try:
                chunk = os.read(self.master, 4096)
            except OSError:
                return
            if not chunk:
                return
            self.data += chunk
            if b"\x1b[6n" in chunk:
                rows, row, col = simulate(self.data, self.width,
                                           self.height)
                # col is already self.width when the cursor is pending at
                # the margin; a real terminal reports that same column,
                # 1-based, never one past the right edge.
                os.write(self.master,
                         "\x1b[{};{}R".format(row + 1,
                                               min(col + 1, self.width))
                         .encode())

    def screen(self):
        """Return (rows, row, col) for everything captured so far."""
        return simulate(self.data, self.width, self.height)

    def scrollback(self):
        """Rows that have scrolled off the top of the screen so far."""
        sb = []
        simulate(self.data, self.width, self.height, sb)
        return sb

    def text(self):
        """Raw captured bytes, decoded, for tests that want to
        pattern-match on output rather than screen layout."""
        return self.data.decode("utf-8", "replace")

    def close(self):
        with contextlib.suppress(Exception):
            os.kill(self.pid, 9)
        with contextlib.suppress(Exception):
            os.waitpid(self.pid, 0)
        with contextlib.suppress(Exception):
            os.close(self.master)
        if self._tmphome:
            import shutil
            with contextlib.suppress(Exception):
                shutil.rmtree(self._tmphome)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()


ENTER = "\n"
BACKSPACE = "\x7f"
DELETE = "\x1b[3~"
LEFT = "\x1b[D"
RIGHT = "\x1b[C"
CTRL_LEFT = "\x1b[1;5D"
CTRL_RIGHT = "\x1b[1;5C"
HOME = "\x1b[H"
END = "\x1b[F"
UP = "\x1b[A"
DOWN = "\x1b[B"
CTRL_R = "\x12"
CTRL_C = "\x03"
CTRL_W = "\x17"
CTRL_L = "\x0c"


def run(steps, **kw):
    """Convenience one-shot: run `steps` in a fresh Session and
    return it (already closed is the caller's job via `with`, or
    just let it be garbage collected after reading .screen()/.text())
    """
    s = Session(**kw)
    for step in steps:
        s.send(step)
    return s
