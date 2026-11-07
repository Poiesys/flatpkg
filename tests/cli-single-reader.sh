#!/bin/sh
# cli-single-reader.sh - cli.c is the only reader of the environment and the
# only comparer of a command-line token.  For the environment: only cli.o may
# name getenv/getopt/environ, and every other non-shim object rule must carry
# $(NOENV); src/daemon/ops/evui.c is the one named exception, named so a
# second unpoisoned rule cannot hide.  For argv: no other file may compare a
# token, because that is how a command word grew a second parser.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
fail=0

poison_for() {
    awk -v obj="$1" '
        $0 ~ "^\\$\\(OBJDIR\\)/" obj "\\.o:" { inrule=1; next }
        inrule && /^\$\(OBJDIR\)\// { inrule=0 }
        inrule { print }
    ' "$ROOT/Makefile"
}

if poison_for cli/cli | grep -q 'NOENV'; then
    echo "FAIL cli/cli.o carries NOENV.h"
    fail=1
fi

# Every object rule, nested paths included.  The first version matched only
# one path component, so every object under cli/, daemon/ and shared/ was
# invisible and the guard passed on a tree with no rule checked at all.
objs=$(grep -oE '\$\(OBJDIR\)/[a-z_/]+\.o' "$ROOT/Makefile" |
    sed 's#^\$(OBJDIR)/##;s#\.o$##' | sort -u)
n=0
for obj in $objs; do
    n=$((n + 1))
    case "$obj" in
    cli/cli | daemon/ops/evui) continue ;;
    esac
    if ! poison_for "$obj" | grep -q 'NOENV'; then
        echo "FAIL $obj.o missing NOENV.h"
        fail=1
    fi
done
[ "$n" -ge 20 ] || {
    echo "FAIL the rule scan found only $n objects; the Makefile shape moved"
    fail=1
}

# The argv half.  dispatch.c used to match the service words with
# strcmp(argv[1], ...), which is exactly the second parser this file exists
# to prevent; cli_service_entry() moved it into cli.c.  The daemon's own
# entry point is the one exemption: flatpkgd links no CLI, and its single
# word is its contract.
bad=$(grep -rnE 'strn?cmp\(argv\[' "$ROOT/src" --include='*.c' |
    grep -v 'src/cli/cli.c' | grep -v 'src/daemon/main.c' || true)
if [ -n "$bad" ]; then
    echo "FAIL a command-line token is compared outside cli.c:"
    printf '%s\n' "$bad" | sed 's/^/       /'
    fail=1
fi
# cli.c must still do the matching, or the exemption above covers an empty
# tree and the check cannot fail.
own=$(grep -cE 'strn?cmp\(argv\[' "$ROOT/src/cli/cli.c" || true)
[ "$own" -ge 1 ] || {
    echo "FAIL cli.c compares no command-line token at all; the carve moved"
    fail=1
}
# And the matcher itself can see the shape it was written for.
printf '%s\n' 'src/cli/dispatch.c: strcmp(argv[1], "portal")' |
    grep -qE 'strn?cmp\(argv\[' ||
    { echo "FAIL the matcher does not recognise an argv comparison"; fail=1; }

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "PASS: cli is the only reader of the environment, evui excepted, and"
echo "      the only comparer of a command-line token"
exit 0
