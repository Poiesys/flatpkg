#!/bin/sh
# probelib.sh - build tests/dbusprobe.c for whichever guard needs it.  SOURCED,
# never run; `build_probe <dest>` echoes the path and returns 0 built, 1 build
# failed, 2 prerequisite missing (a skip the suite reports rather than counts).
build_probe() {
	_dest=$1
	shift 2>/dev/null || true
	_cc=$(command -v cc 2>/dev/null || true)
	_pkg=$(command -v pkg-config 2>/dev/null || true)

	if [ -z "$_cc" ] || [ -z "$_pkg" ]; then
		echo "SKIP no cc/pkg-config to build the D-Bus probe"
		return 2
	fi
	if ! $_pkg --exists gio-2.0 2>/dev/null; then
		echo "SKIP no gio-2.0 (glib), which tests/dbusprobe.c needs"
		return 2
	fi
	if ! $_cc -O0 -Wall -Wextra -o "$_dest" \
	    "$PROBE_SRC" $($_pkg --cflags --libs gio-2.0) >"$_dest.log" 2>&1; then
		echo "BUILD-FAIL tests/dbusprobe.c does not build:"
		sed 's/^/        /' "$_dest.log" | head -8
		return 1
	fi
	# A warning is a failure: that harness then printed a nonsense case
	# count and exited 0, and nothing in its output showed it.
	if grep -q 'warning:' "$_dest.log" 2>/dev/null; then
		echo "BUILD-WARN tests/dbusprobe.c builds with warnings:"
		sed 's/^/        /' "$_dest.log" | head -8
		return 1
	fi
	echo "$_dest"
	return 0
}

# tests/dbusprobe.c sits next to this library, whichever guard sourced it.
PROBE_SRC=${PROBE_SRC:-"$(dirname "$0")/dbusprobe.c"}
