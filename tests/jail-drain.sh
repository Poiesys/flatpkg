#!/bin/sh
# jail-drain.sh - the launch waits for the JAIL, not for the one pid it
# exec'd: kate's first process exits at once, so waitpid() returned while the
# app was still running and the jail was torn down around it.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tests/commentlib.sh"

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/src/daemon/sandbox_exec.c"

pass=0
fail=0
ok()   { printf '  ok    - %s\n' "$1"; pass=$((pass + 1)); }
bad()  { printf '  FAIL  - %s\n' "$1"; fail=$((fail + 1)); }

[ -f "$SRC" ] || { echo "jail-drain: no $SRC"; exit 1; }

# The body of sandbox_exec(), comments stripped so a sentence about the drain
# cannot be mistaken for the drain.  Anchor the range on the name: the source
# writes `int sandbox_exec(sandbox_t* sb, ...)` with no space before the `*`.
body=$(strip_comments < "$SRC" | awk '/^int sandbox_exec\(/,/^}/')

# 1. the drain is in this function
# Matched as a CALL, not as the word: the comment above the drain names it,
# and a sentence about a call is not a call.
if printf '%s\n' "$body" | grep -qE 'while \(jail_process_count\('; then
	ok "sandbox_exec() drains the jail"
else
	bad "sandbox_exec() does not drain the jail - apps that fork will be killed at teardown"
fi

# 2. it waits on the JAIL, not on the pid
if printf '%s\n' "$body" | grep -q 'jail_process_count(sb->jid)'; then
	ok "the drain waits on the jail id"
else
	bad "the drain does not wait on sb->jid"
fi

# 3. unbounded: no counter, no deadline, in the drain itself
drain=$(printf '%s\n' "$body" | awk '/while \(jail_process_count/,/^\t*}/')
if printf '%s\n' "$drain" | grep -qE 'left|deadline|timeout|JAIL_DRAIN_SECONDS'; then
	bad "the drain is bounded - kate was killed mid-use by a 30s bound"
else
	ok "the drain is unbounded (the launch is the app's lifetime)"
fi

# 4. the helper exists and reads the jail id from the kernel
if grep -q 'kinfo_getallproc' "$SRC" && grep -q 'ki_jid' "$SRC"; then
	ok "jail_process_count() enumerates by ki_jid"
else
	bad "jail_process_count() does not enumerate processes by jail id"
fi

# A floor, so a rename cannot make every claim above vacuous.
if [ "$pass" -lt 3 ]; then
	echo "jail-drain: only $pass claim(s) could be checked - the guard is not looking at what it thinks"
	exit 1
fi

printf 'jail-drain: passed %d, failed %d\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
