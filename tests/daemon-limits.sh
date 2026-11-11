#!/bin/sh
# daemon-limits.sh - the daemon forks one root worker per connection and
# serves the event broker from its own single loop, so both are bounded: a
# connection is refused before the fork when the broker is full, the frames
# the broker sends are non-blocking so a saturated listener cannot stall every
# other client, and a worker that never says HELLO is dropped on a timeout.
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
	send=$1 main=$2 hello=$3

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
	grep -q 'SO_RCVTIMEO' "$main" ||
		bug "a connection that never sends a frame is never timed out"

	grep -q 'SO_RCVTIMEO' "$hello" ||
		bug "the timeout is not cleared at HELLO, so an idle client dies"
}

( check "$T/send.inc" "$T/main.inc" "$T/hello.inc" )

# --- the plants ----------------------------------------------------------
prove() {
	what=$1
	shift
	if ( "$@" ) >/dev/null 2>&1; then
		echo "FAIL: the guard passes with $what"
		exit 1
	fi
}

sed 's/MSG_NOSIGNAL | MSG_DONTWAIT/MSG_NOSIGNAL/' "$T/send.inc" \
	> "$T/plant-send.inc"
prove "a blocking broker send" \
	check "$T/plant-send.inc" "$T/main.inc" "$T/hello.inc"

sed 's/broker_peer_count() >= BUS_MAX_PEERS/0/' "$T/main.inc" \
	> "$T/plant-cap.inc"
prove "no connection cap" \
	check "$T/send.inc" "$T/plant-cap.inc" "$T/hello.inc"

sed 's/(void)setsockopt(c->conn, SOL_SOCKET, SO_RCVTIMEO, \&tv,/(void)tv;/' \
	"$T/hello.inc" > "$T/plant-hello.inc"
if cmp -s "$T/hello.inc" "$T/plant-hello.inc"; then
	echo "FAIL: the HELLO plant was not applied"
	exit 1
fi
prove "the timeout left on an idle client" \
	check "$T/send.inc" "$T/main.inc" "$T/plant-hello.inc"

echo "PASS: the worker count is capped before the fork, the broker never"
echo "      blocks its loop, and a silent connection is timed out until HELLO"
