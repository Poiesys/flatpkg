#!/bin/sh
# teardown-guard.sh - a jail root with a mount that cannot be taken down is
# left alone, and the startup sweep's lock probe counts only the regular file
# it opened as a lock.  The mount table and unmount are replaced at COMPILE
# time (-DFLATPKG_TESTING), so the daemon has no runtime switch that could skip
# the guard; the fakes exist only in this harness.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

[ -x "$ROOT/build/flatpkg" ] ||
	{ echo "SKIP: build/flatpkg not built"; exit 0; }
command -v cc >/dev/null 2>&1 ||
	{ echo "SKIP: no cc in PATH"; exit 0; }
command -v mdo >/dev/null 2>&1 &&
	mdo -u root true </dev/null >/dev/null 2>&1 ||
	{ echo "SKIP: mdo cannot give root without a prompt (the run dir is root's)"; exit 0; }
[ -f "$ROOT/build/obj/daemon/ops/evui.o" ] ||
	{ echo "SKIP: the daemon's logging object is not built"; exit 0; }

T=$(mktemp -d /tmp/.fpkg-teardown.XXXXXX)
trap 'rm -rf "$T"' EXIT

# sandbox.c under the test seam; the rest of its sections are dropped by
# --gc-sections at link time.
if ! cc -O1 -I"$ROOT/include" -include sys/param.h \
	-include "$ROOT/include/fpkg_noenv.h" -DFLATPKG_TESTING \
	-ffunction-sections -fdata-sections \
	-c "$ROOT/src/daemon/sandbox.c" -o "$T/sandbox.o" 2>"$T/cc.log"; then
	echo "FAIL: the teardown build seam does not compile:"
	head -8 "$T/cc.log" | sed 's/^/       /'
	exit 1
fi

if ! cc -O1 -I"$ROOT/include" -o "$T/teardown-guard" \
	"$ROOT/tests/teardown-guard.c" "$T/sandbox.o" \
	"$ROOT/build/obj/daemon/ops/evui.o" -L"$ROOT/build" \
	-lflatpkg_shared -Wl,--gc-sections -Wl,-rpath,"$ROOT/build" \
	-L/usr/local/lib -ljail -ldbus-1 -lutil -lz -lgio-2.0 -lgobject-2.0 \
	-lglib-2.0 \
	2>"$T/ld.log"; then
	echo "FAIL: the teardown harness does not link:"
	head -8 "$T/ld.log" | sed 's/^/       /'
	exit 1
fi

# The harness creates its root under the run dir and exits non-zero on a
# failure; mdo is only for the privilege.  A hang is a failure: probing a fifo
# without O_NONBLOCK never returns.
mdo timeout 30 "$T/teardown-guard"
