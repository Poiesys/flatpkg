#!/bin/sh
# shell-calls.sh - a program run through a shell must have an ABSOLUTE path.
# /bin/sh resolves a bare name through the CALLER's PATH in a root worker, so a
# grep is not enough: each site is carved to the function that fills the argument.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

fail=0
note() { echo "FAIL: $*"; fail=1; }

tmp=$(mktemp -d /tmp/.fpkg-shell.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

# The canonical stripper, never a hand-written copy: a guard that greps a comment
# finds the word a deleted check left behind.
# shellcheck source=tests/commentlib.sh
. "$ROOT/tests/commentlib.sh"

# No shell may run a command from this tree: every program word is exec'd.
shells=0
for f in "$ROOT"/src/*.c "$ROOT"/src/*/*.c "$ROOT"/src/*/*/*.c; do
	strip_comments < "$f" > "$tmp/one.c"
	n=$(grep -cE '\b(system|popen)\(' "$tmp/one.c" || true)
	shells=$((shells + n))
done
if [ "$shells" -ne 0 ]; then
	echo "FAIL: $shells system()/popen() call site(s) remain in src/: a root"
	echo "       worker would hand a string to /bin/sh, which resolves a bare"
	echo "       program word through the caller's PATH"
	fail=1
else
	echo "ok   no system()/popen() in src/: no string is handed to /bin/sh"
fi

# ---- the EXEC half: the same shape, a different sink --------------------
# `/bin/sh`'s PATH search and `execvp()`'s resolve a program word the same way, in a
# privileged process.  The **program word** is argument 1 for the `v` forms and `fexecve`,
# argument 3 for `posix_spawn`, and argument 1 for the `l` forms too: `execl(path, arg0,
# ...)` names the path FIRST, so taking argv[0] reports `execl("/bin/sh", "sh", ...)` as a
# bare name.  Three outcomes, no inference: a LITERAL beginning with `/` is closed, one
# that is not is REPORTED, and a VARIABLE must be in EXEC_EXEMPT.
echo "exec sites: the exec* family, the same shape through a different sink"
: > "$tmp/execs"
for f in "$ROOT"/src/*.c "$ROOT"/src/*/*.c "$ROOT"/src/*/*/*.c; do
	strip_comments < "$f" > "$tmp/one.c"
	grep -nE '\b(execv|execve|execl|execlp|execvp|execvpe|posix_spawn|fexecve)\(' \
	    "$tmp/one.c" | cut -d: -f1 |
	while read -r ln; do
		printf '%s:%s\n' "$(basename "$f")" "$ln" >> "$tmp/execs"
	done
done
nexec=$(grep -c . "$tmp/execs" || true)
# The floor.  It sits below the real count so removing a site is not a failure -
# a shape with sites removed has changed and this file's reasoning has to be
# re-read - while an extraction that sees nothing still fails.
[ "${nexec:-0}" -ge 12 ] || {
	echo "FAIL: only $nexec exec-family site(s) found in src/, below the"
	echo "       floor of 12 - the extraction broke, or the tree stopped"
	echo "       running programs directly"
	fail=1
}

# EXEC_EXEMPT - one line per site whose program word is a variable: exec
# file:function, producing file:function, and a FACT that producer must still contain.
# `!FACT` names a resolver; `~FACT` is checked at every call site.
EXEC_EXEMPT='dbus_proxy.c:dbus_start_proxies:dbus_proxy.c:dbus_start_proxies:FLATPAK_DBUSPROXY
portal.c:spawn_child_exec:portal.c:flatpkg_bin_path:access
portal.c:handle_update:portal.c:flatpkg_bin_path:access
sandbox_exec.c:child_exec:sandbox_exec.c:child_set_base_env:/app/bin:/run/flatpak:/usr/bin
sandbox_root.c:run_ldconfig:extensions.c:fp_extensions_apply:/usr/bin/ldconfig
util.c:run_program:util.c:run_program:argv[0][0] != '/' 
instance_ctl.c:cmd_enter:instance_ctl.c:cmd_enter:/app/bin:/usr/bin
sandbox_root.c:p11kit_ensure_server:sandbox_root.c:p11kit_ensure_server:/usr/local/bin/p11-kit
ostree.c:run_direct_cb:ostree.c:tool_binary:/usr/local/bin/ostree
ostree.c:run_capture:ostree.c:gpg_program:!gpg_program'

exec_resolved=0
exec_registered=0
while IFS=: read -r xbase xline; do
	[ -n "$xbase" ] || continue
	strip_comments < "$(srcf "$xbase")" > "$tmp/xbody.c"
	# The call's name and its program word, off the ONE line.  Both come
	# from the same match, so they cannot disagree.
	xcall=$(awk -v L="$xline" 'NR == L {
		if (match($0, /(execv|execve|execl|execlp|execvp|execvpe|posix_spawn|fexecve)\(/)) {
			name = substr($0, RSTART, RLENGTH - 1)
			s = substr($0, RSTART + RLENGTH)
			sub(/^[ \t]+/, "", s)
			n = split(s, a, /[,)]/)
			# argument 1 for every spelling, argument 3 for
			# posix_spawn(path, argv, envp, ...)
			word = (name == "posix_spawn") ? a[3] : a[1]
			print name, word
		}
	}' "$tmp/xbody.c")
	xkind=${xcall%% *}
	xword=${xcall#* }
	if [ -z "$xkind" ] || [ "$xcall" = "$xword" ]; then
		note "$xbase:$xline is an exec-family line this guard cannot read"
		echo "       the call's program word from; it is UNVERIFIED, not safe"
		continue
	fi
	# A literal, checked where it stands.  **Strip the quotes before asking
	# whether it is absolute**: `/*` tested against `""/bin/sh""` is a quote and
	# not a slash, and reported five correct sites as bare names.
	case "$xword" in
	'"'*'"') xlit=$(printf '%s' "$xword" | tr -d '"') ;;
	*) xlit="" ;;
	esac
	if [ -n "$xlit" ]; then
		case "$xlit" in
		/*) exec_resolved=$((exec_resolved + 1)); continue ;;
		esac
		note "$xbase:$xline hands the bare name '$xlit' to $xkind()"
		echo "       /bin/sh and execvp() both resolve a bare name through"
		echo "       the CALLER's PATH, and the worker is root with the"
		echo "       caller's environment (round 140, round 221)"
		continue
	fi
	# A variable, and a BARE ARM OF A CONDITIONAL is the defect this section exists
	# for: if an assignment to the word is a conditional, EVERY literal on its
	# right-hand side must be absolute.  Reading only the first arm leaves it green.
	wbase=${xword%%[.\[]*}
	# **grep needs the FILE, not a printf of its name.**  Piping the path into
	# the grep searched the text "/tmp/.../xbody.c", matched no assignment on
	# any tree, and this check could not fire at all.
	dassign=$(grep -nE "(^|[^A-Za-z_0-9])${wbase}(\\[[0-9]+\\])?[[:space:]]*=[^=]" \
	    "$tmp/xbody.c" | cut -d: -f1 || true)
	dbad=""
	for al in $dassign; do
		# THE STATEMENT, not the line: the two-armed assignment is written over
		# two lines, so the `?` was on the line the grep did not read.  Read to
		# the terminating `;`, as the shell half of this file does.
		astmt=$(awk -v S="$al" 'NR >= S {
				printf "%s", $0
				if (/;/) exit
			}' "$tmp/xbody.c" | sed 's/;.*//')
		case "$astmt" in
		*'?'*':'*) : ;;
		*) continue ;;
		esac
		for lit in $(printf '%s\n' "$astmt" | grep -oE '"[^"]*"' |
		    tr -d '"'); do
			case "$lit" in
			/*) : ;;
			*) dbad="$dbad $lit" ;;
			esac
		done
	done
	if [ -n "$dbad" ]; then
		note "$xbase:$xline picks a program word with a conditional, and the"
		echo "       bare arm(s):$dbad - /bin/sh and execvp() both resolve"
		echo "       a bare name through the CALLER's PATH, in a root worker"
		fail=1
		continue
	fi

	# A variable: the enclosing function, then the register.  The function is the last
	# column-0 definition above the line, compared as a NAME against the register's
	# field - not as a line number, which reports "no exemption" for every site.
	xsite=$(awk -v L="$xline" '
    NR < L && /^(static[ 	]+)?(int|void|bool|char|size_t|pid_t|ssize_t|fpkg_error_t|FILE|DIR|const)[ 	*]+[A-Za-z_][A-Za-z_0-9]*\(/ {
        line = $0
        sub(/\(.*/, "", line)
        n = split(line, a, /[ 	]+/)
        last = a[n]
    }
    END { print last }' "$tmp/xbody.c")
	xfact=""
	for e in $EXEC_EXEMPT; do
		[ "$(printf '%s' "$e" | cut -d: -f1)" = "$xbase" ] || continue
		[ "$(printf '%s' "$e" | cut -d: -f2)" = "$xsite" ] || continue
		xfact=$(printf '%s' "$e" | cut -d: -f5)
	done
	if [ -z "$xfact" ]; then
		note "$xbase:$xline hands $xkind() the variable '$xword', whose"
		echo "       value this guard cannot see, and $xbase:$xsite() is not"
		echo "       in EXEC_EXEMPT - UNVERIFIED, not safe.  Name the site"
		echo "       there with the literal its program word comes from."
		continue
	fi
	exec_registered=$((exec_registered + 1))
