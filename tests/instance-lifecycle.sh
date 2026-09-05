#!/bin/sh
# instance-lifecycle.sh - the daemon's view and control of a running instance:
# ps lists it, enter runs inside its jail, kill removes it, a quick app still
# returns, and ps's bytes are not polluted by the jls table jail_exists() forks.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)

# kill and enter are served by the ROOT worker, so a daemon-down skip is a
# verdict about the box, not the tree.  Test the PROCESS, not the socket:
# `service stop` leaves control.sock behind, and "sbin/flatpkg" matches rc.d.
pgrep -f "sbin/flatpkgd" >/dev/null 2>&1 || {
	echo "SKIP: no flatpkg daemon running - these commands are served by its root worker, not by this account"
	exit 0
}

APP=${FLATPKG_LIFECYCLE_APP:-com.github.tchx84.Flatseal}
SLEEP_FOR=${FLATPKG_LIFECYCLE_SLEEP:-60}

# The subject is "an installed app", not a particular id: the store is the
# user's and changes, so fall back to the first installed app and skip if none.
if ! flatpkg info "$APP" >/dev/null 2>&1; then
	APP=$(flatpkg complete apps 2>/dev/null | head -1)
fi
if [ -z "$APP" ]; then
	echo "SKIP: no application is installed"
	exit 0
fi

pass=0
fail=0
ok()  { printf '  ok    - %s\n' "$1"; pass=$((pass + 1)); }
bad() { printf '  FAIL  - %s\n' "$1"; fail=$((fail + 1)); }

command -v flatpkg >/dev/null 2>&1 || { echo "instance-lifecycle: no flatpkg on PATH"; exit 1; }

# The daemon must be up: every claim here is about what the daemon says,
# and a missing daemon would make all of them fail for one reason that is
# not theirs.
if ! flatpkg ps >/dev/null 2>&1; then
	echo "instance-lifecycle: no daemon - skipping"
	exit 0
fi

# Clear any instance left by an earlier run: a stale registration makes every
# claim below answer about the wrong jail (measured once as 6 passed, 1 failed).
stale=$(flatpkg ps 2>/dev/null | awk -v a="$APP" '$3 == a { print $4; exit }')
[ -n "$stale" ] && flatpkg kill "$stale" >/dev/null 2>&1
sleep 1

run_pid=""
cleanup() {
	[ -n "$run_pid" ] && kill "$run_pid" 2>/dev/null
	# Remove anything left by a failed assertion, WITH privilege, and
	# then verify - a guard that leaks a jail breaks the next run.
	jid=$(flatpkg ps 2>/dev/null | awk -v a="$APP" '$3 == a { print $4; exit }')
	[ -n "$jid" ] && flatpkg kill "$jid" >/dev/null 2>&1
	sleep 1
}
trap cleanup EXIT INT TERM

# The command does NOT fork, a limitation stated: this claim's fork case could
# not be reproduced here (a shell's `sleep 300 &` does not survive in the
# jail), so kate in apps-visual.sh and jail-drain.sh cover it.
flatpkg run --command=/bin/sleep "$APP" -- "$SLEEP_FOR" >/dev/null 2>&1 &
run_pid=$!

jid=""
i=0
while [ "$i" -lt 40 ]; do
	sleep 2
	jid=$(flatpkg ps 2>/dev/null | awk -v a="$APP" '$3 == a { print $4; exit }')
	[ -n "$jid" ] && break
	i=$((i + 1))
done

if [ -n "$jid" ]; then
	ok "ps lists the running instance (jid $jid)"
else
	bad "ps did not list $APP while it was running"
	printf 'instance-lifecycle: passed %d, failed %d\n' "$pass" "$fail"
	exit 1
fi

# Claim 2, and the reason it is a separate assertion: the jls table went
# to the caller's stdout, so the check is on the BYTES of the output and
# not on the exit status of the command.
if flatpkg ps 2>/dev/null | grep -q 'IP Address'; then
	bad "ps printed a jls table - jail_exists() is not redirecting its child"
else
	ok "ps prints only its own table (no jls output leaked)"
fi

if flatpkg ps 2>/dev/null | awk -v a="$APP" '$3 == a { found = 1 } END { exit !found }'; then
	ok "the instance line carries the app id"
else
	bad "the instance line does not carry the app id"
fi

out=$(flatpkg enter "$jid" /bin/sh -c 'echo ENTER-OK' 2>&1)
case "$out" in
*ENTER-OK*) ok "enter runs a command inside the instance's jail" ;;
*)          bad "enter did not run inside the jail: $out" ;;
esac

flatpkg kill "$jid" >/dev/null 2>&1

# Let the teardown finish before touching the launch again: killing the client
# here raced the worker's unmounting and left a nullfs mount for check-clean.
# Waiting is free - the client returns on its own.
i=0
while [ "$i" -lt 20 ] && kill -0 "$run_pid" 2>/dev/null; do
	sleep 1
	i=$((i + 1))
done
wait "$run_pid" 2>/dev/null
run_pid=""          # the trap must not kill a launch that has finished

if flatpkg ps 2>/dev/null | awk -v a="$APP" '$3 == a { found = 1 } END { exit !found }'; then
	bad "the instance is still listed after kill"
else
	ok "kill removes the instance"
fi

# The launch RETURNS, with the app's status: the drain waits for the jail to
# empty, and a wait that never ends is the obvious way to get that wrong.
# Bounded by timeout(1), because a planted bad drain HUNG this guard.
start=$(date +%s)
timeout 60 flatpkg run --command=/bin/true "$APP" >/dev/null 2>&1
rc=$?
elapsed=$(( $(date +%s) - start ))
if [ "$rc" -eq 0 ] && [ "$elapsed" -lt 60 ]; then
	ok "a quick app returns (rc 0 in ${elapsed}s)"
else
	bad "a quick app did not return cleanly (rc $rc after ${elapsed}s)"
fi

timeout 60 flatpkg run --command=/bin/false "$APP" >/dev/null 2>&1
rc=$?
if [ "$rc" -eq 1 ]; then
	ok "the app's exit status survives the jail drain (rc 1)"
else
	bad "exit status lost through the jail drain (rc $rc, expected 1)"
fi

# Floor: a guard whose subject vanished must not report a clean sheet.
if [ "$pass" -lt 4 ]; then
	echo "instance-lifecycle: only $pass claim(s) ran - not looking at what it thinks"
	exit 1
fi

printf 'instance-lifecycle: passed %d, failed %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
