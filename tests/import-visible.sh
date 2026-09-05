#!/bin/sh
# import-visible.sh - an import is a first-class store entry: listed,
# completed, exported, and kept rather than installed or upgraded from a
# remote.  It has no <id>/<arch>/<branch>/active, so a deploy-only walk misses it.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tests/scratchd.sh"

tmp=$(mktemp -d /tmp/.fpkg-import.XXXXXX)
trap 'scratchd_stop; rm -rf "$tmp"' EXIT
scratchd_start
FLATPKG=$SD_BIN

fail=0
note() { echo "FAIL: $1"; fail=1; }

C1=1111111111111111111111111111111111111111111111111111111111111111
APPID=org.example.Imported
S=$SD_STORE
SRC=$tmp/src
mkdir -p "$tmp/home"

# Own the environment the code reads so no host directory can move the
# answer.  PATH drops the caller's entries because the code resolves program
# names through it; TERM=dumb keeps message widths and escapes stable.
XDG_RUNTIME_DIR=$tmp/run
DBUS_SESSION_BUS_ADDRESS=unix:path=$tmp/no-such-bus
XDG_CURRENT_DESKTOP=GNOME
WAYLAND_DISPLAY=wayland-0
PATH=/usr/bin:/bin:/usr/local/bin
TERM=dumb
mkdir -p "$XDG_RUNTIME_DIR" "$tmp/home"
export XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS XDG_CURRENT_DESKTOP
export WAYLAND_DISPLAY PATH TERM NO_COLOR= FORCE_COLOR=

# A deploy root laid out the way `flatpkg import` expects:
#   <src>/<kind>/<id>/x86_64/stable/active/{files,export,metadata}
deploy() {
	kind=$1; id=$2
	d=$SRC/$kind/$id/x86_64/stable/active
	mkdir -p "$d/files/bin" "$d/export/bin" \
	    "$d/export/share/applications"
	printf '#!/bin/true\n' > "$d/files/bin/prog"
	printf '#!/bin/true\n' > "$d/export/bin/prog"
	printf '[Desktop Entry]\nName=Imported\nExec=prog %%U\nTryExec=prog\n' \
	    > "$d/export/share/applications/$id.desktop"
	printf '[Application]\nname=%s\ncommand=prog\n' "$id" > "$d/metadata"
	# store_import() finds the runtime deploy from the app's runtime= ref;
	# without it the import is skipped and nothing is recorded.
	if [ "$kind" = app ]; then
		printf 'runtime=org.example.Platform/x86_64/23.08\n' \
		    >> "$d/metadata"
	fi
	ln -sfn x86_64/stable "$SRC/$kind/$id/current"
}
deploy runtime org.example.Platform
deploy app $APPID
# A file flatpkg never wrote, to prove nothing follows the pointer back
# into the user's tree when the app is removed.
printf 'MY PRECIOUS DATA\n' > "$SRC/app/$APPID/USER-DATA.txt"

run() { HOME=$tmp/home "$FLATPKG" "$@" 2>&1 </dev/null || true; }

# The scratch store becomes a copy of fixture directory $1 (empty when none).
use_store() {
	scratchd_reset
	[ -z "${1:-}" ] || mdo cp -Rp "$1/." "$SD_STORE/"
}
# ...or the state the main case left: $SRC imported, the farm built.
import_store() {
	scratchd_reset
	run import "$SRC" >/dev/null
	run exports >/dev/null
}

# Same command through `-l <file>`: the logger clips long lines to the
# terminal width, so the solver's text would otherwise be truncated.
runlog() {
	rm -f "$tmp/log"
	HOME=$tmp/home "$FLATPKG" -l "$tmp/log" "$@" >/dev/null 2>&1 </dev/null || true
	cat "$tmp/log" 2>/dev/null || true
}

out=$(run import "$SRC")
case $out in
*"imported $APPID"*) ;;
*) note "import did not report success: $out" ;;
esac
[ -f "$S/app/$APPID/pointer" ] ||
    note "no pointer written at <store>/app/<id>/pointer"

