PREFIX?=        /usr/local
# Where flatpkgd listens.  Compile-time only (see FPKG_SOCKET in flatpkg.h);
# tests build a scratch client pointed somewhere else.
SOCKET_PATH?=   /var/run/flatpkg/control.sock
# Where the store lives.  Compile-time only, like SOCKET_PATH.
FLATPKG_STORE_DIR?= /var/db/flatpkg
CC?=            cc
CFLAGS+=        -O2 -ffunction-sections -fdata-sections -Wall -Wextra -Wno-unused-parameter -Wformat-security -Werror -std=gnu11 -Iinclude
# Extra flags for a sanitizer or coverage build: make SANFLAGS="-fsanitize=..."
SANFLAGS?=
CFLAGS+=        $(SANFLAGS)
LDLIBS+=        $(SANFLAGS)
CFLAGS+=        -include sys/param.h
CFLAGS+=        -DFPKG_SOCKET=\"$(SOCKET_PATH)\"
CFLAGS+=        -DFLATPKG_STORE_DIR=\"$(FLATPKG_STORE_DIR)\"
NOENV=          -include include/fpkg_noenv.h
CFLAGS+=        -I/usr/local/include/dbus-1.0 -I/usr/local/lib/dbus-1.0/include
CFLAGS+=        -Ibuild
LDLIBS+=        -ljail
LDLIBS+=        -lnv
LDLIBS+=        -L/usr/local/lib -ldbus-1 -lutil -lz
GLIB_CFLAGS?=   -I/usr/local/include/glib-2.0 -I/usr/local/lib/glib-2.0/include
GLIB_LIBS?=     -L/usr/local/lib -lgio-2.0 -lgobject-2.0 -lglib-2.0 -lintl
CFLAGS+=        $(GLIB_CFLAGS)
LDLIBS+=        $(GLIB_LIBS)
LDLIBS+=        -pthread

BUILD=          build
OBJDIR=         $(BUILD)/obj

BIN_CLI=        $(BUILD)/flatpkg
MANPAGE=        $(BUILD)/flatpkg.8
BIN_DAEMON=     $(BUILD)/flatpkgd
SHLIB=          $(BUILD)/libflatpkg_shared.so

SHIM=           $(BUILD)/signalfd_shim.so
SHIM_SRC=       src/signalfd_shim.c
SECCOMP_SHIM=   $(BUILD)/seccomp_shim.so
SECCOMP_SRC=    src/seccomp_shim.c

DEVT_SHIM=      $(BUILD)/dmabuf_devt_shim.so
DEVT_SHIM_SRC=  src/dmabuf_devt_shim.c

MEMFD_SHIM=     $(BUILD)/memfd_shim.so
MEMFD_SHIM_SRC= src/memfd_shim.c

SYNC_SHIM=      $(BUILD)/syncobj_shim.so
SYNC_SHIM_SRC=  src/syncobj_shim.c

RLIMIT_SHIM=    $(BUILD)/rlimit_shim.so
RLIMIT_SHIM_SRC= src/rlimit_shim.c

MADVISE_SHIM=     $(BUILD)/madvise_shim.so
MADVISE_SHIM_SRC= src/madvise_shim.c

.PHONY: all
all: $(BIN_CLI) $(BIN_DAEMON) $(SHLIB) $(SHIM) $(SECCOMP_SHIM) $(DEVT_SHIM) \
    $(MEMFD_SHIM) $(SYNC_SHIM) $(RLIMIT_SHIM) $(MADVISE_SHIM) $(MANPAGE)


# Shared library objects
SHLIB_OBJS=     $(OBJDIR)/shared/metadata.o \
		$(OBJDIR)/shared/permissions.o \
		$(OBJDIR)/shared/overrides.o \
		$(OBJDIR)/shared/history.o \
		$(OBJDIR)/shared/profile.o \
		$(OBJDIR)/shared/dbus.o \
		$(OBJDIR)/shared/config.o \
		$(OBJDIR)/shared/store.o \
		$(OBJDIR)/shared/instance.o \
		$(OBJDIR)/shared/modules.o \
		$(OBJDIR)/shared/protocol.o \
		$(OBJDIR)/shared/util.o

