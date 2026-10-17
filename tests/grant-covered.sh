#!/bin/sh
# grant-covered.sh - mount_covering() skips a grant already visible through an
# earlier nullfs mount, BEFORE mkdir_p() makes a mount point for it: resolving
# through the home mount, that mkdir creates the directory in the user's real home.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
fail=0
note() { echo "FAIL: $*"; fail=1; }

tmp=$(mktemp -d /tmp/.fpkg-covered.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

# shellcheck source=tests/commentlib.sh
. "$(dirname "$0")/commentlib.sh"
strip_comments < "$ROOT/src/daemon/sandbox_root.c" > "$tmp/sb.c"

fn=$(carve_function '^mount_covering' "$tmp/sb.c" || true)
printf '%s\n' "$fn" | grep -q 'return NULL' || {
	note "could not carve mount_covering() out of src/daemon/sandbox_root.c"
	exit 1
}

CC_BIN=${CC:-cc}
if ! command -v "$CC_BIN" >/dev/null 2>&1; then
	echo "SKIP: no C compiler ($CC_BIN), so the behaviour claim cannot run"
	exit 0
fi

{
	cat <<'EOF'
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef struct { char* fstype; char* from; char* to; } sb_mount_t;
EOF
	printf '%s\n' "$fn"
	cat <<'EOF'
static int bad;
static void expect(const char* name, const sb_mount_t* ms, size_t n,
		   const char* from, const char* to, int want) {
	const sb_mount_t* r = mount_covering(ms, n, from, to);
	int got = r == NULL ? -1 : (int)(r - ms);
	if (got == want) printf("ok   %s\n", name);
	else { printf("FAIL: %s: covered by entry %d, wanted %d\n", name, got, want); bad = 1; }
}
int main(void) {
	sb_mount_t home[] = {{"nullfs", "/home/u", "/jail/home/u"}};
	expect("a grant under a granted home is covered", home, 1,
	       "/home/u/.config/kdeglobals", "/jail/home/u/.config/kdeglobals", 0);
	expect("the mount itself is not a proper descendant", home, 1,
	       "/home/u", "/jail/home/u", -1);
	expect("a different suffix is not the same file", home, 1,
	       "/home/u/.config/a", "/jail/home/u/.config/b", -1);
	expect("/home/u is not an ancestor of /home/u2/x", home, 1,
	       "/home/u2/x", "/jail/home/u2/x", -1);
	sb_mount_t tm[] = {{"tmpfs", "/home/u", "/jail/home/u"}};
	expect("a tmpfs is not a view of the host", tm, 1,
	       "/home/u/.config/k", "/jail/home/u/.config/k", -1);
	sb_mount_t nosrc[] = {{"nullfs", NULL, "/jail/home/u"}};
	expect("a mount with no source covers nothing", nosrc, 1,
	       "/home/u/.config/k", "/jail/home/u/.config/k", -1);
	expect("an empty list covers nothing", NULL, 0,
	       "/home/u/.config/k", "/jail/home/u/.config/k", -1);
	sb_mount_t two[] = {{"tmpfs", "/x", "/jail/x"},
			    {"nullfs", "/home/u", "/jail/home/u"}};
	expect("a cover that is not the first entry is found", two, 2,
	       "/home/u/.config/k", "/jail/home/u/.config/k", 1);
	sb_mount_t skew[] = {{"nullfs", "/home/u", "/jail/other"}};
	expect("an ancestor of the host path alone is not enough", skew, 1,
	       "/home/u/.config/k", "/jail/home/u/.config/k", -1);
	return bad;
}
EOF
} > "$tmp/h.c"

if ! "$CC_BIN" -std=gnu11 -Wall -Wextra -Werror -o "$tmp/h" "$tmp/h.c" 2> "$tmp/cc.log"; then
	note "the carved mount_covering() no longer compiles on its own"
	sed 's/^/       | /' "$tmp/cc.log"
	exit 1
fi
"$tmp/h" > "$tmp/run.out" || fail=1
cat "$tmp/run.out"
n=$(grep -c '^ok' "$tmp/run.out" || true)
[ "$n" -ge 9 ] || note "only $n behaviour case(s) passed; the harness lost some"

# ---- 2. order: the check precedes the mount point ------------------------
call=$(grep -n 'mount_covering(' "$tmp/sb.c" | sed -n 2p | cut -d: -f1)
mk=$(grep -n "cannot create mount point for '%s', skipping" "$tmp/sb.c" | head -1 | cut -d: -f1)
if [ -z "$call" ] || [ -z "$mk" ]; then
	note "could not find the call to mount_covering() or the grant's mkdir_p() in the grant loop"
elif [ "$call" -ge "$mk" ]; then
	note "mount_covering() is called after the grant's mount point is made (line $call, mkdir at $mk): the directory is already in the user's home"
elif [ $((mk - call)) -gt 60 ]; then
	note "mount_covering() (line $call) is nowhere near the grant's mkdir_p() (line $mk); it guards some other path"
fi

[ $fail -eq 0 ] &&
	echo "PASS: a grant inside an earlier mount is skipped before its mount point is made ($n cases)"
exit $fail
