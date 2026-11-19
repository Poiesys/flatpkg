#!/bin/sh
# zsh-completion-shadow.sh - the generated zsh completion must not shadow
# zsh's special `words` array: _arguments reads it to find the command line.
# A local `words` breaks every app's completion, not just ours.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
CLI=$ROOT/build/flatpkg
[ -x "$CLI" ] || { echo "SKIP: $CLI not built"; exit 0; }

fail=0
note() { echo "FAIL: $1"; fail=1; }

tmp=$(mktemp -d /tmp/.fpkg-zsh.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

"$CLI" complete zsh flatpak > "$tmp/_flatpak" 2>/dev/null

# The candidate-option array must be `opts`; a `local` of `words` is the bug.
if grep -qE '^[[:space:]]*local -a[[:space:]].*\bwords\b' "$tmp/_flatpak"; then
	note "the completion declares a local 'words'"
fi
grep -qE 'local -a opts' "$tmp/_flatpak" ||
	note "the candidate array is not 'opts'"

# zsh's special array is still read for the current word, and the
# subcommand dispatch is by $line, not by shadowing a special.
grep -q '\$words\[CURRENT\]' "$tmp/_flatpak" ||
	note "the current word no longer reads \$words"
grep -q 'case \$line\[1\] in' "$tmp/_flatpak" ||
	note "the subcommand dispatch is not by \$line"
grep -q '_describe -t options .option. opts' "$tmp/_flatpak" ||
	note "_describe does not use the opts array"

if command -v zsh >/dev/null 2>&1; then
	zsh -n "$tmp/_flatpak" || note "the completion is not valid zsh"
fi

[ "$fail" -eq 0 ] && echo "PASS: the zsh completion keeps zsh's words and uses opts"
exit "$fail"
