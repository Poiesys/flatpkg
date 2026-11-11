#!/bin/sh
# summary-cache.sh - a .flatpakrepo file is the key's source, so it comes over
# https and it is small; and the summary cache the planner trusts is root's,
# written by a pull and owned by the daemon rather than accepted as whoever
# pulled left it.  Both the reader and the ownership repair judge the object
# they opened, not a path a swap can re-point.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FLATPKG="$ROOT/build/flatpkg"
REPO="$ROOT/src/ostree.c"
CMDS="$ROOT/src/ostree_cmds.c"

[ -f "$REPO" ] && [ -f "$CMDS" ] || { echo "FAIL: sources are missing"; exit 1; }
. "$ROOT/tests/commentlib.sh"

T=$(mktemp -d /tmp/.fpkg-sumcache.XXXXXX)
trap 'rm -rf "$T"' EXIT
# The environment the CLI half reads is owned here rather than inherited: a
# name the host decides is a verdict the host can change.
export HOME="$T" XDG_RUNTIME_DIR="$T/run"
export PATH="/usr/bin:/bin"
export TERM=dumb
export XDG_CURRENT_DESKTOP= DBUS_SESSION_BUS_ADDRESS= WAYLAND_DISPLAY=
export NO_COLOR= FORCE_COLOR=
mkdir -p "$XDG_RUNTIME_DIR"

strip_comments "$REPO" > "$T/ostree.c"
strip_comments "$CMDS" > "$T/cmds.c"
carve_function '^int cmd_remote_add\(' "$T/cmds.c" > "$T/remote.inc" ||
	{ echo "FAIL: cmd_remote_add() not found"; exit 1; }
carve_function '^static char\* summary_xa_metadata\(' "$T/ostree.c" \
	> "$T/xa.inc" || { echo "FAIL: summary_xa_metadata() not found"; exit 1; }
carve_function '^char\* summary_ref_commit\(' "$T/ostree.c" > "$T/commit.inc" ||
	{ echo "FAIL: summary_ref_commit() not found"; exit 1; }
carve_function '^static int summary_cache_open\(' "$T/ostree.c" \
	> "$T/open.inc" || { echo "FAIL: summary_cache_open() not found"; exit 1; }
carve_function '^static GBytes\* summary_cache_bytes\(' "$T/ostree.c" \
	> "$T/bytes.inc" || { echo "FAIL: summary_cache_bytes() not found"; exit 1; }
