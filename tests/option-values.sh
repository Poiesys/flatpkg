#!/bin/sh
# option-values.sh - the three shell completions must offer exactly the value
# vocabulary the cli.c parser accepts.  The vocabulary is written four times
# (run(1) and [Context] loops in permissions.c), so every copy must agree.
set -e
# shellcheck source=tests/commentlib.sh
. "$(dirname "$0")/commentlib.sh"

# The expected set is `parsed minus refused`, both carved from the same function,
# so the --share/--unshare asymmetry is not hardcoded here.  carve_block PATTERN
# FILE extracts a brace-delimited block from PATTERN to its close.
carve_block() {
    CARVE_SIG=$1 awk '
        !inb && index($0, ENVIRON["CARVE_SIG"]) { inb = 1 }
        inb { print; o += gsub(/\{/, "{"); c += gsub(/\}/, "}"); if (o > 0 && c >= o) exit }
        END { if (!inb) exit 1 }
    ' "$2"
}

DIR=$(dirname "$0")
SRC="$DIR/../src/cli/cli.c"
BASH="$DIR/../completions/flatpkg.bash"
ZSH="$DIR/../completions/_flatpkg"
FISH="$DIR/../completions/flatpkg.fish"

tmp=$(mktemp -d /tmp/.fpkg-optvals-test.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

# This guard runs the binary for the completion vocabulary, so it owns the
# caller environment those commands could read (tests/worker-env.sh's rule).
export HOME="$tmp/home"
export XDG_RUNTIME_DIR="$tmp/run"
export XDG_CURRENT_DESKTOP=GNOME
export DBUS_SESSION_BUS_ADDRESS="unix:path=$tmp/nonexistent-bus"
export WAYLAND_DISPLAY=
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export TERM=dumb
export NO_COLOR= FORCE_COLOR=
mkdir -p "$HOME" "$XDG_RUNTIME_DIR"

# fish spreads `complete -l NAME \` over two lines; join the continuations
# so one option is one record.  awk, not `sed :a;N;$!ba`: that is a GNUism.
awk '
	/\\$/ { sub(/\\$/, " "); buf = buf $0; next }
	{ print buf $0; buf = "" }
	END { if (buf != "") print buf }
' "$FISH" > "$tmp/fish.flat"

# Carve by counting braces; the `{` is on the line after the signature, so
# only exit once one is seen.  The signature travels in the environment, not
# through `awk -v`: -v expands escapes and would eat the `\(` backslashes.
fail() { echo "FAIL: $*"; exit 1; }

# sockets= and devices= are CHOICE rows fed by arrays; their vocabulary is
# a C initialiser rather than a function.  The two arrays are the parser's
# authority (OPTF_CHECK refuses anything else).
from_set() {
	sed -n "/^static const char\* const $1\[\] = {/,/NULL};/p" "$SRC" |
		grep -o '"[a-z0-9-]\+"' | tr -d '"' | sort -u
}

# --share/--unshare keep their vocabulary in run_share(), because the value
# also has to carry the `!` prefix and report which flag it came from.
share_fn=$(carve_function '^static int run_share\(' "$SRC")
case "$share_fn" in
*run_share*) ;;
*) fail "carving run_share() produced nothing" ;;
esac
printf '%s\n' "$share_fn" | grep -q '^}$' \
	|| fail "carving run_share() stopped before its closing brace"
from_strcmp() {
	printf '%s\n' "$1" | grep -o 'strcmp(n, *"[a-z0-9-]\+")' |
		grep -o '"[a-z0-9-]\+"' | tr -d '"' | sort -u
}

socket_vals=$(from_set sockets_set)
device_vals=$(from_set devices_set)
share_vals=$(from_strcmp "$share_fn")

for v in socket_vals device_vals share_vals; do
	eval "got=\$$v"
	[ -n "$got" ] || fail "no vocabulary extracted for $v - the parsers moved"
done

# The refusal branch must key on --share.  The grep matches the unnegated
# `unshare &&` too, so losing the `!` is caught here rather than surfacing
# later as "the completions are missing process and all".
refuse_line=$(printf '%s\n' "$share_fn" | grep 'unshare &&' | head -1)
if [ -n "$refuse_line" ]; then
	# Match the negation itself: `unshare &&` still contains "unshare".
	case "$refuse_line" in
	*'!unshare'*) ;;
	*) fail "set_share()'s refusal branch no longer keys on --share" ;;
	esac
	refused=$(from_strcmp "$refuse_line")
	[ -n "$refused" ] || fail "set_share()'s refusal branch no longer names values"