# The whole point: an imported app has to be visible everywhere a
# store-local deploy is, and reachable in the menu.
run list | grep -q 'org.example.Impo' ||
    # `list` is a table that clips a long id to "org.example.Impo~", so the
    # match is on the surviving prefix; `complete apps` below is the
    # exact-match half.
    note "\`flatpkg list\` does not show the imported app"
run complete apps | grep -qx "$APPID" ||
    note "\`flatpkg complete apps\` does not offer the imported app"
run exports | grep -q 'refreshed: 1 app' ||
    note "\`flatpkg exports\` did not export the imported app: $(run exports)"

desktop=$S/exports/share/applications/$APPID.desktop
[ -f "$desktop" ] || note "the imported app got no desktop entry"
if [ -f "$desktop" ]; then
	# The arch and branch come from the pointer, not the store.
	grep -q -- '--arch=x86_64' "$desktop" ||
	    note "the rewrite lost the arch: $(grep '^Exec=' "$desktop")"
	grep -q -- '--branch=stable' "$desktop" ||
	    note "the rewrite lost the branch: $(grep '^Exec=' "$desktop")"
	grep -q -- "--command=prog" "$desktop" ||
	    note "the rewrite lost the command: $(grep '^Exec=' "$desktop")"
	grep -q 'TryExec' "$desktop" &&
	    note "TryExec= survived the rewrite"
fi

# Every symlink the farm created must resolve: an import has no `current`
# and no deploy under app/, so a relative target dangles.  The walk covers
# the whole farm because prune_dangling() only covers share.
dangling=$(find "$S/exports" -type l 2>/dev/null | while read -r l; do
	[ -e "$l" ] || printf '%s\n' "$l"
done)
[ -z "$dangling" ] ||
    note "the farm has dangling symlinks: $dangling"

# And it must not have mirrored the deploy: the farm holds export/,
# never files/ or metadata/.
[ ! -e "$S/exports/files" ] || note "the farm mirrored files/"
[ ! -e "$S/exports/metadata" ] || note "the farm mirrored metadata/"

# An import is linked by absolute path: there is no relative path from
# the farm to a tree outside the store.  Asserted so a change that
# "simplifies" this back to a relative link is caught as a dangling link.
[ -L "$S/exports/bin/prog" ] || note "no symlink for an exported binary"
if [ -L "$S/exports/bin/prog" ]; then
	case $(readlink "$S/exports/bin/prog") in
	/*) ;;
	*) note "an import's farm link should be absolute, got '$(readlink "$S/exports/bin/prog")'" ;;
	esac
fi

# Negative: a store-local deploy must still get the *relative* link, which
# is what lets the farm survive a store move.  Making every link absolute
# would pass everything above and lose that silently.
N=$tmp/native
# A commit directory holds files/ and export/ directly: `active` is a
# symlink to the commit dir, so current/active/export is <commit>/export.
mkdir -p "$N/app/org.example.Native/x86_64/stable/$C1/files" \
    "$N/app/org.example.Native/x86_64/stable/$C1/export/bin"
printf '[Application]\nname=Native\ncommand=prog\n' \
    > "$N/app/org.example.Native/x86_64/stable/$C1/metadata"
printf 'x\n' \
    > "$N/app/org.example.Native/x86_64/stable/$C1/export/bin/prog"
ln -sfn $C1 "$N/app/org.example.Native/x86_64/stable/active"
ln -sfn x86_64/stable "$N/app/org.example.Native/current"
use_store "$N"
run exports >/dev/null
NF=$SD_STORE/exports/bin/prog
[ -L "$NF" ] || note "a native deploy produced no farm link"
if [ -L "$NF" ]; then
	case $(readlink "$NF") in
	/*) note "a store-local deploy got an absolute link: $(readlink "$NF")" ;;
	*) ;;
	esac
	[ -e "$NF" ] ||
	    note "a store-local deploy's farm link dangles"
fi
# Back to the imported store for what follows.
import_store

# Structural: the pointer path is written in exactly one place.  Matched on
# the literal, not the helper name, so any other spelling of the layout is
# caught whichever way it is written.
n_spellings=$(grep -c 'app/%s/pointer' "$ROOT/src"/*.c "$ROOT/src"/*/*.c "$ROOT/src"/*/*/*.c 2>/dev/null | \
    awk -F: '{ t += $2 } END { print t + 0 }')
