#!/bin/sh
# exit-status.sh - the exit status the client reports is the daemon's, taken
# from the last event (FP_EV_OP_FINISHED), not from a socket trailer.  Runtime
# cases catch a lying status; structural ones the two sides disagreeing.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FLATPKG="$ROOT/build/flatpkg"
tmp=$(mktemp -d /tmp/.fpkg-exitstatus.XXXXXX)
fail=0
note() { echo "FAIL: $*"; fail=1; }
trap 'rm -rf "$tmp"' EXIT INT TERM

export HOME="$tmp/home"
export XDG_RUNTIME_DIR="$tmp/run"
export XDG_CURRENT_DESKTOP=GNOME
export DBUS_SESSION_BUS_ADDRESS="unix:path=$tmp/nonexistent-bus"
export WAYLAND_DISPLAY=
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export TERM=dumb
export NO_COLOR= FORCE_COLOR=
mkdir -p "$HOME" "$XDG_RUNTIME_DIR"

[ -x "$FLATPKG" ] || { echo "SKIP: build/flatpkg not built"; exit 0; }
[ -S /var/run/flatpkg/control.sock ] || { echo "SKIP: the daemon is not running"; exit 0; }

. "$(dirname "$0")/commentlib.sh"

# $? is read immediately and never after a pipeline: the first measurement of
# the round-212 bug printed rc=0 for a refusal because the status came from
# the `head` at the end of the pipe.
rc_of() {
	set +e
	"$FLATPKG" "$@" >"$tmp/out" 2>"$tmp/err"
	rc=$?
	set -e
	echo "$rc"
}

# ---- successes ---------------------------------------------------------
n_ok=0
for c in history list remotes; do
	# shellcheck disable=SC2086
	set -- $c
	rc=$(rc_of "$@")
	n_ok=$((n_ok + 1))
	[ "$rc" -eq 0 ] || note "'flatpkg $c' succeeded but exited $rc"
done
rc=$(rc_of --version)
[ "$rc" -eq 0 ] || note "'flatpkg --version' exited $rc"
n_ok=$((n_ok + 1))
[ "$n_ok" -ge 4 ] || note "only $n_ok success cases ran"

# ---- failures ----------------------------------------------------------
n_no=0
rc=$(rc_of run org.example.NoSuchAppForThisGuard)
[ "$rc" -ne 0 ] || note "'run' of an app that does not exist exited 0"
[ "$rc" -ne 255 ] || note "'run' of a missing app exited 255"
n_no=$((n_no + 1))
rc=$(rc_of kill 999999)
[ "$rc" -ne 0 ] || note "'kill' of a jail that does not exist exited 0"
[ "$rc" -ne 255 ] || note "'kill' exited 255"
n_no=$((n_no + 1))
[ "$n_no" -ge 2 ] || note "only $n_no failure cases ran"

# ---- the framing bytes never reach the caller's stdout -----------------
"$FLATPKG" list >"$tmp/stdout" 2>/dev/null || true
if LC_ALL=C grep -q 'FPKG' "$tmp/stdout"; then
	note "control framing reached the caller's stdout:"
	LC_ALL=C grep -n 'FPKG' "$tmp/stdout" | head -3 | sed 's/^/        /'
fi
"$FLATPKG" complete apps >"$tmp/complete" 2>/dev/null || true
bad=$(LC_ALL=C grep -cvE '^[A-Za-z0-9._-]+$' "$tmp/complete" || true)
[ "${bad:-0}" -eq 0 ] || note "$bad line(s) of 'complete apps' are not bare app ids"

# ---- structurally, both sides name the same status ---------------------
carved() {
	strip_comments <"$1" >"$tmp/stripped.c"
	carve_function "$2" "$tmp/stripped.c"
}

render=$(carved "$ROOT/src/cli/client_typed.c" '^static int typed_render' || true)
[ -n "$render" ] || note "could not carve typed_render()"
printf '%s\n' "$render" | grep -q 'exit_code = (int)ev.u.finished.status' ||
	note "the client does not take the exit status from FP_EV_OP_FINISHED"
printf '%s\n' "$render" | grep -q 'return exit_code' ||
	note "typed_render() never returns the event's status"
client=$(carved "$ROOT/src/cli/client_typed.c" '^int client_try_typed' || true)
printf '%s\n' "$client" | grep -q 'typed_complete(' ||
	note "client_try_typed() does not return typed_complete()'s status"

finish=$(carved "$ROOT/src/daemon/ops/typed_worker.c" '^op_finish' || true)
[ -n "$finish" ] || note "could not carve op_finish()"
printf '%s\n' "$finish" | grep -q 'FP_EV_OP_FINISHED' ||
	note "the daemon does not send FP_EV_OP_FINISHED"
printf '%s\n' "$finish" | grep -q 'finished.status' ||
	note "the finish event carries no status"

# The old channel is gone, not merely unused: a client that still looked for
# a trailer would wait for bytes that never come, and a daemon that still
# wrote one would put them in the caller's stream.
if grep -rq 'FPKG_TRAILER_MAGIC' "$ROOT/src" "$ROOT/include" 2>/dev/null; then
	note "the framed trailer magic is still in the tree after the framed path"
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "PASS: the client reports the daemon's exit status - 0 for the $n_ok"
	echo "      commands that succeed and non-zero for the $n_no that are"
	echo "      refused - through FP_EV_OP_FINISHED, and no framing bytes reach"
	echo "      the caller's stdout."
fi
exit $fail
