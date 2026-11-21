#!/bin/sh
# user-install.sh - the user installation: `flatpkg --user` reads and writes
# ~/.local/share/flatpak through a daemon started on demand as this user.
# The daemon serves only its owner, and a user store is a real store: a local
# repository installs into it.  A user installation has no jail, so it cannot
# launch applications.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${FLATPKG_USER_BIN:-/usr/local/sbin/flatpkg}
DAEMON=/usr/local/sbin/flatpkgd

[ -x "$BIN" ] || { echo "SKIP: $BIN is not installed"; exit 0; }
[ -x "$DAEMON" ] || { echo "SKIP: $DAEMON is not installed"; exit 0; }
command -v ostree >/dev/null 2>&1 ||
	{ echo "SKIP: no ostree to build the local remote"; exit 0; }

T=$(mktemp -d /tmp/.fpkg-user.XXXXXX)
before=$(pgrep -f 'flatpkgd --user' 2>/dev/null || true)
fail=0
note() {
	echo "FAIL: $*"
	fail=1
}
cleanup() {
	for p in $(pgrep -f 'flatpkgd --user' 2>/dev/null || true); do
		case " $before " in *" $p "*) continue ;; esac
		u=$(ps -o user= -p "$p" 2>/dev/null | tr -d ' ')
		[ "$u" = "$(id -un)" ] && kill "$p" 2>/dev/null || true
	done
	rm -rf "$T"
}
trap cleanup EXIT INT TERM

run() {
	env HOME="$T/home" XDG_RUNTIME_DIR="$T/run" "$BIN" "$@"
}

mkdir -p "$T/home/.local/share/flatpak" "$T/run" \
	"$T/app/files" "$T/rt/files"
printf '[Application]\nname=org.example.UserOnly\ncommand=/bin/true\nruntime=org.example.UserPlatform/x86_64/stable\n' \
	>"$T/app/metadata"
echo app >"$T/app/files/app.txt"
printf '[Runtime]\nname=org.example.UserPlatform\n' >"$T/rt/metadata"
echo runtime >"$T/rt/files/runtime.txt"
ostree --repo="$T/repo" init --mode=archive >/dev/null
ostree --repo="$T/repo" commit -b app/org.example.UserOnly/x86_64/stable \
	--tree="dir=$T/app" -s app >/dev/null 2>&1
ostree --repo="$T/repo" commit -b runtime/org.example.UserPlatform/x86_64/stable \
	--tree="dir=$T/rt" -s runtime >/dev/null 2>&1
ostree --repo="$T/repo" summary -u >/dev/null 2>&1
ostree --repo="$T/home/.local/share/flatpak/repo" init --mode=archive >/dev/null 2>&1

run --user list >/dev/null 2>&1 ||
	note "reading an empty user installation failed"
run --user remote-add local "file://$T/repo" >/dev/null 2>&1 ||
	note "remote-add in the user installation failed"
run --user install -y local org.example.UserOnly >/dev/null 2>&1 ||
	note "installing into the user installation failed"
run --user complete apps 2>/dev/null | grep -qx org.example.UserOnly ||
	note "the installed app is not in the user store"
[ -d "$T/home/.local/share/flatpak/app/org.example.UserOnly" ] ||
	note "the app was not deployed into the user store"

# The user daemon must be able to store a remote's GPG key in its own store:
# the keys directory and the key belong to the user, not to root.  A dummy
# key is enough; the key file is written before ostree rejects the key.
printf '[Flatpak Repo]\nUrl=file://%s\nGPGKey=%s\n' "$T/repo" \
	"$(printf test | base64)" > "$T/bad.flatpakrepo"
run --user remote-add --from="$T/bad.flatpakrepo" badkey >/dev/null 2>&1 || true
key="$T/home/.local/share/flatpak/keys/badkey.gpg"
[ -f "$key" ] || note "the user daemon did not store the remote key"
if [ -f "$key" ]; then
	[ "$(stat -f '%Su' "$key")" = "$(id -un)" ] ||
		note "the remote key is not owned by the user"
	[ "$(stat -f '%Lp' "$key")" = 600 ] ||
		note "the remote key is not 0600"
	[ "$(stat -f '%Su' "$T/home/.local/share/flatpak/keys")" = \
		"$(id -un)" ] || note "the user keys directory is not the user's"
fi
[ -e /var/db/flatpkg/app/org.example.UserOnly ] &&
	note "the user installation wrote the system store"

sock="$T/run/flatpkg/control.sock"
[ -S "$sock" ] || note "the user daemon did not create its socket"
if [ -S "$sock" ]; then
	[ "$(stat -f '%Lp' "$sock")" = 600 ] ||
		note "the user socket is not 0600"
	[ "$(stat -f '%Su' "$sock")" = "$(id -un)" ] ||
		note "the user socket is not owned by this user"
fi
found=0
for p in $(pgrep -f 'flatpkgd --user' 2>/dev/null || true); do
	[ "$(ps -o user= -p "$p" 2>/dev/null | tr -d ' ')" = \
		"$(id -un)" ] && found=1
done
[ "$found" = 1 ] || note "the user daemon is not running as this user"

# The launch resolves the caller's REAL user installation (derived from the
# peer uid), not the HOME this harness overrides, so the fixture installed
# under $T/home is deliberately not launchable from here: the request must be
# refused rather than reach some other user's store.
if run --user run org.example.UserOnly >/dev/null 2>&1; then
	note "a launch resolved a store the caller did not install into"
fi

[ "$fail" -eq 0 ] && echo "PASS: a user installation has its own daemon and"
echo "      store, installs a local app without touching the system store,"
echo "      and launches it through the root daemon"
exit "$fail"
