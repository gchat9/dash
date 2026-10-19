#!/usr/bin/env python3
"""
Regression tests for src/miniedit.c (the dietlibc-friendly line
editor), driven over a real pty against the actual built dash binary.

Usage:
    python3 test_miniedit.py                # uses ../../src/dash
    MINIEDIT_DASH=/path/to/dash python3 test_miniedit.py

Exit status is 0 iff every test passed, so this is CI-friendly, but
nothing here is wired into `make check` - it needs python3 and is
entirely optional. See vtsim.py's docstring for what the simulated
screen can and can't be trusted to model.

Each test is a plain function that raises AssertionError on failure;
add new ones by adding to CASES. Tests that need to check what ended
up on screen use `sess.screen()`; tests that only care what a
command actually executed to use `output_of()`.
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))

from harness import (Session, run, ENTER, BACKSPACE, DELETE, LEFT,
                      RIGHT, CTRL_LEFT, CTRL_RIGHT, HOME, END, UP,
                      DOWN, CTRL_R, CTRL_C, CTRL_W, CTRL_L)

_ANSI_RE = re.compile(rb"\x1b\[[0-9;]*[A-Za-z]")


def output_of(steps, **kw):
    """Run `steps` (which must end by submitting a line) and return
    the last non-blank line of whatever appeared, with escape
    sequences stripped - i.e. what the submitted command printed, or
    the shell's own error message for it."""
    with Session(**kw) as s:
        for step in steps:
            s.send(step)
        clean = _ANSI_RE.sub(b"", s.data).decode("utf-8", "replace")
        lines = [l for l in clean.split("\r\n") if l.strip()]
        return lines[-1] if lines else None


# ---------------------------------------------------------------- #
# Basic editing
# ---------------------------------------------------------------- #

def test_utf8_backspace():
    # Two backspaces on "fóó" must remove two whole characters (four
    # bytes), not two bytes - the original bug this fork exists for.
    got = output_of(["echo f\u00f3\u00f3", BACKSPACE, BACKSPACE, "oo",
                      ENTER])
    assert got == "foo", got


def test_mid_line_insert():
    got = output_of(["echo helloworld", LEFT * 5, "_", ENTER])
    assert got == "hello_world", got


def test_mid_line_backspace_visual():
    # Regression: mid-line backspace used to leave a stale copy of
    # the deleted character on screen even though the buffer (and
    # thus the executed command) was already correct.
    with Session() as s:
        s.send("echo abcd")
        s.send(LEFT + LEFT)
        s.send("X")
        s.send(BACKSPACE)
        rows, row, col = s.screen()
        assert rows[0].rstrip() == "echo abcd", rows
        s.send(ENTER)


def test_delete_key():
    got = output_of(["echo abXcd", LEFT * 3, DELETE, ENTER])
    assert got == "abcd", got


def test_delete_utf8_char():
    got = output_of(["echo \u00e9\u00e8x", LEFT + LEFT, DELETE, ENTER])
    assert got == "\u00e9x", got


def test_home_end():
    got = output_of(["helloworld", HOME, "echo ", ENTER])
    assert got == "helloworld", got
    got = output_of(["echo hello", LEFT * 5, "well ", END, " world",
                      ENTER])
    assert got == "well hello world", got


# ---------------------------------------------------------------- #
# Word movement
# ---------------------------------------------------------------- #

def test_ctrl_left_word():
    got = output_of(["echo foo bar baz", CTRL_LEFT * 2, "X", ENTER])
    assert got == "foo Xbar baz", got


def test_ctrl_right_bash_style():
    # bash's forward-word semantics: land right after the next word,
    # not after the whitespace that follows it too.
    full = "echo foo    bar quix"
    got = output_of([full, LEFT * 11, CTRL_RIGHT, "X", ENTER])
    assert got == "foo barX quix", got


# ---------------------------------------------------------------- #
# Terminal wrapping (narrow terminal)
# ---------------------------------------------------------------- #

