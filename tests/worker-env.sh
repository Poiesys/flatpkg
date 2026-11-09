#!/bin/sh
# worker-env.sh - every FLAT(PKG|PAK)_* the tree reads is CLASSIFIED, and the classes are
# the daemon's to enforce.  `daemon.c` replays the caller's environment while the worker
# runs as root, so a name the tree reads and the register omits fails.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/src"
fail=0

echo "worker environment: every FLAT(PKG|PAK)_* the tree names is classified"
echo

# ---------------------------------------------------------------------------
# The register.  Two classes, and a name in neither is a failure.  The lists hold BARE
# NAMES and nothing else: reasons inside the quoted lists are word-split into the
# comparison (`grep -qx "--devel"` dies), so they live here.
#
# DAEMON_OWNED - from the daemon, never the request; checked against daemon.c's own
# setenv/unsetenv.
#   APP_UID/GID/USER/HOME  from getpeereid(), which the client cannot forge
#   APP_TTY/APP_MASTER     the pty the daemon opened; cleared when it has
#                          none, so a caller's export cannot survive
DAEMON_OWNED="FLATPKG_APP_UID FLATPKG_APP_GID FLATPKG_APP_USER
              FLATPKG_APP_HOME FLATPKG_APP_TTY FLATPKG_APP_MASTER"

# CALLER_OK - a caller may set these.  This half is a *record*, not a check: no
# guard can prove a name is harmless, only that a new one cannot appear unremarked.
#   STORE             ignored when euid==0, so the store root is not
#                     caller-redirectable (measured through the daemon and
#                     on the direct path)
#   DAEMON            a client-side switch; the worker is past it
#   LOGFMT            the caller's own log destination, made safe by
#                     refusing anything not already theirs
#   SIMULATE          a per-request dry run, refused by dispatch for
#                     every command that cannot honour it
#   FORWARD_FDS       names where the fds the client actually sent land;
#                     bounds-checked in apply_forwarded_fds()
#   DEBUG_KEEP        leaves the jail up for inspection
#   BUILD_TIMING      timings only
#   NO_SHIMS          drops the seccomp filter for the caller's own app
#   SECCOMP_DEVEL     --devel parity; read by the shim inside the app
#   SECCOMP_ALLOW_*   set by features=canbus/bluetooth; the shim again
#   *_SHIM_TRACE      trace what a compatibility shim intercepts; read by
#                     the shim, which runs as the app's user
#   OSTREE_VERBOSE    only makes the real ostree chatty; the tool's path
#                     is still the store's
#   VERBOSE           turns on the debug-level chatter; the logger
#                     filters it before formatting, so a caller setting
#                     it costs a read, not a side effect
#   INTERACTIVE       set by the daemon itself
#   SPAWN_NONCE       internal portal plumbing
#
# **Two names here are spelled `FLATPAK_`, not `FLATPKG_`** - upstream flatpak's
# own, read for compatibility (see `read_names`'s pattern):
#   GL_DRIVERS        a `:`-separated list of driver NAMES, compared with
#                     strcmp() against a basename and never used to build a
#                     path.  Mirrors flatpak_get_gl_drivers()
#                     (flatpak-context.c), so a caller overriding it is
#                     overriding flatpkg's own default
#   DBUSPROXY         the xdg-dbus-proxy to run: the exec is unprivileged
#                     BECAUSE dbus_start_proxies()'s child checks initgroups()/
#                     setgid()/setuid() and refuses to exec as root if any fails,
#                     and tests/shell-calls.sh asserts that ordering
CALLER_OK="FLATPKG_STORE FLATPKG_LOGFMT FLATPKG_VERBOSE
           FLATPKG_PROFILE
           FLATPKG_SIMULATE
           FLATPKG_FORWARD_FDS FLATPKG_DEBUG_KEEP
           FLATPKG_NO_SHIMS FLATPKG_SECCOMP_DEVEL FLATPKG_SECCOMP_ALLOW_CAN
           FLATPKG_SECCOMP_ALLOW_BT FLATPKG_DEVT_SHIM_TRACE
           FLATPKG_MEMFD_SHIM_TRACE FLATPKG_RLIMIT_SHIM_TRACE
           FLATPKG_MADVISE_SHIM_TRACE
           FLATPKG_SYNCOBJ_SHIM_TRACE FLATPKG_OSTREE_VERBOSE
           FLATPKG_INTERACTIVE FLATPKG_SPAWN_NONCE
           FLATPAK_GL_DRIVERS FLATPAK_DBUSPROXY
           FLATPKG_FILECHOOSER_TIMEOUT"