done < "$tmp/execs"

# The floors on the outcomes, so a sweep that suddenly accounts for nothing
# cannot pass.  A count that can only be zero is not a check, and neither is
# one that can only be everything.
[ "$exec_resolved" -ge 3 ] || {
	echo "FAIL: only $exec_resolved of $nexec exec-family site(s) closed on an"
	echo "       absolute literal (floor 3) - the extraction is not seeing the"
	echo "       tree, or every site became a variable"
	fail=1
}
[ "$exec_registered" -ge 9 ] || {
	echo "FAIL: only $exec_registered of $nexec exec-family site(s) accounted for"
	echo "       by EXEC_EXEMPT (floor 9) - either a new variable word"
	echo "       appeared without a decision recorded, or the register lost"
	echo "       entries.  A word with no entry is not safe, it is unseen."
	fail=1
}

# The register, in the direction a list of names rots: every entry must name a
# site function that exists and still holds an exec call, and the FACT must still
# be in the function that produces the word.
printf '%s\n' "$EXEC_EXEMPT" > "$tmp/exec_exempt"
while IFS= read -r e; do
	[ -n "$e" ] || continue
	e_file=$(printf '%s' "$e" | cut -d: -f1)
	e_fn=$(printf '%s' "$e" | cut -d: -f2)
	e_sfile=$(printf '%s' "$e" | cut -d: -f3)
	e_src=$(printf '%s' "$e" | cut -d: -f4)
	e_fact=$(printf '%s' "$e" | cut -d: -f5-)
	[ -n "$e_fn" ] && [ -n "$e_sfile" ] && [ -n "$e_src" ] &&
	    [ -n "$e_fact" ] ||
		{ note "EXEC_EXEMPT entry '$e' is missing a field"; continue; }
	[ -f "$(srcf "$e_file")" ] ||
		{ note "EXEC_EXEMPT names src/$e_file, which does not exist"; continue; }
	# **carve_function takes a FILE, not the file's contents.**  Handing it the
	# contents made awk read STDIN - the guard's own site list - so every entry
	# was reported as not found.
	strip_comments < "$(srcf "$e_file")" > "$tmp/xs.c"
	carve_function "^$e_fn\(" "$tmp/xs.c" > "$tmp/xs_fn.c" || true
	if [ ! -s "$tmp/xs_fn.c" ]; then
		note "EXEC_EXEMPT names $e_fn(), which is not in src/$e_file any more"
		continue
	fi
	grep -qE '\b(execv|execve|execl|execlp|execvp|execvpe|posix_spawn)\(' \
	    "$tmp/xs_fn.c" ||
		{ note "EXEC_EXEMPT names $e_fn(), which no longer execs anything"
		  continue; }
	# The producing function, in its own file.
	if [ "$e_sfile" = "$e_file" ]; then
		strip_comments < "$(srcf "$e_sfile")" > "$tmp/xp.c"
	else
		[ -f "$(srcf "$e_sfile")" ] ||
			{ note "EXEC_EXEMPT names src/$e_sfile, which does not exist"
			  continue; }
		strip_comments < "$(srcf "$e_sfile")" > "$tmp/xp.c"
	fi
	carve_function "^$e_src\(" "$tmp/xp.c" > "$tmp/xp_fn.c" || true
	if [ ! -s "$tmp/xp_fn.c" ]; then
		note "EXEC_EXEMPT names $e_src() in src/$e_sfile as where $e_fn()'s"
		echo "       program word comes from, and it is not there"
		continue
	fi
	case "$e_fact" in
	\~*)
		# EVERY call site of the named function passes an absolute literal as the
		# word's argument, with a floor on how many were found.  The DEFINITION is
		# counted on its own and must be exactly one.
		e_fn2=${e_fact#\~}
		defs=$(grep -cE "^[A-Za-z_][A-Za-z0-9_*[:space:]]*[[:space:]*]$e_fn2\(" "$tmp/xp.c" || true)
		[ "${defs:-0}" -eq 1 ] ||
			{ note "EXEC_EXEMPT names $e_fn2(), and src/$e_sfile has"
			  echo "       ${defs:-0} definitions of it - the carve broke"
			  continue; }
		bad2=$(grep -oE "$e_fn2\([^,)]*,[[:space:]]*\"[^\"]*\"" \
		    "$tmp/xp.c" | grep -vE ',[[:space:]]*"/' || true)
		ncalls=$(grep -coE "$e_fn2\([^,)]*,[[:space:]]*\"" \
		    "$tmp/xp.c" || true)
		if [ -n "$bad2" ]; then
			note "$e_fn2() has a caller passing a BARE program word:"
			echo "       $(printf '%s' "$bad2" | tr '\n' ' ')"
			fail=1
		elif [ "${ncalls:-0}" -lt 6 ]; then
			note "only $ncalls $e_fn2() call site(s) pass a literal"
			echo "       program word (floor 6) - the others would be"
			echo "       UNVERIFIED rather than safe"
		fi
		;;
	\!*)
		# A RESOLVER: the fact is the function whose return value fills the word, so
		# the thing to assert is ITS candidate table - and a table holding a bare name
		# is the defect this section exists for.
		e_res=${e_fact#\!}
		carve_function "^$e_res\(" "$tmp/xp.c" > "$tmp/xr.c" || true
		if [ ! -s "$tmp/xr.c" ]; then
			note "EXEC_EXEMPT names the resolver $e_res(), and it is not"
			echo "       in src/$e_sfile"
			continue
		fi
		bare=$(grep -oE '"[^"]+"' "$tmp/xr.c" | grep -vE '^"/' || true)
		if [ -n "$bare" ]; then
			note "$e_res() holds a BARE candidate: $(printf '%s' \
			    "$bare" | tr '\n' ' ')"
			echo "       $e_fn() execs whatever it returns"
			fail=1
		fi
		;;
	*)
		grep -qF "$e_fact" "$tmp/xp_fn.c" ||
			note "EXEC_EXEMPT says $e_fn()'s word rests on '$e_fact' in"
		;;
	esac