def test_wrap_pending_boundary():
    # Content landing exactly on a multiple of the terminal width
    # must not corrupt the display (an off-by-one here was a real,
    # previously-shipped bug). Crucially, there must be prior
    # content *above* the edit block: an erroneous extra "cursor up"
    # is invisible when row 0 is already the top of the screen (it
    # just clamps there, in both a real terminal and our simulator)
    # - the original bug report only showed up with earlier commands
    # already on screen for the cursor to overshoot into.
    with Session(width=20) as s:
        s.send("echo one" + ENTER)
        s.send("echo two" + ENTER)
        rows, row, col = s.screen()
        two_row = next(i for i, r in enumerate(rows) if r == "two")

        s.send("echo " + "a" * 15)  # exactly 20 chars
        rows, row, col = s.screen()
        assert rows[row] == "echo aaaaaaaaaaaaaaa", rows

        s.send("X")
        rows, row, col = s.screen()
        # the earlier commands' output must be untouched, at the
        # exact row it was always at - not shifted, not overwritten
        assert rows[two_row] == "two", rows
        assert rows[two_row + 1] == "echo aaaaaaaaaaaaaaa", rows
        assert rows[two_row + 2] == "X", rows
        s.send(ENTER)


def test_wrap_home_end():
    with Session(width=20) as s:
        s.send("echo " + "a" * 30)
        s.send(HOME)
        rows, row, col = s.screen()
        assert (row, col) == (0, 0), (row, col)
        s.send(LEFT * 35 + END)
        rows, row, col = s.screen()
        assert (row, col) == (1, 15), (row, col)
        s.send(ENTER)


def test_wrap_enter_no_clobber():
    # Regression: pressing Enter while the cursor is mid-line on a
    # wrapped multi-row command used to clobber the rows below it.
    with Session(width=20) as s:
        s.send("echo " + "a" * 20)
        s.send(HOME)
        s.send("X")
        rows, row, col = s.screen()
        assert rows[0] == "Xecho aaaaaaaaaaaaaa", rows
        assert rows[1] == "aaaaaa", rows
        s.send(ENTER)
        rows, row, col = s.screen()
        assert rows[0] == "Xecho aaaaaaaaaaaaaa", rows
        assert rows[1] == "aaaaaa", rows


# ---------------------------------------------------------------- #
# History (Up/Down + persistence)
# ---------------------------------------------------------------- #

def test_history_up_down():
    home = None
    with Session() as s:
        home = s.home
        s.send("echo one" + ENTER)
        s.send("echo two" + ENTER)
        s.send("echo three" + ENTER)
        s.send(UP)
        rows, row, col = s.screen()
        assert rows[row].strip() == "echo three", rows
        s.send(UP)
        rows, row, col = s.screen()
        assert rows[row].strip() == "echo two", rows
        s.send(DOWN)
        rows, row, col = s.screen()
        assert rows[row].strip() == "echo three", rows
        s.send(DOWN)
        rows, row, col = s.screen()
        assert rows[row].strip() == "", rows


def test_history_persists_across_processes():
    import shutil
    import tempfile
    home = tempfile.mkdtemp(prefix="miniedit-test-")
    try:
        with Session(home=home) as s1:
            s1.send("echo persisted" + ENTER)
        with Session(home=home) as s2:
            s2.send(UP)
            rows, row, col = s2.screen()
            assert rows[row].strip() == "echo persisted", rows
    finally:
        shutil.rmtree(home, ignore_errors=True)


def test_history_gracefully_absent():
    # No ~/.bash_history at all: Up/Down must be a silent no-op,
    # never a crash, and normal typing must still work.
    got = output_of([UP, DOWN, "echo ok", ENTER])
    assert got == "ok", got


# ---------------------------------------------------------------- #
# Ctrl-R incremental search
# ---------------------------------------------------------------- #

def _seed_history(home):
    path = os.path.join(home, ".bash_history")
    with open(path, "w") as f:
        f.write("echo apple\necho banana\necho cherry apple\n")


def test_ctrl_r_blank_until_typed():
    with Session(ps1="$ ") as s:
        _seed_history(s.home)
        s.send(CTRL_R)
        rows, row, col = s.screen()
        assert rows[0] == "$ (reverse-i-search)`': ", rows


def test_ctrl_r_match_and_cursor_position():
    with Session(ps1="$ ") as s:
        _seed_history(s.home)
        s.send(CTRL_R)
        s.send("appl")
        rows, row, col = s.screen()
        assert rows[0] == "$ (reverse-i-search)`appl': echo cherry apple", \
            rows
        # cursor must sit on the first letter of the *match*
        # ("apple" inside "cherry apple"), not at line/prefix end
        assert rows[row][col] == "a", (rows[row], col)


def test_ctrl_r_repeat_goes_older():
    with Session(ps1="$ ") as s:
        _seed_history(s.home)
        s.send(CTRL_R)
        s.send("appl")
        s.send(CTRL_R)
        rows, row, col = s.screen()
        assert rows[0].endswith("echo apple"), rows


