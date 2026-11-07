#!/bin/sh
# table-cols.sh - a table's column count is derived, never a bare number.
# The count and the headers array are two places that must agree; a literal
# next to an array is the one that goes stale when a column is added, which is
# what FP_TABLE_COLS() and the per-table column enums exist to prevent.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$(dirname "$0")/commentlib.sh"
tmp=$(mktemp -d /tmp/.fpkg-tablecols.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
fail=0

note() {
	echo "FAIL: $*"
	fail=1
}

echo "checking table column counts"
echo

for f in "$ROOT"/src/*.c "$ROOT"/src/*/*.c "$ROOT"/src/*/*/*.c; do
	[ -f "$f" ] || continue
	# The call may wrap, so scan it character by character: the second
	# top-level argument is the count.  FP_TABLE_COLS(headers) carries
	# parens of its own, which the depth count keeps out of the split.
	strip_comments < "$f" | awk -v file="${f##*/}" '
	function trim(s) {
		sub(/^[ \t\r]+/, "", s); sub(/[ \t\r]+$/, "", s)
		return s
	}
	BEGIN { incall = 0 }
	{
		line = $0
		i = 1
		while (i <= length(line)) {
			if (!incall) {
				p = index(substr(line, i), "fp_print_table(")
				if (p == 0) break
				incall = 1
				depth = 1
				arg = 1
				text = ""
				start = NR
				i += p + 14
				continue
			}
			c = substr(line, i, 1)
			if (c == "(") depth++
			else if (c == ")") {
				depth--
				if (depth == 0) {
					if (arg == 2 && trim(text) ~ /^[0-9]+$/)
						printf "FAIL %s:%d: fp_print_table() column count is a literal\n", file, start
					incall = 0
					continue
				}
			} else if (c == "," && depth == 1) {
				if (arg == 2 && trim(text) ~ /^[0-9]+$/)
					printf "FAIL %s:%d: fp_print_table() column count is a literal\n", file, start
				arg++
				text = ""
				i++
				continue
			}
			if (arg == 2) text = text c
			i++
		}
	}' >> "$tmp/out"
done

if [ -s "$tmp/out" ]; then
	cat "$tmp/out"
	fail=1
fi

# A carve that finds no calls passes every table; keep it honest.
calls=$(strip_comments < "$ROOT/src/cli/dispatch.c" | grep -c 'fp_print_table(' || true)
if [ "$calls" -lt 1 ]; then
	note "the scan found no fp_print_table() call in src/cli/dispatch.c"
fi
echo "ok   scanned every fp_print_table() call in src/"
echo

if [ "$fail" -eq 0 ]; then
	echo "PASS: a table's count comes from its headers, not a literal"
	exit 0
fi
echo "FAILED: a table names a column count that can go stale"
exit 1
