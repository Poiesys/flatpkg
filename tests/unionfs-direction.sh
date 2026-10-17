#!/bin/sh
# unionfs-direction.sh - the tree's only unionfs mount must keep `below = true` and its
# argument order, or the stack inverts while reading identically.  The direction needs root
# to observe, so this reads the shape of the decision.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
fail=0
note() { echo "FAIL: $*"; fail=1; }

tmp=$(mktemp -d /tmp/.fpkg-union.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

# shellcheck source=tests/commentlib.sh
. "$(dirname "$0")/commentlib.sh"

strip_comments < "$ROOT/src/daemon/sandbox_root.c" > "$tmp/sb.c"

# A call site is not a mention: the token also appears in the definition, and
# `m_unionfs` is the name of an argument in `mount_tool`'s tail call.
calls() { # calls CARVED TOKEN
	awk -v tok="$2" '
		{
			i = index($0, tok)
			if (i == 0) next
			before = substr($0, 1, i - 1)
			if (before ~ /"/) next
			print
		}' "$1"
}

fn=$(carve_function '^m_unionfs' "$tmp/sb.c" || true)
if [ -z "$fn" ]; then
	note "could not carve m_unionfs() out of src/daemon/sandbox_root.c - the shape moved"
else
	printf '%s\n' "$fn" > "$tmp/fn.c"

	# ---- 3. the two paths must not be able to disagree -----------------
	# The direct path's option string is built above the `if` and used under it, so
	# require the `&&`: its absence is the inversion, in the line that looks constant.
	direct=$(grep -n 'do_nmount("unionfs"' "$tmp/fn.c" | head -1 | cut -d: -f1)
	[ -n "$direct" ] || {
		note "m_unionfs() no longer has a direct do_nmount() path - the shape"
		echo "       moved, and this guard is no longer looking at the thing it"
		echo "       was written for"
	}
	if [ -n "$direct" ]; then
		# `|| true` because when the `&&` is GONE the pattern matches nothing, grep
		# exits 1, and under `set -e` an ASSIGNMENT whose substitution fails ends the
		# script before the `case` written to handle exactly that.
		cond=$(sed -n "${direct}p" "$tmp/fn.c" |
			grep -oE 'if *\(below[^)]*\)' || true)
		case "$cond" in
		*'&&'*) : ;;
		*) note "m_unionfs()'s direct path no longer tests \`below\`: its option"
		   echo "       string is built unconditionally as \"below\", so dropping the"
		   echo "       && makes the function mount below even when told not to" ;;
		esac
	fi
	# and the fallback must choose its -o from the same flag
	# Pattern matches "below ? (char*)\"below\" : (char*)\"rw\"" with optional spaces
	grep -q 'below ? (char\*)"below" *: *(char\*)"rw"' "$tmp/fn.c" || {
		note "m_unionfs()'s fallback no longer picks -o below/rw from the flag,"
		echo "       so the two mount paths can disagree about the direction"
	}
	[ "$fail" -eq 0 ] &&
		echo "ok   m_unionfs()'s two mount paths agree: the direct one is gated"
	echo "        on \`below\`, and the fallback picks -o from the same flag"
fi

# ---- 1 and 2. the one call site's direction and argument order ----------
# The call WRAPS, so it is joined before it is read: `m_unionfs(sb,` is on one line
# and the rest on the next two.  A line-oriented read gives three wrong answers.
joined=$(tr '\n' ' ' < "$tmp/sb.c")
# Counted, not windowed: a character window on a wrapped call cuts off the very
# text the comparison needs.  `grep -c` counts LINES and `$joined` is one line, so
# `-o | wc -l` everywhere.  Safe without `|| true`: the pipeline's status is `tr`'s.
n=$(printf '%s' "$joined" |
	grep -o 'm_unionfs(sb,[[:space:]]*plan->xdg_runtime_dir,[[:space:]]*runuser,[[:space:]]*true' |
	wc -l | tr -d ' ')
allc=$(printf '%s' "$joined" | grep -o 'm_unionfs(sb,' | wc -l | tr -d ' ')
[ "${n:-0}" -eq 1 ] && [ "${allc:-0}" -eq 1 ] || {
	note "found ${allc:-0} unionfs mount(s), ${n:-0} of the documented shape (exactly one"
	echo "       is expected: the /usr and /etc unions were removed for EINVAL"
	echo "       crashes and DESIGN.md section 2 says so).  A new union mount is a"
	echo "       deliberate change - update DESIGN.md section 2 and this file with it."
}
[ "${n:-0}" -eq 1 ] && [ "${allc:-0}" -eq "${n:-0}" ] && {
	echo "ok   the one unionfs mount is m_unionfs(sb, plan->xdg_runtime_dir,"
	echo "        runuser, true): below, host runtime dir UNDER the per-app one"
}

if [ $fail -eq 0 ]; then
	echo "PASS: the tree's only unionfs mount is the documented one, in the"
	echo "      documented direction, and its two code paths cannot disagree"
fi
exit $fail
