#!/bin/sh
# runtime-dir.sh - the root worker must not act on a directory that is not
# already the caller's: every ownership test in sandbox.c must refuse inside the
# statement it guards, and the chmod/chown must stay behind that refusal.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

fail=0
note() { echo "FAIL: $*"; fail=1; }

tmp=$(mktemp -d /tmp/.fpkg-rtdir.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

# The canonical stripper, never a hand-written copy.
# shellcheck source=tests/commentlib.sh
. "$ROOT/tests/commentlib.sh"

strip_comments < "$ROOT/src/daemon/sandbox_root.c" > "$tmp/sb.c"

# ---- 1. every ownership test must refuse inside the statement it guards --
# Only lines that COMPARE the uid are sites: a grep for `st_uid` also finds
# warning messages' `(unsigned)st.st_uid`, which is not a second site.
grep -nE 'st_uid *[!=]=' "$tmp/sb.c" | cut -d: -f1 > "$tmp/sites"
nsites=$(grep -c . "$tmp/sites" || true)
# The floor: a site that cannot be found is a site that cannot be checked, so a
# count that drops is a failure rather than a smaller pass.
[ "${nsites:-0}" -ge 3 ] || {
	echo "FAIL: found ${nsites:-0} ownership test(s) in src/daemon/sandbox_root.c, below"
	echo "       the floor of 3 - the extraction broke or the shape moved"
	fail=1
}
while read -r ul; do
	[ -n "$ul" ] || continue
	if sed -n "${ul}p" "$tmp/sb.c" | grep -q '{'; then
		# braced: the end is this block's close brace
		uend=$(awk -v S="$ul" 'NR >= S && NR > S {
			o += gsub(/\{/, "{"); c += gsub(/\}/, "}")
			if (o > 0 && c >= o) { print NR; exit } }' "$tmp/sb.c")
	else
		# brace-less: the body is one statement, so the end is its `;`
		uend=$(awk -v S="$ul" 'NR > S && /;/ { print NR; exit }' "$tmp/sb.c")
	fi
	[ -n "$uend" ] || uend=$((ul + 2))
	blk=$(sed -n "${ul},${uend}p" "$tmp/sb.c")
	# Tests against 0 restore ownership of a path already verified to be the
	# caller's; they are not refusing access to a foreign one.
	if printf '%s\n' "$blk" | grep -qE 'st_uid *!= *0|st_gid *!= *0'; then
		continue
	fi
	if ! printf '%s\n' "$blk" | grep -q 'return'; then
		note "the ownership test at src/daemon/sandbox_root.c:$ul does not refuse inside"
		echo "       the statement it guards: root would go on to act on a path"
		echo "       that is not the caller's"
	fi
	# The trap: in a root worker `getuid()` is 0, so a test against it reads
	# every caller's directory as somebody else's and every launch fails.  The
	# comparison must be against the CALLER's uid, however it is spelled.
	if printf '%s\n' "$blk" | grep -qE 'get[eu]id\(\)'; then
		note "the ownership test at src/daemon/sandbox_root.c:$ul compares against"
		echo "       getuid()/geteuid(), which is 0 in the root worker"
	fi
done < "$tmp/sites"

# ---- 2. the ordering claims, in the one function that acts on the dir ----
fn=$(carve_function '^sandbox_ensure_runtime_dir' "$tmp/sb.c")
printf '%s\n' "$fn" > "$tmp/fn.c"
# A carve that stopped early loses every assertion below, and each would
# then pass on an empty set - so its shape is checked first.
case "$fn" in
*sandbox_ensure_runtime_dir*) : ;;
*) echo "FAIL: could not carve sandbox_ensure_runtime_dir() out of src/daemon/sandbox_root.c"
   exit 1 ;;
esac
grep -q '^}$' "$tmp/fn.c" ||
	{ echo "FAIL: the carve stopped before the function's closing brace"; exit 1; }
grep -q 'xdg_runtime_dir' "$tmp/fn.c" ||
	{ echo "FAIL: the carve has no xdg_runtime_dir in it - the shape moved"; exit 1; }