[ "$n_spellings" -le 1 ] ||
    note "the <store>/app/<id>/pointer layout is spelled out $n_spellings times;"\
" it belongs in store_pointer_path() alone"
grep -q 'store_pointer_path' "$ROOT/include/flatpkg.h" ||
    note "store_pointer_path() is not declared in flatpkg.h"

# An import is kept, never installed or upgraded from a remote: the
# solver's installed_list() only sees a real deploy dir, so an import reads
# as "not installed" and would be quietly replaced by a remote package.
out=$(runlog upgrade --simulate)
printf '%s' "$out" | grep -qE 'Installing app/'"$APPID" &&
    note "upgrade plans to install the import from a remote: $out"
printf '%s' "$out" | grep -qE 'Upgrading app/'"$APPID" &&
    note "upgrade plans to upgrade the import from a remote: $out"
printf '%s' "$out" | grep -q 'imported from a local deploy root' ||
    note "upgrade said nothing about the import it declined to upgrade: $out"

# Same for an import whose deploy root is gone: still kept, never quietly
# replaced from a remote (repair reports the broken deploy separately).
cp -R "$SRC" "$tmp/src2"
use_store ""
run import "$tmp/src2" >/dev/null
rm -rf "$tmp/src2"
rm -f "$tmp/log2"
HOME=$tmp/home "$FLATPKG" -l "$tmp/log2" upgrade --simulate >/dev/null 2>&1 </dev/null || true
out2=$(cat "$tmp/log2" 2>/dev/null || true)
printf '%s' "$out2" | grep -qE '(Installing|Upgrading) app/'"$APPID" &&
    note "a broken import was scheduled to be replaced from a remote: $out2"
run repair | grep -q 'broken' ||
    note "repair did not report the import whose deploy root is gone"
import_store

# An import missing from the world is planned for removal, and that removal
# cannot be undone by reinstalling: the store holds a pointer, not a copy.
# The plan is the last place to say so, so dropping the warning is destructive.
mdo sh -c "mkdir -p '$S/clients/flatpkg'; : > '$S/clients/flatpkg/world'"
# `upgrade` only sweeps what the world no longer references when asked
# to (`--prune`); without it the same store legitimately plans nothing.
out=$(run upgrade --prune --simulate)
printf '%s' "$out" | grep -qE "$APPID[[:space:]].*Remove" ||
    note "an import missing from the world is no longer planned for removal: $out"
out=$(runlog upgrade --prune --simulate)
printf '%s' "$out" | grep -q 'cannot restore it' ||
    note "the plan does not say that removing an import is unrecoverable: $out"

# Negative direction: a store-local deploy must still be upgraded, or a
# build that refuses to plan anything at all would pass every line above.
ND=$tmp/nativestore
mkdir -p "$ND/app/org.example.Native/x86_64/stable/$C1/files" "$tmp/home"
printf '[Application]\nname=Native\ncommand=prog\n' \
    > "$ND/app/org.example.Native/x86_64/stable/$C1/metadata"
ln -sfn $C1 "$ND/app/org.example.Native/x86_64/stable/active"
ln -sfn x86_64/stable "$ND/app/org.example.Native/current"
mkdir -p "$ND/clients/flatpkg"
printf 'org.example.Native\n' > "$ND/clients/flatpkg/world"
use_store "$ND"
out=$(run upgrade --simulate)
printf '%s' "$out" | grep -qE 'org\.example\.Native[[:space:]].*Upgrade' ||
    note "a store-local deploy is no longer planned for upgrade"
