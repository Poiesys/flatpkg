#!/bin/sh
# jail-root-gc.sh - a jail root no jail refers to is swept at daemon startup:
# whatever is still mounted under it is unmounted first, even a mount flatpkg
# never recorded, and then the tree goes.  A root a live jail points at, a root
# whose worker still holds the lock, and a name that is not a jail root are all
# left alone.
#
# Needs root (mdo) to make the trees, the mount and the live jail, and a scratch
# daemon to run the sweep.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tests/scratchd.sh"

command -v cc >/dev/null 2>&1 ||
	{ echo "SKIP: no cc for the lock holder"; exit 0; }
command -v mdo >/dev/null 2>&1 ||
	{ echo "SKIP: mdo not found; the sweep runs as root"; exit 0; }
mdo -u root true </dev/null >/dev/null 2>&1 ||
	{ echo "SKIP: mdo cannot give root without a prompt"; exit 0; }

fail=0
note() { echo "FAIL: $*"; fail=1; }

RUN=/var/run/flatpkg
leaked="$RUN/leaked-gc.$$.aaa111"
alive="$RUN/leaked-gc.$$.bbb222"
held="$RUN/held-gc.$$.ccc333"
other="$RUN/not-a-jail-root"
jname="leaked-gc-$$"
T=$(mktemp -d /tmp/.fpkg-jailgc.XXXXXX)

cleanup() {
	scratchd_stop
	if [ -f "$T/holder.pid" ]; then
		mdo kill "$(cat "$T/holder.pid")" >/dev/null 2>&1 || true
	fi
	mdo jail -r "$jname" >/dev/null 2>&1 || true
	mdo sh -c "umount '$leaked/mnt' 2>/dev/null;
	    rm -rf '$leaked' '$alive' '$held' '$other'" >/dev/null 2>&1 || true
	rm -rf "$T"
}
trap cleanup EXIT INT TERM

# A root nobody owns, with a mount that never reached sb_track().
mdo sh -c "mkdir -p '$leaked/mnt' '$alive' '$held' '$other'"
mdo sh -c "mount -t tmpfs tmpfs '$leaked/mnt' &&
    echo canary > '$leaked/mnt/canary'"
# A root a live jail points at: the sweep must not touch it.
mdo jail -c "path=$alive" "name=$jname" persist
# A root a worker still holds (a launch in progress): the lock says it is live.
mdo sh -c ": > '$held/.lock'; echo held > '$held/canary';
    echo other > '$other/canary'"
cat > "$T/hold.c" <<'EOF'
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <unistd.h>

int main(int argc, char** argv) {
	int fd;
	(void)argc;
	fd = open(argv[1], O_RDONLY);
	if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) return 1;
	if (argv[2] != NULL) {
		FILE* f = fopen(argv[2], "w");

		if (f != NULL) {
			fprintf(f, "%ld\n", (long)getpid());
			fclose(f);
		}
	}
	for (;;) pause();
}
EOF
cc -O1 -o "$T/hold" "$T/hold.c" || exit 1
mdo "$T/hold" "$held/.lock" "$T/holder.pid" &
sleep 1

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
[ -d "$held" ] ||
	note "the sweep removed a root a worker still holds the lock on"
[ -d "$other" ] ||
	note "the sweep removed a directory that is not a jail root"
mdo jail -r "$jname" >/dev/null 2>&1 || true

if [ "$fail" -eq 0 ]; then
	echo "PASS: the startup sweep unmounts and removes a leaked jail root, and"
	echo "      leaves a live jail's root, a held root and a non-root name alone"
fi
exit "$fail"
