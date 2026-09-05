#!/bin/sh
# spawn-signal-scope.sh - the portal's SpawnSignal signals only a process its own caller
# spawned, and is refused for any other connection.  SKIPS unless
# FLATPKG_SANDBOX_LAUNCH=1; the recorded pid is the forked client, not the app.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

APP=com.github.tchx84.Flatseal
FLATPKG="$ROOT/build/flatpkg"

[ "${FLATPKG_SANDBOX_LAUNCH:-0}" = 1 ] || {
	echo "SKIP: starts a real sandbox to give the portal a real pid to" \
	    "signal (a ~13s union root, and a jail).  Run it with" \
	    "FLATPKG_SANDBOX_LAUNCH=1."
	exit 0
}
command -v timeout >/dev/null 2>&1 || {
	echo "SKIP: no timeout(1), so the launch could not be bounded"
	exit 0
}
[ -x "$FLATPKG" ] || { echo "SKIP: build/flatpkg not built"; exit 0; }
[ -S /var/run/flatpkg/control.sock ] || {
	echo "SKIP: the daemon is not running (a Spawn forks 'flatpkg run',"
	echo "      which talks to it)"
	exit 0
}
# The proxy is not optional: with none there is no caller the portal will serve,
# and the case would be "the guard could not build its own instrument" wearing a
# pass.
PROXY=""
for cand in /usr/local/bin/xdg-dbus-proxy /usr/bin/xdg-dbus-proxy; do
	[ -x "$cand" ] && { PROXY=$cand; break; }
done
[ -n "$PROXY" ] || {
	echo "SKIP: no xdg-dbus-proxy, so there is no caller the portal"
	echo "      accepts and no pid it would record"
	exit 0
}
DBUS_DAEMON=$(command -v dbus-daemon 2>/dev/null || true)
[ -n "$DBUS_DAEMON" ] || {
	echo "SKIP: no dbus-daemon, so no private session bus"
	exit 0
}
"$FLATPKG" complete apps 2>/dev/null | grep -qx "$APP" || {
	echo "SKIP: $APP is not installed on this box"
	exit 0
}
mdo jls >/dev/null 2>&1 || { echo "SKIP: cannot read the jail list"; exit 0; }

# shellcheck source=tests/commentlib.sh
. "$ROOT/tests/commentlib.sh"
# shellcheck source=tests/probelib.sh
. "$ROOT/tests/probelib.sh"

fail=0
note() { echo "FAIL: $*"; fail=1; }

T=$(mktemp -d /tmp/.fpkg-spawnsig.XXXXXX)
portal=""
bus=""
pxy1=""
pxy2=""
spawned=""
spawn_jid=""

cleanup() {
	# With privilege, and then VERIFY.  **This trap does NOT run `--clean`** on an
	# incomplete cleanup: it acts on `/var/run/flatpkg/`, the daemon's runtime
	# directory.  The jail is removed by id and the leftovers are REPORTED.
	for p in "$spawned" "$pxy1" "$pxy2" "$portal" "$bus"; do
		[ -n "$p" ] && kill -TERM "$p" 2>/dev/null || true
	done
	if [ -n "$spawned" ] && kill -0 "$spawned" 2>/dev/null; then
		kill -KILL "$spawned" 2>/dev/null || true
	fi
	# The app is in the jail and the recorded pid is only its client, so the jail is
	# what has to go - and there is **ONE JAIL PER SPAWN**, and this guard makes two.
	# So the cleanup removes EVERY jail whose hostname is the app's.
	mdo jls > "$T/jls-clean.txt" 2>/dev/null || true
	allj=$(awk -v a="$APP" 'NR > 1 {
			for (i = 1; i <= NF; i++)
				if ($i == a) { print $1; break }
		}' "$T/jls-clean.txt" 2>/dev/null)
	for j in $spawn_jid $allj; do
		[ -n "$j" ] || continue
		mdo jail -r "$j" >/dev/null 2>&1 || true
	done
	# Wait for jails AND mounts: `jail -r` is asynchronous with respect to the union
	# root's mounts, and the teardown is the part that takes the time.  Break when
	# clean - `|| break` on the grep fires when it SUCCEEDS.
	n=0
	while [ "$n" -lt 30 ]; do
		mdo sh "$ROOT/tests/check-clean.sh" > "$T/clean.txt" 2>/dev/null ||
			true
		if grep -q "live jails: 0" "$T/clean.txt" &&
		   grep -q "flatpkg mounts still up: 0" "$T/clean.txt"; then
			break
		fi
		sleep 1
		n=$((n + 1))
	done
	mdo jls -q jid > "$T/jls-left.txt" 2>/dev/null || true
	left_j=$(grep -c . "$T/jls-left.txt" 2>/dev/null || true)
	left_m=$(mount 2>/dev/null | grep -c flatpkg || true)
	if [ "${left_j:-0}" != 0 ] || [ "${left_m:-0}" != 0 ]; then
		echo "note: CLEANUP INCOMPLETE - ${left_j:-0} jail(s) and" \
		    "${left_m:-0} flatpkg mount(s) remain.  Remove them with:"
		echo "        mdo jail -r \$(mdo jls -q jid | head -1)"
		echo "        mdo sh $ROOT/tests/check-clean.sh --clean"
	fi
	rm -rf "$T"
}
trap cleanup EXIT INT TERM

