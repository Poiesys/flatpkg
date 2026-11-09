#!/bin/sh
# jail-root-gc.sh - a jail root no jail refers to is swept at daemon startup:
# whatever is still mounted under it is unmounted first, even a mount flatpkg
# never recorded, and then the tree goes.  A root a live jail still points at
# is left alone.
#
# Needs root (mdo) to make the leaked tree, its mount and the live jail, and a
# scratch daemon to run the sweep.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tests/scratchd.sh"

command -v mdo >/dev/null 2>&1 ||
	{ echo "SKIP: mdo not found; the sweep runs as root"; exit 0; }
mdo -u root true </dev/null >/dev/null 2>&1 ||
	{ echo "SKIP: mdo cannot give root without a prompt"; exit 0; }

fail=0
note() { echo "FAIL: $*"; fail=1; }

RUN=/var/run/flatpkg
leaked="$RUN/leaked-gc.$$.a"
alive="$RUN/leaked-gc.$$.b"
jname="leaked-gc-$$"

cleanup() {
	scratchd_stop
	mdo jail -r "$jname" >/dev/null 2>&1 || true
	mdo sh -c "umount '$leaked/mnt' 2>/dev/null; rm -rf '$leaked' '$alive'" \
		>/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

# A root nobody owns, with a mount that never reached sb_track().
mdo sh -c "mkdir -p '$leaked/mnt' '$alive'"
mdo sh -c "mount -t tmpfs tmpfs '$leaked/mnt' && echo canary > '$leaked/mnt/canary'"
# A root a live jail points at: the sweep must not touch it.
mdo jail -c "path=$alive" "name=$jname" persist

scratchd_start

# The sweep runs before the daemon serves, and scratchd_start only waits for
# the socket; wait for the verdict instead of sampling it.
n=0
while [ -e "$leaked" ] && [ "$n" -lt 150 ]; do
	sleep 0.1
	n=$((n + 1))
done
[ ! -e "$leaked" ] ||
	note "a leaked root with a mount flatpkg never recorded was not swept"
grep -q 'removed leaked sandbox' "$SD_LOG" 2>/dev/null ||
	note "the sweep removed the tree without saying so"
[ -d "$alive" ] ||
	note "the sweep removed the root a live jail points at"
mdo jail -r "$jname" >/dev/null 2>&1 || true

if [ "$fail" -eq 0 ]; then
	echo "PASS: the startup sweep unmounts and removes a leaked jail root, and"
	echo "      leaves a root a live jail still points at"
fi
exit "$fail"
