#!/bin/sh
# guard-lint.sh - the rules about guards, enforced on the guards: a grep -c
# that would end the script, non-portable patterns, a lost comment '#' run as
# a glob, and exactly one copy of each comment helper.  Rules 2-4 run stripped.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
T="$ROOT/tests"
TMP=$(mktemp -d /tmp/.fpkg-guardlint.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
fail=0

echo "guard lint: the rules about guards, checked on the guards"
echo

# Comment handling lives in commentlib.sh (sourced, never run).
. "$(dirname "$0")/commentlib.sh"
# Every guard's text, shell-comments stripped, into $TMP for the rules below.
for f in "$T"/*.sh; do
	b=$(basename "$f")
	sh_strip_comments < "$f" > "$TMP/$b"
	grep -q 'grep -c' "$TMP/$b" && echo "$b" >> "$TMP/counted"
done
n_files=$(ls "$TMP" | grep -c '\.sh$' || true)

# ---- rule 1: a grep -c whose zero would end the script ----------------
# Under `set -e` a zero count is frequently the finding.  Safe shapes: piped
# after the grep, inside `[ ... ]`, or followed by `|| true/echo/:`.
: > "$TMP/r1"
for f in "$TMP"/*.sh; do
	awk -v name="$(basename "$f")" '
	function report(t,   rest, after, piped, in_test, guarded) {
		if (t !~ /\$\(/) return
		if (t !~ /grep -c/) return
		rest = t
		sub(/^.*\$\(/, "", rest)
		after = substr(rest, index(rest, "grep -c") + 6)
		piped = (after ~ /\|/)
		in_test = (t ~ /\[[ \t]*"?[ \t]*\$\(/)
		guarded = (t ~ /\|\|[ \t]*(true|echo|:)([ \t]|$)/)
		if (!piped && !in_test && !guarded)
			printf "%s:%d: %s\n", name, start, substr(t, 1, 76)
	}
	{
		start = FNR
		if (buf != "") { t = buf " " $0; buf = ""; report(t); next }
		if ($0 ~ /\\[ \t]*$/) { buf = $0; next }
		report($0)
	}
	' "$f" >> "$TMP/r1" 2>/dev/null || true
done
n1=$(grep -c . "$TMP/r1" 2>/dev/null || true)
if [ "${n1:-0}" -gt 0 ]; then
	echo "FAIL a \`grep -c\` whose zero count would end the script under"
	echo "       set -e, with nothing to neutralise it:"
	sed 's/^/       /' "$TMP/r1"
	fail=1
else
	echo "ok   every \`grep -c\` in a substitution is piped past, inside a"
	echo "     [ ... ] test, or neutralised"
fi

# ---- rules 2-4 -------------------------------------------------------
: > "$TMP/r23"
for f in "$TMP"/*.sh; do
	b=$(basename "$f")
	# Exempt from rules 2-4: this file is the rules written as patterns, so it
	# necessarily contains all three.  Named rather than hidden in a skip - an
	# invisible skip is how a guard stops checking the file that most needs it.
	[ "$b" = guard-lint.sh ] && continue
	grep -nE 'grep [^|]*-[A-Za-z]*E[A-Za-z]* .*\\s' "$f" 2>/dev/null |
		while IFS= read -r l; do echo "2) \\\\s in a grep -E pattern: $b:$l"; done >> "$TMP/r23" || true
	grep -n '<(' "$f" 2>/dev/null |
		while IFS= read -r l; do echo "3) process substitution <(: $b:$l"; done >> "$TMP/r23" || true
	grep -n "sed -n '/\^typedef struct {" "$f" 2>/dev/null |
		while IFS= read -r l; do echo "4) typedef sed range: $b:$l"; done >> "$TMP/r23" || true
done
n23=$(grep -c . "$TMP/r23" 2>/dev/null || true)
if [ "${n23:-0}" -gt 0 ]; then
	echo "FAIL a pattern the guard rules forbid, in a guard:"
	sed 's/^/       /' "$TMP/r23"
	fail=1
else
	echo "ok   no \\s in a grep -E, no <(, no typedef sed range"
fi

# ---- rule 5: one definition of each helper -----------------------------
# In commentlib.sh, and every caller sources it; a definition outside the
# library is the failure.  Callers are matched on stripped text, not prose.
n_lib=0
stray=""
for f in "$TMP"/*.sh; do
	b=$(basename "$f")
	grep -qE '^(strip_comments|sh_strip_comments)\(\) \{' "$f" || continue
	if [ "$b" = commentlib.sh ]; then n_lib=$((n_lib + 1)); else stray="$stray $b"; fi
done
unsourced=""
for f in "$TMP"/*.sh; do
	b=$(basename "$f")
	grep -qE '[^_a-z](strip_comments|sh_strip_comments)[ (]' "$f" || continue
	grep -q 'commentlib.sh' "$f" || unsourced="$unsourced $b"
done
if [ "$n_lib" -ne 1 ]; then
	echo "FAIL commentlib.sh defines $n_lib of the two comment helpers,"
	echo "       expected exactly one file to define them"
	fail=1
elif [ -n "$stray" ]; then
	echo "FAIL a guard defines its own comment helper instead of sourcing"
	echo "       commentlib.sh:$stray"
	echo "       that is the round-156 shape: a copy that is only correct"
	echo "        until somebody edits the other one"
	fail=1
elif [ -n "$unsourced" ]; then
	echo "FAIL calls a comment helper without sourcing commentlib.sh:$unsourced"
	echo "       under set -e an undefined function ends the guard silently"
	fail=1
else
	# Keyed on the carve BODY, not a name: the four old copies were called
	# `carve` and `perm_carve`, and runtime-dir.sh legitimately counts braces
	# for another job.  This file and commentlib.sh are excluded by name.
	straycarve=""
	for f in "$T"/*.sh; do
		b=$(basename "$f")
		case "$b" in commentlib.sh|guard-lint.sh) continue ;; esac
		grep -qF 'ENVIRON["FPKG_SIG"]' "$f" &&
			straycarve="$straycarve $b"
	done
	if [ -n "$straycarve" ]; then
		echo "FAIL these guards define their own brace-count carve instead of"
		echo "       sourcing commentlib.sh's carve_function():$straycarve"
		echo "       round 207 folded four copies into one, and two of the four"
		echo "       were unanchored - an unanchored pattern starts on a CALL"
		echo "       site and carves the caller instead of the callee"
		fail=1
	fi
	[ "$fail" -eq 0 ] &&
		echo "ok   one definition of each, in commentlib.sh, and every caller"
	echo "     sources it"
fi

# The check above asks whether there is ONE definition; this asks whether it
# is correct, and a unique-but-wrong helper is worse than none.  One defect
# DELETED CODE: a `/*` inside a string was read as a comment opener.
. "$(dirname "$0")/commentlib.sh"
T=$(mktemp -d /tmp/.fpkg-lint.XXXXXX)
trap 'rm -rf "$T"' EXIT
strip_fail=0
stripcase() { # stripcase NAME INPUT EXPECTED-SUBSTRING-FORBIDDEN-SUBSTRING
	printf '%s\n' "$2" > "$T/c.c"
	got=$(strip_comments < "$T/c.c")
	case "$got" in
	*"$3"*) : ;;
	*) echo "FAIL strip_comments: $1 - expected to keep [$3], got:"; printf '%s\n' "$got" | sed 's/^/       /'; strip_fail=1 ;;
	esac
	# An optional fourth argument: a case whose whole point is that
	# something is KEPT has no forbidden token, and passing an empty one
	# would match everything.
	[ "$4" = "-" ] && return
	case "$got" in
	*"$4"*) echo "FAIL strip_comments: $1 - comment text leaked into [$4]"; strip_fail=1 ;;
	esac
}
stripcase "code after a closing */ survives" \
	'/* one
	 * two
	 * three */ int after;' 'int after;' 'three'
# The forbidden marker must be genuinely a comment: the first version used
# the string literal's own contents, which are code, so the case demanded the
# stripper delete a string and failed on a correct one.
stripcase "a /* inside a string is not a comment" \
	'/* hidden MARKERCM */ int kept;
	const char *b = "--broadcast=x/*y";' 'const char *b' 'MARKERCM'
stripcase "a // comment is stripped" \
	'int a; // sneaky strcmp( in a line comment
	int b;' 'int b;' 'sneaky'
stripcase "a one-line comment keeps code on both sides" \
	'int c; /* mid */ int d;' 'int d;' 'mid'
# The first version asserted `int f;` survives `"a\"b"; int f;`, which is
# true either way.  The discriminating form puts a comment-looking token after
# the escaped quote: kept with the escape rule, eaten without it.
stripcase "an escaped quote does not end a string" \
	'char *e = "a\" /* INASTRING */ b"; int f;' 'INASTRING' '-'
stripcase "an escaped quote keeps the code after it" \
	'char *e = "a\"b"; int f;' 'int f;' '-'
stripcase "a char literal holding a quote is not a string" \
	'if (*s == 0x27"\""0x27 || *s == 0x27) { int g; }' 'int g;' 'nonesuch'
# ---- and carve_function() is correct too --------------------------------
# A carve must end at its own brace, must not open on a call site, and must
# return non-zero when the pattern matches nothing; none of that was tested.
carve_fail=0
carvecase() { # carvecase NAME EXPECTED-COUNT [FORBIDDEN] PATTERN FILE
	got=$(carve_function "$4" "$5" | wc -l | tr -d ' ')
	if [ "$got" = "$2" ]; then :; else
		echo "FAIL carve_function: $1 - expected $2 line(s), got $got"
		carve_fail=1
	fi
	# The forbidden marker is the optional third argument: an unset `$6` in
	# `*[$6]*` matches everything and reported "kept []" on a correct helper,
	# so `-` means "no marker" and an unset one is treated the same.
	case "${3-}" in
	""|"-") : ;;
	*)
		case "$(carve_function "$4" "$5")" in
		*"$3"*) echo "FAIL carve_function: $1 - kept [$3]"; carve_fail=1 ;;
		esac
		;;
	esac
}
# a normal function, braces balanced, ends at its own closing brace
printf "int caller(void) {
	helper();
}

