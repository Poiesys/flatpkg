#!/bin/sh
# remote-info-modify.sh - the two remote commands a third-party app's update
# path uses: `remote-modify` changes a remote's settings, and `remote-info`
# reports the ref the remote advertises, with flatpak's `-c` and `-r` machine
# forms.  `info` then answers the same two questions about a deploy, which is
# what `flatpak info --show-origin/--show-commit` is for.
#
# The remote is a repository this guard builds under /tmp, so nothing here
# reaches the network or the live store.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tests/scratchd.sh"

command -v ostree >/dev/null 2>&1 ||
	{ echo "SKIP: no ostree to build the remote"; exit 0; }

T=$(mktemp -d /tmp/.fpkg-rmtinfo.XXXXXX)
tmp=$T
trap 'scratchd_stop; rm -rf "$tmp"' EXIT INT TERM

fail=0
note() {
	echo "FAIL: $*"
	fail=1
}

# One app and the runtime it declares, in a repository the guard owns.
mkdir -p "$T/app/files" "$T/rt/files"
printf '[Application]\nname=org.example.App\ncommand=/bin/true\nruntime=org.example.Platform/x86_64/stable\n' \
	>"$T/app/metadata"
echo hello >"$T/app/files/hello.txt"
printf '[Runtime]\nname=org.example.Platform\n' >"$T/rt/metadata"
echo runtime >"$T/rt/files/runtime.txt"
ostree --repo="$T/repo" init --mode=archive >/dev/null
ostree --repo="$T/repo" commit -b app/org.example.App/x86_64/stable \
	--tree="dir=$T/app" -s app >/dev/null 2>&1
ostree --repo="$T/repo" commit -b runtime/org.example.Platform/x86_64/stable \
	--tree="dir=$T/rt" -s runtime >/dev/null 2>&1
ostree --repo="$T/repo" summary -u >/dev/null
commit=$(ostree --repo="$T/repo" rev-parse app/org.example.App/x86_64/stable)

scratchd_start
FLATPKG_STORE=$SD_STORE
export FLATPKG_STORE

run() {
	"$SD_BIN" "$@" </dev/null
}

url="file://$T/repo"
run remote-add --if-not-exists local "$url" >/dev/null 2>&1 ||
	note "remote-add of a local repository failed"

# remote-modify changes the settings remote-list reports.
run remote-modify --default-branch=stable local >/dev/null 2>&1 || true
run remote-list >"$T/list" 2>/dev/null || true
grep -q 'stable' "$T/list" ||
	note "remote-modify --default-branch did not reach remote-list"

run remote-modify --url="$url" local >/dev/null 2>&1 ||
	note "remote-modify --url failed on the url it already had"

run remote-modify --no-gpg-verify local >/dev/null 2>&1 || true
run remote-list >"$T/list" 2>/dev/null || true
grep -Eq 'local +[^ ]+ +false +false' "$T/list" ||
	note "remote-modify --no-gpg-verify did not clear the flags"
run remote-modify --gpg-verify local >/dev/null 2>&1 || true
run remote-list >"$T/list" 2>/dev/null || true
grep -Eq 'local +[^ ]+ +true +true' "$T/list" ||
	note "remote-modify --gpg-verify did not set the flags"

# A modify that does not mention the GPG flags must leave them alone: a
# zeroed request once meant "switch verification off", so every
# `remote-modify --url` (and the app's own key import) silently did.
run remote-modify --gpg-verify local >/dev/null 2>&1 || true
run remote-modify --url="$url" local >/dev/null 2>&1 || true
run remote-list >"$T/list" 2>/dev/null || true
grep -Eq 'local +[^ ]+ +true +true' "$T/list" ||
	note "a modify that does not name the GPG flags turned them off"

# A remote that does not exist is a refusal, not a new remote.
if run remote-modify --url="$url" not-there >/dev/null 2>&1; then
	note "remote-modify created a remote that did not exist"
fi

# remote-info answers with the remote's own ref and commit.
got=$(run remote-info --show-commit local org.example.App 2>/dev/null | tail -1)
[ "$got" = "$commit" ] ||
	note "remote-info --show-commit gave '$got', wanted $commit"
got=$(run remote-info --show-ref local org.example.App 2>/dev/null | tail -1)
[ "$got" = "app/org.example.App/x86_64/stable" ] ||
	note "remote-info --show-ref gave '$got'"
run remote-info --show-metadata local org.example.App >"$T/meta" 2>/dev/null ||
	note "remote-info --show-metadata failed"
grep -q 'runtime=org.example.Platform/x86_64/stable' "$T/meta" ||
	note "remote-info --show-metadata did not print the metadata"

# The fixture repository is unsigned, so verification must be off to install.
run remote-modify --no-gpg-verify local >/dev/null 2>&1 || true

# Install it, then info answers the same questions about the deploy.
run install -y local org.example.App >/dev/null 2>&1 ||
	note "install from the local remote failed"
if run list 2>/dev/null | grep -q org.example.App; then
	got=$(run info --show-origin org.example.App 2>/dev/null | tail -1)
	[ "$got" = "local" ] ||
		note "info --show-origin gave '$got', wanted local"
	got=$(run info --show-commit org.example.App 2>/dev/null | tail -1)
	[ "$got" = "$commit" ] ||
		note "info --show-commit gave '$got', wanted $commit"
	got=$(run info --show-ref org.example.App 2>/dev/null | tail -1)
	[ "$got" = "app/org.example.App/x86_64/stable" ] ||
		note "info --show-ref gave '$got'"
	# flatpak's friendly block, which is what a client greps.
	run info org.example.App >"$T/info" 2>/dev/null || true
	grep -q '^ *Ref: app/org.example.App/x86_64/stable' "$T/info" ||
		note "info did not print flatpak's Ref field"
	grep -q "^ *Commit: $commit" "$T/info" ||
		note "info did not print flatpak's Commit field"
else
	note "the deploy is not listed after install"
fi

[ "$fail" -eq 0 ] &&
	echo "PASS: remote-modify changes a remote, remote-info reports it, and"
echo "      info reports the same ref, origin and commit of the deploy"
exit "$fail"
