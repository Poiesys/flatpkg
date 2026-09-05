#!/bin/sh
# logfmt-caller-path.sh - the daemon never opens a caller-named log path with
# more privilege than the caller has: chowning a file root created in a
# directory the caller cannot write lets the caller plant a root-run script.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tests/scratchd.sh"

tmp=$(mktemp -d /tmp/.fpkg-logfmt.XXXXXX)
trap 'scratchd_stop; rm -rf "$tmp"' EXIT
scratchd_start

fail=0
note() { echo "FAIL: $1"; fail=1; }
owner() { ls -l "$1" | awk '{print $3}'; }
me=$(id -un)

# A directory I cannot write, holding a file I cannot write.
RO=$SD_TMP/rootonly
mdo mkdir -m 755 "$RO"
mdo sh -c "printf 'root was here\\n' > '$RO/existing'; chmod 644 '$RO/existing'"
if touch "$RO/probe" 2>/dev/null; then
	rm -f "$RO/probe"
	note "the caller can write the root-only directory itself - the guard would prove nothing"
fi

runl() { # runl FILE: `exports` with -l FILE, bounded
	timeout 60 "$SD_BIN" -l "$1" exports </dev/null >/dev/null 2>&1 || true
}

# 1. a name inside a directory the caller cannot write.
runl "$RO/evil"
[ ! -e "$RO/evil" ] ||
	note "-l created $RO/evil in a root-only directory (owner $(owner "$RO/evil"))"

# 2. an existing root-owned file.
runl "$RO/existing"
[ "$(cat "$RO/existing")" = "root was here" ] ||
	note "-l appended to a root-owned file the caller cannot write"

# 3. a symlink: followed, it would reach a file the caller owns, and the log
# would land in the target.  O_NOFOLLOW must refuse it.
: > "$tmp/target"
ln -s "$tmp/target" "$tmp/link"
runl "$tmp/link"
[ ! -s "$tmp/target" ] || note "-l followed a symlink and wrote through it"

# 4. a FIFO with a reader attached: opened, the worker could be held by it or
# its records fed to another process.  Bounded by `timeout` in runl, and the
# reader must see nothing.
mkfifo "$tmp/fifo"
( cat "$tmp/fifo" > "$tmp/fifo.seen" ) &
reader=$!
s=$(date +%s)
runl "$tmp/fifo"
e=$(date +%s)
kill "$reader" 2>/dev/null || true
wait "$reader" 2>/dev/null || true
[ ! -s "$tmp/fifo.seen" ] || note "-l wrote log records into a FIFO"
[ $((e - s)) -lt 50 ] || note "-l on a FIFO held the command for $((e - s))s"

# 5. the legitimate case.
rm -f "$tmp/mine.log"
runl "$tmp/mine.log"
if [ ! -f "$tmp/mine.log" ]; then
	note "-l did not create the caller's own log file"
else
	[ "$(owner "$tmp/mine.log")" = "$me" ] ||
		note "the caller's log is owned by $(owner "$tmp/mine.log"), not $me"
	grep -q 'level=' "$tmp/mine.log" ||
		note "the caller's log holds no logfmt records: $(head -c 200 "$tmp/mine.log")"
fi
# ...and appending to an existing file of the caller's own works too.
before=$(wc -c < "$tmp/mine.log" 2>/dev/null || echo 0)
runl "$tmp/mine.log"
after=$(wc -c < "$tmp/mine.log" 2>/dev/null || echo 0)
[ "$after" -gt "$before" ] || note "-l did not append to the caller's existing log"

[ $fail -eq 0 ] &&
	echo "PASS: -l opens as the caller: nothing created in a root-only directory, no root-owned file appended to, no symlink followed, a FIFO refused, and the caller's own log still written"
exit $fail
