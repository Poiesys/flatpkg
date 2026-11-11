#!/bin/sh
# portal-fc.sh - the FileChooser option parser reads a variant's VALUE, never
# the variant, and checks every field's type before it reads it.  get_basic()
# on a container aborts the shared portal (libdbus's own assertion), which any
# sandboxed app can aim at it with one call; the filter rules used to be read
# through two recurse() calls and no next(), so a non-string pattern was read
# as a pointer.  The parser is carved out and driven with real messages.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/src/cli/portal.c"

[ -f "$SRC" ] || { echo "FAIL: $SRC is missing"; exit 1; }
command -v cc >/dev/null 2>&1 || { echo "SKIP: no cc in PATH"; exit 0; }
[ -x "$ROOT/build/flatpkg" ] ||
	{ echo "SKIP: build/flatpkg is not built"; exit 0; }

if command -v pkg-config >/dev/null 2>&1 &&
    pkg-config --exists dbus-1 2>/dev/null; then
	dbus_cflags=$(pkg-config --cflags dbus-1)
	dbus_libs=$(pkg-config --libs dbus-1)
elif [ -d /usr/local/include/dbus-1.0 ]; then
	dbus_cflags="-I/usr/local/include/dbus-1.0 -I/usr/local/lib/dbus-1.0/include"
	dbus_libs="-L/usr/local/lib -ldbus-1"
else
	echo "SKIP: no dbus-1 headers"
	exit 0
fi

T=$(mktemp -d /tmp/.fpkg-portalfc.XXXXXX)
trap 'rm -rf "$T"' EXIT

. "$ROOT/tests/commentlib.sh"
strip_comments "$SRC" > "$T/portal.c"

: > "$T/fc.inc"
for sig in '^static bool dict_lookup\(' '^static bool variant_is\(' \
	   '^static void fc_parse_filters\(' '^static bool fc_parse_options\('; do
	if ! carve_function "$sig" "$T/portal.c" >> "$T/fc.inc"; then
		echo "FAIL: cannot carve $sig out of $SRC"
		exit 1
	fi
done

if grep -q 'dbus_message_iter_get_basic(&v,' "$T/fc.inc"; then
	echo "FAIL: a dict VALUE iterator is passed to get_basic(); the value is"
	echo "      a variant and the read has to go through variant_is()"
	exit 1
fi

try() {
	if ! cc -O0 -Wall -Wextra -I"$T" $dbus_cflags -o "$T/fc" \
		"$ROOT/tests/portal-fc.c" -L"$ROOT/build" -lflatpkg_shared \
		-Wl,-rpath,"$ROOT/build" $dbus_libs 2>"$T/cc.log"; then
		echo "FAIL: the FileChooser harness does not build:"
		head -8 "$T/cc.log" | sed 's/^/       /'
		return 1
	fi
	"$T/fc"
}

try

# --- the plants: each rule must fire on the tree it was written for --------
# 1. read the variant itself, as the code did
sed 's/dbus_message_iter_get_basic(&b, multiple);/dbus_message_iter_get_basic(\&v, multiple);/' \
	"$T/fc.inc" > "$T/plant.inc"
if ! grep -q 'get_basic(&v, multiple)' "$T/plant.inc"; then
	echo "FAIL: the plant was not applied"
	exit 1
fi
mv "$T/fc.inc" "$T/fc.real"
cp "$T/plant.inc" "$T/fc.inc"
if try >/dev/null 2>&1; then
	echo "FAIL: the guard passes when the variant itself is read"
	exit 1
fi
cp "$T/fc.real" "$T/fc.inc"

# 2. the filter rule's fields read without next(), as the code did
sed 's/if (!dbus_message_iter_next(&rule) ||/if (0 ||/' "$T/fc.inc" \
	> "$T/plant2.inc"
if cmp -s "$T/fc.inc" "$T/plant2.inc"; then
	echo "FAIL: the second plant was not applied"
	exit 1
fi
cp "$T/plant2.inc" "$T/fc.inc"
if try >/dev/null 2>&1; then
	echo "FAIL: the guard passes when a filter field is read without next()"
	exit 1
fi
cp "$T/fc.real" "$T/fc.inc"

echo "PASS: FileChooser reads typed values, and a container cannot abort it"
