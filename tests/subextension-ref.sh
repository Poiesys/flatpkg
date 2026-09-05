#!/bin/sh
# subextension-ref.sh - ext_id_matches_declared() and group_covers_branch(), which
# decide what `upgrade --prune` may delete.  A sub-extension is named in no metadata,
# the runtime declares the parent, so the branch dimension matters.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$(dirname "$0")/../src/shared/metadata.c"
HARNESS="$(dirname "$0")/subextension-ref.c"

if ! command -v cc >/dev/null 2>&1; then
	echo "SKIP: no cc in PATH"
	exit 0
fi

line=$(grep -n '^static bool group_covers_branch(const fp_ext_group_t\* g' "$SRC" \
    | cut -d: -f1)
if [ -z "$line" ]; then
	echo "FAIL: group_covers_branch() not found in $SRC"
	exit 1
fi

tmp=$(mktemp -d /tmp/.fpkg-subext-test.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/tree"

# The two helpers are adjacent, in this order, and the fragment ends at the closing
# brace of ext_id_matches_declared(): carve from the `static bool` above
# group_covers_branch() to the *second* column-0 '}', then assert the span is them.
awk -v s="$line" '
	NR >= s { print }
	NR > s && $0 == "}" { n++; if (n == 2) exit }
' "$SRC" > "$tmp/subext.inc"

# The signatures are anchored on the NAME, not the return type: the file spells
# one `static bool` and the other plain `bool`, so assuming one spelling matched
# only the first and reported a reordering.
for want in '^static bool group_covers_branch(const fp_ext_group_t\* g' \
            '^bool ext_id_matches_declared(const char\* id'; do
	if ! grep -q "$want" "$tmp/subext.inc"; then
		echo "FAIL: the extracted fragment lacks $want;"
		echo "      the two helpers are no longer adjacent in that order"
		exit 1
	fi
done
if grep -q '^fp_metadata_free(' "$tmp/subext.inc"; then
	echo "FAIL: the extracted fragment runs on past ext_id_matches_declared()"
	exit 1
fi

cc -O0 -Wall -Wextra -I"$tmp" -o "$tmp/subext" "$HARNESS"
"$tmp/subext"

# --- the ref kind at every uninstall_deploy() call site ---
# deploy_type is the store directory (<store>/extension/<id>); ref_kind is the ostree ref
# prefix, deploy/runtime/<id> for an extension.  Passing the layout name as the ref kind
# leaves the real ref pinning every object of the removed extension.
. "$(dirname "$0")/commentlib.sh"

OSTREE="$(dirname "$0")/../src/ostree.c"

# Every call site, in either spelling: two literals (checked directly) or ONE
# identifier twice, which is only right if the function has already dealt with
# "extension" by then - for an extension the two arguments differ.
strip_comments < "$OSTREE" > "$(dirname "$0")/.subext_stripped.$$"
STRIPPED="$(dirname "$0")/.subext_stripped.$$"
trap 'rm -f "$STRIPPED"' EXIT
sites=$(awk '
	/^[A-Za-z_].*\(/ && !/;/ { fn = $0; seen_ext = 0 }
	/strcmp\([A-Za-z_]+, "extension"\) == 0/ { seen_ext = 1 }
	/^[[:space:]]*uninstall_deploy\(/ {
		n++
		line = $0
		sub(/^[[:space:]]*uninstall_deploy\(/, "", line)
		split(line, a, /,[[:space:]]*/)
		kind = (a[1] ~ /^"/) ? "lit" : "var"
		same = (a[1] == a[2]) ? 1 : 0
		printf "%d|%s|%s|%s|%d|%d\n", NR, kind, a[1], a[2], same, seen_ext
	}' "$STRIPPED")
ncalls=$(printf '%s\n' "$sites" | grep -c . || true)
# Floor: the two callers left after the system-helper uninstall moved to
# the CLI transaction.  A drop means the extraction stopped seeing them,
# not that the removals went away.
if [ "${ncalls:-0}" -lt 2 ]; then
	echo "FAIL: found $ncalls uninstall_deploy() call sites, expected at least 2."
	echo "      If call sites moved, update this check - do not delete it, or it"
	echo "      stops testing anything."
	exit 1
fi
bad_kind=$(printf '%s\n' "$sites" | awk -F'|' '
	$2 == "lit" && $4 !~ /^"(app|runtime)"$/ { print }
	$2 == "var" && ($5 != 1 || $6 != 1) { print }')
if [ -n "$bad_kind" ]; then
	echo "FAIL: an uninstall_deploy() call site can pass a ref kind the store has"
	echo "      no ref for.  ref_kind is the ostree ref prefix; only \"app\" and"
	echo "      \"runtime\" exist.  An extension deploy lives in <store>/extension/"
	echo "      but its ref is deploy/runtime/, so \"extension\" as the ref kind asks"
	echo "      ostree to delete a ref that does not exist and leaves the real one"
	echo "      pinning every object against --refs-only prune.  A literal site must"
	echo "      name app or runtime; a variable site must pass one identifier twice,"
	echo "      after a strcmp(<it>, \"extension\") branch (line|kind|arg1|arg2|same|"
	echo "      after-branch):"
	printf '%s\n' "$bad_kind" | sed 's/^/       /'
	exit 1
fi

# The call sites are the check.  What is asserted about the convention is the
# SIGNATURE, not the sentence above it: looking for the prose failed on a rewording
# and would be disabled by deleting the comment.
defn=$(carve_function '^uninstall_deploy' "$OSTREE" || true)
if [ -z "$defn" ]; then
	echo "FAIL: uninstall_deploy()'s definition was not found in ostree.c."
	exit 1
fi
# Match the PARAMETER LIST, not the whole carve: the body still says ref_kind, so a
# substring test over the carve passes with the parameter gone.
sig=$(printf '%s\n' "$defn" | sed -n '1,/^{/p' | tr '\n' ' ')
# The file writes `const char* ref_kind`: a whitespace-only difference in the
# source must not read as the parameter's removal.
case "$sig" in
*"const char* ref_kind"*) ;;
*)
	echo "FAIL: uninstall_deploy() no longer takes a ref_kind parameter."
	echo "      The call sites above are the whole set of places the ref kind"
	echo "      is chosen, so losing the parameter means they stop being that."
	exit 1
	esac

echo "ok   every uninstall_deploy() call site passes a ref_kind the store has"

# --- write_deploy_data() is given a BRANCH dir, not a commit dir ---
# The function appends the commit itself, so passing the commit dir that
# deploy_checkout() fills writes <commit>/<commit>/deploy: a file that exists, sits one
# level below where the reader looks, and is reported by nobody.
#
# Comments are excluded POSITIONALLY, because strip_comments() deletes lines and would
# report the violation at the wrong line.  A call after a trailing /* ... */ is missed.
code_grep() { grep -n "$@" | grep -vE '^[0-9]+:[[:space:]]*(\*|/\*|//)' || true; }

NDATA=$(code_grep 'write_deploy_data(' "$OSTREE" | wc -l | tr -d ' ')
passing_deploy=$(code_grep 'write_deploy_data(deploy' "$OSTREE")
if [ -n "$passing_deploy" ]; then
	echo "FAIL: write_deploy_data() called with \"deploy\", which is the COMMIT"
	echo "      dir deploy_checkout() fills.  The function appends the commit"
	echo "      itself, so this writes <commit>/<commit>/deploy - a file that"
	echo "      exists, is ignored by every reader, and satisfies no check."
	echo "      Trim one component first, as write_origin() does:"
	printf '%s\n' "$passing_deploy" | sed 's/^/       /'
	exit 1
fi

# The floor comes AFTER the specific check, not before: when it was first, a plant
# that introduced the bug tripped the floor instead ("found 6, expected 5") and sent
# the reader to a count.  5 = declaration, definition, three call sites.
if [ "${NDATA:-0}" -ne 5 ]; then
	echo "FAIL: found $NDATA write_deploy_data() references in $OSTREE, expected 5"
	echo "      (the forward declaration, the definition, and three call"
	echo "      sites).  If they moved, update this check - do not delete it,"
	echo "      or it stops testing anything."
	exit 1
fi
echo "ok   no write_deploy_data() call site is handed a commit dir"

# --- the backfill reaches extensions, proven by RUNNING it ---
# The checks above are structural; this runs the real binary on a synthetic store, so
# it is the same evidence on any machine.  `repair` runs in the daemon as root.
. "$ROOT/tests/scratchd.sh"
trap 'scratchd_stop; rm -rf "$tmp" "$EXTFX"; rm -f "$STRIPPED"' EXIT
EXTFX=$(mktemp -d /tmp/.fpkg-subext-fx.XXXXXX)
scratchd_start
FLATPKG=$SD_BIN
EXTSTORE=$SD_STORE
COMMIT=aaaa1111bbbb2222cccc3333dddd4444eeee5555ffff6666aaaa7777bbbb8888

# Two deploys, one app and one extension, neither with a deploy file or a
# .ref marker.  The app one is the CONTROL: if it is not repaired either,
# the fixture is wrong and the extension result would prove nothing.
for kind in app extension; do
	D="$EXTFX/$kind/org.example.$kind/x86_64/stable/$COMMIT"
	mkdir -p "$D/files" "$D/export"
	ln -s "$COMMIT" "$EXTFX/$kind/org.example.$kind/x86_64/stable/active"
	printf '[Application]\nname=org.example.%s\n' "$kind" > "$D/metadata"
done
mdo cp -Rp "$EXTFX/." "$EXTSTORE/"

out=$(HOME="$EXTFX" timeout 120 "$FLATPKG" repair 2>&1 </dev/null) || true

# Safety, asserted rather than assumed: prove we were not pointed at the
# live store before believing anything the run reported.
if [ "$EXTSTORE" = /var/db/flatpkg ] || [ -z "$EXTSTORE" ]; then
	echo "FAIL: the scratch store is the live one; refusing to run repair"
	exit 1
fi

# `repair` is report-only (man/flatpkg.8: a repair that repairs can destroy a
# working tree), so the claim is that the synthetic deploys are left alone.
bad=""
for kind in app extension; do
	D="$EXTSTORE/$kind/org.example.$kind/x86_64/stable/$COMMIT"
	[ -e "$D/deploy" ] && bad="$bad $kind:deploy-written"
	[ -e "$D/$COMMIT/deploy" ] && bad="$bad $kind:deploy-one-level-too-deep"
	[ -e "$D/files/.ref" ] && bad="$bad $kind:ref-written"
done
if [ -n "$bad" ]; then
	echo "FAIL repair wrote to a store it only reports on:$bad"
	echo "       repair said:"
	printf '%s\n' "$out" | sed 's/^/         /'
	exit 1
fi

# ---- the launch-time half: enable-if= warns once per GROUP ------------
# `enable-if=` is parsed and read nowhere, so flatpkg says it ignores it: once per group,
# not per (target, version).  Three claims below.  commentlib.sh is sourced, never run.
EXT="$ROOT/src/shared/extensions.c"
sb=$(strip_comments < "$EXT")

# 1. ONE copy of the sentence.  Zero is a regression (the key is silently
#    ignored again); two is the defect returning in a second place.
n=$(printf '%s\n' "$sb" | grep -c 'download-if= instead' || true)
[ "${n:-0}" -eq 1 ] || {
	echo "FAIL src/shared/extensions.c has $n copies of the enable-if= warning;"
	echo "       one, because it is one rule about one key (round 104)."
	exit 1
}

# 2. It is NOT in resolve_extension() any more.  A grep for "does it warn about
#    enable-if?" cannot tell the two apart - the sentence is there either way.
res=$(carve_function '^resolve_extension' "$EXT" || true)
[ -n "$res" ] || { echo "FAIL could not carve resolve_extension() out of $EXT"; exit 1; }
case "$res" in
*'download-if= instead'*)
	echo "FAIL the enable-if= warning is back inside resolve_extension(), which"
	echo "       runs once per (target, version) - four times for one group."
	exit 1 ;;
esac

# 3. It is emitted from the group loops, once each, and BEFORE the
#    per-version loop - so it cannot be per version no matter what the
#    loop bodies do.  Two call sites: the app's groups and the runtime's.
calls=$(printf '%s\n' "$sb" | grep -c 'warn_extension_keys(g);' || true)
[ "${calls:-0}" -eq 2 ] || {
	echo "FAIL the enable-if= warning has $calls call sites, expected 2 -"
	echo "       one per metadata ([Application] groups, [Runtime] groups)."
	exit 1
}
# Ordering inside each loop: the call precedes the version iteration.
printf '%s\n' "$sb" | awk '
	/warn_extension_keys\(g\);/ { if (!warned) warned = NR }
	/for \(size_t v = 0; v < g->versions.count; v\+\+\)/ {
		if (warned && warned < NR) { found++; warned = 0 }
	}
	END { exit(found == 2 ? 0 : 1) }' || {
	echo "FAIL the enable-if= warning is emitted inside or after the"
	echo "       per-version loop in one of the two group loops, so it still"
	echo "       repeats once per version."
	exit 1
}

echo "ok   the enable-if= warning is said once per GROUP, not per version"
