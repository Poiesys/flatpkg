#!/bin/sh
# home-persists.sh - for an app without home access, $HOME is its private dir:
# a file written there outlives the launch, and is not written to the host home.
# Before this, $HOME was an empty directory in the jail and Firefox, Zen and
# every app with a legacy ~/.<name> lost its profile at every restart.
#
# The subject is the caller's own session: the daemon takes the sandbox home
# from the passwd entry, not from HOME, so a pinned HOME would only test the
# harness.  Declared in worker-env.sh's exception list for that reason.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FLATPKG="$ROOT/build/flatpkg"
[ -x "$FLATPKG" ] || { echo "SKIP: build/flatpkg not built"; exit 0; }
id -Gn | tr ' ' '\n' | grep -qx flatpkg ||
	{ echo "SKIP: not in the flatpkg group; the root worker is the subject"; exit 0; }
[ -S /var/run/flatpkg/control.sock ] ||
	{ echo "SKIP: no control socket; the daemon is not running"; exit 0; }

# An app with home access makes $HOME the real home on purpose; this is about
# the ordinary case.
app=
for a in $("$FLATPKG" complete apps 2>/dev/null); do
	if "$FLATPKG" info "$a" 2>/dev/null |
	    grep -Eq '^[[:space:]]+filesystems[[:space:]]+(host|home)([[:space:]]|$)'; then
		continue
	fi
	app=$a
	break
done
[ -n "$app" ] || { echo "SKIP: every installed app has home access"; exit 0; }

probe=.flatpkg-home-probe
data="$HOME/.var/app/$app"
trap 'rm -f "$data/$probe" "$HOME/$probe"' EXIT
rm -f "$data/$probe" "$HOME/$probe"

fail=0
note() { echo "FAIL: $*"; fail=1; }

"$FLATPKG" run --command=/usr/bin/sh "$app" \
	-c 'echo probe >"$HOME/.flatpkg-home-probe"' >/dev/null 2>&1 || true

if [ ! -f "$data/$probe" ]; then
	note "a write to \$HOME did not reach $data/$probe"
	note "the app's home is not its private dir: its data does not persist"
fi
[ ! -f "$HOME/$probe" ] ||
	note "a write to \$HOME landed in the real home ($HOME/$probe)"

if [ "$fail" -eq 0 ]; then
	echo "PASS: \$HOME is the app's private dir; writes to it persist and do"
	echo "      not touch the real home"
fi
exit $fail
