#!/bin/sh
# check-clean.sh - fail on flatpkg pollution: a live jail, or a mount under
# its per-launch root.  The whole-table diff is printed but not failed on:
# the baseline is the host's, so its verdict would not be flatpkg's.
BASELINE="$(dirname "$0")/mount-baseline.txt"

# What flatpkg leaves behind, as ONE expression shared with --clean: a
# second list of "these are flatpkg's" is a list to forget, and the cleaner
# would then remove things the check had called clean.
FPKG_MOUNTS_RE='/var/run/flatpkg/|on /tmp/(mt|fpkg-|lt[0-9])'

# strip header comments from the baseline before comparing
grep -v '^#' "$BASELINE" > /tmp/.fpkg-baseline.txt

mount | sort > /tmp/.fpkg-mount-now.txt
if diff -q /tmp/.fpkg-baseline.txt /tmp/.fpkg-mount-now.txt >/dev/null 2>&1; then
	echo "mount table: CLEAN (matches baseline)"
else
	echo "mount table: differs from the baseline (reported, not failed on"
	echo "  - only flatpkg's own mounts are this guard's business):"
	diff /tmp/.fpkg-baseline.txt /tmp/.fpkg-mount-now.txt | grep '^>' | sed 's/^> /  /'
fi
rm -f /tmp/.fpkg-mount-now.txt /tmp/.fpkg-baseline.txt

JAILS=$(jls -q jid 2>/dev/null | wc -l | tr -d ' ')
LEFTOVER=$(mount | grep -cE "$FPKG_MOUNTS_RE" || true)
echo "live jails: $JAILS (should be 0)"
echo "flatpkg mounts still up: $LEFTOVER (should be 0)"

fail=0
if [ "$JAILS" != 0 ]; then
	echo "FAILED: $JAILS jail(s) are still alive."
	echo "        mdo jls"
	echo "        mdo jail -r \$(mdo jls -q jid | head -1)"
	fail=1
fi
if [ "${LEFTOVER:-0}" != 0 ]; then
	echo "FAILED: $LEFTOVER flatpkg mount(s) are still up:"
	mount | grep -E "$FPKG_MOUNTS_RE" | sed 's/^/        /'
	echo "        mdo sh $(dirname "$0")/check-clean.sh --clean"
	fail=1
fi
if [ "$fail" -eq 0 ]; then
	echo "OK: no jail, and no flatpkg mount, left behind."
fi

if [ "$1" = "--clean" ]; then
	echo "cleaning..."
	for j in $(jls -q jid 2>/dev/null); do jail -r "$j" 2>/dev/null; done
	for m in $(mount | grep -E "$FPKG_MOUNTS_RE" | awk '{print $3}'); do
		mkdir -p "$m" 2>/dev/null   # reach orphaned mountpoints
		umount -f "$m" 2>/dev/null || true
	done
	rm -rf /var/run/flatpkg 2>/dev/null
	echo "done (verify with: sh tests/check-clean.sh)"
fi

exit $fail
