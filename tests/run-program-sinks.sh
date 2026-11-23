#!/bin/sh
# run-program-sinks.sh - run_program() keeps a caller's stderr where the caller
# put it when stdout and stderr are the same fd.  A child that closes that fd
# after the stdout dup leaves the stderr dup with EBADF, so the child's
# diagnostics land on the caller's terminal - the "jls: jail N not found" leak
# from jail_exists() was exactly this.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

[ -x "$ROOT/build/flatpkg" ] ||
	{ echo "SKIP: build/flatpkg not built"; exit 0; }
command -v cc >/dev/null 2>&1 ||
	{ echo "SKIP: no cc in PATH"; exit 0; }
[ -f "$ROOT/build/obj/daemon/ops/evui.o" ] ||
	{ echo "SKIP: the daemon's logging object is not built"; exit 0; }

T=$(mktemp -d /tmp/.fpkg-sinks.XXXXXX)
trap 'rm -rf "$T"' EXIT

if ! cc -O1 -I"$ROOT/include" -include sys/param.h \
	-o "$T/run-program-sinks" "$ROOT/tests/run-program-sinks.c" \
	"$ROOT/build/obj/daemon/ops/evui.o" -L"$ROOT/build" \
	-lflatpkg_shared -Wl,-rpath,"$ROOT/build" -L/usr/local/lib -ljail \
	-ldbus-1 -lutil -lz -lgio-2.0 -lgobject-2.0 -lglib-2.0 -lintl \
	2>"$T/cc.log"; then
	echo "FAIL: the run_program harness does not build:"
	head -8 "$T/cc.log" | sed 's/^/       /'
	exit 1
fi

rc=0
"$T/run-program-sinks" 2>"$T/err" || rc=$?
[ "$rc" -eq 0 ] ||
	echo "FAIL: the harness reported a routing failure (rc=$rc)"
if [ -s "$T/err" ]; then
	echo "FAIL: a silenced child wrote to the caller's stderr:"
	sed 's/^/       /' "$T/err"
	rc=1
fi

# The capture a tool's stdout lands in is bounded: a remote's commit metadata
# can be any size, and a prefix that parses as a whole document is worse than a
# refusal.  The appender is carried out and run on its own.
strip_comments <"$ROOT/src/ostree.c" > "$T/ostree-stripped.c" 2>/dev/null ||
	. "$ROOT/tests/commentlib.sh"
strip_comments <"$ROOT/src/ostree.c" > "$T/ostree-stripped.c"
carve_function '^size_t buf_append_capped\(' "$T/ostree-stripped.c" \
	> "$T/capped.inc" ||
	{ echo "FAIL: buf_append_capped() not found"; exit 1; }
cat > "$T/capped.c" <<'PROBE'
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FPKG_TOOL_CAPTURE_MAX (16u << 20)
static void fpkg_errorf(const char* fmt, ...) {
	(void)fmt;
	abort();
}
PROBE
cat "$T/capped.inc" >> "$T/capped.c"
cat >> "$T/capped.c" <<'PROBE'
int main(void) {
	char* buf = NULL;
	size_t cap = 0, len = 0;
	bool stopped = false;
	char chunk[4096];

	memset(chunk, 'x', sizeof(chunk));
	for (int i = 0; i < 5000; i++)
		buf_append_capped(&buf, &cap, &len, &stopped, chunk,
				  sizeof(chunk));
	if (!stopped) return 1;
	if (len > FPKG_TOOL_CAPTURE_MAX) return 2;
	if (strlen(buf) != len) return 3;
	size_t before = len;
	buf_append_capped(&buf, &cap, &len, &stopped, "zzz", 3);
	if (len != before) return 4;
	return 0;
}
PROBE
if cc -std=gnu11 -Wall -Wextra -Werror -o "$T/capped" "$T/capped.c" \
	> "$T/capped.log" 2>&1; then
	"$T/capped" ||
		{ echo "FAIL: the tool capture is not bounded at the cap"; rc=1; }
	sed 's/if (\*len + n + 1 > FPKG_TOOL_CAPTURE_MAX) {/if (0) {/' \
		"$T/capped.c" > "$T/capped-plant.c"
	cmp -s "$T/capped.c" "$T/capped-plant.c" &&
		{ echo "FAIL: the capture plant was not applied"; rc=1; }
	cc -std=gnu11 -o "$T/capped-plant" "$T/capped-plant.c" \
		> /dev/null 2>&1 &&
		"$T/capped-plant" &&
		{ echo "FAIL: the capture probe passes without the cap"; rc=1; }
else
	echo "FAIL: the capture probe did not build:"
	head -4 "$T/capped.log" | sed 's/^/       /'
	rc=1
fi

[ "$rc" -eq 0 ] &&
	echo "PASS: one fd for both sinks silences the child, distinct sinks" \
	     "still carry what was written to them, and the tool capture is capped"
exit "$rc"