# CLI binary objects
CLI_OBJS=       $(OBJDIR)/cli/dispatch.o \
		$(OBJDIR)/cli/cli.o \
		$(OBJDIR)/cli/client.o \
		$(OBJDIR)/cli/client_typed.o \
		$(OBJDIR)/cli/solver.o \
		$(OBJDIR)/cli/tb_core.o \
		$(OBJDIR)/cli/tb_progress.o \
		$(OBJDIR)/cli/tb_tui.o \
		$(OBJDIR)/cli/upgrade.o \
		$(OBJDIR)/cli/repair.o \
		$(OBJDIR)/cli/translations.o \
		$(OBJDIR)/cli/portal.o \
		$(OBJDIR)/cli/sessionhelper.o

# Daemon binary objects
DAEMON_OBJS=    $(OBJDIR)/daemon/daemon.o \
		$(OBJDIR)/daemon/bus.o \
		$(OBJDIR)/daemon/extensions.o \
		$(OBJDIR)/daemon/sandbox.o \
		$(OBJDIR)/daemon/sandbox_root.o \
		$(OBJDIR)/daemon/sandbox_exec.o \
		$(OBJDIR)/daemon/systemhelper.o \
		$(OBJDIR)/daemon/ops/typed_worker.o \
		$(OBJDIR)/daemon/ops/run.o

# Legacy objects still in src/ root (need to be moved).  These are the ones
# both binaries link: the OSTree backend and the store primitives.
LEGACY_OBJS=    $(OBJDIR)/ostree.o \
		$(OBJDIR)/ostree_plan.o \
		$(OBJDIR)/ostree_cmds.o \
		$(OBJDIR)/daemon/ops/store_write.o

# Daemon-only ops.  They carry CLI-shaped entry points (cmd_enter, cmd_kill,
# cmd_exports) that only the daemon's typed worker calls, so --gc-sections
# normally drops them from the CLI link.  Relying on that hid an undefined
# reference to the daemon whenever GC kept the sections, so they are listed
# here instead of in LEGACY_OBJS.
DAEMON_OPS_OBJS=$(OBJDIR)/daemon/ops/dbus_proxy.o \
		$(OBJDIR)/daemon/ops/instance_ctl.o

# Policy the daemon runs in process (the solver and upgrade planner).  It is
# the one CLI-side object set the daemon links, together with the logger and
# env accessor the primitives call.
POLICY_OBJS=    $(OBJDIR)/cli/solver.o

DAEMON_LINK=    $(DAEMON_OBJS) $(LEGACY_OBJS) $(DAEMON_OPS_OBJS) \
		$(POLICY_OBJS) $(OBJDIR)/daemon/ops/evui.o

# The CLI links no daemon object: `flatpkg daemon` is gone, and the system
# helper is flatpkgd's entry point.
CLI_LINK=       $(ENTRY_CLI) $(CLI_OBJS) $(LEGACY_OBJS)

# Shared library
$(SHLIB): $(SHLIB_OBJS) Makefile
	$(CC) -shared -o $@ $(SHLIB_OBJS) $(LDLIBS)

# The two binaries no longer share a dispatcher: the daemon's worker speaks
# only the framed protocol, and the CLI's argv path never runs privileged.
# $ORIGIN finds libflatpkg_shared.so beside the binary in a build tree,
# $(PREFIX)/lib once installed.
ENTRY_CLI=      $(OBJDIR)/cli/entry.o
ENTRY_DAEMON=   $(OBJDIR)/daemon/main.o
RPATH=          -Wl,-z,origin -Wl,-rpath,'$$ORIGIN:$(PREFIX)/lib' -Wl,--gc-sections

$(BIN_CLI): $(CLI_LINK) $(SHLIB) Makefile
	$(CC) $(CFLAGS) -o $@ $(CLI_LINK) \
	    -L$(BUILD) -lflatpkg_shared $(RPATH) $(LDLIBS)

