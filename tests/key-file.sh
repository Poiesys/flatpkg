#!/bin/sh
# key-file.sh - a key file is the trust anchor for every pull, and the names it
# is written through live in directories a group member may be able to write:
# the writer opens the directory, never follows a symlink in place of the key,
# replaces rather than truncates it, and creates it 0600.  The keys directory
# itself is root's and 0755.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/src/shared/util.c"
CMDS="$ROOT/src/ostree_cmds.c"

[ -f "$SRC" ] && [ -f "$CMDS" ] ||
	{ echo "FAIL: sources are missing"; exit 1; }
command -v cc >/dev/null 2>&1 || { echo "SKIP: no cc in PATH"; exit 0; }
. "$ROOT/tests/commentlib.sh"

T=$(mktemp -d /tmp/.fpkg-keyfile.XXXXXX)
trap 'rm -rf "$T"' EXIT

build() {
	cc -O1 -Wall -Wextra -std=gnu11 -I"$ROOT/include" -o "$2" \
		"$ROOT/tests/key-file.c" "$1" \
		"$ROOT/src/cli/tb_core.c" "$ROOT/src/cli/tb_progress.c" \
		"$ROOT/src/cli/tb_tui.c" -pthread >"$T/cc.log" 2>&1
}

try() {
	if ! build "$1" "$T/h"; then
		echo "FAIL: the file-writer harness does not build:"
		head -8 "$T/cc.log" | sed 's/^/       /'
		return 1
	fi
	"$T/h"
}

try "$SRC"

# --- the plants: each rule must fire on the tree it was written for --------
prove() {
	what=$1
	shift
	if ( "$@" ) >/dev/null 2>&1; then
		echo "FAIL: the guard passes with $what"
		exit 1
	fi
}

# 1. a key created 0644
sed 's/(void)fchmod(fd, 0600);/(void)fchmod(fd, 0644);/' "$SRC" \
	> "$T/plant-mode.c"
if cmp -s "$SRC" "$T/plant-mode.c"; then
	echo "FAIL: the mode plant was not applied"
	exit 1
fi
prove "a key file created 0644" try "$T/plant-mode.c"

# 2. a write that follows a symlink
sed 's/O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC/O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC/' \
	"$SRC" > "$T/plant-follow.c"
if cmp -s "$SRC" "$T/plant-follow.c"; then
	echo "FAIL: the symlink plant was not applied"
	exit 1
fi
prove "a write that follows a symlink" try "$T/plant-follow.c"

# --- the source half: the writers and the keys directory -------------------
strip_comments "$SRC" > "$T/util.c"
strip_comments "$CMDS" > "$T/cmds.c"
carve_function '^fpkg_error_t write_secret_file_at\(' "$T/util.c" \
	> "$T/secret.inc" ||
	{ echo "FAIL: write_secret_file_at() not found"; exit 1; }
carve_function '^static int write_key_file\(' "$T/cmds.c" > "$T/key.inc" ||
	{ echo "FAIL: write_key_file() not found"; exit 1; }

bug() {
	echo "FAIL: $1"
	exit 1
}

check() {
	key=$1
	secret=$2

	grep -q 'open(keysdir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW' "$key" ||
		bug "the keys directory is opened by path"
	grep -q 'write_secret_file_at(dfd' "$key" ||
		bug "the key is not written through the opened directory"
	grep -q 'fchmod(dfd, 0755)' "$key" ||
		bug "the keys directory is not made 0755"
	grep -q 'fchown(dfd, 0, 0)' "$key" ||
		bug "the keys directory is not made root's"
	grep -q 'st.st_uid != 0' "$key" ||
		bug "the keys directory's owner is not checked"

	grep -q 'O_CREAT | O_EXCL | O_NOFOLLOW' "$secret" ||
		bug "a key is created without O_EXCL and O_NOFOLLOW"
	grep -q 'unlinkat(dirfd, name, 0)' "$secret" ||
		bug "an old key is not unlinked, so a symlink could still stand"
	grep -q 'fchmod(fd, 0600)' "$secret" ||
		bug "a key file is not 0600"
}

( check "$T/key.inc" "$T/secret.inc" )

# The key writer must not fall back to the generic path writer.
sed 's/write_secret_file_at(dfd, fname, armored)/write_file_at(dfd, fname, armored, 0644)/' \
	"$T/key.inc" > "$T/plant-key.inc"
if cmp -s "$T/key.inc" "$T/plant-key.inc"; then
	echo "FAIL: the key-writer plant was not applied"
	exit 1
fi
prove "a key written by the generic writer" check "$T/plant-key.inc" \
	"$T/secret.inc"

# O_EXCL is what makes the create exhaustive rather than in place.
sed 's/O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC/O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC/' \
	"$T/secret.inc" > "$T/plant-excl.inc"
if cmp -s "$T/secret.inc" "$T/plant-excl.inc"; then
	echo "FAIL: the O_EXCL plant was not applied"
	exit 1
fi
prove "a key created without O_EXCL" check "$T/key.inc" "$T/plant-excl.inc"

echo "PASS: a key is written inside the directory it was judged in, created"
echo "      0600 and never through a name a caller can swap"
