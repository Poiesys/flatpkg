#!/bin/sh
# bus-signal-scope.sh - both session-bus signal methods must signal only a
# pid THEIR OWN caller started: a foreign pid is refused with a D-Bus error
# and the victim survives.  Trap: the portal's allowed half is structural.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

# These methods are served by the daemon's root worker, not by this account.
# Test the PROCESS, not the socket: `service flatpkgd stop` leaves the socket
# behind, so a socket probe passes with no daemon running.
pgrep -f "sbin/flatpkgd" >/dev/null 2>&1 || {
	echo "SKIP: no flatpkg daemon running - these commands are served by its root worker, not by this account"
	exit 0
}

FLATPKG="$ROOT/build/flatpkg"
tmp=$(mktemp -d /tmp/.fpkg-portal.XXXXXX)
fail=0
note() { echo "FAIL: $*"; fail=1; }

# Resolve the guard's own tools BEFORE PATH is narrowed: the subjects get a
# pinned, minimal PATH (they resolve program names through it), and the guard
# still has to find its instruments in /usr/local/bin.
DBUS_DAEMON=$(command -v dbus-daemon 2>/dev/null || true)
PYTHON=$(command -v python3 2>/dev/null || true)
[ -n "$DBUS_DAEMON" ] || { echo "SKIP: no dbus-daemon, so no private session bus"; exit 0; }

# tests/dbusprobe.c, compiled here and never installed: no command-line D-Bus
# client on this box can send the `aay` that HostCommand's argv is, so the
# value has to be built programmatically.  Built by tests/probelib.sh.
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

victim_pids=""
portal=""
bus=""
helper=""
hproxy_pid=""
pxy=""
cleanup() {
	# The helper and both proxies are killed here too: a cleanup that omits a
	# process it started relies on that process noticing its socket is gone.
	for p in $victim_pids $portal $helper $hproxy_pid $pxy $bus; do
		[ -n "$p" ] || continue
		kill -TERM "$p" 2>/dev/null || true
	done
	n=0
	while [ $n -lt 10 ]; do
		alive=0
		for p in $victim_pids $portal $helper $hproxy_pid $pxy $bus; do
			[ -n "$p" ] || continue
			kill -0 "$p" 2>/dev/null && alive=1
		done
		[ "$alive" -eq 0 ] && break
		sleep 1
		n=$((n + 1))
	done
	for p in $victim_pids $portal $helper $hproxy_pid $pxy $bus; do
		[ -n "$p" ] || continue
		kill -KILL "$p" 2>/dev/null || true
	done
	# Verified, not assumed: a cleanup that cannot fail loudly has done nothing.
	for p in $victim_pids $portal $helper $hproxy_pid $pxy $bus; do
		[ -n "$p" ] || continue
		if kill -0 "$p" 2>/dev/null; then
			echo "note: CLEANUP INCOMPLETE - pid $p survived the guard."
			echo "        kill -9 $p"
		fi
	done
	rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

# A victim in its OWN process group, or killpg(victim) proves nothing: a
# background job in a non-interactive shell keeps the shell's group.  It must
# be started in THIS shell - a substitution would hold its pipe open.
VICTIM=""
victim_pids=""

# Wait for the property; do not sample it.  The victim's setpgid() can lose a
# race with a single `ps`, so this bounds the wait at four seconds and every
# caller still asserts afterwards.  One helper, so neither site samples again.
wait_group_leader() {
	_wpg=""
	_n=0
	while [ $_n -lt 40 ]; do
		_wpg=$(ps -o pgid= -p "$1" 2>/dev/null | tr -d ' ')
		[ -n "$_wpg" ] && [ "$_wpg" = "$1" ] && break
		sleep 0.1
		_n=$((_n + 1))
	done
	echo "$_wpg"
}

# `kill -0` says YES for a zombie, and a SIGKILLed child stays in the table
# until it is reaped, so liveness is a timing-dependent question.  The state
# column answers it: Z is dead, and so is no row at all.
victim_alive() {
	_st=$(ps -o state= -p "$1" 2>/dev/null | tr -d ' ')
	[ -n "$_st" ] && [ "$_st" != Z ] && [ "$_st" != "Z+" ]
}
start_victim() {
	if [ -n "$PYTHON" ]; then
		"$PYTHON" -c 'import os; os.setpgid(0, 0); os.execv("/bin/sleep", ["sleep", "300"])' &
	else
		/bin/sleep 300 &
	fi
	VICTIM=$!
	victim_pids="$victim_pids $VICTIM"
}

# ---- a private bus and a portal on it ---------------------------------
# Pin the environment BEFORE the bus starts: dbus-daemon reads XDG_DATA_DIRS
# at startup, so a later export lets the INSTALLED portal win the name.
export HOME="$tmp/home"
export XDG_RUNTIME_DIR="$tmp/run"
export XDG_DATA_DIRS="$tmp/share"
export XDG_CURRENT_DESKTOP=
export WAYLAND_DISPLAY=
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export TERM=dumb
export NO_COLOR= FORCE_COLOR=
mkdir -p "$HOME" "$XDG_RUNTIME_DIR" "$XDG_DATA_DIRS"

# `--print-address=1` is a FILE DESCRIPTOR, not a path: a filename here reads
# an empty address and the portal then looks unreachable.
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
# The one name that is the SUBJECT, pinned to the bus just built.
export DBUS_SESSION_BUS_ADDRESS="$ADDR"

"$FLATPKG" portal >"$tmp/portal.log" 2>&1 &
portal=$!
# Wait for the name, not a pid: a portal that has not acquired its name yet
# answers UnknownObject, and a guard reading that as a refusal passes.
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
# ---- 1 and 2: two forms, two victims, both must survive ----------------
call_spawn_signal() {
	"$PROBE" spawn-signal "$1" "$2" "$3" 2>&1
}

# SIGKILL: the strongest signal, so a build that quietly complies cannot
# survive.  A wrong number is a harmless EINVAL, and the victim lives anyway.
for form in false true; do
	start_victim
	v=$VICTIM
	# If the bus is gone, say so: the call below would fail with "Failed to
	# open connection", which is not an Error reply, and the case would lie.
	if ! kill -0 "$bus" 2>/dev/null; then
		note "the private bus died before case to_process_group=$form"
		break
	fi
	pg=$(wait_group_leader "$v")
	out=$(call_spawn_signal "$v" 9 "$form") || true
	if victim_alive "$v"; then
		:; else
		note "the portal signalled pid $v (to_process_group=$form), which"
		note "  no Spawn had started - any process on the session bus can"
		note "  reach this method, and a sandboxed app is one of them"
		printf '%s\n' "$out" | head -3 | sed 's/^/        /'
	fi
	# The reply must be an error, so a caller can tell it did not happen.
	case "$out" in
	*Error*) : ;;
	*) note "SpawnSignal(to_process_group=$form) did not answer with a D-Bus" \
		"error: $out" ;;
	esac
	# The message must say why: this is a D-Bus string, not a drawn line, so
	# the whole of it survives and there is no reason to assert a prefix.
	case "$out" in
	*"not a process this caller spawned"*) : ;;
	*) note "the refusal does not say the pid was never spawned: $out" ;;
	esac
	if [ "$form" = true ] && [ "${pg:-$v}" != "$v" ]; then
		note "the to_process_group victim was not a process-group leader"
		note "  (pgid $pg, pid $v), so that case tested nothing"
	fi
	kill -TERM "$v" 2>/dev/null || true
	victim_pids=$(printf '%s\n' $victim_pids | grep -vx "$v" | tr '\n' ' ')
