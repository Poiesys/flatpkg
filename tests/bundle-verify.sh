#!/bin/sh
# bundle-verify.sh - a bundle's commit is verified against the keyring of the
# remote it claims to come from, not against the repo-wide keyring.  The
# repo-wide one holds every remote's keys, so one remote's signature would
# vouch for another remote's content; and the bundle's own origin is what
# names the remote, so it is exactly the thing that must not be trusted
# loosely.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
REPO="$ROOT/src/daemon/repo.c"
CMDS="$ROOT/src/ostree_cmds.c"

[ -f "$REPO" ] || { echo "FAIL: $REPO is missing"; exit 1; }
[ -f "$CMDS" ] || { echo "FAIL: $CMDS is missing"; exit 1; }

T=$(mktemp -d /tmp/.fpkg-bundle.XXXXXX)
trap 'rm -rf "$T"' EXIT

. "$ROOT/tests/commentlib.sh"
strip_comments "$REPO" > "$T/repo.c"
strip_comments "$CMDS" > "$T/cmds.c"

fail() {
	echo "FAIL: $*"
	exit 1
}

carve_function '^int fp_repo_verify_commit\(' "$T/repo.c" > "$T/verify.inc" ||
	fail "cannot carve fp_repo_verify_commit() out of $REPO"

check() {
	f=$1
	grep -q 'ostree_repo_verify_commit_for_remote(repo, checksum,' "$f" ||
		{ echo "verification is not for a named remote"; return 1; }
	grep -q 'remote, NULL,' "$f" ||
		{ echo "the remote argument is not passed through"; return 1; }
	return 0
}

check "$T/verify.inc" ||
	fail "a bundle's commit is not verified against its remote's keyring"

# The bundle path has to name that remote: the check is only as good as the
# argument, and the argument is what the bundle claims.
grep -q 'fp_repo_verify_commit(commit, useremote,' "$T/cmds.c" ||
	fail "the bundle path does not verify against the remote it recorded"

# And the bundle's own file name must not pick the remote: that would let a
# file called "flathub" write into the real one.
grep -q 'bundle-local' "$T/cmds.c" ||
	fail "an unmatched origin no longer falls back to a reserved name"
# The remote comes from the bundle's declared origin, never from its path:
# a file named after a remote would otherwise select that remote.
grep -q 'bundle_find_remote(origin,' "$T/cmds.c" ||
	fail "the remote is not looked up from the bundle's declared origin"

# Both directions: a verification that takes the repo-wide keyring passes the
# text check above only because of the else branch, so prove the plant fails.
sed 's/remote, NULL,/NULL, NULL,/' "$T/verify.inc" > "$T/verify-plant.inc"
cmp -s "$T/verify.inc" "$T/verify-plant.inc" &&
	fail "the plant was not applied"
check "$T/verify-plant.inc" >/dev/null 2>&1 &&
	fail "the guard passes when the remote is dropped from the check"

echo "PASS: a bundle's commit is verified against the keyring of the remote"
echo "      it claims, and an unmatched origin stays on a reserved name"
