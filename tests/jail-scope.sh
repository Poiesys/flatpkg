#!/bin/sh
# jail-scope.sh - root must not kill or enter a jail flatpkg did not create:
# kill/enter take a bare jail id and act as root, so the test is the jail's
# name prefix - the one property readable in every jail state on this box.
set -e

# kill and enter are served by the ROOT worker, so a daemon-down skip is a
# verdict about the box, not the tree.  Test the PROCESS, not the socket:
# `service stop` leaves control.sock behind, and "sbin/flatpkg" matches rc.d.
pgrep -f "sbin/flatpkgd" >/dev/null 2>&1 || {
	echo "SKIP: no flatpkg daemon running - these commands are served by its root worker, not by this account"
	exit 0
}

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/src"
FLATPKG="$ROOT/build/flatpkg"
tmp=$(mktemp -d /tmp/.fpkg-jailscope.XXXXXX)
fail=0
note() { echo "FAIL: $*"; fail=1; }

# The daemon reads its own environment, and this guard needs none of the real
# session: pin every name, XDG_RUNTIME_DIR included, so the verdict cannot
# depend on which instance dirs the host happens to hold.
export HOME="$tmp/home"
export XDG_RUNTIME_DIR="$tmp/run"
export XDG_CURRENT_DESKTOP=GNOME
export DBUS_SESSION_BUS_ADDRESS="unix:path=$tmp/nonexistent-bus"
export WAYLAND_DISPLAY=
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export TERM=dumb
export NO_COLOR= FORCE_COLOR=
mkdir -p "$HOME" "$XDG_RUNTIME_DIR"

command -v mdo >/dev/null 2>&1 || { echo "SKIP: no mdo, so no jail can be planted"; exit 0; }
[ -x "$FLATPKG" ] || { echo "SKIP: build/flatpkg not built"; exit 0; }
[ -S /var/run/flatpkg/control.sock ] || { echo "SKIP: the daemon is not running"; exit 0; }

# strip_comments() and carve_function() live in commentlib.sh (sourced, never run).
. "$(dirname "$0")/commentlib.sh"

# The name the header gives every jail flatpkg creates, read from the header
# rather than typed here: a guard that spells the vocabulary a third time is
# how the vocabulary drifts.
PREFIX=$(sed -n 's/^#define FLATPKG_JAIL_NAME_PREFIX *"\(.*\)"$/\1/p' \
	"$ROOT/include/flatpkg.h" | head -1)
[ -n "${PREFIX:-}" ] || { echo "FAIL: include/flatpkg.h does not define FLATPKG_JAIL_NAME_PREFIX"; exit 1; }
OURS="${PREFIX}jailscope"
THEIRS="not${PREFIX}jailscope"
RUNDIR=$(sed -n 's/^#define FLATPKG_RUN_DIR *"\(.*\)"$/\1/p' \
	"$ROOT/include/flatpkg.h" | head -1)

# `jls -N` prints each jail's NAME where its JID would be, so the name is
# always the first field; a jail with no address has no IP column.  (`jls
# <name>` is not a lookup: it reads the argument as a parameter name.)
named_jids() {
	mdo jls -N 2>/dev/null | awk -v a="$OURS" -v t="$THEIRS" \
		'NR > 1 && ($1 == a || $1 == t) { print $1 }'
}

