# miniedit tests

Regression tests for `src/miniedit.c`, the small built-in line editor
this fork uses in place of libedit (which doesn't build cleanly
against dietlibc). They drive the actual built `dash` binary over a
real pty and check what ends up on screen, not just what a command
prints - most of the bugs this suite exists to catch (mid-line
backspace leaving stale characters, an off-by-one exactly at wrapped
line boundaries, Enter clobbering rows below the cursor, Ctrl+R
overwriting the prompt) were invisible from the executed command's
output alone.

## Running

```
cd src && CC="diet gcc" ./configure && make    # build dash first
cd ../tests/miniedit
python3 test_miniedit.py
```

Requires Python 3 (stdlib only - no pip packages). Not wired into
`make check`; this covers this fork's line editor specifically, not
dash as a whole, and needs the dietlibc build to exist first.

By default it looks for the binary at `../../src/dash`; point it
elsewhere with `MINIEDIT_DASH=/path/to/dash`.

## Layout

- `vtsim.py` - a small, deliberately partial VT100 simulator that
  turns the raw bytes dash writes into actual screen rows + cursor
  position. See its docstring for exactly what it does and doesn't
  model before trusting it for something new.
- `harness.py` - spawns `dash -i` on a pty, sends keystrokes, and
  answers the terminal cursor-position query (`\x1b[6n`) the way a
  real terminal would (miniedit relies on that query to find the
  true prompt column; without a real answer it silently falls back
  to column 1, which would hide real bugs).
- `test_miniedit.py` - the test cases themselves, plus a small
  dependency-free runner (`python3 test_miniedit.py`).

## Adding a test

Add a `test_*` function to `test_miniedit.py`; it's picked up
automatically. Use `Session` directly (see existing tests) when you
need to inspect the actual screen (`sess.screen()` -> `(rows, row,
col)`); use `output_of(steps)` when you only care what a submitted
command printed.

If you're adding a regression test for a bug you just fixed, it's
worth deliberately reverting the fix locally and confirming the new
test actually fails - a few of the tests in this suite initially
passed against a reintroduced bug because the assertions were too
loose (e.g. checking a row exists with the right content *somewhere*
nearby, rather than at the exact row it must be at). See the comment
on `test_wrap_pending_boundary` for a concrete example: the bug it
guards against is invisible unless there's real content *above* the
edit block for an over-shooting cursor movement to land on.
