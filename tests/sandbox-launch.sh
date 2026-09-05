#!/bin/sh
# sandbox-launch.sh - the ONE guard that observes a real launch, and it skips
# unless you ask for it: FLATPKG_SANDBOX_LAUNCH=1.  It puts a window on the
# screen and takes ~13s, so it is not in the default suite run.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# The sanctioned app.  Named, not discovered: a guard that picks whatever
# is installed is a guard whose subject changes between boxes.
APP=com.github.tchx84.Flatseal

# Skipped, not passed, and the reason is printed so the skip is legible
# in the suite's own summary.
[ "${FLATPKG_SANDBOX_LAUNCH:-0}" = 1 ] || {
	echo "SKIP: launches $APP for real (a window, and a ~13s union root)."
	echo "      Run it with FLATPKG_SANDBOX_LAUNCH=1."
	exit 0
}
# capturelib.sh is SOURCED, never run: the one implementation of "take a
# screenshot of this session", shared with visual-capture.sh so the two cannot
# disagree about what a capture is.  Sourced AFTER the opt-in skip.
. "$ROOT/tests/capturelib.sh"
command -v timeout >/dev/null 2>&1 || {
	echo "SKIP: no timeout(1), so the launch could not be bounded"
	exit 0
}
# **This exercises the INSTALLED binary, through the daemon.**  The worker that
# assembles the root and logs the phases is the daemon's installed copy, so to
# see a source change: `make install` and restart the daemon first.
INSTALLED=/usr/local/sbin/flatpkg
[ -x "$INSTALLED" ] || { echo "SKIP: $INSTALLED is not installed"; exit 0; }
[ -S /var/run/flatpkg/control.sock ] || { echo "SKIP: the daemon is not running"; exit 0; }

FLATPKG="$ROOT/build/flatpkg"
[ -x "$FLATPKG" ] || { echo "SKIP: build/flatpkg not built"; exit 0; }
"$FLATPKG" complete apps 2>/dev/null | grep -qx "$APP" || {
	echo "SKIP: $APP is not installed on this box"
	exit 0
}
# The jail and mount state are the subject, so refuse to run blind.
mdo jls >/dev/null 2>&1 || { echo "SKIP: cannot read the jail list"; exit 0; }
mount >/dev/null 2>&1 || { echo "SKIP: cannot read the mount table"; exit 0; }

fail=0
note() { echo "FAIL: $*"; fail=1; }

T=$(mktemp -d /tmp/.fpkg-launch.XXXXXX)
pid=""
log="$T/launch.log"

cleanup() {
	if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
		kill -TERM "$pid" 2>/dev/null || true
		# Bounded: the teardown is supposed to be prompt, and a wait
		# that hangs is worse than one that gives up and says so.
		n=0
		while kill -0 "$pid" 2>/dev/null && [ "$n" -lt 20 ]; do
			sleep 1
			n=$((n + 1))
		done
		kill -KILL "$pid" 2>/dev/null || true
	fi
	# If the kernel still thinks the sandbox is there, say so and REMOVE IT,
	# then check: `check-clean.sh --clean` needs root and this trap runs as the
	# invoking user, so its `jail -r` failed into /dev/null.
	if [ "$(mdo jls -q jid 2>/dev/null | grep -c . || true)" != 0 ]; then
		echo "note: jails survived the launch; removing them"
		for j in $(mdo jls -q jid 2>/dev/null); do
			mdo jail -r "$j" >/dev/null 2>&1 || true
		done
	fi
	if [ "$(mount | grep -c flatpkg || true)" != 0 ]; then
		echo "note: flatpkg mounts survived the launch; unmounting them"
		mdo sh "$ROOT/tests/check-clean.sh" --clean >/dev/null 2>&1 || true
	fi
	left_j=$(mdo jls -q jid 2>/dev/null | grep -c . || true)
	left_m=$(mount | grep -c flatpkg || true)
	if [ "${left_j:-0}" != 0 ] || [ "${left_m:-0}" != 0 ]; then
		echo "note: CLEANUP INCOMPLETE - $left_j jail(s) and $left_m"
		echo "      flatpkg mount(s) remain.  Remove them with:"
		echo "        mdo jail -r \$(mdo jls -q jid | head -1)"
		echo "        mdo sh $ROOT/tests/check-clean.sh --clean"
	fi
	rm -rf "$T"
}
trap cleanup EXIT INT TERM

flat_mounts() { mount | grep -c flatpkg || true; }
live_jails() { mdo jls 2>/dev/null | tail -n +2 | grep -c . || true; }

# The baseline the claim is about.  Non-zero here is not this guard's
# business to fix, but it must not be mistaken for the mount the launch
# made.
base_m=$(flat_mounts)
base_j=$(live_jails)
echo "  baseline: $base_m flatpkg mount(s), $base_j live jail(s)"

# DISPLAY if there is one, so the app is not failing on the display before it has
# shown that the sandbox works.  Optional: the root is built long before that.
[ -S /tmp/.X11-unix/X0 ] && DISPLAY=:0
export DISPLAY 2>/dev/null || true
export NO_COLOR= FORCE_COLOR= TERM=xterm
# **The other six names are deliberately NOT pinned**, the second declared
# exception in `tests/worker-env.sh`: a fake HOME would not be the launch.
# ---- 0. a "before" capture, taken while nothing of ours is running ----
# Taken HERE, so "the screen changed" cannot be the session's own doing.
have_display=0
if capture_resolve_display; then
	if capture_display "$T/before.png"; then
		have_display=1
		echo "  captured the screen before the launch"
	else
		echo "note: no screenshot before the launch; the screen claim is skipped"
	fi