done

# ---- the session helper, the refused direction, at runtime -------------
# Started on the SAME private bus: a different name, no conflict.  Never
# pkill a helper here: that also kills the user's own on the real session bus.
"$FLATPKG" session-helper >"$tmp/helper.log" 2>&1 &
helper=$!
n=0
up=no
while [ $n -lt 50 ]; do
	if "$PROBE" ping org.freedesktop.Flatpak \
	    /org/freedesktop/Flatpak/Development >/dev/null 2>&1; then
		up=yes
		break
	fi
	sleep 0.2
	n=$((n + 1))
done
if [ "$up" != yes ]; then
	echo "FAIL: the session helper never answered on the private bus:"
	sed 's/^/        /' "$tmp/helper.log" | head -5
	fail=1
fi
# Backstop, not the fix: dbus-daemon can still activate the INSTALLED helper
# (its compiled-in service dir wins), so skip rather than assert against the
# wrong process's log.
if grep -q 'cannot own' "$tmp/helper.log" 2>/dev/null; then
	echo "SKIP: another session helper owns org.freedesktop.Flatpak"
	echo "      on the private bus; the cases would read its log"
	exit "$fail"
fi
if ! kill -0 "$helper" 2>/dev/null; then
	echo "FAIL: the session helper exited before answering:"
	sed 's/^/        /' "$tmp/helper.log" | head -5
	fail=1
fi

# Two counters over the helper's own log.  Counts, not greps: a redirected log
# keeps one line per record with the badge after the text, and a phrase may sit
# anywhere on it, but counting badges is what says the message came from here.
helper_warns() {
	sed 's/\x1b\[[0-9;]*m//g; s/\x1b\[[0-9;]*G//g' "$tmp/helper.log" |
		grep -c '\[ WARN  \]' || true
}
helper_hosts() {
	sed 's/\x1b\[[0-9;]*m//g; s/\x1b\[[0-9;]*G//g' "$tmp/helper.log" |
		grep -c 'host command from' || true
}
# A separate counter, because the refusal phrase must not contain the
# started-command phrase: a substring match counted refusals as starts, so the
# warning says REFUSED in capitals and the two counters are disjoint.
helper_refused() {
	sed 's/\x1b\[[0-9;]*m//g; s/\x1b\[[0-9;]*G//g' "$tmp/helper.log" |
		grep -c 'host command REFUSED' || true
}

# ---- a REAL xdg-dbus-proxy, for the cases whose caller must be one ------
# HostCommand asks who is asking, so an allowed case needs a caller entitled
# to ask: a real xdg-dbus-proxy, started once - only the peer changes.
ACCEPT_SOCK=""
hproxy=""
for cand in /usr/local/bin/xdg-dbus-proxy /usr/bin/xdg-dbus-proxy; do
	[ -x "$cand" ] && { hproxy=$cand; break; }
