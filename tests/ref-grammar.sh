#!/bin/sh
# ref-grammar.sh - one grammar for refs, remote names and the other path
# components a ref carries: letters, digits, '.', '-' and '_', never a leading
# dot or dash, never a doubled dot.  Without it a ref component becomes a path
# (`app/../../pwn/stable`), an argv word for root's ostree (a remote named
# `--untrusted`) or both.
#
# The accepts half matters as much as the rejects: a grammar that is too tight
# breaks every ordinary install, so a valid dotted ref must still reach the
# planner and a valid remote name must still work.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tests/scratchd.sh"
. "$ROOT/tests/commentlib.sh"

T=$(mktemp -d /tmp/.fpkg-refgrammar.XXXXXX)
tmp=$T
trap 'scratchd_stop; rm -rf "$tmp"' EXIT INT TERM

fail=0
note() {
	echo "FAIL: $*"
	fail=1
}

scratchd_start
run() {
	"$SD_BIN" "$@" </dev/null
}

# A ref component that walks out of the store, or that becomes an option,
# is refused before anything runs.
bad_refs="app/../../pwn/stable app/x/../../y app/.hidden/x app/x/y/z/w \
	app/x/..y/x app/-x/y .. . app/x/-branch \
	bad..remote:app/org.gnome.Gedit :app/org.gnome.Gedit"
for r in $bad_refs; do
	out=$(run uninstall -y "$r" 2>&1) && {
		note "uninstall accepted the invalid ref '$r'"
		continue
	}
	case "$out" in
	*"invalid ref"*) : ;;
	*) note "'$r' was refused without naming the grammar: $out" ;;
	esac
done

# A remote name is an argv word for root's ostree: no leading dash.
for n in --untrusted -x .hidden ..; do
	if run remote-delete -- "$n" >/dev/null 2>&1; then
		note "remote-delete accepted the invalid name '$n'"
	fi
	if run remote-add --if-not-exists "$n" https://example.invalid/r \
		>/dev/null 2>&1; then
		note "remote-add accepted the invalid name '$n'"
	fi
done

# The accepts half: an ordinary dotted ref reaches the planner (it then fails
# on having no remote for it, which is not a grammar refusal), and an ordinary
# remote name can be configured and listed.
out=$(run install -n org.gnome.Gedit 2>&1) || true
case "$out" in
*"invalid ref"*) note "a valid dotted ref was refused: $out" ;;
esac
run remote-add --if-not-exists local "file://$T/repo" >/dev/null 2>&1 ||
	note "a valid remote name was refused"
run remote-list >"$T/list" 2>/dev/null || true
grep -q '^local' "$T/list" || note "the valid remote did not reach remote-list"

# Source half, both directions: the grammar has one definition and the
# destructive paths ask the containment question.
strip_comments <"$ROOT/src/ostree.c" >"$T/ostree.c"
for fn in 'int ref_parse\(' 'bool remote_name_valid\('; do
	body=$(carve_function "^${fn}" "$T/ostree.c" || true)
	[ -n "$body" ] || {
		note "$fn could not be carved"
		continue
	}
	case "$body" in
	*fpkg_component_ok*) : ;;
	*) note "$fn does not go through fpkg_component_ok" ;;
	esac
done
for fn in uninstall_deploy deploy_checkout; do
	body=$(carve_function "^.*${fn}\(" "$T/ostree.c" || true)
	[ -n "$body" ] || {
		note "$fn could not be carved"
		continue
	}
	case "$body" in
	*store_path_contained*) : ;;
	*) note "$fn deletes or creates without store_path_contained" ;;
	esac
done
# mkdir_p follows a symlink that is already there, so the containment
# question is asked of the nearest existing ancestor before it runs.
body=$(carve_function "^int deploy_checkout\(" "$T/ostree.c" || true)
printf '%s\n' "$body" >"$T/deploy.txt"
if ! awk '/store_parent_contained\(/{a=NR} /mkdir_p\(/{b=NR}
	END{exit !(a && b && a<b)}' "$T/deploy.txt"; then
	note "deploy_checkout creates the directory before judging containment"
fi

# A remote a caller carries in a ref, an origin file or a frame is an argv
# word for root's ostree: every pull sink asks the grammar.
strip_comments <"$ROOT/src/ostree.c" >"$T/ostree-plain.c"
for fn in 'int tool_pull\(' 'int tool_pull_metadata\(' \
	   'char\*\* fetch_remote_refs\(' \
	   'int ostree_fetch_ref_metadata\('; do
	body=$(carve_function "^${fn}" "$T/ostree-plain.c" || true)
	case "$body" in
	*remote_name_valid*) : ;;
	*) note "a pull sink ($fn) takes a remote without the grammar" ;;
	esac
done
strip_comments <"$ROOT/src/ostree_cmds.c" >"$T/cmds.c"
for fn in pull_remote_ref pull_child_repo; do
	body=$(carve_function "^static int ${fn}\(" "$T/cmds.c" || true)
	case "$body" in
	*remote_name_valid*) : ;;
	*) note "$fn hands root's ostree an unvalidated remote" ;;
	esac
done

# An extension id comes from remote metadata and becomes a ref and a deploy
# path, so the parser applies the same grammar.
strip_comments <"$ROOT/src/shared/metadata.c" >"$T/metadata.c"
grep -q 'ext_skip = !fpkg_component_ok(group + 10)' "$T/metadata.c" ||
	note "an extension id is not run through the component grammar"

[ "$fail" -eq 0 ] &&
	echo "PASS: refs and remote names carry one grammar, the destructive store"
echo "      paths prove containment, and ordinary names still work"
exit "$fail"
