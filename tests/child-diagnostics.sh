#!/bin/sh
# child-diagnostics.sh - no buffered log call may sit in front of an _exit()
# within three lines: _exit() skips stdio flushing and fpkglog.h exposes no
# flush, so such a message is never seen and the child exits with a bare status.
set -e

ROOT="$(dirname "$0")/../src"
fail=0

echo "child diagnostics: a buffered log in front of _exit() is lost"
echo

# A log call and its _exit() are at most three lines apart (the call may be
# wrapped over two).  Keyed on FNR, which resets per file, not on NR.
awk '
	FNR == 1 { delete src; delete ring }
	/[^_]fpkg_(errorf|warnf|infof|okf)\(/ { ring[FNR % 4] = FNR }
	/_exit\(/ {
		for (i = FNR - 3; i < FNR; i++) {
			if (i in src && ring[i % 4] == i) {
				printf "FAIL %s:%d logs then _exit()s at line %d\n",
				    FILENAME, ring[i % 4], FNR
				bad++
			}
		}
	}
	{ src[FNR] = 1 }
	END { exit (bad ? 1 : 0) }
' "$ROOT"/*.c "$ROOT"/*/*.c || fail=1

if [ "$fail" -eq 0 ]; then
	echo "ok   no buffered log is lost to an _exit()"
fi

echo
if [ "$fail" -eq 0 ]; then
	echo "PASS: every child diagnostic survives the _exit()"
	exit 0
fi
echo "FAILED: a diagnostic would be lost - use dprintf(2, ...)"
exit 1