$(BIN_DAEMON): $(ENTRY_DAEMON) $(DAEMON_LINK) $(SHLIB) Makefile
	$(CC) $(CFLAGS) -o $@ $(ENTRY_DAEMON) $(DAEMON_LINK) \
	    -L$(BUILD) -lflatpkg_shared $(RPATH) $(LDLIBS)

$(OBJDIR)/cli/entry.o: src/cli/entry.c
	@mkdir -p $(OBJDIR)/cli
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/cli/entry.c

$(OBJDIR)/daemon/main.o: src/daemon/main.c
	@mkdir -p $(OBJDIR)/daemon
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/daemon/main.c

sinclude $(OBJDIR)/cli/entry.d
sinclude $(OBJDIR)/daemon/main.d

# Explicit compile rules for shared library objects
$(OBJDIR)/shared/metadata.o: src/shared/metadata.c
	@mkdir -p $(OBJDIR)/shared
	$(CC) $(CFLAGS) $(NOENV) -fPIC -MMD -MP -c -o $@ src/shared/metadata.c

$(OBJDIR)/shared/permissions.o: src/shared/permissions.c
	@mkdir -p $(OBJDIR)/shared
	$(CC) $(CFLAGS) $(NOENV) -fPIC -MMD -MP -c -o $@ src/shared/permissions.c

$(OBJDIR)/shared/overrides.o: src/shared/overrides.c
	@mkdir -p $(OBJDIR)/shared
	$(CC) $(CFLAGS) $(NOENV) -fPIC -MMD -MP -c -o $@ src/shared/overrides.c

$(OBJDIR)/daemon/extensions.o: src/daemon/extensions.c
	@mkdir -p $(OBJDIR)/daemon
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/daemon/extensions.c

$(OBJDIR)/shared/history.o: src/shared/history.c
	@mkdir -p $(OBJDIR)/shared
	$(CC) $(CFLAGS) $(NOENV) -fPIC -MMD -MP -c -o $@ src/shared/history.c

$(OBJDIR)/shared/profile.o: src/shared/profile.c
	@mkdir -p $(OBJDIR)/shared
	$(CC) $(CFLAGS) $(NOENV) -fPIC -MMD -MP -c -o $@ src/shared/profile.c

$(OBJDIR)/shared/dbus.o: src/shared/dbus.c
	@mkdir -p $(OBJDIR)/shared
	$(CC) $(CFLAGS) $(NOENV) -fPIC -MMD -MP -c -o $@ src/shared/dbus.c

$(OBJDIR)/shared/protocol.o: src/shared/protocol.c include/protocol.h
	@mkdir -p $(OBJDIR)/shared
	$(CC) $(CFLAGS) $(NOENV) -fPIC -MMD -MP -c -o $@ src/shared/protocol.c

$(OBJDIR)/shared/store.o: src/shared/store.c
	@mkdir -p $(OBJDIR)/shared
	$(CC) $(CFLAGS) $(NOENV) -fPIC -MMD -MP -c -o $@ src/shared/store.c

$(OBJDIR)/shared/util.o: src/shared/util.c
	@mkdir -p $(OBJDIR)/shared
	$(CC) $(CFLAGS) $(NOENV) -fPIC -MMD -MP -c -o $@ src/shared/util.c

$(OBJDIR)/shared/config.o: src/shared/config.c
	@mkdir -p $(OBJDIR)/shared
	$(CC) $(CFLAGS) $(NOENV) -DSYSCONFDIR=\"$(PREFIX)/etc\" -fPIC -MMD -MP -c -o $@ src/shared/config.c

$(OBJDIR)/shared/instance.o: src/shared/instance.c
	@mkdir -p $(OBJDIR)/shared
	$(CC) $(CFLAGS) $(NOENV) -fPIC -MMD -MP -c -o $@ src/shared/instance.c

$(OBJDIR)/shared/modules.o: src/shared/modules.c
	@mkdir -p $(OBJDIR)/shared
	$(CC) $(CFLAGS) $(NOENV) -fPIC -MMD -MP -c -o $@ src/shared/modules.c

# CLI binary objects
$(OBJDIR)/cli/dispatch.o: src/cli/dispatch.c
	@mkdir -p $(OBJDIR)/cli
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/cli/dispatch.c

