/*
 * miniedit: a tiny built-in line editor.  See miniedit.h for the
 * rationale and the (deliberately small) feature set.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include "shell.h"
#include "error.h"
#include "miniedit.h"
#include "var.h"

/* Number of bytes in the UTF-8 character starting with byte c. */
static int utf8_seqlen(unsigned char c)
{
	if ((c & 0x80) == 0x00)
		return 1;
	if ((c & 0xe0) == 0xc0)
		return 2;
	if ((c & 0xf0) == 0xe0)
		return 3;
	if ((c & 0xf8) == 0xf0)
		return 4;
	return 1;	/* stray continuation byte or invalid lead byte */
}

/* Byte offset of the character immediately before position pos. */
static int utf8_prev(const char *s, int pos)
{
	int i = pos - 1;

	while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80)
		i--;
	return i;
}

/* Number of UTF-8 characters in s[0..n). */
static int utf8_count(const char *s, int n)
{
	int i = 0, chars = 0;

	while (i < n) {
		i += utf8_seqlen((unsigned char)s[i]);
		chars++;
	}
	return chars;
}

/* Byte offset of the start of the previous word (a maximal run of
 * non-space bytes) to the left of pos: skip any spaces immediately
 * to the left, then skip the word itself. */
static int word_left(const char *s, int pos)
{
	int i = pos;

	while (i > 0 && s[i - 1] == ' ')
		i--;
	while (i > 0 && s[i - 1] != ' ')
		i--;
	return i;
}

/* Byte offset just past the end of the next word to the right of
 * pos: skip any spaces first, then the word itself.  If there is no
 * further word, this lands on len (the very end of the line) -
 * matching bash's forward-word behaviour. */
static int word_right(const char *s, int len, int pos)
{
	int i = pos;

	while (i < len && s[i] == ' ')
		i++;
	while (i < len && s[i] != ' ')
		i++;
	return i;
}

/* Read a single byte, retrying on EINTR unless a signal is waiting
 * to be handled (in which case the caller needs to bail out so the
 * pending interrupt can be processed by our caller). */
/* Input that was read ahead of the editor (see get_cursor) and must
 * still be seen by it, before anything more is read from the terminal. */
static char pend[32];
static int npend, ppos;

static void oflush(void);

static int read1(char *c)
{
	int n;

	if (ppos < npend) {
		*c = pend[ppos++];
		if (ppos == npend)
			npend = ppos = 0;
		return 1;
	}
	oflush();
	/* An interrupt may have arrived while we were not blocked in read()
	 * (e.g. during the cursor query); report it rather than block. */
	if (int_pending())
		return -1;
	do
		n = read(0, c, 1);
	while (n < 0 && errno == EINTR && !int_pending());
	return n;
}

/*
 * Output is collected here and sent with one write() per keystroke:
 * oflush() runs before every wait for input, before the cursor query,
 * and when the line is finished.  A write() per cursor move would be a
 * separate packet each over ssh.
 */
#define OBUF_MAX 256
static char obuf[OBUF_MAX];
static int olen;

static void oflush(void)
{
	int off = 0;

	while (off < olen) {
		int n = write(2, obuf + off, olen - off);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		off += n;
	}
	olen = 0;
}

static void __attribute__((noinline)) out(const char *s, int n)
{
	while (n > 0) {
		int k = OBUF_MAX - olen;

		if (k > n)
			k = n;
		if (k == 0) {
			oflush();
			continue;
		}
		memcpy(obuf + olen, s, k);
		olen += k;
		s += k;
		n -= k;
	}
}

static void outseq(const char *fmt, int n)
{
	char seq[24];
	int l;

	l = snprintf(seq, sizeof(seq), fmt, n);
	out(seq, l);
}

/* Terminal width in columns, for figuring out where long lines wrap.
 * Falls back to a conservative guess if we can't ask the tty. */