def test_ctrl_r_enter_executes_match():
    got = None
    with Session() as s:
        _seed_history(s.home)
        s.send(CTRL_R)
        s.send("banana")
        s.send(ENTER)
        clean = _ANSI_RE.sub(b"", s.data).decode("utf-8", "replace")
        lines = [l for l in clean.split("\r\n") if l.strip()]
        got = lines[-1] if lines else None
    assert got == "banana", got


def test_ctrl_r_right_exits_relative_to_match():
    with Session() as s:
        _seed_history(s.home)
        s.send(CTRL_R)
        s.send("appl")  # matches "echo cherry apple", match at offset 12
        s.send(RIGHT)   # exit, cursor at matchoff+1 = 13 ('p')
        s.send("X")
        s.send(ENTER)
        clean = _ANSI_RE.sub(b"", s.data).decode("utf-8", "replace")
        lines = [l for l in clean.split("\r\n") if l.strip()]
        assert lines[-1] == "cherry aXpple", lines[-1]


def test_ctrl_r_exit_restores_prompt_cleanly():
    # Regression: exiting used to leave a truncated
    # "(reverse-i-search)" fragment because the search UI overwrote
    # the real prompt (column 1) while the exit redraw only erased
    # from the prompt's real column onward.
    with Session(ps1="$ ") as s:
        _seed_history(s.home)
        s.send(CTRL_R)
        s.send("apple")
        s.send(ENTER)
        rows, row, col = s.screen()
        # "apple" is a substring of both history entries; the most
        # recent one ("echo cherry apple") is the one that must win
        assert rows[0] == "$ echo cherry apple", rows
        assert "reverse-i-search" not in rows[0], rows


def test_ctrl_r_no_match_submits_empty():
    got = None
    with Session() as s:
        _seed_history(s.home)
        s.send(CTRL_R)
        s.send("zzz_no_such_thing")
        s.send(ENTER)
        clean = _ANSI_RE.sub(b"", s.data).decode("utf-8", "replace")
        # nothing should ever have been executed - only the prompt(s)
        assert "not found" not in clean, clean


# ---------------------------------------------------------------- #
# Ctrl-C
# ---------------------------------------------------------------- #

def test_ctrl_c_shows_feedback_and_resets():
    with Session() as s:
        s.send("echo hello world")
        s.send(CTRL_C)
        rows, row, col = s.screen()
        assert rows[0].endswith("^C"), rows
        # shell must still be alive and usable afterwards
        s.send("echo alive" + ENTER)
        clean = _ANSI_RE.sub(b"", s.data).decode("utf-8", "replace")
        lines = [l for l in clean.split("\r\n") if l.strip()]
        assert lines[-1] == "alive", lines[-1]


def test_ctrl_c_no_clobber_on_wrapped_line():
    with Session(width=20) as s:
        s.send("echo " + "a" * 30)
        s.send(HOME)
        s.send(CTRL_C)
        rows, row, col = s.screen()
        assert rows[0] == "echo aaaaaaaaaaaaaaa", rows
        assert rows[1] == "aaaaaaaaaaaaaaa^C", rows


def test_ctrl_c_inside_ctrl_r():
    with Session(ps1="$ ") as s:
        _seed_history(s.home)
        s.send(CTRL_R)
        s.send("ban")
        s.send(CTRL_C)
        rows, row, col = s.screen()
        assert rows[0] == "$ (reverse-i-search)`ban': echo banana^C", rows


# ---------------------------------------------------------------- #
# Ctrl-W (bash's unix-word-rubout)
# ---------------------------------------------------------------- #

def test_ctrl_w_deletes_word():
    got = output_of(["echo foo bar", CTRL_W, "baz", ENTER])
    assert got == "foo baz", got


def test_ctrl_w_skips_trailing_spaces_first():
    # spaces just before the cursor go too, then the word before them
    got = output_of(["echo foo   ", CTRL_W, "x", ENTER])
    assert got == "x", got


def test_ctrl_w_repeated():
    got = output_of(["echo a b c", CTRL_W * 2, ENTER])
    assert got == "a", got


def test_ctrl_w_mid_word_and_mid_line():
    # only the part of the word before the cursor is deleted ("ba" of
    # "bar"), and everything after the cursor is kept (checked
    # against bash/readline)
    got = output_of(["echo foo bar baz", LEFT * 5, CTRL_W, ENTER])
    assert got == "foo r baz", got


def test_ctrl_w_at_line_start_is_noop():
    got = output_of([CTRL_W, "echo ok", ENTER])
    assert got == "ok", got