$(OBJDIR)/cli/cli.o: src/cli/cli.c include/cli.h include/fpkglog.h
	@mkdir -p $(OBJDIR)/cli
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ src/cli/cli.c

$(OBJDIR)/cli/client.o: src/cli/client.c
	@mkdir -p $(OBJDIR)/cli
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/cli/client.c

$(OBJDIR)/cli/client_typed.o: src/cli/client_typed.c
	@mkdir -p $(OBJDIR)/cli
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/cli/client_typed.c

$(OBJDIR)/cli/solver.o: src/cli/solver.c
	@mkdir -p $(OBJDIR)/cli
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/cli/solver.c

$(OBJDIR)/cli/tb_core.o: src/cli/tb_core.c
	@mkdir -p $(OBJDIR)/cli
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/cli/tb_core.c

$(OBJDIR)/cli/tb_progress.o: src/cli/tb_progress.c
	@mkdir -p $(OBJDIR)/cli
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/cli/tb_progress.c

$(OBJDIR)/cli/tb_tui.o: src/cli/tb_tui.c
	@mkdir -p $(OBJDIR)/cli
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/cli/tb_tui.c

$(OBJDIR)/cli/upgrade.o: src/cli/upgrade.c
	@mkdir -p $(OBJDIR)/cli
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/cli/upgrade.c

$(OBJDIR)/cli/translations.o: src/cli/translations.c include/translations.h
	@mkdir -p $(OBJDIR)/cli
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/cli/translations.c

$(OBJDIR)/ostree.o: src/ostree.c src/ostree_internal.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/ostree.c

$(OBJDIR)/ostree_plan.o: src/ostree_plan.c src/ostree_internal.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/ostree_plan.c

$(OBJDIR)/ostree_cmds.o: src/ostree_cmds.c src/ostree_internal.h
	@mkdir -p $(OBJDIR)
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/ostree_cmds.c

$(OBJDIR)/cli/repair.o: src/cli/repair.c
	@mkdir -p $(OBJDIR)/cli
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/cli/repair.c

$(OBJDIR)/daemon/ops/dbus_proxy.o: src/daemon/ops/dbus_proxy.c
	@mkdir -p $(OBJDIR)/daemon/ops
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/daemon/ops/dbus_proxy.c

$(OBJDIR)/daemon/ops/typed_worker.o: src/daemon/ops/typed_worker.c
	@mkdir -p $(OBJDIR)/daemon/ops
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/daemon/ops/typed_worker.c

$(OBJDIR)/daemon/ops/instance_ctl.o: src/daemon/ops/instance_ctl.c
	@mkdir -p $(OBJDIR)/daemon/ops
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/daemon/ops/instance_ctl.c

$(OBJDIR)/daemon/ops/store_write.o: src/daemon/ops/store_write.c
	@mkdir -p $(OBJDIR)/daemon/ops
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/daemon/ops/store_write.c

# The daemon's replacement for the terminal library.  It is deliberately the
# one non-shim object built without the getenv poison: the worker has already
# applied the allowlist, and these reads are the daemon's own names.
$(OBJDIR)/daemon/ops/evui.o: src/daemon/ops/evui.c
	@mkdir -p $(OBJDIR)/daemon/ops
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ src/daemon/ops/evui.c

$(OBJDIR)/daemon/ops/run.o: src/daemon/ops/run.c
	@mkdir -p $(OBJDIR)/daemon/ops
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/daemon/ops/run.c

$(OBJDIR)/daemon/bus.o: src/daemon/bus.c src/daemon/bus.h
	@mkdir -p $(OBJDIR)/daemon
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/daemon/bus.c

$(OBJDIR)/daemon/daemon.o: src/daemon/daemon.c
	@mkdir -p $(OBJDIR)/daemon
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/daemon/daemon.c

$(OBJDIR)/cli/portal.o: src/cli/portal.c
	@mkdir -p $(OBJDIR)/cli
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/cli/portal.c

$(OBJDIR)/daemon/sandbox.o: src/daemon/sandbox.c
	@mkdir -p $(OBJDIR)/daemon
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/daemon/sandbox.c

