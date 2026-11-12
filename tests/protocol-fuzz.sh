#!/bin/sh
# protocol-fuzz.sh - libFuzzer, with ASan, against the framed protocol
# decoder.  protocol.sh asserts known shapes; this one looks for the shape
# nobody wrote down.  SKIPS when the compiler has no libFuzzer runtime.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

CC_BIN=${CC:-cc}

command -v "$CC_BIN" >/dev/null 2>&1 || {
	echo "SKIP: no C compiler in PATH"
	exit 0
}

tmp=$(mktemp -d /tmp/.fpkg-protofuzz.XXXXXX)
trap 'rm -rf "$tmp"' EXIT INT TERM

# The runtime probe: a compiler may accept -fsanitize=fuzzer and fail to link.
probe="$tmp/probe"
printf 'int LLVMFuzzerTestOneInput(const unsigned char* d, unsigned long n){(void)d;(void)n;return 0;}\n' \
	> "$tmp/probe.c"
if ! "$CC_BIN" -fsanitize=fuzzer,address -o "$probe" "$tmp/probe.c" \
	>"$tmp/probe.log" 2>&1; then
	echo "SKIP: no libFuzzer + ASan runtime on this compiler"
	exit 0
fi

# Seeds: valid frames the mutator can start from.
"$CC_BIN" -O1 -g -DFPKG_FUZZ_SEEDS -Iinclude -o "$tmp/seeds" \
	tests/protocol-fuzz.c src/shared/protocol.c -lnv ||
	{ echo "FAIL: the seed generator did not build"; exit 1; }
mkdir -p "$tmp/corpus"
"$tmp/seeds" "$tmp/corpus" || { echo "FAIL: the seed generator failed"; exit 1; }
nseeds=$(find "$tmp/corpus" -type f | wc -l | tr -d ' ')
[ "$nseeds" -ge 3 ] || { echo "FAIL: only $nseeds seed frame(s)"; exit 1; }

if ! "$CC_BIN" -O1 -g -fsanitize=fuzzer,address -Iinclude \
	-o "$tmp/fuzz" tests/protocol-fuzz.c src/shared/protocol.c -lnv \
	>"$tmp/build.log" 2>&1; then
	echo "FAIL: the fuzzer did not build:"
	tail -n 10 "$tmp/build.log" | sed 's/^/       /'
	exit 1
fi

# 20 seconds is the guard's budget; the artifact, if any, names the input.
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
run="$tmp/fuzz"
set +e
"$run" -max_total_time=20 -rss_limit_mb=512 -timeout=10 \
	-artifact_prefix="$tmp/" "$tmp/corpus" >"$tmp/fuzz.out" 2>&1
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
	echo "FAIL: the protocol decoder fuzzer found a case (exit $rc):"
	grep -E "ERROR|SUMMARY|crash|artifact|deadly" "$tmp/fuzz.out" |
		head -8 | sed 's/^/       /'
	exit 1
fi

execs=$(grep -oE '^#[0-9]+.*DONE' "$tmp/fuzz.out" | wc -l | tr -d ' ')
echo "PASS: 20s of libFuzzer over the protocol decoder, $nseeds seeds, no crash"