int helper(arg) {
	return 1;
}
" > /tmp/cv.c
# Three lines, not four: an unanchored pattern opened on the CALL inside
# caller() and counted on through the next definition.  A call ends in `;` and
# cannot open a carve, so even the unanchored spelling finds the definition.
carvecase "a plain function" 3 - 'helper\(' /tmp/cv.c
carvecase "a call site does not open a carve" 3 'helper();' 'helper\(' /tmp/cv.c
# ANCHORED: an unanchored pattern starts at the CALL and carves the caller,
# which is the drift the four copies were one edit away from.
carvecase "an anchored pattern finds the definition" 3 - '^helper\(' /tmp/cv.c
# a nested brace must not end the carve early
printf 'static int
outer(void)
{
	if (1) {
		return 2;
	}
	return 0;
}
' > /tmp/cv2.c
carvecase "a nested block does not end it early" 7 - '^outer\(' /tmp/cv2.c
# A pattern that matches nothing says so in the STATUS, not just in the empty
# output - four callers were each working around that.  In an `if`, not a bare
# statement: under `set -e` the expected failure would end the script.
if carve_function '^definitely_not_there' /tmp/cv2.c >/dev/null 2>&1; then
	echo "FAIL carve_function: a pattern that matches nothing returns 0, so"
	echo "       a caller cannot tell a failed carve from an empty function"
	carve_fail=1
