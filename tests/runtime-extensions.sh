#!/bin/sh
# runtime-extensions.sh - a runtime's own [Extension] groups are planned with
# the runtime, and are healed when its extensions are gone but the runtime
# stays.  GeForce NOW ships an app whose runtime (24.08) was already installed
# without the GL extension that belongs to it: the app then had no Mesa and
# every EGL lookup failed ("No usable GPUs are found").
#
# The remote is a repository this guard builds under /tmp.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tests/scratchd.sh"

command -v ostree >/dev/null 2>&1 ||
	{ echo "SKIP: no ostree to build the remote"; exit 0; }

T=$(mktemp -d /tmp/.fpkg-rext.XXXXXX)
tmp=$T
trap 'scratchd_stop; rm -rf "$tmp"' EXIT INT TERM

fail=0
note() {
	echo "FAIL: $*"
	fail=1
}

# The runtime declares one extension group; the extension is a runtime ref of
# its own, which is how flatpak publishes GL, Locale and friends.
mkdir -p "$T/app/files" "$T/rt/files" "$T/ext/files"
printf '[Application]\nname=org.example.App\ncommand=/bin/true\nruntime=org.example.Platform/x86_64/stable\n' \
	>"$T/app/metadata"
echo app >"$T/app/files/app.txt"
printf '[Runtime]\nname=org.example.Platform\n\n[Extension org.example.GL]\ndirectory=GL\n' \
	>"$T/rt/metadata"
echo runtime >"$T/rt/files/runtime.txt"
printf '[Runtime]\nname=org.example.GL\n' >"$T/ext/metadata"
echo gl >"$T/ext/files/gl.txt"
ostree --repo="$T/repo" init --mode=archive >/dev/null
ostree --repo="$T/repo" commit -b app/org.example.App/x86_64/stable \
	--tree="dir=$T/app" -s app >/dev/null 2>&1
ostree --repo="$T/repo" commit -b runtime/org.example.Platform/x86_64/stable \
	--tree="dir=$T/rt" -s runtime >/dev/null 2>&1
ostree --repo="$T/repo" commit -b runtime/org.example.GL/x86_64/stable \
	--tree="dir=$T/ext" -s gl >/dev/null 2>&1
ostree --repo="$T/repo" summary -u >/dev/null

scratchd_start
FLATPKG_STORE=$SD_STORE
export FLATPKG_STORE

run() {
	"$SD_BIN" "$@" </dev/null
}

ext="$SD_STORE/extension/org.example.GL/x86_64/stable"

run remote-add --if-not-exists local "file://$T/repo" >/dev/null 2>&1 ||
	note "remote-add of a local repository failed"
run install -y local org.example.App >/dev/null 2>&1 ||
	note "install from the local remote failed"
[ -d "$ext" ] ||
	note "the runtime's extension was never installed"

# Now the runtime stays and the extension is gone: a plain install of the app
# has to notice, because nothing else plans a runtime the store already has.
mdo rm -rf "$SD_STORE/extension/org.example.GL" >/dev/null 2>&1 || true
[ -d "$ext" ] && note "the guard could not remove the extension deploy"
run install -y local org.example.App >/dev/null 2>&1 || true
[ -d "$ext" ] ||
	note "installing the app did not heal the missing runtime extension"

[ "$fail" -eq 0 ] &&
	echo "PASS: a runtime's extensions are installed with it, and an install"
echo "      of an app heals them when only the runtime is still there"
exit "$fail"
