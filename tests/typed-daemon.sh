#!/bin/sh
# typed-daemon.sh - a command sent over the framed typed protocol runs on a real flatpkgd
# and its answer reaches the client.  Socket and store are COMPILE-TIME settings, so the
# live daemon is never touched.  SKIPS without root via mdo.
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if ! command -v cc >/dev/null 2>&1 || ! command -v make >/dev/null 2>&1; then
	echo "SKIP: no cc or make in PATH"
	exit 0
fi
if ! command -v mdo >/dev/null 2>&1 || ! mdo -u root true </dev/null >/dev/null 2>&1; then
	echo "SKIP: mdo cannot give root without a prompt"
	exit 0
fi

tmp=$(mktemp -d /tmp/.fpkg-typed-daemon.XXXXXX)
dpid=""
hogs=""
cleanup() {
	[ -n "$hogs" ] && kill $hogs >/dev/null 2>&1 || true
	[ -n "$dpid" ] && mdo kill "$dpid" >/dev/null 2>&1 || true
	mdo rm -rf "$tmp" >/dev/null 2>&1 || rm -rf "$tmp"
}
trap cleanup EXIT

mkdir -p "$tmp/s" "$tmp/store"
chmod 755 "$tmp"
jobs=$(sysctl -n hw.ncpu 2>/dev/null || echo 2)
# A short ldconfig timeout, for the hung-ldconfig case below.
if ! (cd "$ROOT" && CFLAGS="${CFLAGS:-} -DFLATPKG_LDCONFIG_TIMEOUT_S=3" make -j"$jobs" BUILD="$tmp/build" \
	SOCKET_PATH="$tmp/s/control.sock" FLATPKG_STORE_DIR="$tmp/store" \
	"$tmp/build/flatpkg" \
	"$tmp/build/flatpkgd") > "$tmp/make.log" 2>&1; then
	echo "FAIL: the scratch build did not build:"
	tail -n 15 "$tmp/make.log"
	exit 1
fi

# FLATPKG_TD_VALGRIND=DIR runs the daemon under valgrind and leaves one report per
# process in DIR, for tests/memcheck.sh to read.  Many times slower; off by default.
wrap=""
socket_wait=50
if [ -n "${FLATPKG_TD_VALGRIND:-}" ]; then
	if ! command -v valgrind >/dev/null 2>&1; then
		echo "SKIP: FLATPKG_TD_VALGRIND is set but there is no valgrind"
		exit 0
	fi
	mkdir -p "$FLATPKG_TD_VALGRIND"
	wrap="valgrind --log-file=$FLATPKG_TD_VALGRIND/%p.log --leak-check=full --show-leak-kinds=definite,indirect --errors-for-leak-kinds=definite,indirect --track-origins=yes --trace-children=no"
	socket_wait=1800
fi
# FLATPKG_TD_SANITIZER_LOGS=DIR is for a build made with -fsanitize=...: the daemon
# runs as root with its stderr in a directory this script deletes, so the sanitizers
# write one file per process into DIR.
sanenv=""
if [ -n "${FLATPKG_TD_SANITIZER_LOGS:-}" ]; then
	mkdir -p "$FLATPKG_TD_SANITIZER_LOGS"
	sanenv="UBSAN_OPTIONS=log_path=$FLATPKG_TD_SANITIZER_LOGS/ubsan ASAN_OPTIONS=detect_leaks=0:log_path=$FLATPKG_TD_SANITIZER_LOGS/asan"
	export UBSAN_OPTIONS="log_path=$FLATPKG_TD_SANITIZER_LOGS/ubsan-client"
	export ASAN_OPTIONS="detect_leaks=0:log_path=$FLATPKG_TD_SANITIZER_LOGS/asan-client"
fi
mdo sh -c "$sanenv $wrap $tmp/build/flatpkgd > $tmp/daemon.log 2>&1 & echo \$! > $tmp/pid"
dpid=$(cat "$tmp/pid")
n=0
while [ ! -S "$tmp/s/control.sock" ] && [ "$n" -lt "$socket_wait" ]; do
	sleep 0.1
	n=$((n + 1))
done
if [ ! -S "$tmp/s/control.sock" ]; then
	echo "FAIL: the scratch daemon did not create its socket"
	cat "$tmp/daemon.log"
	exit 1
fi

bin="$tmp/build/flatpkg"
# A second client for the bus: follow an operation and cancel one.
busprobe=""
if cc -O2 -Wall -Wextra -I"$ROOT/include" -o "$tmp/busprobe" \
    "$ROOT/tests/busprobe.c" -L"$tmp/build" -lflatpkg_shared \
    -Wl,-rpath,"$tmp/build" >"$tmp/busprobe.cc" 2>&1; then
	busprobe="$tmp/busprobe"
fi
fail=0
note() {
	echo "FAIL: $*"
	fail=1
}
# run ARGS...: rc, and stdout+stderr with colour stripped in $tmp/out.
run() {
	# One run; the status is captured with || so set -e cannot end the guard on a
	# command that is supposed to fail.
	NO_COLOR=1 TERM=dumb timeout 60 "$bin" "$@" < /dev/null > "$tmp/raw" 2>&1 &&
		rc=0 || rc=$?
	sed 's/\033\[[0-9;?]*[A-Za-z]//g' "$tmp/raw" > "$tmp/out"
}

# 1. a failure comes back as a failure, with the daemon's own words.
run kill no-such-instance-xyz
[ "$rc" -eq 1 ] || note "kill of a missing instance exited $rc, not 1"
grep -q "neither a running instance" "$tmp/out" ||
	note "kill: the daemon's reason did not reach the client"
grep -q "Input/output error" "$tmp/out" &&
	note "kill: the client could not talk to the daemon"

run remote-delete no-such-remote-xyz
[ "$rc" -eq 1 ] || note "remote-delete of a missing remote exited $rc, not 1"
grep -q "no-such-remote-xyz" "$tmp/out" ||
	note "remote-delete: the daemon's reason did not reach the client"

# A remote change is published on the `remote` topic.
"$busprobe" watch-topic-any "$tmp/s/control.sock" remote \
	>"$tmp/bus.remote" 2>&1 &
rpid=$!
n=0
while [ $n -lt 80 ]; do
	grep -q '^sub=ok' "$tmp/bus.remote" && break
	sleep 0.1
	n=$((n + 1))
done
"$bin" remote-add testbus file:///tmp/fpkg-no-repo >/dev/null 2>&1 || true
"$bin" remote-delete testbus >/dev/null 2>&1 || true
wait "$rpid" 2>/dev/null || true
grep -q '^event=remote name=testbus' "$tmp/bus.remote" ||
	note "a remote change was not published on the remote topic"

run rollback org.example.NotInstalled abc123
[ "$rc" -eq 1 ] || note "rollback of a missing app exited $rc, not 1"
grep -q "not installed" "$tmp/out" ||
	note "rollback: the daemon's reason did not reach the client"

# 2. success is success, and --simulate is honoured (repair must not repair).
run repair --simulate
[ "$rc" -eq 0 ] || note "repair --simulate exited $rc, not 0"
grep -q "repair" "$tmp/out" || note "repair: no line from the daemon reached the client"

