#!/bin/sh
# x11-fallback.sh - an app that asks for wayland AND fallback-x11 must get
# X11 when the host has no Wayland display.  A fresh MATE session on X11 (the
# bhyve test VM) has no wayland-0 socket; the launcher used to grant the app
# WAYLAND_DISPLAY anyway and leave DISPLAY unset, so GTK died with
# "Failed to open display" before the window ever appeared.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN="$ROOT/build/flatpkg"

. "$ROOT/tests/commentlib.sh"

[ -x "$BIN" ] || { echo "SKIP: build/flatpkg is not built"; exit 0; }
[ -n "$DISPLAY" ] || { echo "SKIP: no X11 display to fall back to"; exit 0; }
[ -n "$XDG_RUNTIME_DIR" ] || { echo "SKIP: no XDG_RUNTIME_DIR"; exit 0; }
"$BIN" complete apps 2>/dev/null | grep -qx 'io.github.kolunmi.Bazaar' ||
	{ echo "SKIP: Bazaar (fallback-x11 + wayland) is not installed"; exit 0; }

# A runtime dir with no wayland-* socket stands in for the X11 session.
T=$(mktemp -d /tmp/.fpkg-x11fb.XXXXXX)
trap 'rm -rf "$T"' EXIT INT TERM

# The jail's hostname is the app id, so a filtered Xauthority record still
# has to be rewritten to that address; filtering alone leaves the host's
# hostname and Xlib in the jail cannot match it.
strip_comments "$ROOT/src/daemon/sandbox_x11.c" > "$T/root.c"
carve_function '^bool sandbox_xauth_write_filtered\(' "$T/root.c" > "$T/xauth.inc" || {
	echo "FAIL: sandbox_xauth_write_filtered() not found"
	exit 1
}
check_xauth() {
	grep -q 'recs\[i\]\.rewrite' "$1" || return 1
	grep -q 'strcmp(addr, app_id) != 0' "$1" || return 1
	grep -q 'write_all(out, app_id, alen)' "$1" || return 1
	return 0
}
check_xauth "$T/xauth.inc" ||
	{ echo "FAIL: the Xauthority address is not rewritten to the app id"; exit 1; }
sed 's/recs\[nrecs\]\.rewrite = keep && strcmp(addr, app_id) != 0;/recs[nrecs].rewrite = false;/' \
	"$T/xauth.inc" > "$T/plant-xauth.inc"
grep -q 'rewrite = false' "$T/plant-xauth.inc" ||
	{ echo "FAIL: the Xauthority plant was not applied"; exit 1; }
if (check_xauth "$T/plant-xauth.inc") >/dev/null 2>&1; then
	echo "FAIL: the guard passes without the Xauthority rewrite"
	exit 1
fi

out=$(env -u WAYLAND_DISPLAY XDG_RUNTIME_DIR="$T" timeout 60 "$BIN" run \
	--command=env io.github.kolunmi.Bazaar 2>&1) || {
	echo "FAIL: the app did not run"
	printf '%s\n' "$out" | tail -8
	exit 1
}
printf '%s\n' "$out" | grep -q '^DISPLAY=' ||
	{ echo "FAIL: the sandbox has no DISPLAY"; exit 1; }
printf '%s\n' "$out" | grep -q '^WAYLAND_DISPLAY=' &&
	{ echo "FAIL: the sandbox was promised a Wayland display"; exit 1; }
printf '%s\n' "$out" | grep -q '^TMPDIR=/var/tmp$' ||
	{ echo "FAIL: the sandbox temp dir is not /var/tmp"; exit 1; }
# The jail's /var/tmp must be the app's OWN cache/tmp, not the host's
# directory: the host one would let two apps write over each other.
grep -q '{"cache/tmp", "var/tmp"}' "$ROOT/src/daemon/sandbox_root.c" ||
	{ echo "FAIL: the jail's /var/tmp is not the app's own cache/tmp"; exit 1; }
grep -q 'm_nullfs(sb, from, to, false)' "$ROOT/src/daemon/sandbox_root.c" ||
	{ echo "FAIL: the app's cache/tmp is not mounted writable"; exit 1; }
# The host's own Xauthority is not in one place (Hyprland's Xwayland uses a
# private one): check the staging only where the caller's ~/.Xauthority is
# what the cookie came from.
if [ -f "$HOME/.Xauthority" ]; then
	printf '%s\n' "$out" | grep -q '^XAUTHORITY=/run/flatpak/Xauthority$' ||
		{ echo "FAIL: the sandbox Xauthority was not staged"; exit 1; }
fi

# WAYLAND_DISPLAY becomes a path segment root unlinks and re-links when it
# stages the socket, so it has to be one component.
strip_comments "$ROOT/src/shared/dbus.c" > "$T/dbus.c"
strip_comments "$ROOT/src/shared/util.c" > "$T/util.c"
carve_function '^char\* fp_wayland_display_name\(' "$T/dbus.c" > "$T/wl.inc" ||
	{ echo "FAIL: fp_wayland_display_name() not found"; exit 1; }
carve_function '^bool fpkg_component_ok\(' "$T/util.c" > "$T/ck.inc" ||
	{ echo "FAIL: fpkg_component_ok() not found"; exit 1; }
cat > "$T/wlprobe.c" <<'PROBE'
#include <stdbool.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
static const char* g_wl;
static const char* cli_getenv(const char* n) {
	return strcmp(n, "WAYLAND_DISPLAY") == 0 ? g_wl : NULL;
}
static char* xstrdup(const char* s) { return strdup(s); }
PROBE
cat "$T/ck.inc" >> "$T/wlprobe.c"
cat "$T/wl.inc" >> "$T/wlprobe.c"
cat >> "$T/wlprobe.c" <<'PROBE'
int main(void) {
	char* n;

	g_wl = "wayland-0";
	n = fp_wayland_display_name(NULL);
	if (n == NULL || strcmp(n, "wayland-0") != 0) return 1;
	free(n);
	g_wl = "../../../../etc/x";
	if (fp_wayland_display_name(NULL) != NULL) return 2;
	g_wl = "a/b";
	if (fp_wayland_display_name(NULL) != NULL) return 3;
	return 0;
}
PROBE
cc -std=gnu11 -Wall -Wextra -Werror -o "$T/wlprobe" "$T/wlprobe.c" \
	> "$T/wlprobe.log" 2>&1 ||
	{ echo "FAIL: the Wayland probe did not build: $(tail -2 "$T/wlprobe.log")"; exit 1; }
"$T/wlprobe" ||
	{ echo "FAIL: the Wayland display name is not one component"; exit 1; }
sed 's/fpkg_component_ok(wl)/(1)/' "$T/wlprobe.c" > "$T/wlprobe-plant.c"
cmp -s "$T/wlprobe.c" "$T/wlprobe-plant.c" &&
	{ echo "FAIL: the Wayland plant was not applied"; exit 1; }
cc -std=gnu11 -o "$T/wlprobe-plant" "$T/wlprobe-plant.c" >/dev/null 2>&1 &&
	"$T/wlprobe-plant" &&
	{ echo "FAIL: the Wayland probe passes without the grammar"; exit 1; }

echo "PASS: fallback-x11 gives an app on an X11-only host DISPLAY, does not"
echo "      invent a Wayland display, and puts the Xauthority record on the"
echo "      jail's hostname"