at() { grep -n "$1" "$tmp/fn.c" | head -1 | cut -d: -f1; }
uidline=$(at 'st.st_uid != plan->uid')
refuse=$(at 'return -1')
tighten=$(grep -n 'chmod(plan->xdg_runtime_dir, 0700)' "$tmp/fn.c" | head -1 |
	cut -d: -f1)
hand=$(grep -nE 'chown(_nofollow)?\(plan->xdg_runtime_dir' "$tmp/fn.c" |
	head -1 | cut -d: -f1)

if [ -z "$uidline" ] || [ -z "$refuse" ]; then
	note "sandbox_ensure_runtime_dir(): cannot locate both the ownership"
	echo "       test and a refusal, so 'never take over a directory that is"
	echo "       not already the caller's' is UNVERIFIED here"
elif [ "$refuse" -le "$uidline" ]; then
	note "sandbox_ensure_runtime_dir() refuses at $refuse, before it tests"
	echo "       ownership at $uidline - that refuses everything, or nothing"
fi
if [ -z "$tighten" ]; then
	note "sandbox_ensure_runtime_dir() never tightens the mode; round 19's"
	echo "       'the session runtime dir ends up root-owned' is unaddressed"
elif [ -n "$refuse" ] && [ "$tighten" -le "$refuse" ]; then
	note "sandbox_ensure_runtime_dir() tightens the mode at $tighten, before"
	echo "       it refuses at $refuse - that is root chmod'ing a directory"
	echo "       that is not the caller's"
fi
if [ -z "$hand" ]; then
	note "sandbox_ensure_runtime_dir() never hands a directory it created to"
	echo "       the caller: it would be root-owned and unusable"
elif [ -n "$refuse" ] && [ "$hand" -le "$refuse" ]; then
	note "sandbox_ensure_runtime_dir() chowns at $hand, before it refuses at"
	echo "       $refuse - the handover is reachable on a refused directory"
fi
# The floor on the operations themselves: the ownership test, the refusal,
# the tightening and the handover.  A tree that loses one has changed
# shape, and a count that can only be zero is not a check.
n=$(grep -cE 'st_uid|chmod\(plan->xdg_runtime_dir, 0700\)|chown(_nofollow)?\(plan->xdg_runtime_dir' \
	"$tmp/fn.c" || true)
[ "${n:-0}" -ge 4 ] || {
	echo "FAIL: only $n of the four documented operations are present in"
	echo "       sandbox_ensure_runtime_dir(), below the floor of 4"
	fail=1
}

# ---- 3. the privilege boundary, both directions -------------------------
# Per-app data directories under the user's HOME are created by the UNPRIVILEGED
# client; only the runtime dir is the root worker's.
if grep -q 'client_ensure_app_data_dirs' "$ROOT/src/daemon/daemon.c" 2>/dev/null; then
	note "src/daemon/daemon.c calls client_ensure_app_data_dirs(): the root worker"
	echo "       would be creating directories under the user's home, which is"
	echo "       the client's job (round 19)"
fi
if grep -q 'sandbox_ensure_runtime_dir' "$ROOT/src/cli/client.c" 2>/dev/null; then
	note "src/cli/client.c calls sandbox_ensure_runtime_dir(): the unprivileged"
	echo "       client must not chown or chmod anything"
fi
if ! grep -lq 'client_ensure_app_data_dirs' "$ROOT"/src/*.c "$ROOT"/src/*/*.c "$ROOT"/src/*/*/*.c 2>/dev/null; then
	note "no source creates the per-app data directories; the client is"
	echo "       supposed to, before it dispatches to the worker"
fi

if [ $fail -eq 0 ]; then
	echo "PASS: all $nsites ownership tests in src/daemon/sandbox_root.c refuse inside the"
	echo "      statement they guard, and none compares against the worker's own"
	echo "      uid; the worker refuses a runtime dir that is not the caller's"
	echo "      (test at $uidline, refusing at $refuse) before it tightens"
	echo "      ($tighten) or hands anything over ($hand); and the per-app data"
	echo "      directories are still created client-side"
fi
exit $fail