# WORKER_OWNED - the third class.  Not read by the worker and not set by `daemon.c`:
# `sandbox_exec()` **writes them into the app's** environment after `jail_attach()`, so
# a value the caller exported is overwritten before the app sees it.  To DAEMON_OWNED
# would be a lie the "stray" check catches.
#   FLATPAK_ID          the app id; also interpolated into $PS1, so the
#                       app's prompt reads "[app $FLATPAK_ID ~/...]"
#   FLATPAK_INFO        /run/.flatpak-info inside the jail
#   FLATPAK_USER_DIR    the per-app writable store, which sandbox.c
#                       exports and the app's data dirs hang off
WORKER_OWNED="FLATPAK_ID FLATPAK_INFO FLATPAK_USER_DIR"


# The same register, for the names that are NOT ours: every non-FLATPKG_ variable the code
# reads, and which guard that runs the binary owns it in.  Owning a name means the guard
# sets it on every invocation, so the host cannot move the verdict - PATH included,
# because the code resolves program names through it.  NO_COLOR and FORCE_COLOR are in it
# because a second list of "these two are fine" is a list to forget.
NONFLAT_READ="HOME XDG_RUNTIME_DIR XDG_CURRENT_DESKTOP
               DBUS_SESSION_BUS_ADDRESS WAYLAND_DISPLAY PATH TERM
               NO_COLOR FORCE_COLOR"

# The guards that invoke build/flatpkg, and the names each deliberately does NOT own; an
# empty value means "owns all of them".
#
# The exceptions are all one sentence - *the subject of this guard IS the caller's own
# session* - so they are recorded rather than fixed by pinning: logfmt-path.sh tests the
# daemon receiving the caller's environment, sandbox-launch.sh launches in the session
# the app has to start in, and home-persists.sh is about the home the daemon resolves
# from the passwd entry rather than from HOME.  Checked both ways: the count may not
# drift, and every entry must name a guard that really does invoke the binary.
GUARD_ENV_OWNED_ALL="cli-arg-parsing.sh exports-collision.sh
                     import-visible.sh remote-name-path.sh
                     rollback-commit-ref.sh fplog.sh"
GUARD_ENV_EXCEPT="logfmt-path.sh sandbox-launch.sh apps-visual.sh
                  home-persists.sh"