$(OBJDIR)/daemon/sandbox_root.o: src/daemon/sandbox_root.c
	@mkdir -p $(OBJDIR)/daemon
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/daemon/sandbox_root.c

$(OBJDIR)/daemon/sandbox_exec.o: src/daemon/sandbox_exec.c
	@mkdir -p $(OBJDIR)/daemon
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/daemon/sandbox_exec.c

$(OBJDIR)/cli/sessionhelper.o: src/cli/sessionhelper.c
	@mkdir -p $(OBJDIR)/cli
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/cli/sessionhelper.c

$(OBJDIR)/daemon/systemhelper.o: src/daemon/systemhelper.c
	@mkdir -p $(OBJDIR)/daemon
	$(CC) $(CFLAGS) $(NOENV) -MMD -MP -c -o $@ src/daemon/systemhelper.c

# Shim rules
$(DEVT_SHIM): $(DEVT_SHIM_SRC)
	@mkdir -p $(BUILD)
	/usr/local/bin/zig cc --target=x86_64-linux-gnu -shared -fPIC -O2 -pthread -o $@ $(DEVT_SHIM_SRC)

$(MEMFD_SHIM): $(MEMFD_SHIM_SRC)
	@mkdir -p $(BUILD)
	/usr/local/bin/zig cc --target=x86_64-linux-gnu -shared -fPIC -O2 -o $@ $(MEMFD_SHIM_SRC)

$(SYNC_SHIM): $(SYNC_SHIM_SRC)
	@mkdir -p $(BUILD)
	/usr/local/bin/zig cc --target=x86_64-linux-gnu -shared -fPIC -O2 -o $@ $(SYNC_SHIM_SRC)

$(RLIMIT_SHIM): $(RLIMIT_SHIM_SRC)
	@mkdir -p $(BUILD)
	/usr/local/bin/zig cc --target=x86_64-linux-gnu -shared -fPIC -O2 -o $@ \
	    $(RLIMIT_SHIM_SRC)

$(MADVISE_SHIM): $(MADVISE_SHIM_SRC)
	@mkdir -p $(BUILD)
	/usr/local/bin/zig cc --target=x86_64-linux-gnu -shared -fPIC -O2 -o $@ \
	    $(MADVISE_SHIM_SRC)

$(SHIM): $(SHIM_SRC) src/signalfd_shim.map
	/usr/local/bin/zig cc --target=x86_64-linux-gnu -O2 -fPIC -shared -o $@ \
	    $(SHIM_SRC) -Wl,--version-script=src/signalfd_shim.map

$(SECCOMP_SHIM): $(SECCOMP_SRC) src/seccomp_shim.map
	/usr/local/bin/zig cc --target=x86_64-linux-gnu -O2 -fPIC -shared -o $@ \
	    $(SECCOMP_SRC) -Wl,--version-script=src/seccomp_shim.map

# Dependency includes
sinclude $(OBJDIR)/cli/dispatch.d
sinclude $(OBJDIR)/cli/cli.d
sinclude $(OBJDIR)/cli/client.d
sinclude $(OBJDIR)/cli/client_typed.d
sinclude $(OBJDIR)/cli/solver.d
sinclude $(OBJDIR)/cli/tb_core.d
sinclude $(OBJDIR)/cli/tb_progress.d
sinclude $(OBJDIR)/cli/tb_tui.d
sinclude $(OBJDIR)/cli/upgrade.d
sinclude $(OBJDIR)/cli/translations.d
sinclude $(OBJDIR)/daemon/daemon.d
sinclude $(OBJDIR)/daemon/bus.d
sinclude $(OBJDIR)/cli/portal.d
sinclude $(OBJDIR)/daemon/sandbox.d
sinclude $(OBJDIR)/daemon/sandbox_root.d
sinclude $(OBJDIR)/daemon/sandbox_exec.d
sinclude $(OBJDIR)/cli/sessionhelper.d
sinclude $(OBJDIR)/daemon/systemhelper.d
sinclude $(OBJDIR)/shared/metadata.d
sinclude $(OBJDIR)/shared/permissions.d
sinclude $(OBJDIR)/shared/overrides.d
sinclude $(OBJDIR)/daemon/extensions.d
sinclude $(OBJDIR)/shared/history.d
sinclude $(OBJDIR)/shared/profile.d
sinclude $(OBJDIR)/shared/dbus.d
sinclude $(OBJDIR)/shared/config.d
sinclude $(OBJDIR)/shared/store.d
sinclude $(OBJDIR)/shared/protocol.d
sinclude $(OBJDIR)/shared/instance.d
sinclude $(OBJDIR)/shared/modules.d
sinclude $(OBJDIR)/shared/util.d
sinclude $(OBJDIR)/ostree.d
sinclude $(OBJDIR)/ostree_plan.d
sinclude $(OBJDIR)/ostree_cmds.d
sinclude $(OBJDIR)/cli/repair.d
sinclude $(OBJDIR)/daemon/ops/dbus_proxy.d
sinclude $(OBJDIR)/daemon/ops/typed_worker.d
sinclude $(OBJDIR)/daemon/ops/evui.d
sinclude $(OBJDIR)/daemon/ops/run.d
sinclude $(OBJDIR)/daemon/ops/instance_ctl.d
sinclude $(OBJDIR)/daemon/ops/store_write.d

