#!/bin/sh
# cli-arg-parsing.sh - no command may silently accept an unknown option, or
# turn it into a name, and the global dry-run flag must never run a
# destructive command.  Runs against a scratch daemon and a throwaway HOME.
set -e

DIR=$(dirname "$0")
ROOT="$(cd "$DIR/.." && pwd)"
CLI="$ROOT/src/cli/cli.c"
. "$ROOT/tests/scratchd.sh"

BAD="--zzz-flatpkg-guard"

# "command[:extra-args]", whitespace separated: the second argument is there
# for the commands that take a pair, because without it the buggy behaviour is
# masked by an unrelated usage error.  The inventory below must cover all of them.
CASES="upgrade: install: uninstall: update: rollback: list-commits:
       remote-add:@REPOFILE@ remote-delete:
       override:com.example.Guard import: complete: ps: enter: kill: run:
       info: remove: list: exports: remote-list: remotes: history: repair:
       help:"

# Commands where a leading '-' is the user's text, not a flag.
LITERAL_DASH="search"

tmp=$(mktemp -d /tmp/.fpkg-cliparse.XXXXXX)
trap 'scratchd_stop; rm -rf "$tmp" "$psrt" 2>/dev/null' EXIT
mkdir -p "$tmp/home"
scratchd_start
FLATPKG=$SD_BIN

# Own the environment the code reads, so the host cannot move the answer:
# `ps` owns XDG_RUNTIME_DIR per case because the two shapes of "no instances"
# are different branches of cmd_ps.
XDG_CURRENT_DESKTOP=GNOME
WAYLAND_DISPLAY=wayland-0
PATH=/usr/bin:/bin:/usr/local/bin
TERM=dumb
export XDG_CURRENT_DESKTOP WAYLAND_DISPLAY PATH TERM
NO_COLOR=
FORCE_COLOR=
export NO_COLOR FORCE_COLOR

# A .flatpakrepo that parses without touching the network: remote-add writes
# the remote to <store>/repo/config and fetches nothing, so a buggy run really
# does register a remote named after the flag.  Without it the bug is invisible.
REPOFILE="$tmp/guard.flatpakrepo"
printf '[Flatpak Repo]\nUrl=https://example.invalid/repo\n' > "$REPOFILE"

fail=0
ran=0
for case in $CASES; do
	c=${case%%:*}
	extra=${case#*:}
	[ "$c" = "$extra" ] && extra=""
	case "$extra" in @REPOFILE@) extra=$REPOFILE ;; esac

	# shellcheck disable=SC2086
	out=$(HOME="$tmp/home" \
		timeout 30 "$FLATPKG" "$c" "$BAD" $extra 2>&1) && rc=0 || rc=$?

	# Strip colour before measuring: an escape-only message would pass
	# a non-empty test while telling the user nothing.
	msg=$(printf '%s' "$out" | sed 's/\x1b\[[0-9;]*m//g' | tr -d '[:space:]')
	why=""
	[ "$rc" -eq 0 ] && why="$why exits 0"
	[ -n "$msg" ] || why="$why prints nothing"
	# Exit status cannot tell a refused flag from one taken for a name, so
	# only the wording can: "I looked for this name" phrases, spelled without
	# spaces ($msg is stripped) and cut at the usage block (help says it too).
	diag=${msg%%Usage:*}
	if printf '%s' "$diag" | grep -qiE \
	    'unknownapp|notinstalled|unknownref|isnotaref|nosuchapp|isnotavalid|nodeploy'; then
		why="$why reports the flag as a name"
	fi
	ran=$((ran + 1))
	if [ -n "$why" ]; then
		echo "FAIL $c accepts $BAD:$why"
		fail=1
	fi
done

# `search` is the deliberate exception, asserted rather than omitted: upstream
# flatpak treats a leading '-' as literal text, so it must not say "unknown option".
n_literal=0
for c in $LITERAL_DASH; do
	n_literal=$((n_literal + 1))
	out=$(HOME="$tmp/home" \
		timeout 30 "$FLATPKG" "$c" "$BAD" 2>&1) && rc=0 || rc=$?
	msg=$(printf '%s' "$out" | sed 's/\x1b\[[0-9;]*m//g')
	if printf '%s' "$msg" | grep -q "unknown option"; then
		echo "FAIL $c rejects a leading '-' as an option; upstream flatpak"
		echo "       treats it as literal search text, and this guard is the"
		echo "       only thing recording that difference"
		fail=1
	fi
