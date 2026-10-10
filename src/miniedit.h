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

#endif
