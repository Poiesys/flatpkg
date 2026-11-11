#!/bin/sh
# portal-identity.sh - the portal and the session helper are D-Bus activated,
# so their environment is the activation environment: the instance base they
# read identity from is derived from the uid, never from XDG_RUNTIME_DIR; the
# proxy path they trust is a file the caller could not have written; and a
# HostCommand is only served for this uid and for a jail a flatpkg launch
# claims.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

. "$ROOT/tests/commentlib.sh"

T=$(mktemp -d /tmp/.fpkg-pident.XXXXXX)
trap 'rm -rf "$T"' EXIT

strip_comments "$ROOT/src/shared/instance.c" > "$T/instance.c"
strip_comments "$ROOT/src/cli/portal.c" > "$T/portal.c"
strip_comments "$ROOT/src/shared/dbus.c" > "$T/dbus.c"
strip_comments "$ROOT/src/cli/sessionhelper.c" > "$T/helper.c"

carve_function '^static bool runtime_dir_is_ours\(' "$T/instance.c" \
	> "$T/trust.inc" || { echo "FAIL: runtime_dir_is_ours() not found"; exit 1; }
carve_function '^const char\* session_instances_dir\(' "$T/instance.c" \
	> "$T/base.inc" || { echo "FAIL: session_instances_dir() not found"; exit 1; }
carve_function '^static void instance_base\(' "$T/portal.c" \
	> "$T/ibase.inc" || { echo "FAIL: instance_base() not found"; exit 1; }
carve_function '^static bool proxy_trusted\(' "$T/dbus.c" \
	> "$T/proxy.inc" || { echo "FAIL: proxy_trusted() not found"; exit 1; }
carve_function '^bool dbus_proxy_is_path\(' "$T/dbus.c" \
	> "$T/proxypath.inc" || { echo "FAIL: dbus_proxy_is_path() not found"; exit 1; }
carve_function '^static void handle_host_command\(' "$T/helper.c" \
	> "$T/hostcmd.inc" || { echo "FAIL: handle_host_command() not found"; exit 1; }

bug() {
	echo "FAIL: $1"
	exit 1
}

check() {
	trust=$1 base=$2 ibase=$3 proxy=$4 proxypath=$5 hostcmd=$6

	grep -q 'st.st_uid != uid' "$trust" ||
		bug "the runtime directory is not required to be the uid's"
	grep -q '/var/run/user/%u' "$trust" ||
		bug "the uid's own runtime dir is not a recognised shape"
	grep -q '/var/run/xdg/%s' "$trust" ||
		bug "the per-user xdg runtime dir is not a recognised shape"
	grep -q 'runtime_dir_is_ours(xdg)' "$base" ||
		bug "the activation environment is believed"

	if grep -q 'cli_getenv("XDG_RUNTIME_DIR")' "$ibase"; then
		bug "the portal reads the instance base out of the environment"
	fi
	grep -q 'session_instances_dir()' "$ibase" ||
		bug "the portal does not use the uid-derived instance base"

	grep -q 'st.st_uid == 0' "$proxy" ||
		bug "a caller-writable file can be the dbus proxy"
	grep -q 'proxy_trusted(path)' "$proxypath" ||
		bug "the proxy check trusts a path it did not vet"
	grep -q 'proxy_trusted(proxy_candidates\[i\])' "$T/dbus.c" ||
		bug "the fallback proxy path is not vetted"

	grep -q 'helper_caller_uid' "$hostcmd" ||
		bug "HostCommand does not ask the bus for the caller's uid"
	grep -q 'caller_uid != getuid()' "$hostcmd" ||
		bug "HostCommand serves another uid"
	grep -q 'instance_app_for_jid(jid)' "$hostcmd" ||
		bug "HostCommand serves a jail no flatpkg launch claims"
}

( check "$T/trust.inc" "$T/base.inc" "$T/ibase.inc" "$T/proxy.inc" \
	"$T/proxypath.inc" "$T/hostcmd.inc" )

# --- the plants ----------------------------------------------------------
prove() {
	what=$1
	shift
	if ( "$@" ) >/dev/null 2>&1; then
		echo "FAIL: the guard passes with $what"
		exit 1
	fi
}

# 1. the portal back on the activation environment
cat > "$T/plant-ibase.inc" <<'INC'
static void instance_base(char* out, size_t n) {
	const char* env = cli_getenv("XDG_RUNTIME_DIR");
	if (env && env[0] == '/') {
		snprintf(out, n, "%s/.flatpak", env);
		return;
	}
	snprintf(out, n, "/var/run/user/%u/.flatpak", (unsigned)getuid());
}
INC
prove "the instance base from the environment" \
	check "$T/trust.inc" "$T/base.inc" "$T/plant-ibase.inc" "$T/proxy.inc" \
	"$T/proxypath.inc" "$T/hostcmd.inc"

# 2. the proxy path trusted as given
sed 's/st.st_uid == 0/1/' "$T/proxy.inc" > "$T/plant-proxy.inc"
prove "a caller-writable proxy path" \
	check "$T/trust.inc" "$T/base.inc" "$T/ibase.inc" \
	"$T/plant-proxy.inc" "$T/proxypath.inc" "$T/hostcmd.inc"

# 3. HostCommand without the uid check
sed 's/caller_uid != getuid()/0/' "$T/hostcmd.inc" > "$T/plant-uid.inc"
prove "HostCommand serving another uid" \
	check "$T/trust.inc" "$T/base.inc" "$T/ibase.inc" "$T/proxy.inc" \
	"$T/proxypath.inc" "$T/plant-uid.inc"

# 4. HostCommand without the jail check
sed 's/instance_app_for_jid(jid)/(char*)1/' "$T/hostcmd.inc" \
	> "$T/plant-jail.inc"
prove "HostCommand serving an unrelated jail" \
	check "$T/trust.inc" "$T/base.inc" "$T/ibase.inc" "$T/proxy.inc" \
	"$T/proxypath.inc" "$T/plant-jail.inc"

echo "PASS: the portal's identity base and proxy path come from the uid, and"
echo "      HostCommand serves only this user's flatpkg jails"