done
# The count, because a loop over an empty or misspelled list asserts nothing
# and prints nothing - a check with no output reads as a pass.
if [ "$n_literal" -lt 1 ]; then
	echo "FAIL no command was tested for the literal-dash behaviour"
	echo "       (LITERAL_DASH is empty: search is the one command where"
	echo "        a leading '-' is the user's text, not a flag)"
	fail=1
else
	echo "ok   $n_literal command(s) take a leading '-' as literal text"
fi

# The inventory, so the list above cannot rot: every dispatched command must
# appear in CASES or LITERAL_DASH, both directions, with a floor on each count
# (an empty list compares equal to everything).
all_cmds=$(HOME="$tmp/home" \
	timeout 30 "$FLATPKG" complete commands 2>/dev/null | sort)
listed=$(for c in $CASES; do printf '%s\n' "${c%%:*}"; done
	 for c in $LITERAL_DASH; do printf '%s\n' "$c"; done | sort -u)
n_all=$(printf '%s\n' "$all_cmds" | grep -c . || true)
n_listed=$(printf '%s\n' "$listed" | grep -c . || true)
if [ "$n_all" -lt 20 ] || [ "$n_listed" -lt 20 ]; then
	echo "FAIL the command inventory did not carve: $n_all dispatched,"
	echo "       $n_listed listed - both are expected to be over 20"
	fail=1
else
	gap=""
	for c in $all_cmds; do
		printf '%s\n' "$listed" | grep -qx "$c" || gap="$gap $c"
	done
	for c in $listed; do
		printf '%s\n' "$all_cmds" | grep -qx "$c" || gap="$gap (phantom)$c"
	done
	if [ -n "$gap" ]; then
		echo "FAIL commands with no flag case, or cases for no command:$gap"
		echo "       a command nobody runs a flag against is a command"
		echo "       whose option handling is never checked"
		fail=1
	else
		echo "ok   all $n_all dispatched commands have a flag case"
	fi
fi

# The other direction, because the check above is satisfiable by a binary that
# rejects everything: `complete` must still emit candidates on stdout, and its
# diagnostics stay on stderr - the completions read stdout directly.
if [ -f "$FLATPKG" ]; then
	n=$(HOME="$tmp/home" \
		timeout 30 "$FLATPKG" complete commands 2>/dev/null | wc -l | tr -d ' ')
	if [ "${n:-0}" -lt 20 ]; then
		echo "FAIL \`complete commands\` emitted ${n:-0} candidates; it"
		echo "       must still produce the list the completions read"
		fail=1
	else
		echo "ok   complete commands still emits $n candidates on stdout"
	fi
	leak=$(HOME="$tmp/home" \
		timeout 30 "$FLATPKG" complete bogus 2>/dev/null | wc -l | tr -d ' ')
	if [ "${leak:-0}" -ne 0 ]; then
		echo "FAIL \`complete bogus\` wrote ${leak} line(s) to stdout; its"
		echo "       diagnostic must go to stderr so the candidate"
		echo "       list stays machine-readable"
		fail=1
	fi
fi

# The flag must never have become a name: a file, a directory, a ref or
# a remote.  Checked after every case, so one leak is enough to fail.
leaked=$(find "$tmp" "$SD_STORE" -name "*$BAD*" 2>/dev/null)
if [ -s "$SD_STORE/repo/config" ] &&
   grep -q -- "$BAD" "$SD_STORE/repo/config" 2>/dev/null; then
	leaked="$leaked
$SD_STORE/repo/config (remote config names the flag)"
fi

if [ -n "$leaked" ]; then
	echo "FAIL the flag was accepted as a name - these exist because of it:"
	echo "$leaked" | sed 's/^/       /'
	fail=1
fi

if [ "$ran" -lt 10 ]; then
	echo "FAIL only $ran of 14 command cases ran; the case list is broken"
	fail=1
fi

# No exit status may be a wrapped negative: an internal -1 reaches the shell as
# 255 (what `exports` did before flatpkg_main() folded it to 1), so any that gets
# past the fold is caught here.  `enter`/`kill` are excluded: they pass one through.
esc=0
for c in exports list repair history ps remotes update rollback info \
         import override install uninstall search; do
	out=$(HOME="$tmp/home" \
		timeout 30 "$FLATPKG" "$c" 2>&1) && rc=0 || rc=$?
	esc=$((esc + 1))
	if [ "$rc" -ge 128 ] && [ "$rc" -ne 255 ]; then
		echo "NOTE $c exited $rc (128+N: a propagated signal status)"
	fi
	if [ "$rc" -eq 255 ]; then
		echo "FAIL $c exited 255: a negative return reached the shell."
		echo "       flatpkg_main() must fold a negative rc to 1."
		fail=1
	fi