carve_function '^void ostree_summary_cache_own\(' "$T/ostree.c" \
	> "$T/own.inc" || { echo "FAIL: ostree_summary_cache_own() not found"; exit 1; }
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
	remote=$1 xa=$2 commit=$3 run=$4 main=$5 zlib=$6 open=$7 bytes=$8
	own=$9

	hits=$(grep -c 'strncmp(\(from\|url\), "https://", 8) != 0' "$remote" || true)
	[ "$hits" -eq 2 ] ||
		bug "cmd_remote_add() fetches a repo file over a scheme it cannot trust ($hits of 2)"
	grep -q 'fetch_url(from, REPO_FILE_MAX' "$remote" ||
		bug "the --from fetch has no size cap"
	grep -q 'fetch_url(url, REPO_FILE_MAX' "$remote" ||
		bug "the URL fetch has no size cap"
	# A repo file's bytes come from the CLI, which read them as the
	# caller: the worker must not open a caller-named file as root.
	if grep -q 'slurp_file(' "$remote"; then
		bug "the worker still reads a caller-named repo file"
	fi
	grep -q 'data' "$remote" ||
		bug "the worker ignores the caller-read repo bytes"

	# A reader maps the bytes of the file it judged, not of a path.
	grep -q 'summary_cache_bytes(path)' "$xa" ||
		bug "the xa.cache reader does not read through the cache opener"
	grep -q 'summary_cache_bytes(path)' "$commit" ||
		bug "the ref-commit reader does not read through the cache opener"
	if grep -q 'g_mapped_file_new(' "$xa" "$commit"; then
		bug "a cache reader maps the file by path"
	fi
	grep -q 'g_mapped_file_new_from_fd(fd' "$bytes" ||
		bug "the cache is mapped from a path, not from the judged descriptor"
	[ "$(grep -c 'O_NOFOLLOW' "$open" || true)" -eq 2 ] ||
		bug "the cache file or its directory is opened through a symlink"
	grep -q 'openat(dfd, base' "$open" ||
		bug "the cache file is opened by path, not inside the judged directory"
	grep -q 'fstat(dfd, &st)' "$open" ||
		bug "the cache directory is not judged by its descriptor"
	grep -q 'fstat(fd, &st)' "$open" ||
		bug "the cache file is not judged by its descriptor"
	grep -q 'st.st_uid != 0' "$open" ||
		bug "a cache object does not have to be root's"

	grep -q 'ostree_summary_cache_own()' "$run" ||
		bug "nothing takes the cache back after a tool run"
	if grep -q 'strcmp(argv' "$run"; then
		bug "the tool argv is scanned for a subcommand"
	fi
	grep -q 'ostree_summary_cache_own()' "$main" ||
		bug "the daemon does not repair the cache at startup"

	# The ownership repair opens, judges and changes the same objects.
	grep -q 'openat(cd, "summaries"' "$own" ||
		bug "the summaries directory is opened by path"
	grep -q '(void)fchmod(cd, 0755)' "$own" ||
		bug "the cache directory is chmod'ed by path"
	grep -q '(void)fchown(cd, 0, 0)' "$own" ||
		bug "the cache directory is chown'ed by path"
	grep -q '(void)fchmod(dfd, 0755)' "$own" ||
		bug "the summaries directory is chmod'ed by path"
	grep -q '(void)fchmod(fd, 0644)' "$own" ||
		bug "a cache file is chmod'ed by path"
	grep -q '(void)fchown(fd, 0, 0)' "$own" ||
		bug "a cache file is chown'ed by path"
	# Taking a file over would bless it: a group member's planted summary
	# is dropped, never made root's.
	grep -q 'unlinkat(dfd, de->d_name, 0)' "$own" ||
		bug "a cache file root did not write is adopted, not dropped"
	grep -q 'st.st_uid != 0' "$own" ||
		bug "the ownership repair does not ask whose the file is"
	if grep -E '(^|[^f])ch(mod|own)\(' "$own" >/dev/null; then
		bug "the ownership repair still changes a path by name"
	fi

	# The xa.cache entry is deflated and hostile until the signature says
	# otherwise: the expansion needs a ceiling, not just doublings.
	grep -q 'SUMMARY_META_MAX' "$zlib" ||
		bug "a summary's metadata decompresses without a size ceiling"
	grep -q 'dstlen <= SUMMARY_META_MAX' "$zlib" ||
		bug "the expansion loop does not stop at the ceiling"
}

( check "$T/remote.inc" "$T/xa.inc" "$T/commit.inc" "$T/run.inc" \
	"$T/main.inc" "$T/zlib.inc" "$T/open.inc" "$T/bytes.inc" "$T/own.inc" )

# --- the plants ----------------------------------------------------------
prove() {
	what=$1
	shift
	if ( check "$@" ) >/dev/null 2>&1; then
		echo "FAIL: the guard passes with $what"
		exit 1
	fi
}

sed 's/strncmp(from, "https:\/\/", 8) != 0/0/' "$T/remote.inc" \
	> "$T/plant-http.inc"
if cmp -s "$T/remote.inc" "$T/plant-http.inc"; then
	echo "FAIL: the http plant was not applied"
	exit 1
fi
prove "an http repo file accepted" "$T/plant-http.inc" "$T/xa.inc" \
	"$T/commit.inc" "$T/run.inc" "$T/main.inc" "$T/zlib.inc" \
	"$T/open.inc" "$T/bytes.inc" "$T/own.inc"

sed 's/g_mapped_file_new_from_fd(fd, FALSE, NULL)/g_mapped_file_new(path, FALSE, NULL)/' \
	"$T/bytes.inc" > "$T/plant-bytes.inc"
if cmp -s "$T/bytes.inc" "$T/plant-bytes.inc"; then
	echo "FAIL: the descriptor-map plant was not applied"
	exit 1