# 3. install and uninstall do the work: a local OSTree repository with one app,
# added as a remote through the daemon, installed with -y, seen in the scratch
# store, removed again.
if command -v ostree >/dev/null 2>&1; then
	mkdir -p "$tmp/app/files/bin"
	printf '[Application]\nname=org.test.Hello\ncommand=hello\n' > "$tmp/app/metadata"
	printf '#!/bin/sh\necho hi\n' > "$tmp/app/files/bin/hello"
	ostree init --repo="$tmp/repo" --mode=archive &&
		ostree commit --repo="$tmp/repo" --tree=dir="$tmp/app" -m v1 \
			--branch=app/org.test.Hello/x86_64/stable >/dev/null &&
		ostree summary -u --repo="$tmp/repo" ||
		note "could not build the fixture repository"
	run remote-add fixture "file://$tmp/repo"
	[ "$rc" -eq 0 ] || note "remote-add exited $rc, not 0"
	# A client's state is its own: another client name writes under its own directory
	# and cannot see flatpkg's world.
	"$busprobe" state-write "$tmp/s/control.sock" note hello \
		>"$tmp/state.write" 2>&1 || true
	grep -q '^state=ok' "$tmp/state.write" ||
		note "a client could not write its own state"
	[ -f "$tmp/store/clients/busprobe/note" ] ||
		note "the client state file is not in clients/busprobe/"
	"$busprobe" state-read "$tmp/s/control.sock" note \
		>"$tmp/state.read" 2>&1 || true
	grep -q '^event=client-state name=note data=hello' "$tmp/state.read" ||
		note "a client could not read the state it wrote"
	"$busprobe" state-undeclared "$tmp/s/control.sock" other bad \
		>"$tmp/state.undeclared" 2>&1 || true
	grep -q '^state=denied' "$tmp/state.undeclared" ||
		note "a client wrote a state file it did not declare"
	[ -e "$tmp/store/clients/busprobe/other" ] &&
		note "an undeclared state file was created"
	[ -e "$tmp/store/world" ] &&
		note "the daemon still writes a store-root world file"
	run install -y org.test.Hello
	[ "$rc" -eq 0 ] || note "install -y exited $rc, not 0"
	[ -d "$tmp/store/app/org.test.Hello" ] ||
		note "install -y reported success and installed nothing"
	[ -f "$tmp/store/clients/flatpkg/world" ] ||
		note "install did not write the client's world file"
	grep -q 'org.test.Hello' "$tmp/store/clients/flatpkg/world" ||
		note "the client's world file does not name the installed app"
	[ -f "$tmp/store/clients/busprobe/note" ] ||
		note "the install disturbed another client's state"

	if [ -n "$busprobe" ]; then
		"$busprobe" subscribe "$tmp/s/control.sock" op \
			>"$tmp/bus.sub" 2>&1 || true
		grep -q '^sub=ok' "$tmp/bus.sub" ||
			note "SUBSCRIBE was not acknowledged"
		"$busprobe" cancel "$tmp/s/control.sock" 0 \
			>"$tmp/bus.cancel" 2>&1 || true
		grep -q '^ack=ok' "$tmp/bus.cancel" ||
			note "CANCEL was not acknowledged"
		"$busprobe" watch-topic "$tmp/s/control.sock" op \
			>"$tmp/bus.watch" 2>&1 &
		wpid=$!
		n=0
		while [ $n -lt 80 ]; do
			grep -q '^sub=ok' "$tmp/bus.watch" && break
			sleep 0.1
			n=$((n + 1))
		done
		birc=0
		"$bin" install -y org.test.Hello \
			>"$tmp/bus.install" 2>&1 || birc=$?
		wait "$wpid" 2>/dev/null || true
		[ "$birc" -eq 0 ] ||
			note "the bus install failed: $(head -3 "$tmp/bus.install")"
		grep -q '^event=started' "$tmp/bus.watch" ||
			note "a watcher did not receive the started event"
		grep -q '^event=finished' "$tmp/bus.watch" ||
			note "a watcher did not receive the finished event"
	fi
	# Two refs are one transaction: one table, not one per ref.  The second app is
	# removed again so the update/upgrade cases below still see a single-app world.
	mkdir -p "$tmp/app2/files/bin" "$tmp/platform/files"
	printf '[Runtime]\nname=org.test.Platform\n' \
		> "$tmp/platform/metadata"
	ostree commit --repo="$tmp/repo" --tree=dir="$tmp/platform" -m platform \
		--branch=runtime/org.test.Platform/x86_64/stable >/dev/null
	printf '[Application]\nname=org.test.Second\ncommand=second\n' \
		> "$tmp/app2/metadata"
	printf 'runtime=org.test.Platform/x86_64/stable\n' \
		>> "$tmp/app2/metadata"
	printf '#!/bin/sh\necho second\n' > "$tmp/app2/files/bin/second"
	ostree commit --repo="$tmp/repo" --tree=dir="$tmp/app2" -m second \
		--branch=app/org.test.Second/x86_64/stable >/dev/null &&
		ostree summary -u --repo="$tmp/repo" ||
		note "could not build the second fixture app"
	run install -y org.test.Hello org.test.Second
	[ "$rc" -eq 0 ] || note "batched install -y exited $rc, not 0"
	[ -d "$tmp/store/app/org.test.Second" ] ||
		note "batched install -y installed only the first ref"
	[ -d "$tmp/store/runtime/org.test.Platform" ] ||
		note "the second app's runtime dependency was not installed: $(grep -i 'platform\|runtime' "$tmp/out" | tr '\n' '|' | cut -c1-400)"
	[ "$(grep -c 'ID *Arch *Branch *Operation *Remote' "$tmp/out")" -eq 1 ] ||
		note "batched install drew more than one plan table"
	grep -q 'org.test.Second' "$tmp/out" ||
		note "batched install's table did not name the second ref"
	run uninstall -y org.test.Second
	[ "$rc" -eq 0 ] || note "cleanup uninstall of the second app exited $rc"
	# apk del semantics: the runtime the app pulled in is an orphan now and goes
	# with it, without waiting for `upgrade --prune`.
	[ -e "$tmp/store/runtime/org.test.Platform" ] &&
		note "uninstall left the app's now-unreferenced runtime behind"
	# The store snapshot holds the last events, so a watcher that arrives just after
	# the removal still sees it.
	"$busprobe" watch-topic-any "$tmp/s/control.sock" store \
		>"$tmp/bus.store" 2>&1 || true
	grep -q '^event=store action=remove ref=.*org.test.Second' \
		"$tmp/bus.store" ||
		note "a removal was not published on the store topic: watch=[$(tr '\n' ' ' <"$tmp/bus.store")] daemon=[$(grep 'EMIT store' "$tmp/daemon.log" | tail -10 | tr '\n' ' ')]"
	run install -y org.test.NotInTheRemote
	[ "$rc" -eq 1 ] || note "install of an unknown app exited $rc, not 1"
	# A newer commit in the remote: update brings it in, and a further one is left for
	# upgrade -y; --simulate must change nothing.
	act="$tmp/store/app/org.test.Hello/x86_64/stable/active"
	newcommit() {
		printf '#!/bin/sh\necho "%s"\n' "$1" > "$tmp/app/files/bin/hello"
		ostree commit --repo="$tmp/repo" --tree=dir="$tmp/app" -m "$1" \
			--branch=app/org.test.Hello/x86_64/stable >/dev/null &&
			ostree summary -u --repo="$tmp/repo" ||
			note "could not commit $1 to the fixture repository"
	}
	active() { readlink "$act" 2>/dev/null || echo none; }
	# update is a plan: the delta is shown and confirmed before the store changes.
	# No terminal here, so the question fails with "pass -y" and the old commit stays.
	before=$(active)
	newcommit v2
	run update
	[ "$rc" -eq 1 ] || note "update without -y and without a terminal exited $rc, not 1"
	grep -q "org.test.Hello" "$tmp/out" ||
		note "update without -y: the plan did not reach the caller"
	grep -q "pass -y" "$tmp/out" ||
		note "update without -y: the question was not put to the caller"
	[ "$(active)" = "$before" ] ||
		note "update without -y changed the active commit before anyone was asked"
	run update -y
	[ "$rc" -eq 0 ] || note "update -y exited $rc, not 0"
	[ "$(active)" != "$before" ] ||
		note "update -y reported success and brought nothing in"
	before=$(active)
	newcommit v3
	run upgrade --simulate
	[ "$rc" -eq 0 ] || note "upgrade --simulate exited $rc, not 0"
	[ "$(active)" = "$before" ] ||
		note "upgrade --simulate changed the active commit"
	run upgrade -y
	[ "$rc" -eq 0 ] || note "upgrade -y exited $rc, not 0"
	[ "$(active)" != "$before" ] ||
		note "upgrade -y reported success and deployed nothing"
	# Without -y the daemon only PLANS: it shows the changeset and stops at the
	# question, and the client is the one that asks.  No terminal here, so the
	# question fails with "pass -y" - the plan reached the caller and nothing changed.
	before=$(active)
	newcommit v4
	run upgrade
	[ "$rc" -eq 1 ] || note "upgrade without -y and without a terminal exited $rc, not 1"
	grep -q "pass -y" "$tmp/out" ||
		note "upgrade without -y: the question was not put to the caller"
	[ "$(active)" = "$before" ] ||
		note "upgrade without -y deployed before anyone was asked"
	run uninstall org.test.Hello
	[ "$rc" -eq 1 ] || note "uninstall without -y and without a terminal exited $rc, not 1"
	grep -q "org.test.Hello" "$tmp/out" ||
		note "uninstall without -y: the plan did not reach the caller"
	grep -q "pass -y" "$tmp/out" ||
		note "uninstall without -y: the question was not put to the caller"
	[ -d "$tmp/store/app/org.test.Hello" ] ||
		note "uninstall without -y removed the app before anyone was asked"
	# --simulate is the plan without the question: the table is shown, nothing removed.
	run uninstall --simulate org.test.Hello
	[ "$rc" -eq 0 ] || note "uninstall --simulate exited $rc, not 0"
	grep -q "simulate: no changes made" "$tmp/out" ||
		note "uninstall --simulate did not say it simulated"
	[ -d "$tmp/store/app/org.test.Hello" ] ||
		note "uninstall --simulate removed the app"
	run uninstall -y org.test.Hello
	# Not a terminal here: no progress-bar redraws in the output.
	[ "$(grep -c '[█▏▎▍▌▋▊▉]' "$tmp/out")" -eq 0 ] ||
		note "uninstall -y wrote progress-bar frames into a pipe"
	[ "$rc" -eq 0 ] || note "uninstall -y exited $rc, not 0"
	[ -e "$tmp/store/app/org.test.Hello" ] &&
		note "uninstall -y reported success and removed nothing"
	run install --simulate org.test.Hello//stable
	[ "$rc" -eq 0 ] || note "install --simulate exited $rc, not 0"
	grep -q "simulate: no changes made" "$tmp/out" ||
		note "install --simulate did not say it simulated"
	[ -e "$tmp/store/app/org.test.Hello" ] &&
		note "install --simulate installed the app"
	# A partial name is resolved by the CLI: the daemon takes exact refs, and the
	# request stays typed.
	run install --simulate Hello
	[ "$rc" -eq 0 ] || note "install --simulate a partial name exited $rc, not 0"
	grep -q "org.test.Hello" "$tmp/out" ||
		note "a partial name was not resolved to a ref the daemon could use"
	[ -e "$tmp/store/app/org.test.Hello" ] &&
		note "install --simulate a partial name installed the app"
	# install without -y, for a fully pinned ref: planned, shown, not installed.
	run install org.test.Hello//stable
	[ "$rc" -eq 1 ] || note "install without -y and without a terminal exited $rc, not 1"
	grep -q "org.test.Hello" "$tmp/out" ||
		note "install without -y: the plan did not reach the caller"
	grep -q "pass -y" "$tmp/out" ||
		note "install without -y: the question was not put to the caller"
	[ -e "$tmp/store/app/org.test.Hello" ] &&
		note "install without -y installed the app before anyone was asked"
	# The answer itself needs a terminal; expect provides one where it exists.
	if command -v expect >/dev/null 2>&1; then
		cat > "$tmp/ask.exp" <<'EXP'