# The probe, with probelib's three outcomes - a build failure and a missing
# prerequisite are different answers and neither is a silent zero.
set +e
PROBE=$(build_probe "$T/probe" 2>&1)
probe_rc=$?
set -e
case $probe_rc in
0) : ;;
1) echo "SKIP: the D-Bus probe would not build"; echo "$PROBE"; exit 0 ;;
2) echo "SKIP: $PROBE"; exit 0 ;;
esac

# Record the jail of every instance this guard caused, called after EACH Spawn and
# appending: a cleanup that knows one of two leaves a jail behind.  The jail must be
# OUR app's, because the pinned XDG_RUNTIME_DIR can hold another launch's instances.
record_jids() {
	# `mdo jls` with NO argument: the per-jid form prints **empty** on this box, and the
	# instance `info` keyfiles are under the worker's runtime dir.  **Through a FILE,
	# not a pipe**: `mdo jls` prints nothing on stdout when it is not a tty.
	mdo jls > "$T/jls.txt" 2>/dev/null || true
	# Match the app id as a whole FIELD and take the jid as the first
	# whitespace-separated token: `cut -d' ' -f1` on a row whose first character is
	# a space yields the EMPTY field, and awk on whitespace does not depend on it.
	found=$(awk -v a="$APP" 'NR > 1 {
			for (i = 1; i <= NF; i++)
				if ($i == a) { print $1; break }
		}' "$T/jls.txt" 2>/dev/null)
	for j in $found; do
		case "$j" in
		'' | *[!0-9]*) continue ;;
		esac
		case " $spawn_jid " in
		*" $j "*) : ;;
		*) spawn_jid="$spawn_jid $j" ;;
		esac
	done
}

# A private bus, and an environment that does not depend on this box's session,
# so the instance directory the worker creates is under $T.
"$DBUS_DAEMON" --session --nofork --print-address=1 >"$T/addr" 2>"$T/dbus.log" &
bus=$!
n=0
while [ $n -lt 40 ]; do
	[ -s "$T/addr" ] && break
	sleep 0.2
	n=$((n + 1))
done
ADDR=$(head -1 "$T/addr" 2>/dev/null || true)
[ -n "$ADDR" ] || { echo "SKIP: dbus-daemon printed no address"; exit 0; }
echo "  private bus: ${ADDR%%,*}"

export DBUS_SESSION_BUS_ADDRESS="$ADDR"
export HOME="$T/home"
export XDG_RUNTIME_DIR="$T/run"
export XDG_CURRENT_DESKTOP=
export WAYLAND_DISPLAY=
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export TERM=dumb
export NO_COLOR= FORCE_COLOR=
mkdir -p "$HOME" "$XDG_RUNTIME_DIR"