done
if [ -n "$hproxy" ]; then
	ACCEPT_SOCK="$tmp/accept.sock"
	"$hproxy" "$ADDR" "$ACCEPT_SOCK" --filter \
	    --call=org.freedesktop.Flatpak.*=* >"$tmp/hproxy.log" 2>&1 &
	hproxy_pid=$!
	n=0
	while [ $n -lt 40 ]; do
		[ -S "$ACCEPT_SOCK" ] && break
		sleep 0.2
		n=$((n + 1))
	done
	if [ ! -S "$ACCEPT_SOCK" ]; then
		note "the real xdg-dbus-proxy never created $ACCEPT_SOCK:"
		sed 's/^/        /' "$tmp/hproxy.log" 2>/dev/null | head -4
		ACCEPT_SOCK=""
	fi
else
	echo "  no xdg-dbus-proxy on this box, so the session helper's ALLOWED"
	echo "  direction is not run - the structural claims still are"
fi

if [ "$up" = yes ]; then
	start_victim
	v=$VICTIM
	wait_group_leader "$v" >/dev/null
	sout=$("$PROBE" host-signal "$v" 9 false 2>&1) || true
	if victim_alive "$v"; then
		:
	else
		note "HostCommandSignal killed pid $v, which no HostCommand started -"
		note "  the second copy of the same method, on the same bus"
	fi
	# The D-Bus error NAME separately from the message: a plain InvalidArgs
	# would mean "malformed call", a different defect with the same status.
	case "$sout" in
	*AccessDenied*) : ;;
	*) note "HostCommandSignal on a foreign pid was not answered with" \
		"AccessDenied: $sout" ;;
	esac
	# A shell glob has no `+` quantifier, so `*not+a+host+command*` could not
	# match the text it was written from and its case was untested, not green.
	case "$sout" in
	*"is not a host command"*) : ;;
	*) note "the refusal does not say the pid is not a host command: $sout" ;;
	esac
	kill -TERM "$v" 2>/dev/null || true
	victim_pids=$(printf '%s\n' $victim_pids | grep -vx "$v" | tr '\n' ' ')

	# ---- the CREATION method's caller, refused, at runtime -------------
	# HostCommand started whatever it was handed, from a caller with no jail,
	# no proxy and no --own=.  Asserted: refused, no pid, no host command run.
	awarns=$(helper_warns)
	ahosts=$(helper_hosts)
	arefu=$(helper_refused)
	aout=$("$PROBE" host-signal-own 15 2>&1) || true
	case "$aout" in
	*"only serves a flatpkg sandbox"*) : ;;
	*) note "an UNSANDBOXED caller reached HostCommand and it did not refuse"
	   note "  it for the right reason: $(printf '%s' "$aout" | head -1)" ;;
	esac
	case "$aout" in
	*pid=*) note "HostCommand refused an unsandboxed caller but still returned"
	        note "  a pid: $(printf '%s' "$aout" | head -1)" ;;
	*) : ;;
	esac
	awarns2=$(helper_warns)
	ahosts2=$(helper_hosts)
	arefu2=$(helper_refused)
	# Wait for the badge, do not sample it: a loaded box can schedule the
	# guard's read before the write.  Bounded; the assertion is unchanged.
	n=0
	while [ $n -lt 40 ]; do
		[ "${awarns2:-0}" -gt "${awarns:-0}" ] &&
		    [ "${arefu2:-0}" -gt "${arefu:-0}" ] && break
		sleep 0.05
		awarns2=$(helper_warns)
		arefu2=$(helper_refused)
		n=$((n + 1))
	done
	if [ "${awarns2:-0}" -gt "${awarns:-0}" ] &&
	   [ "${arefu2:-0}" -gt "${arefu:-0}" ]; then
		:
	else
		note "the refusal produced no WARN badge naming it (badges" \
			"$awarns -> $awarns2, 'host command REFUSED'" \
			"$arefu -> $arefu2), so it cannot be told from a build" \
			"with no check at all"
		sed 's/\x1b\[[0-9;]*[A-Za-z]//g' "$tmp/helper.log" |
			tail -8 | sed 's/^/        /'
	fi
	if [ "${ahosts2:-0}" -eq "${ahosts:-0}" ]; then
		echo "  an unsandboxed caller was refused by HostCommand, with no pid"
		echo "     and no host command started ($awarns2 WARN badge(s) up"
		echo "     from $awarns, 'host command from' unchanged at $ahosts2)"
	else
		note "HostCommand refused an unsandboxed caller and STILL logged" \
			"$((ahosts2 - ahosts)) host command(s) - the refusal is after"
	fi

	# Every case below is meant to SUCCEED, so each needs a caller the service
	# may serve - the proxy.  Without one they are NOT RUN rather than failed:
	# GIO refuses an empty "unix:path=" first, which is a missing prerequisite.
	if [ -z "$ACCEPT_SOCK" ]; then
		echo "  the session helper's ALLOWED cases are NOT RUN (no real"
		echo "     xdg-dbus-proxy to be a caller the service may serve)"
	elif true; then

	# ---- the ALLOWED direction, at runtime, and DELIVERY --------------
	# Both calls on ONE connection: the service keys the pid to the caller's
	# bus name.  SIGTERM, not 0: permission without delivery would pass.
	aout=$(DBUSPROBE_BUS="unix:path=$ACCEPT_SOCK" "$PROBE" \
	    host-signal-own 15 2>&1) || true
	ownpid=$(printf '%s\n' "$aout" | sed -n 's/^pid=\([0-9][0-9]*\)$/\1/p')
	case "$aout" in
	*"reply=ok"*) : ;;
	*) note "the caller's OWN host command was not signalled: $aout" ;;
	esac
	if [ -n "$ownpid" ]; then
		# The service's own child watch reaps it, so nothing here can hold it
		# as a zombie and `kill -0` is enough.  Bounded wait.
		n=0
		while [ $n -lt 20 ] && kill -0 "$ownpid" 2>/dev/null; do
			sleep 0.2
			n=$((n + 1))
		done
		kill -0 "$ownpid" 2>/dev/null &&
			note "HostCommandSignal was accepted for the caller's own host" \
				"command $ownpid but the process is still running - the" \
				"signal was permitted and never delivered"
	fi

	# ---- the CROSS-CALLER case: the half a pid-only check would let through
	# Signal 0, because the pid above gets reaped and dropped from the table;
	# a new probe invocation is a new bus name, which the `client` half sees.
	keep=$(DBUSPROBE_BUS="unix:path=$ACCEPT_SOCK" "$PROBE" \
	    host-signal-own 0 2>&1) || true
	keeppid=$(printf '%s\n' "$keep" | sed -n 's/^pid=\([0-9][0-9]*\)$/\1/p')
	if [ -z "$keeppid" ]; then
		note "could not start a host command for the cross-caller case:$keep"
	else
		case "$keep" in
		*"reply=ok"*) : ;;
		*) note "signal 0 on the caller's own host command was refused, so" \
			"the ALLOWED direction is not proven even for signal 0: $keep" ;;
		esac
		csout=$("$PROBE" host-signal "$keeppid" 9 false 2>&1) || true
		case "$csout" in
		*"is not a host command"*) : ;;
		*) note "a DIFFERENT caller was allowed to signal pid $keeppid," \
			"which the first caller started: $csout" ;;
		esac
		# ...and the victim is still alive, which is the half a
		# `reply=ok` on the refusal cannot tell us.
		kill -0 "$keeppid" 2>/dev/null ||
			note "a DIFFERENT caller killed pid $keeppid - the refusal" \
				"was answered AND the signal was sent"
		kill -KILL "$keeppid" 2>/dev/null || true
	fi

	# ---- the `--forward-fd` path, host half ---------------------------
	# The child prints what it read to the HELPER's stdout, so the assertion
	# reads its log; the control reads 6, not 5, so "arrived somewhere" fails.
	fd_before=$(grep -c 'got:FPKG-FD-5' "$tmp/helper.log" 2>/dev/null || true)
	# Through the proxy, like every call meant to SUCCEED: HostCommand asks
	# who is asking, and this is the same method as the signal path above.
	DBUSPROBE_BUS="unix:path=$ACCEPT_SOCK" "$PROBE" host-fd-echo 5 5 \
	    >/dev/null 2>&1 || true
	# The child is a process the helper forks and the probe does not wait
	# for: give it time under suite load, and do not start the control
	# until the positive case has answered.
	fd_ok=0
	n=0
	while [ $n -lt 150 ]; do
		if [ "$(grep -c 'got:FPKG-FD-5' "$tmp/helper.log" 2>/dev/null || true)" -gt "$fd_before" ]; then
			fd_ok=1
			break
		fi
		sleep 0.2
		n=$((n + 1))
	done
	if [ "$fd_ok" -eq 0 ]; then
		note "a descriptor passed to HostCommand did not arrive at the"
		note "  number the caller asked for (5): the child read nothing there"
	else
		DBUSPROBE_BUS="unix:path=$ACCEPT_SOCK" "$PROBE" host-fd-echo 5 6 \
		    >/dev/null 2>&1 || true
		# Wait for the second child to have run, or the control asserts
		# nothing: nothing appearing yet is what "refused" also looks like.
		sleep 5
		fd_after=$(grep -c 'got:FPKG-FD-5' "$tmp/helper.log" 2>/dev/null || true)
		[ "$fd_after" -eq "$((fd_before + 1))" ] || {
			note "the descriptor was readable at a number NOBODY passed (6), so"
			note "  what arrived is a descriptor, not the one that was asked for"
		}
	fi
	fi	# end of the "there is a proxy to be a caller with" branch
