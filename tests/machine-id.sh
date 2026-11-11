#!/bin/sh
# machine-id.sh - a sandbox gets a machine-id that GDBus accepts, in both the
# places glib looks.  GDBus refuses to autolaunch a bus without one, and a host
# file holding "" or "uninitialized" used to be copied verbatim, so every glib
# client in the sandbox reported an invalid machine-id.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FLATPKG="$ROOT/build/flatpkg"
[ -x "$FLATPKG" ] || { echo "SKIP: build/flatpkg not built"; exit 0; }
id -Gn | tr ' ' '\n' | grep -qx flatpkg ||
	{ echo "SKIP: not in the flatpkg group; the root worker is the subject"; exit 0; }
[ -S /var/run/flatpkg/control.sock ] ||
	{ echo "SKIP: no control socket; the daemon is not running"; exit 0; }

# The environment the subject reads, owned here rather than inherited: every
# name is PINNED, or the host decides the verdict.
T=$(mktemp -d /tmp/.fpkg-mid.XXXXXX)
# The worker stages into the caller's runtime dir as root, so a launch can
# leave a directory here that this user cannot remove; a cleanup that fails
# turns a passing guard into a failing one.
cleanup() {
	rm -rf "$T" 2>/dev/null || true
	if [ -d "$T" ] && command -v mdo >/dev/null 2>&1; then
		mdo rm -rf "$T" >/dev/null 2>&1 || true
	fi
	return 0
}
trap cleanup EXIT
export HOME="$T"
export XDG_RUNTIME_DIR="$T/run"
export PATH="/usr/bin:/bin"
export TERM=dumb
export XDG_CURRENT_DESKTOP= DBUS_SESSION_BUS_ADDRESS= WAYLAND_DISPLAY=
export NO_COLOR= FORCE_COLOR=
mkdir -p "$XDG_RUNTIME_DIR"

# Any app will do: the files are the sandbox's, not the app's.
app=$("$FLATPKG" complete apps 2>/dev/null | head -1)
[ -n "$app" ] || { echo "SKIP: no app is installed to borrow a runtime"; exit 0; }

fail=0
note() { echo "FAIL: $*"; fail=1; }

# cat is in every runtime, and stdout is only the files.
out=$("$FLATPKG" run --command=/usr/bin/cat "$app" /etc/machine-id \
	/var/lib/dbus/machine-id 2>/dev/null || true)
etc=$(printf '%s\n' "$out" | sed -n 1p)
dbus=$(printf '%s\n' "$out" | sed -n 2p)

case "$etc" in
"") note "the sandbox has no /etc/machine-id; glib cannot autolaunch a bus" ;;
*)
	printf '%s' "$etc" | grep -Eqx '[0-9a-fA-F]{32}' ||
		note "/etc/machine-id is not 32 hex digits: '$etc'"
	;;
esac
case "$dbus" in
"") note "the sandbox has no /var/lib/dbus/machine-id" ;;
*) [ "$dbus" = "$etc" ] ||
	note "the two machine-id files disagree: '$etc' vs '$dbus'" ;;
esac

# And the file ends with a newline: glib accepts 32 or 33 bytes, and printing
# both files in one cat separates them only if each carries one.
if [ "$(printf '%s\n' "$out" | grep -c . || true)" -ne 2 ]; then
	note "the two machine-id files did not print as two lines: '$out'"
fi

if [ "$fail" -eq 0 ]; then
	echo "PASS: the sandbox exposes one valid machine-id at /etc/machine-id"
	echo "      and /var/lib/dbus/machine-id"
fi
exit $fail
