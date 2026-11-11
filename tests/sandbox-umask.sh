#!/bin/sh
# sandbox-umask.sh - the app inherits a sane umask.  sandbox_exec() used to
# start every app with umask(0), so a file the app created on a writable host
# mount landed 0666/0777 whatever the caller's own umask was.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FLATPKG="$ROOT/build/flatpkg"
[ -x "$FLATPKG" ] || { echo "SKIP: build/flatpkg not built"; exit 0; }
id -Gn | tr ' ' '\n' | grep -qx flatpkg ||
	{ echo "SKIP: not in the flatpkg group; the root worker is the subject"; exit 0; }
[ -S /var/run/flatpkg/control.sock ] ||
	{ echo "SKIP: no control socket; the daemon is not running"; exit 0; }

T=$(mktemp -d /tmp/.fpkg-umask.XXXXXX)
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

app=$("$FLATPKG" complete apps 2>/dev/null | head -1)
[ -n "$app" ] || { echo "SKIP: no app is installed to borrow a runtime"; exit 0; }

out=$("$FLATPKG" run --command=/bin/sh "$app" -c 'umask' 2>/dev/null || true)
case "$out" in
"")
	echo "FAIL: the sandbox did not report its umask"
	exit 1
	;;
esac
mask=$(printf '%s\n' "$out" | sed -n '$p' | tr -d ' \t\r')
if [ "$mask" = "0000" ]; then
	echo "FAIL: the app starts with umask 0, so host-mount files land"
	echo "      0666/0777 regardless of the caller's umask"
	exit 1
fi
case "$mask" in
[0-7][0-7][0-7][0-7]) ;;
*)
	echo "FAIL: the sandbox printed '$mask', which is not a umask"
	exit 1
	;;
esac
echo "PASS: the app starts with umask $mask, not 0"
