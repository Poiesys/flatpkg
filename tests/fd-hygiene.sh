#!/bin/sh
# fd-hygiene.sh - jail_attach(2) chroots internally and refuses while any
# directory fd is open, so close_inherited_fds() must run first in
# sandbox_exec(), honour FLATPKG_FORWARD_FDS and spare 0/1/2.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
fail=0
note() { echo "FAIL: $*"; fail=1; }

tmp=$(mktemp -d /tmp/.fpkg-fd.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

# shellcheck source=tests/commentlib.sh
. "$(dirname "$0")/commentlib.sh"

strip_comments < "$ROOT/src/daemon/sandbox.c" > "$tmp/sb.c"

# Calls, not text: a token with a quote somewhere before it on the line is
# inside a string literal.
calls() { # calls CARVED-FILE TOKEN
	awk -v tok="$2" '
		{
			i = index($0, tok)
			if (i == 0) next
			before = substr($0, 1, i - 1)
			if (before ~ /"/) next          # inside a literal
			print
		}' "$1"
}

# ---- 1. the close precedes every attach, in sandbox_exec() --------------
ex=$(carve_function '^sandbox_exec\(' "$tmp/sb.c")
if [ -z "$ex" ]; then
	note "could not carve sandbox_exec() out of src/daemon/sandbox.c - the shape moved"
else
	printf '%s\n' "$ex" > "$tmp/ex.c"
	nclose=$(calls "$tmp/ex.c" 'close_inherited_fds();' | wc -l | tr -d ' ')
	nattach=$(calls "$tmp/ex.c" 'jail_attach(' | wc -l | tr -d ' ')
	[ "${nclose:-0}" -ge 1 ] || {
		note "sandbox_exec() never calls close_inherited_fds(): every launch"
		echo "       would fail at jail_attach() with EPERM, because the kernel"
		echo "       refuses to chroot while a directory fd is open"
	}
	[ "${nattach:-0}" -ge 1 ] || {
		note "sandbox_exec() has no jail_attach() call to order against - the"
		echo "       shape moved, and this guard is no longer looking at the"
		echo "       thing it was written for"
	}
	if [ "${nclose:-0}" -ge 1 ] && [ "${nattach:-0}" -ge 1 ]; then
		# The first close, and the first attach AFTER it: taking the first
		# attach would let a call before the close look correctly ordered.
		cl=$(grep -n 'close_inherited_fds();' "$tmp/ex.c" |
			grep -v '"' | head -1 | cut -d: -f1)
		at=$(grep -n 'jail_attach(' "$tmp/ex.c" | cut -d: -f1 |
			awk -v c="$cl" '$1 > c { print; exit }')
		if [ -z "$at" ]; then
			note "sandbox_exec() closes at line $cl and attaches only BEFORE"
			echo "       it - the call is unreachable by the time jail_attach runs"
		elif [ "$at" -lt "$cl" ]; then
			note "sandbox_exec() attaches at $at but only closes at $cl"
		else
			echo "ok   sandbox_exec() closes inherited fds at line $cl, before"
			echo "     its jail_attach() at line $at"
		fi
	fi
fi

# ---- 2 and 3. the function's own contract ------------------------------
f=$(carve_function '^close_inherited_fds' "$tmp/sb.c" || true)
if [ -z "$f" ]; then
	note "could not carve close_inherited_fds() out of src/daemon/sandbox.c"
else
	printf '%s\n' "$f" > "$tmp/f.c"
	# The getenv() call specifically, not the bare NAME: the function also
	# unsets the variable at its end, so a grep for the name is satisfied by
	# a line that has nothing to do with building the keep-list.
	grep -q 'getenv("FLATPKG_FORWARD_FDS")' "$tmp/f.c" || {
		note "close_inherited_fds() no longer reads FLATPKG_FORWARD_FDS, so the"
		echo "       portal's Spawn --forward-fd=N socket is closed before the"
		echo "       app can reach it (the forwarded-fd list is the exception)"
	}
	grep -q 'close_range' "$tmp/f.c" || {
		note "close_inherited_fds() no longer closes a RANGE - a per-fd loop"
		echo "       is what a rewrite to close(2) in a loop would look like,"
		echo "       and close(2) takes the app's terminal with it"
	}
	# The floor: 3 leaves 0/1/2 alone.  The regex also takes "unsigned low".
	low=$(grep -oE '(unsigned )?low = [0-9]+' "$tmp/f.c" | head -1 | grep -oE '[0-9]+' || true)
	[ -n "$low" ] || {
		note "close_inherited_fds() has no `low = N` water mark, so this guard"
		echo "       cannot see whether 0/1/2 are spared"
	}
	if [ -n "$low" ] && [ "$low" -lt 3 ]; then
		note "close_inherited_fds() starts closing at $low: 0/1/2 are the app's"
		echo "       terminal and closing them does not fail, it just takes the"
		echo "       app's stdio away"
	fi
	if [ -n "$low" ] && [ "$low" -ge 3 ]; then
		echo "ok   close_inherited_fds() closes from $low upward - 0/1/2, the"
		echo "     app's own stdio, are never touched"
	fi
	# The second call site is p11kit_ensure_server()'s forked child, where a
	# full wipe is right; losing it hands the helper the worker's descriptors.
	n=$(calls "$tmp/sb.c" 'close_inherited_fds();' | wc -l | tr -d ' ')
	[ "${n:-0}" -ge 2 ] || {
		note "only $n call site(s) of close_inherited_fds() left; one of them is"
		echo "       the p11-kit helper's forked child, and losing THAT one"
		echo "       would hand the helper the whole worker's descriptor set"
	}
	[ "${n:-0}" -ge 2 ] &&
		echo "ok   $n call sites, the second being p11kit_ensure_server()'s forked"
fi

if [ $fail -eq 0 ]; then
	echo "PASS: the launch closes inherited descriptors before jail_attach,"
	echo "      spares 0/1/2, and keeps the portal's forwarded fds"
fi
exit $fail