# ---- cleanup, privileged and then verified -----------------------------
cleanup() {
	for j in $(named_jids); do
		mdo jail -r "$j" >/dev/null 2>&1 || true
	done
	left=$(named_jids | grep -c . || true)
	if [ "${left:-0}" -ne 0 ]; then
		echo "note: CLEANUP INCOMPLETE - $left jail(s) still named $OURS or $THEIRS."
		echo "        mdo jls -N; mdo jail -r <jid>"
	fi
	mdo rmdir "$CTRL" >/dev/null 2>&1 || true
	rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

# `persist` is required: jail(8) refuses one that neither persists nor
# attaches, and a persisted jail is the kind whose NAME jail_getname() reads.
# The jid is found by DIFFERENCE: a new jail always takes the highest id.
make_jail() {
	before=$(mdo jls -q jid 2>/dev/null | sort -n | tail -1)
	before=${before:-0}
	if [ -n "${3:-}" ]; then
		mdo jail -c -n "$1" persist path="$2" host.hostname="$1" \
			"meta=flatpkg-uid=$3" >/dev/null 2>&1 || return 1
	else
		mdo jail -c -n "$1" persist path="$2" host.hostname="$1" \
			>/dev/null 2>&1 || return 1
	fi
	after=$(mdo jls -q jid 2>/dev/null | sort -n | tail -1)
	[ -n "$after" ] && [ "$after" -gt "$before" ] || return 1
	echo "$after"
}
alive() { mdo jls -q jid 2>/dev/null | grep -qx "$1" && echo 1 || echo 0; }

# ---- 1 and 2: a jail of somebody else's is refused, and survives --------
# `path=/` is the most damning case and the easiest to plant: not merely
# somewhere else, it is the host's own root.
other=$(make_jail "$THEIRS" / || true)
if [ -z "${other:-}" ]; then
	echo "SKIP: could not create a jail to test with (jail(8) refused)"
	exit 0
fi
echo "  planted a jail that is not ours: jid $other, path /"

# $? read straight after the command, never after a pipe: the first
# measurement of this printed rc=0 for a refusal, because the status came
# from the `head` at the end of the pipeline.
rc=0
out=$(timeout 30 "$FLATPKG" kill "$other" 2>&1) || rc=$?
[ "$rc" -ne 0 ] || note "kill of a jail flatpkg does not own exited 0"
# The visible part of the line, not the whole sentence: fplog truncates at
# the terminal width, and the message is written verdict-first so this
# prefix survives the cut.
case "$out" in
*"not a flatpkg instance"*) : ;;
*) note "kill did not say the jail is not one of ours: $out" ;;
esac
[ "$(alive "$other")" = 1 ] ||
	note "kill removed jail $other after refusing it - the refusal is not the claim, the survival is"

rc=0
out=$(timeout 30 "$FLATPKG" enter "$other" /bin/true 2>&1) || rc=$?
[ "$rc" -ne 0 ] || note "enter of a jail flatpkg does not own exited 0"
case "$out" in
*"not a flatpkg instance"*) : ;;
*) note "enter did not say the jail is not one of ours: $out" ;;
esac
[ "$(alive "$other")" = 1 ] ||
	note "enter of jail $other disturbed it"

# ---- 3: no such jail ---------------------------------------------------
# A stale instance id is the common case; the claim is only that it is
# refused and says why.
rc=0
out=$(timeout 30 "$FLATPKG" kill 999999 2>&1) || rc=$?
[ "$rc" -ne 0 ] || note "kill of a jail that does not exist exited 0"
case "$out" in
*"not a flatpkg instance"*) : ;;
*) note "kill of a missing jail did not say so: $out" ;;
esac

mdo jail -r "$other" >/dev/null 2>&1 || true

# ---- 4: the control - a jail that IS ours is accepted -------------------
# Without this, cases 1-3 pass on a build that refuses every jail id.  The
# jail has sandbox.c's shape; jail(8) wants the path to exist, then remove it.
CTRL="$RUNDIR/$OURS"
mdo mkdir -p "$CTRL" >/dev/null 2>&1
ours=$(make_jail "$OURS" "$CTRL" "$(id -u)" || true)
if [ -z "${ours:-}" ]; then
	echo "FAIL: could not create the control jail named $OURS"
	echo "      (if this box will not let one exist, the control cannot be"
	echo "       proven and the refusal cases above prove less than they look)"
	fail=1
else
	rc=0
	out=$(timeout 30 "$FLATPKG" kill "$ours" 2>&1) || rc=$?
	[ "$rc" -eq 0 ] ||
		note "kill of a jail named the way ours are was refused: $out"
	[ "$(alive "$ours")" = 0 ] ||
		note "kill reported success but jail $ours is still alive"
fi

# Fed strip_comments output, never the raw file: the carve counts braces in
# whatever it is given, and the file is passed BY NAME - a pipe here is a
# no-op, and every carve would then read the raw file, comments included.
carved() {
	strip_comments < "$1" > "$tmp/nocomments.c" || return 1
	carve_function "$2" "$tmp/nocomments.c"
}

# Real calls only: a quote before the token means the token is inside a
# string, which is how cmd_enter()'s dprintf("jail_attach(%d)") can look
# like the syscall it reports failing.
first_call() {
	awk -v tok="$1" 'index($0, tok) > 0 {
		q = index($0, "\""); p = index($0, tok);
		if (q == 0 || p < q) { print NR; exit }
	}'
}