set timeout 120
set answer [lindex $argv 0]
spawn {*}[lrange $argv 1 end]
expect {
	-re {Proceed} { send "$answer\r" }
	timeout { exit 98 }
	eof { exit 99 }
}
expect eof
catch wait r
exit [lindex $r 3]
EXP
		NO_COLOR=1 expect "$tmp/ask.exp" n "$bin" install org.test.Hello//stable \
			> "$tmp/ask.out" 2>&1 && rc=0 || rc=$?
		[ "$rc" -eq 1 ] || note "install answered no exited $rc, not 1 (98: never asked)"
		[ -e "$tmp/store/app/org.test.Hello" ] &&
			note "install answered no installed the app anyway"
		NO_COLOR=1 expect "$tmp/ask.exp" y "$bin" install org.test.Hello//stable \
			> "$tmp/ask.out" 2>&1 && rc=0 || rc=$?
		[ "$rc" -eq 0 ] || note "install answered yes exited $rc, not 0 (98: never asked)"
		[ -d "$tmp/store/app/org.test.Hello" ] ||
			note "install answered yes and installed nothing"
		NO_COLOR=1 expect "$tmp/ask.exp" y "$bin" uninstall org.test.Hello \
			> "$tmp/ask.out" 2>&1 && rc=0 || rc=$?
		[ "$rc" -eq 0 ] || note "uninstall answered yes exited $rc, not 0 (98: never asked)"
		[ -e "$tmp/store/app/org.test.Hello" ] &&
			note "uninstall answered yes and removed nothing"
	else
		echo "NOTE: no expect; the yes/no answer to a plan was not exercised"
	fi

	# The same answers through tests/ptyrun.py, which needs no expect, and the keys
	# that mean "stop": Ctrl-C, Ctrl-D and Escape are a no whatever the prompt's
	# default is ("Proceed with these changes?" defaults to yes).
	if command -v python3 >/dev/null 2>&1; then
		ask() { # ask KEY ARGS...: run flatpkg on a terminal, answer the question with KEY
			_key="$1"
			shift
			NO_COLOR=1 TERM=xterm python3 "$ROOT/tests/ptyrun.py" \
				--wait Proceed --send "$_key" --timeout 120 "$tmp/ask.out" "$bin" "$@" \
				&& rc=0 || rc=$?
		}
		ask 'n' install org.test.Hello//stable
		[ "$rc" -eq 1 ] || note "install answered no exited $rc, not 1 (124: never asked)"
		[ -e "$tmp/store/app/org.test.Hello" ] &&
			note "install answered no installed the app anyway"
		for key in '\x03' '\x04' '\x1b'; do
			ask "$key" install org.test.Hello//stable
			[ "$rc" -ne 0 ] ||
				note "install cancelled with key $key exited 0: the question was answered yes"
			[ "$rc" -ne 124 ] ||
				note "install cancelled with key $key hung: the key did not end the question"
			[ -e "$tmp/store/app/org.test.Hello" ] &&
				note "install cancelled with key $key installed the app"
			rm -rf "$tmp/ask-keep" 2>/dev/null || true
		done
		ask 'y' install org.test.Hello//stable
		[ "$rc" -eq 0 ] || note "install answered yes exited $rc, not 0 (124: never asked)"
		[ -d "$tmp/store/app/org.test.Hello" ] ||
			note "install answered yes and installed nothing"
		cp "$tmp/ask.out" "$tmp/install.transcript"
		for key in '\x03' '\x04' '\x1b'; do
			ask "$key" uninstall org.test.Hello
			[ "$rc" -ne 0 ] ||
				note "uninstall cancelled with key $key exited 0: the question was answered yes"
			[ -d "$tmp/store/app/org.test.Hello" ] ||
				note "uninstall cancelled with key $key removed the app"
		done
		ask 'y' uninstall org.test.Hello
		[ "$rc" -eq 0 ] || note "uninstall answered yes exited $rc, not 0 (124: never asked)"
		[ -e "$tmp/store/app/org.test.Hello" ] &&
			note "uninstall answered yes and removed nothing"
		cp "$tmp/ask.out" "$tmp/uninstall.transcript"
		# The plan is the same table whichever side worked it out: the daemon sends
		# an uninstall's rows as data (FP_EV_OP_TABLE) and this side draws them,
		# header and all, so removing an app looks like installing one.
		for t in install uninstall; do
			tr -d '\r' < "$tmp/$t.transcript" | sed 's/\x1b\[[0-9;?]*[A-Za-z]//g' > "$tmp/$t.plain"
			grep -Eq 'ID +Arch +Branch +Operation +Remote' "$tmp/$t.plain" ||
				note "$t: the plan is not drawn as a table with a header: $(head -n 6 "$tmp/$t.plain" | tr '\n' '|' | cut -c1-300)"
			grep -Eq '^daemon +org.test.Hello' "$tmp/$t.plain" &&
				note "$t: the plan came as log lines from the daemon"
		done
	fi
