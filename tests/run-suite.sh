#!/bin/sh
# run-suite.sh - the whole guard suite in one command.
# It counts a skip as a skip, notices a guard that wrote to the tree, and
# enumerates its own guards - none of which a retyped loop in prose can do.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR="${BUILD_DIR:-build}"
LOGS=$(mktemp -d /tmp/.fpkg-suite.XXXXXX)
# KEEP_LOGS=1 leaves the logs behind, which is what a residue report is
# useless without.  Default off: a suite that leaves files around is the
# thing this script exists to catch.
trap '[ "${KEEP_LOGS:-0}" = 1 ] || rm -rf "$LOGS"' EXIT

# The manifest is path, size and mtime, taken over the whole tree
# *including* build/ - measured, not assumed: compiles.sh builds into a
# scratch BUILD= and the real one does not move.
manifest() {
	find "$ROOT" -path "$ROOT/.git" -prune -o -type f -print0 |
		xargs -0 stat -f '%N %z %m' | sort
}

# Three categories, and every script in tests/ must be in one: the guards this
# file runs, SOURCED (never run: shimtest.sh, commentlib.sh, probelib.sh,
# capturelib.sh, scratchd.sh), and NOT_GUARDS (this file).
SOURCED='shimtest.sh commentlib.sh probelib.sh capturelib.sh scratchd.sh'
NOT_GUARDS='run-suite.sh'

# -- preflight: the guards must run the binary this tree describes -----------
# A *missing* binary is a note; a stale one is exit 2, never a failure.  The input
# list is the build's, not "the repository": editing a doc must not block a run.
if [ -x "$ROOT/${BUILD_DIR}/flatpkg" ]; then
	# **Only the sources the binary is built from.**  `find src` also matches
	# the seven `*_shim.c` files, which are separate Linux ELFs and NOT in
	# `OBJS`, so editing one called a current binary stale.
	shims=$(ls "$ROOT"/src/*_shim.c 2>/dev/null | sed "s|^$ROOT/||")
	stale=$(find "$ROOT/src" "$ROOT/include" "$ROOT/Makefile" \
	    "$ROOT/usage.hlp" -type f -newer "$ROOT/${BUILD_DIR}/flatpkg" 2>/dev/null |
	    sed "s|^$ROOT/||" |
	    grep -vxF -e "$shims" 2>/dev/null | sort)
	if [ -n "$stale" ]; then
		echo "FAIL  ${BUILD_DIR}/flatpkg is older than sources in this tree, so"
		echo "      every guard that runs it is testing a build the tree"
		echo "      does not describe:"
		printf '%s\n' "$stale" | head -8 | sed 's/^/        /'
		echo
		echo "      make HELPLANG_DIR=\${HELPLANG_DIR:-$HELPLANG_DIR} && \\"
		echo "      sh tests/run-suite.sh"
		exit 2
	fi
else
	echo "note  there is no ${BUILD_DIR}/flatpkg; the guards that run the binary"
	echo "      will SKIP and the summary will count them as skips."
fi

guards=$*
if [ -z "$guards" ]; then
	guards=$(ls "$ROOT"/tests/*.sh | sed 's|.*/||' | sort | tr '\n' ' ')
fi

n_pass=0
n_fail=0
n_skip=0
skipped=
failed=
not_guards=
sourced=

manifest > "$LOGS/before" || { echo "cannot read the tree; giving up"; exit 2; }

for g in $guards; do
	is_guard=yes
	for s in $SOURCED $NOT_GUARDS; do
		if [ "$g" = "$s" ]; then is_guard=no; fi
	done
	if [ "$is_guard" = no ]; then
		# "-", not "skip": a script that is not a guard is not a skip,
		# and the summary counts skips separately.  Two meanings of one
		# word in one screen is how "0 skipped" gets read as a pass.
		was_sourced=no
		for s2 in $SOURCED; do
			[ "$g" = "$s2" ] && was_sourced=yes
		done
		if [ "$was_sourced" = yes ]; then
			sourced="$sourced $g"
			echo "-    $g (sourced, never run)"
		else
			not_guards="$not_guards $g"
			echo "-    $g (not a guard)"
		fi
		continue
	fi
	if [ ! -f "$ROOT/tests/$g" ]; then
		echo "FAIL  $g: no such file in tests/"
		n_fail=$((n_fail + 1))
		failed="$failed $g"
		continue
	fi

	log="$LOGS/$g.log"
	if sh "$ROOT/tests/$g" > "$log" 2>&1; then
		rc=0
	else
		rc=$?
	fi

	if [ "$rc" -ne 0 ]; then
		n_fail=$((n_fail + 1))
		failed="$failed $g"
		echo "FAIL  $g (rc=$rc)"
		tail -25 "$log" | sed 's/^/        /'
		continue
	fi

	# A guard whose whole output is one SKIP line did not run; one that
	# printed a SKIP *and* a verdict did run and skipped part way through, so
	# the verdict line decides, not the SKIP.
	if grep -q '^SKIP:' "$log" && ! grep -qE '^(PASS|ok|FAIL)' "$log"; then
		n_skip=$((n_skip + 1))
		skipped="$skipped $g"
		echo "skip  $g - $(grep -m1 '^SKIP:' "$log" | cut -c6-)"
		continue
	fi

	n_pass=$((n_pass + 1))
	echo "pass  $g"
done

manifest > "$LOGS/after"
if ! diff -u "$LOGS/before" "$LOGS/after" > "$LOGS/residue" 2>&1; then
	n_fail=$((n_fail + 1))
	echo
	echo "FAIL the suite changed the tree it verifies (added, removed or"
	echo "     rewritten files - the whole diff is in $LOGS/residue, which"
	echo "     the trap above removes on exit; re-run with KEEP_LOGS=1):"
	sed 's/^/        /' "$LOGS/residue"
fi

echo
echo "passed $n_pass, failed $n_fail, skipped $n_skip"
if [ -n "$skipped" ]; then
	echo "  skipped (did not run):$skipped"
fi
if [ -n "$failed" ]; then
	echo "  failed: $failed"
fi
if [ -n "$sourced" ]; then
	echo "  sourced, never run:$sourced"
fi
if [ -n "$not_guards" ]; then
	echo "  not run (not guards):$not_guards"
fi

# No "is every script classified?" check here: the run set is every tests/*.sh
# minus the two declared lists, so an undeclared script is simply run.

if [ "$n_fail" -ne 0 ]; then
	exit 1
fi
exit 0
