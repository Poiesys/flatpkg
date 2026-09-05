#!/bin/sh
# hosts-order.sh - the sandbox's /etc/hosts answers "localhost" with IPv4:
# glibc resolves from the first matching line, so an app listening on
# 127.0.0.1 is refused when a ::1-first file sends its dial to IPv6.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
fail=0
note() { echo "FAIL: $*"; fail=1; }

tmp=$(mktemp -d /tmp/.fpkg-hosts.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

# shellcheck source=tests/commentlib.sh
. "$(dirname "$0")/commentlib.sh"

strip_comments < "$ROOT/src/daemon/sandbox.c" > "$tmp/sb.c"

kind=$(carve_function '^hosts_line_kind' "$tmp/sb.c" || true)
stage=$(carve_function '^stage_hosts' "$tmp/sb.c" || true)
etcfn=$(carve_function '^build_etc' "$tmp/sb.c" || true)

# ---- floors: a carve that came back empty would pass everything below ----
printf '%s\n' "$kind" | grep -q 'return v6lo' || {
	note "could not carve hosts_line_kind() out of src/daemon/sandbox.c - the"
	echo "       shape moved, and this guard is no longer looking at the thing"
	echo "       it was written for"
}
printf '%s\n' "$stage" | grep -q 'getline' || {
	note "could not carve stage_hosts() out of src/daemon/sandbox.c"
}
printf '%s\n' "$etcfn" | grep -q 'passwd' || {
	note "could not carve build_etc() out of src/daemon/sandbox.c"
}
if [ $fail -ne 0 ]; then
	exit $fail
fi

# ---- 1. behaviour: run the carved functions on fixtures ------------------
CC_BIN=${CC:-cc}
if ! command -v "$CC_BIN" >/dev/null 2>&1; then
	echo "SKIP: no C compiler ($CC_BIN), so the behaviour claim cannot run"
	exit 0
fi

{
	cat <<'EOF'
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#define fpkg_errorf(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
EOF
	printf '%s\n' "$kind"
	printf '%s\n' "$stage"
	cat <<'EOF'
int main(int argc, char** argv) {
	if (argc != 3) return 2;
	return stage_hosts(argv[1], argv[2]) == 0 ? 0 : 1;
}
EOF
} > "$tmp/h.c"

if ! "$CC_BIN" -include sys/param.h -std=gnu11 -Wall -Wextra \
	-Wno-unused-parameter -Werror -o "$tmp/h" "$tmp/h.c" 2> "$tmp/cc.log"; then
	note "the carved hosts functions no longer compile on their own"
	sed 's/^/       | /' "$tmp/cc.log"
	exit 1
fi

ran=0
check() { # check NAME SOURCE-FILE WANT-FILE
	d="$tmp/case.$ran"
	mkdir -p "$d"
	ran=$((ran + 1))
	if "$tmp/h" "$2" "$d" 2> "$d/err" && cmp -s "$d/hosts" "$3"; then
		echo "ok   $1"
	else
		note "$1: the staged hosts file is not what a Linux app expects"
		echo "       want:"
		sed 's/^/         | /' "$3"
		echo "       got:"
		if [ -f "$d/hosts" ]; then
			sed 's/^/         | /' "$d/hosts"
		else
			echo "         (no file written)"
		fi
		sed 's/^/       stderr: /' "$d/err"
	fi
}

# The FreeBSD default: ::1 first.  Comments stay where they were relative
# to the non-::1 lines; the ::1 line moves behind them; nothing is added,
# because an IPv4 localhost line is already there.
printf '# Host Database\n::1\t\t\tlocalhost localhost.my.domain\n127.0.0.1\t\tlocalhost localhost.my.domain\n# tail\n' > "$tmp/a.in"
printf '# Host Database\n127.0.0.1\t\tlocalhost localhost.my.domain\n# tail\n::1\t\t\tlocalhost localhost.my.domain\n' > "$tmp/a.want"
check "the FreeBSD default puts 127.0.0.1 ahead of ::1" "$tmp/a.in" "$tmp/a.want"

# No IPv4 line for localhost at all: one is supplied, the rest is kept.
printf '::1 localhost\n10.0.0.2 nas\n' > "$tmp/b.in"
printf '127.0.0.1\tlocalhost\n10.0.0.2 nas\n::1 localhost\n' > "$tmp/b.want"
check "an IPv6-only file gets an IPv4 localhost" "$tmp/b.in" "$tmp/b.want"

# No file: the sandbox still gets a hosts file a Linux app can use.
printf '127.0.0.1\tlocalhost\n' > "$tmp/c.want"
check "a missing host file still yields localhost" "$tmp/does-not-exist" "$tmp/c.want"

# The name appears only in a comment, so the IPv4 line is NOT a localhost
# line and one must be added - or ::1 would still be the first answer.
printf '127.0.0.1 foo # localhost\n::1 localhost\n' > "$tmp/d.in"
printf '127.0.0.1\tlocalhost\n127.0.0.1 foo # localhost\n::1 localhost\n' > "$tmp/d.want"
check "a name inside a comment is not a mapping" "$tmp/d.in" "$tmp/d.want"

# glibc matches names without regard to case; a hostname alias and a
# 127.0.1.1 line survive; the last line had no newline and gets one.
printf '::1 LOCALHOST\n127.0.0.1 LocalHost myhost\n127.0.1.1 box' > "$tmp/e.in"
printf '127.0.0.1 LocalHost myhost\n127.0.1.1 box\n::1 LOCALHOST\n' > "$tmp/e.want"
check "names match without case, aliases and the last line survive" "$tmp/e.in" "$tmp/e.want"

# `::1` as a LATER token is not an address line and must not move.
printf '10.0.0.9 weird ::1\n127.0.0.1 localhost\n' > "$tmp/f.in"
cp "$tmp/f.in" "$tmp/f.want"
check "::1 as a name token is not mistaken for an address" "$tmp/f.in" "$tmp/f.want"

# A guard that ran nothing proves nothing.
[ "$ran" -ge 6 ] || note "only $ran behaviour case(s) ran; the harness lost some"

# ---- 2. build_etc() stages it, and a failed write fails the launch -------
printf '%s\n' "$etcfn" | tr '\n\t' '  ' |
	grep -q 'want_network && stage_hosts("/etc/hosts", etc) != 0) *return -1;' || {
	note "build_etc() no longer stages the sandbox's hosts file behind"
	echo "       plan->want_network, or ignores a failed write: the sandbox"
	echo "       would serve whatever the verbatim bind gives it"
}

# ---- 3. the verbatim bind list must not name hosts -----------------------
binds=$(awk '/etc_binds\[\] *=/ { on = 1 } on { print } on && /;/ { exit }' "$tmp/sb.c")
printf '%s\n' "$binds" | grep -q '"resolv.conf"' || {
	note "could not find the etc_binds list (or it lost resolv.conf) - the"
	echo "       shape moved, and claim 3 would pass on an empty carve"
}
if printf '%s\n' "$binds" | grep -q '"hosts"'; then
	note "etc_binds lists \"hosts\": the host's file is mounted over the staged"
	echo "       one, so the sandbox serves FreeBSD's ::1-first order again"
fi

if [ $fail -eq 0 ]; then
	echo "PASS: the sandbox's hosts file puts 127.0.0.1 ahead of ::1 ($ran cases),"
	echo "      build_etc() stages it, and nothing binds the host's over it"
fi
exit $fail
