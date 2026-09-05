PORTNAME=	flatpkg
DISTVERSION=	0.1.0
CATEGORIES=	sysutils
COMMENT=	Run unmodified Flatpak Linux applications in FreeBSD jails
WWW=		https://github.com/Poiesys/flatpkg

LICENSE=	BSD3CLAUSE

# libjail is the jail(8) API every launch goes through; it is part of the
# base system, so there is no port to depend on.  glib and dbus are what the
# portal and the desktop helpers link against.
LIB_DEPENDS=	libglib-2.0.so:devel/glib20 \
		libdbus-1.so:devel/dbus

# The ostree CLI drives the store, so it is a runtime tool.  The libostree
# port ships in this repository's ports/ overlay: a merged tree gets the
# dependency, a stock one is pointed at it by pkg-message.
RUN_DEPENDS=	${LOCALBASE}/bin/dbus-daemon:devel/dbus
.if exists(${PORTSDIR}/sysutils/libostree)
RUN_DEPENDS+=	${LOCALBASE}/bin/ostree:sysutils/libostree
.endif

# The seven LD_PRELOAD shims are Linux ELFs, so they must be cross-compiled.
# The port runs `make`, not `zig build`: `USES=zig` would take over do-build
# and do-install, so zig is a plain build dependency.
BUILD_DEPENDS=	zig:lang/zig
USES=		gmake
SHEBANG_FILES=	scripts/*.sh

# There is no upstream tag to fetch.  Run `make dist` at the repository
# root to package this checkout as ports/dist/flatpkg-0.1.0.tar.gz; the
# path is relative to this port, so an overlay ports tree works too.
MASTER_SITES=	file://${.CURDIR}/../../dist/
DISTFILES=	${PORTNAME}-${DISTVERSION}${EXTRACT_SUFX}

post-install:
	@${MKDIR} ${STAGEDIR}/var/db/flatpkg
# The ports framework owns configuration files: ship the helper map as a
# sample, and let pkg copy it into place.
	@${MV} ${STAGEDIR}${PREFIX}/etc/flatpkg/helper.conf \
		${STAGEDIR}${PREFIX}/etc/flatpkg/helper.conf.sample
# /var/lib is not package content; the plist creates these two compatibility
# symlinks at install time.
	@${RM} -f ${STAGEDIR}/var/lib/flatpkg ${STAGEDIR}/var/lib/flatpak
	@${RMDIR} ${STAGEDIR}/var/lib 2>/dev/null || ${TRUE}

.include <bsd.port.mk>
