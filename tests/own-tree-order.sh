#!/bin/sh
# own-tree-order.sh - every root write into the per-launch tree happens before
# the tree is handed to the app, and the one write that cannot (the pid file,
# known only after the fork) goes through the directory registration held
# open.  After the handover a path write is root following a symlink the app
# planted, and an unmount by path is a mount the app chooses.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/tests/commentlib.sh"

T=$(mktemp -d /tmp/.fpkg-own-tree.XXXXXX)
trap 'rm -rf "$T"' EXIT

strip_comments "$ROOT/src/daemon/ops/run.c" > "$T/run.c"
strip_comments "$ROOT/src/daemon/sandbox.c" > "$T/sandbox.c"
strip_comments "$ROOT/src/daemon/sandbox_exec.c" > "$T/exec.c"

carve_function '^int cmd_run\(' "$T/run.c" > "$T/cmd_run.inc" ||
	{ echo "FAIL: cmd_run() not found in run.c"; exit 1; }
carve_function '^int sandbox_create_jail\(' "$T/sandbox.c" > "$T/create.inc" ||
	{ echo "FAIL: sandbox_create_jail() not found"; exit 1; }
carve_function '^void sandbox_own_tree\(' "$T/sandbox.c" > "$T/own.inc" ||
	{ echo "FAIL: sandbox_own_tree() not found"; exit 1; }
carve_function '^static bool unmount_entry\(' "$T/sandbox.c" > "$T/unmount.inc" ||
	{ echo "FAIL: unmount_entry() not found"; exit 1; }
carve_function '^static void write_instance_pid\(' "$T/exec.c" > "$T/pid.inc" ||
	{ echo "FAIL: write_instance_pid() not found"; exit 1; }

pos() {
	grep -n "$1" "$2" | head -1 | cut -d: -f1
}

bug() {
	echo "FAIL: $1"
	exit 1
}

# The checker runs on copies under $T, so the plants below prove each rule on
# a broken tree without ever writing to the tree it verifies.
check() {
	run=$1 own=$2 create=$3 pid=$4 unmount=$5

	# All four calls before the handover, in this order: the proxies' mount
	# points and the instance files are root writes into the tree.
	p1=$(pos 'sandbox_create_jail(' "$run")
	p2=$(pos 'dbus_start_proxies(' "$run")
	p3=$(pos 'instance_register(' "$run")
	p4=$(pos 'sandbox_own_tree(' "$run")
	p5=$(pos 'sandbox_exec(' "$run")
	[ -n "$p1" ] || bug "cmd_run() no longer calls sandbox_create_jail()"
	[ -n "$p2" ] || bug "cmd_run() no longer calls dbus_start_proxies()"
	[ -n "$p3" ] || bug "cmd_run() no longer calls instance_register()"
	[ -n "$p4" ] || bug "cmd_run() no longer calls sandbox_own_tree()"
	[ -n "$p5" ] || bug "cmd_run() no longer calls sandbox_exec()"
	[ "$p1" -lt "$p2" ] || bug "the proxies start before the jail exists"
	[ "$p2" -lt "$p3" ] || bug "the instance is registered before the proxies"
	[ "$p3" -lt "$p4" ] || bug "the tree is handed over before registration"
	[ "$p4" -lt "$p5" ] || bug "the tree is handed over after exec"

	# Nothing root writes by path may follow the handover.
	tail -n +"$((p4 + 1))" "$run" > "$T/after.inc"
	if grep -nE 'write_file\(|mkdir_p\(|chown|nmount|mount\(|dbus_start_proxies\(|instance_register\(' \
		"$T/after.inc" > "$T/after.hits"; then
		bug "cmd_run() writes into the tree after the handover:
$(head -3 "$T/after.hits" | sed 's/^/       /')"
	fi

	# jail creation is earlier still, and hands nothing over itself.
	if grep -q 'own_tree_dir(\|sandbox_own_tree(' "$create"; then
		bug "sandbox_create_jail() still runs the ownership walk"
	fi
	grep -q 'own_tree_dir(sb, plan->uid, plan->gid, sb->jailroot)' "$own" ||
		bug "sandbox_own_tree() does not walk the jail root"

	# The one write left after the handover goes through the held directory.
	grep -q 'openat(sb->instance_mirror_fd, "pid"' "$pid" ||
		bug "the pid file is written by path after the handover"
	if grep -q '/run/flatpak/.flatpak/%s/pid' "$pid"; then
		bug "the pid file path is still built from the jail root"
	fi

	# An unmount resolves an id the kernel issued, never a name.
	grep -q 'MNT_BYFSID' "$unmount" ||
		bug "the in-jail unmount resolves a path the app owns"
	if grep -q 'unmount(e->path\|unmount(l\.ent\[i\]\.path' "$unmount"; then
		bug "the in-jail unmount falls back to the path"
	fi
}

# A check that fails exits its own subshell, so the plants below can observe
# the failure instead of being taken down with it.
( check "$T/cmd_run.inc" "$T/own.inc" "$T/create.inc" "$T/pid.inc" \
	"$T/unmount.inc" )

# --- the plants: each rule must fire on the tree it was written for --------
prove() {
	what=$1
	shift
	if ( "$@" ) >/dev/null 2>&1; then
		echo "FAIL: the guard passes on a tree with $what";
		exit 1
	fi
}

# 1. the handover moved ahead of the instance files
awk '
	/^	sandbox_own_tree\(/ { own = $0; next }
	{ print }
	/^	instance_register\(/ && !done { print own; done = 1 }
' "$T/cmd_run.inc" > "$T/plant-order.inc"
prove "the handover before registration" \
	check "$T/plant-order.inc" "$T/own.inc" "$T/create.inc" "$T/pid.inc" \
	"$T/unmount.inc"

# 2. a root write put back after the handover
awk '{ print } /^	sandbox_own_tree\(/ {
	print "\twrite_file(p, info, 0644);"
}' "$T/cmd_run.inc" > "$T/plant-write.inc"
prove "a path write after the handover" \
	check "$T/plant-write.inc" "$T/own.inc" "$T/create.inc" "$T/pid.inc" \
	"$T/unmount.inc"

# 3. the ownership walk back inside jail creation
{ cat "$T/create.inc"; echo '	own_tree_dir(sb, plan->uid, plan->gid, sb->jailroot);'; } \
	> "$T/plant-create.inc"
prove "the walk inside sandbox_create_jail()" \
	check "$T/cmd_run.inc" "$T/own.inc" "$T/plant-create.inc" "$T/pid.inc" \
	"$T/unmount.inc"

# 4. the pid file back on a path under the jail root
sed 's|openat(sb->instance_mirror_fd, "pid"|snprintf(p, sizeof(p), "%s/run/flatpak/.flatpak/%s/pid", sb->jailroot, sb->instance_id) \&\& open(p, "pid"|' \
	"$T/pid.inc" > "$T/plant-pid.inc"
prove "the pid file written by path" \
	check "$T/cmd_run.inc" "$T/own.inc" "$T/create.inc" "$T/plant-pid.inc" \
	"$T/unmount.inc"

# 5. the in-jail unmount back on a path
sed 's/MNT_BYFSID/0/' "$T/unmount.inc" > "$T/plant-unmount.inc"
prove "the unmount by path" \
	check "$T/cmd_run.inc" "$T/own.inc" "$T/create.inc" "$T/pid.inc" \
	"$T/plant-unmount.inc"

echo "PASS: jail-root writes precede the handover, and the rest hold a handle"
