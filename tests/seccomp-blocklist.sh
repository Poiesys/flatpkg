#!/bin/sh
# seccomp-blocklist.sh - the blocklist the shim advertises is the blocklist the
# shim enforces: every entry in both tables must be refused through syscall(2),
# the one route the shim controls for all of them.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tests/shimtest.sh"
[ -f "$SHIMTEST_SHIM" ] || { echo "SKIP: build/seccomp_shim.so not built"; exit 0; }
shim_require_fresh || exit 1

fail=0
note() { echo "FAIL: $1"; fail=1; }

tmp=$(mktemp -d /tmp/.fpkg-bl.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

# The expected set is written out **here**, not carved from the shim: an
# expectation derived from the thing it checks shrinks with it and cannot name
# what went missing.  If you add an entry, pin it here in the same commit.
EXPECT_EPERM="acct add_key chroot get_mempolicy keyctl mbind migrate_pages
mount move_pages perf_event_open pivot_root quotactl request_key
set_mempolicy setns syslog umount umount2 unshare uselib"
EXPECT_ENOSYS="clone3 fsconfig fsmount fsopen fspick mount_setattr
move_mount open_tree"
n_rows=$(python3 - "$ROOT/src/seccomp_shim.c" "$tmp/main.c" <<'PY'
import re, sys
src = open(sys.argv[1]).read()
nums = dict(re.findall(r'#define\s+(SYS_[a-z_0-9]+)\s+(\d+)', src))
rows = []
for tbl in ('blocklist_eperm', 'blocklist_enosys'):
    m = re.search(r'seccomp_rule\s+%s\[\]\s*=\s*\{(.*?)\n\};' % tbl, src, re.S)
    for name, err in re.findall(r'\{\s*(SYS_[a-z_0-9]+)\s*,\s*(\w+)\s*\}', m.group(1)):
        if name in nums:
            rows.append((int(nums[name]), err, name))
open(sys.argv[2], 'w').write('''#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/syscall.h>

static const struct { long nr; int err; const char *name; } t[] = {
%s
};

int main(void)
{
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        errno = 0;
        /* Six zero arguments.  The shim must refuse before the kernel
         * is reached; if it does not, every one of these fails on
         * NULL/0 anyway, so a missing entry can cost a missed
         * assertion here but cannot do damage. */
        long r = syscall(t[i].nr, 0L, 0L, 0L, 0L, 0L);
        if (r == 0)
            printf("LEAK %%s\\n", t[i].name);
        else if (errno != t[i].err)
            printf("WRONG %%s %%d %%d\\n", t[i].name, errno, t[i].err);
    }
    printf("COUNT %%d\\n", (int)(sizeof(t) / sizeof(t[0])));
    return 0;
}
''' % '\n'.join('    { %d, %s, "%s" },' % r for r in rows))
print(len(rows))
PY
)
# The pinned set, compared against the shim's tables by name: a missing entry is
# a hole, an extra one an entry nobody agreed to block.
carved_e=$(awk '/^static const struct seccomp_rule blocklist_eperm/,/^};/' \
    "$ROOT/src/seccomp_shim.c" | grep -oE 'SYS_[a-z_0-9]+' | sed 's/^SYS_//' |
    sort | tr '\n' ' ')
carved_n=$(awk '/^static const struct seccomp_rule blocklist_enosys/,/^};/' \
    "$ROOT/src/seccomp_shim.c" | grep -oE 'SYS_[a-z_0-9]+' | sed 's/^SYS_//' |
    sort | tr '\n' ' ')
want_e=$(printf '%s\n' $EXPECT_EPERM | sort | tr '\n' ' ')
want_n=$(printf '%s\n' $EXPECT_ENOSYS | sort | tr '\n' ' ')
if [ "$carved_e" != "$want_e" ]; then
	missing=$(printf '%s\n' $carved_e | tr ' ' '\n' | sort > "$tmp/c1"; printf '%s\n' $EXPECT_EPERM | sort > "$tmp/w1"; comm -23 "$tmp/w1" "$tmp/c1")
	extra=$(printf '%s\n' $carved_e | tr ' ' '\n' | sort > "$tmp/c1"; comm -13 "$tmp/w1" "$tmp/c1")
	[ -n "$missing" ] && note "blocklist_eperm is missing: $(echo $missing)"
	[ -n "$extra" ] && note "blocklist_eperm has unpinned entries: $(echo $extra)"
fi
if [ "$carved_n" != "$want_n" ]; then
	missing=$(printf '%s\n' $carved_n | tr ' ' '\n' | sort > "$tmp/c2"; printf '%s\n' $EXPECT_ENOSYS | sort > "$tmp/w2"; comm -23 "$tmp/w2" "$tmp/c2")
	extra=$(printf '%s\n' $carved_n | tr ' ' '\n' | sort > "$tmp/c2"; comm -13 "$tmp/w2" "$tmp/c2")
	[ -n "$missing" ] && note "blocklist_enosys is missing: $(echo $missing)"
	[ -n "$extra" ] && note "blocklist_enosys has unpinned entries: $(echo $extra)"
fi
[ "$n_rows" -ge 28 ] || note "carved $n_rows blocklist entries, expected 28"

# The errno is a property of the *table*: the shim says these return ENOSYS because
# flatpak's filter does.  Pinned here for the same reason as the set - a generated
# expectation agreed with a SYS_syslog changed from EPERM to ENOSYS.
for pair in "blocklist_eperm EPERM" "blocklist_enosys ENOSYS"; do
	tbl=${pair% *}
	want=${pair#* }
	bad=$(awk "/^static const struct seccomp_rule $tbl/,/^};/" \
	    "$ROOT/src/seccomp_shim.c" |
	    grep -oE 'SYS_[a-z_0-9]+ *, *[A-Z]+' |
	    grep -v ", *$want\$" | tr '\n' ' ')
	[ -n "$bad" ] && note "$tbl entries do not all return $want: $bad"
done

bin=$(shim_build "$tmp/main.c" "$tmp/t" 2>/dev/null) || true
if [ -z "$bin" ] || [ ! -x "$tmp/t" ]; then
	echo "SKIP: cannot build the blocklist test (zig cc unavailable?)"
	exit 0
fi

out=$(shim_run "$tmp/t")
n=$(printf '%s' "$out" | sed -n 's/^COUNT //p')
[ "$n" = "$n_rows" ] || note "the test exercised $n entries, expected $n_rows"
if printf '%s' "$out" | grep -q '^LEAK '; then
	printf '%s' "$out" | grep '^LEAK ' |
	    sed 's/^LEAK /FAIL: /;s/$/ was not refused on the syscall() route/'
	fail=1
fi
if printf '%s' "$out" | grep -q '^WRONG '; then
	printf '%s' "$out" | grep '^WRONG ' |
	    sed 's/^WRONG /FAIL: /;s/$/ was refused with the wrong errno/'
	fail=1
fi

[ $fail -eq 0 ] &&
    echo "PASS: all $n_rows blocklist entries are refused on the syscall() route"
exit $fail
