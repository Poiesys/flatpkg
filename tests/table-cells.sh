#!/bin/sh
# table-cells.sh - a table cell is data, and data cannot carry an escape.
#
# Cell text comes from app metadata, appstream and remote files.  The renderer
# passes escapes through on purpose (fplog's own colours are escapes), so a
# control byte in a cell used to reach the terminal as one: OSC 0 renames the
# window, OSC 52 writes the clipboard.  The cells are cleaned at the one place
# they are handed to the renderer, and this drives that renderer.
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

cat > "$T/probe.c" <<'PROBE'
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

if ! cc -O1 -I"$ROOT/include" -include sys/param.h -o "$T/probe" \
	"$T/probe.c" "$ROOT/build/obj/cli/tb_tui.o" \
	"$ROOT/build/obj/cli/tb_core.o" \
	"$ROOT/build/obj/cli/tb_progress.o" \
	"$ROOT/build/obj/cli/translations.o" -L"$ROOT/build" \
	-lflatpkg_shared -Wl,-rpath,"$ROOT/build" -L/usr/local/lib -ljail \
	-ldbus-1 -lutil -lz -lgio-2.0 -lgobject-2.0 -lglib-2.0 -lintl \
	>"$T/cc.log" 2>&1; then
	echo "FAIL: the table probe does not build:"
	head -6 "$T/cc.log" | sed 's/^/       /'
	exit 1
fi

"$T/probe" >"$T/out" 2>/dev/null
# The header line carries fplog's own colour, so an escape is not itself the
# finding: the OSC and the injected SGR are.  A BEL cannot appear at all.
if tr -d '\n' <"$T/out" | grep -qF "$(printf '\033]')"; then
	echo "FAIL: a table cell reached the terminal as an OSC escape"
	exit 1
fi
if tr -d '\n' <"$T/out" | grep -qF "$(printf '\007')"; then
	echo "FAIL: an unterminated OSC survived into the table"
	exit 1
fi
# The cell itself still says what it said, with the control bytes replaced.
grep -q 'pwned' "$T/out" ||
	{ echo "FAIL: the cell text was dropped instead of cleaned"; exit 1; }
grep -qF "$(printf '\033[31m')" "$T/out" &&
	{ echo "FAIL: an injected colour reached the terminal"; exit 1; }

# The probe must be able to fail: with the cell cleaner compiled out, the OSC
# comes back.  The renderer is compiled here rather than copied, so this is the
# same source the binary is built from.
sed 's/buf\[k++\] = (c < 0x20 || c == 0x7f) ? .?. : (char)c;/buf[k++] = (char)c;/' \
	"$ROOT/src/cli/tb_tui.c" >"$T/tb_tui_plant.c"
cmp -s "$ROOT/src/cli/tb_tui.c" "$T/tb_tui_plant.c" &&
	{ echo "FAIL: the cell-cleaner plant was not applied"; exit 1; }
if cc -O1 -w -I"$ROOT/include" -include sys/param.h -c \
	-o "$T/tb_tui_plant.o" "$T/tb_tui_plant.c" >"$T/plant.log" 2>&1; then
	cc -O1 -I"$ROOT/include" -include sys/param.h -o "$T/probe-plant" \
		"$T/probe.c" "$T/tb_tui_plant.o" \
		"$ROOT/build/obj/cli/tb_core.o" \
		"$ROOT/build/obj/cli/tb_progress.o" \
		"$ROOT/build/obj/cli/translations.o" -L"$ROOT/build" \
		-lflatpkg_shared -Wl,-rpath,"$ROOT/build" -L/usr/local/lib \
		-ljail -ldbus-1 -lutil -lz -lgio-2.0 -lgobject-2.0 -lglib-2.0 \
		-lintl >"$T/plant-cc.log" 2>&1 &&
		"$T/probe-plant" >"$T/out-plant" 2>/dev/null &&
		tr -d '\n' <"$T/out-plant" |
			grep -qF "$(printf '\033]')" ||
		{ echo "FAIL: the probe passes without the cell cleaner"; exit 1; }
else
	echo "FAIL: the planted renderer does not build:"
	head -4 "$T/plant.log" | sed 's/^/       /'
	exit 1
fi

echo "PASS: a table cell carrying an OSC or a colour is cleaned before it"
echo "      reaches the terminal, and fplog's own colour still does."
