#!/bin/sh
# daemon-required.sh - a command that needs flatpkgd does not run without it:
# it exits 69 and names the missing socket, while help, --version, usage errors
# and the read-only commands still run locally.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if ! command -v cc >/dev/null 2>&1 || ! command -v make >/dev/null 2>&1; then
	echo "SKIP: no cc or make in PATH"
	exit 0
fi

tmp=$(mktemp -d /tmp/.fpkg-daemon-required.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

sockdir="$tmp/no-such-dir"
bin="$tmp/build/flatpkg"
fail=0
note() {
	echo "FAIL: $*"
	fail=1
}

jobs=$(sysctl -n hw.ncpu 2>/dev/null || echo 2)
if ! (cd "$ROOT" && make -j"$jobs" BUILD="$tmp/build" \
	SOCKET_PATH="$sockdir/control.sock" "$bin") > "$tmp/make.log" 2>&1; then
	echo "FAIL: the scratch client did not build:"
	tail -n 15 "$tmp/make.log"
	exit 1
fi

# run ARGS...: sets rc, with stdout in $tmp/out and stderr in $tmp/err.
run() {
	rc=0
	"$bin" "$@" > "$tmp/out" 2> "$tmp/err" < /dev/null || rc=$?
}

# --- 1. what needs the daemon refuses without it ---------------------------
for cmd in "upgrade --simulate" "install org.example.App" \
	"uninstall org.example.App" "update" "remote-add foo https://example.com" \
	"override --system org.example.App --filesystem=host" \
	"remote-delete foo" "rollback org.example.App abc123" "exports"; do
	# $cmd is deliberately split into words.
	run $cmd
	if [ "$rc" -ne 69 ]; then
		note "'flatpkg $cmd' without the daemon exited $rc, not 69"
		continue
	fi
	if [ -s "$tmp/out" ]; then
		note "'flatpkg $cmd' printed to stdout without the daemon - it ran"
		echo "      in this process instead of asking flatpkgd"
	fi
	if ! grep -q 'flatpkgd is not running' "$tmp/err"; then
		note "'flatpkg $cmd' failed without saying flatpkgd is not running"
	fi
	if ! grep -q 'no control socket at' "$tmp/err"; then
		note "'flatpkg $cmd' did not say which socket was missing"
	fi
	if ! grep -q 'service flatpkgd start' "$tmp/err"; then
		note "'flatpkg $cmd' gave no hint how to start the daemon"
	fi
done

# --- 2. what needs no daemon still works -----------------------------------
for cmd in "--version" "--help" "help" "list --help" "complete commands"; do
	run $cmd
	if [ "$rc" -ne 0 ]; then
		note "'flatpkg $cmd' needs no daemon but exited $rc"
	fi
	if grep -q 'flatpkgd' "$tmp/err"; then
		note "'flatpkg $cmd' mentioned flatpkgd; it needs no daemon"
	fi
	if [ ! -s "$tmp/out" ]; then
		note "'flatpkg $cmd' printed nothing"
	fi
done
run complete commands
if ! grep -q '^install$' "$tmp/out"; then
	note "'complete commands' did not list the commands"
fi

# `complete` is read-only: the CLI reads the group-readable store itself, as the
# caller, so every kind works without a daemon.  `complete bogus` is the
# command's usage error, not "daemon unavailable".
for kind in apps refs runtimes world remotes instances; do
	run complete "$kind"
	if [ "$rc" -ne 0 ]; then
		note "'complete $kind' needs no daemon but exited $rc"
	fi
	if grep -q 'flatpkgd' "$tmp/err"; then
		note "'complete $kind' mentioned flatpkgd; it reads the store itself"
	fi
done
run complete bogus
if [ "$rc" -eq 69 ]; then
	note "'complete bogus' reported the daemon unavailable; it is a usage error"
fi

# A usage error is the parser's, not the daemon's: its own status.
run list --bogus
if [ "$rc" -eq 0 ] || [ "$rc" -eq 69 ]; then
	note "'list --bogus' exited $rc; a usage error is neither 0 nor 69"
fi
if ! grep -q "unknown option" "$tmp/err"; then
	note "'list --bogus' did not report the unknown option"
fi

# --- 3. there is no switch for running a privileged command in-process -------
# FLATPKG_DAEMON=0 no longer exists, so setting it must change nothing: the
# command still exits 69 and prints nothing to stdout (so it did not run).
for cmd in "upgrade --simulate" "install org.example.App" "exports"; do
	rc=0
	# $cmd is deliberately split into words.
	FLATPKG_DAEMON=0 "$bin" $cmd > "$tmp/out" 2> "$tmp/err" < /dev/null || rc=$?
	if [ "$rc" -ne 69 ]; then
		note "FLATPKG_DAEMON=0 'flatpkg $cmd' exited $rc, not 69: the variable still does something"
	fi
	if [ -s "$tmp/out" ]; then
		note "FLATPKG_DAEMON=0 'flatpkg $cmd' printed to stdout: it ran in-process"
	fi
done

# --- 4. a stale socket (the file is there, nobody listens) -------------------
cat > "$tmp/stale.c" <<'CEOF'
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
int main(int argc, char** argv) {
	struct sockaddr_un sa;
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || argc < 2) return 1;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (strlcpy(sa.sun_path, argv[1], sizeof(sa.sun_path)) >= sizeof(sa.sun_path))
		return 1;
	if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) return 1;
	close(fd); /* the file stays; nothing listens */
	return 0;
}
CEOF
mkdir "$sockdir"
cc -o "$tmp/stale" "$tmp/stale.c"
"$tmp/stale" "$sockdir/control.sock"
if [ ! -S "$sockdir/control.sock" ]; then
	note "could not make a stale socket for the test"
else
	run upgrade --simulate
	if [ "$rc" -ne 69 ]; then
		note "a stale socket: 'flatpkg upgrade --simulate' exited $rc, not 69"
	fi
	if ! grep -q 'not accepting' "$tmp/err"; then
		note "a stale socket was not reported as one"
	fi
fi

if [ "$fail" -ne 0 ]; then
	echo
	echo "FAILED: a command that needs flatpkgd ran, or failed wrongly, without it"
	exit 1
fi
echo "PASS: without flatpkgd, 10 commands that need it exit 69 and name the socket"
echo "      (and print nothing, so none ran in-process); help, --version,"
echo "      'list --help' and 'complete commands' still work; a usage error keeps"
echo "      its own status; a stale socket is reported as stale; and"
echo "      FLATPKG_DAEMON=0 changes nothing - there is no direct path."
