#!/bin/sh
# metadata-limits.sh - the metadata parser reads a file a remote published, so
# every field has a bound.  A value longer than the line buffer used to be
# read in two pieces and its tail parsed as a fresh key=value pair, a group
# name was truncated into another group, and the environment list had no
# count.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/src/shared/metadata.c"

[ -f "$SRC" ] || { echo "FAIL: $SRC is missing"; exit 1; }
command -v cc >/dev/null 2>&1 || { echo "SKIP: no cc in PATH"; exit 0; }

T=$(mktemp -d /tmp/.fpkg-metalimits.XXXXXX)
trap 'rm -rf "$T"' EXIT

build() {
	meta=$1
	out=$2
	cc -O1 -Wall -Wextra -std=gnu11 -I"$ROOT/include" -o "$out" \
		"$ROOT/tests/metadata-limits.c" "$meta" \
		"$ROOT/src/shared/util.c" "$ROOT/src/cli/tb_core.c" \
		"$ROOT/src/cli/tb_progress.c" "$ROOT/src/cli/tb_tui.c" \
		-pthread >"$T/cc.log" 2>&1
}

try() {
	if ! build "$1" "$T/h"; then
		echo "FAIL: the metadata harness does not build:"
		head -8 "$T/cc.log" | sed 's/^/       /'
		return 1
	fi
	"$T/h" 2>/dev/null
}

try "$SRC"

# --- the plants: each rule must fire on the tree it was written for --------
# 1. the over-long line read as two lines again
sed "s/line\[len - 1\] != '\\\\n'/line[len - 1] == '\\\\n'/" "$SRC" \
	> "$T/plant-line.c"
if cmp -s "$SRC" "$T/plant-line.c"; then
	echo "FAIL: the long-line plant was not applied"
	exit 1
fi
if try "$T/plant-line.c" >/dev/null 2>&1; then
	echo "FAIL: the guard passes when a long line is read in two pieces"
	exit 1
fi

# 2. the group name truncated again
sed 's/if (strlen(l + 1) >= sizeof(group)) {/if (0) {/' "$SRC" \
	> "$T/plant-group.c"
if cmp -s "$SRC" "$T/plant-group.c"; then
	echo "FAIL: the group plant was not applied"
	exit 1
fi
if try "$T/plant-group.c" >/dev/null 2>&1; then
	echo "FAIL: the guard passes when two long group names merge"
	exit 1
fi

echo "PASS: an over-long line, an over-long group name and an endless"
echo "      environment list are all bounded, and the parser says so"
