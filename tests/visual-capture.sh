#!/bin/sh
# visual-capture.sh - a real screenshot of the desktop can be captured and is not a black
# rectangle: grim resolves a display, writes a PNG, and the PNG has more than one colour.
# An X11 grab of a -rootless Xwayland root window is entirely black.  SKIPS with no display.

set -eu

# capturelib.sh lives beside this script.  `dirname "$0"`, not `$0`: probelib.sh
# sets PROBE_SRC the same way, and sourcing from outside tests/ yields a path that
# does not exist.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tests/capturelib.sh"

say() { printf '%s\n' "$*"; }
note() { printf '  %s\n' "$*"; }

pass=0
fail=0

check() {
	# check <description> <ok:0/1>
	if [ "$2" -eq 0 ]; then
		pass=$((pass + 1))
		note "ok    - $1"
	else
		fail=$((fail + 1))
		note "FAIL  - $1"
	fi
}

# --- 1. grim and the display -------------------------------------------

if [ -z "${GRIM_BIN:-}" ] && ! capture_resolve_display; then
	say "visual-capture: no grim or no Wayland display; skipping"
	say "visual-capture: (an X11 capture would be the black-rectangle trap here)"
	exit 0
fi
check "grim resolved (${GRIM_BIN})" 0
check "display resolved ($CAPTURE_RUNTIME_DIR/$CAPTURE_DISPLAY)" 0

# --- 2. capture --------------------------------------------------------

tmp=$(mktemp -d "${TMPDIR:-/tmp}/fpkg-visual.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
png="$tmp/shot.png"

if capture_display "$png"; then
	check "grim exited 0 and wrote a file" 0
else
	check "grim exited 0 and wrote a file" 1
fi

if [ -s "$png" ]; then
	# PNG magic.  A file that is not a PNG would make claim 3 meaningless.
	if [ "$(dd if="$png" bs=1 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')" \
	    = "89504e47" ]; then
		check "the capture is a PNG" 0
	else
		check "the capture is a PNG" 1
	fi
fi

# --- 3. it is not a black rectangle ------------------------------------

# The claim that matters: a rootless Xwayland grab, a grim run against the wrong
# output and a capture taken before the compositor painted all produce a well-formed
# PNG that is entirely black, and only a pixel test separates them.
colours=$(capture_colours "$png")

# Floor of 2: one colour means every sampled pixel is identical, which for a
# full-screen capture is the black-rectangle signature.  0 means the count could not
# be made, and it must not read as a pass.
if [ "${colours:-0}" -ge 2 ] 2>/dev/null; then
	check "the capture is not a black rectangle ($colours distinct colours)" 0
else
	check "the capture is not a black rectangle (got ${colours:-0} colour)" 1
fi

say "visual-capture: passed $pass, failed $fail"
[ "$fail" -eq 0 ] || exit 1
exit 0
