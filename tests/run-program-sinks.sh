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

[ "$rc" -eq 0 ] &&
	echo "PASS: one fd for both sinks silences the child, and distinct sinks" \
	     "still carry what was written to them"
exit "$rc"
