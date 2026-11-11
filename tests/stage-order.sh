#!/bin/sh
# stage-order.sh - the order the per-launch tree is assembled in is what
# decides which mount the app sees: the host's own directories go down before
# the home overlay and before the explicit grants, the grants go shallow
# first, and a narrower grant mounts on top of the wider one it lives in.  A
# read-only grant under a wider mount used to be listed and then skipped, so
# the wider mount's permissions were the answer.  A socket in a shared
# directory is refused rather than mounted, on every path that stages one.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/src/daemon/sandbox_root.c"

[ -f "$SRC" ] || { echo "FAIL: $SRC is missing"; exit 1; }
. "$ROOT/tests/commentlib.sh"

T=$(mktemp -d /tmp/.fpkg-stage.XXXXXX)
trap 'rm -rf "$T"' EXIT

strip_comments "$SRC" > "$T/sb.c"
carve_function '^int sandbox_build_root\(' "$T/sb.c" > "$T/root.inc" ||
	{ echo "FAIL: sandbox_build_root() not found"; exit 1; }
carve_function '^static int stage_grants\(' "$T/sb.c" > "$T/grants.inc" ||
	{ echo "FAIL: stage_grants() not found"; exit 1; }
carve_function '^static int apply_grant\(' "$T/sb.c" > "$T/apply.inc" ||
	{ echo "FAIL: apply_grant() not found"; exit 1; }
carve_function '^static int stage_pulseaudio\(' "$T/sb.c" > "$T/pulse.inc" ||
	{ echo "FAIL: stage_pulseaudio() not found"; exit 1; }
carve_function '^static int m_tmpfs_sized\(' "$T/sb.c" > "$T/tmpfs.inc" ||
	{ echo "FAIL: m_tmpfs_sized() not found"; exit 1; }
carve_function '^static int m_tmpfs\(' "$T/sb.c" > "$T/tmpfs-default.inc" ||
	{ echo "FAIL: m_tmpfs() not found"; exit 1; }
carve_function '^void stage_dev_shm\(' "$T/sb.c" > "$T/shm.inc" ||
	{ echo "FAIL: stage_dev_shm() not found"; exit 1; }

pos() {
	grep -n "$1" "$2" | head -1 | cut -d: -f1
}

bug() {
	echo "FAIL: $1"
	exit 1
}

check() {
	root=$1 grants=$2 apply=$3 pulse=$4 tmpfs=$5 tmpfs_default=$6 shm=$7

	host=$(pos 'stage_host_mounts(' "$root")
	home=$(pos 'stage_home(' "$root")
	explicit=$(pos 'stage_grants(' "$root")
	overlays=$(pos 'stage_host_overlays(' "$root")

	[ -n "$host" ] || bug "the builder no longer lays down the host mounts"
	[ -n "$home" ] || bug "the builder no longer stages the home"
	[ -n "$explicit" ] || bug "the builder no longer stages grants"
	[ -n "$overlays" ] || bug "the builder no longer stages /run/host"
	[ "$host" -lt "$home" ] ||
		bug "the host mount covers the home overlay"
	[ "$home" -lt "$explicit" ] ||
		bug "a grant is mounted before the home overlay"
	[ "$explicit" -lt "$overlays" ] ||
		bug "/run/host is staged before the grants it mirrors"

	grep -q 'grant_depth(&plan->grants\[order\[j - 1\]\])' "$grants" ||
		bug "the grants are applied in metadata order, not shallow first"
	grep -q 'apply_grant(sb, plan, g, home_granted)' "$grants" ||
		bug "stage_grants() no longer applies the sorted order"

	grep -q 'cover_ro == want_ro' "$apply" ||
		bug "a covering mount is skipped whatever its permissions"
	grep -q 'strcmp(cover->from, g->host_path) == 0' "$apply" ||
		bug "the same source is not recognised as already visible"
	# The covering test must fall THROUGH to the mount: a return between
	# the test's closing brace and the line that says so skips it.
	if ! awk '/^[[:space:]]*$/ { next }
		  prev ~ /\}/ && /narrows/ { seen = 1 }
		  { prev = $0 }
		  END { exit seen ? 0 : 1 }' "$apply"; then
		bug "a covering mount is skipped even when its permissions differ"
	fi

	grep -q 'socket_dir_too_broad(pdir)' "$pulse" ||
		bug "pulse staging mounts a shared directory unchecked"

	# A tmpfs the app can fill carries a size, in both mount spellings.
	grep -q 'size=%llu' "$tmpfs" ||
		bug "a per-launch tmpfs has no size limit"
	grep -q '"-o"' "$tmpfs" ||
		bug "the mount(8) fallback drops the size option"
	grep -q 'TMPFS_SIZE_DEFAULT' "$tmpfs_default" ||
		bug "m_tmpfs() does not go through the sized mount"
	grep -q 'TMPFS_SIZE_SHM' "$shm" ||
		bug "/dev/shm is capped like every other tmpfs"
}

