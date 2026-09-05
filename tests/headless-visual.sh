#!/bin/sh
# headless-visual.sh - launch every installed app for real and confirm it drew,
# on an off-screen wlmaker the guard owns (headless backend, pixman renderer)
# rather than the user's session.  A fresh compositor per app.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)

FLOOR=${FLATPKG_HEADLESS_FLOOR:-5}
WAIT=${FLATPKG_HEADLESS_WAIT:-32}
SHOT_DIR=${FLATPKG_VISUAL_DIR:-/tmp/flatpkg-visual}
RT=${FLATPKG_HEADLESS_RUNTIME:-/tmp/flatpkg-headless-rt}

if [ "${FLATPKG_SANDBOX_LAUNCH:-0}" != "1" ]; then
	echo "SKIP: launches every installed app on a private compositor."
	echo "      Run it with FLATPKG_SANDBOX_LAUNCH=1."
	exit 0
fi
# Sourced after the opt-in skip: capturelib.sh is the one screenshot
# implementation, and sourcing it above would turn its absence into a
# failure before this guard had declared itself a SKIP.
. "$ROOT/tests/capturelib.sh"

WLM=$(command -v wlmaker 2>/dev/null || true)
[ -n "$WLM" ] || { echo "SKIP: no wlmaker on PATH"; exit 0; }
XWAYLAND=$(command -v Xwayland 2>/dev/null || true)
[ -n "$XWAYLAND" ] || { echo "SKIP: no Xwayland on PATH"; exit 0; }
[ -n "$(command -v grim 2>/dev/null || true)" ] || { echo "SKIP: no grim"; exit 0; }
GRIM_BIN=$(command -v grim)
command -v flatpkg >/dev/null 2>&1 || { echo "headless-visual: no flatpkg"; exit 1; }

# `FLATPKG_APPS` narrows the sweep to named apps, the same spelling
# `apps-visual.sh` uses - a nine-app run is six minutes and the first
# thing anyone wants is one app.
apps=${FLATPKG_APPS:-$(flatpkg complete apps 2>/dev/null | grep . || true)}
apps=$(printf '%s\n' $apps | grep . || true)
if [ -z "$apps" ]; then
	echo "headless-visual: no installed apps to check"
	exit 0
fi

pass=0
fail=0
mkdir -p "$SHOT_DIR" 2>/dev/null || true

# A session bus is needed (Bazaar's daemon refuses without one, and says so),
# so take the caller's or look for a live one: the private compositor's own
# environment has none, which is the mistake that made Bazaar look broken.
bus=${DBUS_SESSION_BUS_ADDRESS:-}
if [ -z "$bus" ]; then
	for s in /tmp/dbus-*; do
		[ -S "$s" ] || continue
		if DBUS_SESSION_BUS_ADDRESS="unix:path=$s" timeout 5 gdbus call \
		    --session --dest org.freedesktop.DBus \
		    --object-path /org/freedesktop/DBus \
		    --method org.freedesktop.DBus.ListNames >/dev/null 2>&1; then
			bus="unix:path=$s"
			break
		fi
	done
fi
[ -n "$bus" ] || echo "note: no live session bus; bus-dependent apps will fail"

XWAY_PID=
cleanup() {
	pkill -x wlmaker 2>/dev/null
	pkill -f 'flatpkg run' 2>/dev/null
	[ -n "$XWAY_PID" ] && kill "$XWAY_PID" 2>/dev/null
	XWAY_PID=
}
trap cleanup EXIT INT TERM

for app in $apps; do
	cleanup
	sleep 2
	rm -rf "$RT"
	mkdir -p "$RT" && chmod 700 "$RT"

	XDG_RUNTIME_DIR="$RT" WLR_BACKENDS=headless WLR_RENDERER=pixman \
		WLR_LIBINPUT_NO_DEVICES=1 XDG_SESSION_TYPE=wayland \
		"$WLM" >"$SHOT_DIR/$app.compositor.log" 2>&1 &
	sleep 6
	# wlmaker starts a foot terminal at 0,0; it would cover an app window
	# of the same position and make the capture show the terminal.
	pkill -x foot 2>/dev/null || true

	# Xwayland inside the private compositor gives X11-only clients
	# (Bitwig, Electron's X11 path) a server that the capture can see,
	# instead of the user's display or no server at all.
	XDISP=:90
	XDG_RUNTIME_DIR="$RT" WAYLAND_DISPLAY="$RT/wayland-0" \
		"$XWAYLAND" "$XDISP" -noreset \
		>"$SHOT_DIR/$app.xwayland.log" 2>&1 &
	XWAY_PID=$!
	xn=0
	while [ "$xn" -lt 20 ]; do
		[ -S "/tmp/.X11-unix/X${XDISP#:}" ] && break
		sleep 1
		xn=$((xn + 1))
	done

	CAPTURE_RUNTIME_DIR="$RT" CAPTURE_DISPLAY=wayland-0 \
		capture_display "$SHOT_DIR/$app.before.png" || {
		printf '  %-38s %-8s %s\n' "$app" "FAIL" \
			"the private compositor produced no capture"
		fail=$((fail + 1))
		continue
	}

	# The app gets this compositor's Xwayland, not the host's
	# DISPLAY, so an X11 window lands on the surface we capture.
	env XDG_RUNTIME_DIR="$RT" WAYLAND_DISPLAY=wayland-0 DISPLAY="$XDISP" \
		DBUS_SESSION_BUS_ADDRESS="$bus" \
		timeout $((WAIT + 120)) flatpkg run --socket=x11 "$app" \
		>"$SHOT_DIR/$app.launch.log" 2>&1 &
	# Wait for the app to be RUNNING, not for a fixed sleep: Bitwig is a JVM
	# and was still not up when a 32s sleep ended, so the verdict was about
	# the instrument.  A live jail in jls is the daemon's own answer.
	waited=0
	while [ "$waited" -lt "$WAIT" ]; do
		if mdo jls 2>/dev/null | grep -q "$app"; then
			break
		fi
		sleep 2
		waited=$((waited + 2))
	done
	sleep 20         # let the window map and paint once the app is up

	if ! CAPTURE_RUNTIME_DIR="$RT" CAPTURE_DISPLAY=wayland-0 \
	     capture_display "$SHOT_DIR/$app.png"; then
		printf '  %-38s %-8s %s\n' "$app" "FAIL" \
			"no capture while the app was running"
		fail=$((fail + 1))
		continue
	fi

	delta=$(capture_change_fraction "$SHOT_DIR/$app.before.png" \
		"$SHOT_DIR/$app.png" 2>/dev/null || echo 0)

	# "Drew nothing" and "the app is not there" are different findings: a bare
	# "changed 0%" reads as a broken app, and Bitwig's unmapped window on a
	# headless compositor is not broken.  Say which of the two it is.
	running=no
	mdo jls 2>/dev/null | grep -q "$app" && running=yes

	if [ "${delta:-0}" -ge "$FLOOR" ]; then
		printf '  %-38s %-8s %s\n' "$app" "PASS" \
			"+${delta}% repainted on the off-screen compositor"
		pass=$((pass + 1))
	elif [ "$running" = yes ]; then
		printf '  %-38s %-8s %s\n' "$app" "FAIL" \
			"the app is RUNNING but drew nothing visible (+$delta, floor $FLOOR) - see the header: this instrument cannot see an unmapped window"
		fail=$((fail + 1))
	else
		printf '  %-38s %-8s %s\n' "$app" "FAIL" \
			"changed only $delta% (floor $FLOOR%) and the app is not running - it did not draw"
		fail=$((fail + 1))
	fi
done

cleanup
echo "headless-visual: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