else
	echo "NOTE: ostree not in PATH; install and uninstall were not exercised"
fi

# install --from: the bundle is handed over as an open descriptor, and what the
# daemon makes of it comes back.  Not a file at all: refused by the client first.
head -c 4096 /dev/urandom > "$tmp/not-a-bundle"
run install --from "$tmp/not-a-bundle"
[ "$rc" -eq 1 ] || note "install --from a file that is not a bundle exited $rc, not 1"
grep -qi "bundle" "$tmp/out" ||
	note "install --from: the daemon's reason did not reach the client"
run install --from "$tmp/does-not-exist"
[ "$rc" -eq 1 ] || note "install --from a missing file exited $rc, not 1"
grep -q "cannot open" "$tmp/out" || note "install --from a missing file: no reason given"
: > "$tmp/empty-bundle"
run install --from "$tmp/empty-bundle"
[ "$rc" -eq 1 ] || note "install --from an empty file exited $rc, not 1"
grep -qi "empty" "$tmp/out" || note "install --from an empty file: no reason given"

# A runnable app for the `run` check: a Linux echo and just the libraries it loads, as
# an app and its runtime - small on purpose, since a whole userland takes minutes to
# copy and to assemble a jail from.  In place only now: the daemon owns the store.
LINUX=/compat/linux
have_run=0
if [ -x "$LINUX/bin/echo" ] && [ -e "$LINUX/lib64/ld-linux-x86-64.so.2" ] &&
	[ -e "$LINUX/lib64/libc.so.6" ]; then
	rt="$tmp/store/runtime/org.freedesktop.Platform/x86_64/23.08/active"
	ap="$tmp/store/app/org.test.Echo/x86_64/23.08/active"
	mdo mkdir -p "$rt/files/lib64" "$ap/files/bin"
	for l in ld-linux-x86-64.so.2 libc.so.6 libpthread.so.0 libdl.so.2 \
		librt.so.1 libm.so.6; do
		[ -e "$LINUX/lib64/$l" ] && mdo cp -L "$LINUX/lib64/$l" "$rt/files/lib64/"
	done
	mdo cp "$LINUX/bin/echo" "$ap/files/bin/hello"
	printf '[Runtime]\nname=org.freedesktop.Platform\n' > "$tmp/rt.meta"
	printf '[Application]\nname=org.test.Echo\nruntime=org.freedesktop.Platform/x86_64/23.08\ncommand=hello\n' \
		> "$tmp/ap.meta"
	# A runtime ldconfig that never returns: the launch must not wait for it.
	mdo mkdir -p "$rt/files/sbin"
	printf '#!/bin/sh\nexec sleep 600\n' > "$tmp/ldconfig.hang"
	mdo cp "$tmp/ldconfig.hang" "$rt/files/sbin/ldconfig"
	mdo chmod 755 "$rt/files/sbin/ldconfig"
	mdo cp "$tmp/rt.meta" "$rt/metadata"
	mdo cp "$tmp/ap.meta" "$ap/metadata"
	have_run=1
fi

# 4. run, with stdout and stderr not terminals: the app's own descriptors are the
# caller's, so its output lands in the caller's file and its status is the command's.
run run org.test.NotInstalled
[ "$rc" -eq 1 ] || note "run of an app that is not installed exited $rc, not 1"
grep -q "not installed" "$tmp/out" ||
	note "run: the daemon's reason did not reach the client"