else
	refused=""
fi

# comm(1) compares files, not shell strings.  Sorted, so the result does not
# depend on the order the completions happen to list the values in.
setdiff() { # setdiff A B MODE -> the lines in A (MODE 23) or B (MODE 13)
	comm "-$3" "$1" "$2"
}
printf '%s\n' "$share_vals" | sort -u > "$tmp/share.all"
printf '%s\n' "$refused" | sort -u > "$tmp/share.refused"
share_ok=$(setdiff "$tmp/share.all" "$tmp/share.refused" 23)
[ -n "$share_ok" ] || fail "every --share value is refused; the parser moved"

# option -> the set the completions must offer for it.
set_expected() {
	case "$1" in
	socket) printf '%s\n' "$socket_vals" ;;
	device) printf '%s\n' "$device_vals" ;;
	unshare) printf '%s\n' "$share_vals" ;;
	share) printf '%s\n' "$share_ok" ;;
	esac
}

# The completions no longer carry a value list: each asks `flatpkg complete
# values`, which reads the same table the parser does.  So the check is that
# they ask, and that the answer is the parser's vocabulary.
FLATPKG="$DIR/../build/flatpkg"
[ -x "$FLATPKG" ] || { echo "SKIP: build/flatpkg not built"; exit 0; }

for f in "$BASH" "$ZSH" "$FISH"; do
	grep -q 'complete words' "$f" || fail "$(basename "$f") does not ask for options"
	grep -q 'complete values' "$f" || fail "$(basename "$f") does not ask for values"
	grep -q 'complete operands' "$f" || fail "$(basename "$f") does not ask for operands"
done

rc=0
for opt in socket share unshare device; do
	expected=$tmp/exp.$opt
	set_expected "$opt" | sort -u > "$expected"
	[ -s "$expected" ] || fail "no expected values for --$opt"

	offered=$tmp/got.$opt
	"$FLATPKG" complete values -- "--$opt" | sort -u > "$offered"
	[ -s "$offered" ] || fail "complete values --$opt offers nothing"

	missing=$(comm -23 "$expected" "$offered" | tr '\n' ' ')
	extra=$(comm -13 "$expected" "$offered" | tr '\n' ' ')
	[ -n "$missing" ] && {
		echo "FAIL: complete values --$opt omits accepted value(s): $missing"
		rc=1
	}
	[ -n "$extra" ] && {
		echo "FAIL: complete values --$opt offers value(s) the parser rejects: $extra"
		rc=1
	}
done

# ---- and the vocabulary is written FOUR times ------------------------
# The other two copies are the loops in permissions_apply_overrides(), which had
# drifted: set_device() refused `snd` while the loop honoured it.
PERM="$DIR/../src/shared/permissions.c"
ctx_loop() { # ctx_loop LIST-NAME
	# No `^` anchor: the loop is indented eight spaces.  Signature quoted
	# through the environment, not `awk -v` - -v eats the backslashes.
	carve_block "for (size_t i = 0; i < meta->$1.count" "$PERM"
}

ovr_loop() { # ovr_loop LIST-NAME
	carve_block "for (size_t i = 0; i < opts->$1.count" "$PERM"
}

# Carved on their loop headers, not on a comment: the override path walks
# opts-> lists and the [Context] path below walks meta-> ones.  Matched on the
# `plan->` field they assign, so a compare with no assignment does not count.
socket_loop=$(ovr_loop sockets)
device_loop=$(ovr_loop devices)
share_loop=$(ovr_loop shares)
for pair in "socket:$socket_loop" "device:$device_loop" "share:$share_loop"; do
	lname=${pair%%:*}; lbody=${pair#*:}
	case "$lbody" in
	*"plan->"*) ;;
	*) fail "carving the --$lname override loop produced no plan-> assignment - permissions.c moved" ;;
	esac
done
loop_vals() { # loop_vals BODY FIELD-PREFIX
	printf '%s\n' "$1" | grep -o "strcmp(n, \"[a-z0-9-]\+\")" |
		grep -o '"[a-z0-9-]\+"' | tr -d '"' | sort -u
}
perm_socket=$(loop_vals "$socket_loop")
perm_device=$(loop_vals "$device_loop")
perm_share=$(loop_vals "$share_loop")
for v in perm_socket perm_device perm_share; do
	eval "got=\$$v"
	[ -n "$got" ] || fail "no vocabulary extracted from the --$v override loop - the loop moved"