clean:
	rm -rf $(BUILD)

# The port has no upstream tag to fetch: package this checkout instead.
# Fixed owner and mtime plus gzip -n make the tarball reproducible, so
# distinfo only changes when a tracked file does.
VERSION=        0.1.0
DISTNAME=       flatpkg-$(VERSION)
DISTDIR?=       ports/dist

dist:
	@mkdir -p $(DISTDIR)
	@tmp=$$(mktemp); git ls-files | sort -u | grep -v '^ports/' | \
	    while IFS= read -r f; do [ -f "$$f" ] && printf '%s\n' "$$f"; done \
	        > $$tmp; \
	    tar --format=ustar --uid 0 --gid 0 --uname root --gname wheel \
	        --mtime '2020-01-01 00:00:00' \
	        -s ',^,$(DISTNAME)/,' -cf - -T $$tmp | \
	        gzip -9n > $(DISTDIR)/$(DISTNAME).tar.gz; \
	    rm -f $$tmp
	@echo "wrote $(DISTDIR)/$(DISTNAME).tar.gz"
	@sha256 -q $(DISTDIR)/$(DISTNAME).tar.gz

$(MANPAGE): $(BIN_CLI) man/flatpkg.8.in scripts/gen-man.sh
	sh scripts/gen-man.sh $(BIN_CLI) man/flatpkg.8.in > $@

