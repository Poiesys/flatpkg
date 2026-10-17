#!/bin/sh
# name-as-path.sh - every path flatpkg builds from a caller-supplied name
# must be a single path component.  This is the bare-`snprintf` sink shape,
# which the store_path() carve in remote-name-path.sh cannot see.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
fail=0
note() { echo "FAIL: $1"; fail=1; }

# Comments are stripped before any body is grepped for a validator: a
# validator named only in a comment is not a validator.
. "$(dirname "$0")/commentlib.sh"
# Each entry: file:function:the validator it must carry.  "digits" is
# resolve_jid's all-digits loop, which is the right rule for an instance
# id rather than a name rule.
check() {
	f=$(srcf "$1")
	fn=$2
	want=$3
	[ -f "$f" ] || { note "$1 is missing"; return; }
	# strip_comments here too: without it the guard passes on a deleted
	# validator named in the comment above the function.
	# Use carve_function from commentlib.sh which handles return types
	body=$(tmp=$(mktemp /tmp/.fpkg-carve.XXXXXX); strip_comments < "$f" > "$tmp"; carve_function "^""$fn""" "$tmp"; rm -f "$tmp")
	if [ -z "$body" ]; then
		note "$fn() is gone from $1 - re-run the survey"
		return
	fi
	case $want in
	digits)
		printf '%s' "$body" |
		    grep -q "\*c < '0' || \*c > '9'" ||
		    note "$fn() lost its all-digits guard"
		;;
	*)
		printf '%s' "$body" | grep -q "remote_name_valid" ||
		    note "$fn() no longer calls remote_name_valid()"
		;;
	esac
}

# The remote-name sinks.
check ostree_cmds.c cmd_remote_add       remote
check ostree_cmds.c cmd_remote_delete    remote
# The app-id sink: the one in a bare snprintf, not store_path.
check overrides.c overrides_path         remote
# The instance id: a jail id, not a name, so digits.
check instance_ctl.c resolve_jid             digits

# The `store_path` class, counted mechanically.  A format of exactly "%s"
# is exempt: the two sites taking a name out of a static table of literals.
n_sites=0
n_guarded=0
# cmd_remote_add writes keys/%s.gpg and must validate the name.
for fn in cmd_remote_add; do
	body=$(tmp=$(mktemp /tmp/.fpkg-carve.XXXXXX); strip_comments < "$ROOT/src/ostree_cmds.c" > "$tmp"; carve_function "^""$fn""" "$tmp"; rm -f "$tmp")
	if [ -z "$body" ]; then
		note "$fn() is gone from ostree_cmds.c - re-run the survey"
		continue
	fi
	n_sites=$((n_sites + 1))
	if printf '%s' "$body" | grep -qE 'remote_name_valid'; then
		n_guarded=$((n_guarded + 1))
	else
		note "$fn() lost its remote_name_valid guard"
	fi
done
[ "$n_sites" -ge 1 ] ||
    note "expected at least 1 interpolated store_path() site, found $n_sites"
[ "$n_guarded" = "$n_sites" ] ||
    note "only $n_guarded of $n_sites interpolated store paths are validated"

# The `app/<id>/pointer` layout is the import convention; one spelling
# keeps the three callers from drifting.
n_spell=$(grep -c 'app/%s/pointer' "$ROOT"/src/*.c "$ROOT"/src/*/*.c "$ROOT"/src/*/*/*.c 2>/dev/null |
    awk -F: '{ t += $2 } END { print t + 0 }')
[ "$n_spell" -le 1 ] ||
    note "the <store>/app/<id>/pointer layout is spelled out $n_spell times"

[ $fail -eq 0 ] &&
    echo "PASS: every name flatpkg turns into a path is checked first"
exit $fail