body=$(carved "$(srcf instance_ctl.c)" '^int cmd_kill' || true)
[ -n "$body" ] || note "could not carve cmd_kill() out of src/daemon/ops/instance_ctl.c"
kr=$(printf '%s\n' "$body" | first_call 'jail_remove(')
pc=$(printf '%s\n' "$body" | first_call 'require_flatpkg_jail(')
[ -n "$kr" ] || note "cmd_kill() has no jail_remove() call to order against"
[ -n "$pc" ] || note "cmd_kill() does not call the ownership predicate at all"
# Ordering: the predicate must come BEFORE jail_remove(2).  A grep for
# "does it check?" is satisfied by a check after the removal, which is the
# same shape runtime-dir.sh's chmod/chown ordering cases exist for.
if [ -n "$kr" ] && [ -n "$pc" ] && [ "$pc" -ge "$kr" ]; then
	note "cmd_kill() removes the jail before checking it owns it (line $kr vs $pc)"
fi

body=$(carved "$(srcf instance_ctl.c)" '^int cmd_enter' || true)
[ -n "$body" ] || note "could not carve cmd_enter() out of src/daemon/ops/instance_ctl.c"
pc=$(printf '%s\n' "$body" | first_call 'require_flatpkg_jail(')
ja=$(printf '%s\n' "$body" | first_call 'jail_attach(')
[ -n "$pc" ] || note "cmd_enter() does not call the ownership predicate at all"
[ -n "$ja" ] || note "cmd_enter() has no jail_attach() call to order against"
if [ -n "$pc" ] && [ -n "$ja" ] && [ "$pc" -ge "$ja" ]; then
	note "cmd_enter() attaches before checking it owns the jail (line $pc vs $ja)"
fi

# ---- the run-environ snapshot: WHERE it is taken, and who reads it ----
# `run-environ` is the app's environment, so it is written after the app's
# [Environment]/--env= are applied and before the chroot, and enter reads it.
body=$(carved "$(srcf instance_ctl.c)" '^int instance_register' || true)
[ -n "$body" ] || note "could not carve instance_register() out of src/daemon/ops/instance_ctl.c"
case "$body" in
*instance_save_run_environ*|*write_run_environ*)
	note "instance_register() still writes the run-environ snapshot, so it"
	note "  is recorded before sandbox_exec() applies the app's own"
	note "  environment - the file says the app and holds the worker" ;;
*) : ;;
esac
# ...and the writer must be called from where the environment is final.
sbody=$(carved "$(srcf sandbox.c)" '^int sandbox_exec' || true)
[ -n "$sbody" ] || note "could not carve sandbox_exec() out of src/daemon/sandbox.c"
case "$sbody" in
*instance_save_run_environ*) : ;;
*) note "sandbox_exec() does not call instance_save_run_environ(), so"
   note "  nothing records the environment the app actually ran with" ;;
esac
# The snapshot is taken BEFORE the fork and the chroot: inside the chroot
# there is no $XDG_RUNTIME_DIR/.flatpkg/, so a write there failed with ENOENT
# on every launch.  The two env lists are applied to a copy instead.
sattach=$(printf '%s\n' "$sbody" | first_call 'jail_attach(' | cut -d: -f1)
sfork=$(printf '%s\n' "$sbody" | first_call 'fork(' | cut -d: -f1)
ss=$(printf '%s\n' "$sbody" | first_call 'instance_save_run_environ(' |
	cut -d: -f1)
if [ -z "$ss" ]; then
	: # already reported above
elif [ -n "$sattach" ] && [ "$ss" -ge "$sattach" ]; then
	note "sandbox_exec() records the run-environ at line $ss, at or after"
	note "  the jail_attach() at $sattach - inside the chroot there is no"
	note "  \$XDG_RUNTIME_DIR/.flatpkg/, so the write fails every launch"
fi
# ...and before the fork too, so the file is written once, by the worker
# rather than by a child that is about to chroot.
if [ -n "$ss" ] && [ -n "$sfork" ] && [ "$ss" -ge "$sfork" ]; then
	note "sandbox_exec() records the run-environ at line $ss, after the"
	note "  fork() at $sfork - a child cannot write the host's file"