def test_ctrl_w_utf8():
    got = output_of(["echo \u00e9\u00e8 \u00fc\u00df", CTRL_W, ENTER])
    assert got == "\u00e9\u00e8", got


def test_ctrl_w_wrapped_redraw():
    # removing a word that spans wrapped rows must leave no stale
    # characters on the rows it used to occupy
    with Session(width=20) as s:
        s.send("echo one")
        s.send(ENTER)
        s.send("echo " + "a" * 30)
        s.send(CTRL_W)
        rows, row, col = s.screen()
        assert rows[row].rstrip() == "echo", rows
        assert not any("a" in r for r in rows[2:]), rows
        s.send("x" + ENTER)


# ---------------------------------------------------------------- #
# Ctrl-L
# ---------------------------------------------------------------- #

def _fill(s, n=3):
    """Run n commands so the prompt ends up well down the screen."""
    for ch in "abcdefghij"[:n]:
        s.send("echo %s" % ch + ENTER)


def test_ctrl_l_scrolls_edit_line_to_top():
    # Everything above the edit line must scroll up (so it ends up in
    # the scrollback, not erased) until the edit line is the top row.
    with Session(height=10, ps1="$ ") as s:
        _fill(s, 3)
        s.send("echo hi")
        rows, row, col = s.screen()
        assert row == 6, (row, rows)
        before = rows[:6]
        s.send(CTRL_L)
        rows, row, col = s.screen()
        assert rows[0] == "$ echo hi", rows
        assert all(r == "" for r in rows[1:]), rows
        assert (row, col) == (0, 9), (row, col)
        # nothing was lost: it all went into the scrollback, in order
        assert s.scrollback()[-6:] == before, s.scrollback()


def test_ctrl_l_keeps_cursor_and_editing():
    with Session(height=10, ps1="$ ") as s:
        _fill(s, 3)
        s.send("echo hello" + LEFT * 3)
        s.send(CTRL_L)
        rows, row, col = s.screen()
        assert (row, col) == (0, 9), (row, col)    # still before "llo"
        s.send("X")
        rows, row, col = s.screen()
        assert rows[0] == "$ echo heXllo", rows
        s.send(ENTER)
        assert "heXllo" in s.text()


def test_ctrl_l_already_at_top_scrolls_nothing():
    with Session(height=10, ps1="$ ") as s:
        s.send("echo hi")
        s.send(CTRL_L)
        rows, row, col = s.screen()
        assert rows[0] == "$ echo hi", rows
        assert s.scrollback() == [], s.scrollback()


def test_ctrl_l_repeated_is_stable():
    with Session(height=10, ps1="$ ") as s:
        _fill(s, 2)
        s.send("echo hi")
        s.send(CTRL_L)
        n = len(s.scrollback())
        s.send(CTRL_L + CTRL_L)
        rows, row, col = s.screen()
        assert rows[0] == "$ echo hi", rows
        assert len(s.scrollback()) == n, s.scrollback()


def test_ctrl_l_wrapped_block_cursor_in_second_row():
    # a block spanning two terminal rows, cursor on its second row:
    # the *first* row of the block ends up on top, cursor keeps its place
    with Session(width=20, height=10, ps1="$ ") as s:
        _fill(s, 2)
        s.send("echo " + "a" * 20)      # 2 + 5 + 20 = 27 columns
        rows, row, col = s.screen()
        assert (row, col) == (5, 7), (row, col)
        s.send(CTRL_L)
        rows, row, col = s.screen()
        assert rows[0] == "$ echo " + "a" * 13, rows
        assert rows[1] == "a" * 7, rows
        assert all(r == "" for r in rows[2:]), rows
        assert (row, col) == (1, 7), (row, col)
        s.send("X")
        rows, row, col = s.screen()
        assert rows[1] == "a" * 7 + "X", rows


def test_ctrl_l_does_not_submit():
    with Session(height=10, ps1="$ ") as s:
        _fill(s, 2)
        s.send("echo notyet")
        s.send(CTRL_L)
        assert "\r\nnotyet" not in s.text()
        s.send(ENTER)
        assert "\r\nnotyet" in s.text()


# ---------------------------------------------------------------- #
# Tab: file name completion (expected screens match bash's)
# ---------------------------------------------------------------- #

