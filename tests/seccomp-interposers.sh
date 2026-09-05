#!/bin/sh
# seccomp-interposers.sh - every interposer in the shim must be reachable: the
# symbol defined non-static AND listed in the version script, or the function is
# dead code that compiles, reads correctly, and nothing calls.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/src/seccomp_shim.c
MAP=$ROOT/src/seccomp_shim.map

[ -f "$SRC" ] || { echo "SKIP: src/seccomp_shim.c not found"; exit 0; }

fail=0
note() { echo "FAIL: $1"; fail=1; }

tmp=$(mktemp -d /tmp/.fpkg-interp.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

# An interposer is a definition at column 0 whose name is followed by `(` - the
# return type is on the same line, so indentation cannot tell a static helper
# from an interposer and the shape has to be matched as the file has it.
grep -E '^(int|long|ssize_t|void|unsigned|char) [a-z_][a-z_0-9]*[ ]*\(' \
    "$SRC" | sed -E 's/^[a-z_]+ +([a-z_][a-z_0-9]*).*/\1/' | sort -u \
    > "$tmp/def"

# The map is indented with a TAB, and `^ +` does not match one.  `tr -d ' ;'`
# strips spaces and semicolons but NOT the tab, so every entry kept a leading tab
# and nothing matched the def list.  Strip the blank characters too.
grep -oE '^[[:space:]]+[a-z_0-9]+;' "$MAP" | tr -d ' ' | tr -d ';' |
    tr -d '[:blank:]' | sort -u > "$tmp/exp"

n_def=$(wc -l < "$tmp/def" | tr -d ' ')
n_exp=$(wc -l < "$tmp/exp" | tr -d ' ')

# A floor under both counts, so a carve that stops matching fails rather than passing
# on an empty comparison.  It sits well below the real counts and the comparison
# still runs, so a deleted export is not reported as "the carve broke".
[ "$n_def" -ge 10 ] ||
    note "expected at least 10 interposers in seccomp_shim.c, found $n_def - the carve broke"
[ "$n_exp" -ge 10 ] ||
    note "expected at least 10 exports in seccomp_shim.map, found $n_exp - the carve broke"

if [ "$n_def" -ge 10 ] && [ "$n_exp" -ge 10 ]; then
	comm -23 "$tmp/def" "$tmp/exp" > "$tmp/missing"
	while read -r sym; do
		[ -n "$sym" ] || continue
		note "$sym() is defined but not in seccomp_shim.map, so the dynamic linker never calls it"
	done < "$tmp/missing"

	comm -13 "$tmp/def" "$tmp/exp" > "$tmp/stale"
	while read -r sym; do
		[ -n "$sym" ] || continue
		note "seccomp_shim.map exports $sym, which is not defined - a stale entry"
	done < "$tmp/stale"

	[ "$n_def" = "$n_exp" ] ||
	    note "$n_def interposers but $n_exp exports in the version script"
fi

[ $fail -eq 0 ] &&
    echo "PASS: every interposer in the shim is exported, and every export is defined"
exit $fail
