#!/bin/sh
# memcheck.sh - valgrind over the protocol codec and over the daemon's
# workers, failing on any error or leak that is not a documented artifact of
# running valgrind on FreeBSD.
#
# protocol.sh already runs the codec's hostile-input harness under
# AddressSanitizer and UBSan.  valgrind adds what neither can: it follows a
# value from where it was created to where a branch or a write used it, so a
# read of memory nobody initialised is found with the line that left it
# uninitialised, and ASan cannot be combined with it in one binary.  The
# daemon half is the real thing: tests/typed-daemon.sh with the scratch daemon
# started under valgrind (FLATPKG_TD_VALGRIND), so every worker it forks for
# a request - run and enter on a terminal, install, kill, remote-add, a
# bundle - leaves a report of its own.
#
# What the first run found, on 2026-10-30, all fixed:
#   - snapshot_env() never wrote the terminator byte it had reserved, so every
#     run-environ file ended in a byte of uninitialised heap;
#   - fp_extensions_free() did not free sb->ldconfig_path;
#   - bundle_load() never released the superblock variant;
#   - plan_apply_locked() never freed the metadata of the app being added.
# The rest of what valgrind said is tests/memcheck-scan.py's KNOWN list, each
# entry with the evidence that it is the tool's and not flatpkg's.
#
# SKIPS without valgrind, cc, python3 or root (the daemon half needs it, the
# same way typed-daemon.sh does).  Slow: a few minutes, so it is a guard to
# run before a release rather than on every edit.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

for tool in valgrind cc python3; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "SKIP: no $tool in PATH"
		exit 0
	fi
done

tmp=$(mktemp -d /tmp/.fpkg-memcheck.XXXXXX)
trap 'rm -rf "$tmp"' EXIT INT TERM

fail=0
note() {
	echo "FAIL: $*"
	fail=1
}

# 1. The codec, built without a sanitizer (valgrind cannot share a binary
#    with one), through the same hostile-input harness protocol.sh uses.
mkdir -p "$tmp/vg-codec"
if ! cc -O1 -g -Iinclude -o "$tmp/protocol-test" tests/protocol-test.c \
	src/shared/protocol.c -lnv 2>"$tmp/cc.log"; then
	cat "$tmp/cc.log"
	note "the protocol harness did not build"
else
	valgrind --quiet --leak-check=full --show-leak-kinds=definite,indirect \
		--errors-for-leak-kinds=definite,indirect --track-origins=yes \
		--log-file="$tmp/vg-codec/%p.log" "$tmp/protocol-test" \
		>"$tmp/codec.out" 2>&1 || note "the protocol harness failed under valgrind: $(tail -n 3 "$tmp/codec.out")"
fi

# 1b. The metadata parser on a keyfile that repeats keys.  A remote's
#     metadata is untrusted, and a repeated key used to leak the first value
#     (seven blocks for this file before the parser released what it replaced).
codec_min=2
if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists glib-2.0; then
	# shellcheck disable=SC2046
	if ! cc -g -O0 -Iinclude -include sys/param.h $(pkg-config --cflags glib-2.0) \
		-o "$tmp/metadata-dup" tests/memcheck-metadata.c src/shared/metadata.c \
		src/shared/util.c \
		$(pkg-config --libs glib-2.0) 2>"$tmp/cc-meta.log"; then
		cat "$tmp/cc-meta.log"
		note "the metadata harness did not build"
	else
		codec_min=3
		valgrind --quiet --leak-check=full --show-leak-kinds=definite,indirect \
			--errors-for-leak-kinds=definite,indirect --track-origins=yes \
			--log-file="$tmp/vg-codec/%p.log" "$tmp/metadata-dup" \
			>"$tmp/meta.out" 2>&1 || note "the metadata harness failed under valgrind (status $?)"
	fi
else
	echo "NOTE: no pkg-config or glib-2.0; the metadata harness was not run"
fi

# 2. The daemon's workers.
mkdir -p "$tmp/vg-daemon"
FLATPKG_TD_VALGRIND="$tmp/vg-daemon" sh tests/typed-daemon.sh >"$tmp/td.out" 2>&1 && rc=0 || rc=$?
if grep -q '^SKIP' "$tmp/td.out"; then
	echo "NOTE: the daemon half was skipped: $(grep -m1 '^SKIP' "$tmp/td.out")"
	rm -rf "$tmp/vg-daemon"
	have_daemon=0
else
	have_daemon=1
	if [ "$rc" -ne 0 ] || ! grep -q '^PASS' "$tmp/td.out"; then
		sed 's/\x1b\[[0-9;]*m//g' "$tmp/td.out" | tail -n 15
		note "typed-daemon.sh did not pass with the daemon under valgrind"
	fi
fi

# 3. Read the reports.  The codec harness makes two (it forks), the metadata
#    harness one; the daemon and its workers make dozens.
if ! python3 tests/memcheck-scan.py "$codec_min" "$tmp/vg-codec"; then
	note "the codec reports have findings"
fi
if [ "$have_daemon" -eq 1 ]; then
	if ! python3 tests/memcheck-scan.py 50 "$tmp/vg-daemon"; then
		note "the daemon's reports have findings"
	fi
fi

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "PASS: valgrind found no error or leak in the protocol codec or in the"
echo "      daemon's workers beyond the documented artifacts of the tool"
