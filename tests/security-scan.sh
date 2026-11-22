#!/bin/sh
# security-scan.sh - clang-tidy static analysis of the privileged modules, plus a
# grep for strcpy/sprintf/gets/strcat in them.  Compile errors are failures; skips
# when clang-tidy19 is not installed.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

# ---- skip: no clang-tidy19 -------------------------------------------
TIDY=$(command -v clang-tidy19 2>/dev/null || true)
if [ -z "$TIDY" ]; then
	echo "SKIP: clang-tidy19 not found"
	exit 0
fi

# ---- the check set ---------------------------------------------------
# Broad bugprone-* is too noisy; this is the curated list that caught real
# defects: two UAFs, unchecked setuid/setgid, malloc(0) on an empty store.
CKS='\
-*,\
clang-analyzer-security.*,\
clang-analyzer-core*,\
clang-analyzer-deadcode.DeadStores,\
clang-analyzer-unix.Malloc,\
clang-analyzer-unix.StdCLibraryFunctions,\
clang-analyzer-optin.portability.UnixAPI,\
bugprone-suspicious-realloc-usage,\
bugprone-argument-comment,\
bugprone-macro-parentheses,\
bugprone-sizeof-expression,\
bugprone-signal-handler,\
bugprone-undefined-memory-manipulation\
'
CKS=$(echo "$CKS" | tr -d '\n')

# ---- compile flags ---------------------------------------------------
# dbus-1.0 and glib-2.0 include paths are needed for the ostree units and GDBus
# modules.  -w silences compiler warnings: this is an analysis run, not a build.
CF="-Iinclude -Ibuild -std=gnu11 -include sys/param.h"
CF="$CF -I/usr/local/include/dbus-1.0 -I/usr/local/lib/dbus-1.0/include"
CF="$CF -I/usr/local/include/glib-2.0 -I/usr/local/lib/glib-2.0/include"
CF="$CF -w"

# ---- the privileged set ----------------------------------------------
FILES="\
src/daemon/daemon.c src/daemon/sandbox.c src/daemon/sandbox_root.c src/daemon/sandbox_exec.c src/daemon/apply.c src/daemon/extensions.c src/cli/solver.c src/ostree.c src/ostree_plan.c src/ostree_cmds.c src/shared/store.c src/shared/config.c src/daemon/ops/store_write.c src/daemon/ops/evui.c src/shared/dbus.c src/daemon/ops/dbus_proxy.c \
src/shared/permissions.c src/shared/protocol.c src/shared/instance.c src/daemon/ops/instance_ctl.c src/cli/portal.c src/cli/sessionhelper.c \
src/daemon/systemhelper.c src/cli/dispatch.c"

# ---- strcpy/sprintf/gets/strcat exemptions ---------------------------
# Each entry: file:function (that call name is skipped in that file) or file (skipped
# entirely).  Add one only when the call is provably safe and say why.
EXEMPT_STRCPY="src/daemon/sandbox.c:ld_path"          # strlcpy already used
EXEMPT_SPRINTF="src/ostree.c:repo_arg"         # bounded by snprintf
EXEMPT_STRCAT="src/ostree_cmds.c:key_out"      # bounded by realloc
# No gets() exemptions: none exist in the tree.

fail=0
tidy_fail=0

echo "security-scan: clang-tidy + insecure-API grep over privileged modules"
echo

# ---- clang-tidy scan --------------------------------------------------
TMP=$(mktemp -d /tmp/.fpkg-tidyscan.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

# Each unit is analysed in its own process: 24 files at ~1-10 minutes each is
# an hour of wall time one after another, and the analyser is the only slow
# part of the suite.  The outputs are collected per file and printed in order,
# so the report reads the same as a serial run.
tidy_one() {
	f=$1
	out=$2
	rc=0
	timeout "${FPKG_TIDY_TIMEOUT:-600}" "$TIDY" -checks="$CKS" "$f" -- $CF \
		> "$out" 2>&1 || rc=$?
	if [ "$rc" -eq 124 ]; then
		echo "TIMEOUT" > "$out"
		echo "tidytimeout" > "$out.verdict"
	elif grep -q 'error:' "$out"; then
		echo "tidyerror" > "$out.verdict"
	elif grep 'warning:' "$out" |
		grep -v DeprecatedOrUnsafeBufferHandling > "$out.warn" &&
		[ -s "$out.warn" ]; then
		echo "tidywarn" > "$out.verdict"
	else
		echo "clean" > "$out.verdict"
	fi
}

# The analyser is CPU-bound, so one unit per core is the useful ceiling, and
# the batches make the fan-out portable (/bin/sh has no `wait -n`).
jobs=${FPKG_TIDY_JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}
[ "$jobs" -gt 8 ] && jobs=8
[ "$jobs" -lt 1 ] && jobs=1
running=0
n=0
for f in $FILES; do
	n=$((n + 1))
	tidy_one "$f" "$TMP/out.$n" &
	running=$((running + 1))
	# Bounded fan-out, in portable batches: the analyser is memory-hungry
	# and /bin/sh has no `wait -n`.
	if [ "$running" -ge "$jobs" ]; then
		wait
		running=0
	fi
done
wait

n=0
for f in $FILES; do
	n=$((n + 1))
	echo "-- $f"
	verdict=$(cat "$TMP/out.$n.verdict" 2>/dev/null || echo tidyerror)
	case "$verdict" in
	clean) ;;
	tidytimeout)
		echo "TIMEOUT (${FPKG_TIDY_TIMEOUT:-600}s exceeded)"
		tidy_fail=1
		;;
	tidyerror)
		grep 'error:' "$TMP/out.$n" | head -5
		tidy_fail=1
		;;
	tidywarn)
		cat "$TMP/out.$n.warn"
		tidy_fail=1
		;;
	*)
		echo "no verdict recorded"
		tidy_fail=1
		;;
	esac
done

if [ "$tidy_fail" -eq 1 ]; then
	echo
	echo "FAIL: clang-tidy findings above"
	fail=1
else
	echo "clang-tidy: clean"
fi

# ---- insecure-API grep ------------------------------------------------
echo
echo "-- insecure API grep (strcpy/sprintf/gets/strcat)"

check_api() {
	fn="$1"
	exempt="$2"
	found=0
	for f in $FILES; do
		# Skip exempted file for this API.
		case " $exempt " in *" $f:"*|*" $f "*) continue;; esac

		# Strip C comments so a mention in a comment is not a finding.
		. "$(dirname "$0")/commentlib.sh"
		stripped=$(strip_comments < "$f")

		# Count real calls: the function name at word boundary,
		# not inside a string literal or comment.
		c=$(printf '%s\n' "$stripped" | grep -c "\b${fn}(" || true)
		if [ "$c" -gt 0 ]; then
			printf '%s\n' "$stripped" | grep -n "\b${fn}(" | head -3
			found=1
		fi
	done
	if [ "$found" -eq 1 ]; then
		fail=1
		return 1
	fi
	return 0
}

# strcpy
if ! check_api strcpy ""; then
	echo "FAIL: strcpy() found (use strlcpy or memcpy)"
fi
# sprintf
if ! check_api sprintf ""; then
	echo "FAIL: sprintf() found (use snprintf)"
fi
# gets
if ! check_api gets ""; then
	echo "FAIL: gets() found (use fgets)"
fi
# strcat
if ! check_api strcat ""; then
	echo "FAIL: strcat() found (use strlcat or strncat)"
fi

if [ "$fail" -eq 0 ]; then
	echo
	echo "PASS: security-scan clean"
	exit 0
else
	echo
	echo "FAIL: security-scan found findings"
	exit 1
fi