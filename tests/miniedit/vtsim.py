"""
A small, deliberately partial VT100/ANSI terminal simulator.

This exists purely to let the miniedit test suite check what actually
ends up on screen (row contents and cursor position), rather than only
checking the raw escape-sequence bytes dash writes or the text of a
command's own output. Several real miniedit bugs (mid-line backspace
leaving stale characters, an off-by-one exactly at wrapped-row
boundaries, Enter clobbering rows below the cursor) were only visible
at this level - the raw byte stream and the final executed command
both looked fine while the actual screen was wrong.

Scope / limitations (please read before trusting this for anything
beyond miniedit's own test suite):

  - Only the handful of escape sequences miniedit.c itself emits are
    understood: CUU (\\x1b[<n>A - cursor up), CUD (\\x1b[<n>B - cursor
    down, stopping at the bottom row), CUF (\\x1b[<n>C - cursor right),
    CUB (\\x1b[<n>D - cursor left), CUP (\\x1b[<row>;<col>H), CHA
    (\\x1b[<n>G - cursor to column n), ED (\\x1b[J - erase from cursor
    to end of screen), EL (\\x1b[K - erase to end of line), plus plain
    \\r, \\n and BS (\\x08). Any other
    recognized-as-CSI sequence (e.g. dash's own "\\x1b[6n"
    cursor-position query) is consumed and ignored rather than painted
    as literal text, but its *effect* (if any) is not modeled.
  - Autowrap is modeled with the same "pending wrap" deferral a real
    terminal uses (writing the last column of a row does not move to
    the next row until another character actually follows), and
    height-bounded scrolling pushes the top row off the screen once
    content needs to go past the last one (a linefeed on the bottom
    row scrolls too) - both because miniedit had real bugs here.
    Rows that scroll off are appended to the `scrollback` list if one
    is passed in, standing in for a terminal's scrollback buffer.
  - No colour, no scroll regions, no wide/combining characters.

If you need to test something this simulator doesn't understand,
extend it deliberately rather than assuming it is correct.
"""

import re

_CSI_RE = re.compile(rb"\x1b\[([0-9;]*)([A-Za-z])")


def simulate(data, width, height=1000, scrollback=None):
    """Replay `data` (bytes written to the terminal) against a
    `width`-column, `height`-row screen and return (rows, row, col):
    rows is a list of strings (one per screen row, top to bound row),
    and (row, col) is the final cursor position, 0-based.  Rows that
    scrolled off the top are appended to `scrollback`, if given. `col` is
    reported as `width` while the cursor is in the "pending wrap"
    state, matching how a real terminal appears from the outside.
    """
    rows = [""]
    row, col = 0, 0
    pending_wrap = False

    def ensure_row(r):
        while len(rows) <= r:
            rows.append("")

    def scroll():
        ensure_row(height - 1)
        gone = rows.pop(0)
        if scrollback is not None:
            scrollback.append(gone)
        rows.append("")

    def putc(ch):
        nonlocal row, col, pending_wrap
        if pending_wrap:
            row += 1
            col = 0
            pending_wrap = False
            if row >= height:
                scroll()
                row = height - 1
        ensure_row(row)
        line = rows[row]
        if len(line) <= col:
            line = line + " " * (col - len(line)) + ch
        else:
            line = line[:col] + ch + line[col + 1:]
        rows[row] = line
        col += 1
        if col >= width:
            col = width - 1
            pending_wrap = True

    i = 0
    n = len(data)
    while i < n:
        m = _CSI_RE.match(data, i)
        if m:
            num_s, final = m.group(1), m.group(2)
            first = num_s.split(b";")[0]
            num = int(first) if first else 1
            if final == b"A":
                row = max(0, row - num)
                pending_wrap = False
            elif final == b"B":
                row = min(row + num, height - 1)
                ensure_row(row)
                pending_wrap = False
            elif final == b"H":
                r_s, _, c_s = num_s.partition(b";")
                row = min(int(r_s or 1), height) - 1
                col = min(int(c_s or 1), width) - 1
                ensure_row(row)
                pending_wrap = False
            elif final == b"C":
                col = min(col + num, width - 1)
                pending_wrap = False
            elif final == b"D":
                col = max(col - num, 0)
                pending_wrap = False
            elif final == b"G":
                col = num - 1
                pending_wrap = False
            elif final == b"J":
                ensure_row(row)
                rows[row] = rows[row][:col]
                for r in range(row + 1, len(rows)):
                    rows[r] = ""
            elif final == b"K":
                ensure_row(row)
                # from the cursor (the last column, if it is pending) to the end of the row
                rows[row] = rows[row][:(width - 1 if pending_wrap else col)]
            # anything else recognized as CSI: consumed, not modeled
            i = m.end()
            continue
        if data[i] == 0x07:  # BEL: audible only
            i += 1
            continue
        if data[i] == 0x08:  # BS: one column left, never past the first
            col = max(col - 1, 0)
            pending_wrap = False
            i += 1
            continue
        if data[i] == 0x0D:  # \r
            col = 0
            pending_wrap = False
            i += 1
            continue
        if data[i] == 0x0A:  # \n
            if row >= height - 1:
                scroll()
                row = height - 1
            else:
                row += 1
            ensure_row(row)
            i += 1
            continue
        putc(chr(data[i]))
        i += 1

    return rows, row, (width if pending_wrap else col)
