/*
 * miniedit: a tiny built-in line editor for interactive input.
 *
 * This is not a readline/libedit replacement.  It exists because
 * building libedit (which hard-requires a curses/terminfo library,
 * and internally relies on wide-character libc support) is not
 * practical on minimal libcs such as dietlibc.  It supports only:
 *
 *   - inserting typed text at the cursor (UTF-8 aware)
 *   - Left / Right arrows to move by a character, Ctrl+Left / Ctrl+Right
 *     by a word, Home / End to jump to the start / end of the line
 *   - Backspace and Delete, one whole UTF-8 character at a time, and
 *     Ctrl+W to delete the word before the cursor
 *   - Up / Down to walk through the history, which is kept in
 *     ~/.bash_history, and Ctrl+R to search it
 *   - Tab to complete file names (only), as bash does
 *   - Ctrl+L to scroll the edit line to the top of the screen
 *   - Ctrl+C to abandon the line and Ctrl+D to end the input
 *
 * It assumes a vt100-compatible terminal (xterm and practically
 * everything else in common use).
 *
 * Ordinary editing never asks the terminal anything: no cursor position
 * report (ESC [ 6 n) is sent while typing, moving the cursor, using
 * history or completing names.  The only exception is Ctrl+L, which
 * needs the cursor's screen row.  This matters for programs that drive
 * the shell by typing into a pty before, or without, a terminal
 * answering: they must get the line echoed and edited straight away.
 */

#ifndef MINIEDIT_H
#define MINIEDIT_H

/* Maximum line length (bytes), including the trailing '\n' if any. */
#define MINIEDIT_MAXLINE 1024

/*
 * Read one line from fd 0, echoing to fd 2, using our own minimal
 * raw-mode editing.  On success, returns a pointer to a buffer
 * holding the line (terminated with '\n' if the user pressed
 * Enter), and stores its length in *lenp.  The returned buffer is
 * only valid until the next call.  Returns NULL on EOF (Ctrl-D on
 * an empty line).
 */
const char *miniedit_gets(int *lenp);

/*
 * Called by the parser each time it prints a prompt, with the expanded
 * prompt text.  The editor works out where typing starts from this text
 * (its visible width), rather than asking the terminal where the cursor
 * is: a terminal that is not answering, such as a pty a program is
 * driving, would otherwise stall the shell or garble its prompt.  The
 * prompt belongs to the next miniedit_gets() call.
 */
void miniedit_prompt_shown(const char *s);

/*
 * History folding of multi-line compound commands (see miniedit.c).
 * miniedit_cmd_begin() is called when the parser starts a command;
 * miniedit_cmd_done() when it has finished one, with complete set to
 * 0 if the command was cut short (EOF) and must not be folded.
 */
void miniedit_cmd_begin(void);
void miniedit_cmd_done(int complete);

#endif
