#!/bin/sh
# context-grants.sh - the [Context] -> sandbox_plan_t translation and the
# metadata parser feeding it: every value, key and negated filesystem token an
# app states must set its flag, and one flatpkg does not know must warn and name it.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

tmp=$(mktemp -d /tmp/.fpkg-ctx.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

# A floor on the case count: a harness whose cases were all deleted prints
# "0 case(s), 0 failed" and exits 0.  capture() in tests/permissions.c is what
# lets the override path assert a warning, at a terminal width the child sets.
floor=36

srcs="tests/permissions.c src/shared/permissions.c src/shared/metadata.c \
      src/shared/util.c src/cli/tb_core.c src/cli/tb_progress.c src/cli/tb_tui.c"
for s in $srcs; do
	[ -f "$ROOT/$s" ] || { echo "SKIP: $s is not in the tree"; exit 0; }
done

# -Wall -Wextra with warnings fatal: the case counter was once shadowed by a
# local array, so the harness printed "546301392 case(s), 0 failed" and exited
# 0 - the number this guard floors on was garbage.
if ! cc -O0 -Wall -Wextra -std=gnu11 -I"$ROOT/include" \
	-o "$tmp/h" $srcs -pthread > "$tmp/build.log" 2>&1; then
	echo "FAIL: the [Context] harness does not build:"
	sed 's/^/        /' "$tmp/build.log"
	exit 1
fi
if grep -q 'warning:' "$tmp/build.log"; then
	echo "FAIL: the [Context] harness builds with warnings:"
	sed 's/^/        /' "$tmp/build.log"
	exit 1
fi

# The environment the harness reads, owned here: the three names fplog
# consults for colour.  Exported EMPTY rather than excepted, because a
# second list of names that are "fine" is a list to forget.
out=$(TERM=dumb NO_COLOR= FORCE_COLOR= "$tmp/h" 2>/dev/null) || rc=$?
rc=${rc:-0}
echo "$out" | sed 's/^/  /'

# The count, taken from the output rather than assumed.
got=$(echo "$out" | sed -n 's/^  \([0-9][0-9]*\) case(s), \([0-9][0-9]*\) failed$/\1/p')
[ -n "$got" ] || {
	echo "FAIL: the harness printed no case count, so its floor cannot be"
	echo "      checked - and a harness with no cases exits 0."
	exit 1
}
if [ "$got" -lt "$floor" ]; then
	echo "FAIL: the harness ran $got cases, below the floor of $floor."
	exit 1
fi

if [ "$rc" -ne 0 ]; then
	echo "FAIL: the [Context] translation or the metadata parser rejected or"
	echo "      mishandled a case (rc=$rc)"
	exit 1
fi

echo "PASS: unknown [Context] VALUES and misspelled metadata KEYS both warn"
echo "      naming what was wrong, known entries and unimplemented-but-legal"
echo "      keys set their flag and stay quiet, and \`if:\` conditionals are"
echo "      consumed in both directions - including the negated"
echo "      \`filesystem=\` token, in both of the paths that read it."
exit 0