class _tabdir:
    """A scratch directory holding some files, for completion tests."""
    FILES = ("apple", "apricot", "avocado", "banana", "a space",
             "a$dollar", ".hidden", ".hid2", "sp/one", "sp/two",
             "adir/f1", "adir/inner/x")

    def __init__(self, extra=()):
        import tempfile
        self.path = os.path.realpath(tempfile.mkdtemp(prefix="tab-"))
        for f in self.FILES + tuple(extra):
            full = os.path.join(self.path, f)
            os.makedirs(os.path.dirname(full), exist_ok=True)
            open(full, "w").close()

    def __enter__(self):
        return self.path

    def __exit__(self, *exc):
        import shutil
        shutil.rmtree(self.path, ignore_errors=True)


def _tab(keys, extra=(), **kw):
    """Run keys in a fresh session in a scratch dir; return
    (rows, row, col, bells) of the screen afterwards."""
    with _tabdir(extra) as d:
        with Session(ps1="$ ", cwd=d, home=d, **kw) as s:
            for k in keys:
                s.send(k)
            rows, row, col = s.screen()
            return rows, row, col, s.text().count("\x07")


def test_tab_unique_file_adds_space():
    rows, row, col, bells = _tab(["ls apr", "\t"])
    assert rows == ["$ ls apricot "] and bells == 0, rows
    assert (row, col) == (0, 13)


def test_tab_unique_dir_adds_slash():
    rows, row, col, bells = _tab(["ls adi", "\t"])
    assert rows == ["$ ls adir/"] and bells == 0, rows


def test_tab_ambiguous_rings_and_changes_nothing():
    rows, row, col, bells = _tab(["ls ap", "\t"])
    assert rows == ["$ ls ap"] and bells == 1, (rows, bells)


def test_tab_completes_common_prefix():
    rows, row, col, bells = _tab(["ls .h", "\t"])
    assert rows == ["$ ls .hid"] and bells == 1, rows


def test_tab_no_match_rings():
    rows, row, col, bells = _tab(["ls zzz", "\t"])
    assert rows == ["$ ls zzz"] and bells == 1, rows


def test_tab_second_tab_lists_in_columns():
    rows, row, col, bells = _tab(["ls a", "\t\t"])
    assert rows == ["$ ls a",
                    "a space   a$dollar  adir/     apple     apricot   avocado   ",
                    "$ ls a"], rows
    assert (row, col) == (2, 6)


def test_tab_list_includes_hidden_and_marks_dirs():
    rows, row, col, bells = _tab(["ls ", "\t\t"])
    assert rows == ["$ ls ",
                    ".hid2     a space   adir/     apricot   banana    ",
                    ".hidden   a$dollar  apple     avocado   sp/       ",
                    "$ ls "], rows


def test_tab_list_wraps_into_columns_for_the_width():
    rows, row, col, bells = _tab(["ls a", "\t\t"], width=30)
    assert rows == ["$ ls a",
                    "a space   adir/     apricot   ",
                    "a$dollar  apple     avocado   ",
                    "$ ls a"], rows


def test_tab_listing_needs_the_first_tab_to_change_nothing():
    # like bash: a Tab that completed something is not followed by a
    # listing on the next Tab; only one that got nowhere is
    rows, row, col, bells = _tab(["ls .h", "\t", "\t"])
    assert rows == ["$ ls .hid"] and bells == 2, (rows, bells)
    rows, row, col, bells = _tab(["ls .h", "\t", "\t", "\t"])
    assert rows[1:] == [".hid2    .hidden  ", "$ ls .hid"], rows


def test_tab_inside_a_directory():
    rows, row, col, bells = _tab(["ls sp/", "\t\t"])
    assert rows == ["$ ls sp/", "one  two  ", "$ ls sp/"], rows


def test_tab_tilde():
    rows, row, col, bells = _tab(["ls ~/adi", "\t"])
    assert rows == ["$ ls ~/adir/"], rows


def test_tab_word_starts_after_shell_separator():
    rows, row, col, bells = _tab(["echo hi; ls apr", "\t"])
    assert rows == ["$ echo hi; ls apricot "], rows


def test_tab_mid_line_keeps_the_rest_and_no_double_space():
    rows, row, col, bells = _tab(["ls apr tail", LEFT * 5, "\t"])
    assert rows == ["$ ls apricot tail"] and (row, col) == (0, 12), rows


def test_tab_escapes_special_characters():
    rows, row, col, bells = _tab(["ls a\\ sp", "\t"])
    assert rows == ["$ ls a\\ space "], rows
    rows, row, col, bells = _tab(["ls a\\$d", "\t"])
    assert rows == ["$ ls a\\$dollar "], rows
    # ...and where completion itself has to supply the backslashes
    rows, row, col, bells = _tab(["ls xx", "\t"], ("xx y",))
    assert rows == ["$ ls xx\\ y "], rows
    rows, row, col, bells = _tab(["ls ww", "\t"], ("ww(1)'q",))
    assert rows == ["$ ls ww\\(1\\)\\'q "], rows