# Strip comments before grepping, or the names are matched in the comment that
# explains the rule.  commentlib.sh is sourced, never run - one copy, not five.
. "$(dirname "$0")/commentlib.sh"
tmp=$(mktemp -d /tmp/.fpkg-workerenv.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

for f in "$SRC"/*.c "$SRC"/*/*.c "$SRC"/*/*/*.c; do strip_comments < "$f" > "$tmp/$(basename "$f")"; done

# What the tree reads, and what the daemon does about each name.  The
# APP_* family for the first check; every named FLAT(PKG|PAK)_* for the
# classification below.
app_names=$(grep -hoE 'getenv\("FLATPKG_APP_[A-Z_]+"\)' "$tmp"/*.c |
	grep -oE 'FLATPKG_APP_[A-Z_]+' | sort -u)
# The trailing quote is deliberately NOT in the pattern: `FLATPKG_SPAWN_NONCE` is
# matched as `strncmp(a, "FLATPKG_SPAWN_NONCE=", 20)`, so the literal ends in `=`
# and a carve wanting the closing quote reports a false "phantom".
read_names=$(grep -hoE '"FLAT(PKG|PAK)_[A-Z0-9_]+' "$tmp"/*.c |
	grep -oE 'FLAT(PKG|PAK)_[A-Z0-9_]+' | sort -u)
# **Both spellings, because the tree reads both**: a register that matches only the
# project's own prefix is a register of the project's own names, and FLATPAK_DBUSPROXY
# names the program `src/shared/dbus.c` execs.  `getenv("...")` and not a bare string
# literal, which finds 24 names, most of them log messages.
host_names=$(grep -hoE 'getenv\("(HOME|XDG_[A-Z_]+|PATH|TERM|NO_COLOR|FORCE_COLOR|WAYLAND_DISPLAY|DBUS_SESSION_BUS_ADDRESS)"\)' "$tmp"/*.c |
	grep -oE '(HOME|XDG_[A-Z_]+|PATH|TERM|NO_COLOR|FORCE_COLOR|WAYLAND_DISPLAY|DBUS_SESSION_BUS_ADDRESS)' | sort -u)
# Centralised reader: host names also appear as quoted literals in cli.c's
# env_vars[] table, so fold those in (deduped).
host_names=$(printf '%s\n' "$host_names" $(grep -hoE '"(HOME|XDG_[A-Z_]+|PATH|TERM|NO_COLOR|FORCE_COLOR|WAYLAND_DISPLAY|DBUS_SESSION_BUS_ADDRESS)"' "$tmp"/cli.c | tr -d '"' | sort -u) | sort -u)
n_app=$(printf '%s\n' "$app_names" | grep -c . || true)
n_read=$(printf '%s\n' "$read_names" | grep -c . || true)

# `[^a-z]` in front of `setenv` on purpose: **`unsetenv` contains `setenv`**, and
# unanchored the occurrence counts below read one `unsetenv` line as a second
# `setenv` and report "the carve broke".
set_names=$(grep -hoE '[^a-z]setenv\("FLATPKG_[A-Z0-9_]+' "$tmp/daemon.c" "$tmp/typed_worker.c" |
	grep -oE 'FLATPKG_[A-Z0-9_]+' | sort -u)
clear_names=$(grep -hoE '[^a-z]unsetenv\("FLATPKG_[A-Z0-9_]+' "$tmp/daemon.c" "$tmp/typed_worker.c" |
	grep -oE 'FLATPKG_[A-Z0-9_]+' | sort -u)
n_set=$(printf '%s\n' "$set_names" | grep -c . || true)
n_clear=$(printf '%s\n' "$clear_names" | grep -c . || true)

# Floors, because an empty list compares equal to everything - and this
# guard is worthless if the carves stop matching and say so by agreeing.
if [ "$n_app" -lt 5 ] || [ "$n_set" -lt 4 ]; then
	echo "FAIL the carve found $n_app APP_* names read and $n_set set by"
	echo "       the daemon (expected at least 5 and 4): a carve that"
	echo "       returns nothing is not a guard, it is a compliment"
	fail=1
else
	unowned=""
	for n in $app_names; do
		if ! printf '%s\n' "$set_names" | grep -qx "$n" &&
		   ! printf '%s\n' "$clear_names" | grep -qx "$n"; then
			unowned="$unowned $n"
		fi
	done
	if [ -n "$unowned" ]; then
		echo "FAIL read by the worker with no daemon counterpart:$unowned"
		echo "       the caller sets its value (the daemon replays the"
		echo "       caller's environment) and the worker runs as root"
		fail=1
	else
		echo "ok   all $n_app FLATPKG_APP_* the tree reads are set or"
		echo "     cleared by the daemon ($n_set set, $n_clear cleared)"
	fi
fi

# The second half: a name the daemon only sets in *one* branch must be cleared in
# the other.  An `if (master >= 0)` that sets TTY and MASTER with no else is the
# exact defect, so it is checked as one.
tty_set=$(grep -hE '[^a-z]setenv\("FLATPKG_APP_TTY"' "$tmp/daemon.c" "$tmp/typed_worker.c" | grep -c . || true)
tty_clear=$(grep -hE '[^a-z]unsetenv\("FLATPKG_APP_TTY"\)' "$tmp/daemon.c" "$tmp/typed_worker.c" | grep -c . || true)
mst_set=$(grep -hE '[^a-z]setenv\("FLATPKG_APP_MASTER"' "$tmp/daemon.c" "$tmp/typed_worker.c" | grep -c . || true)
mst_clear=$(grep -hE '[^a-z]unsetenv\("FLATPKG_APP_MASTER"\)' "$tmp/daemon.c" "$tmp/typed_worker.c" | grep -c . || true)
if [ "$tty_set" -ne 1 ] || [ "$mst_set" -ne 1 ]; then
	echo "FAIL expected exactly one setenv for APP_TTY and APP_MASTER,"
	echo "       found $tty_set and $mst_set - the carve broke, or there"
	echo "       are now two places that set them"
	fail=1
elif [ "$tty_clear" -lt 1 ] || [ "$mst_clear" -lt 1 ]; then
	echo "FAIL APP_TTY/APP_MASTER are set but never cleared: when the"
	echo "       client has no pty the caller's own value survives and"
	echo "       sandbox_exec() dup2's it onto the app's 0/1/2"
	fail=1
else
	echo "ok   the pty descriptors are set in one branch and cleared in"
	echo "     the other, so a caller-exported value cannot survive"
fi

# The classification itself: every FLAT(PKG|PAK)_* the tree names must be in one of the
# lists above, and nothing may be in a list that no code names.  The carve is **string
# literals**, not the bare identifier, which also returns enums and SHIM_TRACE names.
classified=$(printf '%s\n' $DAEMON_OWNED $CALLER_OK $WORKER_OWNED | sort -u)
n_class=$(printf '%s\n' "$classified" | grep -c . || true)
unclassified=""
for n in $read_names; do
	printf '%s\n' "$classified" | grep -qx "$n" || unclassified="$unclassified $n"
done
phantom=""
for n in $classified; do
	printf '%s\n' "$read_names" | grep -qx "$n" || phantom="$phantom $n"
done
if [ "$n_class" -lt 15 ] || [ "${#read_names}" -lt 15 ]; then
	echo "FAIL the classification did not carve: $n_read named in the tree,"
	echo "       $n_class in the register (both expected over 15)"
	fail=1
elif [ -n "$unclassified" ]; then
	echo "FAIL FLAT(PKG|PAK)_ name(s) the tree names but the register does"
	echo "       not:$unclassified"
	echo "       every name a caller can set needs a decision recorded: is"
	echo "       it the daemon's, or may a caller set it?"
	fail=1
elif [ -n "$phantom" ]; then
	echo "FAIL register entr(y|ies) for names no code reads:$phantom"
	echo "       a register entry nothing reads is a decision about a"
	echo "       name that does not exist, which reads as coverage"
	fail=1
else
	echo "ok   all $n_read FLAT(PKG|PAK)_* names the tree names are classified:"
	echo "     $(printf '%s\n' $DAEMON_OWNED | grep -c . || true) daemon-owned,"
	echo "     the rest caller-settable with a stated reason"
fi

# Every DAEMON_OWNED name must be set or cleared by daemon.c - the register's claim,
# checked rather than trusted.
stray=""
for n in $DAEMON_OWNED; do
	if ! printf '%s\n' "$set_names" | grep -qx "$n" &&
	   ! printf '%s\n' "$clear_names" | grep -qx "$n"; then
		stray="$stray $n"
	fi
done
if [ -n "$stray" ]; then
	echo "FAIL register says the daemon owns these, but daemon.c does not"
	echo "       set or clear them:$stray"
	echo "       a caller can set any name the worker does not re-set"
	fail=1
else
	echo "ok   every name the register calls the daemon's is set or cleared"
	echo "     by daemon.c, so the caller's copy is overwritten"
fi

# And the third class, checked rather than asserted: a WORKER_OWNED name must be
# `setenv()`ed somewhere in the tree and NOT also `getenv()`ed, because a name that
# is both read and written depends on which half of the launch asks.  Floor 3.
n_worker=$(printf '%s\n' $WORKER_OWNED | grep -c . || true)
worker_bad=""
worker_read=""
for n in $WORKER_OWNED; do
	grep -qE "[^a-z]setenv\(\"$n\"" "$tmp"/*.c || worker_bad="$worker_bad $n"
	grep -qE "getenv\(\"$n\"\)" "$tmp"/*.c && worker_read="$worker_read $n"
done
if [ "$n_worker" -lt 3 ]; then
	echo "FAIL WORKER_OWNED has $n_worker member(s), below the floor of 3 -"
	echo "       the class exists to hold names the worker writes into the"
	echo "       app's environment, and an empty one is an empty list"
	fail=1
elif [ -n "$worker_bad" ]; then
	echo "FAIL register says the worker overwrites these for the app, but"
	echo "       no setenv() in src/ writes them:$worker_bad"
	echo "       a caller's exported copy would then reach the app"
	fail=1
elif [ -n "$worker_read" ]; then
	echo "FAIL name(s) classified as write-only are also read:$worker_read"
	echo "       sandbox_exec() runs after the worker's own getenv()s, so"
	echo "       the value depends on which half of the launch asks"
	fail=1
else
	echo "ok   all $n_worker WORKER_OWNED names are setenv()ed for the app by"
	echo "     the worker and read by nothing, so the caller's copy is gone"
fi

# And the ownership half: a guard that runs the binary owns every name above, so
# the host cannot move its verdict.  `export NAME=` and a per-invocation `NAME=`
# both count, and *where* it appears does not matter - only that it is pinned.
n_host=$(printf '%s\n' $NONFLAT_READ | grep -c . || true)
n_carved=$(printf '%s\n' "$host_names" | grep -c . || true)
if [ "$n_host" -lt 5 ] || [ "$n_carved" -lt 5 ]; then
	echo "FAIL the host-environment carve found $n_carved names in the tree"
	echo "       against $n_host in the register (both expected over 5):"
	echo "       an empty carve makes the ownership check vacuous"
	fail=1
else
	unowned=""
	unguarded=0
	seen_except=""
	for f in "$ROOT"/tests/*.sh; do
		b=$(basename "$f")
		# Only guards that actually INVOKE the binary: mentioning the path in a
		# comment or a `FLATPKG=` assignment is not invoking it.  An invocation
		# is the variable *quoted* on a command line.
		grep -q 'FLATPKG=".*build/flatpkg"' "$f" || continue
		grep -q '"\$FLATPKG"' "$f" || continue
		skip=no
		for e in $GUARD_ENV_EXCEPT; do
			if [ "$b" = "$e" ]; then
				skip=yes
				case " $seen_except " in
				*" $e "*) : ;;
				*) seen_except="$seen_except $e" ;;
				esac
			fi
		done
		if [ "$skip" = yes ]; then
			unguarded=$((unguarded + 1))
			continue
		fi
		for v in $NONFLAT_READ; do
			grep -qE "(^|[^A-Z_])$v=" "$f" || unowned="$unowned $b:$v"
		done
	done
	# The other direction on the exception list: an entry naming a guard that does
	# NOT invoke the binary is an exception granted to nothing, and `unguarded`
	# counts only the exceptions that were *used*, so it stays right.
	unused_except=""
	for e in $GUARD_ENV_EXCEPT; do
		case " $seen_except " in
		*" $e "*) : ;;
		*) unused_except="$unused_except $e" ;;
		esac
	done
	if [ "$unguarded" -ne 4 ]; then
		echo "FAIL expected exactly 4 declared exceptions (logfmt-path.sh,"
		echo "       which runs as the caller by design; sandbox-launch.sh,"
		echo "       which must launch in that same session;"
		echo "       apps-visual.sh, which launches EVERY installed app and so"
		echo "       needs that session's D-Bus address, Wayland display and"
		echo "       DISPLAY - pinned, it would handicap every app and report"
		echo "       the harness as the subject; and home-persists.sh, whose"
		echo "       subject is the home the daemon resolves from passwd),"
		echo "       found $unguarded -"
		echo "       the exception list and the guards have drifted apart"
		fail=1
	elif [ -n "$unused_except" ]; then
		echo "FAIL declared exception(s) naming a guard that does not run the"
		echo "       binary at all:$unused_except"
		echo "       an exception is coverage of nothing: either the guard grew"
		echo "       an invocation, or the entry is stale"
		fail=1
	elif [ -n "$unowned" ]; then
		echo "FAIL guard(s) that run the binary but do not own:$unowned"
		echo "       a name the code reads, left to the host, is a guard whose"
		echo "       verdict the host can change (round 158's ps case)"
		fail=1
	else
		# Both directions on the register: a name the code reads and the register
		# omits is a name nobody checked, and an entry the code does not read is a
		# decision about nothing.
		reg_gap=""
		for v in $host_names; do
			printf '%s\n' $NONFLAT_READ | grep -qx "$v" ||
				reg_gap="$reg_gap $v"
		done
		reg_extra=""
		for v in $NONFLAT_READ; do
			printf '%s\n' "$host_names" | grep -qx "$v" ||
				reg_extra="$reg_extra $v"
		done
		if [ -n "$reg_gap" ] || [ -n "$reg_extra" ]; then
			echo "FAIL the host-name register disagrees with the code"
			echo "       (read but unregistered:$reg_gap)"
			echo "       (registered but never read:$reg_extra)"
			fail=1
		else
			echo "ok   every guard that runs the binary owns all $n_carved"
			echo "     names the code reads, except the $unguarded declared"
			echo "     exceptions, which are guards whose subject IS the"
			echo "     caller's own session"
		fi
	fi
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "PASS: the worker takes its identity from the peer, not the caller"
	exit 0
fi
echo "FAILED: a caller-set variable reaches the root worker"
exit 1