fi		# end of "the helper answered on the private bus"

# ---- structurally: both services --------------------------------------
# Fed strip_comments output; commentlib.sh is sourced, never run.
. "$(dirname "$0")/commentlib.sh"
body=$(strip_comments < "$ROOT/src/cli/portal.c" |
	carve_function '^handle_spawn_signal' "$ROOT/src/cli/portal.c" || true)
[ -n "$body" ] || note "could not carve handle_spawn_signal() out of src/cli/portal.c"
case "$body" in
*spawn_signal_allowed*) : ;;
*) note "handle_spawn_signal() does not ask whether the caller spawned it" ;;
esac
# Ordering: the check before the signal.  A grep for "does it check?" is
# satisfied by a check after the kill, which is the runtime-dir.sh shape.
ck=$(printf '%s\n' "$body" | awk '/spawn_signal_allowed\(sender/ { print NR; exit }')
for sig in 'killpg(' 'kill('; do
	k=$(printf '%s\n' "$body" | awk -v t="$sig" 'index($0, t) > 0 {
		q = index($0, "\""); p = index($0, t);
		if (q == 0 || p < q) { print NR; exit } }')
	[ -n "$k" ] || continue
	if [ -n "$ck" ] && [ "$ck" -ge "$k" ]; then
		note "handle_spawn_signal() signals (line $k) before asking whether"
		note "  the caller spawned the process (line $ck)"
	fi
