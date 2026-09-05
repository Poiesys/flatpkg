#!/bin/sh
# port-dist.sh - the port's distinfo describes the tarball make dist writes.
# A stale distinfo is a port that cannot fetch, and the hash changes whenever a
# tracked source file does, so it is checked rather than remembered.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT="$ROOT/ports/sysutils/flatpkg"
tmp=$(mktemp -d /tmp/.fpkg-portdist.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
fail=0
note() { echo "FAIL: $*"; fail=1; }

make -s -C "$ROOT" dist DISTDIR="$tmp" >/dev/null ||
	{ echo "FAIL: make dist failed"; exit 1; }
tar="$tmp/flatpkg-0.1.0.tar.gz"
[ -f "$tar" ] || { echo "FAIL: make dist wrote no tarball"; exit 1; }

want_sha=$(awk '/^SHA256 / { print $4 }' "$PORT/distinfo")
want_size=$(awk '/^SIZE / { print $4 }' "$PORT/distinfo")
got_sha=$(sha256 -q "$tar")
got_size=$(stat -f '%z' "$tar")
[ -n "$want_sha" ] || note "distinfo has no SHA256 line"
[ -n "$want_size" ] || note "distinfo has no SIZE line"
[ "$want_sha" = "$got_sha" ] ||
	note "distinfo SHA256 is stale: run make dist and update it"
[ "$want_size" = "$got_size" ] ||
	note "distinfo SIZE is stale: run make dist and update it"

bad_root=$(tar -tf "$tar" | grep -v '^flatpkg-0.1.0/' | head -1)
[ -z "$bad_root" ] ||
	note "the tarball has an entry outside flatpkg-0.1.0/: $bad_root"
tar -tf "$tar" | grep -q '^flatpkg-0.1.0/ports/' &&
	note "the tarball carries ports/, which contains its own distinfo"

grep -q 'MASTER_SITES=.*file://' "$PORT/Makefile" ||
	note "the port does not fetch the tarball from a file:// master site"

if [ "$fail" -eq 0 ]; then
	echo "PASS: distinfo matches make dist, the tarball root is right,"
	echo "      and the port fetches it from the checkout"
fi
exit "$fail"