"$FLATPKG" portal >"$T/portal.log" 2>&1 &
portal=$!
# Wait for the NAME, not for a pid: a portal that has not acquired
# org.freedesktop.portal.Flatpak answers UnknownObject, and a guard that
# read that as a refusal would be testing a service that is not up yet.
n=0
up=no
while [ $n -lt 60 ]; do
	if "$PROBE" ping org.freedesktop.portal.Flatpak \
	    /org/freedesktop/portal/Flatpak >/dev/null 2>&1; then
		up=yes
		break
	fi
	sleep 0.2
	n=$((n + 1))
done
if [ "$up" != yes ]; then
	echo "FAIL: the portal never answered on the private bus:"
	sed 's/^/        /' "$T/portal.log" | head -5
	fail=1
	echo
fi

# Two proxies, same options, different processes.  `--own=<app id>` on both, because
# `resolve_app_id()` reads the app id out of the proxy's argv and a proxy without
# one is refused for a reason that has nothing to do with the case.
start_proxy() {
	_p="$1"
	"$PROXY" "$ADDR" "$_p" --filter "--own=$APP" \
	    --call=org.freedesktop.portal.*=* >"$T/proxy.log" 2>&1 &
	eval "$2=\$!"
}
start_proxy "$T/p1.sock" pxy1
start_proxy "$T/p2.sock" pxy2
n=0
while [ $n -lt 40 ]; do
	[ -S "$T/p1.sock" ] && [ -S "$T/p2.sock" ] && break
	sleep 0.2
	n=$((n + 1))
done
if [ ! -S "$T/p1.sock" ] || [ ! -S "$T/p2.sock" ]; then
	echo "SKIP: the real xdg-dbus-proxy never created its socket"
	sed 's/^/        /' "$T/proxy.log" 2>/dev/null | head -4
	exit 0
fi

echo
echo "the portal's SpawnSignal allowed direction, at runtime:"

# ONE spawn, two callers, and the order is the design: the refused case is signal 0
# (nothing delivered) and leaves the pid alive for the allowed case to kill.
# `spawn-signal-own` does both calls in ONE probe process on ONE connection: the claim is
# keyed on the caller's unique bus name, and the caller is the PROXY'S UPSTREAM connection.
sout=$(DBUSPROBE_BUS="unix:path=$T/p1.sock" "$PROBE" \
    spawn /bin/sleep 300 2>&1) || true
spawned=$(printf '%s\n' "$sout" |
    sed -n 's/^reply=ok: (uint32 \([0-9][0-9]*\),)$/\1/p' | head -1)
if [ -z "$spawned" ]; then
	note "Spawn from a real proxy did not return a pid: $sout"
	sed 's/^/        /' "$T/portal.log" 2>/dev/null | tail -6
	echo
	echo "FAIL: the portal did not record a pid, so the allowed direction"
	echo "      of SpawnSignal is unproven."
	exit 1
fi
echo "  1. Spawn from one real proxy -> pid $spawned"

# The kernel's own view that a real jail exists, with the app's id as its hostname -
# `sandbox.c` sets `host.hostname` to the app id.  It is the difference between "the
# portal returned a pid" and "a sandbox is up".
mdo jls > "$T/jls-live.txt" 2>/dev/null || true
if grep -q "$APP" "$T/jls-live.txt" 2>/dev/null; then
	echo "     (and a jail is live, with the app id as its hostname)"
	record_jids
	echo "     (jail${spawn_jid:+ }${spawn_jid:-<not found>}, from an" \
	    " instance info keyfile)"
else
	note "the portal returned pid $spawned but no jail is live - nothing"
	note "  was sandboxed, so the pid is not a signal target worth having"
fi

# 2. A DIFFERENT caller, signal 0, and the pid is refused AND survives.
xout=$(DBUSPROBE_BUS="unix:path=$T/p2.sock" "$PROBE" \
    spawn-signal "$spawned" 0 false 2>&1) || true
xrefused=no
case "$xout" in
*"is not a process this caller spawned"*) xrefused=yes ;;
*) note "a DIFFERENT caller was allowed to signal pid $spawned: $xout" ;;
esac
if kill -0 "$spawned" 2>/dev/null; then
	xsurvived=yes
