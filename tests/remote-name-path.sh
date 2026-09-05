#!/bin/sh
# remote-name-path.sh - a remote name and an app id stay one path component.
# `name` from argv becomes <store>/keys/<name>.gpg, which a root worker writes
# and unlinks, so the assertions are on the filesystem, not the exit status.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tests/scratchd.sh"

tmp=$(mktemp -d /tmp/.fpkg-rmtname.XXXXXX)
trap 'scratchd_stop; rm -rf "$tmp"' EXIT
scratchd_start
FLATPKG=$SD_BIN
# The directory the traversals climb to.  The store is $W/store, so the
# depth is measured from there and the "victim" files sit in $W itself.
W=$SD_TMP

# Own the environment the code reads, so the host cannot move the answer.  PATH
# drops the caller's: the code resolves program names through it.  TERM=dumb
# turns colour off so a message's width and escapes are the same every run.
XDG_RUNTIME_DIR=$tmp/run
DBUS_SESSION_BUS_ADDRESS=unix:path=$tmp/no-such-bus
XDG_CURRENT_DESKTOP=GNOME
WAYLAND_DISPLAY=wayland-0
PATH=/usr/bin:/bin:/usr/local/bin
TERM=dumb
mkdir -p "$XDG_RUNTIME_DIR" "$tmp/home"
export XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS XDG_CURRENT_DESKTOP
export WAYLAND_DISPLAY PATH TERM NO_COLOR= FORCE_COLOR=

# Source commentlib.sh for carve_function and strip_comments
. "$(dirname "$0")/commentlib.sh"

fail=0
note() { echo "FAIL: $1"; fail=1; }

S=$SD_STORE
FX=$tmp/fixture
mkdir -p "$FX/repo" "$FX/keys" "$tmp/home" "$W/victim"
printf 'a real keyring\n' > "$FX/keys/flathub.gpg"
mdo cp -R "$FX/." "$S/"
printf 'do not delete me\n' > "$W/victim/secret.gpg"

run() { HOME=$tmp/home "$FLATPKG" "$@" >/dev/null 2>&1 </dev/null || true; }

# How many levels up <store>/keys has to climb to reach $tmp.  Worked out by
# asking the kernel: a wrong depth lands on a path that does not exist and the
# test passes for the wrong reason.
up=""
p="$S/keys"
while [ "$p" != "$W" ]; do p=$(dirname "$p"); up="$up/.."; done
[ -n "$up" ] || up="/.."

# Names that must never reach the filesystem: the traversal, plus a hidden
# name, a nested name and a bare dot so the rule is structural, not a denylist.
for name in \
    "$up/victim/secret" \
    "$up/victim" \
    '..' \
    '.' \
    '.hidden' \
    'a/b' \
    'x y' \
    'x:y' \
    "x'y"
do
    # Fresh victim before *each* command, or a remote-delete's unlink would be
    # attributed to the remote-add that follows it.
    printf 'do not delete me\n' > "$W/victim/secret.gpg"
    run remote-delete "$name"
    [ -f "$W/victim/secret.gpg" ] ||
        note "remote-delete '$name' deleted a file outside the store"
    printf 'do not delete me\n' > "$W/victim/secret.gpg"
    run remote-add "$name" https://example.invalid/repo/
    [ -f "$W/victim/secret.gpg" ] ||
        note "remote-add '$name' deleted a file outside the store"
done