def test_tab_completed_names_work_when_run():
    # the escaping has to be right for the shell, not just look right
    for typed, want in (("echo a\\ sp", "a space"), ("echo a\\$d", "a$dollar"),
                        ("echo z", "z{1,2}"), ("echo q", "q*?")):
        with _tabdir(("z{1,2}", "q*?")) as d:
            with Session(cwd=d, home=d) as s:
                s.send(typed + "\t" + ENTER)
                clean = _ANSI_RE.sub(b"", s.data).decode("utf-8", "replace")
                lines = [l for l in clean.split("\r\n") if l.strip()]
                assert lines[-1] == want, (typed, lines[-1])


def test_tab_asks_before_listing_many():
    many = tuple("f%03d" % i for i in range(120))
    rows, row, col, bells = _tab(["ls f", "\t\t", "n"], many)
    assert rows == ["$ ls f", "Display all 120 possibilities? (y or n)",
                    "$ ls f"], rows
    rows, row, col, bells = _tab(["ls f", "\t\t", "y"], many, height=40)
    assert rows[1].startswith("Display all 120") and rows[2].startswith("f000"), rows
    assert rows[-1] == "$ ls f" and "f119" in "".join(rows), rows


def test_tab_list_redraws_a_wrapped_line():
    rows, row, col, bells = _tab(["echo aaaaaaaaaaaaaaaaaaa ls al", "\t\t"],
                                 ("alpha_one", "alpha_two"), width=20)
    assert rows[-2:] == ["$ echo aaaaaaaaaaaaa", "aaaaaa ls alpha_"], rows


# ---------------------------------------------------------------- #
# Prompt escapes
# ---------------------------------------------------------------- #

def test_prompt_user_and_cwd():
    import getpass
    with Session(ps1=r"\u:\w> ") as s:
        want = "%s:%s> " % (getpass.getuser(), os.getcwd())
        assert s.screen()[0][0] == want, s.screen()[0]
        # typing must start right after the expanded prompt
        s.send("a")
        assert s.screen()[0][0] == want + "a", s.screen()[0]


def _prompt_w(home, cwd):
    """What \\w shows for the given $HOME and working directory."""
    old = os.getcwd()
    os.chdir(cwd)
    try:
        with Session(home=home, ps1=r"\w> ") as s:
            row = s.screen()[0][0]
    finally:
        os.chdir(old)
    assert row.endswith("> "), row
    return row[:-2]


def test_prompt_w_folds_home_into_tilde():
    import shutil
    import tempfile
    base = os.path.realpath(tempfile.mkdtemp(prefix="miniedit-test-"))
    try:
        for d in ("h/x", "hx"):
            os.makedirs(os.path.join(base, d))
        h = os.path.join(base, "h")
        assert _prompt_w(h, h) == "~"
        assert _prompt_w(h, h + "/x") == "~/x"
        assert _prompt_w(base, h) == "~/h"
        # all of the following are NOT folded, exactly as in bash:
        # a prefix that does not end at a path boundary...
        assert _prompt_w(h, base + "/hx") == base + "/hx"
        # ...a $HOME that is not a prefix at all...
        assert _prompt_w(h + "/x", h) == h
        # ...a $HOME with a trailing slash, an empty one, or just "/"
        assert _prompt_w(h + "/", h + "/x") == h + "/x"
        assert _prompt_w("", h) == h
        assert _prompt_w("/", h) == h
        assert _prompt_w("/", "/") == "/"
    finally:
        shutil.rmtree(base, ignore_errors=True)


def test_prompt_cwd_not_reexpanded():
    import shutil
    import tempfile
    base = tempfile.mkdtemp(prefix="miniedit-test-")
    odd = os.path.join(base, "we$ird`dir")
    os.mkdir(odd)
    old = os.getcwd()
    try:
        os.chdir(odd)
        with Session(ps1=r"\w> ") as s:
            assert s.screen()[0][0] == odd + "> ", s.screen()[0]
    finally:
        os.chdir(old)
        shutil.rmtree(base, ignore_errors=True)


# ---------------------------------------------------------------- #
# Runner
# ---------------------------------------------------------------- #

# ---------------------------------------------------------------- #
# History folding of multi-line compound commands
# ---------------------------------------------------------------- #

