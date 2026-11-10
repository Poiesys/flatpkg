#!/bin/sh
# key-armor.sh - a flatpakrepo's GPGKey is base64 of *something*: flathub
# encodes the binary key packets, NVIDIA encodes the ASCII-armored text.
# Armoring the decoded bytes unconditionally buries NVIDIA's key in a second
# armor, ostree then refuses it ("GPG: Unable to export keys: GPGME: No data")
# and `flatpak remote-add` of their repo fails.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

[ -x "$ROOT/build/flatpkg" ] ||
	{ echo "SKIP: build/flatpkg not built"; exit 0; }
command -v cc >/dev/null 2>&1 ||
	{ echo "SKIP: no cc in PATH"; exit 0; }
command -v pkg-config >/dev/null 2>&1 && pkg-config --exists glib-2.0 ||
	{ echo "SKIP: no glib-2.0 for pkg-config"; exit 0; }
[ -f "$ROOT/build/obj/daemon/ops/evui.o" ] ||
	{ echo "SKIP: the daemon's logging object is not built"; exit 0; }

T=$(mktemp -d /tmp/.fpkg-keyarmor.XXXXXX)
trap 'rm -rf "$T"' EXIT

# ostree_cmds.c under the test seam; --gc-sections drops the rest of it.
if ! cc -O1 -I"$ROOT/include" -include sys/param.h \
	-include "$ROOT/include/fpkg_noenv.h" -DFLATPKG_TESTING \
	-ffunction-sections -fdata-sections \
	$(pkg-config --cflags glib-2.0 gio-2.0) \
	-c "$ROOT/src/ostree_cmds.c" -o "$T/ostree_cmds.o" 2>"$T/cc.log"; then
	echo "FAIL: the key-armor build seam does not compile:"
	head -8 "$T/cc.log" | sed 's/^/       /'
	exit 1
fi

if ! cc -O1 -I"$ROOT/include" -include sys/param.h \
	-o "$T/key-armor" "$ROOT/tests/key-armor.c" "$T/ostree_cmds.o" \
	"$ROOT/build/obj/daemon/ops/evui.o" -L"$ROOT/build" \
	-lflatpkg_shared -Wl,--gc-sections -Wl,-rpath,"$ROOT/build" \
	-L/usr/local/lib -ljail -ldbus-1 -lutil -lz -lgio-2.0 -lgobject-2.0 \
	-lglib-2.0 2>"$T/ld.log"; then
	echo "FAIL: the key-armor harness does not link:"
	head -8 "$T/ld.log" | sed 's/^/       /'
	exit 1
fi

"$T/key-armor"