fi
prove "the cache mapped from a path" "$T/remote.inc" "$T/xa.inc" \
	"$T/commit.inc" "$T/run.inc" "$T/main.inc" "$T/zlib.inc" \
	"$T/open.inc" "$T/plant-bytes.inc" "$T/own.inc"

sed 's/O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC/O_DIRECTORY | O_CLOEXEC/' \
	"$T/open.inc" > "$T/plant-open.inc"
if cmp -s "$T/open.inc" "$T/plant-open.inc"; then
	echo "FAIL: the symlink plant was not applied"
	exit 1
fi
prove "a cache directory opened through a symlink" "$T/remote.inc" \
	"$T/xa.inc" "$T/commit.inc" "$T/run.inc" "$T/main.inc" "$T/zlib.inc" \
	"$T/plant-open.inc" "$T/bytes.inc" "$T/own.inc"

sed 's/(void)fchmod(fd, 0644);/(void)chmod(p, 0644);/' "$T/own.inc" \
	> "$T/plant-own.inc"
if cmp -s "$T/own.inc" "$T/plant-own.inc"; then
	echo "FAIL: the ownership plant was not applied"
	exit 1
fi
prove "a cache file chmod'ed by path" "$T/remote.inc" "$T/xa.inc" \
	"$T/commit.inc" "$T/run.inc" "$T/main.inc" "$T/zlib.inc" \
	"$T/open.inc" "$T/bytes.inc" "$T/plant-own.inc"

sed 's/dstlen <= SUMMARY_META_MAX/dstlen > 0/' "$T/zlib.inc" \
	> "$T/plant-zlib.inc"
if cmp -s "$T/zlib.inc" "$T/plant-zlib.inc"; then
	echo "FAIL: the zlib plant was not applied"
	exit 1
fi
prove "an unbounded summary expansion" "$T/remote.inc" "$T/xa.inc" \
	"$T/commit.inc" "$T/run.inc" "$T/main.inc" "$T/plant-zlib.inc" \
	"$T/open.inc" "$T/bytes.inc" "$T/own.inc"

# 4. a planted cache file adopted instead of dropped
sed 's/st\.st_uid != 0) {/st.st_uid >= 0) {/' "$T/own.inc" \
	> "$T/plant-bless.inc"
if cmp -s "$T/own.inc" "$T/plant-bless.inc"; then
	echo "FAIL: the ownership plant was not applied"
	exit 1
fi
prove "a group member's cache file made root's" "$T/remote.inc" "$T/xa.inc" \
	"$T/commit.inc" "$T/run.inc" "$T/main.inc" "$T/zlib.inc" \
	"$T/open.inc" "$T/bytes.inc" "$T/plant-bless.inc"

# 5. a caller-named repo file read by the worker again
{ cat "$T/remote.inc"; echo '	buf = slurp_file(from);'; } \
	> "$T/plant-slurp.inc"
prove "a caller-named repo file read as root" "$T/plant-slurp.inc" \
	"$T/xa.inc" "$T/commit.inc" "$T/run.inc" "$T/main.inc" "$T/zlib.inc" \
	"$T/open.inc" "$T/bytes.inc" "$T/own.inc"

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
	if out=$("$FLATPKG" remote-add --no-color --from=/dev/zero \
		fpkg-zero-must-refuse 2>&1); then
		bug "remote-add accepted an endless repo file"
	fi
	printf '%s\n' "$out" | grep -qi 'too large' ||
		bug "the endless repo file was not refused for its size: $out"
	if out=$("$FLATPKG" remote-add --no-color --from=/nonexistent/x.flatpakrepo \
		fpkg-missing-must-refuse 2>&1); then
		bug "remote-add accepted a missing repo file"
	fi
	printf '%s\n' "$out" | grep -q 'cannot read' ||
		bug "the missing repo file was not reported by the caller: $out"
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

echo "PASS: a repo file is https-only and bounded, the summary cache is read"
echo "      and owned through the descriptors that were judged, and its"
echo "      deflated metadata decompresses only up to a ceiling"
