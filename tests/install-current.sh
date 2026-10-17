#!/bin/sh
# install-current.sh - the files `make install` put on this box match the tree
# the guards read: a man page edited and not reinstalled passes every doc check
# and is still the one `man flatpkg` prints.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MK="$ROOT/Makefile"
PREFIX=/usr/local
STORE_DIR=/var/db/flatpkg
fail=0

echo "installed artefacts: the copies under $PREFIX must match the tree"
echo

# Nothing installed: say so and skip.  A skip counted as a pass is the
# round-149 failure, and the runner reports skips separately.
if [ ! -x "$PREFIX/sbin/flatpkg" ]; then
	echo "SKIP: $PREFIX/sbin/flatpkg is not installed on this box"
	exit 0
fi

# "tree source|installed path", pinned.  A destination ending in "/" takes
# the source's basename, which is how install(1) behaves.
TABLE="build/flatpkg|$PREFIX/sbin/flatpkg
build/flatpkgd|$PREFIX/sbin/flatpkgd
build/libflatpkg_shared.so|$PREFIX/lib/libflatpkg_shared.so
build/flatpkg.8|$PREFIX/man/man8/flatpkg.8
build/signalfd_shim.so|$PREFIX/lib/flatpkg/signalfd_shim.so
build/seccomp_shim.so|$PREFIX/lib/flatpkg/seccomp_shim.so
build/dmabuf_devt_shim.so|$PREFIX/lib/flatpkg/dmabuf_devt_shim.so
build/memfd_shim.so|$PREFIX/lib/flatpkg/memfd_shim.so
build/syncobj_shim.so|$PREFIX/lib/flatpkg/syncobj_shim.so
build/rlimit_shim.so|$PREFIX/lib/flatpkg/rlimit_shim.so
build/madvise_shim.so|$PREFIX/lib/flatpkg/madvise_shim.so
scripts/org.freedesktop.portal.Flatpak.service|$PREFIX/share/dbus-1/services/org.freedesktop.portal.Flatpak.service
scripts/org.freedesktop.portal.Desktop.service|$PREFIX/share/dbus-1/services/org.freedesktop.portal.Desktop.service
scripts/org.freedesktop.Flatpak.service|$PREFIX/share/dbus-1/services/org.freedesktop.Flatpak.service
scripts/org.freedesktop.Flatpak.SystemHelper.service|$PREFIX/share/dbus-1/system-services/org.freedesktop.Flatpak.SystemHelper.service
scripts/org.freedesktop.Flatpak.SystemHelper.conf|$PREFIX/etc/dbus-1/system.d/org.freedesktop.Flatpak.SystemHelper.conf
scripts/flatpkg-helper.conf|$PREFIX/etc/flatpkg/helper.conf
scripts/flatpkg.sh|$PREFIX/etc/profile.d/flatpkg.sh
scripts/99-flatpkg.conf|$PREFIX/lib/environment.d/99-flatpkg.conf
completions/flatpkg.bash|$PREFIX/share/bash-completion/completions/flatpkg
completions/flatpkg.bash|$PREFIX/share/bash-completion/completions/flatpak
completions/_flatpkg|$PREFIX/share/zsh/site-functions/_flatpkg
completions/flatpkg.fish|$PREFIX/share/fish/vendor_completions.d/flatpkg.fish
completions/flatpkg.fish|$PREFIX/share/fish/vendor_completions.d/flatpak.fish"

checked=0
stale=""
TMP=$(mktemp /tmp/.fpkg-rc.XXXXXX)
trap 'rm -f "$TMP"' EXIT

# `while read` over the table rather than `for row in $TABLE`: the rows
# are line-separated, and word-splitting them would be a second parser
# with its own idea of where a path ends.
while IFS='|' read -r src dst; do
	[ -n "$src" ] || continue
	checked=$((checked + 1))
	[ -f "$ROOT/$src" ] || {
		echo "FAIL $src is in the table but not in the tree"
		fail=1
		continue
	}
	if [ ! -e "$dst" ]; then
		echo "FAIL $dst is missing: the tree has $src, the box does not"
		fail=1
		continue
	fi
	if ! cmp -s "$ROOT/$src" "$dst"; then
		stale="$stale $dst"
	fi
done <<EOF
$TABLE
EOF