if [ "$have_run" -eq 1 ]; then
	"$busprobe" watch-topic-any "$tmp/s/control.sock" \
		instance/org.test.echo >"$tmp/bus.instance" 2>&1 &
	ipid=$!
	n=0
	while [ $n -lt 80 ]; do
		grep -q '^sub=ok' "$tmp/bus.instance" && break
		sleep 0.1
		n=$((n + 1))
	done
	NO_COLOR=1 TERM=dumb timeout 90 "$bin" run org.test.Echo typed run ok \
		< /dev/null > "$tmp/run.out" 2> "$tmp/run.err" && rc=0 || rc=$?
	[ "$rc" -eq 0 ] || note "run exited $rc, not 0 (124 is a hang)"
	wait "$ipid" 2>/dev/null || true
	grep -q '^event=instance-started app=org.test.Echo' \
		"$tmp/bus.instance" ||
		note "a launch was not published on its instance topic: daemon=[$(grep 'EMIT instance' "$tmp/daemon.log" | tail -5 | tr '\n' ' ')]"
	grep -q '^event=instance-exited app=org.test.Echo' \
		"$tmp/bus.instance" ||
		note "a launch exit was not published on its instance topic"
	[ "$(cat "$tmp/run.out")" = "typed run ok" ] ||
		note "run: the app's output did not reach the caller's stdout"
	grep -q "ldconfig did not finish" "$tmp/run.err" ||
		note "run: a hung ldconfig was not given up on (or not reported)"
	NO_COLOR=1 TERM=dumb timeout 90 "$bin" run --command=/bin/false org.test.Echo \
		< /dev/null > "$tmp/run.out" 2> "$tmp/run.err" && rc=0 || rc=$?
	[ "$rc" -ne 0 ] || note "run: a failing command was reported as success"

	# Forwarded descriptors: the portal's Spawn hands them to the app on the numbers
	# it names (FLATPKG_FORWARD_FDS).  A static Linux reader reads fd 9, so the claim
	# is about the descriptor the app holds, not about the request.
	if command -v zig >/dev/null 2>&1; then
		printf '#include <unistd.h>\nint main(void){char b[64];ssize_t n=read(9,b,sizeof b);if(n<=0)return 1;return write(1,b,(size_t)n)==n?0:3;}\n' \
			> "$tmp/readfd.c"
		if zig cc --target=x86_64-linux-musl -static \
			-o "$tmp/readfd" "$tmp/readfd.c" > "$tmp/zig.log" 2>&1; then
			mdo cp "$tmp/readfd" "$ap/files/bin/readfd"
			mdo chmod 755 "$ap/files/bin/readfd"
			printf 'through fd 9\n' > "$tmp/fd9"
			NO_COLOR=1 TERM=dumb timeout 90 \
				env FLATPKG_FORWARD_FDS=9 "$bin" run \
				--command=readfd org.test.Echo < /dev/null \
				> "$tmp/fwd.out" 2> "$tmp/fwd.err" 9< "$tmp/fd9" &&
				rc=0 || rc=$?
			[ "$rc" -eq 0 ] ||
				note "run with a forwarded descriptor exited $rc, not 0"
			[ "$(cat "$tmp/fwd.out")" = "through fd 9" ] ||
				note "run: the forwarded descriptor did not reach the app"
			# Control: the same reader, no descriptor.  It must fail, or the
			# case above is about a reader that ignores its input.
			NO_COLOR=1 TERM=dumb timeout 90 "$bin" run --command=readfd \
				org.test.Echo < /dev/null > "$tmp/fwd.out" \
				2> "$tmp/fwd.err" 9< "$tmp/fd9" && rc=0 || rc=$?
			[ "$rc" -ne 0 ] ||
				note "run without FLATPKG_FORWARD_FDS still found descriptor 9"
			# A number the caller does not hold is refused before the launch.
			NO_COLOR=1 TERM=dumb timeout 90 \
				env FLATPKG_FORWARD_FDS=8 "$bin" run \
				--command=readfd org.test.Echo < /dev/null \
				> "$tmp/fwd.out" 2> "$tmp/fwd.err" 9< "$tmp/fd9" &&
				rc=0 || rc=$?
			[ "$rc" -eq 1 ] ||
				note "run with an unopened forwarded descriptor exited $rc, not 1"
			grep -q "FLATPKG_FORWARD_FDS" "$tmp/fwd.err" ||
				note "run with an unopened forwarded descriptor gave no reason"
		else
			echo "NOTE: zig could not build the static fd reader; forwarding was not exercised"
		fi
	else
		echo "NOTE: no zig; forwarded descriptors were not exercised"
	fi

	# A client that disconnects mid-run must end the app: the worker watches the
	# control socket and turns EOF into the run's termination.  The sleeper prints its
	# own pid, so the case watches the process rather than an exit status.
	if command -v zig >/dev/null 2>&1; then
		cat > "$tmp/sleeper.c" <<'EOF'