done

# The one ORDER claim in the register: the credential drop has to be ABOVE the exec,
# or the program named by $FLATPAK_DBUSPROXY is exec'd as root.  A grep for "does it
# setuid?" is satisfied by exactly the broken arrangement.
strip_comments < "$ROOT/src/daemon/ops/dbus_proxy.c" > "$tmp/dbus.c"
dfunc=$(carve_function '^dbus_start_proxies\(' "$tmp/dbus.c" || true)
if [ -z "$dfunc" ]; then
	note "could not carve dbus_start_proxies() out of src/daemon/ops/dbus_proxy.c - the shape"
	echo "       moved, and the order claim is UNVERIFIED"
else
	# The three calls in ONE checked condition, ABOVE THE EXEC.  Neither "count the calls"
	# nor "find the first condition" works: the earlier child that creates the proxy
	# directory has the same three.  The claim is positional - from the LAST initgroups()
	# above the exec down to the exec.
	dexec=$(printf '%s\n' "$dfunc" | grep -n 'execv(proxy' | head -1 |
		cut -d: -f1)
	dcond=0
	dthree=no
	if [ -n "$dexec" ]; then
		dcond=$(printf '%s\n' "$dfunc" |
			grep -n 'initgroups(plan->user_name' |
			awk -F: -v E="$dexec" '$1 < E { last = $1 }
				END { print last + 0 }')
		if [ "$dcond" -gt 0 ]; then
			# Exactly one initgroups() in the block, all three calls tested against
			# 0, the later two joined by `||`.  dbus.c mixes tabs and spaces, so
			# whitespace is squeezed or `|| <tab>setgid(` matches nothing.
			dblk=$(printf '%s\n' "$dfunc" |
				sed -n "${dcond},$((dexec - 1))p" |
				tr '\n\t' '  ' | tr -s ' ')
			case "$dblk" in
			*'|| setgid(plan->gid) != 0 || setuid(plan->uid) != 0'*)
				case "$dblk" in
				*initgroups*initgroups*) dthree=no ;;
				*) dthree=yes ;;
				esac
				;;
			esac
		fi
	fi
	drefuse=$(printf '%s\n' "$dfunc" | grep -c 'refusing to run %s as root' ||
		true)
	if [ -z "$dexec" ] || [ "$dcond" -eq 0 ]; then
		note "dbus_start_proxies(): cannot locate the credential drop and"
		echo "       the exec of \$FLATPAK_DBUSPROXY, so the 'unprivileged"
		echo "       because the drop comes first' claim is UNVERIFIED"
	elif [ "$dcond" -ge "$dexec" ]; then
		note "dbus_start_proxies() execs \$FLATPAK_DBUSPROXY at $dexec, before"
		echo "       the credential drop at $dcond - a caller-named program"
		echo "       would run as root"
	elif [ "$dthree" != yes ] || [ "$drefuse" -lt 1 ]; then
		note "dbus_start_proxies(): the drop between the initgroups() at"
		echo "       $dcond and the exec at $dexec is not one checked"
		echo "       condition over all three calls (shape: $dthree), or its"
		echo "       refusal is not said ($drefuse mention(s)) - three"
		echo "       unchecked calls in front of one exec is the arrangement"
		echo "       with no way to fail loudly"
	else
		echo "ok   dbus_start_proxies() drops to the app's uid in one checked"
		echo "     condition at line $dcond and only then execs the proxy at"
		echo "     $dexec, so \$FLATPAK_DBUSPROXY cannot name a program root"
		echo "     runs, and the refusal is said in as many words"
	fi
fi

if [ $fail -eq 0 ]; then
	echo "PASS: no system()/popen() reaches /bin/sh, so no string is handed"
	echo "      to the caller's PATH to resolve as root,"
	echo "      $nexec exec-family sites: $exec_resolved close on an absolute"
	echo "      literal and $exec_registered are accounted for by EXEC_EXEMPT,"
	echo "      each with the literal its program word comes from, so no bare"
	echo "      name reaches execv()/execvp() from a root worker either"
fi
exit $fail
