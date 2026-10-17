#!/bin/sh
# system-helper-scope.sh - the root system helper must ask before it acts.  D-Bus
# activates it as root, so a refusal of an unprivileged caller proves nothing about
# flatpkg: the bus daemon refuses first.  The gate is asserted per handler.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/src/daemon/systemhelper.c"
# **The INSTALLED helper, not build/flatpkg.**  The system bus starts
# /usr/local/sbin/flatpkgd (User=root in the .service file), so that copy is the
# subject - and a runtime plant needs `make install` AND a killed helper.
HELPER=/usr/local/sbin/flatpkgd
tmp=$(mktemp -d /tmp/.fpkg-syshelper.XXXXXX)
fail=0
note() { echo "FAIL: $*"; fail=1; }
trap 'rm -rf "$tmp"' EXIT INT TERM

[ -f "$SRC" ] || { echo "SKIP: no $SRC in the tree"; exit 0; }
[ -x "$HELPER" ] || { echo "SKIP: $HELPER is not installed, and the system bus"; \
	echo "      serves that copy, not build/flatpkg"; exit 0; }
command -v gdbus >/dev/null 2>&1 || { echo "SKIP: no gdbus, so the system bus cannot be called"; exit 0; }
[ -S /var/run/dbus/system_bus_socket ] || { echo "SKIP: no system bus on this box"; exit 0; }

# commentlib.sh is sourced, never run: the checks read comment-stripped text,
# because a `GetConnectionUnixUser` in a COMMENT would satisfy a grep that the code
# never makes the call.
. "$(dirname "$0")/commentlib.sh"
sb=$(strip_comments < "$SRC")

# ---- 1. the uid comes from the bus daemon ------------------------------
# The call must be a REPLY to the daemon, and the reply is the uid - not a sender
# name or any message field taken as the uid.
case "$sb" in
*GetConnectionUnixUser*) : ;;
*) note "authorize() no longer asks the bus daemon for the caller's uid;" \
	"       the whole boundary is that number" ;;
esac
# And the uid must be *used from that call*, not defaulted: if the reply
# were ignored and the variable left at 0, every caller would be root.
case "$sb" in
*'uid = (uid_t)u'*) : ;;
*) note "the uid from GetConnectionUnixUser does not reach the caller_uid()" \
	"out-parameter - check the assignment, not the call" ;;
esac

# ---- 2. the group rule --------------------------------------------------
# Function signature is "static gboolean uid_allowed(uid_t uid) {"
g=$(printf '%s\n' "$sb" | sed -n '/^static gboolean uid_allowed(/,/^}/p')
case "$g" in
*"uid == 0"*) : ;;
*) note "uid_allowed() no longer admits root outright" ;;
esac
case "$g" in
*flatpkg*) : ;;
*) note "uid_allowed() no longer names the flatpkg group" ;;
esac

