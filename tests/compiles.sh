#!/bin/sh
# compiles.sh - the rest of this suite is greps, so a tree that does not
# build, or builds with warnings, passes all of them.  Runs the real build
# into a scratch BUILD=; SKIPS when the toolchain is absent.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

command -v zig >/dev/null 2>&1 || {
	echo "SKIP: zig not found; the suite does not require a toolchain"
	exit 0
}

SCRATCH=$(mktemp -d /tmp/.fpkg-build.XXXXXX)
trap 'rm -rf "$SCRATCH"' EXIT

if ! make -C "$ROOT" -n BUILD="$SCRATCH" >/dev/null 2>&1; then
	echo "SKIP: make itself will not run here."
	exit 0
fi

log="$SCRATCH/build.log"
if ! make -C "$ROOT" BUILD="$SCRATCH" > "$log" 2>&1; then
	echo "FAIL the tree does not build - every other guard here is a grep and"
	echo "     would still report green.  Compiler output:"
	grep -E 'error:|Error|undefined reference|ld:' "$log" | head -20 |
		sed 's/^/       /'
	tail -5 "$log" | sed 's/^/       /'
	exit 1
fi

# The build must be warning-free too: a warning is not an error, so no
# guard here would otherwise see one.  A clean tree has zero warnings.
if grep -q 'warning:' "$log"; then
	echo "FAIL the tree builds with warnings - a warning is a defect this"
	echo "     suite cannot otherwise see, and it was one for six rounds."
	echo "     Compiler output:"
	grep 'warning:' "$log" | head -12 | sed 's/^/       /'
	exit 1
fi

# The sanitizer build runs by default, not on request: an out-of-bounds read
# in a parser is invisible until it is not.  The compiler must be able to link
# the runtime, so probe first and SKIP otherwise.
probe="$SCRATCH/sanprobe"
if ! cc -fsanitize=address,undefined -o "$probe" -x c - <<'EOF' >/dev/null 2>&1
int main(void) { return 0; }
EOF
then
	echo "SKIP: no address/undefined sanitizer runtime on this compiler"
	exit 0
fi

asan="$SCRATCH/asan"
if ! make -C "$ROOT" BUILD="$asan" \
	SANFLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -g" \
	> "$SCRATCH/asan.log" 2>&1; then
	echo "FAIL the tree does not build with the sanitizers:"
	grep -E 'error:|Error|undefined reference|ld:' "$SCRATCH/asan.log" |
		head -20 | sed 's/^/       /'
	tail -5 "$SCRATCH/asan.log" | sed 's/^/       /'
	exit 1
fi

export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
for args in "--version" "help" "help run" "complete commands" \
	    "complete words run" "complete man global"; do
	# shellcheck disable=SC2086
	if ! "$asan/flatpkg" $args >/dev/null 2>"$SCRATCH/san.err"; then
		echo "FAIL the sanitizer binary failed on 'flatpkg $args':"
		head -12 "$SCRATCH/san.err" | sed 's/^/       /'
		exit 1
	fi
	if [ -s "$SCRATCH/san.err" ]; then
		echo "FAIL the sanitizers reported a problem on 'flatpkg $args':"
		head -12 "$SCRATCH/san.err" | sed 's/^/       /'
		exit 1
	fi
done

echo "PASS: the tree still builds, with no warnings, and the sanitizer build"
echo "      runs the CLI clean (scratch BUILD dirs; the real one is untouched)"
exit 0

echo "FAIL the tree does not build - every other guard here is a grep and"
echo "     would still report green.  Compiler output:"
grep -E 'error|Error' "$log" | head -12 | sed 's/^/       /'
exit 1