fi
# ...and the two lists are applied to the snapshot, not to the live
# environment, which is what lets the write happen before the chroot.
ibody=$(carved "$(srcf instance_ctl.c)" '^static int snapshot_env' || true)
[ -n "$ibody" ] || note "could not carve snapshot_env() out of src/daemon/ops/instance_ctl.c"
case "$ibody" in
*env_pairs*env_overrides*) : ;;
*) note "snapshot_env() does not apply the app's own [Environment] and"
   note "  --env= lists, so a pre-chroot snapshot is the worker's" ;;
esac
case "$sbody" in
*'instance_save_run_environ(sb, meta, plan)'*) : ;;
*) note "sandbox_exec() does not pass meta and plan to"
   note "  instance_save_run_environ(), so the app's own environment is"
   note "  not in the snapshot" ;;
esac
# And the consumer.
body=$(carved "$(srcf instance_ctl.c)" '^int cmd_enter' || true)
case "$body" in
*instance_load_run_environ*) : ;;
*) note "cmd_enter() does not read the run-environ snapshot, so entering an"
   note "  instance gives a shell with none of the app's environment" ;;
esac
case "$body" in
*'setenv("PATH"'*) : ;;
*) note "cmd_enter() no longer pins a sandbox PATH for the entered shell" ;;
esac
# The four must be set AFTER the snapshot, or a recorded PATH would send the
# entered shell at the host's - which is the one property the snapshot must
# not be able to override.
rl=$(printf '%s\n' "$body" | grep -n 'instance_load_run_environ' | head -1 |
	cut -d: -f1)
rp=$(printf '%s\n' "$body" | grep -n 'setenv("PATH"' | head -1 | cut -d: -f1)
if [ -n "$rl" ] && [ -n "$rp" ] && [ "$rl" -ge "$rp" ]; then
	note "cmd_enter() loads the snapshot at line $rl, after pinning PATH"
	note "  at $rp - a recorded PATH would point the shell at the host's"
fi

body=$(carved "$(srcf instance_ctl.c)" '^static bool jail_is_flatpkg_owned' || true)
[ -n "$body" ] || note "could not carve jail_is_flatpkg_owned() out of src/daemon/ops/instance_ctl.c"
case "$body" in
*jail_getname*) : ;;
*) note "the ownership predicate does not read the jail's name" ;;
esac
case "$body" in
*FLATPKG_JAIL_NAME_PREFIX*) : ;;
*) note "the ownership predicate does not test FLATPKG_JAIL_NAME_PREFIX" ;;
esac

# And the prefix is one definition with two users, not a coincidence.
sb=$(strip_comments < "$(srcf sandbox.c)")
# Braces, not a second line: `cmd || note a` followed by `note b` runs b
# UNCONDITIONALLY, which is how the first version of this reported a
# missing sandbox.c prefix on a tree that has it.
printf '%s\n' "$sb" | grep -q 'FLATPKG_JAIL_NAME_PREFIX' || {
	note "sandbox.c no longer names its jails with FLATPKG_JAIL_NAME_PREFIX -"
	note "the name instance.c tests is no longer the name it writes"
}
n=$(grep -c 'define FLATPKG_JAIL_NAME_PREFIX' "$ROOT/include/flatpkg.h" || true)
[ "$n" = 1 ] || note "FLATPKG_JAIL_NAME_PREFIX is defined $n times in include/flatpkg.h"
l=$(grep -rn 'define FLATPKG_JAIL_NAME_PREFIX' "$SRC" | grep -c . || true)
[ "${l:-0}" = 0 ] ||
	note "a .c file defines its own copy of the jail-name prefix ($l found)"

echo
if [ "$fail" -eq 0 ]; then
	echo "PASS: root refuses to kill or enter a jail flatpkg did not create, the"
	echo "      jail survives the refusal, a jail named the way ours are is"
	echo "      accepted, and both call sites ask before they act."
	echo "      cmd_enter() also restores the app's recorded environment from"
	echo "      the run-environ snapshot, and that snapshot is taken in"
	echo "      sandbox_exec() - after the app's own [Environment] and every"
	echo "      --env= are applied - rather than in instance_register(), which"
	echo "      runs first and would have recorded the worker's."
fi
exit $fail
