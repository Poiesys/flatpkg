#!/bin/sh
# daemon-link.sh - the root binary does not link the CLI: no terminal
# renderer, argv parser, client or translations in a root process.  The link
# is checked as a list, not symbols: evui.c defines the same names as data.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
fail=0
note() {
	echo "FAIL: $*"
	fail=1
}

# bmake expands a variable without running anything.  This project is
# FreeBSD-only, but say so rather than reading an empty list as a pass.
link=$(make -s -C "$ROOT" -V DAEMON_LINK 2>/dev/null || true)
[ -n "$link" ] || {
	echo "FAIL: make -V DAEMON_LINK printed nothing; the Makefile shape moved"
	exit 1
}

n=$(printf '%s\n' $link | wc -l | tr -d ' ')
[ "$n" -ge 10 ] || note "the daemon link has only $n objects; the variable moved"

# The forbidden set: the CLI parser and help, the terminal library, the
# translations, the client and the shared dispatcher.
bad=$(printf '%s\n' $link |
	grep -E '/(cli/cli|cli/client|cli/client_typed|cli/tb_[a-z_]+|cli/translations|cli/upgrade|main)\.o$' ||
	true)
[ -z "$bad" ] || note "the daemon links CLI objects: $(printf '%s' "$bad" | tr '\n' ' ')"

# And the replacement is there, with the two entry points that make the
# binary a daemon rather than a client.
for want in build/obj/daemon/ops/evui.o build/obj/daemon/daemon.o \
    build/obj/daemon/ops/typed_worker.o; do
	printf '%s\n' $link | grep -qx "$want" ||
		note "the daemon does not link $want"
done

# The matcher itself: a planted CLI object must be recognised, or the check
# above passes because the pattern matches nothing.
plant="build/obj/cli/cli.o"
printf '%s\n' "$plant" |
	grep -qE '/(cli/cli|cli/client|cli/client_typed|cli/tb_[a-z_]+|cli/translations|cli/upgrade|main)\.o$' ||
	note "the forbidden-object pattern does not recognise $plant"

# The solver is policy and must not be in the root binary either.  It is
# linked as an object (the world/txn primitives live beside it) but section
# GC drops the policy entry points when nothing in the daemon calls them.
for sym in solver_solve solver_commit cmd_install_many cmd_upgrade \
    ostree_plan_install ostree_plan_remove ostree_plan_upgrade; do
	if nm -g "$ROOT/build/flatpkgd" 2>/dev/null |
	    grep -q " T $sym$"; then
		note "the daemon carries solver policy: $sym"
	fi
done

# The CLI must still have the dispatcher and the terminal library, or the
# split was made by deleting both sides rather than separating them.
cli_link=$(make -s -C "$ROOT" -V CLI_OBJS 2>/dev/null || true)
for want in build/obj/cli/cli.o build/obj/cli/tb_core.o; do
	printf '%s\n' $cli_link | grep -qx "$want" ||
		note "the CLI no longer links $want"
done

# The mirror, and the reason `flatpkg daemon` is gone: the CLI carries no
# daemon object, so it cannot become the daemon even by accident.
cli_all=$(make -s -C "$ROOT" -V CLI_LINK 2>/dev/null || true)
[ -n "$cli_all" ] || {
	echo "FAIL: make -V CLI_LINK printed nothing; the Makefile shape moved"
	exit 1
}
n_cli=$(printf '%s\n' $cli_all | wc -l | tr -d ' ')
[ "$n_cli" -ge 10 ] ||
	note "the CLI link has only $n_cli objects; the variable moved"
cli_bad=$(printf '%s\n' $cli_all |
	grep -E '/daemon/(daemon|bus|extensions|sandbox|sandbox_root|sandbox_exec|systemhelper)\.o$' ||
	true)
[ -z "$cli_bad" ] ||
	note "the CLI links daemon objects: $(printf '%s' "$cli_bad" | tr '\n' ' ')"
printf '%s\n' "build/obj/daemon/sandbox_root.o" |
	grep -qE '/daemon/(daemon|bus|extensions|sandbox|sandbox_root|sandbox_exec|systemhelper)\.o$' ||
	note "the daemon-object pattern does not recognise sandbox_root.o"

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "PASS: flatpkgd links the daemon, not the CLI: no cli.o, tb_*.o,"
echo "      translations.o, client*.o, upgrade.o or main.o; evui.o is its"
echo "      replacement.  The CLI links no daemon object, so it has no way"
echo "      to become the daemon."
exit 0