# ---- 3. every handler asks before it acts ------------------------------
# Carve each handler by brace count, then compare the line of the first gate call with
# the line of the first effect - both as LINE NUMBERS, since a character offset makes
# every handler look like "NO AUTH".  The effect vocabulary is the store calls.
SH_OK=0
SH_N=0
printf '%s\n' "$sb" | awk '
function flush() {
	if (name == "" || !inb) return
	n++
	# A handler with NO store call has nothing to gate: "no gate before its first
	# store call" is a sentence about a call that does not exist.  Both no-op cases
	# are counted BEFORE the defect case.
	if (auth == 0 && eff == 0) { quiet++; return }
	if (auth == 0) { print "  " name ": store call at line " eff " with no gate"; bad++; return }
	if (eff == 0) { print "  " name ": asks, but never reaches the store"; ok++; return }
	if (auth < eff) { ok++; return }
	print "  " name ": store call at line " eff ", asks at line " auth; bad++
}
/^handle_[a-z_]+\(GDBusConnection/ {
	flush()
	name = $0; sub(/^handle_/, "", name); sub(/\(.*/, "", name)
	auth = 0; eff = 0; depth = 0; inb = 0
	next
}
name != "" {
	o = gsub(/\{/, "{"); c = gsub(/\}/, "}")
	if (o > 0) inb = 1
	if (inb) {
		# NR for both: the same clock, so the comparison cannot be a character
		# offset against a line number, which reports every handler as having
		# no gate.
		if (auth == 0 && $0 ~ /(helper_call_ok|authorize)[ \t]*\(/) auth = NR
		if (eff == 0 && $0 ~ /ostree_[a-z_]+/) eff = NR
		depth += o - c
		if (depth <= 0) { flush(); name = ""; inb = 0 }
	}
}
END { flush(); printf "%d %d %d %d\n", n, ok, bad+0, quiet+0 > "/dev/stderr" }
' >"$tmp/order" 2>"$tmp/counts"
SH_N=$(awk '{print $1}' "$tmp/counts")
SH_OK=$(awk '{print $2}' "$tmp/counts")
SH_BAD=$(awk '{print $3}' "$tmp/counts")
SH_QUIET=$(awk '{print $4}' "$tmp/counts")
# A floor, because "every handler" is an expectation this file wrote: a carve that
# matched nothing would leave 0 == 0 and pass.  Six handlers today.
[ "${SH_N:-0}" -ge 6 ] || note "the handler carve found ${SH_N:-0} handlers in $SRC," \
	"expected at least 6 - a carve that returns nothing is not a check"
if [ "${SH_BAD:-1}" -ne 0 ]; then
	note "${SH_BAD} of ${SH_N} handlers reach a store call" \
		"before they ask who is calling:"
	grep '^  ' "$tmp/order" | sed 's/^/      /'
fi
echo "  $SH_OK of $SH_N handlers that reach the store ask before they do" \
	"(two spellings: helper_call_ok() and authorize() directly);" \
	"$SH_QUIET more touch no store at all"

# ---- 4. and the allow side, at runtime ---------------------------------
# The group member's call must get PAST the gate, so the answer is the NEXT validation
# and never "denied".  The repo path is "/n": a path that cannot exist.
out=$(gdbus call --system --dest org.freedesktop.Flatpak.SystemHelper \
	--object-path /org/freedesktop/Flatpak/SystemHelper \
	--method org.freedesktop.Flatpak.SystemHelper.DeployAppstream \
	"[47,110,111]" 0 flathub rounds219 x86_64-default 2>&1) || true
case "$out" in
*AccessDenied*|*Not\ authorized*)
	note "a flatpkg-group member was refused by the system helper: $out" ;;
*'Unknown installation'*|*Invalid*|*not*found*|*No\ such*|*Cannot*|*cannot*)
	echo "  a group member got past the gate: $(printf '%s' "$out" | head -c 90)" ;;
*)
	# An answer we cannot place is not evidence either way: say so.
	echo "  a group member's call answered (not a refusal, not a known" \
		"next-check): $(printf '%s' "$out" | head -c 90)" ;;
esac

# Uninstall is a transaction the CLI owns: the helper must exec the CLI,
# not call the deploy primitive (which would skip world/orphan handling).
un=$(awk '/^static void handle_uninstall/ { f=1 }
	f && /^static .*\(/ && !/handle_uninstall/ { exit }
	f' "$SRC")
printf '%s' "$un" | grep -q 'fpkg_helper_command' ||
	note "Uninstall does not read the helper command map"
printf '%s' "$un" | grep -q 'mapped == 0' ||
	note "Uninstall does not refuse an unconfigured request"
printf '%s' "$un" | grep -q 'remove_deploy_ref' &&
	note "Uninstall still falls back to the deploy primitive"
printf '%s' "$un" | grep -q 'ostree_uninstall_from_helper' &&
	note "Uninstall still calls the old deploy primitive directly"

# InstallBundle returns the ref from the bundle, not from the CLI's
# width-truncated log line.
ib=$(awk '/^handle_install_bundle/ { f=1 }
	f && /^static .*\(/ && !/handle_install_bundle/ { exit }
	f' "$SRC")
printf '%s' "$ib" | grep -q 'ostree_bundle_ref(bundle_path, &ref)' ||
	note "InstallBundle does not read the ref from the bundle"
printf '%s' "$ib" | grep -q 'run_helper_command(argv, argc, &ref)' &&
	note "InstallBundle still takes the ref from captured CLI output"

# A bundle deploys through the store repo: ostree_deploy_from_repo(NULL).
# The old assert turned every bundle install into a daemon abort.
dep=$(awk '/^int ostree_deploy_from_repo/ { f=1 }
	f && /^}/ { print; exit }
	f' src/ostree_cmds.c)
printf '%s' "$dep" | grep -q 'assert(child_repo != NULL)' &&
	note "ostree_deploy_from_repo still rejects the NULL child repo"
# ostree_config_has() asks without a buffer.
cfg=$(awk '/^static int ostree_config_get/ { f=1 }
	f && /^}/ { print; exit }
	f' src/ostree_cmds.c)
printf '%s' "$cfg" | grep -q 'assert(out != NULL)' &&
	note "ostree_config_get still rejects a NULL output buffer"

echo
if [ "$fail" -eq 0 ]; then
	echo "PASS: the root system helper resolves the caller's uid from the bus"
	echo "      daemon, admits root and the flatpkg group, and every handler"
	echo "      asks before it touches the store."
fi
exit $fail
