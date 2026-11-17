#!/bin/sh
# user-store-mount.sh - an app that declares xdg-data/flatpak manages the real
# ~/.local/share/flatpak (the same installation `flatpkg --user` uses), not a
# private copy under its data dir.  The store must be visible and writable in
# the sandbox; libflatpak then finds an existing user installation.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${FLATPKG_USER_BIN:-/usr/local/sbin/flatpkg}
APP=io.github.kolunmi.Bazaar

[ -x "$BIN" ] || { echo "SKIP: $BIN is not installed"; exit 0; }
"$BIN" complete apps 2>/dev/null | grep -qx "$APP" ||
	{ echo "SKIP: $APP is not installed"; exit 0; }

fail=0
note() {
	echo "FAIL: $*"
	fail=1
}
store="$HOME/.local/share/flatpak"
probe=".fpkg-guard-$$"
mkdir -p "$store" 2>/dev/null || true
: >"$store/$probe"
trap 'rm -f "$store/$probe"' EXIT INT TERM

out=$(timeout 60 "$BIN" run --command=env "$APP" 2>/dev/null) || true
printf '%s\n' "$out" | grep -qx "FLATPAK_USER_DIR=$store" ||
	note "the sandbox did not get the user installation at $store"

out=$(timeout 90 "$BIN" run --command=sh "$APP" -c \
	'test -f "$FLATPAK_USER_DIR/'"$probe"'" && test -w "$FLATPAK_USER_DIR" && echo shared' \
	2>/dev/null) || true
printf '%s\n' "$out" | grep -qx shared ||
	note "the user installation is not visible and writable in the sandbox"

[ "$fail" -eq 0 ] && echo "PASS: xdg-data/flatpak points at the real user"
echo "      installation and the sandbox sees it read-write"
exit "$fail"
