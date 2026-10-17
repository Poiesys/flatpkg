#!/bin/sh
# nullfs-duplicate.sh - check m_nullfs()'s duplicate-mount guard against what the
# kernel does: a nullfs over the same lower vnode gets EDEADLK, so the guard must
# call that a no-op.  Needs vfs.usermount=1; the function is extracted verbatim.
set -e

SRC="$(dirname "$0")/../src/daemon/sandbox_root.c"
HARNESS="$(dirname "$0")/nullfs-duplicate.c"

if [ "$(sysctl -n vfs.usermount 2>/dev/null)" != "1" ]; then
	echo "SKIP: vfs.usermount=0 (mounts would need root)"
	exit 0
fi
if ! command -v cc >/dev/null 2>&1; then
	echo "SKIP: no cc in PATH"
	exit 0
fi

tmp=$(mktemp -d /tmp/.fpkg-nullfs-test.XXXXXX)
root="$tmp/tree"
cleanup() {
	for _ in 1 2 3 4 5; do
		umount -f "$root/T1" 2>/dev/null || break
	done
	rm -rf "$tmp"
}
trap cleanup EXIT

line=$(grep -n '^static bool nullfs_already_mounted(const char\* from, const char\* to)' \
	"$SRC" | cut -d: -f1)
if [ -z "$line" ]; then
	echo "FAIL: nullfs_already_mounted() not found in $SRC"
	exit 1
fi
# From the `static bool` line to the closing brace.  Start at `line`, not
# `line - 1`: one up begins inside the comment above the function.
awk -v s="$line" 'NR>=s {print; if (NR>s && $0=="}") exit}' "$SRC" \
	> "$tmp/nlfn.inc"

cc -O0 -Wall -Wextra -I"$tmp" -o "$tmp/nullfs-duplicate" "$HARNESS"
echo "note: 'Resource deadlock avoided' / 'not a file system root' on"
echo "      stderr are the expected answers to the probe mounts."
"$tmp/nullfs-duplicate" "$root"
