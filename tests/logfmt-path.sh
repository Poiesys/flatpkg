#!/bin/sh
# logfmt-path.sh - a root worker must not write to a path the caller chose:
# -l/FLATPKG_LOGFMT open in flatpkg_main() as root on the caller's argv, so
# O_NOFOLLOW and an ownership check - and the caller's own log still works.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
FLATPKG="$ROOT/build/flatpkg"
[ -x "$FLATPKG" ] || { echo "SKIP: build/flatpkg not built"; exit 0; }
# There is no separate flatpkgd binary: `daemon` is a subcommand, so look
# for the running process rather than a flatpkgd on PATH.
id -Gn | tr ' ' '\n' | grep -qx flatpkg ||
	{ echo "SKIP: not in the flatpkg group; the root worker is the subject"; exit 0; }

fail=0
note() { echo "FAIL: $1"; fail=1; }

T=$(mktemp -d /tmp/.fpkg-logfmt.XXXXXX)
trap 'rm -rf "$T"' EXIT

# All three spellings drive the same open: run_route 0 is -l, 1 and 2 set the
# environment.  The command is `history`, which only reads but logs; `repair`
# mutated the live store, and the root worker ignores FLATPKG_STORE anyway.
run_route() {
	case $1 in
	0) "$FLATPKG" -l "$2" history >/dev/null 2>&1 ;;
	1) FLATPKG_LOGFMT="$2" "$FLATPKG" history >/dev/null 2>&1 ;;
	2) FLATPKG_LOGFMT="$2" "$FLATPKG" history >/dev/null 2>&1 ;;
	esac
}
ROUTE_NAMES="-l FLATPKG_LOGFMT FLATPKG_LOGFMT"

for route in 0 1 2; do
	rname=$(printf '%s\n' $ROUTE_NAMES | sed -n "$((route + 1))p")

	# 1. A symlink the caller owns must not be followed by root.
	printf 'PRECIOUS\n' > "$T/victim"
	ln -sf "$T/victim" "$T/link"
	run_route "$route" "$T/link" || true
	grep -q '^ts=' "$T/victim" &&
		note "$rname: root appended through a caller-owned symlink"
	[ "$(cat "$T/victim")" = "PRECIOUS" ] ||
		note "$rname: the file behind the symlink was modified"

	# 2. A path that does not exist: creating it is intended, but root must
	#    not keep it, and it must hold the record - a fix that refuses
	#    everything would pass otherwise.
	rm -f "$T/owned"
	run_route "$route" "$T/owned" || true
	[ -e "$T/owned" ] || note "$rname: the log file was not created; it is broken"
	if [ -e "$T/owned" ]; then
		owner=$(stat -f '%Su' "$T/owned")
		[ "$owner" = "$(id -un)" ] ||
			note "$rname: the log file is owned by '$owner', not the caller"
		grep -q '^ts=' "$T/owned" ||
			note "$rname: the log file is empty; logfmt is being refused, not redirected"
	fi

	# 3. Naming a root-owned file must be refused outright.  /etc/hosts
	#    is only ever read here, and the guard asserts it did not change.
	before=$(cksum /etc/hosts | awk '{print $1, $2}')
	run_route "$route" /etc/hosts || true
	after=$(cksum /etc/hosts | awk '{print $1, $2}')
	[ "$before" = "$after" ] ||
		note "$rname: modified a root-owned file (/etc/hosts)"
done

[ $fail -eq 0 ] &&
	echo "PASS: a root worker writes only to a file the caller owns, on all three routes"
exit $fail