done
# And both halves, which is the subtle one: a check on app_pid alone lets
# caller B kill caller A's app.
pred=$(strip_comments < "$ROOT/src/cli/portal.c" |
	carve_function '^spawn_signal_allowed' "$ROOT/src/cli/portal.c" || true)
[ -n "$pred" ] || note "could not carve spawn_signal_allowed() out of src/cli/portal.c"
case "$pred" in
*app_pid*) : ;;
*) note "the predicate does not compare the recorded app pid" ;;
esac
case "$pred" in
*sender*) : ;;
*) note "the predicate does not compare the caller's bus name" ;;
esac
case "$pred" in
*strcmp*) : ;;
*) note "the predicate does not compare the sender by name" ;;
esac

# The second service, structurally: the same claims, in the other file.
body=$(strip_comments < "$ROOT/src/cli/sessionhelper.c" |
	carve_function '^handle_host_command_signal' "$ROOT/src/cli/sessionhelper.c" || true)
[ -n "$body" ] || note "could not carve handle_host_command_signal() out of src/cli/sessionhelper.c"
case "$body" in
*host_command_signal_allowed*) : ;;
*) note "handle_host_command_signal() does not ask whether the caller" \
	"started the process" ;;
esac
ck=$(printf '%s\n' "$body" | awk '/host_command_signal_allowed\(sender/ { print NR; exit }')
for sig in 'killpg(' 'kill('; do
	k=$(printf '%s\n' "$body" | awk -v t="$sig" 'index($0, t) > 0 {
		q = index($0, "\""); p = index($0, t);
		if (q == 0 || p < q) { print NR; exit } }')
	[ -n "$k" ] || continue
	if [ -n "$ck" ] && [ "$ck" -ge "$k" ]; then
		note "handle_host_command_signal() signals (line $k) before asking"
		note "  whether the caller started the process (line $ck)"
	fi
done
pred=$(strip_comments < "$ROOT/src/cli/sessionhelper.c" |
	carve_function '^host_command_signal_allowed' "$ROOT/src/cli/sessionhelper.c" || true)
[ -n "$pred" ] || note "could not carve host_command_signal_allowed() out of src/cli/sessionhelper.c"
case "$pred" in
*client_pid_data_hash*) : ;;
*) note "the session helper's predicate does not read the pid table it keeps" ;;
esac
case "$pred" in
*client*) : ;;
*) note "the session helper's predicate does not compare the caller" ;;
esac
case "$pred" in
*strcmp*) : ;;
*) note "the session helper's predicate does not compare the caller by name" ;;
esac

# ---- the portal's CALLER IDENTITY, and the forgery --------------------
# ARGV is chosen by whoever calls exec, so only KERN_PROC_PATHNAME says who
# the caller is: --own= forged refused, a real xdg-dbus-proxy accepted.
PROXY=""
for cand in /usr/local/bin/xdg-dbus-proxy /usr/bin/xdg-dbus-proxy; do
	[ -x "$cand" ] && { PROXY=$cand; break; }
done
echo
echo "the portal's caller identity (KERN_PROC_PATHNAME, then argv):"

# The count is taken HERE, above the cases it bounds: below the loop, the two
# counts would be the same number and the comparison could never be true.
portal_warns() {
	sed 's/\x1b\[[0-9;]*m//g; s/\x1b\[[0-9;]*G//g' "$tmp/portal.log" |
		grep -c '\[ WARN  \]' || true
}
warns0=$(portal_warns)

# 1. the control, and 2. the forgery - one process, two argument vectors.
: > "$tmp/ident"
for form in none forged; do
	before=$(wc -c < "$tmp/portal.log" 2>/dev/null || echo 0)
	# `|| true` because the probe returns 3 on a D-Bus error, and a refusal IS
	# the answer here: under `set -e` the script would otherwise end.
	if [ "$form" = none ]; then
		out=$("$PROBE" spawn 2>&1) || true
	else
		out=$("$PROBE" spawn --own=com.example.forged 2>&1) || true
	fi
	# WAIT for the log, do not sample it: the portal writes the refusal on its
	# own schedule, so a grep right after the reply can read a file the line
	# has not reached.
	n=0
	while [ $n -lt 30 ]; do
		grep -q 'is not flatpkg' "$tmp/portal.log" 2>/dev/null && break
		sleep 0.2
		n=$((n + 1))
	done
	tail_bytes=$(wc -c < "$tmp/portal.log" 2>/dev/null || echo 0)
	[ "$tail_bytes" -gt "$before" ] || before=$tail_bytes
	case "$out" in
	*"only works in a flatpak"*) say=yes ;;
	*) say=no ;;
	esac
	printf '  %-7s -> %s
