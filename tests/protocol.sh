#!/bin/sh
# protocol.sh - the client/daemon wire protocol (src/shared/protocol.c).  flatpkgd
# is root and this parses bytes an unprivileged process chose, so it is tested as
# hostile input.  protocol.c itself is compiled into the harness.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/src/shared/protocol.c"
HARNESS="$ROOT/tests/protocol-test.c"

if ! command -v cc >/dev/null 2>&1; then
	echo "SKIP: no cc in PATH"
	exit 0
fi

tmp=$(mktemp -d /tmp/.fpkg-protocol-test.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

san=""
echo 'int main(void) { return 0; }' > "$tmp/probe.c"
if cc -fsanitize=address,undefined -o "$tmp/probe" "$tmp/probe.c" >/dev/null 2>&1 &&
    "$tmp/probe" >/dev/null 2>&1; then
	san="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
else
	echo "note: no sanitizer runtime; running without it"
fi

# $san is deliberately unquoted: it is zero or two words.
cc -O1 -g -Wall -Wextra -Werror $san -I"$ROOT/include" \
	-o "$tmp/protocol-test" "$HARNESS" "$SRC" -lnv
"$tmp/protocol-test"