done

# Two names reach the loop as something else, so neither is a defect: `process`
# is inherent (an explicit no-op case) and `all` is expanded by set_share() into
# `!network`/`!ipc`.  Both exemptions are verified below, or they are holes.
exempt_share="process all"

# Assert the expansion, not the comment above: set_share() must push `!network`
# AND `!ipc` for `all`, or --unshare=all silently keeps every share.  Anchored on
# the push, not the strcmp; `|| true` because an empty grep ends `set -e`.
expand_net=$(printf '%s\n' "$share_fn" |
	grep -F -B 3 'push(&r->run.shares, "!network")' || true)
case "$expand_net" in
*'"all"'*) : ;;
*) fail "run_share() no longer expands 'all' into '!network', so the"
   fail "       --share exemption below would hide a real dangling value" ;;
esac
expand_ipc=$(printf '%s\n' "$share_fn" |
	grep -F -B 3 'push(&r->run.shares, "!ipc")' || true)
case "$expand_ipc" in
*'"all"'*) : ;;
*) fail "run_share() expands 'all' into '!network' but not '!ipc'" ;;
esac
# Selected by NAME, not packed into a colon-joined triple: the values are
# newline-separated, so `cut -d: -f2` would take one line of a list.
for oname in socket device share; do
	case "$oname" in
	socket) parser_side=$socket_vals; loop_side=$perm_socket; exempt="" ;;
	device) parser_side=$device_vals; loop_side=$perm_device; exempt="" ;;
	share)  parser_side=$share_vals;  loop_side=$perm_share;
	         exempt=$exempt_share ;;
	esac
	printf '%s\n' "$parser_side" | sort -u > "$tmp/p.$oname"
	printf '%s\n' "$loop_side" | sort -u > "$tmp/l.$oname"
	# `tr ' ' '\n'` because the exemption is one readable string while comm(1)
	# compares lines, and `grep -v '^$'` drops the empty line an empty list
	# leaves, which would sort first and take a real name with it.
	printf '%s\n' "$exempt" | tr ' ' '\n' | grep -v '^$' | sort -u \
		> "$tmp/x.$oname"
	comm -23 "$tmp/p.$oname" "$tmp/x.$oname" > "$tmp/pe.$oname"

	dangling=$(comm -23 "$tmp/pe.$oname" "$tmp/l.$oname" | tr '\n' ' ')
	unreachable=$(comm -13 "$tmp/p.$oname" "$tmp/l.$oname" | tr '\n' ' ')
	[ -n "$dangling" ] && {
		echo "FAIL: --$oname accepts $dangling but permissions_apply_overrides()"
		echo "       ignores it: flatpkg tells the user the value is valid and"
		echo "       then does not grant it"
		rc=1
	}
	[ -n "$unreachable" ] && {
		echo "FAIL: permissions_apply_overrides() honours $unreachable for"
		echo "       --$oname but the cli.c parser refuses it, so the [Context] path can"
		echo "       grant what the command line cannot ask for"
		rc=1
	}
	[ -z "$dangling" ] && [ -z "$unreachable" ] &&
		echo "ok   the --$oname vocabulary agrees between the cli.c parser and"
	echo "        permissions_apply_overrides() ($(printf '%s\n' "$parser_side" | grep -cv '^$' || true) value(s), minus $(printf '%s\n' "$exempt" | tr ' ' '\n' | grep -cv '^$' || true) declared inherent)"
done

# The [Context] loops are a FIFTH copy, and the one that matters: an app
# declares its permissions there, so a name dropped here is unreachable.
# Extraction takes "anything up to the comma" - the loops compare different locals.
ctx_names() { # ctx_names CARVED
	printf '%s\n' "$1" |
		grep -o 'strcmp([^,]*, *"[a-z0-9-]\+")' |
		grep -o '"[a-z0-9-]\+"' | tr -d '"' | sort -u
}
ctx_socket=$(ctx_names "$(ctx_loop sockets)")
ctx_device=$(ctx_names "$(ctx_loop devices)")
ctx_shared=$(ctx_names "$(ctx_loop shared)")
for v in ctx_socket ctx_device ctx_shared; do
	eval "got=\$$v"
	[ -n "$got" ] || fail "no vocabulary extracted from the [Context] ${v#ctx_} loop"
done

