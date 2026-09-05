#!/bin/sh
# update-monitor-scope.sh - CreateUpdateMonitor hands back a real, CALLABLE object path in
# the upstream layout, and two monitors get two paths.  No sandbox needed: unlike Spawn it
# does not refuse a direct caller.  Its Close and Update are unreachable, not asserted.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FLATPKG="$ROOT/build/flatpkg"
tmp=$(mktemp -d /tmp/.fpkg-updatemon.XXXXXX)
fail=0
note() { echo "FAIL: $*"; fail=1; }

# The guard's OWN tools, resolved to absolute paths BEFORE PATH is narrowed below:
# the subject must see a minimal, pinned PATH and the guard must still be able to
# find its instruments.
DBUS_DAEMON=$(command -v dbus-daemon 2>/dev/null || true)
[ -n "$DBUS_DAEMON" ] || { echo "SKIP: no dbus-daemon, so no private session bus"; exit 0; }

# tests/dbusprobe.c, compiled here rather than by the Makefile: it is a test
# instrument and must not be installed.
. "$(dirname "$0")/probelib.sh"
PROBE=""
if ! PROBE=$(build_probe "$tmp/dbusprobe"); then
	case $? in
	2) echo "SKIP: $(echo "$PROBE" | sed 's/^SKIP //')"
	   exit 0 ;;
	*) fail=1 ;;
	esac
fi
[ -x "$FLATPKG" ] || { echo "SKIP: build/flatpkg not built"; exit 0; }

