#!/bin/sh
# deploy-commit.sh - deploy_commit()'s guards on the two commit values it
# writes to /.flatpak-info: a truncated or non-hex checksum is worse than an
# absent key, because a consumer comparing it mismatches forever.
set -e

SRC="$(dirname "$0")/../src/daemon/ops/instance_ctl.c"
HARNESS="$(dirname "$0")/deploy-commit.c"

if ! command -v cc >/dev/null 2>&1; then
	echo "SKIP: no cc in PATH"
	exit 0
fi

line=$(grep -n '^static bool is_ostree_checksum(const char\* s)' "$SRC" | cut -d: -f1)
if [ -z "$line" ]; then
	echo "FAIL: is_ostree_checksum() not found in $SRC"
	exit 1
fi

tmp=$(mktemp -d /tmp/.fpkg-commit-test.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/tree"

# Carve from the is_ostree_checksum() signature to deploy_commit()'s closing
# brace - the third column-0 '}' - so the fragment is the three helpers and
# nothing else; the assertions below catch a wrong span.
awk -v s="$line" '
	NR >= s { print }
	NR > s && $0 == "}" { n++; if (n == 3) exit }
' "$SRC" > "$tmp/deploy_commit.inc"

if ! grep -q '^static void deploy_commit(const char\* files_dir' "$tmp/deploy_commit.inc"
then
	echo "FAIL: the extracted fragment does not contain deploy_commit();"
	echo "      the commit helpers are no longer adjacent"
	exit 1
fi
if grep -q '^instance_register(' "$tmp/deploy_commit.inc"; then
	echo "FAIL: the extracted fragment runs on past deploy_commit()"
	exit 1
fi

cc -O0 -Wall -Wextra -I"$tmp" -o "$tmp/deploy-commit" "$HARNESS"
"$tmp/deploy-commit" "$tmp/tree"
