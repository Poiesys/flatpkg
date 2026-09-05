#!/bin/sh
# seccomp-socket.sh - the shim's socket(2) filter must reach every route an app
# can take: the glibc interposer and syscall(SYS_socket), both through
# socket_allowed(), while an ordinary AF_INET SOCK_DGRAM socket still opens.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tests/shimtest.sh"

[ -f "$SHIMTEST_SHIM" ] || { echo "SKIP: build/seccomp_shim.so not built"; exit 0; }
shim_require_fresh || exit 1

fail=0
note() { echo "FAIL: $1"; fail=1; }

tmp=$(mktemp -d /tmp/.fpkg-sock.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

# `socket` has to be in the version script, or the interposer is unreachable
# however correct the C is.  The map is tab-indented; `^ *` does not match a tab.
grep -qE '^[[:space:]]*socket;$' "$ROOT/src/seccomp_shim.map" ||
    note "src/seccomp_shim.map does not export socket, so the interposer is unreachable"

cat > "$tmp/t.c" <<'EOF'
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <linux/netlink.h>

int main(void)
{
    errno = 0;
    int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    printf("GLIBC_RAW %d\n", fd >= 0 ? 0 : errno);
    if (fd >= 0) close(fd);

    errno = 0;
    long r = syscall(SYS_socket, AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    printf("SYSCALL_RAW %d\n", r >= 0 ? 0 : errno);
    if (r >= 0) close((int)r);

    errno = 0;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    printf("GLIBC_DGRAM %d\n", fd >= 0 ? 0 : errno);
    if (fd >= 0) close(fd);

    /*
     * The FLAGGED spelling of SOCK_RAW.  `type` arrives with
     * SOCK_CLOEXEC OR'd in, and the types are small integers rather than
     * bits, so `type == SOCK_RAW` is false for the form every real program
     * writes and the check did not apply to it - measured in round 334,
     * when the predicate was corrected to mask the type.  AF_NETLINK is
     * the family to test it on: the host allows a raw netlink socket
     * outright, so a refusal here is the shim and not the kernel.
     */
    errno = 0;
    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    printf("GLIBC_RAW_CLOEXEC %d\n", fd >= 0 ? 0 : errno);
    if (fd >= 0) close(fd);
    return 0;
}
EOF

bin=$(shim_build "$tmp/t.c" "$tmp/t") || {
	echo "SKIP: cannot build the test program (zig cc unavailable?)"
	exit 0
}

get() { printf '%s' "$1" | sed -n "s/^$2 //p"; }
plain=$(shim_run_plain "$bin")
shim=$(shim_run "$bin")

[ -n "$plain" ] || { note "the control run produced no output"; exit 1; }

# Control: the raw netlink socket must work *without* the shim, or a
# refusal proves nothing.  Here the host allows it outright.
[ "$(get "$plain" GLIBC_RAW)" = "0" ] ||
    note "the host already refuses a raw netlink socket, so the shim is untested"
[ "$(get "$plain" SYSCALL_RAW)" = "0" ] ||
    note "the host already refuses it on the syscall route too"

# Both routes refused, with EPERM(1).
for k in GLIBC_RAW SYSCALL_RAW; do
	[ "$(get "$shim" "$k")" = "1" ] ||
	    note "$k was not refused with EPERM: errno=$(get "$shim" "$k")"
done

# The flagged spelling must be refused too - this is the case the
# unmasked comparison let through.
[ "$(get "$plain" GLIBC_RAW_CLOEXEC)" = "0" ] ||
    note "the host already refuses a flagged raw netlink socket, so that case is untested"
[ "$(get "$shim" GLIBC_RAW_CLOEXEC)" = "1" ] ||
    note "SOCK_RAW|SOCK_CLOEXEC was not refused with EPERM: errno=$(get "$shim" GLIBC_RAW_CLOEXEC)"

# Negative direction: an ordinary datagram socket must still open.
[ "$(get "$shim" GLIBC_DGRAM)" = "0" ] ||
    note "the shim broke an ordinary AF_INET SOCK_DGRAM socket: errno=$(get "$shim" GLIBC_DGRAM)"

[ $fail -eq 0 ] &&
    echo "PASS: socket(2) is filtered on both routes and normal sockets work"
exit $fail
