#!/bin/sh
# Assemble man/flatpkg.8 from its template and the option tables in cli.c.
# Usage: gen-man.sh BUILD/flatpkg man/flatpkg.8.in > BUILD/flatpkg.8
set -e

bin=$1
tmpl=$2

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"$bin" complete man override > "$tmp/override"
"$bin" complete man run > "$tmp/run"
"$bin" complete man global > "$tmp/global"

awk -v ov="$tmp/override" -v run="$tmp/run" -v gl="$tmp/global" '
function emit(f,   l) {
	while ((getline l < f) > 0) print l
	close(f)
}
{
	if ($0 == "@@OVERRIDE_OPTIONS@@") emit(ov)
	else if ($0 == "@@RUN_OPTIONS@@") emit(run)
	else if ($0 == "@@GLOBAL_OPTIONS@@") emit(gl)
	else print
}' "$tmpl"
