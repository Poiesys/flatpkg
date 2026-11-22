#!/bin/sh
# world-read.sh - a missing worldfile is the installed set, read in memory.
# The CLI runs as the caller, so it must never write the store: on a fresh
# install clients/flatpkg is root-owned 0755, and seeding the file from the
# reader failed with "cannot write .../world.tmp: Permission denied".
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN="$ROOT/build/flatpkg"

[ -x "$BIN" ] || { echo "SKIP: build/flatpkg is not built"; exit 0; }

T=$(mktemp -d /tmp/.fpkg-worldread.XXXXXX)
trap 'chmod -R u+w "$T" 2>/dev/null || true; rm -rf "$T"' EXIT

fail=0
note() {
	echo "FAIL: $*"
	fail=1
}

# The fresh-install shape: the store is root's, so this user cannot create
# clients/flatpkg/world.  Reading must succeed without trying.
store="$T/store"
mkdir -p "$store/clients/flatpkg"
chmod 0555 "$store/clients/flatpkg" "$store/clients"
out=$(FLATPKG_STORE="$store" "$BIN" --no-color complete world 2>&1) || {
	note "reading a missing world failed: $out"
}
[ -e "$store/clients/flatpkg/world" ] &&
	note "the reader created a worldfile"
[ -e "$store/clients/flatpkg/world.tmp" ] &&
	note "the reader left a temp worldfile"
[ -z "$out" ] || note "an empty store printed a world entry: $out"
chmod 0755 "$store/clients" "$store/clients/flatpkg"

# The worldfile's text is client-supplied, and an entry becomes a ref, which
# becomes a path: a name that is not one component is refused out loud rather
# than kept.
store="$T/store3"
mkdir -p "$store/clients/flatpkg"
printf 'app/../../escape\napp.drey.Warp\n' > "$store/clients/flatpkg/world"
err=$(FLATPKG_STORE="$store" "$BIN" --no-color complete world 2>&1 >/dev/null) ||
	true
case "$err" in
*"cannot parse"*) ;;
*) note "a traversal name in the worldfile was quietly dropped: $err" ;;
esac
out=$(FLATPKG_STORE="$store" "$BIN" --no-color complete world 2>/dev/null) ||
	true
case "$out" in
*escape*) note "the refused world entry was still offered: $out" ;;
*) ;;
esac
printf '%s\n' "$out" | grep -qx 'app.drey.Warp' ||
	note "a good world entry after a bad one was dropped: $out"

# Even where the reader could write, reading is not seeding: the first writer
# (the daemon, or the client's state write) creates the file.
store="$T/store2"
mkdir -p "$store/clients/flatpkg"
out=$(FLATPKG_STORE="$store" "$BIN" --no-color complete world 2>&1) ||
	note "reading a missing world from a writable store failed: $out"
[ -e "$store/clients/flatpkg/world" ] &&
	note "reading a missing world seeded the file"
[ -e "$store/clients/flatpkg/world.tmp" ] &&
	note "the reader left a temp worldfile"

[ "$fail" -eq 0 ] &&
	echo "PASS: a missing worldfile is the installed set, read in memory,"
echo "      and the unprivileged CLI never writes the store"
exit "$fail"
