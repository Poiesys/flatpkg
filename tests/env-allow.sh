#!/bin/sh
# env-allow.sh - the worker's allowlist of caller-supplied environment names
# is a security boundary, not a convenience list: the worker runs as root and
# replays these into a root process.  The exact contents are pinned here, so
# adding a name is a deliberate edit to this guard as well, and the names that
# carry caller identity are refused by name.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/src/daemon/ops/typed_worker.c"

[ -f "$SRC" ] || { echo "FAIL: $SRC is missing"; exit 1; }

T=$(mktemp -d /tmp/.fpkg-envallow.XXXXXX)
trap 'rm -rf "$T"' EXIT

fail() {
	echo "FAIL: $*"
	exit 1
}

# One name per line, in source order, quotes and commas stripped.
names_of() {
	awk '/^static const char\* const env_allow\[\] = \{/ { inlist = 1 }
	     inlist && /NULL\};/ { exit }
	     inlist {
		line = $0
		sub(/\/\/.*/, "", line)
		n = split(line, parts, "\"")
		for (i = 2; i <= n; i += 2)
			if (parts[i] != "")
				print parts[i]
	     }' "$1"
}

names_of "$SRC" > "$T/got"
[ -s "$T/got" ] || fail "cannot read the allowlist out of $SRC"

# Pinned: the caller's session, locale, terminal and device sockets.
cat > "$T/want" <<'PINNED'
XDG_RUNTIME_DIR
XDG_CURRENT_DESKTOP
DISPLAY
WAYLAND_DISPLAY
DBUS_SESSION_BUS_ADDRESS
DBUS_SYSTEM_BUS_ADDRESS
AT_SPI_BUS_ADDRESS
LC_ALL
LC_MESSAGES
LANG
TERM
COLORTERM
SSH_AUTH_SOCK
PULSE_SERVER
PULSE_RUNTIME_PATH
PULSE_CLIENTCONFIG
CUPS_SERVER
PCSCLITE_CSOCK_NAME
FLATPKG_NO_SHIMS
FLATPKG_OSTREE_VERBOSE
FLATPKG_FILECHOOSER_TIMEOUT
FLATPAK_GL_DRIVERS
PINNED

compare() {
	# $1 = file of names, $2 = description
	if ! diff -u "$T/want" "$1" > "$T/diff" 2>&1; then
		echo "$2:"
		sed -n '3,$p' "$T/diff" | sed 's/^/        /'
		return 1
	fi
	return 0
}

compare "$T/got" "the allowlist is not the pinned one" ||
	fail "a name was added to or removed from the worker's allowlist"

# The identity the worker derives from the peer must never be settable by the
# caller, and TZDIR is read by root-side code.
for bad in FLATPKG_APP_UID FLATPKG_APP_GID FLATPKG_APP_USER FLATPKG_APP_HOME \
	   TZDIR FLATPKG_DEBUG_KEEP; do
	grep -qx -- "$bad" "$T/got" &&
		fail "$bad is caller-settable"
done

# The comparison above has to be able to fail, so plant an addition and a
# removal in a copy of the list and require both to be reported.
{ cat "$T/got"; echo TZDIR; } > "$T/added"
grep -qx -- TZDIR "$T/added" || fail "the plant did not apply"
compare "$T/added" "plant" >/dev/null 2>&1 &&
	fail "an added name is not reported"

sed '/^TERM$/d' "$T/got" > "$T/removed"
cmp -s "$T/got" "$T/removed" && fail "the removal plant did not apply"
compare "$T/removed" "plant" >/dev/null 2>&1 &&
	fail "a removed name is not reported"

# A name may not be allowed without a validator deciding whether its value is
# acceptable for that name.
grep -q 'env_value_ok(eq + 1, uid, \*a)' "$SRC" ||
	fail "a name is allowed without a per-name validation"

echo "PASS: the worker's environment allowlist is pinned, and the names that"
echo "      carry caller identity are not in it"