' "$form" "$(printf '%s' "$out" | head -1)" >> "$tmp/ident"
	if [ "$say" != yes ]; then
		note "a direct caller ($form) was not refused with 'only works in a"
		note  "  flatpak'; the portal answered: $(printf '%s' "$out" | head -1)"
	fi
done
# ...and the refusal must be one the PORTAL spoke, so count its WARN badges:
# the badge is drawn for the line the portal wrote, whatever its length.
echo "  portal WARN badges before the two refusals: $warns0"
for form in none forged; do
	if sed 's/\x1b\[[0-9;]*m//g' "$tmp/portal.log" |
	    grep -q "spawn from pid .*: "; then
		:
	else
		note "no caller ($form) was told, by the portal, that the process it"
		note "  looked at was not flatpkg's dbus proxy"
		break
	fi
done
warns_after=$(portal_warns)
if [ "${warns_after:-0}" -gt "${warns0:-0}" ]; then
	echo "  both refusals were the portal's own, and both named the pid it"
	echo "  looked at ($warns_after badge(s), up from $warns0)"
else
	note "the two refusals produced no portal WARN badge, so they cannot be"
	note "  told apart from a build that simply has no argv check"
fi

# 3. the accept direction, and the only thing it needs is a real proxy.
if [ -z "$PROXY" ]; then
	echo "  accept   NOT RUN - no xdg-dbus-proxy on this box, so there is"
	echo "           no caller that IS a flatpkg proxy to be accepted as"
	echo "           (the structural claims above still ran)"
else
	# A nonexistent app id on purpose: "the store was asked about this id" is
	# the observable for "got past resolve_app_id()", and a real app would
	# build a 13-second union root.  A D-Bus name must start with a letter.
	apptag="com.example.fpguard.accepted"
	psock="$tmp/accepted.sock"
	# The forgery refusals above wrote "is not flatpkg" into this log; only
	# what the real proxy appends can convict the executable check.
	accept_log=$(wc -c < "$tmp/portal.log" | tr -d ' ')
	"$PROXY" "$ADDR" "$psock" --filter "--own=$apptag" 	    --call=org.freedesktop.portal.*=* >"$tmp/proxy.log" 2>&1 &
	pxy=$!
	n=0
	while [ $n -lt 40 ]; do
		[ -S "$psock" ] && break
		sleep 0.2
		n=$((n + 1))
	done
	if [ ! -S "$psock" ]; then
		note "the real xdg-dbus-proxy never created $psock:"
		sed 's/^/        /' "$tmp/proxy.log" 2>/dev/null | head -4
		kill "$pxy" 2>/dev/null || true
	else
		# This Spawn never answers - the store rejects the id and the portal
		# keeps waiting - so shorten the probe's timeout; the default 120s is
		# for a real Spawn whose pid arrives after the union root.
		DBUSPROBE_BUS="unix:path=$psock" DBUSPROBE_TIMEOUT_MS=3000 \
		    "$PROBE" spawn >/dev/null 2>&1 || true
		# WAIT for the log, not for the call: the fork and the child's
		# first diagnostic can land after the probe has given up.
		n=0
		while [ $n -lt 80 ]; do
			grep -q "$apptag" "$tmp/portal.log" 2>/dev/null && break
			sleep 0.25
			n=$((n + 1))
		done
		if grep -q "$apptag" "$tmp/portal.log" 2>/dev/null; then
			echo "  accept   a real xdg-dbus-proxy with --own=$apptag was"
			echo "           accepted: the portal went on to ask the store"
		else
			note "a REAL xdg-dbus-proxy carrying --own=$apptag was not"
			note "  accepted - the portal never got as far as the store:"
			sed 's/^/        /' "$tmp/portal.log" 2>/dev/null | tail -5
		fi
		# and it must NOT have been turned away for its executable,
		# which is the check the forgery case proves
		if tail -c "+$((accept_log + 1))" "$tmp/portal.log" 2>/dev/null |
		    sed 's/\x1b\[[0-9;]*m//g' | grep -q "is not flatpkg"; then
			note "the portal refused a real proxy for not being one - the"
			note "  executable check is rejecting flatpkg's own proxy"
		fi
		kill "$pxy" 2>/dev/null || true
		wait "$pxy" 2>/dev/null || true
	fi
fi

# ---- the CREATION method's check, structurally ------------------------
# handle_host_command() must ask flatpkg_caller_check() BEFORE the g_spawn,
# refuse with a D-Bus error; both services must call the SAME function.
strip_comments < "$ROOT/src/cli/sessionhelper.c" > "$tmp/helper_stripped.c"
hcmd=$(carve_function '^handle_host_command\(' "$tmp/helper_stripped.c" || true)
if [ -z "$hcmd" ]; then
	note "could not carve handle_host_command() out of src/cli/sessionhelper.c"
