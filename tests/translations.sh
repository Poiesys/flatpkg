#!/bin/sh
# translations.sh - the localized string layer is present and wired in.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
fail=0

for f in include/translations.h include/translations/en.h src/cli/translations.c; do
    [ -f "$ROOT/$f" ] || { echo "FAIL missing $f"; fail=1; }
done

grep -q 'const char\* get_string' "$ROOT/include/translations.h" || { echo "FAIL get_string missing"; fail=1; }
grep -q 'translations.o' "$ROOT/Makefile" || { echo "FAIL translations.o not built"; fail=1; }

if [ "$fail" -ne 0 ]; then exit 1; fi
echo "PASS: translation layer present and wired"
exit 0