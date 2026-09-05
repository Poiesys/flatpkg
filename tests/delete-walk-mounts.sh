#!/bin/sh
# delete-walk-mounts.sh - every tree-removal walk must refuse to cross a
# mount point.  FTW_PHYS/FTS_PHYSICAL only stop symlink following, so without
# FTW_MOUNT/FTS_XDEV a delete descends into the sandbox's nullfs mounts.
set -e

SRC="$(dirname "$0")/../src"
fail=0

# The flag list can wrap onto the next line, so look a small window past
# the call rather than on its own line.
# report <file> <lineno> <call-regex> <required-flag>
report() {
	if grep -A4 -E "$3" "$1" | grep -q "$4"; then
		return 0
	fi
	echo "FAIL $(basename "$1"): $3 without $4 (near line $2):"
	grep -nE "$3" "$1" | sed 's/^/    /'
	fail=1
}

echo "checking tree-removal walks are mount-safe"
echo

for f in "$SRC"/*.c "$SRC"/*/*.c "$SRC"/*/*/*.c; do
	[ -f "$f" ] || continue
	# nftw(...) used as a removal walk -> must carry FTW_MOUNT.
	if grep -n "nftw(" "$f" | grep -q "rm_cb"; then
		n=$(grep -n "nftw(.*rm_cb" "$f" | head -1 | cut -d: -f1)
		report "$f" "$n" "nftw\(.*rm_cb" FTW_MOUNT
	fi
	# fts_open() that walks physically -> must carry FTS_XDEV.
	if grep -n "fts_open(" "$f" | grep -q "FTS_PHYSICAL"; then
		n=$(grep -n "fts_open(.*FTS_PHYSICAL\|FTS_PHYSICAL.*fts_open" "$f" | head -1 | cut -d: -f1)
		[ -n "$n" ] && report "$f" "$n" "fts_open\(" FTS_XDEV
	fi
done

if [ "$fail" -eq 0 ]; then
	echo "ok   no nftw(rm_cb) walk without FTW_MOUNT"
	echo "ok   no fts_open() walk without FTS_XDEV"
	echo
	echo "PASS: every delete walk is mount-safe"
	exit 0
fi
echo
echo "FAILED: a delete walk can cross a mount point"
exit 1