# ...and the import warning must not fire on it, or every auto-removed
# runtime in the store would carry a sentence about deploy roots.
rm -f "$tmp/log3"
HOME=$tmp/home "$FLATPKG" -l "$tmp/log3" upgrade --simulate >/dev/null 2>&1 </dev/null || true
grep -q 'imported from a local deploy root' "$tmp/log3" 2>/dev/null &&
    note "the import warning fired for a store-local deploy"

# A store built only by import has no <store>/repo, so uninstalling its last
# app must exit 0 with no WARN or ERROR: the store layer stays silent about
# an empty store and each command reports it in its own words.
import_store
mdo sh -c "mkdir -p '$S/clients/flatpkg'; : > '$S/clients/flatpkg/world'"
out=$(runlog uninstall -y $APPID)
if printf '%s' "$out" | grep -qE 'level=(WARN|ERROR)'; then
	note "uninstalling an imported app printed a diagnostic on a successful removal: $out"
fi
printf '%s' "$out" | grep -q 'uninstalled app' ||
    note "uninstall of the last app did not report success: $out"
# And the user's own deploy root must be untouched by any of it.
[ -f "$SRC/app/$APPID/USER-DATA.txt" ] ||
    note "uninstall reached into the source tree the pointer names"

# The audit trail: `flatpkg history` is documented as printing the install,
# upgrade and uninstall log, so a hand-typed removal and an import must be in
# it.  An import is the one install the store cannot repeat.
out=$(runlog history)
printf '%s' "$out" | grep -q 'import' ||
    note "the import is not in the history log: $out"
printf '%s' "$out" | grep -qE 'remove .*'"$APPID" ||
    note "the uninstall is not in the history log: $out"
# Exactly once each: logging at both the operation and the solver's call site
# would double every solver-driven removal.  The `|| true` matters too - a zero
# count is the failure here, and under `set -e` it would kill the guard.
n_import=$(printf '%s' "$out" | grep -c 'import') || true
n_remove=$(printf '%s' "$out" | grep -c "remove .*$APPID") || true
[ "$n_import" -eq 1 ] ||
    note "the import appears $n_import times in the history log"
[ "$n_remove" -eq 1 ] ||
    note "the uninstall appears $n_remove times in the history log"
# ...and a solver-driven removal must not be doubled either.  This has to be a
# committing run: counting the `--simulate` above always gives 0, which passes
# against a build that logs every removal twice.
SV=$tmp/solverdriven
mkdir -p "$SV/app/org.example.Swept/x86_64/stable/$C1/files"
printf '[Application]\nname=Swept\ncommand=prog\n' \
    > "$SV/app/org.example.Swept/x86_64/stable/$C1/metadata"
ln -sfn $C1 "$SV/app/org.example.Swept/x86_64/stable/active"
ln -sfn x86_64/stable "$SV/app/org.example.Swept/current"
# The world file must EXIST and be empty: world_seed() repopulates a missing
# one from the installed list, and then the solver has nothing to remove.
mkdir -p "$SV/clients/flatpkg"
: > "$SV/clients/flatpkg/world"
use_store "$SV"
rm -f "$tmp/log4"
# FLATPKG_INTERACTIVE=0: this is the one run that actually commits, and a
# confirmation prompt reading EOF aborts it, so the removal never happens.
HOME=$tmp/home FLATPKG_INTERACTIVE=0 \
    "$FLATPKG" -l "$tmp/log4" upgrade --prune -y </dev/null >/dev/null 2>&1 || true
# Counted by *reading the log back*: history_append() writes the store's
# history file and its lines reach the log only when `flatpkg history` reads
# them, so grepping the upgrade's own log finds plan lines, never the entry.
rm -f "$tmp/log5"
HOME=$tmp/home "$FLATPKG" -l "$tmp/log5" history >/dev/null 2>&1 </dev/null || true
n_solved=$(grep -c 'remove .*org.example.Swept' "$tmp/log5" 2>/dev/null || true)
[ "${n_solved:-0}" -eq 1 ] || note "a solver-driven removal logged $n_solved times, want 1"

