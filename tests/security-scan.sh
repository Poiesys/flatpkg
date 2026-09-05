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
# dbus-1.0 and glib-2.0 include paths are needed for ostree.c and the GDBus
# modules.  -w silences compiler warnings: this is an analysis run, not a build.
CF="-Iinclude -Ibuild -std=gnu11 -include sys/param.h"
CF="$CF -I/usr/local/include/dbus-1.0 -I/usr/local/lib/dbus-1.0/include"
CF="$CF -I/usr/local/include/glib-2.0 -I/usr/local/lib/glib-2.0/include"
CF="$CF -w"

# ---- the privileged set ----------------------------------------------
FILES="\
src/daemon/daemon.c src/daemon/sandbox.c src/ostree.c src/shared/store.c src/shared/config.c src/daemon/ops/store_write.c src/shared/dbus.c src/daemon/ops/dbus_proxy.c \
src/shared/permissions.c src/shared/protocol.c src/shared/instance.c src/daemon/ops/instance_ctl.c src/cli/portal.c src/cli/sessionhelper.c \
src/daemon/systemhelper.c src/cli/dispatch.c"

# ---- strcpy/sprintf/gets/strcat exemptions ---------------------------
# Each entry: file:function (that call name is skipped in that file) or file (skipped
# entirely).  Add one only when the call is provably safe and say why.
EXEMPT_STRCPY="src/daemon/sandbox.c:ld_path"          # strlcpy already used
EXEMPT_SPRINTF="src/ostree.c:repo_arg"         # bounded by snprintf
EXEMPT_STRCAT="src/ostree.c:key_out"           # bounded by realloc
# No gets() exemptions: none exist in the tree.

fail=0
tidy_fail=0

echo "security-scan: clang-tidy + insecure-API grep over privileged modules"
echo

# ---- clang-tidy scan --------------------------------------------------
TMP=$(mktemp -d /tmp/.fpkg-tidyscan.XXXXXX)
trap 'rm -rf "$TMP"' EXIT

for f in $FILES; do
	echo "-- $f"
	# Captured to a file so compile errors can be checked for separately.  The limit
	# stops a hung analyser: ostree.c takes 60-90s here.  The status is captured with
	# `||` because `set -e` would exit on a bare `cmd; rc=$?`.
	tidy_rc=0
	timeout "${FPKG_TIDY_TIMEOUT:-600}" "$TIDY" -checks="$CKS" "$f" -- $CF \
		> "$TMP/out.txt" 2>&1 || tidy_rc=$?

	# timeout (124) means the file took too long to analyse.
	# That is not a finding, but it is not clean either.
	if [ "$tidy_rc" -eq 124 ]; then
		echo "TIMEOUT (${FPKG_TIDY_TIMEOUT:-600}s exceeded)"
		tidy_fail=1
		continue
	fi

	# Compile errors: "error:" lines that are not warnings.
	# A compile error means the file cannot be analysed and is a failure.
	if grep -q 'error:' "$TMP/out.txt"; then
		grep 'error:' "$TMP/out.txt" | head -5
		tidy_fail=1
		continue
	fi

	# Warnings: filter out the DeprecatedOrUnsafeBufferHandling noise
	# (every sprintf/snprintf triggers it) and count the rest.
	W=$(grep 'warning:' "$TMP/out.txt" | grep -v DeprecatedOrUnsafeBufferHandling || true)
	if [ -n "$W" ]; then
		echo "$W"
		tidy_fail=1
	fi
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