( check "$T/root.inc" "$T/grants.inc" "$T/apply.inc" "$T/pulse.inc" \
	"$T/tmpfs.inc" "$T/tmpfs-default.inc" "$T/shm.inc" )

# --- the plants ----------------------------------------------------------
prove() {
	what=$1
	shift
	if ( "$@" ) >/dev/null 2>&1; then
		echo "FAIL: the guard passes with $what"
		exit 1
	fi
}

# 1. the host mounts back after the grants
awk '
	/stage_host_mounts\(/ { host = $0; next }
	{ print }
	/stage_grants\(/ && !done { print host; done = 1 }
' "$T/root.inc" > "$T/plant-order.inc"
prove "the host mounts after the grants" \
	check "$T/plant-order.inc" "$T/grants.inc" "$T/apply.inc" "$T/pulse.inc" \
	"$T/tmpfs.inc" "$T/tmpfs-default.inc" "$T/shm.inc"

# 2. the grants back in metadata order
sed 's/grant_depth(&plan->grants\[order\[j - 1\]\])/0/' "$T/grants.inc" \
	> "$T/plant-depth.inc"
prove "the grants unsorted" \
	check "$T/root.inc" "$T/plant-depth.inc" "$T/apply.inc" "$T/pulse.inc" \
	"$T/tmpfs.inc" "$T/tmpfs-default.inc" "$T/shm.inc"

# 3. the narrower grant skipped again: a return right after the covering test
awk '/narrows/ { print "\t\t\treturn 0;" } { print }' "$T/apply.inc" \
	> "$T/plant-cover.inc"
if cmp -s "$T/apply.inc" "$T/plant-cover.inc"; then
	echo "FAIL: the covering plant was not applied"
	exit 1
fi
prove "the narrower grant skipped" \
	check "$T/root.inc" "$T/grants.inc" "$T/plant-cover.inc" "$T/pulse.inc" \
	"$T/tmpfs.inc" "$T/tmpfs-default.inc" "$T/shm.inc"

# 4. pulse staging without the shared-directory check
sed 's/if (socket_dir_too_broad(pdir)) {/if (0) {/' "$T/pulse.inc" \
	> "$T/plant-pulse.inc"
prove "pulse staging unchecked" \
	check "$T/root.inc" "$T/grants.inc" "$T/apply.inc" \
	"$T/plant-pulse.inc" "$T/tmpfs.inc" "$T/tmpfs-default.inc" \
	"$T/shm.inc"

# 5. the tmpfs back to an unbounded mount
sed 's/size=%llu/nosize/' "$T/tmpfs.inc" > "$T/plant-tmpfs.inc"
prove "an unbounded tmpfs" \
	check "$T/root.inc" "$T/grants.inc" "$T/apply.inc" "$T/pulse.inc" \
	"$T/plant-tmpfs.inc" "$T/tmpfs-default.inc" "$T/shm.inc"

echo "PASS: the host view, the home overlay and the grants are layered in"
echo "      that order, a narrower grant mounts on top, no socket staging"
echo "      path mounts a shared directory, and every tmpfs is bounded"