# The count, from the Makefile itself: every `install -m` line is one table
# row (two sources are installed twice, for the flatpak alias), so a new
# source fails here until someone says where it lands.
mk_lines=$(grep -cE '^[[:space:]]*install .*-m [^[:space:]]+' "$MK" || true)
n_rows=$(printf '%s\n' "$TABLE" | grep -c . || true)
if [ "$mk_lines" -lt 15 ] || [ "$n_rows" -lt 15 ]; then
	echo "FAIL the install count is implausible: $mk_lines install lines,"
	echo "       $n_rows table rows (both expected over 15)"
	fail=1
elif [ "$mk_lines" -ne "$n_rows" ]; then
	echo "FAIL make install has $mk_lines 'install -m' lines and the table"
	echo "       has $n_rows rows: a file was added to the install target"
	echo "       and nobody has said where it lands"
	fail=1
fi

if [ -n "$stale" ]; then
	echo "FAIL installed file(s) differ from the tree:$stale"
	echo "       the guards verified the tree copy; the user reads these"
	fail=1
fi

# The generated rc.d script: compared after the same sed the Makefile
# applies, because that is what lands on the box.
if [ -f "$PREFIX/etc/rc.d/flatpkgd" ]; then
	sed -e "s|@PREFIX@|$PREFIX|g" "$ROOT/scripts/rc.d/flatpkgd" > "$TMP"
	if cmp -s "$TMP" "$PREFIX/etc/rc.d/flatpkgd"; then
		checked=$((checked + 1))
	else
		echo "FAIL $PREFIX/etc/rc.d/flatpkgd differs from the tree's"
		echo "       scripts/rc.d/flatpkgd after the @PREFIX@ substitution:"
		echo "       a stale one means 'service flatpkgd start' runs old code"
		fail=1
	fi
else
	echo "FAIL $PREFIX/etc/rc.d/flatpkgd is missing"
	fail=1
fi

# The two symlinks, by target: they are the only way `flatpak` and
# /var/lib/flatpkg exist, and a wrong target is invisible to cmp.
for pair in "$PREFIX/bin/flatpak|../sbin/flatpkg" \
            "/var/lib/flatpkg|$STORE_DIR" \
            "/var/lib/flatpak|flatpkg"; do
	link=${pair%%|*}
	want=${pair#*|}
	got=$(readlink "$link" 2>/dev/null || true)
	checked=$((checked + 1))
	if [ "$got" != "$want" ]; then
		echo "FAIL $link -> '${got:-<not a symlink>}', expected '$want'"
		fail=1
	fi
done

# --- and the RUNNING daemon is that install ---------------------------
# `run` executes in the daemon's process, so a daemon started before the
# last install runs the old build while every guard reports on the new one.
if [ -x "$PREFIX/sbin/flatpkg" ]; then
	dpid=$(pgrep -f "^$PREFIX/sbin/flatpkgd$" 2>/dev/null | head -n 1 || true)
	if [ -z "$dpid" ]; then
		echo "note: no flatpkgd listener running; the stale-daemon check is skipped"
	else
		# Seconds SINCE the daemon started, not a parsed start date: the
		# lstart parse failed here, left the comparison skipped, and the
		# guard reported ok against a daemon two builds behind.
		bin_m=$(stat -f %m "$PREFIX/sbin/flatpkgd" 2>/dev/null || echo 0)
		age=$(ps -o etimes= -p "$dpid" 2>/dev/null | tr -d ' ' || true)
		now_m=$(date +%s)
		checked=$((checked + 1))
		if [ -z "${age:-}" ] || [ -z "${now_m:-}" ] || [ -z "${bin_m:-}" ]; then
			echo "FAIL could not read the daemon's start time (pid $dpid);"
			echo "       the stale-daemon check is unmeasured, not passed"
			fail=1
		elif [ "$bin_m" -gt $((now_m - age)) ]; then
			echo "FAIL $PREFIX/sbin/flatpkgd is NEWER than the running"
			echo "       daemon (pid $dpid): every 'flatpkg run' is"
			echo "       executing the old build.  Restart it:"
			echo "         mdo service flatpkgd restart"
			fail=1
		else
			echo "ok   the running daemon predates no install (pid $dpid)"
		fi
	fi
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "PASS: all $checked installed artefacts match the tree"
	exit 0
fi
echo "FAILED: the box and the tree have drifted apart"
exit 1
