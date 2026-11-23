#!/bin/sh
# help-golden.sh - the help page is reviewed text, not whatever the tables
# produce today.
#
# The help page is generated from the tables in cli.c, so a wording change, a
# dropped option or a reordered group is invisible in a diff of the C source.
# These golden files are the page as it reads now: a change to them is a
# change a reviewer sees.  Regenerate only when the new text is what you mean:
#
#     FLATPKG_GOLDEN_UPDATE=1 sh tests/help-golden.sh
#
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN="$ROOT/build/flatpkg"
GOLDEN="$ROOT/tests/golden"

[ -x "$BIN" ] || { echo "SKIP: build/flatpkg not built"; exit 0; }

T=$(mktemp -d /tmp/.fpkg-helpgold.XXXXXX)
trap 'rm -rf "$T"' EXIT

# A fixed environment decides the page: no colour, the dumb terminal, and the
# C locale.  The width is the terminal's, and the page is not on a terminal
# here, so it takes its fallback.
run_help() {
	out=$1
	shift
	env -i PATH=/usr/bin:/bin:/usr/local/bin TERM=dumb NO_COLOR=1 \
		LC_ALL=C "$BIN" "$@" >"$out" 2>/dev/null
}

fail=0
check() {
	name=$1
	golden=$2
	shift 2
	out="$T/$name.out"
	run_help "$out" "$@" || {
		echo "FAIL: '$*' did not exit 0"
		fail=1
		return
	}
	if [ ! -f "$golden" ]; then
		echo "FAIL: $golden is missing; regenerate it with"
		echo "      FLATPKG_GOLDEN_UPDATE=1 sh tests/help-golden.sh"
		fail=1
		return
	fi
	if cmp -s "$out" "$golden"; then
		return
	fi
	if [ "${FLATPKG_GOLDEN_UPDATE:-0}" = 1 ]; then
		cp "$out" "$golden"
		echo "updated $golden"
		return
	fi
	echo "FAIL: the help page changed; read the diff, and if the new text is"
	echo "      what you mean, regenerate with FLATPKG_GOLDEN_UPDATE=1:"
	diff -u "$golden" "$out" | sed 's/^/        /'
	fail=1
}

check main "$GOLDEN/help-main.txt" --help
check run "$GOLDEN/help-run.txt" help run

# An empty or truncated page must not become the golden file.
wc -l "$GOLDEN/help-main.txt" | awk '$1 < 40 { exit 1 }' || {
	echo "FAIL: the golden main help page is implausibly short"
	fail=1
}

[ "$fail" -eq 0 ] &&
	echo "PASS: --help and a command page read as the reviewed text in" 	     "tests/golden/, so a wording change shows up in review."
exit "$fail"