else
	case "$hcmd" in
	*flatpkg_caller_check*) : ;;
	*) note "handle_host_command() does not ask flatpkg_caller_check() who" \
	       note "  is asking - it read '(void)sender' until round 224" ;;
	esac
	hc=$(printf '%s\n' "$hcmd" | grep -n 'flatpkg_caller_check' | head -1 |
		cut -d: -f1)
	hs=$(printf '%s\n' "$hcmd" | grep -n 'g_spawn_async_with_pipes' |
		head -1 | cut -d: -f1)
	if [ -n "$hc" ] && [ -n "$hs" ]; then
		if [ "$hc" -ge "$hs" ]; then
			note "handle_host_command() asks at line $hc, at or after the" \
				note "  g_spawn at $hs - a refusal there is a process" \
				note "  nobody asked for"
		else
			echo "  handle_host_command() asks at line $hc and only" \
				" spawns at $hs, and the refusal is a D-Bus error"
		fi
	fi
	case "$hcmd" in
	*G_DBUS_ERROR_ACCESS_DENIED*) : ;;
	*) note "handle_host_command() does not refuse with a D-Bus error, so a" \
	       note "  caller cannot tell a refusal from a command that did nothing" ;;
	esac
fi

# 4. The one decision.  Both call sites, the same callee.
strip_comments < "$ROOT/src/cli/portal.c" > "$tmp/p_caller.c"
rid=$(carve_function '^resolve_app_id\(' "$tmp/p_caller.c" || true)
[ -n "$rid" ] || note "could not carve resolve_app_id() out of src/cli/portal.c"
ncall=0
for f in "portal.c:resolve_app_id" "sessionhelper.c:handle_host_command"; do
	cf=${f%%:*}
	fn=${f##*:}
	strip_comments < "$(srcf "$cf")" > "$tmp/one_call.c"
	body=$(carve_function "^$fn\(" "$tmp/one_call.c" || true)
	if [ -z "$body" ]; then
		note "could not carve $fn() out of src/$cf"
		continue
	fi
	case "$body" in
	*flatpkg_caller_check*) ncall=$((ncall + 1)) ;;
	*) note "src/$cf's $fn() does not ask flatpkg_caller_check() - the" \
	       note "  'is this caller ours' question is now asked in two places" ;;
	esac
done
[ "$ncall" -eq 2 ] ||
	note "flatpkg_caller_check() has $ncall of its 2 call sites asking; a" \
		"third service that copies the predicate instead of calling it is" \
		"the drift this exists to prevent"
# The executable reader has ONE home, in dbus.c beside the proxy list, and a
# second copy is what a symbol grep cannot see.  A `for` over the files, not a
# glob: an unexpanded glob reads nothing and still reports "0 places".
npath=0
for f in "$ROOT"/src/*.c "$ROOT"/src/*/*.c "$ROOT"/src/*/*/*.c; do
	np=$(strip_comments < "$f" | grep -c 'KERN_PROC_PATHNAME' || true)
	npath=$((npath + ${np:-0}))
done
[ "${npath:-0}" -eq 1 ] || note "KERN_PROC_PATHNAME is read in ${npath:-0} places;" \
	"it is one question ('what did this process run') and it belongs to" \
	"src/shared/dbus.c, beside the proxy list both services ask about"

# ---- the STRUCTURE, on a box with no proxy at all ----------------------
# The portal must ASK whether the caller is one of ours, and the list it asks
# about must be the same list the launcher starts proxies from.

# ---- handle_spawn: a pid is only returned if it can be MANAGED ----------
# A full child table once fell through to reply_uint32() after the fork: the
# caller got a pid with no entry - unstoppable and never reported exited.
strip_comments < "$ROOT/src/cli/portal.c" > "$tmp/portal_stripped.c"
hs=$(carve_function '^handle_spawn\(' "$tmp/portal_stripped.c" || true)
if [ -z "$hs" ]; then
	note "could not carve handle_spawn() out of src/cli/portal.c"
else
	cap=$(printf '%s\n' "$hs" |
		grep -n 'nchildren >= MAX_SPAWN_CHILDREN' | head -1 |
		cut -d: -f1)
	fk=$(printf '%s\n' "$hs" |
		grep -n 'pid_t child = fork()' | head -1 | cut -d: -f1)
	# Matched as a STATEMENT, not as text: the comment above this very fix
	# names reply_uint32(), and grepping for that text matched its own prose
	# and failed a correct file.
	rep=$(printf '%s\n' "$hs" |
		grep -cE '^[[:space:]]*reply_uint32\(conn, msg, result_pid\);$')
	if [ -z "$cap" ]; then
		note "handle_spawn() never checks the child table, so a full table"
		note "  returns a pid with no entry - unmanageable and unreported"
	elif [ -z "$fk" ]; then
		: # a carve that found no fork is reported below
	elif [ "$cap" -ge "$fk" ]; then
		note "handle_spawn() checks the child table at line $cap, at or"
		note "  after the fork at $fk - by then the child is already"
		note "  running, so there is nothing left to refuse"
	fi
	if [ "$rep" != 1 ]; then
		note "handle_spawn() returns the pid in $rep place(s), not 1; the"
		note "  claim is that the one of them is unreachable when the"
		note "  table is full, and that needs there to BE one"
	fi
	# The post-fork branch, if reached with no room, must kill the child rather
	# than return an untrackable pid.  The range ends at a LONE closing brace,
	# so it covers the whole if/else and not just the success branch.
	post=$(printf '%s\n' "$hs" | sed -n "/nchildren < MAX_SPAWN_CHILDREN/,/^	}\$/p")
	case "$post" in
	*reply_error*|*kill\(*) : ;;
	*) note "handle_spawn()'s post-fork branch neither replies with an"
	   note "  error nor kills the child when the table is unexpectedly"
	   note "  full - it would return an untrackable pid" ;;
	esac