def _history_entries(home):
    with open(os.path.join(home, ".bash_history")) as f:
        return f.read().split(chr(10))[:-1]


def _type_and_run(s, lines):
    for ln in lines:
        s.send(ln)
        s.send(ENTER)
    # A trailing command so the folded entry is no longer the last one.
    s.send("echo done")
    s.send(ENTER)


def test_fold_if_else_into_one_entry():
    with Session(ps1="$ ") as s:
        _type_and_run(s, ["if true; then", "echo foo", "else",
                          "echo bar", "fi"])
        entries = _history_entries(s.home)
        assert entries == ["if true; then echo foo; else echo bar; fi",
                           "echo done"], entries


def test_fold_loop_into_one_entry():
    with Session(ps1="$ ") as s:
        _type_and_run(s, ["for i in 1 2; do", "  echo $i", "done"])
        entries = _history_entries(s.home)
        assert entries[0] == "for i in 1 2; do echo $i; done", entries


def test_fold_up_arrow_shows_folded_entry():
    with Session(ps1="$ ") as s:
        _type_and_run(s, ["if true; then", "echo foo", "fi"])
        s.send(UP)
        s.send(UP)
        rows, row, col = s.screen()
        assert any(r.endswith("if true; then echo foo; fi") for r in rows), rows


def test_fold_leaves_single_line_alone():
    with Session(ps1="$ ") as s:
        _type_and_run(s, ["echo one"])
        assert _history_entries(s.home) == ["echo one", "echo done"]


def test_fold_skips_open_quote():
    # A quote spanning lines is kept as typed rather than mangled.
    with Session(ps1="$ ") as s:
        _type_and_run(s, ["echo 'a", "b'"])
        entries = _history_entries(s.home)
        assert entries[0] == "echo 'a" and entries[1] == "b'", entries


def test_fold_skips_comment_and_heredoc():
    with Session(ps1="$ ") as s:
        _type_and_run(s, ["if true; then # note", "echo x", "fi"])
        entries = _history_entries(s.home)
        assert entries[:3] == ["if true; then # note", "echo x", "fi"], entries


def test_fold_nested_if():
    with Session(ps1="$ ") as s:
        _type_and_run(s, ["if true; then", "  if false; then", "echo no",
                          "  else", "echo yes", "  fi", "fi"])
        entries = _history_entries(s.home)
        assert entries[0] == "if true; then if false; then echo no; else echo yes; fi; fi", entries


def test_fold_function_definition():
    with Session(ps1="$ ") as s:
        _type_and_run(s, ["f()", "{", "echo in f", "}"])
        entries = _history_entries(s.home)
        assert entries[0] == "f() { echo in f; }", entries


def test_fold_entries_run_like_the_original():
    # A folded entry must mean the same thing as the lines typed.
    import subprocess
    dash = os.environ.get("MINIEDIT_DASH",
                          os.path.join(os.path.dirname(__file__),
                                       "..", "..", "src", "dash"))
    script = ("if false; then\necho no\nelse\nfor i in a b; do\n"
              "echo $i\ndone\nfi\n")
    with Session(ps1="$ ") as s:
        _type_and_run(s, script.rstrip("\n").split("\n"))
        folded = _history_entries(s.home)[0]
    want = subprocess.run([dash, "-c", script], capture_output=True,
                          text=True).stdout
    got = subprocess.run([dash, "-c", folded], capture_output=True,
                         text=True).stdout
    assert got == want and want == "a\nb\n", (folded, got, want)


def test_fold_not_disturbed_by_parallel_session():
    # Another dash sharing the same $HOME runs a command while this one
    # is halfway through an if.  Its entry must come out before (never
    # inside) the folded compound command.
    import tempfile
    home = tempfile.mkdtemp(prefix="miniedit-shared-")
    with Session(ps1="$ ", home=home) as a:
        a.send("if true; then"); a.send(ENTER)
        a.send("echo foo"); a.send(ENTER)
        with Session(ps1="$ ", home=home) as b:
            b.send("echo other"); b.send(ENTER)
        a.send("fi"); a.send(ENTER)
        entries = _history_entries(home)
        assert entries == ["echo other", "if true; then echo foo; fi"], entries


# ---------------------------------------------------------------- #
# A terminal that does not answer the cursor query (e.g. mc's subshell)
# ---------------------------------------------------------------- #