done
if [ "$esc" -lt 10 ]; then
	echo "FAIL only $esc exit-status cases ran"
	fail=1
else
	echo "ok   no command's exit status is a wrapped negative ($esc checked)"
fi

# `ps` must speak when nothing runs and `--columns=` must stay silent.  Assert
# both branches - instances dir absent, and present but empty: only the second
# honoured --columns=.  XDG_RUNTIME_DIR is owned so the host cannot decide.
psrt=$(mktemp -d /tmp/.fpkg-psrt.XXXXXX)
mkdir -p "$psrt/home"
psrun() {
	# $1: "absent" (no instances dir) or "present" (dir exists, empty)
	rm -rf "$psrt/run"
	# The dot matters: instances_dir() appends .flatpkg to XDG_RUNTIME_DIR, and
	# creating plain flatpkg left the present shape still absent, so both cases
	# took the same branch.  Planting each branch is what proves them independent.
	mkdir -p "$psrt/run"
	[ "$1" = present ] && mkdir -p "$psrt/run/.flatpak"
	HOME="$psrt/home" \
		XDG_RUNTIME_DIR="$psrt/run" \
		timeout 30 "$FLATPKG" ps "${2:-}" 2>&1 |
		sed 's/\x1b\[[0-9;]*m//g' | tr -d '[:space:]'
}
ps_cases=0
for shape in absent present; do
	ps_cases=$((ps_cases + 1))
	if [ -z "$(psrun "$shape")" ]; then
		echo "FAIL \`flatpkg ps\` printed nothing with no instances running"
		echo "       (instances dir $shape): the default form must say so"
		echo "       rather than look broken"
		fail=1
	fi
	if [ -n "$(psrun "$shape" --columns=instance)" ]; then
		echo "FAIL \`flatpkg ps --columns=\` is not silent with no instances"
		echo "       (instances dir $shape): that form is machine-readable"
		echo "       and an empty list is the correct answer"
		fail=1
	fi
done
# The count, because a loop over an empty list asserts nothing and prints
# nothing - a check with no output reads as a pass.
if [ "$ps_cases" -ne 2 ]; then
	echo "FAIL only $ps_cases of the 2 instances-dir shapes were tested"
	fail=1
else
	echo "ok   ps speaks and --columns= stays silent, with the instances"
	echo "     directory absent and present"
fi

# --simulate is a plan: install, uninstall, update, upgrade and repair show the
# delta and stop before touching the store.  Commands with no plan (rollback,
# import, remote-*, override) refuse it rather than do the real thing.
list=$(awk '/no_simulate\[\] *= *\{/ {inb=1; next}
             inb { print; if (/\}/) inb=0 }' "$CLI" |
	grep -oE '"[a-z][a-z-]*"' | tr -d '"' | sort -u)

if [ -z "$list" ]; then
	echo "FAIL could not find the --simulate no_simulate[] table in src/cli/cli.c"
	fail=1
else
	dispatched=$(awk '/const cli_cmd_t commands\[\] *= *\{/ {inb=1; next}
	         inb && /^\};/ {inb=0} inb' "$CLI" |
		grep -oE '\{ *CMD_[A-Z_]+, *"[a-z][a-z-]*"(, *"[a-z][a-z-]*")?' |
		grep -oE '"[a-z][a-z-]*"' | tr -d '"' | sort -u)
	typo=""
	for c in $list; do
		echo "$dispatched" | grep -qx -- "$c" || typo="$typo $c"
	done
	if [ -n "$typo" ]; then
		echo "FAIL --simulate refuses a name the command table does not carry:"
		for c in $typo; do echo "       $c"; done
		fail=1
	else
		echo "ok   --simulate refuses only real commands ($(echo "$list" |
			wc -l | tr -d ' ') of them)"
	fi
fi

SIM_PLANNED="install update uninstall remove upgrade repair"
SIM_REFUSED="rollback import remote-add remote-delete override"
sim_missing=""
sim_extra=""
for c in $SIM_REFUSED; do
	echo "$list" | grep -qx -- "$c" || sim_missing="$sim_missing $c"
done
for c in $list; do
	echo "$SIM_REFUSED" | tr ' ' '\n' | grep -qx -- "$c" || sim_extra="$sim_extra $c"