#include <stdio.h>
#include <unistd.h>
int main(void) {
	printf("%d\n", (int)getpid());
	fflush(stdout);
	for (;;) pause();
}
EOF
		if zig cc --target=x86_64-linux-musl -static \
			-o "$tmp/sleeper" "$tmp/sleeper.c" >"$tmp/zig-sleeper.log" 2>&1; then
			mdo cp "$tmp/sleeper" "$ap/files/bin/sleeper"
			mdo chmod 755 "$ap/files/bin/sleeper"
			NO_COLOR=1 TERM=dumb "$bin" run --command=sleeper \
				org.test.Echo </dev/null >"$tmp/watch.out" \
				2>"$tmp/watch.err" &
			cpid=$!
			n=0
			apid=""
			while [ $n -lt 80 ]; do
				apid=$(tr -cd '0-9\n' <"$tmp/watch.out" 2>/dev/null | head -1)
				[ -n "$apid" ] && break
				sleep 0.1
				n=$((n + 1))
			done
			if [ -z "$apid" ]; then
				note "the run watcher case never got the app's pid"
				kill "$cpid" 2>/dev/null || true
				wait "$cpid" 2>/dev/null || true
			else
				kill -TERM "$cpid" 2>/dev/null || true
				wait "$cpid" 2>/dev/null || true
				n=0
				while [ $n -lt 80 ] && kill -0 "$apid" 2>/dev/null; do
					sleep 0.1
					n=$((n + 1))
				done
				if kill -0 "$apid" 2>/dev/null; then
					note "the app outlived the client that started it (pid $apid)"
					kill -KILL "$apid" 2>/dev/null || true
				fi
			fi
		fi
	fi

	# A terminal on stdin, stdout and stderr: the app gets a pty of the daemon's own and the
	# caller's terminal is relayed to it.  The static probe reports what the app holds - a
	# terminal, the caller's size, a device that is not the caller's own.  "flood N" writes
	# N KiB and exits at once ('#' only: letters also occur in the scratch dir's name).
	if command -v zig >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
		cat > "$tmp/ttyprobe.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc, char **argv) {
	if (argc > 1 && strcmp(argv[1], "flood") == 0) {
		char chunk[1024];
		int kb = argc > 2 ? atoi(argv[2]) : 200;
		memset(chunk, '#', sizeof chunk);
		for (int i = 0; i < kb; i++)
			if (write(1, chunk, sizeof chunk) != (ssize_t)sizeof chunk) return 3;
		return 0;
	}
	/* Where the probe reports.  stdout normally; stderr when stdout is a
	 * file or a pipe; fd 0 when it is stdin that is the terminal, so the
	 * report goes to the pty and the relay has somewhere to put it. */
	int report = 1;
	if (argc > 1 && strncmp(argv[1], "report=", 7) == 0)
		report = atoi(argv[1] + 7);
	struct winsize ws;
	struct stat st;
	memset(&ws, 0, sizeof ws);
	memset(&st, 0, sizeof st);
	int w = ioctl(0, TIOCGWINSZ, &ws);
	int s = fstat(0, &st);
	dprintf(report, "PROBE-WAITING\n");
	char b[128];
	ssize_t n = read(0, b, sizeof b - 1);
	if (n < 0) n = 0;
	while (n > 0 && (b[n - 1] == '\n' || b[n - 1] == '\r')) n--;
	b[n] = 0;
	struct winsize ws2;
	memset(&ws2, 0, sizeof ws2);
	int w2 = ioctl(0, TIOCGWINSZ, &ws2);
	dprintf(report,
		"tty=%d tty0=%d tty1=%d tty2=%d rows=%d cols=%d rdev=%lx now=%dx%d got=%s\n",
		isatty(0) && isatty(1) && isatty(2), isatty(0), isatty(1),
		isatty(2), w == 0 ? ws.ws_row : -1,
		w == 0 ? ws.ws_col : -1, s == 0 ? (unsigned long)st.st_rdev : 0UL,
		w2 == 0 ? ws2.ws_row : -1, w2 == 0 ? ws2.ws_col : -1, b);
	return strcmp(b, "five") == 0 ? 5 : 0;
}
EOF
		if zig cc --target=x86_64-linux-musl -static \
			-o "$tmp/ttyprobe" "$tmp/ttyprobe.c" > "$tmp/zig.log" 2>&1; then
			mdo cp "$tmp/ttyprobe" "$ap/files/bin/ttyprobe"
			mdo chmod 755 "$ap/files/bin/ttyprobe"
			NO_COLOR=1 TERM=xterm python3 "$ROOT/tests/ptyrun.py" --rows 31 --cols 97 \
				--wait PROBE-WAITING --send 'hello\n' --timeout 90 "$tmp/pty.out" \
				"$bin" run --command=ttyprobe org.test.Echo && rc=0 || rc=$?
			[ "$rc" -eq 0 ] || note "run on a terminal exited $rc, not 0 (124 is a hang)"
			grep -q 'tty=1 tty0=1 tty1=1 tty2=1 rows=31 cols=97' "$tmp/pty.out" ||
				note "run on a terminal: the app did not get a terminal of the caller's size"
			grep -q 'got=hello' "$tmp/pty.out" ||
				note "run on a terminal: what the user typed did not reach the app"
			# Not the caller's own terminal: the device number is the pty's
			# unit on the Linux side (136, unit).
			utty=$(cat "$tmp/pty.out.tty" 2>/dev/null)
			unit=${utty##*/}
			rdev=$(sed -n 's/.*rdev=\([0-9a-f]*\).*/\1/p' "$tmp/pty.out" | head -n 1)
			echo "NOTE: caller's terminal $utty; the app saw rdev=$rdev"
			[ -n "$rdev" ] || note "run on a terminal: the app reported no device"
			if [ -n "$rdev" ] && [ "$((0x$rdev & 0xff))" -eq "$unit" ] 2>/dev/null; then
				note "run on a terminal: the app holds the caller's own terminal ($utty)"
			fi
			# The window changes while the app waits: it must see the new size,
			# and the one it started with must not have been that already.
			NO_COLOR=1 TERM=xterm python3 "$ROOT/tests/ptyrun.py" --rows 31 --cols 97 \
				--resize 40,120 --wait PROBE-WAITING --send 'resized\n' --timeout 90 \
				"$tmp/pty.out" "$bin" run --command=ttyprobe org.test.Echo \
				&& rc=0 || rc=$?
			[ "$rc" -eq 0 ] || note "run on a terminal (resize) exited $rc, not 0"
			grep -q 'rows=31 cols=97 .* now=40x120 ' "$tmp/pty.out" ||
				note "run on a terminal: a window resize did not reach the app"
			NO_COLOR=1 TERM=xterm python3 "$ROOT/tests/ptyrun.py" \
				--wait PROBE-WAITING --send 'five\n' --timeout 90 "$tmp/pty.out" \
				"$bin" run --command=ttyprobe org.test.Echo && rc=0 || rc=$?
			[ "$rc" -eq 5 ] ||
				note "run on a terminal: the app's exit status 5 came back as $rc"
			# A mixture of terminal and non-terminal stdio: each descriptor is handled
			# on its own, so the app gets a pty where the caller had a terminal and the
			# caller's own file or pipe elsewhere.  Each case has its own capture file.
			printf 'five\n' > "$tmp/mix.in"
			# stdin and stderr a terminal, stdout a file.
			OUT="$tmp/mix1.out" NO_COLOR=1 TERM=xterm \
			python3 "$ROOT/tests/ptyrun.py" --rows 31 --cols 97 \
				--wait PROBE-WAITING --send 'hello\n' --timeout 90 \
				"$tmp/mix1.pty" sh -c 'exec "$@" >"$OUT"' sh \
				"$bin" run --command=ttyprobe org.test.Echo report=2 \
				&& rc=0 || rc=$?
			[ "$rc" -eq 0 ] ||
				note "run (stdin and stderr a terminal, stdout a file) exited $rc, not 0"
			grep -q 'tty0=1 tty1=0 tty2=1 .*got=hello' "$tmp/mix1.pty" ||
				note "run (stdin and stderr a terminal, stdout a file): the app reported $(tr -d '\r' < "$tmp/mix1.pty" | tail -n 2 | tr '\n' ' ')"
			if [ -s "$tmp/mix1.out" ]; then
				note "run (stdin and stderr a terminal, stdout a file) wrote $(wc -c < "$tmp/mix1.out") bytes to the file"
			fi
			# stdin a file, stdout and stderr terminals.  The status the
			# input chooses comes back as 5.
			IN="$tmp/mix.in" NO_COLOR=1 TERM=xterm \
			python3 "$ROOT/tests/ptyrun.py" --rows 31 --cols 97 \
				--timeout 90 "$tmp/mix2.pty" \
				sh -c 'exec "$@" <"$IN"' sh \
				"$bin" run --command=ttyprobe org.test.Echo report=1 \
				&& rc=0 || rc=$?
			[ "$rc" -eq 5 ] ||
				note "run (stdin a file, stdout and stderr terminals) exited $rc, not 5"
			grep -q 'tty0=0 tty1=1 tty2=1 .*got=five' "$tmp/mix2.pty" ||
				note "run (stdin a file, stdout and stderr terminals): the app reported $(tr -d '\r' < "$tmp/mix2.pty" | tail -n 2 | tr '\n' ' ')"
			# stdin and stdout terminals, stderr a file.  What the
			# client itself logs goes to that file too, so only the
			# app's own descriptors are asserted.
			OUT="$tmp/mix3.err" NO_COLOR=1 TERM=xterm \
			python3 "$ROOT/tests/ptyrun.py" --rows 31 --cols 97 \
				--wait PROBE-WAITING --send 'hello\n' --timeout 90 \
				"$tmp/mix3.pty" sh -c 'exec "$@" 2>"$OUT"' sh \
				"$bin" run --command=ttyprobe org.test.Echo report=1 \
				&& rc=0 || rc=$?
			[ "$rc" -eq 0 ] ||
				note "run (stdin and stdout a terminal, stderr a file) exited $rc, not 0"
			grep -q 'tty0=1 tty1=1 tty2=0 .*got=hello' "$tmp/mix3.pty" ||
				note "run (stdin and stdout a terminal, stderr a file): the app reported $(tr -d '\r' < "$tmp/mix3.pty" | tail -n 2 | tr '\n' ' ')"
			# stdin and stdout files, stderr a terminal.
			OUT="$tmp/mix4.out" IN="$tmp/mix.in" NO_COLOR=1 TERM=xterm \
			python3 "$ROOT/tests/ptyrun.py" --rows 31 --cols 97 \
				--timeout 90 "$tmp/mix4.pty" \
				sh -c 'exec "$@" <"$IN" >"$OUT"' sh \
				"$bin" run --command=ttyprobe org.test.Echo report=2 \
				&& rc=0 || rc=$?
			[ "$rc" -eq 5 ] ||
				note "run (stdin and stdout files, stderr a terminal) exited $rc, not 5"
			grep -q 'tty0=0 tty1=0 tty2=1 .*got=five' "$tmp/mix4.pty" ||
				note "run (stdin and stdout files, stderr a terminal): the app reported $(tr -d '\r' < "$tmp/mix4.pty" | tail -n 2 | tr '\n' ' ')"
			if [ -s "$tmp/mix4.out" ]; then
				note "run (stdin and stdout files, stderr a terminal) wrote $(wc -c < "$tmp/mix4.out") bytes to the file"
			fi
			# Only stdin a terminal.  Nothing can print a marker this harness can
			# see, so the input goes out after a delay.  The pty's output must be
			# read and discarded, or the app blocks on a full terminal.
			OUT="$tmp/mix5.out" ERR="$tmp/mix5.err" NO_COLOR=1 TERM=xterm \
			python3 "$ROOT/tests/ptyrun.py" --rows 31 --cols 97 \
				--send-after 3 --send 'five\n' --timeout 90 "$tmp/mix5.pty" \
				sh -c 'exec "$@" >"$OUT" 2>"$ERR"' sh \
				"$bin" run --command=ttyprobe org.test.Echo report=1 \
				&& rc=0 || rc=$?
			[ "$rc" -eq 5 ] ||
				note "run (only stdin a terminal) exited $rc, not 5 (124 is a hang)"
			grep -q 'tty0=1 tty1=0 tty2=0 .*got=five' "$tmp/mix5.out" ||
				note "run (only stdin a terminal): the app reported $(tr -d '\r' < "$tmp/mix5.out" | tail -n 2 | tr '\n' ' ')"
			# A lot of output through the relay: the app writes N KiB and exits while the
			# caller's terminal is not read, so the relay blocks and every byte must still
			# arrive.  It only fails under CPU load, so the guard makes its own; bytes were
			# lost while the pty slave was still open in the worker's connection monitor.
			flood_reps=${FLATPKG_TD_FLOOD_REPEAT:-1}
			h=0
			while [ "$h" -lt $((jobs * 2)) ]; do
				sh -c 'while :; do :; done' &
				hogs="$hogs $!"
				h=$((h + 1))
			done
			for kb in 64 200; do
				rep=0
				while [ "$rep" -lt "$flood_reps" ]; do
					rep=$((rep + 1))
					NO_COLOR=1 TERM=xterm python3 "$ROOT/tests/ptyrun.py" \
						--pause 2 --timeout 90 "$tmp/pty.out" "$bin" run \
						--command=ttyprobe org.test.Echo flood "$kb" && rc=0 || rc=$?
					[ "$rc" -eq 0 ] ||
						note "run on a terminal (flood $kb KiB) exited $rc, not 0"
					got=$(tr -cd '#' < "$tmp/pty.out" | wc -c | tr -d ' ')
					echo "NOTE: flood $kb KiB #$rep: $got of $((kb * 1024)) bytes arrived"
					[ "$got" -eq $((kb * 1024)) ] ||
						note "run on a terminal: $got of $((kb * 1024)) bytes arrived (flood $kb KiB, pass $rep)"
				done
			done
			kill $hogs >/dev/null 2>&1 || true
			hogs=""

			# enter: a command in a running instance.  The instance is the probe
			# itself, held open on a fifo nobody writes to; the probe entered is a
			# second copy, so what it reports is what an entered command holds.
			mkfifo "$tmp/hold"
			exec 7<> "$tmp/hold"
			"$bin" run --command=ttyprobe org.test.Echo < "$tmp/hold" \
				> "$tmp/inst.out" 2>&1 &
			holder=$!
			iid=""
			n=0
			while [ -z "$iid" ] && [ "$n" -lt 150 ]; do
				iid=$(NO_COLOR=1 "$bin" ps 2>/dev/null |
					awk '/org\.test\.Echo/ {print $1; exit}')
				[ -n "$iid" ] || {
					sleep 0.2
					n=$((n + 1))
				}
			done
			if [ -z "$iid" ]; then
				note "enter: the instance to enter never showed in ps"
			else
				# No terminal: stdio goes over as it is.
				printf 'hello\n' | "$bin" enter "$iid" ttyprobe \
					> "$tmp/enter.out" 2>&1 && rc=0 || rc=$?
				[ "$rc" -eq 0 ] || note "enter (no terminal) exited $rc, not 0"
				grep -q 'tty=0 .*got=hello' "$tmp/enter.out" ||
					note "enter (no terminal): the command did not get its input: $(tr -d '\r' < "$tmp/enter.out" | tail -n 3)"
				printf 'five\n' | "$bin" enter "$iid" ttyprobe \
					> "$tmp/enter.out" 2>&1 && rc=0 || rc=$?
				[ "$rc" -eq 5 ] ||
					note "enter (no terminal): exit status 5 came back as $rc"
				# On a terminal: the command has one of the caller's size, is
				# not holding the caller's own, and sees a resize.
				NO_COLOR=1 TERM=xterm python3 "$ROOT/tests/ptyrun.py" \
					--rows 31 --cols 97 --wait PROBE-WAITING --send 'hello\n' \
					--timeout 90 "$tmp/ent.out" "$bin" enter "$iid" ttyprobe \
					&& rc=0 || rc=$?
				[ "$rc" -eq 0 ] || note "enter on a terminal exited $rc, not 0 (124 is a hang)"
				grep -q 'tty=1 tty0=1 tty1=1 tty2=1 rows=31 cols=97' "$tmp/ent.out" ||
					note "enter on a terminal: the command did not get a terminal of the caller's size"
				grep -q 'got=hello' "$tmp/ent.out" ||
					note "enter on a terminal: what the user typed did not reach the command"
				utty=$(cat "$tmp/ent.out.tty" 2>/dev/null)
				unit=${utty##*/}
				rdev=$(sed -n 's/.*rdev=\([0-9a-f]*\).*/\1/p' "$tmp/ent.out" | head -n 1)
				[ -n "$rdev" ] || note "enter on a terminal: the command reported no device"
				if [ -n "$rdev" ] && [ "$((0x$rdev & 0xff))" -eq "$unit" ] 2>/dev/null; then
					note "enter on a terminal: the command holds the caller's own terminal ($utty)"
				fi
				NO_COLOR=1 TERM=xterm python3 "$ROOT/tests/ptyrun.py" \
					--rows 31 --cols 97 --resize 40,120 --wait PROBE-WAITING \
					--send 'resized\n' --timeout 90 "$tmp/ent.out" \
					"$bin" enter "$iid" ttyprobe && rc=0 || rc=$?
				[ "$rc" -eq 0 ] || note "enter on a terminal (resize) exited $rc, not 0"
				grep -q 'rows=31 cols=97 .* now=40x120 ' "$tmp/ent.out" ||
					note "enter on a terminal: a window resize did not reach the command"
				NO_COLOR=1 TERM=xterm python3 "$ROOT/tests/ptyrun.py" \
					--wait PROBE-WAITING --send 'five\n' --timeout 90 \
					"$tmp/ent.out" "$bin" enter "$iid" ttyprobe && rc=0 || rc=$?
				[ "$rc" -eq 5 ] ||
					note "enter on a terminal: exit status 5 came back as $rc"
				# A mixture, as for run: stdin and stderr a terminal,
				# stdout a file.
				: > "$tmp/ent.mix"
				OUT="$tmp/ent.mix" NO_COLOR=1 TERM=xterm \
				python3 "$ROOT/tests/ptyrun.py" --rows 31 --cols 97 \
					--wait PROBE-WAITING --send 'hello\n' --timeout 90 \
					"$tmp/ent.mix.pty" sh -c 'exec "$@" >"$OUT"' sh \
					"$bin" enter "$iid" ttyprobe report=2 \
					&& rc=0 || rc=$?
				[ "$rc" -eq 0 ] ||
					note "enter (stdin and stderr a terminal, stdout a file) exited $rc, not 0"
				grep -q 'tty0=1 tty1=0 tty2=1 .*got=hello' "$tmp/ent.mix.pty" ||
					note "enter (stdin and stderr a terminal, stdout a file): the command reported $(tr -d '\r' < "$tmp/ent.mix.pty" | tail -n 2 | tr '\n' ' ')"
				if [ -s "$tmp/ent.mix" ]; then
					note "enter (stdin and stderr a terminal, stdout a file) wrote $(wc -c < "$tmp/ent.mix") bytes to the file"
				fi

				# Ownership: enter and kill act for root's privileges on a jail, so
				# only for the user the jail was made for, or for root.  The kernel
				# keeps the owner with the jail (its meta parameter).
				me=$(id -u)
				jid=$(sed -n 's/^jid=//p' "${XDG_RUNTIME_DIR:-/var/run/user/$me}/.flatpak/$iid/info" 2>/dev/null | head -n 1)
				if [ -z "$jid" ]; then
					note "ownership: the held instance's jail id could not be read"
				else
					rec=$(jls -j "$jid" meta 2>/dev/null)
					[ "$rec" = "flatpkg-uid=$me" ] ||
						note "ownership: the jail records '$rec', not 'flatpkg-uid=$me'"
					printf 'hello\n' | "$bin" enter "$jid" ttyprobe > "$tmp/own.out" 2>&1 && rc=0 || rc=$?
					[ "$rc" -eq 0 ] ||
						note "ownership: the owner could not enter its own jail by jail id ($rc)"
					# The jail now says it is someone else's, so neither enter
					# nor kill may act, by jail id or by instance id.
					mdo jail -m jid="$jid" meta=flatpkg-uid=1234 > /dev/null 2>&1 ||
						note "ownership: could not change the jail's recorded owner"
					for how in "$jid" "$iid"; do
						printf 'hello\n' | "$bin" enter "$how" ttyprobe > "$tmp/own.out" 2>&1 && rc=0 || rc=$?
						[ "$rc" -ne 0 ] ||
							note "ownership: enter $how worked on a jail recorded as another user's"
						grep -q 'belongs to another user' "$tmp/own.out" ||
							note "ownership: enter $how gave no refusal: $(tr '\n' ' ' < "$tmp/own.out" | cut -c1-160)"
						"$bin" kill "$how" > "$tmp/own.out" 2>&1 && rc=0 || rc=$?
						[ "$rc" -ne 0 ] ||
							note "ownership: kill $how worked on a jail recorded as another user's"
						grep -q 'belongs to another user' "$tmp/own.out" ||
							note "ownership: kill $how gave no refusal: $(tr '\n' ' ' < "$tmp/own.out" | cut -c1-160)"
					done
					jls -j "$jid" jid > /dev/null 2>&1 ||
						note "ownership: a refused kill removed the jail"
					# No recorded owner: it cannot be shown to be the caller's.
					mdo jail -m jid="$jid" meta= > /dev/null 2>&1 ||
						note "ownership: could not clear the jail's recorded owner"
					printf 'hello\n' | "$bin" enter "$jid" ttyprobe > "$tmp/own.out" 2>&1 && rc=0 || rc=$?
					[ "$rc" -ne 0 ] ||
						note "ownership: enter worked on a jail with no recorded owner"
					grep -q 'no recorded owner' "$tmp/own.out" ||
						note "ownership: no-owner refusal missing: $(tr '\n' ' ' < "$tmp/own.out" | cut -c1-160)"
					# Root may act on any of them.
					printf 'hello\n' | mdo "$bin" enter "$jid" ttyprobe > "$tmp/own.out" 2>&1 && rc=0 || rc=$?
					[ "$rc" -eq 0 ] ||
						note "ownership: root could not enter a jail with no recorded owner ($rc)"
					# Put it back, so the owner can act again.
					mdo jail -m jid="$jid" meta="flatpkg-uid=$me" > /dev/null 2>&1 ||
						note "ownership: could not restore the jail's recorded owner"
					printf 'hello\n' | "$bin" enter "$iid" ttyprobe > "$tmp/own.out" 2>&1 && rc=0 || rc=$?
					[ "$rc" -eq 0 ] ||
						note "ownership: the owner could not enter after the record was restored ($rc)"
				fi
			fi
			# An id that is no instance is refused, with or without a terminal.
			"$bin" enter 999999 ttyprobe > "$tmp/enter.out" 2>&1 && rc=0 || rc=$?
			[ "$rc" -ne 0 ] || note "enter 999999 (no terminal) succeeded"
			NO_COLOR=1 TERM=xterm python3 "$ROOT/tests/ptyrun.py" --timeout 60 \
				"$tmp/ent.out" "$bin" enter 999999 ttyprobe && rc=0 || rc=$?
			[ "$rc" -ne 0 ] && [ "$rc" -ne 124 ] ||
				note "enter 999999 on a terminal exited $rc (0 is success, 124 a hang)"
			grep -q 'not a flatpkg instance' "$tmp/ent.out" ||
				note "enter 999999 on a terminal: no refusal was reported"
			if [ -n "$iid" ]; then
				"$bin" kill "$iid" > /dev/null 2>&1 || true
			fi
			exec 7>&-
			wait "$holder" 2> /dev/null || true
		else
			echo "NOTE: zig could not build the terminal probe; run on a terminal was not exercised"
		fi
	else
		echo "NOTE: no zig or python3; run on a terminal was not exercised"
	fi
else
	echo "NOTE: no /compat/linux (or running as root); run was not exercised"
fi

# 5. the same worker keeps answering: a second connection works too.
run kill another-missing-instance
[ "$rc" -eq 1 ] || note "a second request exited $rc, not 1"

if [ "$fail" -ne 0 ]; then
	echo "      daemon log:"
	sed 's/^/        /' "$tmp/daemon.log" | tail -n 15
	exit 1
fi
echo "PASS: typed requests reach a real daemon; its log lines, exit status"
echo "      and refusals come back to the client"