static int get_width(void)
{
	struct winsize ws;

	if (ioctl(2, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return ws.ws_col;
	return 80;
}

/* Ask the terminal where the cursor is right now (via a Device
 * Status Report).  Returns the column, and the row too if rowp is not
 * NULL (0 if unknown).
 *
 * Editing does not use this: where typing starts is worked out from the
 * prompt's text (see prompt_start).  Ctrl+L still does, because it has
 * to know which screen row the edit block is on, and that can only be
 * asked for.  It is only ever called on that explicit keypress.
 *
 * Only a real reply, ESC [ row ; col R, is taken.  Anything else that
 * arrives in the meantime is typed ahead by the user (or by whoever
 * drives this shell) and is put back for the editor to read, not thrown
 * away.  A terminal that never answers costs one short wait, after which
 * we stop asking. */
static int dsr_silent;		/* a query went unanswered: stop asking */

static int get_cursor(int *rowp)
{
	char got[32];
	int n = 0, ok = 0, row = 0, col = 1;

	if (rowp)
		*rowp = 0;
	if (dsr_silent || npend - ppos > 0)
		return col;
	out("\033[6n", 4);
	oflush();
	for (;;) {
		fd_set rfds;
		struct timeval tv;
		char c;

		FD_ZERO(&rfds);
		FD_SET(0, &rfds);
		tv.tv_sec = 0;
		tv.tv_usec = 200000;
		if (select(1, &rfds, NULL, NULL, &tv) <= 0)
			break;
		if (read(0, &c, 1) <= 0)
			break;
		if (n >= (int)sizeof(got))
			break;
		got[n++] = c;
		if (n == 1 && c == '\033')
			continue;
		if (n == 2 && c == '[')
			continue;
		if (n >= 3 && c == 'R') {
			ok = 1;
			break;
		}
		if (n >= 3 && (isdigit((unsigned char)c) || c == ';'))
			continue;
		break;
	}
	if (!n)
		dsr_silent = 1;
	if (ok) {
		char *p = strchr(got, ';');

		row = atoi(got + 2);
		col = p ? atoi(p + 1) : 1;
		if (rowp)
			*rowp = row;
		return col;
	}
	/* Not a reply: hand back what we read, in order. */
	if (n) {
		memcpy(pend, got, n);
		npend = n;
		ppos = 0;
	}
	return col;
}

/*
 * The prompt the parser last printed: it is in front of the line being
 * read, and its visible width tells us where the edit block begins,
 * without asking the terminal.  That is what keeps the editor from
 * ever sending a query.
 *
 * Only line wrapping needs the width to be right.  Everything else is
 * relative to where the cursor physically is (see refresh), so editing
 * on one row works wherever the prompt happens to start.  When a line
 * wraps after a prompt that did not start in column 1 (output without
 * a final newline, say), the wrap points are guessed, so a cursor move
 * across the first row's boundary can land a column off.  The command
 * itself is never affected.
 */
static char *cur_prompt;
static int cur_prompt_len, cur_prompt_cols;

/* The cursor, as of the last redraw (see refresh): at_off display
 * columns after the start of the edit block, and at_pend if it is in
 * the pending-wrap state. */
static int at_off, at_pend;

/* The text drawn after the block start, as of the last redraw. */
#define SHOWN_MAX (MINIEDIT_MAXLINE + 64)	/* bigger than any Ctrl+R display */
static char shown[SHOWN_MAX];
static int shown_len;

/* Columns taken by the last line of s.  Escape sequences (SGR colours,
 * say) take none, a newline starts a new line, a tab goes to the next
 * multiple of 8, and each UTF-8 character counts as one column, as
 * utf8_count does for the line itself. */
static int text_cols(const char *s)
{
	int col = 0;

	for (; *s; s++) {
		unsigned char c = *s;

		if (c == 0x1b) {	/* ESC: a CSI sequence, or a two-byte one */
			if (s[1] == '[') {
				s += 2;
				while (*s && !(*s >= 0x40 && *s <= 0x7e))
					s++;
				if (!*s)
					break;
			} else if (s[1]) {
				s++;
			}
		} else if (c == '\n' || c == '\r') {
			col = 0;
		} else if (c == '\t') {
			col = (col | 7) + 1;
		} else if (c >= 0x20 && c != 0x7f && (c & 0xc0) != 0x80) {
			col++;
		}
	}
	return col;
}

void miniedit_prompt_shown(const char *s)
{
	size_t n = strlen(s);
	char *p = malloc(n + 1);

	free(cur_prompt);
	cur_prompt = NULL;
	cur_prompt_len = cur_prompt_cols = 0;
	if (!p)
		return;		/* no memory: editing falls back to column 1 */
	memcpy(p, s, n + 1);
	cur_prompt = p;
	cur_prompt_len = n;
	cur_prompt_cols = text_cols(s);
}

/* Set up where typing starts, just after the current prompt, on a
 * terminal width columns wide: *start_col is the 1-based column and
 * *row is 0.  A prompt that fills its last row exactly leaves the cursor
 * in the pending-wrap state (see park_end), so there the edit moves on
 * to a fresh row first, rather than overwriting the prompt's last
 * character. */
static void edit_start(int width, int *start_col, int *row)
{
	int c = cur_prompt_cols;

	if (c > 0 && c % width == 0)
		out("\r\n", 2);
	*start_col = c % width + 1;
	*row = 0;
	at_off = 0;
	at_pend = 0;
	shown_len = 0;
}

/* Insert n bytes at the cursor; 0 if the line has no room for them. */
static int linsert(char *line, int *len, int *cur, const char *s, int n)
{
	if (*len + n > MINIEDIT_MAXLINE - 1)
		return 0;
	memmove(line + *cur + n, line + *cur, *len - *cur);
	memcpy(line + *cur, s, n);
	*len += n;
	*cur += n;
	return 1;
}

/*
 * Cursor motion within the block: dir is 'A' (up), 'B' (down), 'C'
 * (right) or 'D' (left), n steps.  A few steps left are backspaces: one
 * byte each, and they never wrap, since a move never goes past the first
 * column of its row.
 */
static void cmove(int dir, int n)
{
	char seq[16];
	int l;

	if (n <= 0)
		return;
	if (dir == 'D' && n <= 4) {
		while (n-- > 0)
			out("\b", 1);
		return;
	}
	if (dir == 'C' && n == 1) {
		out("\033[C", 3);
		return;
	}
	l = n > 1 ? snprintf(seq, sizeof(seq), "\033[%d%c", n, dir)
		  : snprintf(seq, sizeof(seq), "\033[%c", dir);
	out(seq, l);
}

/*
 * The redraw, as bash does it: `shown` holds the text last drawn after
 * the block start, and each refresh only writes from the first byte
 * that differs from it.  Typing at the end sends just the new bytes;
 * moving the cursor sends just the moves; an edit in the middle sends
 * the rest of the line once, then moves back.
 *
 * Positions are display columns counted from the start of the block;
 * base is the block's start column less one, so base + off is where a
 * position falls in the rows of the terminal (a row being width columns).
 *
 * The cursor is at_off plus at_pend.  at_pend is the terminal's
 * pending-wrap state: after the last character of a full row is
 * written, the cursor stays on that row's last column and the next
 * character wraps to the following row.  Any cursor movement clears
 * it, so a move from there must emit something (see moveto).
 *
 * All moves are relative to where the cursor physically is, never to
 * an absolute column, so the block may begin part-way along its row
 * (after output that had no final newline).
 */
/* Row and column (0-based, relative to the block's first row) of the
 * cursor at display column off; pend says it is in the pending state. */
static void cursor_pos(int width, int base, int off, int pend,
		       int *r, int *c)
{
	int a = base + off;

	if (pend) {
		*r = a / width - 1;
		*c = width - 1;
	} else {
		*r = a / width;
		*c = a % width;
	}
}

/* Move the cursor to display column off, out of the pending state. */
static void moveto(int width, int base, int off)
{
	int r0, c0, r1, c1, moved = 0;

	cursor_pos(width, base, at_off, at_pend, &r0, &c0);
	cursor_pos(width, base, off, 0, &r1, &c1);
	if (r1 < r0) {
		cmove('A', r0 - r1);
		moved = 1;
	} else if (r1 > r0) {
		cmove('B', r1 - r0);
		moved = 1;
	}
	if (c1 != c0) {
		cmove(c1 > c0 ? 'C' : 'D', c1 > c0 ? c1 - c0 : c0 - c1);
		moved = 1;
	}
	/* Leaving the pending state takes a movement; with none needed,
	 * step left and back, which ends where it started. */
	if (at_pend && !moved)
		out("\b\033[C", 4);
	at_off = off;
	at_pend = 0;
}

/*
 * Put the cursor at the end of the line, in the state the terminal
 * leaves it in after writing the last character.  When the line fills
 * its row exactly, that is the pending state, which a plain move cannot
 * reach (the next row may not exist yet): so step onto the last
 * character and write it again, which changes nothing on screen.
 */
static void park_end(int width, int base, const char *line, int len,
		     int total)
{
	if (base + total > 0 && (base + total) % width == 0) {
		int ls = len - 1;

		if (at_off == total && at_pend)
			return;
		while (ls > 0 && ((unsigned char)line[ls] & 0xc0) == 0x80)
			ls--;
		moveto(width, base, total - 1);
		out(line + ls, len - ls);
		at_off = total;
		at_pend = 1;
	} else {
		moveto(width, base, total);
	}
}

static void refresh(int width, int start_col, int *row,
		     const char *line, int len, int cur)
{
	int base = start_col - 1;	/* block start, on its first row */
	int p, off_p, total, erase, r, c;

	/* The first byte that differs from the screen, on a character
	 * boundary of the new line. */
	for (p = 0; p < len && p < shown_len && line[p] == shown[p]; p++)
		;
	while (p > 0 && p < len && ((unsigned char)line[p] & 0xc0) == 0x80)
		p--;

	erase = shown_len > len;
	total = utf8_count(line, len);
	if (p == len && erase && shown_len - len == 1 && at_off == utf8_count(line, len) + 1
	    && !at_pend && (base + at_off) % width != 0) {
		/* Backspace at the end of the line, cursor just after the
		 * deleted character and not at a row's first column: backspace
		 * onto it, blank it, and step back.  Bash sends the same. */
		out("\b \b", 3);
		at_off = total;
		shown_len = len;
		memcpy(shown, line, len);
		goto placed;
	}
	if (p < len || erase) {
		off_p = utf8_count(line, p);
		/* Writing can start right where a pending cursor is, since
		 * the terminal wraps before the next character; an erase
		 * cannot, it would take the last character with it. */
		if (!(at_pend && at_off == off_p && !erase))
			moveto(width, base, off_p);
		if (erase) {
			/* Erase to the end of this row if the old text ends on
			 * it; otherwise everything below too. */
			int oldtot = utf8_count(shown, shown_len);
			int oldrow = base + oldtot > 0 ? (base + oldtot - 1) / width : 0;

			out(oldrow > (base + off_p) / width ? "\033[J" : "\033[K", 3);
		}
		out(line + p, len - p);
		at_off = total;
		/* Pending only if something was written up to the end: a bare
		 * move (say, after a Ctrl+W that shortened the line) leaves the
		 * cursor where moveto put it. */
		at_pend = p < len && base + total > 0 && (base + total) % width == 0;
		memcpy(shown, line, len);
		shown_len = len;
	}

placed:
	if (cur == len)
		park_end(width, base, line, len, total);
	else
		moveto(width, base, utf8_count(line, cur));

	cursor_pos(width, base, at_off, at_pend, &r, &c);
	*row = r;
}

/*
 * History: kept entirely on disk in ~/.bash_history, one command per
 * line.  Submitting a line is a plain open+write+close, no state
 * kept around.  Up/Down instead mmap the file read-only and walk it
 * in place - no line array, no heap allocation, just a pointer and
 * a length for whichever entry is currently shown.
 */

static int hist_path(char *buf, int n)
{
	const char *home = getenv("HOME");

	if (!home || !*home)
		return 0;
	return snprintf(buf, n, "%s/.bash_history", home) < n;
}

/*
 * History folding.  A compound command typed over several lines
 * (if/then/else/fi, loops, and so on) is stored as one history entry
 * instead of one entry per line, as bash does by default:
 *
 *	if true; then
 *	echo foo
 *	else
 *	echo bar
 *	fi
 *
 * becomes the single entry
 *
 *	if true; then echo foo; else echo bar; fi
 *
 * The physical lines are kept in memory as they are typed (hist_append)
 * and nothing touches the history file until the parser reports that a
 * complete command has been read: parsecmd() calls miniedit_cmd_begin()
 * when a command starts and miniedit_cmd_done() when it is finished.
 * At that point the command is written in one O_APPEND write, folded if
 * that is safe, otherwise as typed.  So another session's entries can
 * never land between the lines of a compound command.
 */

/* The physical lines of the command being read, exactly as typed. */
static char *fold_raw;
static size_t fold_rawlen, fold_rawcap;
static int fold_lines;

static void hist_append(const char *s, int n)
{
	if (fold_rawlen + n > fold_rawcap) {
		size_t cap = fold_rawcap ? fold_rawcap : 1024;
		char *p;

		while (cap < fold_rawlen + n)
			cap *= 2;
		p = realloc(fold_raw, cap);
		if (!p)
			return;		/* out of memory: drop the line */
		fold_raw = p;
		fold_rawcap = cap;
	}
	memcpy(fold_raw + fold_rawlen, s, n);
	fold_rawlen += n;
	fold_lines++;
}

/* Append s[0..n) to the history file in a single write.  O_APPEND
 * makes the write land at the current end of file as one block. */
static void hist_write(const char *s, int n)
{
	char path[256];
	int fd;

	if (!hist_path(path, sizeof(path)))
		return;
	fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
	if (fd < 0)
		return;
	while (n > 0) {
		int w = write(fd, s, n);

		if (w <= 0)
			break;
		s += w;
		n -= w;
	}
	close(fd);
}

/* True if the text o[0..olen) is a command whose newline may be
 * replaced by a plain space, because the command is still open. */
static const char *const fold_open_kw[] = {
	"then", "else", "elif", "if", "do", "while", "until", "!", "{", NULL
};

static int fold_is_open_kw(const char *w, int wl)
{
	int i;

	for (i = 0; fold_open_kw[i]; i++)
		if (strlen(fold_open_kw[i]) == (size_t)wl &&
		    !memcmp(fold_open_kw[i], w, wl))
			return 1;
	return 0;
}

/* Decide what a newline after o[0..olen) turns into: nonzero means
 * "; " (the command ends here), zero means " " (it goes on). */
static int fold_needs_semi(const char *o, int olen)
{
	int e = olen, s, ps, pe, ps2, cmdpos;

	if (!olen)
		return 0;
	switch (o[olen - 1]) {
	case ';': case '&': case '|': case '(':
		return 0;
	case ')':
		/* "name()" heads a function definition, whose body may
		 * follow on the next line: "f() {" is fine, "f(); {" is
		 * not.  An empty "$()" is not a function head. */
		if (olen >= 2 && o[olen - 2] == '(' &&
		    !(olen >= 3 && o[olen - 3] == '$'))
			return 0;
		break;
	}
	/* last word is o[s..e) */
	for (s = e; s > 0 && o[s - 1] != ' '; s--)
		;
	/* text before it, with trailing blanks dropped, is o[0..ps) */
	for (ps = s; ps > 0 && o[ps - 1] == ' '; ps--)
		;
	if (ps == 0)
		cmdpos = 1;
	else if (strchr(";&|(", o[ps - 1]))
		cmdpos = 1;
	else {
		/* previous word o[ps2..pe) */
		pe = ps;
		for (ps2 = pe; ps2 > 0 && o[ps2 - 1] != ' '; ps2--)
			;
		/* a function head "f()" is followed by its body */
		cmdpos = fold_is_open_kw(o + ps2, pe - ps2) ||
			 (pe > ps2 && o[pe - 1] == ')');
	}
	if (cmdpos && fold_is_open_kw(o + s, e - s))
		return 0;
	return 1;
}

/* Can this single line take part in folding?  Rejects anything whose
 * meaning depends on where the line ends. */
static int fold_line_ok(const char *s, int n)
{
	int i, q = 0, depth = 0;

	for (i = 0; i < n; i++) {
		char c = s[i];

		if (c == '`')
			return 0;
		if (c == '\\' && q != '\'') {
			if (i + 1 >= n)
				return 0;	/* line continuation */
			i++;
			continue;
		}
		if (q) {
			if (c == q)
				q = 0;
			continue;
		}
		if (c == '\'' || c == '"') {
			q = c;
			continue;
		}
		if (c == '#' || (c == '<' && i + 1 < n && s[i + 1] == '<'))
			return 0;	/* comment, or heredoc */
		if (c == '(')
			depth++;
		else if (c == ')' && --depth < 0)
			return 0;
	}
	if (q || depth)
		return 0;
	return 1;
}

/* Fold the raw lines in in[0..n) into out[], which must hold at least
 * 2*n+2 bytes.  Returns the length (the result is '\n'-terminated), 0 if
 * there is nothing to write, or -1 if the command should be left as typed. */
static int fold_text(const char *in, int n, char *out)
{
	int olen = 0, i = 0;

	if (memmem(in, n, "case", 4) || memmem(in, n, "function", 8))
		return -1;
	while (i < n) {
		int ls = i, le, a, b;

		for (le = ls; le < n && in[le] != '\n'; le++)
			;
		i = le + 1;
		for (a = ls; a < le && (in[a] == ' ' || in[a] == '\t'); a++)
			;
		for (b = le; b > a && (in[b - 1] == ' ' || in[b - 1] == '\t'); b--)
			;
		if (a == b)
			continue;
		/* words are split on blanks only, so leave tabs alone */
		if (memchr(in + a, '\t', b - a) || !fold_line_ok(in + a, b - a))
			return -1;
		if (olen) {
			if (fold_needs_semi(out, olen))
				out[olen++] = ';';
			out[olen++] = ' ';
		}
		memcpy(out + olen, in + a, b - a);
		olen += b - a;
	}
	if (!olen)
		return 0;
	out[olen++] = '\n';
	return olen;
}

/*
 * Keeping commands out of the history, as bash does with
 * HISTCONTROL=ignoreboth:
 *
 *   - a command whose first character is a space is not saved;
 *   - a command whose text exactly matches a line already in the
 *     history file is not saved again.
 *
 * Both are on by default.  --disable-hist-ignorespace and
 * --disable-hist-ignoredups (configure) turn them off.  The fallbacks
 * below only matter when building without configure.
 */
#ifndef HIST_IGNORESPACE
#define HIST_IGNORESPACE 1
#endif
#ifndef HIST_IGNOREDUPS
#define HIST_IGNOREDUPS 1
#endif

static int hist_open(const char **map, size_t *len);

/* True if the history file already has a line whose text is exactly
 * e[0..elen), with or without a final '\n'.  Only whole lines count:
 * a command is never found inside a longer one. */
static int hist_has(const char *e, int elen)
{
	const char *map, *p, *end;
	size_t maplen;
	int found = 0;

	if (elen > 0 && e[elen - 1] == '\n')
		elen--;
	if (elen <= 0 || !hist_open(&map, &maplen))
		return 0;
	end = map + maplen;
	for (p = map; (p = memmem(p, end - p, e, elen)) != NULL; p++) {
		if ((p == map || p[-1] == '\n') &&
		    (p + elen == end || p[elen] == '\n')) {
			found = 1;
			break;
		}
	}
	munmap((void *)map, maplen);
	return found;
}

/* Should this command be left out of the history?  raw is the command
 * as typed; entry[0..elen) is the text that would be written for it. */
static int hist_skip(const char *raw, int rawlen, const char *entry, int elen)
{
#if HIST_IGNORESPACE
	if (rawlen > 0 && raw[0] == ' ')
		return 1;
#endif
#if HIST_IGNOREDUPS
	if (hist_has(entry, elen))
		return 1;
#endif
	return 0;
}

void miniedit_cmd_begin(void)
{
	fold_rawlen = 0;
	fold_lines = 0;
}

void miniedit_cmd_done(int complete)
{
	char *out = NULL;
	int olen = 0;
	const char *entry = fold_raw;
	int elen = fold_rawlen;

	if (complete && fold_lines > 1 && (out = malloc(2 * fold_rawlen + 2)))
		olen = fold_text(fold_raw, fold_rawlen, out);
	if (olen > 0) {
		entry = out;
		elen = olen;
	}
	if (fold_lines && !hist_skip(fold_raw, fold_rawlen, entry, elen))
		hist_write(entry, elen);
	free(out);
	miniedit_cmd_begin();
}

/* mmap the history file read-only.  Returns 0 (leaving *map alone)
 * if there is no usable history to browse. */
static int hist_open(const char **map, size_t *len)
{
	char path[256];
	struct stat st;
	int fd;

	if (!hist_path(path, sizeof(path)))
		return 0;
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	if (fstat(fd, &st) < 0 || st.st_size == 0) {
		close(fd);
		return 0;
	}
	*map = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd);			/* the mapping outlives the fd */
	if (*map == MAP_FAILED) {
		*map = NULL;
		return 0;
	}
	*len = st.st_size;
	return 1;
}

/* Step *cur (and *cur_len) to the history entry immediately before
 * (up = 1) or after (up = 0) the one currently shown; *cur == NULL
 * means "not currently showing any entry".  Lines are '\n'-
 * terminated.  Returns 0 if there is nowhere further to go. */
static int hist_step(const char *map, size_t maplen, int up,
		      const char **cur, int *cur_len)
{
	size_t p;

	if (up) {
		p = *cur ? (size_t)(*cur - map) : maplen;
		if (!p)
			return 0;
		p--;
		if (p > 0 && map[p] == '\n')
			p--;
		while (p > 0 && map[p - 1] != '\n')
			p--;
		*cur = map + p;
	} else {
		if (!*cur)
			return 0;
		p = (*cur - map) + *cur_len;
		if (p < maplen && map[p] == '\n')
			p++;
		if (p >= maplen)
			return 0;
		*cur = map + p;
	}
	p = *cur - map;
	while (p < maplen && map[p] != '\n')
		p++;
	*cur_len = p - (*cur - map);
	if (*cur_len > MINIEDIT_MAXLINE - 1)
		*cur_len = MINIEDIT_MAXLINE - 1;	/* just clamp */
	return 1;
}

/*
 * File name completion, on Tab.  Only names in a directory are completed
 * (not commands or variables), for the word before the cursor: the text
 * back to the last unescaped blank or one of ;|&()<>.  ~/ is understood;
 * other quoting is not.  Names go into the line with backslashes where
 * the shell needs them.  Like bash: a unique match is completed (plus a
 * space, or a / for a directory), several are completed as far as they
 * agree, and if that gets nowhere the next Tab lists them.
 */

static int namecmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static int isdir(const char *dir, const char *name)
{
	char path[MINIEDIT_MAXLINE + 64 + 256];
	struct stat st;

	snprintf(path, sizeof(path), "%s%s", dir, name);
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Show a list of names in columns, and redraw the prompt and line below. */
static void listnames(char **v, int n, const char *dir, int width,
		      char *line, int *len, int *cur, int *start_col, int *row)
{
	char buf[2 * 256 + 4];
	int i, l, mx = 0, cw, nc, nr;

	refresh(width, *start_col, row, line, *len, *len);
	out("\r\n", 2);
	if (n > 100) {
		char k = 0;

		l = snprintf(buf, sizeof(buf),
			     "Display all %d possibilities? (y or n)", n);
		out(buf, l);
		read1(&k);
		out("\r\n", 2);
		if (k != 'y' && k != ' ')
			n = 0;
	}
	qsort(v, n, sizeof(*v), namecmp);
	for (i = 0; i < n; i++) {
		if (isdir(dir, v[i]))
			strcat(v[i], "/");
		l = strlen(v[i]);
		mx = l > mx ? l : mx;
	}
	cw = mx + 2;
	nc = width / cw ? width / cw : 1;	/* columns, then rows */
	nr = (n + nc - 1) / nc;
	for (i = 0; i < nr; i++) {
		int j;

		for (j = i; j < n; j += nr)
			out(buf, snprintf(buf, sizeof(buf), "%-*s", cw, v[j]));
		out("\r\n", 2);
	}
	/* Print the prompt for this line again, exactly as it was
	 * expanded, and put the cursor back after it. */
	out(cur_prompt ? cur_prompt : "", cur_prompt ? cur_prompt_len : 0);
	edit_start(width, start_col, row);
}

/* Returns non-zero if the next Tab should list the possibilities. */
static int tabcomplete(char *line, int *len, int *cur, int again, int width,
		       int *start_col, int *row)
{
	char pre[MINIEDIT_MAXLINE], dir[MINIEDIT_MAXLINE + 64];
	char ins[2 * 256 + 2], **v = NULL;
	char *base, *p, *home;
	struct dirent *e;
	DIR *d;
	int i, j, n = 0, w = 0, dl, bl, cl;

	for (i = 0; i < *cur; i++) {		/* find the word... */
		if (line[i] == '\\')
			i++;
		else if (strchr(" \t;|&()<>", line[i]))
			w = i + 1;
	}
	for (i = w, p = pre; i < *cur; i++) {	/* ...and take the \s out */
		if (line[i] == '\\')
			i++;
		if (i < *cur)
			*p++ = line[i];
	}
	*p = '\0';
	base = strrchr(pre, '/');
	base = base ? base + 1 : pre;
	dl = base - pre;
	bl = strlen(base);

	if (!dl)
		strcpy(dir, "./");
	else if (pre[0] == '~' && pre[1] == '/' && (home = lookupvar("HOME")))
		snprintf(dir, sizeof(dir), "%s/%.*s", home, dl - 2, pre + 2);
	else {
		memcpy(dir, pre, dl);
		dir[dl] = '\0';
	}

	if ((d = opendir(dir))) {
		while ((e = readdir(d))) {
			char *nm = e->d_name, *s, **t;

			if (strncmp(nm, base, bl) || (base[0] != '.' &&
			    nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2]))))
				continue;
			if (!(s = malloc(strlen(nm) + 2)))	/* room for a / */
				break;
			strcpy(s, nm);
			if (!(n & 15) && !(t = realloc(v, (n + 16) * sizeof(*v)))) {
				free(s);
				break;
			}
			if (!(n & 15))
				v = t;
			v[n++] = s;
		}
		closedir(d);
	}
	if (!n) {
		out("\a", 1);
		return 0;
	}

	for (cl = strlen(v[0]), i = 1; i < n; i++) {	/* what they share */
		for (j = 0; j < cl && v[i][j] == v[0][j]; j++)
			;
		cl = j;
	}
	if (n > 1)
		out("\a", 1);
	if (n == 1 || cl > bl) {
		for (p = ins, i = bl; i < cl; i++) {
			if (strchr(" \t\n\\\"'<>;|&()#$`?*[{", v[0][i]))
				*p++ = '\\';
			*p++ = v[0][i];
		}
		if (n == 1) {			/* unless it is already there */
			char sc = isdir(dir, v[0]) ? '/' : ' ';

			if (*cur >= *len || line[*cur] != sc)
				*p++ = sc;
		}
		linsert(line, len, cur, ins, p - ins);
		again = 0;
	} else if (again) {
		listnames(v, n, dir, width, line, len, cur, start_col, row);
		again = 0;
	} else
		again = 1;
	for (i = 0; i < n; i++)
		free(v[i]);
	free(v);
	return again;
}