def _silent_terminal_session(keys, wait, until=None):
    """Run the shell on a pty that never answers ESC[6n, sending `keys`
    as soon as it starts.  Returns (output, seconds until it finished)."""
    import pty, select, tempfile, time
    dash = os.environ.get("MINIEDIT_DASH", os.path.join(
        os.path.dirname(__file__), "..", "..", "src", "dash"))
    home = tempfile.mkdtemp(prefix="miniedit-silent-")
    pid, fd = pty.fork()
    if pid == 0:
        os.environ.update(HOME=home, PS1="$ ", TERM="xterm")
        os.environ.pop("ENV", None)
        os.execv(dash, [dash])
    os.write(fd, keys)
    out = b""
    t0 = time.time()
    while time.time() - t0 < wait:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                d = os.read(fd, 65536)
            except OSError:
                break
            if not d:
                break
            out += d
            if until is not None and until in out:
                break
    took = time.time() - t0
    os.kill(pid, 9)
    os.waitpid(pid, 0)
    return out, took


def test_typed_ahead_input_is_not_eaten_by_cursor_query():
    # The bytes contain capital R, which the old cursor-report reader
    # swallowed as the end of its reply.  They must come through intact.
    word = "R" * 40 + "-ok"
    out, _ = _silent_terminal_session(("echo " + word + "\n").encode(), 3)
    assert ("\r\n" + word + "\r\n").encode() in out, out[-300:]


def test_silent_terminal_does_not_wait_per_prompt():
    keys = b"".join(b"echo n%d\n" % i for i in range(8))
    out, took = _silent_terminal_session(keys, 10, until=b"\r\nn7\r\n")
    assert b"\r\nn7\r\n" in out, out[-300:]
    assert took < 3, took


# ---------------------------------------------------------------- #
# History filters: HISTCONTROL=ignoreboth-style behaviour
# ---------------------------------------------------------------- #

def test_hist_skips_command_starting_with_space():
    with Session(ps1="$ ") as s:
        _type_and_run(s, [" echo secret", "echo shown"])
        assert _history_entries(s.home) == ["echo shown", "echo done"], \
            _history_entries(s.home)


def test_hist_skips_folded_compound_starting_with_space():
    with Session(ps1="$ ") as s:
        _type_and_run(s, [" if true; then", "echo foo", "fi"])
        assert _history_entries(s.home) == ["echo done"], \
            _history_entries(s.home)


def test_hist_skips_exact_duplicate():
    with Session(ps1="$ ") as s:
        _type_and_run(s, ["echo a", "echo b", "echo a"])
        assert _history_entries(s.home) == ["echo a", "echo b", "echo done"], \
            _history_entries(s.home)


def test_hist_duplicate_check_is_whole_file_not_last_entry():
    # Unlike bash's ignoredups, which only compares with the last
    # entry, an earlier copy anywhere in the file counts.
    with Session(ps1="$ ") as s:
        _type_and_run(s, ["echo a", "echo b", "echo c", "echo a"])
        assert _history_entries(s.home) == \
            ["echo a", "echo b", "echo c", "echo done"], \
            _history_entries(s.home)


def test_hist_duplicate_must_match_whole_line():
    # Substrings and prefixes of earlier commands are not duplicates.
    with Session(ps1="$ ") as s:
        _type_and_run(s, ["echo a", "echo ab", "echo a b"])
        assert _history_entries(s.home) == \
            ["echo a", "echo ab", "echo a b", "echo done"], \
            _history_entries(s.home)


def test_hist_skips_duplicate_folded_compound():
    with Session(ps1="$ ") as s:
        _type_and_run(s, ["if true; then", "echo x", "fi"])
        _type_and_run(s, ["if true; then", "echo x", "fi"])
        assert _history_entries(s.home) == \
            ["if true; then echo x; fi", "echo done"], \
            _history_entries(s.home)


CASES = [v for k, v in sorted(globals().items()) if k.startswith("test_")]


def main():
    dash = os.environ.get("MINIEDIT_DASH",
                           os.path.join(os.path.dirname(__file__),
                                        "..", "..", "src", "dash"))
    if not os.path.isfile(dash):
        print("error: dash binary not found at %s\n"
              "build it first, or set MINIEDIT_DASH" % dash,
              file=sys.stderr)
        return 1

    failed = []
    for fn in CASES:
        name = fn.__name__
        try:
            fn()
        except Exception as e:  # noqa: BLE001 - want to catch & report all
            failed.append(name)
            print("FAIL  %-45s %s: %s" % (name, type(e).__name__, e))
        else:
            print("ok    %s" % name)

    print()
    print("%d/%d passed" % (len(CASES) - len(failed), len(CASES)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
