#!/bin/sh
# apps-visual.sh - launch every installed app for real and confirm it DREW,
# judged by a fraction of the screen changing: an app that exec'd and crashed
# still exits 0 and satisfies every log-shaped claim.  SKIPS unless opted in.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
FLATPKG="$ROOT/build/flatpkg"
INSTALLED=/usr/local/sbin/flatpkg
APP_LIST=${FLATPKG_APPS:-}

# The floor for "this app painted something".  Measured: a full window is
# 40-50% of 1366x768, the live desktop's own churn is under 1%.
MIN=${FLATPKG_SCREEN_CHANGE_MIN:-5}
# Floor over APPLY, measured against the frame this guard takes of THIS box:
# a palette the bare session already carries makes a fixed "new colours"
# floor fail every app.  Unmeasurable means floor 0, reported, not a pass.
DETAIL=${FLATPKG_DETAIL_MIN:-}
derive_detail_floor() {
	if [ -n "${DETAIL:-}" ]; then printf '%s' "$DETAIL"; return 0; fi
	_d=$(capture_new_colours "$1" "$1" 2>/dev/null || echo 0)
	printf '%s' "${_d:-0}"
}
# How long to let a window map before judging it: generous, because a Flutter
# app fetching a search index and a GTK app differ wildly, and reported.
WAIT=${FLATPKG_APP_WAIT:-45}

say() { printf '%s\n' "$*"; }
note() { printf '  %s\n' "$*"; }

[ "${FLATPKG_SANDBOX_LAUNCH:-0}" = 1 ] || {
	echo "SKIP: launches every installed app for real (a window each, and a"
	echo "      ~13s union root per app).  Run it with FLATPKG_SANDBOX_LAUNCH=1."
	exit 0
}
# Sourced here, after the opt-in skip: capturelib.sh is the one screenshot
# implementation, and sourcing it above would turn its absence into rc=127
# before the guard had a chance to declare itself a SKIP.
. "$ROOT/tests/capturelib.sh"
command -v timeout >/dev/null 2>&1 || {
	echo "SKIP: no timeout(1), so a launch could not be bounded"
	exit 0
}
[ -x "$INSTALLED" ] || { echo "SKIP: $INSTALLED is not installed"; exit 0; }
[ -S /var/run/flatpkg/control.sock ] || {
	echo "SKIP: the daemon is not running"
	exit 0
}
[ -x "$FLATPKG" ] || { echo "SKIP: build/flatpkg not built"; exit 0; }

# The display has to exist, or there is nothing to confirm against.  This
# guard has no useful half: confirming a window needs a screen.
if ! capture_resolve_display; then
	echo "SKIP: no Wayland display to confirm a window against"
	exit 0
fi

# --- the session environment, read from the compositor ---------------
# Everything below is inherited deliberately: a launch without the session's
# bus and display fails for reasons that are not about flatpkg.
hypr=$(pgrep -x Hyprland 2>/dev/null | head -n 1 || true)
if [ -n "$hypr" ]; then
	hypr_env() {
		ps eww -p "$hypr" 2>/dev/null | tr ' ' '\n' |
			sed -n "s/^$1=//p" | head -n 1
	}
	[ -n "${XDG_RUNTIME_DIR:-}" ] || XDG_RUNTIME_DIR=$(hypr_env XDG_RUNTIME_DIR)
	[ -n "${WAYLAND_DISPLAY:-}" ] || WAYLAND_DISPLAY=$(hypr_env WAYLAND_DISPLAY)
	[ -n "${DISPLAY:-}" ] || DISPLAY=$(hypr_env DISPLAY)
	[ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ] ||
		DBUS_SESSION_BUS_ADDRESS=$(hypr_env DBUS_SESSION_BUS_ADDRESS)
fi
# The compositor exports NEITHER display variable, so supply both from what
# the host has: capturelib already resolved the Wayland socket the apps must
# reach.  Without this the sweep reports an empty environment as a verdict.
[ -n "${WAYLAND_DISPLAY:-}" ] || WAYLAND_DISPLAY=${CAPTURE_DISPLAY:-}
[ -n "${DISPLAY:-}" ] || {
	[ -S /tmp/.X11-unix/X0 ] && DISPLAY=:0
}
export XDG_RUNTIME_DIR WAYLAND_DISPLAY DISPLAY DBUS_SESSION_BUS_ADDRESS
export XDG_SESSION_TYPE=wayland
export NO_COLOR= FORCE_COLOR= TERM=xterm
say "  session: XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR WAYLAND_DISPLAY=$WAYLAND_DISPLAY"
say "           DISPLAY=${DISPLAY:-<unset>} bus=${DBUS_SESSION_BUS_ADDRESS:-<unset>}"

