#!/bin/sh
# stream-split.sh - the daemon must not flatten the worker's two output streams: the
# caller's stdout and stderr are forwarded as ancillary fds and dropped onto 1 and 2, so
# `2>/dev/null` suppresses diagnostics only.  Needs the daemon and the flatpkg group.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FLATPKG="$ROOT/build/flatpkg"
[ -x "$FLATPKG" ] || { echo "SKIP: build/flatpkg not built"; exit 0; }
id -Gn | tr ' ' '\n' | grep -qx flatpkg ||
	{ echo "SKIP: not in the flatpkg group; the root worker is the subject"; exit 0; }
[ -S /var/run/flatpkg/control.sock ] ||
	{ echo "SKIP: no control socket; the daemon is not running"; exit 0; }

fail=0
note() { echo "FAIL: $1"; fail=1; }

# The environment the subject reads, owned here rather than inherited: every name is
# PINNED, or the host decides the verdict.  PATH is minimal because the code resolves
# program names through it, and HOME/XDG_RUNTIME_DIR are a temp dir.
T=$(mktemp -d /tmp/.fpkg-streams.XXXXXX)
trap 'rm -rf "$T"' EXIT
export HOME="$T"
export XDG_RUNTIME_DIR="$T/run"
export PATH="/usr/bin:/bin"
export TERM=dumb
export XDG_CURRENT_DESKTOP= DBUS_SESSION_BUS_ADDRESS= WAYLAND_DISPLAY=
export NO_COLOR= FORCE_COLOR=
mkdir -p "$XDG_RUNTIME_DIR"

# 1. The completion invariant, in the exact form main.c claims: an error about
#    the completion list itself must not land in the list a shell function reads.
out=$("$FLATPKG" complete bogus 2>/dev/null || true)
[ -z "$out" ] || note "a diagnostic reached stdout through 'complete': $(printf '%s' "$out" | head -1)"

# 2. The general form: a worker's error is on the caller's stderr.  Use -y to skip
# the confirmation prompt (no TTY in test); an absent app keeps the "already
# absent" answer, but the request was not satisfied, so the status is non-zero.
rc=0
err=$("$FLATPKG" uninstall -y org.example.NoSuchAppHere 2>&1 1>/dev/null) ||
	rc=$?
[ -n "$err" ] || note "the worker's error is on neither stream - it is being dropped"
case "$err" in
	*"already absent"*) : ;;
	*) note "the worker's error arrived on stderr but does not read like one: $err" ;;
esac
[ "$rc" -ne 0 ] || note "uninstall of an absent app exited 0; the request was not satisfied"
# An absent app shows a changeset table (orphaned extensions that would be
# removed) on stdout: the table is the command's output, the error is not.
out=$("$FLATPKG" uninstall -y org.example.NoSuchAppHere 2>/dev/null || true)
# Accept either empty stdout (old behavior) or a table (new behavior).
case "$out" in
	*"ID"*"Arch"*"Branch"*"Op"*"Remote"*) : ;;
	"") : ;;
	*) note "unexpected stdout content: $(printf '%s' "$out" | head -1)" ;;
esac

# 3. And the data side is untouched: the bare list is still bare and still on
#    stdout.  The count is compared between the two spellings, because the store
#    is the user's and its size is not this guard's claim.
a=$("$FLATPKG" complete apps 2>/dev/null | wc -l | tr -d ' ')
b=$("$FLATPKG" complete apps 2>&1 | wc -l | tr -d ' ')
if [ "$a" = "$b" ]; then :; else note "complete apps lost $((b - a)) line(s) when stderr was redirected"; fi
# No line of a bare list may carry a left column, a badge or colour: no
# ESC byte, and none of the bracket runs the logger draws.
if "$FLATPKG" complete apps 2>/dev/null | LC_ALL=C grep -q "$(printf '\033')"; then
	note "a completion candidate carries a colour escape"
fi
if [ "$a" -gt 0 ]; then
	if "$FLATPKG" complete apps 2>/dev/null | grep -q '\[ WARN\|\[ ERROR\|\[ INFO'; then
		note "a completion candidate carries a log badge"
	fi
else
	echo "note  the store has no apps; the bare-output checks had no lines"
fi

# 4. `run` splits too.  There are TWO ptys here and NEITHER is the worker's
#    stdout: `run`'s is the app's (FLATPKG_APP_TTY), and the install/update/
#    remove/uninstall one is the worker's STDIN.
out=$("$FLATPKG" run org.example.NoSuchAppHere 2>/dev/null || true)
[ -z "$out" ] || note "a diagnostic reached stdout through 'run': $(printf '%s' "$out" | head -1)"
err=$("$FLATPKG" run org.example.NoSuchAppHere 2>&1 1>/dev/null || true)
[ -n "$err" ] || note "run's diagnostic is on neither stream"

# 5. A command's TABLE is its output, and a diagnostic is not.  fplog renders widgets
#    to **stderr** by contract, so `fp_print_table()` takes a sink: FP_TABLE_OUTPUT for
#    a result, FP_TABLE_WIDGET inside a TUI.  Both halves are asserted.
out=$("$FLATPKG" list 2>/dev/null || true)
[ -n "$out" ] || note "flatpkg list put nothing on stdout: a table is the command's OUTPUT, not a diagnostic"
err=$("$FLATPKG" list 2>&1 1>/dev/null || true)
[ -z "$err" ] || note "flatpkg list also wrote to stderr: $(printf '%s' "$err" | head -1)"
out=$("$FLATPKG" info io.github.kolunmi.Bazaar 2>/dev/null || true)
[ -n "$out" ] || note "flatpkg info put nothing on stdout (is io.github.kolunmi.Bazaar installed? this store's, not a claim)"

# 6. `--version` is bare output too, and is read by scripts.
v=$("$FLATPKG" --version 2>/dev/null || true)
[ -n "$v" ] || note "--version printed nothing on stdout"

# One verdict, under the flag: `[ $fail -eq 0 ] && echo "PASS: ..."` followed by
# a second `echo` on the next line binds the continuation to the first echo only,
# so the second runs unconditionally and prints half a PASS line on a failing run.
if [ $fail -eq 0 ]; then
	echo "PASS: the worker's stdout and stderr are the caller's own streams,"
	echo "      so 2>/dev/null suppresses diagnostics and a completion list"
	echo "      stays bare"
fi
exit $fail
