#!/bin/sh
# Regression tests for brace expansion (src/expand.c).
#
#   sh run.sh [path/to/dash]        (default: ../../src/dash)
#
# cases.txt holds pairs of lines: a word, then what
#
#   printf '[%s]' WORD
#
# prints for it.  The expected output was produced by bash 5.2, so this
# checks dash against bash.  Cases where the two intentionally differ
# (see the end of this file) are not in the table.
#
# Needs only a POSIX shell; exits non-zero if anything fails.

here=$(cd "$(dirname "$0")" && pwd)
dash=${1:-$here/../../src/dash}
case $dash in /*) ;; *) dash=$PWD/$dash ;; esac
[ -x "$dash" ] || { echo "no dash binary at $dash" >&2; exit 2; }

work=$(mktemp -d) || exit 2
trap 'rm -rf "$work"' EXIT
cd "$work" || exit 2
mkdir g
: > g/a.c; : > g/a.h; : > g/b.c; : > "g/x y"

pass=0
fail=0

ok() { pass=$((pass + 1)); }
bad() {
	fail=$((fail + 1))
	printf 'FAIL  %s\n   want: %s\n   got:  %s\n' "$1" "$2" "$3"
}

# Table driven part
while read -r word && read -r want; do
	got=$("$dash" -c "x=; HOME=/h; printf '[%s]' $word" 2>&1)
	if [ "$got" = "$want" ]; then ok; else bad "$word" "$want" "$got"; fi
done < "$here/cases.txt"

# check NAME SCRIPT WANT: run SCRIPT with dash -c, compare its output
check() {
	got=$("$dash" -c "$2" 2>&1)
	if [ "$got" = "$3" ]; then ok; else bad "$1" "$3" "$got"; fi
}

check "the example" 'echo foo-{{1..5},9,bar}' \
	'foo-1 foo-2 foo-3 foo-4 foo-5 foo-9 foo-bar'

# Applies to command arguments and for lists, nothing else
check "for list" 'for i in {1..3}; do printf "%s " $i; done' '1 2 3 '
check "set --" 'set -- {x,y,z}; echo $# $*' '3 x y z'
check "command name" '{echo,hi,there}' 'hi there'
check "function args" 'f() { echo "$#:$*"; }; f {1,2}{a,b}' '4:1a 1b 2a 2b'
check "assignment" 'x={a,b}; echo "$x"' '{a,b}'
check "case word" 'case {a,b} in "{a,b}") echo literal;; *) echo no;; esac' 'literal'
check "here-document" 'cat <<EOT
{a,b}
EOT' '{a,b}'
check "redirection" 'echo hi > {a,b}; ls -d "{"*' '{a,b}'

# Results of other expansions are never brace expanded
check "variable" 'v={a,b}; echo $v "$v"' '{a,b} {a,b}'
check "json in variable" 'j="{\"a\":1,\"b\":2}"; echo $j' '{"a":1,"b":2}'
check "no sequence from variables" 'n=3; echo {1..$n}' '{1..3}'
check "comma from variable" 'w=p,q; echo {$w,r}' 'p,q r'
check "IFS comma" 'IFS=,; w=p,q; echo {$w,r}' 'p q r'
check "inside \${}" 'echo ${u:-{a,b}} "${u:-{a,b}}"' '{a,b} {a,b}'
check "pattern operand" 'x=3; echo ${x#{a,b}}' '3}'
check "inside \$(())" '(echo $((1+{1,2}))) 2>&1 | sed "s/^.*: 1: //"' \
	'arithmetic expression: expecting primary: "1+{1,2}"'
check "inside \$()" 'echo $(echo {a,b}) "$(echo {a,b})"' 'a b a b'

# Command substitution is run once per generated word, as in bash
check "substitution count" \
	': > cnt; echo {a,b,c}$(echo x >> cnt) >/dev/null; wc -l < cnt | tr -d " "' 3
check "substitution count 2" \
	': > cnt; echo {$(echo 1 >> cnt),$(echo 2 >> cnt)}x >/dev/null; wc -l < cnt | tr -d " "' 2
check "substitution in alternatives" \
	'echo {$(echo a),b}{$(echo c),d} x$(echo 1){p,q}y$(echo 2)' \
	'ac ad bc bd x1py2 x1qy2'

# Bytes that are special inside dash's parse tree must survive
check "CTL bytes" "printf '%s ' \$'a\\201'{b,c} | od -An -tx1 | tr -s ' '" \
	' 61 81 62 20 61 81 63 20'
check "newline alternative" 'printf "[%s]" {a,$'"'"'\n'"'"',b}' '[a][
][b]'
check "multibyte" 'echo {é,ü}{ß,ø}' 'éß éø üß üø'

# Big and extreme sequences
check "many words" 'echo {1..100000} | wc -w | tr -d " "' 100000
check "product" 'echo {1..100}{1..100} | wc -w | tr -d " "' 10000
check "64 bit range" \
	'echo {-9223372036854775808..-9223372036854775806} {9223372036854775805..9223372036854775807}' \
	'-9223372036854775808 -9223372036854775807 -9223372036854775806 9223372036854775805 9223372036854775806 9223372036854775807'
check "64 bit overflow" 'echo {9223372036854775807..9223372036854775808}' \
	'{9223372036854775807..9223372036854775808}'

# Where dash intentionally differs from bash, all in dash's favour:
#
#   $HOME{a,b}   bash looks up variables HOMEa and HOMEb, since it expands
#                braces on the raw text; dash expands $HOME, then appends.
#   {1..{3,4}}   bash drops the outer braces of a group it considers
#                invalid; dash leaves them and expands the inner group.
#   {A..e}       bash's quote removal eats the \ that the sequence makes.
#   echo > {a,b} bash fails with "ambiguous redirect"; dash does not
#   export x={a,b}   expand redirections or assignment-like arguments.
check "dash: \$HOME{a,b}" 'HOME=/h; echo $HOME{a,b}' '/ha /hb'

printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
