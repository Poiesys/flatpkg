#!/bin/sh
# seccomp-clone.sh - the seccomp shim must close all three routes into a new
# namespace.  clone(2) matches on the argument because clone is also how a thread is
# born, so each namespace bit must be EPERM while threads and fork still work.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SHIM=$ROOT/build/seccomp_shim.so
[ -f "$SHIM" ] || { echo "SKIP: build/seccomp_shim.so not built"; exit 0; }

# Refuse to test a stale shim: deleting the interposer while leaving `clone;` in the
# version script makes the *link* fail, so `make` stops without replacing the .so and
# the guard would PASS against the previous build.
for dep in "$ROOT/src/seccomp_shim.c" "$ROOT/src/seccomp_shim.map"; do
	if [ "$dep" -nt "$SHIM" ]; then
		echo "FAIL: $SHIM is older than $(basename "$dep") - the" \
		     "shim was not rebuilt, so this guard would test the" \
		     "previous binary.  Run make first."
		exit 1
	fi
done

fail=0
note() { echo "FAIL: $1"; fail=1; }

tmp=$(mktemp -d /tmp/.fpkg-seccomp.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

# The symbol has to be in the version script or the dynamic linker binds the
# application's own `clone` and the interposer is never consulted.  The map is
# tab-indented; `^ *clone` does not match a tab.
grep -qE '^[[:space:]]*clone;$' "$ROOT/src/seccomp_shim.map" ||
    note "src/seccomp_shim.map does not export clone, so the interposer is unreachable"

cat > "$tmp/t.c" <<'EOF'
#define _GNU_SOURCE
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>

static void *thr(void *a) { *(int *)a = 42; return NULL; }

int main(void)
{
    pthread_t t;
    int got = 0;
    if (pthread_create(&t, NULL, thr, &got) != 0) {
        printf("THREAD fail %d\n", errno);
    } else {
        pthread_join(t, NULL);
        printf("THREAD ok %d\n", got);
    }
    pid_t p = fork();
    if (p == 0) _exit(0);
    if (p > 0) { int st; waitpid(p, &st, 0); printf("FORK ok\n"); }
    else printf("FORK fail %d\n", errno);

    errno = 0;
    clone(NULL, NULL, CLONE_NEWUSER | SIGCHLD, NULL);
    printf("NEWUSER %d\n", errno);
    errno = 0;
    clone(NULL, NULL, CLONE_NEWNS | SIGCHLD, NULL);
    printf("NEWNS %d\n", errno);
    errno = 0;
    clone(NULL, NULL, CLONE_NEWIPC | SIGCHLD, NULL);
    printf("NEWIPC %d\n", errno);
    return 0;
}
EOF

# Built for the same target as the shim itself (an x86_64-linux-gnu ELF to be
# LD_PRELOADed into).  Skips rather than fails if the toolchain is unavailable,
# so the guard is not a build-dependency for the suite.
if ! zig cc --target=x86_64-linux-gnu -O2 -o "$tmp/t" "$tmp/t.c" 2>/dev/null
then
    echo "SKIP: cannot build the test program (zig cc unavailable?)"
    exit 0
fi

plain=$($tmp/t 2>/dev/null || true)
shim=$(LD_PRELOAD=$SHIM $tmp/t 2>/dev/null || true)

get() { printf '%s' "$1" | sed -n "s/^$2 //p"; }

# Control: the namespace calls must be *doing something* without the shim, or
# "refused with EPERM under the shim" proves nothing.  A non-zero errno other
# than EPERM(1) counts - the point is only that the shim is not what refuses.
[ -n "$plain" ] || { note "the control run produced no output"; exit 1; }
for k in NEWUSER NEWNS NEWIPC; do
	v=$(get "$plain" "$k")
	[ -n "$v" ] || note "the control run never reached the $k case"
	[ "$v" = 1 ] &&
	    note "$k was already refused on this host, so the shim is untested"
done

# Threads and fork must survive: the interposer forwards the
# non-namespace case, and glibc's fork uses the internal __clone, which
# never reaches it.
[ "$(get "$shim" THREAD)" = "ok 42" ] ||
    note "the shim broke pthread_create: $(get "$shim" THREAD)"
[ "$(get "$shim" FORK)" = "ok" ] ||
    note "the shim broke fork: $(get "$shim" FORK)"

# ...and the three namespace bits must be EPERM.
for k in NEWUSER NEWNS NEWIPC; do
	[ "$(get "$shim" "$k")" = "1" ] ||
	    note "clone(CLONE_${k#NEW}) was not refused with EPERM: errno=$(get "$shim" "$k")"
done

[ $fail -eq 0 ] &&
    echo "PASS: clone(2) refuses every namespace bit and still forks and threads"
exit $fail