portal=""
bus=""
cleanup() {
	for p in $portal $bus; do
		[ -n "$p" ] || continue
		kill -TERM "$p" 2>/dev/null || true
	done
	n=0
	while [ $n -lt 10 ]; do
		alive=0
		for p in $portal $bus; do
			[ -n "$p" ] || continue
			kill -0 "$p" 2>/dev/null && alive=1
		done
		[ "$alive" -eq 0 ] && break
		sleep 1
		n=$((n + 1))
	done
	for p in $portal $bus; do
		[ -n "$p" ] || continue
		kill -KILL "$p" 2>/dev/null || true
	done
	for p in $portal $bus; do
		[ -n "$p" ] || continue
		if kill -0 "$p" 2>/dev/null; then
			echo "note: CLEANUP INCOMPLETE - pid $p survived the guard."
			echo "        kill -9 $p"
		fi
	done
	rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

# ---- a private bus and a portal on it ---------------------------------
"$DBUS_DAEMON" --session --nofork --print-address=1 >"$tmp/addr" 2>"$tmp/dbus.log" &
bus=$!
n=0
while [ $n -lt 40 ]; do
	[ -s "$tmp/addr" ] && break
	sleep 0.2
	n=$((n + 1))
done
ADDR=$(head -1 "$tmp/addr" 2>/dev/null || true)
if [ -z "${ADDR:-}" ]; then
	echo "SKIP: dbus-daemon printed no address ($(head -2 "$tmp/dbus.log" 2>/dev/null))"
	exit 0
fi
echo "  private bus: ${ADDR%%,*}"

export DBUS_SESSION_BUS_ADDRESS="$ADDR"
# Pinned because worker-env.sh requires it of every guard that runs the binary,
# and because the portal reads XDG_RUNTIME_DIR when it sweeps stale instance dirs.
# Not a declared exception: nothing here needs the real session.
export HOME="$tmp/home"
export XDG_RUNTIME_DIR="$tmp/run"
export XDG_CURRENT_DESKTOP=
export WAYLAND_DISPLAY=
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export TERM=dumb
export NO_COLOR= FORCE_COLOR=
mkdir -p "$HOME" "$XDG_RUNTIME_DIR"

"$FLATPKG" portal >"$tmp/portal.log" 2>&1 &
portal=$!
# Wait for the name, not for a pid: a portal that has not acquired
# org.freedesktop.portal.Flatpak yet answers UnknownObject, which a guard could read
# as a refusal.
n=0
up=no
while [ $n -lt 50 ]; do
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
	sed 's/^/        /' "$tmp/portal.log" | head -5
	fail=1
fi

# ---- 1: the path is the upstream layout, element by element ------------
# stdout ONLY: the probe's stderr is unbuffered while its stdout is a pipe, so with
# 2>&1 a GLib warning lands in the path line and sed keeps only what precedes it.
p1=$("$PROBE" monitor-create 2>"$tmp/probe.err" |
    sed -n 's/^path=//p' | head -1)
if [ -z "$p1" ]; then
	note "CreateUpdateMonitor returned no object path"
else
	# grep -F on a FIXED STRING: the prefix is what pins the layout, since the
	# defect answered with the caller's unique bus name verbatim (":1.42").  The
	# path has a CAPITAL F in `Flatpak`, which is what three earlier patterns missed.
	if ! printf '%s\n' "$p1" |
	    grep -qF "/org/freedesktop/portal/Flatpak/update_monitor/"; then
		note "the monitor path is not under update_monitor/: $p1"
	fi
fi

# ---- 2: the object answers, in ONE process ----------------------------
# Every subcommand is a separate process with its own bus name, and name_owner_filter()
# marks a monitor closed when its sender leaves the bus: a create in one process and a
# close in the next finds it reaped - a test artifact.
life=$("$PROBE" monitor-lifecycle 2>>"$tmp/probe.err") || true
if [ -z "$life" ]; then
	note "monitor-lifecycle produced no output at all"
else
	# Update goes BEFORE Close: Close marks the monitor closed and the main loop
	# reaps it, so an Update after it finds nothing - correct, not a defect.
	for want in 'path=/org/freedesktop/portal/Flatpak/update_monitor/' \
	    'introspect=<node>' 'reply=ok'; do
		case "$life" in
		*"$want"*) : ;;
		*) note "the monitor lifecycle is missing '$want': $life" ;;
		esac
	done
	# Introspect must name all four members: the XML is the contract a client reads.
	# `method name=X`/`signal name=X`, not `name=X`, because `name=Update` also
	# matches `name=UpdateAvailable`.
	for m in Close Update; do
		printf '%s\n' "$life" | grep -q "method name=\"$m\"" ||
			note "the monitor's Introspect does not declare method $m"
	done
	for m in UpdateAvailable Progress; do
		printf '%s\n' "$life" | grep -q "signal name=\"$m\"" ||
			note "the monitor's Introspect does not declare signal $m"
	done
	# The refusal is only visible in the Progress - the reply is `ok` BY CONTRACT - and
	# it is NOT asserted: the portal emits it and the client never receives it.  And
	# Close must take the path down: afterwards the path answers UnknownMethod.
	after=$("$PROBE" monitor-introspect \
	    "$(printf '%s\n' "$life" | sed -n 's/^path=//p' | head -1)" \
	    2>>"$tmp/probe.err") || true
	# Not "it answered" - an unregistered path answers too, with an empty node.  What
	# must be gone is the monitor's own XML.
	if printf '%s\n' "$after" | grep -q 'method name=Close'; then
		note "the monitor's Introspect survived Close: $after"
	fi
fi

# ---- 3: two monitors are two paths ------------------------------------
p2=$("$PROBE" monitor-create 2>>"$tmp/probe.err" |
    sed -n 's/^path=//p' | head -1)
if [ -z "$p2" ]; then
	note "a second CreateUpdateMonitor returned no object path"
elif [ -n "$p1" ] && [ "$p1" = "$p2" ]; then
	# The token is a per-process counter, so a repeated path means the
	# second monitor overwrote the first in the portal's own table -
	# and in any client's.
	note "two CreateUpdateMonitor calls returned the SAME path: $p1"
fi

# ---- plants: two recorded, one of them unassertable --------------------
# Disabling the sub-path forwarding in handle_message() loses every Introspect member.
# Disabling the `app_id == NULL` check in handle_update() is NOT assertable.

echo
if [ "$fail" -eq 0 ]; then
	echo "PASS: the UpdateMonitor is a real object - a callable path in the"
	echo "      upstream layout, Introspect naming all four members, and"
	echo "      Update and Close both answered, in one process."
	exit 0
fi
echo "FAILED: the UpdateMonitor is not what DESIGN.md section 6 claims"
exit 1
