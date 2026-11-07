#!/bin/sh
# tty-raw-input.sh - the app gets the caller's keystrokes unedited.
# The client relays its stdin to the app's pty, so while it runs, the line
# discipline must not translate: ICRNL delivers Enter as LF, which a TUI
# reads as Ctrl-J (helix's ":" prompt typed a "j" on every Enter), and IXON
# swallows Ctrl-Q.  The app's output still comes back postprocessed, so this
# is an input-only mask, not cfmakeraw().
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$(dirname "$0")/commentlib.sh"
fail=0

note() {
	echo "FAIL $*"
	fail=1
}

# The setting statement can wrap, so join it up to its semicolon.
field() {
	printf '%s\n' "$1" | awk -v f="$2" '
		index($0, f) { on = 1 }
		on { printf "%s ", $0 }
		on && /;/ { exit }'
}

echo "checking the client's raw terminal mode"
echo

body=$(strip_comments < "$ROOT/src/cli/client.c" |
	carve_function '^client_enter_raw_stdin' "$ROOT/src/cli/client.c" ||
	true)
if [ -z "$body" ]; then
	note "could not carve client_enter_raw_stdin() out of src/cli/client.c"
fi

# Input: the translations are off.  A missing ICRNL is the Enter-as-Ctrl-J bug.
iflag=$(field "$body" 't.c_iflag')
for flag in ICRNL IXON; do
	case "$iflag" in
	*"$flag"*) : ;;
	*) note "the raw stdin iflag mask does not clear $flag" ;;
	esac
done

# Local control: no echo and no line editing while the app owns the terminal.
lflag=$(field "$body" 't.c_lflag')
for flag in ICANON ECHO; do
	case "$lflag" in
	*"$flag"*) : ;;
	*) note "the raw stdin lflag mask does not clear $flag" ;;
	esac
done

# The restore path: the saved settings give the shell its prompt back.
saved=$(field "$body" 'g_saved_term.c_lflag')
for flag in ICANON ECHO ISIG; do
	case "$saved" in
	*"$flag"*) : ;;
	*) note "the saved termios does not restore $flag" ;;
	esac
done

# Output: still postprocessed.  Clearing OPOST stairs the app's text on a
# terminal that does not send CR itself.
oflag=$(field "$body" 't.c_oflag')
if [ -n "$oflag" ]; then
	note "the raw mode touches t.c_oflag, so output is no longer postprocessed"
fi

if [ "$fail" -eq 0 ]; then
	echo "ok   raw stdin clears ICRNL and IXON, and keeps output postprocessing"
	echo "ok   the saved termios goes back to a prompt's settings"
	echo
	echo "PASS: the app sees the bytes the caller typed"
	exit 0
fi
echo
echo "FAILED: the line discipline edits the app's input"
exit 1
