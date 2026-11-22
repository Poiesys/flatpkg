#!/bin/sh
# dbus-proxy.sh - the session bus reaches an app only through xdg-dbus-proxy:
# it is a runtime dependency of the package, and when it is missing the
# launcher says so.  A fresh install without it gave the sandbox no
# DBUS_SESSION_BUS_ADDRESS, and Bazaar died with "Failed to connect to
# session bus".
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

. "$ROOT/tests/commentlib.sh"

T=$(mktemp -d /tmp/.fpkg-dbusproxy.XXXXXX)
trap 'rm -rf "$T"' EXIT

strip_comments "$ROOT/src/daemon/ops/dbus_proxy.c" > "$T/proxy.c"
carve_function '^int dbus_start_proxies\(' "$T/proxy.c" > "$T/start.inc" ||
	{ echo "FAIL: dbus_start_proxies() not found"; exit 1; }
carve_function '^static void add_bus_filter_args\(' "$T/proxy.c" \
	> "$T/filter.inc" ||
	{ echo "FAIL: add_bus_filter_args() not found"; exit 1; }

bug() {
	echo "FAIL: $1"
	exit 1
}

check() {
	start=$1 port=$2

	grep -q 'xdg-dbus-proxy not found' "$start" ||
		bug "a missing proxy is silent"
	grep -q 'no session bus' "$start" ||
		bug "the missing-proxy warning does not name the symptom"
	grep -q 'xdg-dbus-proxy:devel/xdg-dbus-proxy' "$port" ||
		bug "the package does not depend on xdg-dbus-proxy"
	grep -q 'dbus-daemon:devel/dbus' "$port" ||
		bug "the dbus runtime dependency was dropped"
}
( check "$T/start.inc" "$ROOT/ports/sysutils/flatpkg/Makefile" )

prove() {
	what=$1
	shift
	if ( "$@" ) >/dev/null 2>&1; then
		echo "FAIL: the guard passes with $what"
		exit 1
	fi
}

sed 's/xdg-dbus-proxy not found/xdg-dbus-proxy absent/' \
	"$T/start.inc" > "$T/plant-silent.inc"
prove "a missing proxy is silent" \
	check "$T/plant-silent.inc" "$ROOT/ports/sysutils/flatpkg/Makefile"

sed '\|xdg-dbus-proxy:devel/xdg-dbus-proxy|d' \
	"$ROOT/ports/sysutils/flatpkg/Makefile" > "$T/plant-port"
prove "the package without its proxy dependency" \
	check "$T/start.inc" "$T/plant-port"

# The filter arguments ARE the grant: a name that reaches them is granted.
# A plan built anywhere must still not put flatpkg's own names in.
check_filter() {
	f=$1
	grep -q 'permissions_bus_name_refused(name)' "$f" ||
		{ echo "the filter does not refuse flatpkg's own bus names"; return 1; }
	grep -q 'continue;' "$f" ||
		{ echo "the refusal does not skip the argument"; return 1; }
	return 0
}
check_filter "$T/filter.inc" ||
	bug "add_bus_filter_args() pushes a policy name without checking it"
sed '/permissions_bus_name_refused(name)/d' \
	"$T/filter.inc" > "$T/filter-plant.inc"
cmp -s "$T/filter.inc" "$T/filter-plant.inc" &&
	bug "the filter plant was not applied"
check_filter "$T/filter-plant.inc" >/dev/null 2>&1 &&
	bug "the guard passes without the bus-name refusal"

echo "PASS: the launcher names a missing xdg-dbus-proxy, the package"
echo "      installs it as a runtime dependency, and the filter refuses"
echo "      flatpkg's own bus names"