else
	xsurvived=no
	note "the cross-caller case sent signal 0 to a pid the portal had not"
	note "  refused, or the process died on its own - a refusal and a"
	note "  delivery cannot be told apart that way"
fi
# **The line is printed only when BOTH halves held**: printed unconditionally it
# announces the result of a check it just failed, in the same second of output.
if [ "$xrefused" = yes ] && [ "$xsurvived" = yes ]; then
	echo "  2. a different connection was refused pid $spawned, and the"
	echo "     process is still running (signal 0, so nothing was delivered)"
fi

# 3. The caller that spawned it, signal 15, and the process is gone.
#    `spawn-signal-own` does both calls on one connection, which is the
#    only shape the allowed direction can have.
aout=$(DBUSPROBE_BUS="unix:path=$T/p1.sock" "$PROBE" \
    spawn-signal-own 15 false /bin/sleep 300 2>&1) || true
ownpid=$(printf '%s\n' "$aout" | sed -n 's/^pid=\([0-9][0-9]*\)$/\1/p')
# The SECOND Spawn's jail too - the case that leaked one.
record_jids
wait_gone() {
	# WAIT, do not sample: the signal is sent by the portal and the
	# process's death is the worker's to notice, and one `kill -0` taken
	# at once can read a process that has not been reaped yet.
	_g=$1; _n=0
	while [ "$_n" -lt 40 ] && kill -0 "$_g" 2>/dev/null; do
		sleep 0.25
		_n=$((_n + 1))
	done
	! kill -0 "$_g" 2>/dev/null
}
case "$aout" in
*"signal=ok"*) : ;;
*) note "the spawning connection was refused its OWN pid: $aout" ;;
esac
# WAIT, do not sample: one `kill -0` taken at once can read a process that has not
# been reaped yet.
if [ -n "$ownpid" ] && wait_gone "$ownpid"; then
	echo "  3. the spawning connection was allowed, the signal was sent," \
	    "     and its process $ownpid is gone"
elif [ -n "$ownpid" ]; then
	note "SpawnSignal was accepted for the caller's own spawned pid" \
		"$ownpid but the process is still running - the signal was" \
		"permitted and never delivered"
fi

# 4. The GROUP form: `killpg()` treats its argument as a GROUP id, and the spawn
#    child is a group leader only because `sandbox_exec()` puts it there.  Without
#    that it was a silent ESRCH and the caller was told `ok`.
gout=$(DBUSPROBE_BUS="unix:path=$T/p1.sock" "$PROBE" \
    spawn-signal-own 15 true /bin/sleep 300 2>&1) || true
gpid=$(printf '%s\n' "$gout" | sed -n 's/^pid=\([0-9][0-9]*\)$/\1/p')
case "$gout" in
*"signal=ok"*) : ;;
*) note "the GROUP form of SpawnSignal was refused for the spawning" \
     note "  connection: $gout" ;;
esac
if [ -n "$gpid" ] && wait_gone "$gpid"; then
	echo "  4. the GROUP form (to_process_group=true) is delivered too:" \
	    "     its process $gpid is gone"
elif [ -n "$gpid" ]; then
	note "SpawnSignal with to_process_group=true was ANSWERED for pid" \
		"$gpid but the process is still running - the portal told the" \
		"caller the group signal was delivered and it was not"
fi
record_jids

echo
if [ "$fail" -eq 0 ]; then
	echo "PASS: the portal's SpawnSignal signals a process ITS OWN caller"
	echo "      spawned - refused for a different connection with the pid"
	echo "      still alive, allowed for the one that spawned it, and the"
	echo "      process is gone afterwards.  Both directions at runtime, on"
	echo "      a real sandbox, with no structural claim standing in for"
	echo "      either - and the to_process_group form delivered too (the"
	echo "      spawn child is put in its own process group so killpg()"
	echo "      has a group to name, and a failed signal answers with an"
	echo "      error rather than a bare ok)."
fi
exit $fail