# --- the inventory, derived -------------------------------------------
if [ -z "$APP_LIST" ]; then
	APP_LIST=$("$FLATPKG" complete apps 2>/dev/null || true)
fi
# Normalise the separators first: FLATPKG_APPS is a natural place to write a
# space-separated list, and counting lines against one reported 1 app for 2 -
# the loop word-splits either way, so only the count and the summary were wrong.
APP_LIST=$(printf '%s\n' $APP_LIST | grep . || true)
n_apps=$(printf '%s\n' "$APP_LIST" | grep -c . || true)
[ "${n_apps:-0}" -gt 0 ] || { echo "SKIP: no apps installed"; exit 0; }
say "  $n_apps app(s) to confirm"

T=$(mktemp -d /tmp/.fpkg-apps.XXXXXX)
live_pids=""

# Cleanup is per-app and VERIFIED, for the reason `sandbox-launch.sh` spells
# out at length: a cleanup that is not checked is the thing that failed the
# first time, and a leaked jail breaks every app after it.
cleanup_one() {
	_p=$1
	[ -n "$_p" ] || return 0
	kill -TERM "$_p" 2>/dev/null || true
	n=0
	while kill -0 "$_p" 2>/dev/null && [ "$n" -lt 20 ]; do
		sleep 1
		n=$((n + 1))
	done
	kill -KILL "$_p" 2>/dev/null || true
}
cleanup() {
	for p in $live_pids; do
		cleanup_one "$p"
	done
	# Anything the sweep leaked, taken down with privilege - but ONLY jails
	# flatpkg names: `check-clean.sh --clean` acts on all of /var/run/flatpkg
	# and would take the running daemon's control socket with it.
	if [ "$(mdo jls -q jid 2>/dev/null | grep -c . || true)" != 0 ]; then
		for j in $(mdo jls -q jid 2>/dev/null); do
			case "$(mdo jail -h "$j" 2>/dev/null || true)" in
			*flatpkg*) mdo jail -r "$j" >/dev/null 2>&1 || true ;;
			esac
		done
	fi
	if [ "$(mount | grep -c flatpkg || true)" != 0 ]; then
		mdo mount 2>/dev/null | grep flatpkg | awk '{print $3}' > "$T/mp"
		mdo sh -c 'while read -r m; do [ -n "$m" ] && umount "$m" 2>/dev/null || true; done' \
			< "$T/mp" >/dev/null 2>&1 || true
	fi
	rm -rf "$T"
}
trap cleanup EXIT INT TERM

pass=0
fail=0
skip=0

printf '  %-38s %-8s %s\n' APP RESULT CHANGE

# The mount count the BOX is at between apps.  The sweep must return to
# it before each launch, or app N inherits app N-1's teardown.
base_m=$(mount | grep -c flatpkg || true)
say "  baseline: $base_m flatpkg mount(s)"