# Nothing may have been written outside <store>/keys either.  The traversal
# names a file, not a directory, so a stray write is <something>.gpg in $tmp;
# assert on that, not the whole tree, or an empty listing satisfies it.
[ -z "$(find "$W" -maxdepth 3 -name '*.gpg' -not -path "$S/keys/*" \
        -not -path "$W/victim/*" 2>/dev/null)" ] ||
    note "a .gpg file was written outside <store>/keys"

# The store's own keyring is untouched by all of the above.
[ -f "$S/keys/flathub.gpg" ] || note "the real keyring was removed"

# Positive direction: the refusals above are all satisfied by a build
# that refuses every name, so the ones that must work have to.
out=$(HOME=$tmp/home \
    "$FLATPKG" remote-add ok-name https://example.invalid/repo/ 2>&1 </dev/null || true)
case $out in
*"added remote ok-name"*) ;;
*) note "a valid remote name was refused: $out" ;;
esac
for name in flathub test-1 my_remote a.b; do
    out=$(HOME=$tmp/home \
        "$FLATPKG" remote-add "$name" https://example.invalid/repo/ 2>&1 </dev/null || true)
    case $out in
    *"added remote $name"*) ;;
    *) note "valid remote name '$name' was refused: $out" ;;
    esac
done

# Structural: one implementation of the rule.  systemhelper.c used to carry its
# own copy of this predicate, which is how the two sides came to disagree about
# whether a name was valid at all.
if grep -qE 'valid_remote_name\(' "$ROOT/src/daemon/systemhelper.c" 2>/dev/null &&
   ! grep -q 'define valid_remote_name' "$ROOT/src/daemon/systemhelper.c" 2>/dev/null
then
    note "systemhelper.c defines its own valid_remote_name(); the rule "\
"must live once, in ostree.c"
fi
grep -q 'remote_name_valid' "$ROOT/include/flatpkg.h" ||
    note "remote_name_valid() is not declared in flatpkg.h, so the two "\
"callers cannot share it"

# Structural, and the honest limit of the runs above: the write site needs a
# fetched repo and a parsed GPGKey, which no fixture can produce.  Every
# interpolating store_path() must sit in a validating function; "%s" is exempt.
n_guarded=0
n_sites=0
# Use carve_function from commentlib.sh (sourced above) which handles
# static functions and multi-line definitions correctly.
for fn in $(awk '
    /^(static )?[a-zA-Z_*][a-zA-Z_0-9* ]* [a-z_][a-z_0-9]*\(/ { 
        fn = $0
        sub(/^(static )?[a-zA-Z_*][a-zA-Z_0-9* ]* /, "", fn)
        sub(/\(.*/, "", fn)
    }
    /store_path\([^,]+,[^,]+, *"/ {
        line = $0
        sub(/.*store_path\([^,]+,[^,]+, *"/, "", line)
        sub(/".*/, "", line)
        if (line ~ /%s/ && line != "%s" && fn != "") print fn
    }
' "$ROOT/src/ostree.c" | sort -u)
do
    body=$(carve_function "^$fn" "$ROOT/src/ostree.c" | strip_comments)
    n_sites=$((n_sites + 1))
    if printf '%s' "$body" | grep -qE 'remote_name_valid|strchr\(arch|valid_arch'; then
        n_guarded=$((n_guarded + 1))
    else
        note "$fn() builds a store path from an interpolated name without validating it"
    fi
done
# If the carve ever stops finding the sites, this must fail rather than
# pass on an empty list.  Three is the count after the dead helper removal.
[ "$n_sites" -ge 3 ] ||
    note "expected at least 3 functions building an interpolated store path,"\
" found $n_sites (the carve no longer matches the source)"
[ "$n_guarded" = "$n_sites" ] ||
    note "only $n_guarded of $n_sites interpolated store paths are validated"

# The same rule, the other path out of the store: `flatpkg override`'s app id
# became ~/.local/share/flatpak/overrides/<id> with no check, in a root worker.
# `--system` writes <store>/overrides/<id> through the daemon (FP_REQ_OVERRIDE).
O=$S
mkdir -p "$tmp/home2"
depth=0; probe=$O/overrides
while [ "$probe" != "$W" ]; do probe=$(dirname "$probe"); depth=$((depth + 1)); done
up=""
i=0; while [ "$i" -lt "$depth" ]; do up="$up/.."; i=$((i + 1)); done

for scope in --system --user; do
    for name in "$up/escaped" '/../../escaped2' '..' '.' '.hidden' 'a/b'; do
        printf 'do not delete me\n' > "$W/victim.gpg"
        HOME=$tmp/home \
            "$FLATPKG" override $scope "$name" --filesystem=host \
            >/dev/null 2>&1 </dev/null || true
        [ -f "$W/victim.gpg" ] ||
            note "override $scope '$name' wrote outside the store"
        [ ! -e "$W/escaped" ] && [ ! -e "$W/escaped2" ] ||
            note "override $scope '$name' created a file outside the store"
    done
done

# Negative direction: the ids flatpak actually uses must still work, in
# both scopes, or the check is satisfied by refusing everything.
for name in org.gnome.Fractal com.example.my-app org.example.App_2; do
    for scope in --system --user; do
        out=$(HOME=$tmp/home \
            "$FLATPKG" override $scope "$name" --filesystem=host 2>&1 </dev/null || true)
        case $out in
        *"invalid application id"*)
            note "override $scope refused the valid app id '$name': $out" ;;
        esac
    done
done
# The global override takes no app id and must not be caught by the
# check, which is why the rule skips a NULL/empty name.
out=$(HOME=$tmp/home \
    "$FLATPKG" override --filesystem=host 2>&1 </dev/null || true)
case $out in
*"invalid application id"*) note "the global override was refused: $out" ;;
esac
[ -f "$tmp/home/.local/share/flatpak/overrides/global" ] ||
    note "the global override was not written"

# The system scope, end to end through the daemon: a root-owned file made of the
# keys the parser knows, merged by a second change, removed by --reset.  Without
# it, a build refusing `override --system` outright would pass.
SYS=$S/overrides/org.example.Sys
run override --system org.example.Sys --filesystem=host --env=FOO=bar
if [ ! -f "$SYS" ]; then
    note "override --system wrote no file through the daemon"
else
    [ "$(ls -l "$SYS" | awk '{print $3}')" = root ] ||
        note "the system override is not root-owned: a caller-owned file in the store"
    grep -q '^filesystems=host;' "$SYS" ||
        note "the system override lost its filesystem change"
    grep -q '^FOO=bar' "$SYS" ||
        note "the system override lost its environment change"
    run override --system org.example.Sys --socket=x11
    { grep -q '^sockets=x11;' "$SYS" && grep -q '^filesystems=host;' "$SYS"; } ||
        note "a second system override did not merge with the first"
    run override --system --reset org.example.Sys
    [ ! -e "$SYS" ] || note "override --system --reset left the file"
fi

# Every overrides_path() call site must survive the NULL this check can return.
# `|| true` on both: `set -e` would end the script on a zero count, which is
# exactly the rename these two are here to catch.
n_sites=$(grep -c 'overrides_path(' "$ROOT/src/shared/overrides.c" || true)
n_nulls=$(grep -c 'if (path == NULL)' "$ROOT/src/shared/overrides.c" || true)
# One of the six is the definition itself, so the call sites are five.
[ "$n_sites" -ge 6 ] ||
    note "expected at least 6 mentions of overrides_path(), found $n_sites"
[ "$n_nulls" -ge 5 ] ||
    note "$n_sites overrides_path() mentions but only $n_nulls NULL checks"

[ $fail -eq 0 ] &&
    echo "PASS: a remote name stays one path component, and so does an app id"
exit $fail