else
	echo "note: no Wayland display; the screen claim is skipped"
fi

"$FLATPKG" run "$APP" > "$log" 2>&1 < /dev/null &
pid=$!

# Wait for the jail, which is the thing that proves the root was built
# and entered - the phases may already have scrolled by.
n=0
while [ "$n" -lt 60 ]; do
	[ "$(live_jails)" -gt "$base_j" ] && break
	kill -0 "$pid" 2>/dev/null || break
	sleep 1
	n=$((n + 1))
done

# The jail exists before the worker execs the app, so wait for the
# phase itself; otherwise a fast check races the line it is looking for.
n=0
while [ "$n" -lt 30 ]; do
	[ -f "$log" ] && sed 's/\x1b\[[0-9;]*m//g' "$log" | \
		grep -q "executing $APP in jail" && break
	kill -0 "$pid" 2>/dev/null || break
	sleep 1
	n=$((n + 1))
done

# ---- 1. the three phases, read from the log -------------------------
flat() {
	# Tolerant on purpose: a client that never started leaves NO log, and the
	# old `|| cat` let `set -e` end the guard on the first missing phase
	# instead of reporting all of them.
	if [ -f "$1" ]; then
		sed 's/\x1b\[[0-9;]*m//g' "$1" 2>/dev/null || true
	fi
}
# The right-edge badge overwrites the tail of a line, so compare against
# the text as drawn, not against a width-dependent rendering.
phases=$(flat "$log")
for want in "assembling sandbox root" "creating jail" "executing $APP in jail"; do
	case "$phases" in
	*"$want"*) : ;;
	*) note "the launch never logged [$want]" ;;
	esac
done
case "$phases" in
*"sandbox root ready"*) : ;;
*) note "the launch never logged [sandbox root ready] - the root did not finish" ;;
esac

# ---- 2. a jail, with the app's id, while the app runs ---------------
j=$(live_jails)
if [ "$j" -le "$base_j" ]; then
	note "no jail exists while the app is running ($j live, baseline $base_j)"
else
	hosts=$(mdo jls 2>/dev/null | grep -c "$APP" || true)
	[ "${hosts:-0}" -ge 1 ] ||
		note "a jail exists but none is named for $APP"
	echo "  a jail is live: $(mdo jls 2>/dev/null | tail -n +2 | tr -s ' ' | tr '\n' ';')"
fi

# ---- 3. the union root is really mounted -----------------------------
m=$(flat_mounts)
[ "$m" -gt "$base_m" ] ||
	note "no flatpkg mounts while the app is running ($m, baseline $base_m)"
echo "  flatpkg mounts while running: $m (baseline $base_m)"

# ---- 4. the app actually drew something ------------------------------
# Claims 1-3 are all satisfied by an app that exec'd and then crashed.  Asked as a
# FRACTION of the screen, because the live desktop moves it on its own.
SCREEN_CHANGE_MIN=${FLATPKG_SCREEN_CHANGE_MIN:-5}
if [ "$have_display" -eq 1 ]; then
	# Flatseal's window maps several seconds after exec; 3s caught the
	# desktop and called it "drew nothing".
	sleep 12
	if capture_display "$T/after.png"; then
		after_colours=$(capture_colours "$T/after.png")
		if [ "${after_colours:-0}" -ge 2 ] 2>/dev/null; then
			echo "  the screen while running: $after_colours distinct colours"
		else
			note "the capture while the app ran was a black rectangle ($after_colours colour) - the app's window may never have mapped"
		fi
		pct=$(capture_change_fraction "$T/before.png" "$T/after.png")
		echo "  screen changed: ${pct:-0}% (floor ${SCREEN_CHANGE_MIN}%)"
		if [ "${pct:-0}" -ge "$SCREEN_CHANGE_MIN" ] 2>/dev/null; then
			echo "  the app PAINTED: ${pct}% of the screen is not what it was"
		else
			note "only ${pct:-0}% of the screen changed (floor ${SCREEN_CHANGE_MIN}%): the app drew nothing distinguishable from the live desktop"
		fi
	else
		note "could not capture the screen while the app ran"
	fi
fi

# ---- and the teardown, which is the half nothing else can see -------
kill -TERM "$pid" 2>/dev/null || true
n=0
while kill -0 "$pid" 2>/dev/null && [ "$n" -lt 30 ]; do
	sleep 1
	n=$((n + 1))
done
pid=""            # the trap has nothing left to do

m2=$(flat_mounts)
j2=$(live_jails)
[ "$m2" -eq "$base_m" ] ||
	note "after the app exited, $m2 flatpkg mount(s) remain (baseline $base_m) - the teardown leaked"
[ "$j2" -eq "$base_j" ] ||
	note "after the app exited, $j2 jail(s) remain (baseline $base_j) - the jail was not destroyed"

if [ $fail -eq 0 ]; then
	echo "PASS: the root was assembled, the jail created and entered, the app"
	echo "      exec'd, and on exit the mounts and the jail unwound back to the"
	echo "      baseline ($base_m mount(s), $base_j jail(s))"
fi
exit $fail
