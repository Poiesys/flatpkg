#!/bin/sh
# ci-libostree.sh - build the overlay libostree port where there is no ports
# tree, which is the FreeBSD CI VM.  The port is the authority: the version,
# tarball and hash come from ports/sysutils/libostree, and its patches are
# applied as they are.  CONFIGURE_ARGS is the one copy; keep it in step with
# ports/sysutils/libostree/Makefile.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORT="$ROOT/ports/sysutils/libostree"
PREFIX=${PREFIX:-/usr/local}
SUDO=${SUDO-sudo}
MAKE=${MAKE:-gmake}

version=$(sed -n 's/^DISTVERSION=[[:space:]]*//p' "$PORT/Makefile")
site=$(sed -n 's/^MASTER_SITES=[[:space:]]*//p' "$PORT/Makefile")
tarball=$(awk '/^SHA256 \(/ { sub(/^SHA256 \(/, ""); sub(/\).*/, ""); print; exit }' \
	"$PORT/distinfo")
want=$(awk '/^SHA256 \(/ { print $4; exit }' "$PORT/distinfo")

if [ -z "$version" ] || [ -z "$site" ] || [ -z "$tarball" ] || [ -z "$want" ]; then
	echo "ci-libostree: cannot read the port's version or distinfo" >&2
	exit 1
fi

url=$(printf '%s%s' "$site" "$tarball" | sed "s/\${DISTVERSION}/$version/")
tmp=$(mktemp -d /tmp/libostree-ci.XXXXXX)
trap 'rm -rf "$tmp"' EXIT

fetch -q -o "$tmp/$tarball" "$url"
got=$(sha256 -q "$tmp/$tarball")
if [ "$got" != "$want" ]; then
	echo "ci-libostree: $tarball is $got, distinfo says $want" >&2
	exit 1
fi

tar -xf "$tmp/$tarball" -C "$tmp"
wrk="$tmp/libostree-$version"

# A patch changes m4 input, so the generated autotools files are stale; the
# port regenerates them too (USES=autoreconf).
for p in "$PORT"/files/patch-*; do
	patch -d "$wrk" -p0 -s -i "$p"
done
(cd "$wrk" && autoreconf -fi)

# --prefix is the ports framework's; the rest is the port's CONFIGURE_ARGS.
# BASH_COMPLETIONSDIR would otherwise come from the host's bash-completion
# package and leave the prefix.  Introspection is off because the image's
# g-ir-scanner drives distutils, which wants gcc, and nothing here needs the
# typelib.
(cd "$wrk" && \
	CPPFLAGS="-I$PORT/files ${CPPFLAGS:-}" \
	BASH_COMPLETIONSDIR="$PREFIX/share/bash-completion/completions" \
	./configure \
	--prefix="$PREFIX" --localstatedir=/var \
	--with-soup3 \
	--without-avahi \
	--without-libmount \
	--without-selinux \
	--disable-gtk-doc \
	--disable-man \
	--disable-man-html \
	--disable-introspection \
	--disable-rofiles-fuse \
	--disable-otmpfile \
	--disable-dependency-tracking)

jobs=$(sysctl -n hw.ncpu 2>/dev/null || echo 2)
(cd "$wrk" && "$MAKE" -j"$jobs")
$SUDO "$MAKE" -C "$wrk" install
case "$PREFIX" in
/usr|/usr/local) $SUDO /sbin/ldconfig -m "$PREFIX/lib" ;;
esac

echo "ci-libostree: installed libostree $version into $PREFIX"