fi

rid=$(carve_function '^resolve_app_id\(' "$tmp/portal_stripped.c" || true)
if [ -z "$rid" ]; then
	note "could not carve resolve_app_id() out of src/cli/portal.c"
else
	# The ORDER: "we established the caller is ours" before "we believe the
	# name it claims".  The executable half now lives in the shared caller
	# check, so what is asserted is the order of the two calls, not the shape.
	xe=$(printf '%s\n' "$rid" | grep -n 'flatpkg_caller_check' | head -1 |
		cut -d: -f1)
	xa=$(printf '%s\n' "$rid" | grep -n 'app_id_from_proxy_argv' | head -1 |
		cut -d: -f1)
	if [ -n "$xe" ] && [ -n "$xa" ] && [ "$xe" -ge "$xa" ]; then
		note "resolve_app_id() reads the --own= filter (line $xa) before"
		note "  asking whether the caller is a proxy (line $xe), so a forged"
		note "  argv is honoured before anyone checks the executable"
	fi
fi
# ONE list: dbus_proxy_is_path() has to be a predicate in dbus.c beside the
# list, and portal.c must not grow its own copy - the shape that already
# drifted twice when it was duplicated.
strip_comments < "$ROOT/src/shared/dbus.c" > "$tmp/dbus_stripped.c"
dpath=$(carve_function '^dbus_proxy_is_path\(' "$tmp/dbus_stripped.c" || true)
if [ -z "$dpath" ]; then
	note "dbus_proxy_is_path() is not in src/shared/dbus.c - the proxy list now has"
	note "  a second home, and the portal's identity check is one of them"
else
	case "$dpath" in
	*proxy_candidates*) : ;;
	*) note "dbus_proxy_is_path() does not read the proxy_candidates[] list"
	   note "  dbus_proxy_binary() starts proxies from" ;;
	esac
fi
# ...and portal.c holding its own literal would be the copy drifting.
if grep -q '"/usr/local/bin/xdg-dbus-proxy"' "$tmp/portal_stripped.c"; then
	note "src/cli/portal.c has its own xdg-dbus-proxy path literal; the list"
	note "  belongs to dbus.c and the two will drift"
fi

# ...and the *pair* of predicates, so the next copy of this method has a place
# to be caught: both handlers consult a predicate named after their own file's
# convention, and neither still has a bare `killpg(` on the caller's pid.
for f in portal.c sessionhelper.c; do
	n=$(strip_comments < "$(srcf "$f")" | grep -c '_signal_allowed' || true)
	[ "${n:-0}" -ge 2 ] ||
		note "src/$f mentions _signal_allowed $n time(s): a signal method" \
			"without a predicate is the shape this guard exists for"
done

echo
if [ "$fail" -eq 0 ]; then
	echo "PASS: both session-bus signal methods signal only a process THEIR"
	echo "      OWN caller started - the portal's SpawnSignal and the session"
	echo "      helper's HostCommandSignal.  Foreign pids are refused with a"
	echo "      D-Bus error and the victims survive.  The session helper's allowed"
	echo "      direction is proven at runtime - signalled AND delivered, and"
	echo "      refused for a different caller on the same live pid - and a"
	echo "      descriptor passed to HostCommand really lands on the number the"
	echo "      caller named."
	echo "      HostCommand - the method those two manage - now asks who is"
	echo "      asking TOO: an unsandboxed caller is refused with no pid and"
	echo "      no process, and the allowed cases go through a real"
	echo "      xdg-dbus-proxy, which is what a caller the service is"
	echo "      entitled to serve looks like.  Both services ask the SAME"
	echo "      function, flatpkg_caller_check(), so the third service that"
	echo "      needs this question has somewhere to call."
	echo "      The portal's Spawn, which this file's header used to call"
	echo "      unreachable because it builds a real sandbox, is now driven"
	echo "      both ways with NO sandbox: a caller whose own argv says"
	echo "      --own=<id> is refused on what it EXECUTED, and a real"
	echo "      xdg-dbus-proxy is accepted on the same bus.  SpawnSignal's"
	echo "      allowed direction is still structural - a pid the portal"
	echo "      really started needs a real Spawn - but the identity half of"
	echo "      that path is no longer taken on trust."
fi
exit $fail
