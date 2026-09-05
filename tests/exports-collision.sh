#!/bin/sh
# exports-collision.sh - the exports farm must not lose an app quietly: the
# namespace is flat and two apps can disagree file-vs-directory, in either
# readdir order, so the loser must be named and must not count as exported.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tests/scratchd.sh"

tmp=$(mktemp -d /tmp/.fpkg-expcoll.XXXXXX)
trap 'scratchd_stop; rm -rf "$tmp"' EXIT
scratchd_start
FLATPKG=$SD_BIN

# Own the environment, so the host cannot move the answer: PATH keeps only
# the system directories (the code resolves program names through it) and
# TERM=dumb keeps the width and escapes the same every run.
XDG_RUNTIME_DIR=$tmp/run
DBUS_SESSION_BUS_ADDRESS=unix:path=$tmp/no-such-bus
XDG_CURRENT_DESKTOP=GNOME
WAYLAND_DISPLAY=wayland-0
PATH=/usr/bin:/bin:/usr/local/bin
TERM=dumb
mkdir -p "$XDG_RUNTIME_DIR" "$tmp/home"
export XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS XDG_CURRENT_DESKTOP
export WAYLAND_DISPLAY PATH TERM NO_COLOR= FORCE_COLOR=

fail=0
note() { echo "FAIL: $1"; fail=1; }

# One app's deploy tree, as a flat name=content listing.  The apps' own
# `active/export/` trees are the store's source of truth and the farm is
# a view of them; nothing here has a business writing to them.
snapshot() {
	find "$1/app" -path '*/active/export/*' -type f | sort |
	while read -r f; do printf '%s=' "$f"; cat "$f"; done
}

# Both spellings, because which app is walked second is readdir order:
# observed to differ between fixtures, so requiring one of them to pass
# would let the other through untested.
for pair in "Aaa Zzz" "Zzz Aaa"; do
	set -- $pair
	# Built as the user, then copied into the scratch store the daemon owns.
	store=$tmp/fx$1$2
	rm -rf "$store"
	mk_app() {
		mkdir -p "$store/app/$1/x86_64/stable/active/export"
		ln -sfn x86_64/stable "$store/app/$1/current"
	}
	mk_app org.example.Aaa
	mk_app org.example.Zzz
	# Aaa exports `lib` as a file, Zzz as a directory containing a file.
	printf 'iam-a-file\n' \
	    > "$store/app/org.example.Aaa/x86_64/stable/active/export/lib"
	mkdir -p "$store/app/org.example.Zzz/x86_64/stable/active/export/lib"
	printf 'iam-in-a-dir\n' \
	    > "$store/app/org.example.Zzz/x86_64/stable/active/export/lib/child"

	scratchd_reset
	mdo cp -Rp "$store/." "$SD_STORE/"
	before=$(snapshot "$SD_STORE")
	HOME=$tmp/home "$FLATPKG" -l "$store.log" exports \
	    >/dev/null 2>&1 </dev/null || true
	after=$(snapshot "$SD_STORE")
	log=$(cat "$store.log" 2>/dev/null || true)

	[ "$before" = "$after" ] ||
	    note "[$pair] a deploy tree was modified by exports"
	# The losing app must not be counted: one of the two can never be
	# exported, and the summary has to say so.
	printf '%s' "$log" | grep -q 'exports refreshed: 1 app' ||
	    note "[$pair] an app was counted as exported though it was not"
	# ...and it must be named, so the user has something to act on.
	printf '%s' "$log" |
	    grep -q 'another app already owns that name' ||
	    note "[$pair] the collision was not reported"
	rm -f "$store.log"
done

# Negative direction: every assertion above is also satisfied by a build that
# refuses to export anything, so a store with no collision must come through
# untouched - both apps exported, nothing warned, the .desktop rewrite there.
clean=$tmp/clean
for id in org.example.One org.example.Two; do
	mkdir -p "$clean/app/$id/x86_64/stable/active/export/share/applications"
	ln -sfn x86_64/stable "$clean/app/$id/current"
	printf '[Desktop Entry]\nExec=%s %%U\n' "$id" \
	    > "$clean/app/$id/x86_64/stable/active/export/share/applications/$id.desktop"
done
scratchd_reset
mdo cp -Rp "$clean/." "$SD_STORE/"
HOME=$tmp/home "$FLATPKG" -l "$clean.log" exports \
    >/dev/null 2>&1 </dev/null || true
clog=$(cat "$clean.log" 2>/dev/null || true)
printf '%s' "$clog" | grep -q 'exports refreshed: 2 apps' ||
    note "a store with no collision did not export both apps: $clog"
printf '%s' "$clog" | grep -q 'level=WARN' &&
    note "a store with no collision produced a warning: $clog"
grep -q 'flatpak run --branch=stable --arch=x86_64 --command=' \
    "$SD_STORE/exports/share/applications/org.example.One.desktop" ||
    note "the .desktop Exec rewrite is gone from the farm"

[ $fail -eq 0 ] &&
    echo "PASS: exports reports collisions and counts what landed"
exit $fail