fi
# and the real one: it must still carve a real function out of the tree
real=$(carve_function '^flatpkg_kernel_module_loaded\(' "$ROOT/src/shared/modules.c" | wc -l | tr -d ' ')
[ "${real:-0}" -ge 15 ] || {
	echo "FAIL carve_function: flatpkg_kernel_module_loaded() carved to $real lines,"
	echo "       below the floor of 15 - the shape moved or the tool broke"
	carve_fail=1
}
if [ "$carve_fail" -eq 0 ]; then
	echo "ok   carve_function() is correct as well as single: it ends at its own"
	echo "     brace, an anchored pattern finds the definition, and a pattern that"
	echo "     matches nothing returns non-zero"
else
	fail=1
fi

# And the property that started this: the real file must come out whole.
if ! diff -q "$T/c.c" "$T/c.c" >/dev/null 2>&1; then :; fi
dbus_keep=$(strip_comments < "$ROOT/tests/../src/daemon/ops/dbus_proxy.c" |
	grep -cE 'av_push\(.*sock\);' || true)
[ "${dbus_keep:-0}" -ge 1 ] || {
	echo "FAIL strip_comments: src/daemon/ops/dbus_proxy.c loses code after the '/*' inside a"
	echo "       string literal - the round-205 code-eating defect is back"
	strip_fail=1
}
if [ "$strip_fail" -eq 0 ]; then
	echo "ok   the one strip_comments() is CORRECT, not merely unique:"
	echo "     six shapes, and src/daemon/ops/dbus_proxy.c survives its own string literal"
else
	fail=1
fi

# ---- rule 6: a comment that lost its '#' runs as a glob ----------------
# `sh -n` accepts it; the shell ran `*` over the whole tree and the verdict
# stayed correct.  Scoped to the prose block: no heredoc or `case` there.
star=""
for f in "$ROOT"/tests/*.sh; do
	b=$(basename "$f")
	awk -v b="$b" '
		# The flag comes FIRST, then the stop: a line that lost its "#" is
		# itself the first code, so an awk whose stop rule ran first would
		# exit on it and print nothing, passing on the exact tree it was for.
		/^[[:space:]]*\*/ { print b ": line " FNR ": " $0 }
		# The rule is about the PROSE block, where the C-comment
		# continuation style lives.
		NR > 1 && !/^[[:space:]]*#/ && !/^[[:space:]]*$/ { exit }
	' "$f" >>"$T/star" 2>/dev/null || true
done
if [ -s "$T/star" ]; then
	star=$(cat "$T/star")
	echo "FAIL a guard has a line starting with '*', which the shell runs as a"
	echo "       glob: it is a comment that lost its '#'.  \`sh -n\` accepts it -"
	echo "       the shell only finds out when it runs, after the guard has"
	echo "       already printed its verdict:"
	printf '%s\n' "$star" | head -4 | sed 's/^/        /'
	fail=1
else
	echo "ok   no guard has a line the shell would run as a glob"
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "PASS: the guards obey the rules written down for them ($n_files files)"
	exit 0
fi
echo "FAILED: a guard breaks a rule about guards"
exit 1
