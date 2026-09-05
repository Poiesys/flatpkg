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

# The one site whose program word is interpolated from a table, and a property
# of that TABLE's contents rather than of the call: `gpgconfs[]` holds
# "/usr/local/bin/gpgconf", so the word is absolute.  The last check pins it.
EXEMPT='sandbox.c:gpg_agent_socket'

: > "$tmp/sites"
for f in "$ROOT"/src/*.c "$ROOT"/src/*/*.c "$ROOT"/src/*/*/*.c; do
	strip_comments < "$f" > "$tmp/one.c"
	grep -nE '\b(system|popen)\(' "$tmp/one.c" | cut -d: -f1 |
	while read -r ln; do
		printf '%s:%s\n' "$(basename "$f")" "$ln" >> "$tmp/sites"
	done
done
total=$(grep -c . "$tmp/sites" || true)
[ "${total:-0}" -ge 1 ] || {
	echo "FAIL: no system()/popen() site found in src/ - either the"
	echo "       extraction broke or the tree stopped handing strings to a shell"
	exit 1
}
# The floor: two sites went with the dead helper removal, and a tree that
# loses another has changed shape and this file's reasoning has to be re-read.
[ "$total" -ge 10 ] || {
	echo "FAIL: only $total shell call site(s) found, below the floor of 10"
	fail=1
}

