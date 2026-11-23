#!/bin/sh
# progress-msg.sh - the progress builder already terminates its buffer.
#
# build_progress_msg() clamps what it writes and then NUL-terminates at
# min(pos, cap - 1), and returns `pos`, which can equal `cap` when the text
# fills the buffer exactly.  Two call sites wrote `msg[len] = '\0'` after it,
# one byte past a 1024-byte stack array.  This holds the contract and the two
# call sites.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

[ -x "$ROOT/build/flatpkg" ] ||
	{ echo "SKIP: build/flatpkg not built"; exit 0; }
command -v cc >/dev/null 2>&1 ||
	{ echo "SKIP: no cc in PATH"; exit 0; }
[ -f "$ROOT/build/obj/cli/tb_progress.o" ] ||
	{ echo "SKIP: tb_progress.o is not built"; exit 0; }

T=$(mktemp -d /tmp/.fpkg-pmsg.XXXXXX)
trap 'rm -rf "$T"' EXIT

src="$ROOT/src/cli/tb_progress.c"

# 1. The contract: with a buffer far too small for the text, the return can
#    equal the buffer size, so a second terminator at [len] is out of bounds.
cat > "$T/probe.c" <<PROBE
#include <stdio.h>
#include <string.h>

#include "fplog.h"

#include "$src"

int main(void) {
	char buf[16];
	const char* label = "a label that cannot possibly fit in sixteen bytes";

	if (build_progress_msg(label, 200, 5, 100, tb_default_chars(), false,
			       buf, sizeof(buf)) < sizeof(buf))
		return 0;
	return strlen(buf) == sizeof(buf) - 1 ? 1 : 2;
}
PROBE
LIBS="-L$ROOT/build -lflatpkg_shared -Wl,-rpath,$ROOT/build"
LIBS="$LIBS -L/usr/local/lib -ljail -ldbus-1 -lutil -lz"
LIBS="$LIBS -lgio-2.0 -lgobject-2.0 -lglib-2.0 -lintl"
if cc -O1 -I"$ROOT/include" -I"$ROOT" -include sys/param.h -o "$T/probe" \
	"$T/probe.c" "$ROOT/build/obj/cli/tb_core.o" \
	"$ROOT/build/obj/cli/translations.o" $LIBS >"$T/cc.log" 2>&1; then
	rc=0
	"$T/probe" || rc=$?
	[ "$rc" -eq 1 ] ||
		{ echo "FAIL: build_progress_msg() no longer returns cap when full"; exit 1; }
else
	echo "FAIL: the progress probe does not build:"
	head -6 "$T/cc.log" | sed 's/^/       /'
	exit 1
fi

# 2. Neither call site terminates the buffer a second time.
check_sites() {
	file=$1
	awk '/build_progress_msg\(/ { left = 8 }
	     left > 0 { if ($0 ~ /\[len\] *=/) found = 1; left-- }
	     END { exit found ? 1 : 0 }' "$file"
}
check_sites "$src" ||
	{ echo "FAIL: a call site writes [len] after build_progress_msg()"; exit 1; }
# The check must be able to fail.
awk 'BEGIN { done = 0 }
     /\(void\)build_progress_msg\(/ && !done { print; print "\tmsg[1024] = 0; msg[len] = 0;"; done = 1; next }
     { print }' "$src" >"$T/planted.c"
cmp -s "$src" "$T/planted.c" &&
	{ echo "FAIL: the call-site plant was not applied"; exit 1; }
check_sites "$T/planted.c" &&
	{ echo "FAIL: the call-site check passes with the write planted back"; exit 1; }

echo "PASS: the progress builder terminates its own buffer, and neither call"
echo "      site writes past it."