# shared is a SUBSET on purpose: upstream's `[Context] shared=` is network and
# ipc only, while the CLI also takes `process` (inherent) and `all` (expanded).
# So the check is one-way: what the metadata loop honours must be accepted.
for oname in socket device; do
	case "$oname" in
	socket) parser_side=$socket_vals; loop_side=$perm_socket ;;
	device) parser_side=$device_vals; loop_side=$perm_device ;;
	esac
	case "$oname" in
	socket) ctx_side=$ctx_socket ;;
	device) ctx_side=$ctx_device ;;
	esac
	printf '%s\n' "$ctx_side" | sort -u > "$tmp/c.$oname"
	printf '%s\n' "$parser_side" | sort -u > "$tmp/p3.$oname"
	printf '%s\n' "$loop_side" | sort -u > "$tmp/l3.$oname"
	# the [Context] loop against BOTH other copies, in one comm each
	only_ctx=$(comm -13 "$tmp/p3.$oname" "$tmp/c.$oname" | tr '\n' ' ')
	missing_ctx=$(comm -23 "$tmp/p3.$oname" "$tmp/c.$oname" | tr '\n' ' ')
	# and against the override loop, so a name honoured by one run(1)
	# path and dropped by the other is caught too
	only_ctx_loop=$(comm -13 "$tmp/l3.$oname" "$tmp/c.$oname" | tr '\n' ' ')
	[ -n "$only_ctx" ] && {
		echo "FAIL: [Context] $oname= honours $only_ctx, which"
		echo "       permissions_apply_overrides() and the cli.c parser do not: the"
		echo "       metadata path grants a capability the CLI refuses"
		rc=1
	}
	[ -n "$missing_ctx" ] && {
		echo "FAIL: [Context] $oname= is missing $missing_ctx, which"
		echo "       the cli.c parser accepts and permissions_apply_overrides() honours:"
		echo "       an app's own manifest cannot ask for it"
		rc=1
	}
	[ -n "$only_ctx_loop" ] && {
		echo "FAIL: [Context] $oname= honours $only_ctx_loop, which the"
		echo "       run(1) override loop does not: the two paths disagree"
		rc=1
	}
	[ -z "$only_ctx" ] && [ -z "$missing_ctx" ] && [ -z "$only_ctx_loop" ] &&
		echo "ok   the [Context] $oname= loop agrees with the cli.c parser and with"
	echo "        permissions_apply_overrides() ($(printf '%s\n' "$ctx_side" | grep -cv '^$' || true) value(s))"
done

# shared: one-way containment, and the subset is the claim.
printf '%s\n' "$ctx_shared" | sort -u > "$tmp/c.shared"
printf '%s\n' "$share_vals" | sort -u > "$tmp/p3.shared"
not_in_cli=$(comm -13 "$tmp/p3.shared" "$tmp/c.shared" | tr '\n' ' ')
[ -z "$not_in_cli" ] || {
	echo "FAIL: [Context] shared= honours $not_in_cli, which the cli.c parser refuses"
	rc=1
}
cli_only=$(comm -23 "$tmp/p3.shared" "$tmp/c.shared" | tr '\n' ' ')
# Assert the size, not the content: an empty metadata loop would satisfy
# containment and grant nothing.
nctx=$(grep -cv '^$' "$tmp/c.shared" || true)
[ "$nctx" -ge 2 ] || {
	echo "FAIL: the [Context] shared= loop honours $nctx name(s), below the"
	echo "       floor of 2 - either the loop moved or the extraction broke"
	rc=1
}
[ -z "$not_in_cli" ] && echo "ok   [Context] shared= honours $nctx name(s), all of them"
echo "        accepted by the parser, and the $cli_only it does not carry is the"
echo "        documented subset (upstream's shared= is network and ipc only)"

# Every argument-taking command must name a candidate kind, read from the
# table the shells ask, so a command that gains an operand kind cannot be
# missed by one shell's private list.
COMP_REQUIRED="run install search enter kill info update rollback
              list-commits override uninstall remove remote-delete
              remote-add upgrade import"
for c in $COMP_REQUIRED; do
	kind=$("$FLATPKG" complete operands "$c" 2>/dev/null)
	[ -n "$kind" ] || {
		echo "FAIL: '$c' takes an argument but complete operands names no kind"
		rc=1
	}
done
ncmds=$("$FLATPKG" complete commands | grep -c . || true)
[ "$ncmds" -ge 20 ] || {
	echo "FAIL: complete commands listed $ncmds commands, below the floor"
	rc=1
}

[ "$rc" -eq 0 ] && echo "PASS: completions offer exactly the accepted value sets"
exit "$rc"