install: $(BIN_CLI) $(BIN_DAEMON) $(SHLIB) $(SHIM) $(SECCOMP_SHIM) $(DEVT_SHIM) $(MEMFD_SHIM) \
    $(SYNC_SHIM) $(RLIMIT_SHIM) $(MADVISE_SHIM) $(MANPAGE)
	install -d ${DESTDIR}${PREFIX}/sbin
	install -o root -g wheel -m 0755 $(BIN_CLI) ${DESTDIR}${PREFIX}/sbin/flatpkg
	install -o root -g wheel -m 0755 $(BIN_DAEMON) ${DESTDIR}${PREFIX}/sbin/flatpkgd
	install -d ${DESTDIR}${PREFIX}/lib
	install -o root -g wheel -m 0755 $(SHLIB) ${DESTDIR}${PREFIX}/lib/libflatpkg_shared.so
	install -d ${DESTDIR}${PREFIX}/bin
	ln -sf ../sbin/flatpkg ${DESTDIR}${PREFIX}/bin/flatpak
	install -d ${DESTDIR}/var/lib
	ln -sfn ${FLATPKG_STORE_DIR} ${DESTDIR}/var/lib/flatpkg
	ln -sfn flatpkg ${DESTDIR}/var/lib/flatpak
	install -d ${DESTDIR}${PREFIX}/man/man8
	install -m 0444 $(MANPAGE) ${DESTDIR}${PREFIX}/man/man8/flatpkg.8
	install -d ${DESTDIR}${PREFIX}/etc/rc.d
	sed -e 's|@PREFIX@|${PREFIX}|g' scripts/rc.d/flatpkgd \
	    > ${DESTDIR}${PREFIX}/etc/rc.d/flatpkgd
	chown root:wheel ${DESTDIR}${PREFIX}/etc/rc.d/flatpkgd
	chmod 0755 ${DESTDIR}${PREFIX}/etc/rc.d/flatpkgd
	install -d ${DESTDIR}${PREFIX}/lib/flatpkg
	install -o root -g wheel -m 0644 $(SHIM) ${DESTDIR}${PREFIX}/lib/flatpkg/signalfd_shim.so
	install -o root -g wheel -m 0644 $(SECCOMP_SHIM) \
	    ${DESTDIR}${PREFIX}/lib/flatpkg/seccomp_shim.so
	install -o root -g wheel -m 0644 $(DEVT_SHIM) \
	    ${DESTDIR}${PREFIX}/lib/flatpkg/dmabuf_devt_shim.so
	install -o root -g wheel -m 0644 $(MEMFD_SHIM) \
	    ${DESTDIR}${PREFIX}/lib/flatpkg/memfd_shim.so
	install -o root -g wheel -m 0644 $(SYNC_SHIM) \
	    ${DESTDIR}${PREFIX}/lib/flatpkg/syncobj_shim.so
	install -o root -g wheel -m 0644 $(RLIMIT_SHIM) \
	    ${DESTDIR}${PREFIX}/lib/flatpkg/rlimit_shim.so
	install -o root -g wheel -m 0644 $(MADVISE_SHIM) \
	    ${DESTDIR}${PREFIX}/lib/flatpkg/madvise_shim.so
	install -d ${DESTDIR}${PREFIX}/share/dbus-1/services
	install -m 0644 scripts/org.freedesktop.portal.Flatpak.service \
	    ${DESTDIR}${PREFIX}/share/dbus-1/services/
	install -m 0644 scripts/org.freedesktop.Flatpak.service \
	    ${DESTDIR}${PREFIX}/share/dbus-1/services/
	install -d ${DESTDIR}${PREFIX}/share/dbus-1/system-services
	install -m 0644 scripts/org.freedesktop.Flatpak.SystemHelper.service \
	    ${DESTDIR}${PREFIX}/share/dbus-1/system-services/
	install -d ${DESTDIR}${PREFIX}/etc/flatpkg
	install -m 0644 scripts/flatpkg-helper.conf \
	    ${DESTDIR}${PREFIX}/etc/flatpkg/helper.conf
	install -d ${DESTDIR}${PREFIX}/etc/dbus-1/system.d
	install -m 0644 scripts/org.freedesktop.Flatpak.SystemHelper.conf \
	    ${DESTDIR}${PREFIX}/etc/dbus-1/system.d/
	install -d ${DESTDIR}${PREFIX}/etc/profile.d
	install -m 0644 scripts/flatpkg.sh ${DESTDIR}${PREFIX}/etc/profile.d/flatpkg.sh
	install -d ${DESTDIR}${PREFIX}/lib/environment.d
	install -m 0644 scripts/99-flatpkg.conf \
	    ${DESTDIR}${PREFIX}/lib/environment.d/99-flatpkg.conf
	install -d ${DESTDIR}${PREFIX}/share/bash-completion/completions
	install -m 0644 completions/flatpkg.bash \
	    ${DESTDIR}${PREFIX}/share/bash-completion/completions/flatpkg
	install -m 0644 completions/flatpkg.bash \
	    ${DESTDIR}${PREFIX}/share/bash-completion/completions/flatpak
	install -d ${DESTDIR}${PREFIX}/share/zsh/site-functions
	install -m 0644 completions/_flatpkg \
	    ${DESTDIR}${PREFIX}/share/zsh/site-functions/_flatpkg
	install -d ${DESTDIR}${PREFIX}/share/fish/vendor_completions.d
	install -m 0644 completions/flatpkg.fish \
	    ${DESTDIR}${PREFIX}/share/fish/vendor_completions.d/flatpkg.fish
	install -m 0644 completions/flatpkg.fish \
	    ${DESTDIR}${PREFIX}/share/fish/vendor_completions.d/flatpak.fish