# The man page must name the action the log now carries, or the log has
# an entry type the documentation does not mention.
grep -q 'install, import, upgrade and uninstall' "$ROOT/build/flatpkg.8" ||
    note "build/flatpkg.8 does not say that history records an import"

# An empty store is a fact, not an error: `list` and `exports` say so at info
# and exit 0.  man/flatpkg.8 reserves 1 for a usage or operational error, and
# an empty farm is neither.
use_store ""

# Every status test here is on the left of `||`, and that is load bearing:
# under `set -e` a failing bare command exits the script before `$?` is read,
# so the assertion never runs and the guard prints nothing.  Only `||` and `if`
# are exempt from `set -e`.
rc_store() { HOME=$tmp/home "$FLATPKG" "$@" </dev/null; }

rc_store list >/dev/null 2>&1 ||
    note "\`flatpkg list\` on an empty store should exit 0"
rc_store list 2>&1 | grep -q 'nothing installed' ||
    note "\`flatpkg list\` on an empty store should say so"
rc_store complete apps >/dev/null 2>&1 ||
    note "\`flatpkg complete apps\` on an empty store should exit 0"
rc_store exports >/dev/null 2>&1 ||
    note "\`flatpkg exports\` on an empty store should exit 0 - an empty farm is not an operational error"

# The exit status alone is satisfied by a command that says nothing, and the
# severity matters as much: the message right with the wrong rc is as bad as
# the rc right with the wrong level.
ex_out=$(rc_store exports 2>&1 || true)
printf '%s' "$ex_out" | grep -q 'nothing installed' ||
    note "\`flatpkg exports\` on an empty store should say so"
if printf '%s' "$ex_out" | grep -q 'ERROR'; then
    note "\`flatpkg exports\` reports an empty store at ERROR; it is a fact, not a failure"
fi

# `remote-list` on an import-only store has apps and zero remotes by
# construction, so exit 0 at info is a complete answer.  The counterpart must
# keep failing: in ref resolution no remote really does stop the command.
rc_store remote-list >/dev/null 2>&1 ||
    note "\`flatpkg remote-list\` on an empty store should exit 0 - listing zero remotes is a complete answer"
rl_out=$(rc_store remote-list 2>&1 || true)
printf '%s' "$rl_out" | grep -q 'no remotes configured' ||
    note "\`flatpkg remote-list\` on an empty store should say so"
if printf '%s' "$rl_out" | grep -q 'ERROR'; then
    note "\`flatpkg remote-list\` reports no remotes at ERROR; nothing is wrong with the store"
fi

# ... and on the fixture store, which has an app and still no remote.
import_store
run remote-list >/dev/null 2>&1 ||
    note "\`flatpkg remote-list\` should exit 0 on an import-only store, which has no remotes by construction"
# The other half: with no remote to install from, `search` must still refuse.
# Not `run ... && note` (run() ends in `|| true` and cannot fail) nor
# `cmd; [ $? -eq 0 ]` (set -e kills it): only an `if` condition carries the status.
if HOME=$tmp/home \
    "$FLATPKG" search "$APPID" >/dev/null 2>&1 </dev/null; then
    note "\`flatpkg search\` must not exit 0 on a store with no remotes to search"
fi

# Import hardening: the tree is the caller's and its paths become root mounts
# later, so a symlinked metadata is refused at import, not resolved as root.
old_src=$SRC
SRC=$tmp/hardened
deploy runtime org.example.Platform
deploy app org.example.Linked
hd=$SRC/app/org.example.Linked/x86_64/stable/active
rm "$hd/metadata"
ln -s /etc/passwd "$hd/metadata"
scratchd_reset
hout=$(run import "$SRC")
printf '%s' "$hout" | grep -q 'metadata is not a reg' ||
    note "an import with a symlinked metadata was not refused"
[ -e "$SD_STORE/app/org.example.Linked" ] &&
    note "a symlinked metadata was imported"
SRC=$old_src

[ $fail -eq 0 ] &&
    echo "PASS: an imported app is a first-class store entry, in the list and in the farm"
exit $fail
