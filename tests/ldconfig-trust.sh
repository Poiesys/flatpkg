#!/bin/sh
# ldconfig-trust.sh - the root daemon must not execute a binary out of a tree
# it does not own.  Imported runtimes are the caller's tree, so
# the ldconfig step is gated on the file itself: regular, owned by root, not
# writable by group or other.
#
# The predicate is compiled out of sandbox_root.c under -DFLATPKG_TESTING, the
# same seam teardown-guard.sh uses; mdo is only there to make a root-owned file
# for a test that runs as the caller.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

[ -x "$ROOT/build/flatpkg" ] ||
	{ echo "SKIP: build/flatpkg not built"; exit 0; }
command -v cc >/dev/null 2>&1 ||
	{ echo "SKIP: no cc in PATH"; exit 0; }
command -v mdo >/dev/null 2>&1 &&
	mdo -u root true </dev/null >/dev/null 2>&1 ||
	{ echo "SKIP: mdo cannot give root without a prompt"; exit 0; }
[ -f "$ROOT/build/obj/daemon/ops/evui.o" ] ||
	{ echo "SKIP: the daemon's logging object is not built"; exit 0; }

T=$(mktemp -d /tmp/.fpkg-ldtrust.XXXXXX)
trap 'rm -rf "$T"' EXIT

# The function, with the rest of the unit dropped by --gc-sections.
if ! cc -O1 -I"$ROOT/include" -I"$ROOT/src/daemon" -include sys/param.h \
	-include "$ROOT/include/fpkg_noenv.h" -DFLATPKG_TESTING \
	-ffunction-sections -fdata-sections \
	-c "$ROOT/src/daemon/sandbox_root.c" -o "$T/sandbox_root.o" \
	2>"$T/cc.log"; then
	echo "FAIL: the ldconfig build seam does not compile:"
	head -8 "$T/cc.log" | sed 's/^/       /'
	exit 1
fi
if ! cc -O1 -I"$ROOT/include" -include sys/param.h \
	-o "$T/ldconfig-trust" "$ROOT/tests/ldconfig-trust.c" \
	"$T/sandbox_root.o" "$ROOT/build/obj/daemon/ops/evui.o" \
	-L"$ROOT/build" -lflatpkg_shared -Wl,--gc-sections \
	-Wl,-rpath,"$ROOT/build" -L/usr/local/lib -ljail -ldbus-1 -lutil \
	-lz -lgio-2.0 -lgobject-2.0 -lglib-2.0 2>"$T/ld.log"; then
	echo "FAIL: the ldconfig harness does not link:"
	head -8 "$T/ld.log" | sed 's/^/       /'
	exit 1
fi

# A tree laid out the way the predicate judges it.  The root-owned files are
# made with mdo; the caller-owned one is not, so this can run unprivileged.
: >"$T/root" && chmod 0755 "$T/root"
: >"$T/writable" && chmod 0666 "$T/writable"
: >"$T/user" && chmod 0755 "$T/user"
mkdir "$T/dir"
ln -s "$T/root" "$T/link"
mdo chown root:wheel "$T/root" "$T/writable" "$T/dir" >/dev/null 2>&1 ||
	{ echo "SKIP: mdo cannot chown to root"; exit 0; }

"$T/ldconfig-trust" "$T"
