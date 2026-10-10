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
#include "memalloc.h"
#include "miniedit.h"
#include "parser.h"
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

static int read1(char *c)
{
	int n;

	if (ppos < npend) {
		*c = pend[ppos++];
		if (ppos == npend)
			npend = ppos = 0;
		return 1;
	}
	do
		n = read(0, c, 1);
	while (n < 0 && errno == EINTR && !int_pending());
	return n;
}

static void out(const char *s, int n)
{
	write(2, s, n);
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
 * Status Report) so we know which column editing starts at, without
 * having to work out the display width of the shell's prompt
 * ourselves.  Returns the column, and the row too if rowp is not
 * NULL (0 if unknown).
 *
 * Only a real reply, ESC [ row ; col R, is taken.  Anything else that
 * arrives in the meantime is typed ahead by the user (or by whoever
 * drives this shell, e.g. a file manager sending its first command) and
 * is put back for the editor to read, not thrown away.  A terminal that
 * never answers costs one short wait, after which we stop asking and
 * fall back to column 1: the display just won't wrap perfectly then. */
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
 * Redraw the whole line from scratch and reposition the cursor.
 * This is simpler and far more robust than patching up the screen
 * incrementally, especially once a line is long enough to wrap onto
 * further terminal rows: we let the terminal's own line wrap do the
 * work, and only have to reason about how many on-screen rows the
 * text before/up to the cursor occupies.
 *
 * *row tracks which wrapped row the cursor was left on by the
 * previous refresh, so we know how far up to move before redrawing.
 */
/*
 * Row the cursor physically rests on right after writing `offset`
 * display columns' worth of characters starting at column 1.
 *
 * When offset lands exactly on a row boundary, a real terminal does
 * NOT advance to the next row until another character is actually
 * written - it stays "pending wrap" at the end of the row it just
 * filled.  Plain offset/width gets this one row too high exactly at
 * multiples of width, which is what caused the display to come
 * apart whenever a line's length landed exactly on a wrapped row
 * boundary (an off-by-one, not a wrapping/scrolling design gap).
 */
static int phys_row(int offset, int width)
{
	if (offset > 0 && offset % width == 0)
		return offset / width - 1;
	return offset / width;
}

static void refresh(int width, int start_col, int *row,
		     const char *line, int len, int cur)
{
	int end, end_row;

	if (*row > 0)
		outseq("\033[%dA", *row);
	outseq("\033[%dG", start_col);
	out("\033[J", 3);

	out(line, len);

	end = start_col - 1 + utf8_count(line, len);
	end_row = phys_row(end, width);

	if (cur == len) {
		/* The cursor is already exactly where it belongs: right
		 * after the last character we just wrote, pending-wrap
		 * state and all - nothing left to do. */
		*row = end_row;
		return;
	}

	/* Repositioning onto an existing character (cur < len) is
	 * unambiguous: unlike `end` above, this offset always names a
	 * real column that was actually written. */
	{
		int before = start_col - 1 + utf8_count(line, cur);
		int want_row = before / width;
		int want_col = before % width + 1;

		if (end_row > want_row)
			outseq("\033[%dA", end_row - want_row);
		outseq("\033[%dG", want_col);
		*row = want_row;
	}
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

void miniedit_cmd_begin(void)
{
	fold_rawlen = 0;
	fold_lines = 0;
}

void miniedit_cmd_done(int complete)
{
	char *out = NULL;
	int olen = 0;

	if (complete && fold_lines > 1 && (out = malloc(2 * fold_rawlen + 2)))
		olen = fold_text(fold_raw, fold_rawlen, out);
	if (olen > 0)
		hist_write(out, olen);
	else if (fold_lines)
		hist_write(fold_raw, fold_rawlen);
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
	struct stackmark smark;
	const char *prompt;
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
	pushstackmark(&smark, stackblocksize());
	prompt = getprompt(NULL);
	out(prompt, strlen(prompt));
	popstackmark(&smark);
	*start_col = get_cursor(NULL);
	*row = 0;
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
	start_col = get_cursor(NULL);

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
			int r, n;

			/* "Clear" the screen by scrolling it up (so it all
			 * stays in the scrollback) until the first row of the
			 * edit block is at the top. */
			get_cursor(&r);
			n = r - row - 1;	/* rows above the block */
			if (r && n > 0) {
				out("\033[999B", 6);	/* to the bottom row */
				for (; n > 0; n -= 8)
					out("\n\n\n\n\n\n\n\n", n < 8 ? n : 8);
				outseq("\033[%d;1H", row + 1);
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
	if (hmap)
		munmap((void *)hmap, hmaplen);
	tcsetattr(0, TCSANOW, &save);

	if (eof) {
		*lenp = 0;
		return NULL;
	}
	*lenp = len;
	return line;
}
