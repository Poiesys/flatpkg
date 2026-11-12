#!/bin/sh
# bus-unit.sh - the daemon's event bus without a daemon: fan-out, the
# late-subscriber snapshot and cancel, with frames as opaque bytes.
# Broker and worker sides are forked and joined by the daemon's socketpairs.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d /tmp/.fpkg-bus.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

if ! cc -O2 -Wall -Wextra -Werror -I"$ROOT/include" -I"$ROOT/src/daemon" \
    -o "$tmp/bus-unit" "$ROOT/tests/bus-unit.c" "$ROOT/src/daemon/bus.c" \
    "$ROOT/src/shared/protocol.c" -lnv \
    >"$tmp/cc.log" 2>&1; then
	echo "FAIL: the bus unit test does not build:"
	tail -10 "$tmp/cc.log" | sed 's/^/        /'
	exit 1
fi
if ! timeout 30 "$tmp/bus-unit" >"$tmp/out" 2>&1; then
	echo "FAIL: the event bus unit test failed:"
	sed 's/^/        /' "$tmp/out" | tail -10
	exit 1
fi
cat "$tmp/out"
