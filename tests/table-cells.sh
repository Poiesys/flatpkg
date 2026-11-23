#!/bin/sh
# table-cells.sh - printed text is data, and data cannot carry an escape.
#
# Cell and log text comes from app metadata, appstream and remote files.  The
# renderer writes escape sequences on purpose for its own colours, so a control
# byte in the text used to reach the terminal as one: OSC 0 renames the window,
# OSC 52 writes the clipboard.  One rule (fpkg_text_clean) cleans the text, and
# this drives the two paths that print it: a table and a log line.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)

[ -x "$ROOT/build/flatpkg" ] ||
	{ echo "SKIP: build/flatpkg not built"; exit 0; }
command -v cc >/dev/null 2>&1 ||
	{ echo "SKIP: no cc in PATH"; exit 0; }
for o in tb_tui tb_core tb_progress translations; do
	[ -f "$ROOT/build/obj/cli/$o.o" ] ||
		{ echo "SKIP: $o.o is not built"; exit 0; }
done

T=$(mktemp -d /tmp/.fpkg-cells.XXXXXX)
trap 'rm -rf "$T"' EXIT

OBJS="$ROOT/build/obj/cli/tb_tui.o $ROOT/build/obj/cli/tb_core.o"
OBJS="$OBJS $ROOT/build/obj/cli/tb_progress.o"
OBJS="$OBJS $ROOT/build/obj/cli/translations.o"
LIBS="-L$ROOT/build -lflatpkg_shared -Wl,-rpath,$ROOT/build"
LIBS="$LIBS -L/usr/local/lib -ljail -ldbus-1 -lutil -lz"
LIBS="$LIBS -lgio-2.0 -lgobject-2.0 -lglib-2.0 -lintl"

cat > "$T/table.c" <<'PROBE'
#include <stdio.h>

#include "fplog.h"

int main(void) {
	const char* headers[] = {"Name", "Version"};
	// An OSC title set, the clipboard write, and a colour nobody asked for.
	const char* cells[] = {"Warp",
			       "\033]0;pwned\007\033]52;c;cHduZWQ=\007\033[31m1.0"};
	const char* const* rows[] = {cells};

	fp_print_table(headers, 2, rows, 1, FP_TABLE_OUTPUT);
	return 0;
}
PROBE

cat > "$T/log.c" <<'PROBE'
#include "flatpkg.h"
#include "fpkglog.h"

int main(void) {
	// A version out of app metadata on its way to the terminal.
	fpkg_errorf("version=%s", "\033]0;pwned\007\033[31m1.0");
	fp_outl("%s", "\033]0;pwned\007");
	return 0;
}
PROBE

build() {
	src=$1
	out=$2
	extra=$3
	# shellcheck disable=SC2086
	cc -O1 -I"$ROOT/include" -include sys/param.h -o "$out" "$src" \
		$OBJS $extra $LIBS >"$T/cc.log" 2>&1
}

# A control byte that reached the terminal is the finding; one that was
# replaced is not.
clean_check() {
	out=$1
	what=$2
	if grep -qF "$(printf '\033]')" "$out"; then
		echo "FAIL: a $what carried an OSC escape to the terminal"
		return 1
	fi
	if grep -qF "$(printf '\007')" "$out"; then
		echo "FAIL: a $what carried an unterminated OSC to the terminal"
		return 1
	fi
	if grep -qF "$(printf '\033[31m')" "$out"; then
		echo "FAIL: a $what carried an injected colour to the terminal"
		return 1
	fi
	return 0
}

if ! build "$T/table.c" "$T/table" ""; then
	echo "FAIL: the table probe does not build:"
	head -6 "$T/cc.log" | sed 's/^/       /'
	exit 1
fi
if ! build "$T/log.c" "$T/log" ""; then
	echo "FAIL: the log probe does not build:"
	head -6 "$T/cc.log" | sed 's/^/       /'
	exit 1
fi

rc=0
"$T/table" >"$T/table.out" 2>/dev/null
clean_check "$T/table.out" "table cell" || rc=1
grep -q 'pwned' "$T/table.out" ||
	{ echo "FAIL: the cell text was dropped, not cleaned"; rc=1; }

"$T/log" >"$T/log.out" 2>&1
clean_check "$T/log.out" "log line" || rc=1
grep -q 'version=' "$T/log.out" ||
	{ echo "FAIL: the log line lost its text"; rc=1; }

# The probes must be able to fail: with the cleaner compiled out, the OSC comes
# back.  The cleaner is compiled here from the source the binary is built from.
sed 's/\*p = .?.;/;/' "$ROOT/src/shared/util.c" >"$T/util_plant.c"
cmp -s "$ROOT/src/shared/util.c" "$T/util_plant.c" &&
	{ echo "FAIL: the text-cleaner plant was not applied"; exit 1; }
if cc -O1 -w -I"$ROOT/include" -include sys/param.h -c \
	-o "$T/util_plant.o" "$T/util_plant.c" >"$T/plant.log" 2>&1; then
	build "$T/table.c" "$T/table-plant" "$T/util_plant.o" ||
		{ echo "FAIL: the planted table probe does not build"; exit 1; }
	build "$T/log.c" "$T/log-plant" "$T/util_plant.o" ||
		{ echo "FAIL: the planted log probe does not build"; exit 1; }
	"$T/table-plant" >"$T/table-plant.out" 2>/dev/null
	grep -qF "$(printf '\033]')" "$T/table-plant.out" ||
		{ echo "FAIL: the table probe passes without the cleaner"; exit 1; }
	"$T/log-plant" >"$T/log-plant.out" 2>&1
	grep -qF "$(printf '\033]')" "$T/log-plant.out" ||
		{ echo "FAIL: the log probe passes without the cleaner"; exit 1; }
else
	echo "FAIL: the planted cleaner does not build:"
	head -4 "$T/plant.log" | sed 's/^/       /'
	exit 1
fi

[ "$rc" -eq 0 ] &&
	echo "PASS: a table cell or a log line carrying an OSC or a colour is" \
	     "cleaned before it reaches the terminal, and fplog's own colour" \
	     "still does."
exit "$rc"
