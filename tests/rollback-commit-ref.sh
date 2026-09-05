#!/bin/sh
# rollback-commit-ref.sh - `flatpkg rollback` must not let its commit argument
# escape the branch directory.  cmd_rollback() joined argv onto <branch_dir>, and
# make_active() then wrote the `active` symlink there, in a root worker.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tests/scratchd.sh"

tmp=$(mktemp -d /tmp/.fpkg-rbref.XXXXXX)
trap 'scratchd_stop; rm -rf "$tmp"' EXIT
scratchd_start
FLATPKG=$SD_BIN

fail=0
note() { echo "FAIL: $1"; fail=1; }

C1=1111111111111111111111111111111111111111111111111111111111111111
C2=2222222222222222222222222222222222222222222222222222222222222222
# The fixture is built as the user and copied into the scratch store.  `outside`
# sits exactly where five `..` from the branch land, so a traversal that stat()
# merely refuses is not mistaken for one refused by the name check.
S=$SD_STORE
B=$S/app/org.example.App/x86_64/stable
FX=$tmp/fixture
FB=$FX/app/org.example.App/x86_64/stable
for c in $C1 $C2; do
	mkdir -p "$FB/$c/files"
	printf '[Application]\nname=App\n' > "$FB/$c/metadata"
done
touch -t 202601010000 "$FB/$C1"
touch -t 202602010000 "$FB/$C2"
ln -sfn $C2 "$FB/active"
ln -sfn x86_64/stable "$FX/app/org.example.App/current"
mdo cp -Rp "$FX/." "$S/"
OUTSIDE=$SD_TMP/outside
mkdir -p "$OUTSIDE/deployed" "$tmp/home"

# Own the environment the code reads, so the host cannot move the answer.  PATH
# drops the caller's: the code resolves program names through it.  TERM=dumb
# turns colour off so a message's width and escapes are the same every run.
XDG_RUNTIME_DIR=$tmp/run
DBUS_SESSION_BUS_ADDRESS=unix:path=$tmp/no-such-bus
XDG_CURRENT_DESKTOP=GNOME
WAYLAND_DISPLAY=wayland-0
PATH=/usr/bin:/bin:/usr/local/bin
TERM=dumb
mkdir -p "$XDG_RUNTIME_DIR" "$tmp/home"
export XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS XDG_CURRENT_DESKTOP
export WAYLAND_DISPLAY PATH TERM NO_COLOR= FORCE_COLOR=

run() { HOME=$tmp/home "$FLATPKG" "$@" 2>&1 < /dev/null || true; }

# Same command, but through `-l <file>`: the logger clips the displayed message
# to the terminal width and a 64-char hash does not fit, so grepping the terminal
# line fails against a run that printed it correctly.  logfmt carries it whole.
runlog() {
	rm -f "$tmp/log"
	HOME=$tmp/home "$FLATPKG" -l "$tmp/log" "$@" >/dev/null 2>&1 </dev/null || true
	cat "$tmp/log" 2>/dev/null || true
}

# Every symlink anywhere under the store, as "<path> -> <target>".
symlinks() {
	find "$S" -type l 2>/dev/null | sort |
	while read -r l; do printf '%s -> %s\n' "$l" "$(readlink "$l")"; done
}

base_symlinks=$(symlinks)
base_active=$(readlink "$B/active")

# The traversal shapes: a bare `..`, a run of them, a path that lands
# outside the store, an absolute path, and a name that is merely long.
for arg in \
    '..' \
    '../..' \
    '../../..' \
    '../../../../../../../../etc' \
    '../../../../../outside/deployed' \
    '/etc' \
    "$C1/../../.." \
    'ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ'
do
	out=$(run rollback org.example.App "$arg")
	case $out in
	*OKAY*|*"rolled back"*)
	    note "rollback accepted '$arg': $out" ;;
	esac
	# The effect, not the exit status: the store's symlink set must be
	# exactly what it was, and the app must still point at its commit.
	now=$(symlinks)
	[ "$now" = "$base_symlinks" ] ||
	    note "rollback '$arg' changed the store's symlinks: $now"
	[ "$(readlink "$B/active")" = "$base_active" ] ||
	    note "rollback '$arg' moved active off $base_active"
done

# Nothing may be planted outside the store either.
[ -z "$(find "$OUTSIDE" -type l 2>/dev/null)" ] ||
    note "a symlink was planted outside the store: $(find "$OUTSIDE" -type l)"

# The store must still be usable: a stray `active` at the app-id level
# is what broke `list` in the first place.
run list | grep -q 'org.example.App' ||
    note "`flatpkg list` no longer reports the app after the refusals"

# Positive direction.  Every assertion above is also satisfied by a rollback
# that refuses everything, so the genuine case has to work - and `list-commits`
# has to keep listing both, because collect_commit_dirs() shares the predicate.
out=$(run rollback org.example.App $C1)
case $out in
*"rolled back"*) ;;
*) note "a real commit was refused: $out" ;;
esac
[ "$(readlink "$B/active")" = "$C1" ] ||
    note "a real commit did not become active: $(readlink "$B/active")"

out=$(runlog list-commits org.example.App)
printf '%s' "$out" | grep -q "$C1" || note "list-commits dropped $C1: $out"
printf '%s' "$out" | grep -q "$C2" || note "list-commits dropped $C2: $out"

[ $fail -eq 0 ] &&
    echo "PASS: rollback accepts only a commit hash and never leaves the branch"
exit $fail