done
if [ -n "$sim_missing" ]; then
	echo "FAIL these commands have no plan and must refuse --simulate:"
	for c in $sim_missing; do echo "       $c"; done
	fail=1
fi
if [ -n "$sim_extra" ]; then
	echo "FAIL the --simulate refusal table lists a command that plans:"
	for c in $sim_extra; do echo "       $c"; done
	fail=1
fi

sim_refused=0
sim_tested=0
for c in $list; do
	sim_tested=$((sim_tested + 1))
	out=$(HOME="$tmp/home" \
		timeout 30 "$FLATPKG" -n "$c" 2>&1) && rc=0 || rc=$?
	msg=$(printf '%s' "$out" | sed 's/\x1b\[[0-9;]*m//g')
	if [ "$rc" -ne 0 ] && printf '%s' "$msg" | grep -q 'simulate'; then
		sim_refused=$((sim_refused + 1))
	else
		echo "FAIL -n $c is in the refusal table but did not refuse"
		echo "       rc=$rc; a destructive command here runs for real"
		fail=1
	fi
done
if [ "$sim_tested" -lt 5 ]; then
	echo "FAIL only $sim_tested --simulate table entries were tested"
	fail=1
else
	echo "ok   all $sim_refused commands in the --simulate table refuse it"
fi

# The other direction: a command with a plan must not be refused.  Run it
# with no operands so nothing can be installed or removed; the claim is the
# diagnostic, not the exit status.
sim_planned=0
for c in $SIM_PLANNED; do
	out=$(HOME="$tmp/home" \
		timeout 30 "$FLATPKG" -n "$c" 2>&1) && rc=0 || rc=$?
	msg=$(printf '%s' "$out" | sed 's/\x1b\[[0-9;]*m//g')
	if printf '%s' "$msg" | grep -q 'simulate is not implemented'; then
		echo "FAIL -n $c still refuses --simulate"
		fail=1
	else
		sim_planned=$((sim_planned + 1))
	fi
done
if [ "$sim_planned" -lt 6 ]; then
	echo "FAIL only $sim_planned planned commands accepted --simulate"
	fail=1
else
	echo "ok   $sim_planned planned commands accept --simulate"
fi

# The mirror: an argument must not be taken for a COMMAND.  Scanning every
# argument for a service word meant `flatpkg run APP portal` started the portal
# instead of the app - the word is the app's own argument.  Asserted on scope.
entry_hits=0
entry_tested=0
for w in daemon portal session-helper system-helper; do
	entry_tested=$((entry_tested + 1))
	log="$tmp/entry-$w.log"
	rm -f "$log"
	# A bus address that cannot work: a service that starts anyway must
	# fail here rather than sit in the foreground holding a name.
	DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
	HOME="$tmp/home" \
		timeout 30 "$FLATPKG" -l "$log" run org.example.NoSuch "$w" \
		>/dev/null 2>&1 || true
	if grep -qE 'scope=(daemon|portal|session|syshelper)( |$)' "$log"; then
		echo "FAIL 'run APP $w' entered the $w service instead of running"
		echo "       the app; the word is the caller's own argument"
		entry_hits=$((entry_hits + 1))
	fi
	grep -qE 'scope=(run|cli|store)( |$)' "$log" || {
		echo "FAIL 'run APP $w' never reached the run path at all:"
		# No log at all is the normal shape when a service was entered: it
		# fails before the logger is up, and its bus error goes to the
		# terminal, not to -l.  Say so rather than let sed complain.
		if [ -s "$log" ]; then
			sed 's/^/       /' "$log"
		else
			echo "       (nothing was logged at all - a service entry"
			echo "        most likely, which is the regression)"
		fi
		entry_hits=$((entry_hits + 1))
	}
done
if [ "$entry_tested" -ne 4 ]; then
	echo "FAIL only $entry_tested of the 4 service entry points were tested"
	fail=1
elif [ "$entry_hits" -eq 0 ]; then
	echo "ok   an app's own argument is never taken for a service entry point"
else
	fail=1
fi

# ... and one level up: an app's argument taken for a global option.  Before
# the one-loop parser `run APP --version` printed a version, exited 0 and never
# launched, and `-- --version` too.  A bare `--` always worked, so it is not tested.
argpath_log() {
	name=$1
	shift
	rm -f "$tmp/arg-$name.log"
	DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent \
	HOME="$tmp/home" \
		timeout 30 "$FLATPKG" -l "$tmp/arg-$name.log" run org.example.NoSuch "$@" \
		>/dev/null 2>&1 || true
	# The `|| true` is load-bearing: `run` is expected to fail here and
	# errexit fires inside a function as at top level, so the script died
	# with no output.  An expected failure is neutralised where it happens.
}

