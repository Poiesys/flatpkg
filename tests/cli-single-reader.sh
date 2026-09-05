#!/bin/sh
# cli-single-reader.sh - only cli.o may name getenv/getopt/environ; every
# other non-shim object rule must carry $(NOENV).  src/daemon/ops/evui.c is
# the one named exception, named so a second unpoisoned rule cannot hide.
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

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "PASS: cli is the only reader of the environment, evui excepted"
exit 0
