PORTNAME=	libostree
DISTVERSION=	2026.4
CATEGORIES=	sysutils
MASTER_SITES=	https://github.com/ostreedev/ostree/releases/download/v${DISTVERSION}/
DISTNAME=	libostree-${DISTVERSION}

MAINTAINER=	demperor@local
COMMENT=	Operating system and container image deployment and management
WWW=		https://github.com/ostreedev/ostree

LICENSE=	LGPL20+
LICENSE_FILE=	${WRKSRC}/COPYING

# ostree is Linux-centric, but its repository core (init/commit/pull/checkout/
# fsck/show/log/static-delta/GPG) is portable.  This port builds libostree-1
# and the `ostree` CLI on FreeBSD by:
#   * providing a <sys/xattr.h> shim over extattr(2) (files/sys/xattr.h);
#   * patching the bundled libglnx Linux-syscall shims;
#   * reimplementing the ext2 immutable-flag ioctls with fchflags(2);
#   * guarding the genuinely Linux-only subsystems (mount namespaces,
#     pivot_root, overlayfs, kexec, fs-verity, O_TMPFILE, rofiles-fuse,
#     systemd, SELinux) so they compile and report "not supported" at runtime.
#
# Sysroot/deployment/bootloader management stays Linux-only; the repository
# commands used by flatpkg (init, commit, pull, checkout, fsck, ...) work.
#
# The feature set matches the known-good build on FreeBSD: libsoup3 for HTTP,
# GPGME for signatures, libarchive for tarballs and GLib for checksums.
# Avahi, libcurl, OpenSSL, SELinux, libmount, rofiles-fuse, composefs and
# O_TMPFILE are Linux-only or unnecessary and are disabled.

BUILD_DEPENDS=	${LOCALBASE}/libdata/pkgconfig/e2p.pc:filesystems/e2fsprogs-core
LIB_DEPENDS=	libarchive.so:archivers/libarchive \
		libgpg-error.so:security/libgpg-error \
		libgpgme.so:security/gpgme \
		libsoup-3.0.so:devel/libsoup3

USES=		autoreconf cpe gettext-runtime gmake gnome libtool pathfix pkgconfig tar:xz
USE_GNOME=	glib20
CPE_VENDOR=	ostree_project
GNU_CONFIGURE=	yes
USE_LDCONFIG=	yes

CONFIGURE_ARGS=	--with-soup3 \
		--without-avahi \
		--without-libmount \
		--without-selinux \
		--disable-gtk-doc \
		--disable-man \
		--disable-man-html \
		--disable-rofiles-fuse \
		--disable-otmpfile \
		--disable-dependency-tracking

# Provide the FreeBSD xattr compatibility shim on the system include path so
# that the Linux <sys/xattr.h> API is resolved from files/sys/xattr.h.
CPPFLAGS+=	-I${PATCHDIR}

OPTIONS_DEFINE=	DOCS
OPTIONS_SUB=	yes

post-install-DOCS-on:
	${MKDIR} ${STAGEDIR}${DOCSDIR}
	${INSTALL_DATA} ${WRKSRC}/README.md ${STAGEDIR}${DOCSDIR}/

.include <bsd.port.mk>