arg_hits=0
arg_cases=0
for w in --version --help -h; do
	arg_cases=$((arg_cases + 1))
	argpath_log "$w" "$w"
	if ! grep -qE 'scope=(run|cli|store)( |$)' "$tmp/arg-$w.log"; then
		echo "FAIL 'run APP $w' never reached the run path: the global"
		echo "       parser claimed the option instead of the command"
		arg_hits=$((arg_hits + 1))
	fi
done
arg_cases=$((arg_cases + 1))
argpath_log sep -- --version
if ! grep -qE 'scope=(run|cli|store)( |$)' "$tmp/arg-sep.log"; then
	echo "FAIL 'run APP -- --version' never reached the run path: the '--'"
	echo "       separator is the documented way to pass an option to the app"
	arg_hits=$((arg_hits + 1))
fi
if [ "$arg_cases" -ne 4 ]; then
	echo "FAIL only $arg_cases of the 4 app-argument cases were run"
	fail=1
elif [ "$arg_hits" -eq 0 ]; then
	echo "ok   a run's own --version/--help/-h/-- --version reach the app"
fi

# `--user -v list`: a short global behind a long one fell through to usage()
# and exit 1.  rc alone says it, but the store must EXIST: with a missing store
# root `list` exits 1 too, and this would measure the store instead.
order_rc() {
	HOME="$tmp/home" \
		timeout 30 "$FLATPKG" "$@" >/dev/null 2>&1 && echo 0 || echo $?
}

rc=$(order_rc --user -v list)
if [ "$rc" -ne 0 ]; then
	echo "FAIL '--user -v list' exits $rc: a short global behind a long one"
	echo "       is not recognised, so it was dispatched on as a command"
	fail=1
else
	echo "ok   a short global is recognised behind a long one"
fi

# `-u NAME` must keep its value wherever the option appears, or the
# installation check downstream checks a word nobody wrote.  The control is the
# point: a missing store refuses both values and the pair proves nothing.
rc=$(order_rc --user -u default list)
if [ "$rc" -ne 0 ]; then
	echo "FAIL the control '--user -u default list' exits $rc, so the"
	echo "       refusal below would be about the environment, not -u"
	fail=1
elif [ "$(order_rc --user -u bogus list)" = 0 ]; then
	echo "FAIL '--user -u bogus list' exits 0: -u's value was dropped, so an"
	echo "       installation that does not exist was accepted silently"
	fail=1
else
	echo "ok   -u keeps its value wherever it appears (--user -u bogus refused)"
fi

# `run` has TWO parsers walking the same argv, and one typo must produce one
# diagnostic: the client pre-scan used to share parse_run_option()'s logging.
# Counted on the terminal, not with -l: the pre-scan logs before logfmt_open().
diag_count() {
	HOME="$tmp/home" \
		timeout 30 "$FLATPKG" run "$@" >"$tmp/diag.out" 2>&1 || true
	grep -c 'unknown option' "$tmp/diag.out" 2>/dev/null || true
}

# The bad option goes BEFORE the app id: after it, `--zzz` is the app's own
# argument and the run reaches the sandbox, so that position would measure
# something else entirely.
n=$(diag_count --zzz org.example.NoSuch)
if [ "$n" -ne 1 ]; then
	echo "FAIL 'run --zzz APP' said 'unknown option' $n times, want 1:"
	echo "       run is parsed twice (client pre-scan + dispatch) and the"
	echo "       client-side walk must not report what the real parse reports"
	fail=1
else
	echo "ok   one bad run option produces exactly one diagnostic"
fi

# The separator case is BEFORE the app id, the only position the pre-scan
# walks; after it `run APP --` stops at the app id and the store says
# "unknown app", which is a correct diagnostic and not what this is about.
n=$(diag_count -- org.example.NoSuch)
if [ "$n" -ne 0 ]; then
	echo "FAIL 'run -- APP' called the '--' separator an unknown option"
	echo "       ($n): the client-side pre-scan parses it as an option"
	fail=1
else
	echo "ok   the '--' separator is not reported as an unknown option"
fi

if [ "$fail" -eq 0 ]; then
	echo "PASS: all $ran commands reject an option they do not accept"
	exit 0
fi
echo "FAILED: the CLI silently accepts things it should refuse"
exit 1
