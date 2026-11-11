#!/bin/sh
# helper-config.sh - the D-Bus command map is parsed without a shell and
# only an absolute program word is accepted.  It is how a daemon-only
# installation runs a client it knows nothing about.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d /tmp/.fpkg-helpercfg.XXXXXX)
trap 'rm -rf "$T"' EXIT INT TERM
fail=0
note() { echo "FAIL: $*"; fail=1; }

[ -f "$ROOT/src/shared/config.c" ] || { echo "SKIP: no config.c"; exit 0; }
cc -O2 -Wall -Wextra -I"$ROOT/include" -DSYSCONFDIR='"/nonexistent"' \
	-o "$T/helper-config" "$ROOT/tests/helper-config.c" \
	"$ROOT/src/shared/config.c" 2>"$T/cc.log" || {
	echo "FAIL: the config harness does not build:"
	cat "$T/cc.log"
	exit 1
}
export FPKG_TEST_STORE="$T/store"
mkdir -p "$FPKG_TEST_STORE"

cat > "$FPKG_TEST_STORE/helper.conf" <<'CONF'
uninstall = /bin/echo uninstall -y -- %r %i %b %n %f %p %s %P %c %u %k %v %S %G %d
CONF
out=$("$T/helper-config")
printf '%s\n' "$out" | grep -q '^mapped=1 argc=19$' ||
	note "the uninstall command did not parse: $out"
printf '%s\n' "$out" | grep -q '^arg0=/bin/echo$' ||
	note "the program word was not kept: $out"
printf '%s\n' "$out" | grep -q '^arg5=default$'
printf '%s\n' "$out" | grep -q '^arg6=/tmp/example.flatpak$' ||
	note "%b was not substituted: $out"
printf '%s\n' "$out" | grep -q '^arg8=7$' ||
	note "%f was not substituted: $out"
printf '%s\n' "$out" | grep -q '^arg9=/tmp/repo$' ||
	note "%p was not substituted: $out"
printf '%s\n' "$out" | grep -q '^arg10=a,b$' ||
	note "%s was not substituted: $out"
printf '%s\n' "$out" | grep -q '^arg11=old$' ||
	note "%P was not substituted: $out"
printf '%s\n' "$out" | grep -q '^arg12=cfg$' ||
	note "%c was not substituted: $out"
printf '%s\n' "$out" | grep -q '^arg13=https://example.invalid/repo$' ||
	note "%u was not substituted: $out"
printf '%s\n' "$out" | grep -q '^arg14=languages$' ||
	note "%k was not substituted: $out"
printf '%s\n' "$out" | grep -q '^arg15=en$' ||
	note "%v was not substituted: $out"
printf '%s\n' "$out" | grep -q '^arg16=/tmp/summary$' ||
	note "%S was not substituted: $out"
printf '%s\n' "$out" | grep -q '^arg17=/tmp/sig$' ||
	note "%G was not substituted: $out"
printf '%s\n' "$out" | grep -q '^arg18=/tmp/src$' ||
	note "%d was not substituted: $out" ||
	note "%i was not substituted: $out"

# A placeholder inside a word, which is how the shipped map spells
# --remote=%o: substituting only a token that is exactly one left the entry
# inert and passed the literal word to the tool. */
cat > "$FPKG_TEST_STORE/helper.conf" <<'CONF'
uninstall = /bin/echo --repo=%p --remote=%n %r
CONF
out=$("$T/helper-config")
printf '%s\n' "$out" | grep -q '^arg1=--repo=/tmp/repo$' ||
	note "a placeholder inside a word was not substituted: $out"
printf '%s\n' "$out" | grep -q '^arg2=--remote=flathub$' ||
	note "a placeholder at the end of a word was not substituted: $out"
printf '%s\n' "$out" | grep -q '^arg3=app/org.example.App/x86_64/stable$' ||
	note "a whole-token placeholder stopped being substituted: $out"

cat > "$FPKG_TEST_STORE/helper.conf" <<'CONF'
uninstall =
CONF
out=$("$T/helper-config")
printf '%s\n' "$out" | grep -q '^mapped=0 argc=0$' ||
	note "an empty entry was not reported as unconfigured: $out"

cat > "$FPKG_TEST_STORE/helper.conf" <<'CONF'
uninstall = relative-cli uninstall %r
CONF
if "$T/helper-config" >/dev/null; then
	note "a relative program word was accepted"
fi

cat > "$FPKG_TEST_STORE/helper.conf" <<'CONF'
other = /bin/true
CONF
out=$("$T/helper-config")
printf '%s\n' "$out" | grep -q '^mapped=0' ||
	note "an unrelated entry did not fall back: $out"

if [ "$fail" -eq 0 ]; then
	echo "PASS: helper.conf maps a D-Bus request to an absolute argv,"
	echo "      substitutes a placeholder wherever it appears in a word, and"
	echo "      reports an empty entry"
fi
exit $fail