resolved=0
interp=0
while IFS=: read -r base line; do
	[ -n "$base" ] || continue
	strip_comments < "$(srcf "$base")" > "$tmp/body.c"

	# The argument.  awk, not sed: `system(`/`popen(` need an alternation, BSD
	# sed has no `\|` and `\b` is a GNUism.  It may be a LITERAL, which is the
	# easiest case there is.
	arg=$(awk -v L="$line" 'NR == L {
		if (match($0, /(system|popen)\(/)) {
			s = substr($0, RSTART + RLENGTH)
			sub(/^[ \t]+/, "", s)
			if (match(s, /^"[^"]*"/))
				print "L " substr(s, 2, RLENGTH - 2)
			else if (match(s, /^[A-Za-z_][A-Za-z_0-9]*/))
				print "V " substr(s, RSTART, RLENGTH)
		}
	}' "$tmp/body.c")
	akind=${arg%% *}
	arg=${arg#* }
	case "$akind" in
	L)
		case "$arg" in
		/*) resolved=$((resolved + 1)) ;;
		*) note "$base:$line hands '$arg' straight to the shell and its"
		   echo "       program word is not absolute" ;;
		esac
		continue
		;;
	V) : ;;
	*)
		note "$base:$line passes something this guard cannot resolve to"
		echo "       system()/popen(); it is UNVERIFIED rather than safe"
		continue
		;;
	esac

	# The enclosing function: back to the last column-0 definition, forward to
	# its closing brace in column 0.  The return type may share the line, so the
	# pattern allows one; anchoring on `^name(` alone found nothing.
	start=$(awk -v L="$line" 'NR < L && /^[A-Za-z_][A-Za-z0-9_ \t*]*\(/ { last = NR }
		END { print last + 1 }' "$tmp/body.c")
	end=$(awk -v S="$start" 'NR >= S && /^\}$/ { print NR; exit }' "$tmp/body.c")
	if [ -z "$start" ] || [ -z "$end" ] || [ "$end" -le "$start" ]; then
		note "could not carve the function around $base:$line"
		continue
	fi
	sed -n "${start},${end}p" "$tmp/body.c" > "$tmp/fn.c"

	# The STATEMENT that fills the argument, not the line: five of the twelve
	# wrap the call, so a line-at-a-time scan finds no literal.  Flatten, then
	# walk from each `snprintf(<arg>` to its `;`.
	lits=$(tr '\n' ' ' < "$tmp/fn.c" | awk -v a="$arg" '
		BEGIN { RS = "" }
		{
			rest = $0
			out = ""
			while (1) {
				p = index(rest, "snprintf(" a)
				q2 = index(rest, "= " a)
				if (p == 0 && q2 == 0) break
				if (p == 0 || (q2 > 0 && q2 < p)) p = q2
				rest = substr(rest, p)
				e = index(rest, ";")
				if (e == 0) e = length(rest)
				stmt = substr(rest, 1, e)
				while (match(stmt, /"[^"]*"/)) {
					out = out substr(stmt, RSTART + 1, RLENGTH - 2) "\n"
					stmt = substr(stmt, RSTART + RLENGTH)
				}
				rest = substr(rest, e + 1)
			}
			printf "%s", out
		}' | sort -u)

	listed=no
	for e in $EXEMPT; do
		[ "$(printf '%s' "$e" | cut -d: -f1)" = "$base" ] || continue
		listed=yes
	done

	if [ -z "$lits" ]; then
		if [ "$listed" = yes ]; then
			interp=$((interp + 1))
		else
			note "$base:$line: no literal fills '$arg' and the site is not in"
			echo "       EXEMPT - it is UNVERIFIED, not safe.  Name it there with"
			echo "       where its program word comes from."
		fi
		continue
	fi

	# **Adjacent C string literals concatenate**, so the decision is made once per
	# STATEMENT: if any literal interpolates the program word, the rest are its
	# arguments.  Read one literal at a time and `"2>/dev/null"` is a bare command.
	if printf '%s\n' "$lits" | grep -q '^%'; then
		: > "$tmp/bad"
	else
		: > "$tmp/bad"
		printf '%s\n' "$lits" | while read -r lit; do
			[ -n "$lit" ] || continue
			case "$lit" in
			/*) : ;;
			*) printf '%s\n' "$lit" >> "$tmp/bad" ;;
			esac
		done
	fi
	if [ -s "$tmp/bad" ]; then
		while read -r b; do
			note "$base:$line builds '$b', whose program word is not absolute"
			echo "       /bin/sh resolves a bare name through the CALLER's PATH,"
			echo "       and the worker is root with the caller's environment"
		done < "$tmp/bad"
	elif printf '%s\n' "$lits" | grep -q '^%'; then
		if [ "$listed" = yes ]; then
			interp=$((interp + 1))
		else
			note "$base:$line interpolates its program word from a table and is"
			echo "       not in EXEMPT - UNVERIFIED, not safe"
		fi
	else
		resolved=$((resolved + 1))
	fi
done < "$tmp/sites"

# The floors on the outcomes, so a guard that suddenly resolves nothing cannot
# pass: a count that can only be zero is not a check.
[ "$resolved" -ge 8 ] || {
	echo "FAIL: only $resolved site(s) resolved to an absolute program word,"
	echo "       below the floor of 8 - the extraction is not seeing the tree"
	fail=1
}
[ "$interp" -ge 1 ] || {
	echo "FAIL: no interpolated site found, so the EXEMPT path is untested and a"
	echo "       table-supplied program word would pass as an absolute one"
	fail=1
}

# Every exemption must name a file that exists, a function in it, and - because
# it rests on the TABLE's contents - an absolute path in that table.  A stale
# exemption is a name protecting nothing.
for e in $EXEMPT; do
	e_file=$(printf '%s' "$e" | cut -d: -f1)
	e_fn=$(printf '%s' "$e" | cut -d: -f2)
	[ -n "$e_fn" ] || { note "EXEMPT entry '$e' names no function"; continue; }
	[ -f "$(srcf "$e_file")" ] ||
		{ note "EXEMPT names src/$e_file, which does not exist"; continue; }
	strip_comments < "$(srcf "$e_file")" > "$tmp/xe.c"
	carve_function "^$e_fn\(" "$tmp/xe.c" > "$tmp/xe_fn.c" || true
	[ -s "$tmp/xe_fn.c" ] ||
		note "EXEMPT names $e_fn(), which is not in src/$e_file any more"
	# and the table it depends on must still hold an absolute path
	strip_comments < "$(srcf "$e_file")" |
		grep -oE '"[^"]*gpgconf[^"]*"' | grep -qv '^"/' &&
		note "the gpgconfs[] table $e_fn() interpolates holds a BARE gpgconf"
done

# ---- the ARGUMENT half, on the one sink where a caller supplies it ----
# The other half of the same class: the INTERPOLATED value, and the only site where a
# REMOTE's extension metadata reaches a command line.  The ORDER is the claim: a
# whitelist placed after the `snprintf` protects nothing.
MODFN=flatpkg_kernel_module_loaded
# From THIS file, not from whatever the loop above happened to end on: a carve
# that reads the previous file reports that the shape moved.
strip_comments < "$ROOT/src/shared/modules.c" > "$tmp/mod.c"
modfn=$(carve_function "^$MODFN\(" "$tmp/mod.c")
if [ -z "$modfn" ]; then
	note "could not carve $MODFN() out of src/shared/modules.c - the shape moved"
else
	# every character the whitelist ACCEPTS: the endpoints of its range
	# tests and the literals of its equality tests.
	accepts=$(printf '%s\n' "$modfn" |
		grep -oE "\*p >= '.' && \*p <= '.'|\*p == '.'" |
		grep -oE "'.'" | tr -d "'" | sort -u | tr -d '\n')
	[ -n "$accepts" ] || {
		note "$MODFN() has no character-class whitelist at all"
		echo "       and it builds a command line - that is root code"
		echo "       execution from a remote's extension metadata"
	}
	bad=$(printf '%s' "$accepts" | tr -d 'a-zA-Z0-9_-')
	if [ -n "$bad" ]; then
		note "$MODFN() accepts a character outside [a-zA-Z0-9_-]: '$bad'"
		echo "       a module name carrying that reaches a root shell"
	fi
	# and the ORDER, which is what the claim actually says - plus that the
	# rejection is INSIDE the loop rather than merely above it.
	cline=$(printf '%s\n' "$modfn" | grep -n 'snprintf(cmd' | head -1 |
		cut -d: -f1)
	lloop=$(printf '%s\n' "$modfn" | grep -n 'for (const char\* p = name' |
		head -1 | cut -d: -f1)
	# The rejection INSIDE the loop, not the first `return false` in the
	# function: there is one above the loop too, for the NULL/empty name.
	vline=$(printf '%s\n' "$modfn" | grep -n 'return false' |
		awk -F: -v L="$lloop" '$1 > L { print $1; exit }')
	if [ -z "$vline" ] || [ -z "$cline" ] || [ -z "$lloop" ]; then
		note "$MODFN(): cannot locate the loop, the rejection and the"
		echo "       command line, so the 'refused before any command line"
		echo "       is built' claim is UNVERIFIED"
	elif [ "$vline" -ge "$cline" ]; then
		note "$MODFN() builds its command line at $cline, before the"
		echo "       whitelist rejects at $vline - a name checked afterwards"
		echo "       is not checked at all"
	elif [ -z "$vline" ]; then
		note "$MODFN() has a character loop at $lloop but no rejection"
		echo "       inside it: nothing refuses a name the loop walks past"
	else
		echo "ok   $MODFN() refuses a name at line $vline, inside the class"
		echo "        loop at $lloop and before the command at $cline, and"
		echo "        accepts only [a-zA-Z0-9_-]"
	fi
	# Two things this does NOT catch: NARROWING the class is not reported, since widening
	# is the dangerous direction; and NEUTERING the loop mid-walk passes, because the order
	# is a LINE comparison - an early `break` keeps the `return false` above the command.
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
[ "${nexec:-0}" -ge 14 ] || {
	echo "FAIL: only $nexec exec-family site(s) found in src/, below the"
	echo "       floor of 14 - the extraction broke, or the tree stopped"
	echo "       running programs directly"
	fail=1
}

# EXEC_EXEMPT - one line per site whose program word is a variable: exec
# file:function, producing file:function, and a FACT that producer must still contain.
# `!FACT` names a resolver; `~FACT` is checked at every call site.
EXEC_EXEMPT='dbus_proxy.c:dbus_start_proxies:dbus_proxy.c:dbus_start_proxies:FLATPAK_DBUSPROXY
portal.c:handle_spawn:portal.c:flatpkg_bin_path:access
portal.c:handle_update:portal.c:flatpkg_bin_path:access
sandbox.c:sandbox_exec:sandbox.c:sandbox_exec:/app/bin:/run/flatpkg:/usr/bin
sandbox.c:stage_ld_cache:extensions.c:fp_extensions_apply:/usr/bin/ldconfig
instance_ctl.c:cmd_enter:instance_ctl.c:cmd_enter:/app/bin:/usr/bin
systemhelper.c:handle_uninstall:config.c:fpkg_helper_command:argv[0][0] != '/'
systemhelper.c:run_helper_command:config.c:fpkg_helper_command:argv[0][0] != '/'
sandbox.c:run_argv:sandbox.c:mount_tool:~mount_tool
sandbox.c:p11kit_ensure_server:sandbox.c:p11kit_ensure_server:/usr/local/bin/p11-kit
ostree.c:run_direct_cb:ostree.c:tool_binary:/usr/local/bin/ostree
instance.c:jail_exists:instance.c:jail_exists:/usr/sbin/jls
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
	echo "PASS: $resolved shell call sites build an absolute program path and"
	echo "      $interp more interpolate theirs from the table EXEMPT names, so"
	echo "      no bare name is left for the caller's PATH to resolve as root,"
	echo "      and the one sink fed a remote's metadata whitelists before it"
	echo "      builds anything"
	echo "      $nexec exec-family sites: $exec_resolved close on an absolute"
	echo "      literal and $exec_registered are accounted for by EXEC_EXEMPT,"
	echo "      each with the literal its program word comes from, so no bare"
	echo "      name reaches execv()/execvp() from a root worker either"
fi
exit $fail
