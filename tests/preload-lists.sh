#!/bin/sh
# preload-lists.sh - the two shim preload lists must agree, and every shim
# they name must be copied into the jailroot.  Both lists are pinned
# independently of the source, so a changed list is a conscious act.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/src/daemon/sandbox_root.c
[ -f "$SRC" ] || { echo "SKIP: src/daemon/sandbox_root.c not found"; exit 0; }

fail=0
note() { echo "FAIL: $1"; fail=1; }

tmp=$(mktemp -d /tmp/.fpkg-pl.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

extract() {   # $1 = the marker line that introduces the block
	awk -v marker="$1" '
	    index($0, marker) { on = 1 }
	    on { print }
	    on && /write_file\(path,/ && seen { exit }
	    on && /write_file\(path,/ { seen = 1 }
	' "$SRC" | grep -oE '/\.[a-z_0-9]+_shim\.so' | sed 's|^/||; s|^\.||; s|\.so$||' | sort -u
}

# /etc/ld.so.preload: the literal written straight into the file.
sed -n '/ld\.so\.preload", etc)/,/0644);/p' "$SRC" |
    grep -oE '/\.[a-z_0-9]+_shim\.so' | sed 's|^/||; s|^\.||; s|\.so$||' | sort -u \
    > "$tmp/ldso"
# LD_PRELOAD: the setenv value.  The call spans several concatenated lines.
sed -n '/setenv("LD_PRELOAD")/,/^[[:space:]]*1);/p' "$SRC" |
    grep -oE '/\.[a-z_0-9]+_shim\.so' | sed 's|^/||; s|^\.||; s|\.so$||' | sort -u \
    > "$tmp/env"

# Both lists carry all five.  Identical is the *fixed* state, not drift: the
# seccomp exclusion was closed once the shim was measured against what a
# subsandbox actually performs.
want_ldso="madvise_shim memfd_shim rlimit_shim seccomp_shim signalfd_shim syncobj_shim"
want_env="$want_ldso"

got_ldso=$(tr '\n' ' ' < "$tmp/ldso" | sed 's/ $//')
got_env=$(tr '\n' ' ' < "$tmp/env" | sed 's/ $//')
w_ldso=$(printf '%s\n' $want_ldso | sort | tr '\n' ' ' | sed 's/ $//')
w_env=$(printf '%s\n' $want_env | sort | tr '\n' ' ' | sed 's/ $//')

[ -s "$tmp/ldso" ] || { note "could not find the ld.so.preload list - the carve broke"; exit 1; }
[ -s "$tmp/env" ]  || { note "could not find the LD_PRELOAD list - the carve broke"; exit 1; }

[ "$got_ldso" = "$w_ldso" ] ||
    note "ld.so.preload now carries [$got_ldso]; this guard pins [$w_ldso]"
[ "$got_env" = "$w_env" ] ||
    note "LD_PRELOAD now carries [$got_env]; this guard pins [$w_env]"

# The two lists must agree: a shim in LD_PRELOAD but not ld.so.preload is one
# a scrubbed-env subsandbox silently runs without.
comm -13 "$tmp/ldso" "$tmp/env" > "$tmp/only_env"
comm -23 "$tmp/ldso" "$tmp/env" > "$tmp/only_ldso"
[ -s "$tmp/only_ldso" ] &&
    note "a shim is in ld.so.preload but not LD_PRELOAD: $(tr '\n' ' ' < "$tmp/only_ldso")"
[ -s "$tmp/only_env" ] &&
    note "these shims are in LD_PRELOAD but not ld.so.preload, so a subsandbox will not get them: $(tr '\n' ' ' < "$tmp/only_env")"

# Every shim in either list must also exist as a source, or the name is
# a typo that ld.so would ignore in silence.
for s in $(cat "$tmp/ldso" "$tmp/env" | sort -u); do
	[ -f "$ROOT/src/$s.c" ] ||
	    note "$s is preloaded but src/$s.c does not exist - ld.so would ignore it"
done

# ... and must also be COPIED into the jailroot, or the name refers to a file
# that is never there and ld.so runs with no filter.  dmabuf_devt_shim.so is
# absent on purpose: it goes to /run/flatpak and is preloaded conditionally.
. "$(dirname "$0")/commentlib.sh"
strip_comments < "$SRC" > "$tmp/nocomment"

no_copy=""
for s in $(cat "$tmp/ldso" "$tmp/env" | sort -u); do
	grep -qF "{\"$s.so\"," "$tmp/nocomment" || no_copy="$no_copy $s"
done
[ -n "$no_copy" ] &&
    note "preloaded but never copied into the jailroot:$no_copy - ld.so ignores a preload entry for a file that is not there"

# The table and copy_shim() are the two halves of that copy.  Assert the
# shape is still there, so a carve that matches nothing cannot pass.
[ "$(grep -cE '\{"[a-z_0-9]+_shim\.so",' "$tmp/nocomment")" -ge 6 ] ||
    note "found fewer than 6 jailroot shim table entries - the carve broke"
[ "$(grep -cF '"%s/.%s"' "$tmp/nocomment")" -ge 1 ] ||
    note "found no jailroot shim copy pattern - the carve broke"

[ $fail -eq 0 ] &&
    echo "PASS: both preload lists carry all six shims, so a subsandbox is not left unfiltered"
exit $fail
