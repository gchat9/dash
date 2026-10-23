# A fork of dash.

Upstream is over there:

- https://en.wikipedia.org/wiki/Debian_Almquist_shell
- http://gondor.apana.org.au/~herbert/dash/
- https://github.com/herbertx/dash

## This fork adds:

- fixes to allow build with fefe's dietlibc;
- some bash-like features:
  - brace expansion;
  - \u and \w prompt string escapes for PS1;
  - prompt/command-line editor, with hardcoded escape codes. Should work on VT-100, Linux console, xterm & derivatives;
  - Tab completion of file names, of commands from $PATH at the start of a command (or after `;`), and of man page names after `man` or `man <section>`;
  - ~/.bash_history, with bash's HISTCONTROL=ignoreboth behaviour: commands starting
    with a space, and commands already in the file, are not saved (a repeat still
    moves to the front for Up). Turn off with
    `--disable-hist-ignorespace` and/or `--disable-hist-ignoredups` at configure time;
  - Up, Down and Ctrl-R order history per session: this shell's own last 32 commands
    first (newest first), then the rest of the file. Down retraces Up exactly. Commands
    from concurrent sessions never appear between a session's own commands;

