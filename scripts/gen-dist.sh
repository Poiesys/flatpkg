#!/bin/sh
# Package this checkout for the port and keep the port's distinfo in step.
# Usage: gen-dist.sh VERSION DISTDIR
#
# A staged copy fixes ownership and every mtime before tar (DragonFlyBSD's
# bsdtar has no --mtime) and the entries are fed sorted (it has no --sort);
# gzip -n keeps the tarball reproducible, so distinfo only changes when a
# tracked file does.
set -e

version=$1
distdir=$2
dname="flatpkg-$version"
# sort's collation and touch's zone decide the archive, so pin both.
LC_ALL=C
TZ=UTC0
export LC_ALL TZ
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

tmp=$(mktemp -d "${TMPDIR:-/tmp}/flatpkg-dist.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

git ls-files | sort -u | grep -v '^ports/' > "$tmp/files" || true
: > "$tmp/present"
while IFS= read -r f; do
	[ -f "$f" ] && printf '%s\n' "$f" >> "$tmp/present"
done < "$tmp/files"

stage="$tmp/stage/$dname"
mkdir -p "$stage"
tar -cf - -T "$tmp/present" | tar -xf - -C "$stage"

find "$stage" -exec touch -h -t 202001010000.00 {} +
(cd "$tmp/stage" && find "$dname" -print | sort) > "$tmp/entries"

mkdir -p "$distdir"
tarball="$distdir/$dname.tar.gz"
tar --format=ustar --uid 0 --gid 0 --uname root --gname wheel \
	--no-recursion -cf - -C "$tmp/stage" -T "$tmp/entries" | \
	gzip -9n > "$tarball"

if command -v sha256 >/dev/null 2>&1; then
	sha=$(sha256 -q "$tarball")
elif command -v sha256sum >/dev/null 2>&1; then
	sha=$(sha256sum "$tarball" | awk '{ print $1 }')
else
	echo "gen-dist: no sha256 tool" >&2
	exit 1
fi
size=$(wc -c < "$tarball" | tr -d ' ')

echo "wrote $tarball"
echo "$sha"

# Only the default location is the one the port fetches and distinfo describes;
# a tarball written somewhere else must not rewrite the checked-in file.
case "$distdir" in
ports/dist|./ports/dist) ;;
*) exit 0 ;;
esac

info=ports/sysutils/flatpkg/distinfo
old_sha=$(awk '$1 == "SHA256" { print $4 }' "$info" 2>/dev/null || true)
old_size=$(awk '$1 == "SIZE" { print $4 }' "$info" 2>/dev/null || true)
if [ "$sha" = "$old_sha" ] && [ "$size" = "$old_size" ]; then
	exit 0
fi

tmpinfo=$(mktemp "$info.XXXXXX")
{
	printf 'TIMESTAMP = %s\n' "$(date +%s)"
	printf 'SHA256 (%s) = %s\n' "$dname.tar.gz" "$sha"
	printf 'SIZE (%s) = %s\n' "$dname.tar.gz" "$size"
} > "$tmpinfo"
chmod 644 "$tmpinfo"
mv "$tmpinfo" "$info"
echo "updated $info"