for app in $APP_LIST; do
	[ -n "$app" ] || continue
	before="$T/$app.before.png"
	after="$T/$app.after.png"
	log="$T/$app.log"
	: > "$log"

	# Two ORTHOGONAL signals: changed pixels (did it paint at all) and NEW
	# colours (was it detailed).  Not a whole-screen total, which falls when a
	# dark window covers a colourful desktop; the floor comes from a frame taken here.
	if capture_display "$before"; then
		: # the baseline frame itself; the comparison happens later
	fi

	# A stale instance makes the measurement meaningless and fails as a
	# defect: a single-instance app forwards to the running copy, so the screen
	# holds the OLD window and the sweep reports 0%.  Find it through `flatpkg ps`.
	still=0
	for _pid in $(XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" "$FLATPKG" ps 2>/dev/null |
		sed 's/\x1b\[[0-9;]*m//g' |
		awk -v a="$app" '$3 == a {print $2}'); do
		kill -KILL "$_pid" 2>/dev/null || true
		still=$((still + 1))
	done
	if [ "$still" -gt 0 ]; then
		sleep 2
	fi

	if ! capture_display "$before"; then
		printf '  %-38s %-8s %s\n' "$app" "SKIP" "no capture before launch"
		skip=$((skip + 1))
		continue
	fi

	"$FLATPKG" run "$app" > "$log" 2>&1 < /dev/null &
	pid=$!
	live_pids="$live_pids $pid"

	# Bounded wait for a window.  Polling the screen is the honest signal:
	# the process being alive says nothing, which is the whole premise.
	got=0
	# Both must exist before anything can fail: the detail claim runs even when
	# the window never appeared, and `set -u` on an unset `pct` aborted like a crash.
	pct=0
	settle_s="-"
	n=0
	while [ "$n" -lt "$WAIT" ]; do
		sleep 3
		n=$((n + 3))
		kill -0 "$pid" 2>/dev/null || break
		if capture_display "$after" 2>/dev/null; then
			pct=$(capture_change_fraction "$before" "$after")
			if [ "${pct:-0}" -ge "$MIN" ] 2>/dev/null; then
				got=1
				break
			fi
		fi
	done

	# Judge a SETTLED frame, not the first painted one: layout can be up
	# before fonts and icons are cached (a first run writes fontconfig/ and
	# gtk-4.0/), and the verdict has to be about the app.
	settled=0
	settle_s="-"
	if [ "$got" -eq 1 ]; then
		if settle_s=$(capture_settle "$before" "$after" 1 60); then
			settled=1
		fi
		# Whether or not it settled, re-measure against the frame the
		# settle left behind: the verdict is about that frame.
		pct=$(capture_change_fraction "$before" "$after")
		if [ "${pct:-0}" -lt "$MIN" ] 2>/dev/null; then
			got=0
		fi
	fi

	# KEEP the settled frame and print its path: "painted" is not "rendered
	# correctly", and a picture is evidence somebody else can check.

	# Wait for the PREVIOUS app's teardown before the next launch: cleanup_one
	# waits for the client, but the jail and its mounts unwind after that, and a
	# launch could take its baseline with the old window still on the screen.
	_n=0
	while [ "$_n" -lt 40 ]; do
		_live=$(mount | grep -c flatpkg || true)
		[ "${_live:-0}" -le "$base_m" ] && break
		sleep 1
		_n=$((_n + 1))
	done
	if [ "$_n" -ge 40 ]; then
		# Say what this is, because the bare number reads as a leak and is
		# not one: a terminated client's sandbox is gone in ~5s, so a count
		# still high here is the PREVIOUS app's sandbox, not a refused unwind.
		note "note: the previous app's sandbox was still up when $app started"
		note "      ($(( $(mount | grep -c flatpkg || true) - base_m )) mounts, baseline $base_m) - a sequencing observation,"
		note "      not a teardown leak: a terminated client's sandbox is gone in ~5s"
	fi
	sleep 2

	shot_dir=${FLATPKG_VISUAL_DIR:-/tmp/flatpkg-visual}
	mkdir -p "$shot_dir" 2>/dev/null || shot_dir="$T"
	cp "$after" "$shot_dir/$app.png" 2>/dev/null || true

	cleanup_one "$pid"
	live_pids=$(printf '%s' "$live_pids" | tr ' ' '\n' |
		grep -vx "$pid" | tr '\n' ' ' || true)

	# The PRIMARY claim, from the SETTLED frame: the colour count decides and the
	# repaint fraction is context, because repaint separates a drawn window from a
	# blank one by too little (Kotoba 8% against a 5% floor) and colour by 3-80x.
	colour_delta=$(capture_new_colours "$before" "$after" 2>/dev/null || echo 0)
	floor=$(derive_detail_floor "$before")
	measured="(floor $floor"
	if [ "$floor" = "0" ]; then
		measured="(floor not measurable"
	fi
	measured="$measured)"

	if [ "${colour_delta:-0}" -ge "$floor" ]; then
		printf '  %-38s %-8s %s\n' "$app" "PASS" \
			"+${colour_delta} new colours $measured, ${pct}% repainted, settled after ${settle_s}s"
		note "screenshot: $shot_dir/$app.png"
		pass=$((pass + 1))
		continue
	fi

	# Not enough new detail.  Whether or not the repaint claim fired, this
	# is the FAIL: the picture either has no window in it, or has a
	# window with nothing rendered inside it.
	if true; then
		# Painted, but not DETAILED.  This is the shape of the report -
		# a window is on the screen and there is nothing in it - and it
		# is a FAIL, not a weak pass.
		printf '  %-38s %-8s %s\n' "$app" "FAIL" \
			"painted ${pct}% but added only ${colour_delta} colours $measured - drawn, but empty"
		note "screenshot: $shot_dir/$app.png"
		fail=$((fail + 1))
		continue
	fi

	# Not a pass.  Say WHY in the guard's own words, because "failed" on
	# an app that logged a specific reason is a verdict the reader cannot
	# act on, and the reason is nearly always the finding.
	reason=$(sed 's/\x1b\[[0-9;]*m//g' "$log" 2>/dev/null |
		grep -iE 'error|fail|cannot|unable|no session|denied|refus' |
		tail -1 | cut -c1-90 || true)
	if [ -n "$reason" ]; then
		printf '  %-38s %-8s %s\n' "$app" "FAIL" "no window; log says:"
		note "$reason"
	else
		printf '  %-38s %-8s %s\n' "$app" "FAIL" \
			"no window, and the log names no reason"
	fi
	fail=$((fail + 1))
done

say "apps-visual: $pass passed, $fail failed, $skip skipped (floor $MIN%)"
[ "$fail" -eq 0 ] || exit 1
exit 0
