#!/bin/sh
# summary-cache.sh - a .flatpakrepo file is the key's source, so it comes over
# https and it is small; and the summary cache the planner trusts is root's,
# written by a pull and owned by the daemon rather than accepted as whoever
# pulled left it.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FLATPKG="$ROOT/build/flatpkg"
REPO="$ROOT/src/ostree.c"
CMDS="$ROOT/src/ostree_cmds.c"

[ -f "$REPO" ] && [ -f "$CMDS" ] || { echo "FAIL: sources are missing"; exit 1; }
. "$ROOT/tests/commentlib.sh"

T=$(mktemp -d /tmp/.fpkg-sumcache.XXXXXX)
trap 'rm -rf "$T"' EXIT

strip_comments "$REPO" > "$T/ostree.c"
strip_comments "$CMDS" > "$T/cmds.c"
carve_function '^int cmd_remote_add\(' "$T/cmds.c" > "$T/remote.inc" ||
	{ echo "FAIL: cmd_remote_add() not found"; exit 1; }
carve_function '^static char\* summary_xa_metadata\(' "$T/ostree.c" \
	> "$T/xa.inc" || { echo "FAIL: summary_xa_metadata() not found"; exit 1; }
carve_function '^char\* summary_ref_commit\(' "$T/ostree.c" > "$T/commit.inc" ||
	{ echo "FAIL: summary_ref_commit() not found"; exit 1; }
carve_function '^int ostree_run_cb\(' "$T/ostree.c" > "$T/run.inc" ||
	{ echo "FAIL: ostree_run_cb() not found"; exit 1; }
carve_function '^static char\* zlib_decompress_str\(' "$T/ostree.c" \
	> "$T/zlib.inc" || { echo "FAIL: zlib_decompress_str() not found"; exit 1; }
strip_comments "$ROOT/src/daemon/daemon.c" > "$T/daemon.c"
carve_function '^int daemon_main\(' "$T/daemon.c" > "$T/main.inc" ||
	{ echo "FAIL: daemon_main() not found"; exit 1; }

bug() {
	echo "FAIL: $1"
	exit 1
}

check() {
	remote=$1 xa=$2 commit=$3 run=$4 main=$5 zlib=$6

	hits=$(grep -c 'strncmp(\(from\|url\), "https://", 8) != 0' "$remote" || true)
	[ "$hits" -eq 2 ] ||
		bug "cmd_remote_add() fetches a repo file over a scheme it cannot trust ($hits of 2)"
	grep -q 'fetch_url(from, REPO_FILE_MAX' "$remote" ||
		bug "the --from fetch has no size cap"
	grep -q 'fetch_url(url, REPO_FILE_MAX' "$remote" ||
		bug "the URL fetch has no size cap"

	grep -q 'summary_cache_trusted(path)' "$xa" ||
		bug "the xa.cache reader trusts a cache file it does not own"
	grep -q 'summary_cache_trusted(path)' "$commit" ||
		bug "the ref-commit reader trusts a cache file it does not own"
	grep -q '"pull"' "$run" ||
		bug "a pull no longer takes the cache back"
	grep -q 'ostree_summary_cache_own()' "$run" ||
		bug "nothing takes the cache back after a pull"
	grep -q 'ostree_summary_cache_own()' "$main" ||
		bug "the daemon does not repair the cache at startup"

	# The xa.cache entry is deflated and hostile until the signature says
	# otherwise: the expansion needs a ceiling, not just doublings.
	grep -q 'SUMMARY_META_MAX' "$zlib" ||
		bug "a summary's metadata decompresses without a size ceiling"
	grep -q 'dstlen <= SUMMARY_META_MAX' "$zlib" ||
		bug "the expansion loop does not stop at the ceiling"
}

( check "$T/remote.inc" "$T/xa.inc" "$T/commit.inc" "$T/run.inc" \
	"$T/main.inc" "$T/zlib.inc" )

# --- the plants ----------------------------------------------------------
prove() {
	what=$1
	shift
	if ( "$@" ) >/dev/null 2>&1; then
		echo "FAIL: the guard passes with $what"
		exit 1
	fi
}

sed 's/strncmp(from, "https:\/\/", 8) != 0/0/' "$T/remote.inc" \
	> "$T/plant-http.inc"
prove "an http repo file accepted" \
	check "$T/plant-http.inc" "$T/xa.inc" "$T/commit.inc" "$T/run.inc" \
	"$T/main.inc" "$T/zlib.inc"

sed 's/if (!summary_cache_trusted(path)) {/if (0) {/' "$T/commit.inc" \
	> "$T/plant-cache.inc"
prove "an untrusted cache file read" \
	check "$T/remote.inc" "$T/xa.inc" "$T/plant-cache.inc" "$T/run.inc" \
	"$T/main.inc" "$T/zlib.inc"

# --- behaviour: the refusal and the ownership ---------------------------
if [ -x "$FLATPKG" ] && [ -S /var/run/flatpkg/control.sock ] &&
    id -Gn 2>/dev/null | tr ' ' '\n' | grep -qx flatpkg; then
	out=$("$FLATPKG" remote-add --no-color --from=http://127.0.0.1:9/x.flatpakrepo \
		fpkg-http-must-refuse 2>&1) && rc=0 || rc=$?
	[ "$rc" -ne 0 ] ||
		bug "remote-add accepted a plain-http repo file"
	printf '%s\n' "$out" | grep -q 'https' ||
		bug "the refusal does not say what is wrong: $out"
	"$FLATPKG" remote-list 2>/dev/null | grep -q '^fpkg-http-must-refuse\b' &&
		bug "the refused remote was added anyway"
	echo "ok   a plain-http repo file is refused before anything is written"
else
	echo "SKIP: the refusal needs the daemon and the flatpkg group"
fi

# The daemon takes the cache back at startup; whatever is there now must be
# his, or the readers would (rightly) ignore it.  A store with no cache is a
# store that has not pulled yet, which is not a failure.
cache=${FLATPKG_STORE:-/var/db/flatpkg}/repo/tmp/cache/summaries
if [ -d "$cache" ]; then
	bad=$(find "$cache" -maxdepth 1 \( -type f -o -type d \) \
		! -user root -print 2>/dev/null | head -3)
	[ -z "$bad" ] || bug "the summary cache is not root's: $bad"
	left=$(find "$cache" -maxdepth 1 -type f \
		\( -perm -g+w -o -perm -o+w \) -print 2>/dev/null | head -3)
	[ -z "$left" ] || bug "a group member can rewrite the cache: $left"
	echo "ok   the summary cache is root-owned and not group-writable"
else
	echo "SKIP: no summary cache in this store"
fi

# 3. the expansion back without a ceiling
sed 's/dstlen <= SUMMARY_META_MAX/dstlen > 0/' "$T/zlib.inc" \
	> "$T/plant-zlib.inc"
prove "an unbounded summary expansion" \
	check "$T/remote.inc" "$T/xa.inc" "$T/commit.inc" "$T/run.inc" \
	"$T/main.inc" "$T/plant-zlib.inc"

echo "PASS: a repo file is https-only and bounded, the summary cache is root's"
echo "      after a pull and at daemon startup, and its deflated metadata has"
echo "      a ceiling"