const char *miniedit_gets(int *lenp)
{
	static char line[MINIEDIT_MAXLINE];
	struct termios save, raw;
	int len = 0, cur = 0;
	int eof = 0;
	int width, start_col, row = 0;
	int tab = 0;		/* the last key was a Tab that got nowhere */
	const char *hmap = NULL;
	size_t hmaplen = 0;
	const char *hcur = NULL;
	int hcur_len = 0;

	if (tcgetattr(0, &save) < 0) {
		*lenp = 0;
		return NULL;
	}
	raw = save;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &raw);

	width = get_width();
	edit_start(width, &start_col, &row);

	for (;;) {
		char c;
		int n;

		n = read1(&c);
		if (n <= 0) {
			/* EOF, read error, or an interrupt is pending:
			 * hand back whatever we have so far. */
			if (n < 0) {
				if (cur != len)
					refresh(width, start_col, &row, line,
						len, len);
				out("^C\r\n", 4);
				len = 0;
			}
			eof = (len == 0);
			break;
		}

		if (c == '\t') {			/* Tab */
			tab = tabcomplete(line, &len, &cur, tab, width,
					  &start_col, &row);
			refresh(width, start_col, &row, line, len, cur);
			continue;
		}
		tab = 0;

do_enter:
		if (c == '\r' || c == '\n') {
			int orig_len = len;

			/* However many rows the line wraps across, land on
			 * the true end first - otherwise "\r\n" only drops
			 * one row below wherever the cursor happened to be
			 * mid-edit, leaving stale rows of the old content
			 * below it. */
			if (cur != len)
				refresh(width, start_col, &row, line, len,
					len);
			if (len < MINIEDIT_MAXLINE - 1)
				line[len++] = '\n';
			if (orig_len)
				hist_append(line, len);
			out("\r\n", 2);
			break;
		}

		if (c == 0x0c) {		/* Ctrl-L */
			int r, n, col;

			/* "Clear" the screen by scrolling it up (so it all
			 * stays in the scrollback) until the first row of the
			 * edit block is at the top. */
			col = get_cursor(&r);
			n = r - row - 1;	/* rows above the block */
			if (r && n > 0) {
				out("\033[999B", 6);	/* to the bottom row */
				for (; n > 0; n -= 8)
					out("\n\n\n\n\n\n\n\n", n < 8 ? n : 8);
				outseq("\033[%d;1H", row + 1);
					outseq("\033[%dG", col);
				if (at_pend) {
					at_off--;	/* same column, not pending */
					at_pend = 0;
				}
			}
			refresh(width, start_col, &row, line, len, cur);
			continue;
		}

		if (c == 4) {			/* Ctrl-D */
			if (len && cur != len)
				refresh(width, start_col, &row, line, len,
					len);
			eof = (len == 0);
			break;
		}

		if (c == 0x12) {	/* Ctrl-R: minimal incremental search */
			char pat[64];
			int patlen = 0;
			const char *scur = NULL;
			int scur_len = 0;
			int matchoff = 0;
			int exit_cur = -1;	/* <0: no explicit request */
			int acted = 0;		/* has the user done anything
						 * yet, or is the pattern (and
						 * thus any match) still just
						 * the initial blank state? */
			char term = 0;

			if (!hmap && !hist_open(&hmap, &hmaplen))
				continue;

			for (;;) {
				char disp[MINIEDIT_MAXLINE + 40];
				int dlen, dcur, plen;
				const char *try_ = scur;
				int try_len = scur_len;
				int n;

				if (acted) {
					while (hist_step(hmap, hmaplen, 1,
							  &try_, &try_len)) {
						const char *m = patlen ?
							memmem(try_, try_len,
							       pat, patlen) :
							try_;

						if (m) {
							scur = try_;
							scur_len = try_len;
							matchoff = m - try_;
							break;
						}
					}
				}

				plen = snprintf(disp, sizeof(disp),
						 "(reverse-i-search)`%.*s': ",
						 patlen, pat);
				if (plen < 0)
					plen = 0;
				if (plen > (int)sizeof(disp) - 1)
					plen = sizeof(disp) - 1;
				dlen = plen;
				if (scur) {
					int m = scur_len;

					if (dlen + m > (int)sizeof(disp) - 1)
						m = sizeof(disp) - 1 - dlen;
					memcpy(disp + dlen, scur, m);
					dlen += m;
					dcur = plen + matchoff;
					if (dcur > dlen)
						dcur = dlen;
				} else {
					dcur = dlen;
				}
				/* Keep the real prompt where it is and
				 * show the search status right after it,
				 * exactly like ordinary editing - so
				 * leaving search mode is just another
				 * ordinary redraw, nothing to restore. */
				refresh(width, start_col, &row, disp, dlen,
					dcur);

				n = read1(&term);
				if (n <= 0) {
					if (n < 0) {
						if (dcur != dlen)
							refresh(width,
								start_col,
								&row, disp,
								dlen, dlen);
						out("^C\r\n", 4);
						len = 0;
					}
					eof = (len == 0);
					goto finish;
				}

				if (term == 0x12) {
					acted = 1;
					continue;	/* older, same pattern */
				} else if (term == 0x7f || term == 0x08) {
					if (patlen)
						patlen--;
				} else if (term == 0x1b) {
					char c2, c3;
					int dir = 0;

					if (read1(&c2) > 0 && c2 == '[' &&
					    read1(&c3) > 0) {
						if (c3 == 'D')
							dir = -1;
						else if (c3 == 'C')
							dir = 1;
					}
					if (dir && scur)
						exit_cur = matchoff + dir;
					break;
				} else if ((unsigned char)term >= 0x20 &&
					   patlen < (int)sizeof(pat)) {
					pat[patlen++] = term;
				} else {
					break;	/* anything else ends it */
				}
				acted = 1;
				scur = NULL;	/* restart from newest */
				scur_len = 0;
			}

			hcur = scur;
			hcur_len = scur_len;
			if (scur) {
				len = scur_len > MINIEDIT_MAXLINE - 1 ?
				      MINIEDIT_MAXLINE - 1 : scur_len;
				memcpy(line, scur, len);
				cur = (exit_cur >= 0 && exit_cur <= len) ?
				      exit_cur : len;
			} else {
				len = cur = 0;
			}
			refresh(width, start_col, &row, line, len, cur);

			if (term == '\r' || term == '\n') {
				c = term;
				goto do_enter;
			}
			continue;
		}

		if (c == 0x7f || c == 0x08 || c == 0x17) {	/* BS, ^W */
			int start, n_del, tail;

			if (!cur)
				continue;
			start = c == 0x17 ? word_left(line, cur) :
					    utf8_prev(line, cur);
			n_del = cur - start;
			tail = len - cur;

			memmove(line + start, line + cur, tail);
			len -= n_del;
			cur = start;
			refresh(width, start_col, &row, line, len, cur);
			continue;
		}

		if (c == 0x1b) {		/* Escape sequence */
			char c2, c3;
			char params[8];
			int np = 0;
			int ctrl;

			if (read1(&c2) <= 0)
				continue;
			if (c2 != '[' && c2 != 'O')
				continue;

			if (read1(&c3) <= 0)
				continue;
			while ((c3 >= '0' && c3 <= '9') || c3 == ';') {
				if (np < (int)sizeof(params) - 1)
					params[np++] = c3;
				if (read1(&c3) <= 0)
					goto esc_done;
			}
			params[np] = '\0';

			/* xterm reports Ctrl as modifier "5", either
			 * alone ("\e[5C", older form) or as a second
			 * parameter ("\e[1;5C", current form). */
			ctrl = !strcmp(params, "5") ||
			       (np >= 2 && !strcmp(params + np - 2, ";5"));

			if (c3 == '~') {
				if (params[0] == '1' || params[0] == '7')
					c3 = 'H';
				else if (params[0] == '4' ||
					 params[0] == '8')
					c3 = 'F';
				else if (params[0] == '3')
					c3 = 'X'; /* our sentinel: Delete */
				else
					c3 = 0;
			}

			switch (c3) {
			case 'C':	/* Right / Ctrl-Right */
				cur = ctrl ? word_right(line, len, cur) :
				      cur < len ?
				      cur + utf8_seqlen(
						(unsigned char)line[cur]) :
				      cur;
				break;
			case 'D':	/* Left / Ctrl-Left */
				cur = ctrl ? word_left(line, cur) :
				      cur > 0 ? utf8_prev(line, cur) : cur;
				break;
			case 'H':	/* Home */
				cur = 0;
				break;
			case 'F':	/* End */
				cur = len;
				break;
			case 'A':	/* Up: previous history entry */
				if (!hmap && !hist_open(&hmap, &hmaplen))
					continue;
				if (!hist_step(hmap, hmaplen, 1, &hcur,
					       &hcur_len))
					continue;
				memcpy(line, hcur, hcur_len);
				len = cur = hcur_len;
				break;
			case 'B':	/* Down: next entry, or back to empty */
				if (!hcur)
					continue;
				if (hist_step(hmap, hmaplen, 0, &hcur,
					      &hcur_len)) {
					memcpy(line, hcur, hcur_len);
					len = cur = hcur_len;
				} else {
					hcur = NULL;
					len = cur = 0;
				}
				break;
			case 'X': {	/* Delete: remove char under cursor */
				int n_del, tail;

				if (cur == len)
					continue;
				n_del = utf8_seqlen(
					(unsigned char)line[cur]);
				tail = len - cur - n_del;
				memmove(line + cur, line + cur + n_del,
					tail);
				len -= n_del;
				break;
			}
			default:
				continue;
			}
			refresh(width, start_col, &row, line, len, cur);
esc_done:
			continue;
		}

		if ((unsigned char)c < 0x20)
			continue;	/* other control characters: ignore */

		{
			char chr[4];
			int nb, i;

			chr[0] = c;
			nb = utf8_seqlen((unsigned char)c);
			for (i = 1; i < nb; i++) {
				char cc;

				if (read1(&cc) <= 0)
					break;
				chr[i] = cc;
			}
			nb = i;

			if (!linsert(line, &len, &cur, chr, nb))
				continue;	/* line full: drop input */
			refresh(width, start_col, &row, line, len, cur);
		}
	}

finish:
	oflush();
	if (hmap)
		munmap((void *)hmap, hmaplen);
	tcsetattr(0, TCSANOW, &save);

	/* The prompt was for this line; the parser prints the next one. */
	free(cur_prompt);
	cur_prompt = NULL;
	cur_prompt_len = cur_prompt_cols = 0;

	if (eof) {
		*lenp = 0;
		return NULL;
	}
	*lenp = len;
	return line;
}
