#!/bin/sh
# daemon-limits.sh - the daemon forks one root worker per connection and
# serves the event broker from its own single loop, so both are bounded: a
# connection is refused before the fork when the broker is full (or when one
# user already holds its share), the frames the broker sends are non-blocking
# so a saturated listener cannot stall every other client, a worker that never
# says HELLO is dropped on a timeout, and an authenticated client that asks
# for nothing is reaped rather than parked in the pool forever.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

. "$ROOT/tests/commentlib.sh"

T=$(mktemp -d /tmp/.fpkg-dlim.XXXXXX)
trap 'rm -rf "$T"' EXIT

strip_comments "$ROOT/src/daemon/bus.c" > "$T/bus.c"
strip_comments "$ROOT/src/daemon/daemon.c" > "$T/daemon.c"
strip_comments "$ROOT/src/daemon/ops/typed_worker.c" > "$T/worker.c"

carve_function '^static int send_frame\(' "$T/bus.c" > "$T/send.inc" ||
	{ echo "FAIL: send_frame() not found"; exit 1; }
carve_function '^int daemon_main\(' "$T/daemon.c" > "$T/main.inc" ||
	{ echo "FAIL: daemon_main() not found"; exit 1; }
carve_function '^static void hello_request\(' "$T/worker.c" > "$T/hello.inc" ||
	{ echo "FAIL: hello_request() not found"; exit 1; }

bug() {
	echo "FAIL: $1"
	exit 1
}

pos() {
	grep -n "$1" "$2" | head -1 | cut -d: -f1
}

check() {
	send=$1 main=$2 hello=$3 worker=$4

	grep -q 'MSG_DONTWAIT' "$send" ||
		bug "a broker frame blocks the daemon's loop"
	if grep -q 'send_all(' "$T/bus.c"; then
		bug "a blocking send_all() is still called"
	fi

	count=$(pos 'broker_peer_count() >= BUS_MAX_PEERS' "$main")
	fork=$(pos 'pid = fork()' "$main")
	[ -n "$count" ] || bug "no connection cap before the fork"
	[ -n "$fork" ] || bug "daemon_main() no longer forks a worker"
	[ "$count" -lt "$fork" ] ||
		bug "the connection cap is checked after the worker is forked"
	grep -q 'broker_peer_count_uid(uid) >= BUS_MAX_PEERS_PER_UID' "$main" ||
		bug "one user can hold every connection in the pool"
	grep -q 'SO_RCVTIMEO' "$main" ||
		bug "a connection that never sends a frame is never timed out"

	grep -q 'SO_RCVTIMEO' "$hello" ||
		bug "the timeout is not cleared at HELLO, so an idle client dies"
	grep -q 'if (c.hello && c.nsubs == 0) {' "$worker" ||
		bug "an authenticated idle connection is never judged"
	grep -q 'idle >= FP_IDLE_TIMEOUT' "$worker" ||
		bug "an authenticated idle connection is never reaped"
	grep -q 'timeout = (int)(FP_IDLE_TIMEOUT - idle) \* 1000' "$worker" ||
		bug "the idle reaper does not wake the worker to run"

	# The boundary's own presentation: a socket or a directory the world can
	# write is a control socket anybody can unlink.
	grep -q 'mkdir_p(FPKG_RUNDIR, 0770)' "$main" ||
		bug "the run directory is not the flatpkg group's alone"
	grep -q 'chmod(FPKG_SOCKET, 0660)' "$main" ||
		bug "the control socket is world-writable"

	# An event is rendered by clients this worker does not own.
	grep -q 'event_text_safe' "$worker" ||
		bug "an event can carry a control byte"
	grep -q 'event_ref_ok(row->ref)' "$worker" ||
		bug "a changeset ref is announced without the grammar"
}

( check "$T/send.inc" "$T/main.inc" "$T/hello.inc" "$T/worker.c" )

# --- the plants ----------------------------------------------------------
prove() {
	what=$1
	shift
	if ( "$@" ) >/dev/null 2>&1; then
		echo "FAIL: the guard passes with $what"
		exit 1
	fi
}

sed 's/(nowait ? MSG_DONTWAIT : 0)/(0)/' "$T/send.inc" \
	> "$T/plant-send.inc"
if cmp -s "$T/send.inc" "$T/plant-send.inc"; then
	echo "FAIL: the send plant was not applied"
	exit 1
fi
prove "a blocking broker send" \
	check "$T/plant-send.inc" "$T/main.inc" "$T/hello.inc" "$T/worker.c"

sed 's/broker_peer_count() >= BUS_MAX_PEERS/0/' "$T/main.inc" \
	> "$T/plant-cap.inc"
prove "no connection cap" \
	check "$T/send.inc" "$T/plant-cap.inc" "$T/hello.inc" "$T/worker.c"

sed 's/broker_peer_count_uid(uid) >= BUS_MAX_PEERS_PER_UID/0/' "$T/main.inc" \
	> "$T/plant-uid.inc"
prove "no per-user share of the pool" \
	check "$T/send.inc" "$T/plant-uid.inc" "$T/hello.inc" "$T/worker.c"

sed 's/(void)setsockopt(c->conn, SOL_SOCKET, SO_RCVTIMEO, &tv,/(void)tv;/' \
	"$T/hello.inc" > "$T/plant-hello.inc"
if cmp -s "$T/hello.inc" "$T/plant-hello.inc"; then
	echo "FAIL: the HELLO plant was not applied"
	exit 1
fi
prove "the timeout left on an idle client" \
	check "$T/send.inc" "$T/main.inc" "$T/plant-hello.inc" "$T/worker.c"

sed 's/if (c.hello \&\& c.nsubs == 0) {/if (0) {/' "$T/worker.c" \
	> "$T/plant-idle.inc"
if cmp -s "$T/worker.c" "$T/plant-idle.inc"; then
	echo "FAIL: the idle plant was not applied"
	exit 1
fi
prove "an authenticated connection never reaped" \
	check "$T/send.inc" "$T/main.inc" "$T/hello.inc" "$T/plant-idle.inc"

sed 's/mkdir_p(FPKG_RUNDIR, 0770)/mkdir_p(FPKG_RUNDIR, 0775)/' "$T/main.inc" \
	> "$T/plant-rundir.inc"
prove "a group-writable run directory" \
	check "$T/send.inc" "$T/plant-rundir.inc" "$T/hello.inc" "$T/worker.c"

sed 's/chmod(FPKG_SOCKET, 0660)/chmod(FPKG_SOCKET, 0666)/' "$T/main.inc" \
	> "$T/plant-socket.inc"
prove "a world-writable control socket" \
	check "$T/send.inc" "$T/plant-socket.inc" "$T/hello.inc" "$T/worker.c"

sed 's/event_ref_ok(row->ref)/1/' "$T/worker.c" > "$T/plant-event.inc"
if cmp -s "$T/worker.c" "$T/plant-event.inc"; then
	echo "FAIL: the event-ref plant was not applied"
	exit 1
fi
prove "a ref announced without the grammar" \
	check "$T/send.inc" "$T/main.inc" "$T/hello.inc" "$T/plant-event.inc"

echo "PASS: the worker count is capped before the fork and shared per user,"
echo "      the broker never blocks its loop, a silent connection is timed"
echo "      out until HELLO, an idle one is reaped after it, and the run"
echo "      directory and control socket belong to the flatpkg group"
