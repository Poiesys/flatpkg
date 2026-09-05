#!/bin/sh
# shimtest.sh - shared harness for the LD_PRELOAD shim guards.  Sourced, never run:
# `shim_require_fresh` fails on a stale shim, `shim_build <cfile>` skips without the
# toolchain, `shim_run` loads it and `shim_run_plain` is the control.

SHIMTEST_ROOT=${SHIMTEST_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}
SHIMTEST_SHIM=$SHIMTEST_ROOT/build/seccomp_shim.so

shim_require_fresh() {
	[ -f "$SHIMTEST_SHIM" ] || return 0   # nothing to be stale about
	for dep in "$SHIMTEST_ROOT/src/seccomp_shim.c" \
	           "$SHIMTEST_ROOT/src/seccomp_shim.map"
	do
		if [ "$dep" -nt "$SHIMTEST_SHIM" ]; then
			echo "FAIL: $(basename "$SHIMTEST_SHIM") is older than" \
			     "$(basename "$dep") - the shim was not rebuilt, so" \
			     "this guard would test the previous binary.  Run make."
			return 1
		fi
	done
	return 0
}

shim_build() {
	src=$1
	out=$2
	zig cc --target=x86_64-linux-gnu -O2 -o "$out" "$src" \
	    2>/dev/null || return 1
	printf '%s' "$out"
}

shim_run() {
	bin=$1
	shift
	env LD_PRELOAD="$SHIMTEST_SHIM" "$@" "$bin" 2>/dev/null || true
}

shim_run_plain() {
	"$@" 2>/dev/null || true
}
