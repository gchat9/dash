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
  - ~/.bash_history;

