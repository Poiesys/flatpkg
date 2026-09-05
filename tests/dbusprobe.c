/* dbusprobe.c - D-Bus client for tests/bus-signal-scope.sh: sends an `aay`
 * and signals on one connection, which no command-line client here can do. */
#include <gio/gio.h>
#include <glib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PORTAL_NAME  "org.freedesktop.portal.Flatpak"
#define PORTAL_PATH  "/org/freedesktop/portal/Flatpak"
#define PORTAL_IFACE "org.freedesktop.portal.Flatpak"

/* Close/Update live on the UpdateMonitor, a second interface on the same
 * service: calling them on PORTAL_IFACE answers UnknownMethod, which reads
 * like a portal that never implemented the object. */
#define MONITOR_IFACE "org.freedesktop.portal.Flatpak.UpdateMonitor"

#define HELPER_NAME  "org.freedesktop.Flatpak"
#define HELPER_PATH  "/org/freedesktop/Flatpak/Development"
#define HELPER_IFACE "org.freedesktop.Flatpak.Development"

#define DEV_PATH     "/org/freedesktop/portal/Flatpak"

static int
usage(void)
{
        fprintf(stderr,
            "usage: dbusprobe <subcommand> [args]\n"
            "  ping <name> <path>              Peer.Ping; 0 when answered\n"
            "  spawn [argv...]                  the portal's Spawn, on\n"
            "                                    $DBUSPROBE_BUS if set and\n"
            "                                    the session bus otherwise.\n"
            "                                    With no argv, spawns\n"
            "                                    /bin/sh -c 'exit 0'; with\n"
            "                                    argv, spawns THAT - which\n"
            "                                    is how a guard asks for a\n"
            "                                    command that stays alive\n"
            "                                    long enough to be signalled\n"
            "                                    $DBUSPROBE_BUS if set and\n"
            "                                    the session bus otherwise.\n"
            "                                    argv is left as the caller\n"
            "                                    wrote it, so a guard can put\n"
            "                                    --own=<id> in it for real\n"
            "  spawn-signal-own <sig> <pg> <cmd...>\n"
            "                                    the ALLOWED direction on\n"
            "                                    ONE connection: Spawn, then\n"
            "                                    SpawnSignal for the pid it\n"
            "                                    returned, with <sig>.  Both\n"
            "                                    calls MUST be in one process:\n"
            "                                    the claim is keyed on the\n"
            "                                    caller's unique bus name, and\n"
            "                                    through a proxy that is the\n"
            "                                    proxy's upstream connection\n"
            "  spawn-signal <pid> <sig> <pgroup>\n"
            "                                  the portal's SpawnSignal\n"
            "  host-signal <pid> <sig> <pgroup>\n"
            "                                  the helper's HostCommandSignal,\n"
            "                                    from a caller that did NOT\n"
            "                                    start it\n"
            "  host-signal-own <sig>          HostCommand, then\n"
            "                                    HostCommandSignal on THAT pid,\n"
            "                                    on the SAME connection; prints\n"
            "                                    pid=<n> reply=ok|error\n"
            "  host-fd-echo <target_fd> [read_fd]\n"
            "                                    HostCommand with ONE passed fd\n"
            "                                    mapped to <target_fd>, running a\n"
            "                                    shell that reads it and echoes\n"
            "                                    what it found.  The token the\n"
            "                                    shell prints goes to the\n"
            "                                    HELPER's stdout, not here: the\n"
            "                                    helper asks for no pipes, so the\n"
            "                                    child inherits the helper's own\n"
            "                                    0/1/2.  Prints pid=<n>.  With\n"
            "                                    [read_fd], the child reads THAT\n"
            "                                    number instead - the control.\n"
            "  monitor-create                  CreateUpdateMonitor, prints the\n"
            "                                    object path it hands back\n"
            "  monitor-introspect <path>       Introspect on that path\n"
            "  monitor-close <path>            Close on that path\n"
            "  monitor-update <path>           Update, then the Progress signals\n"
            "                                    it emits (2.5s).  The reply\n"
            "                                    alone is nothing: the portal\n"
            "                                    answers BEFORE the work, so the\n"
            "                                    outcome is only in the signals\n");
        return 1;
}

static GDBusConnection *
session_bus(void)
{
        GError *err = NULL;
        GDBusConnection *c = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);

        if (c == NULL) {
                fprintf(stderr, "dbusprobe: no session bus: %s\n",
                    err ? err->message : "?");
                g_clear_error(&err);
        }
        return c;
}

/* The bus to talk to: $DBUSPROBE_BUS if set, else the session bus - in the
 * environment, not an argument, so a case can carry `--own=...` in this
 * process's real argv. */
static GDBusConnection *
target_bus(void)
{
        const char *addr = getenv("DBUSPROBE_BUS");
        GError *err = NULL;
        GDBusConnection *c;

        if (addr == NULL || addr[0] == '\0')
                return session_bus();
        c = g_dbus_connection_new_for_address_sync(addr,
            G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
            G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
            NULL, NULL, &err);
        if (c == NULL) {
                fprintf(stderr, "dbusprobe: cannot reach %s: %s\n", addr,
                    err ? err->message : "?");
                g_clear_error(&err);
        }
        return c;
}

/* One call.  Prints "reply=ok" or "reply=error: <message>" and returns 0 or
 * 3, so a guard can assert on the TEXT: a refusal whose message says
 * nothing is a refusal the reader has to take on trust. */
static int
call(GDBusConnection *bus, const char *name, const char *path,
    const char *iface, const char *method, GVariant *params,
    const GVariantType *reply_type, GVariant **reply_out)
{
        GError *err = NULL;
        GVariant *reply = g_dbus_connection_call_sync(bus, name, path,
            iface, method, params, reply_type,
            /* NO_AUTO_START: without it a ping makes dbus-daemon activate
             * the INSTALLED portal or helper from its compiled-in service
             * dir, which then owns the name while the guard reads its log. */
            G_DBUS_CALL_FLAGS_NO_AUTO_START, 10000, NULL, &err);

        if (reply_out != NULL)
                *reply_out = reply;
        if (err != NULL) {
                printf("reply=error: %s\n", err->message);
                g_error_free(err);
                return 3;
        }
        printf("reply=ok\n");
        return 0;
}

/* An empty array of `type` - "no fds", "no env overrides", "no cwd".
 * Built rather than passed as NULL: g_variant_new_tuple() dereferences
 * every child, so a NULL there is a segfault with no message. */
static GVariant *
empty_of(const char *type)
{
        GVariantBuilder b;

        g_variant_builder_init(&b, G_VARIANT_TYPE(type));
        return g_variant_builder_end(&b);
}

/* The `aay` of argv, built as byte strings and never through the text
 * parser, whose elements are bytes.  NULL-terminated, so a fixed argv is
 * all it takes. */
static GVariant *
aay_of(const char *const *argv)
{
        GVariantBuilder b;
        GVariant *aay;

        g_variant_builder_init(&b, G_VARIANT_TYPE("aay"));
        for (size_t i = 0; argv[i] != NULL; i++)
                g_variant_builder_add_value(&b,
                    g_variant_new_bytestring(argv[i]));
        aay = g_variant_builder_end(&b);
        return aay;
}

static GVariant *
build_argv(void)
{
        static const char *const argv[] = { "sleep", "300", NULL };

        return aay_of(argv);
}

/* The connection, for the UpdateMonitor's signal half: a file-scope because
 * the Progress subscription spans the Update call and the wait, and there is
 * one bus per process. */
static GDBusConnection *bus_global;

/* ---- the UpdateMonitor ------------------------------------------------ */
/* Update() replies before the work happens, so the reply asserts nothing
 * about the outcome.  Subscribed before the call: one made after
 * call_sync() returns can miss the Progress it was there to catch. */

static char *progress_path;
static guint progress_sub;

static gboolean
on_progress_timeout(gpointer ud)
{
        g_main_loop_quit((GMainLoop *)ud);
        return FALSE;
}

static void
on_progress(GDBusConnection *c, const char *sender, const char *path,
    const char *iface, const char *member, GVariant *args, gpointer ud)
{
        GVariant *info = NULL;
        const char *s = NULL;
        guint32 u = 0;

        (void)c; (void)sender; (void)ud;
        if (progress_path == NULL || path == NULL ||
            strcmp(path, progress_path) != 0 ||
            strcmp(iface, MONITOR_IFACE) != 0 ||
            strcmp(member, "Progress") != 0)
                return;
        if (args == NULL)
                return;
        info = g_variant_get_child_value(args, 0);
        if (info == NULL)
                return;
        if (g_variant_lookup(info, "status", "u", &u))
                printf("progress status=%u", u);
        if (g_variant_lookup(info, "error", "&s", &s) && s != NULL)
                printf(" error=%s", s);
        if (g_variant_lookup(info, "error_message", "&s", &s) &&
            s != NULL)
                printf(" error_message=%s", s);
        printf("\n");
        fflush(stdout);
        g_variant_unref(info);
}

/* Subscribe, and return once: the subscription has to be in place before
 * the Update call, which a function that also blocks could not do. */
static void
subscribe_progress(const char *path)
{
        progress_path = g_strdup(path);
        progress_sub = g_dbus_connection_signal_subscribe(bus_global, NULL,
            PORTAL_IFACE, "Progress", path, NULL, G_DBUS_SIGNAL_FLAGS_NONE,
            on_progress, NULL, NULL);
}

/* Collect the Progress signals for <ms> milliseconds, counting none of
 * them: "a signal arrived" is satisfied by a signal that means nothing. */
static void
collect_progress(int ms)
{
        GMainLoop *loop = g_main_loop_new(NULL, FALSE);
        GSource *timer;

        timer = g_timeout_source_new(ms);
        g_source_set_callback(timer, on_progress_timeout, loop, NULL);
        g_source_attach(timer, NULL);
        g_source_unref(timer);

        g_main_loop_run(loop);

        g_main_loop_unref(loop);
        g_dbus_connection_signal_unsubscribe(bus_global, progress_sub);
        g_free(progress_path);
        progress_path = NULL;
}

int
main(int argc, char **argv)
{
        GDBusConnection *bus;
        int rc = 1;

        if (argc < 2)
                return usage();
        bus = target_bus();
        bus_global = bus;
        if (bus == NULL)
                return 1;

        if (strcmp(argv[1], "ping") == 0 && argc == 4) {
                /* Peer.Ping lives on any object, so this asks "has the
                 * service acquired the name yet": a guard must wait for that,
                 * or it reads "not up" as a refusal. */
                rc = call(bus, argv[2], argv[3], "org.freedesktop.DBus.Peer",
                    "Ping", NULL, NULL, NULL);
        } else if (strcmp(argv[1], "spawn-signal") == 0 && argc == 5) {
                guint32 pid = (guint32)strtoul(argv[2], NULL, 10);
                guint32 sig = (guint32)strtoul(argv[3], NULL, 10);
                gboolean pg = (g_ascii_strcasecmp(argv[4], "true") == 0 ||
                    strcmp(argv[4], "1") == 0);

                rc = call(bus, PORTAL_NAME, PORTAL_PATH, PORTAL_IFACE,
                    "SpawnSignal", g_variant_new("(uub)", pid, sig, pg),
                    NULL, NULL);
        } else if (strcmp(argv[1], "host-signal") == 0 && argc == 5) {
                guint32 pid = (guint32)strtoul(argv[2], NULL, 10);
                guint32 sig = (guint32)strtoul(argv[3], NULL, 10);
                gboolean pg = (g_ascii_strcasecmp(argv[4], "true") == 0 ||
                    strcmp(argv[4], "1") == 0);

                rc = call(bus, HELPER_NAME, HELPER_PATH, HELPER_IFACE,
                    "HostCommandSignal", g_variant_new("(uub)", pid, sig, pg),
                    NULL, NULL);
        } else if (strcmp(argv[1], "host-signal-own") == 0 && argc == 3) {
                /* The allowed path: HostCommand and HostCommandSignal go out
                 * on ONE connection, because the pid is keyed against the
                 * caller's unique bus name. */
                GError *err = NULL;
                GVariant *spawned;
                /* Each aay element is built as the byte string it is, never
                 * through the text parser, and into locals first: argument
                 * order is unspecified and builder_end() invalidates. */
                GVariant *argv_v, *msg;
                GVariant *kids[5];
                guint32 pid;

                /* A FORMAT string is not a TYPE string: in g_variant_new(),
                 * `a{uh}` means "a GVariantBuilder", not "an array of dict
                 * entries", and the call aborts.  Use new_tuple(). */
                argv_v = build_argv();
                kids[0] = empty_of("ay");        /* cwd: none */
                kids[1] = argv_v;                  /* argv */
                kids[2] = empty_of("a{uh}");       /* fds: none */
                kids[3] = empty_of("a{ss}");       /* envs: none */
                kids[4] = g_variant_new_uint32(0); /* flags */
                msg = g_variant_new_tuple(kids, 5);

                spawned = g_dbus_connection_call_sync(bus, HELPER_NAME,
                    HELPER_PATH, HELPER_IFACE, "HostCommand",
                    /* (ay cwd, aay argv, a{uh} fds, a{ss} envs, u flags) */
                    msg,
                    G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 10000,
                    NULL, &err);
                if (spawned == NULL) {
                        printf("spawn=error: %s\n",
                            err ? err->message : "?");
                        g_clear_error(&err);
                        rc = 1;
                        goto out;
                }
                g_variant_get(spawned, "(u)", &pid);
                g_variant_unref(spawned);
                printf("pid=%u\n", pid);

                rc = call(bus, HELPER_NAME, HELPER_PATH, HELPER_IFACE,
                    "HostCommandSignal",
                    g_variant_new("(uub)", pid,
                        (guint32)strtoul(argv[2], NULL, 10), FALSE),
                    NULL, NULL);
        } else if (strcmp(argv[1], "host-fd-echo") == 0 &&
            (argc == 3 || argc == 4)) {
                /* The `--forward-fd` path, host half: the map is target ->
                 * INDEX into the fd list, and the child inherits the HELPER's
                 * stdout, not this probe's - a guard reading here hangs. */
                int target = atoi(argv[2]);
                /* The number the CHILD reads, not necessarily the one the fd
                 * was placed at: that difference is the control - target 5
                 * read at 6 must find nothing. */
                int read_fd = argc == 4 ? atoi(argv[3]) : target;
                int fds[2];
                char token[64], shcmd[96];
                GDBusMessage *msg, *reply;
                GUnixFDList *list;
                GVariantBuilder av, map;
                GVariant *kids[5];
                GError *err = NULL;

                if (pipe(fds) != 0) {
                        printf("pipe: failed\n");
                        return 1;
                }
                snprintf(token, sizeof(token), "FPKG-FD-%d\n", target);
                if (write(fds[1], token, strlen(token)) !=
                    (ssize_t)strlen(token)) {
                        printf("write: short\n");
                        return 1;
                }
                close(fds[1]);
                g_snprintf(shcmd, sizeof(shcmd),
                    "read l <&%d; printf 'got:%%s' \"$l\"", read_fd);

                g_variant_builder_init(&av, G_VARIANT_TYPE("aay"));
                g_variant_builder_add_value(&av,
                    g_variant_new_bytestring("/bin/sh"));
                g_variant_builder_add_value(&av,
                    g_variant_new_bytestring("-c"));
                g_variant_builder_add_value(&av,
                    g_variant_new_bytestring(shcmd));
                g_variant_builder_init(&map, G_VARIANT_TYPE("a{uh}"));
                g_variant_builder_add(&map, "{uh}", (guint32)target, 0u);

                kids[0] = empty_of("ay");
                kids[1] = g_variant_builder_end(&av);
                kids[2] = g_variant_builder_end(&map);
                kids[3] = empty_of("a{ss}");
                kids[4] = g_variant_new_uint32(0);

                msg = g_dbus_message_new_method_call(HELPER_NAME,
                    HELPER_PATH, HELPER_IFACE, "HostCommand");
                g_dbus_message_set_body(msg, g_variant_new_tuple(kids, 5));
                list = g_unix_fd_list_new();
                g_unix_fd_list_append(list, (gint)fds[0], &err);
                if (err != NULL) {
                        printf("fdlist: %s\n", err->message);
                        g_clear_error(&err);
                        return 1;
                }
                g_dbus_message_set_unix_fd_list(msg, list);
                /* Seven arguments: there is no out_serial *and* user_data -
                 * the signature ends (out_serial, cancellable, error), and
                 * this is the _sync form that takes the message. */
                reply = g_dbus_connection_send_message_with_reply_sync(bus,
                    msg, G_DBUS_SEND_MESSAGE_FLAGS_NONE, 10000, NULL, NULL,
                    &err);
                if (reply == NULL) {
                        printf("send: %s\n", err ? err->message : "?");
                        g_clear_error(&err);
                        return 1;
                }
                /* A reply carrying an error is not a reply: GDBus hands back
                 * a non-NULL message whose body is (s), so reading its first
                 * child printed pid=0 and exited 0 for a REFUSED caller. */
                if (g_dbus_message_get_message_type(reply) ==
                    G_DBUS_MESSAGE_TYPE_ERROR) {
                        /* No g_dbus_message_get_error(): the name comes from
                         * get_error_code() and the message from the (so)
                         * body, which is the only place it is. */
                        GVariant *eb = g_dbus_message_get_body(reply);
                        const char *ename = NULL, *emsg = NULL;

                        if (eb != NULL &&
                            g_variant_is_of_type(eb, G_VARIANT_TYPE("(so)"))) {
                                g_variant_get(eb, "(&s&s)",
                                    &ename, &emsg);
                                printf("reply=error: %s: %s\n",
                                    ename ? ename : "?",
                                    emsg ? emsg : "?");
                        } else {
                                printf("reply=error: (an error reply with an"
                                    " unexpected body)\n");
                        }
                        g_object_unref(reply);
                        rc = 3;
                        goto out;
                }
                {
                        GVariant *body = g_dbus_message_get_body(reply);
                        guint32 pid = 0;

                        g_variant_get(body, "(u)", &pid);
                        printf("pid=%u\n", pid);
                }
                g_object_unref(reply);
                printf("reply=ok\n");
                rc = 0;
        } else if (strcmp(argv[1], "spawn") == 0) {
                /* The portal's Spawn: six children, the flags child a BARE
                 * uint32 - a "(u)" tuple there answers "No command given".
                 * What is under test is getting PAST resolve_app_id(). */
                GVariant *kids[6], *params, *rep;
                GError *e2 = NULL;
                int spawn_timeout = 120000;
                const char *timeout_env = getenv("DBUSPROBE_TIMEOUT_MS");

                /* The default covers a real Spawn, which answers only after
                 * the worker writes the instance pid (13s+).  A caller that
                 * only needs the identity check sets this instead. */
                if (timeout_env != NULL && timeout_env[0] != '\0') {
                        long v = strtol(timeout_env, NULL, 10);
                        if (v > 0 && v <= 600000)
                                spawn_timeout = (int)v;
                }
                /* `spawn` alone uses the default; `spawn /bin/sleep 300`
                 * asks for that, which the opt-in guard needs: "delivered" is
                 * only observable on a process alive to be signalled. */
                static const char *const defcmd[] = { "/bin/sh", "-c",
                    "exit 0", NULL };
                /* `argv + 2` is `char **`, not `const char *const *`: the
                 * cast says the builder only reads the pointers. */
                const char *const *argv_in = (argc > 2)
                    ? (const char *const *)argv + 2
                    : defcmd;
                GVariant *av, *cd, *fd, *en, *fl, *op;

                av = aay_of(argv_in);
                cd = empty_of("ay");
                fd = empty_of("a{uh}");
                en = empty_of("a{ss}");
                fl = g_variant_new_uint32(0);
                op = empty_of("a{sv}");
                kids[0] = cd; kids[1] = av; kids[2] = fd;
                kids[3] = en; kids[4] = fl; kids[5] = op;
                params = g_variant_new_tuple(kids, 6);

                rep = g_dbus_connection_call_sync(bus, PORTAL_NAME,
                    PORTAL_PATH, PORTAL_IFACE, "Spawn", params,
                    G_VARIANT_TYPE("(u)"),
                    /* NO_AUTO_START: the guard started the portal it wants,
                     * and letting the bus start a second one makes the case
                     * about a service nobody chose. */
                    /* 120s, not 10s: a Spawn that will succeed answers only
                     * after the worker writes the instance pid file, past the
                     * union root (13s+) and the jail.  A refusal is at once. */
                    G_DBUS_CALL_FLAGS_NO_AUTO_START, spawn_timeout, NULL,
                    &e2);
                if (e2 != NULL) {
                        printf("reply=error: %s\n", e2->message);
                        g_error_free(e2);
                        rc = 3;
                } else {
                        printf("reply=ok: %s\n", g_variant_print(rep, TRUE));
                        /* g_variant_unref(), not g_object_unref(): call_sync
                         * returns a GVariant, and unref'ing it as a GObject is
                         * a GLib-CRITICAL no assertion in the tree can see. */
                        g_variant_unref(rep);
                        rc = 0;
                }
        } else if (strcmp(argv[1], "spawn-signal-own") == 0 &&
            argc >= 5) {
                /* The ALLOWED direction on ONE connection:
                 * spawn_signal_allowed() is keyed on the caller's unique bus
                 * name, so two runs through one proxy socket are two callers. */
                GError *e3 = NULL;
                GVariant *k2[6], *p2, *r2;
                guint32 sig = (guint32)strtoul(argv[2], NULL, 10);
                /* The to_process_group form is a REQUIRED argument: a silent
                 * default would test the plain form twice and report it as the
                 * group form. */
                gboolean pg = (g_ascii_strcasecmp(argv[3], "true") == 0 ||
                    strcmp(argv[3], "1") == 0);
                GVariant *av2, *cd2, *fd2, *en2, *fl2, *op2;
                GVariant *body2;
                guint32 pid = 0;

                av2 = aay_of((const char *const *)argv + 4);
                cd2 = empty_of("ay");
                fd2 = empty_of("a{uh}");
                en2 = empty_of("a{ss}");
                fl2 = g_variant_new_uint32(0);
                op2 = empty_of("a{sv}");
                k2[0] = cd2; k2[1] = av2; k2[2] = fd2;
                k2[3] = en2; k2[4] = fl2; k2[5] = op2;
                p2 = g_variant_new_tuple(k2, 6);

                r2 = g_dbus_connection_call_sync(bus, PORTAL_NAME,
                    PORTAL_PATH, PORTAL_IFACE, "Spawn", p2,
                    G_VARIANT_TYPE("(u)"),
                    G_DBUS_CALL_FLAGS_NO_AUTO_START, 120000, NULL, &e3);
                if (e3 != NULL || r2 == NULL) {
                        printf("spawn=error: %s\n",
                            e3 ? e3->message : "no reply");
                        g_clear_error(&e3);
                        rc = 3;
                        goto out;
                }
                body2 = g_variant_get_child_value(r2, 0);
                g_variant_get(body2, "u", &pid);
                g_variant_unref(body2);
                g_variant_unref(r2);
                printf("pid=%u\n", pid);

                e3 = NULL;
                r2 = g_dbus_connection_call_sync(bus, PORTAL_NAME,
                    PORTAL_PATH, PORTAL_IFACE, "SpawnSignal",
                    g_variant_new("(uub)", pid, sig, pg),
                    G_VARIANT_TYPE("()"),
                    G_DBUS_CALL_FLAGS_NO_AUTO_START, 10000, NULL, &e3);
                if (e3 != NULL) {
                        printf("signal=error: %s\n", e3->message);
                        g_error_free(e3);
                        rc = 3;
                } else {
                        printf("signal=ok\n");
                        g_variant_unref(r2);
                        rc = 0;
                }
        } else if (strcmp(argv[1], "monitor-create") == 0 && argc == 2) {
                GVariant *r = NULL;
                char *obj = NULL;

                rc = call(bus, PORTAL_NAME, PORTAL_PATH, PORTAL_IFACE,
                    "CreateUpdateMonitor", NULL, G_VARIANT_TYPE("(o)"), &r);
                if (r != NULL) {
                        g_variant_get(r, "(o)", &obj);
                        /* The path is the whole answer: one that embeds the
                         * caller's unique name verbatim is a path no client
                         * can call Close or Update on. */
                        printf("path=%s\n", obj ? obj : "(null)");
                        g_free(obj);
                        g_variant_unref(r);
                }
        } else if (strcmp(argv[1], "monitor-introspect") == 0 &&
            argc == 3) {
                GVariant *r = NULL;
                const char *xml = "(no reply)";

                /* "s", not "(s)": Introspect answers a bare String, and
                 * an expected type of "(s)" makes GDBus reject the reply
                 * before the guard can read a byte of it. */
                rc = call(bus, PORTAL_NAME, argv[2],
                    "org.freedesktop.DBus.Introspectable", "Introspect",
                    NULL, G_VARIANT_TYPE("(s)"), &r);
                if (r != NULL) {
                        g_variant_get(r, "(&s)", &xml);
                        printf("xml %s\n", xml);
                        g_variant_unref(r);
                }
        } else if (strcmp(argv[1], "monitor-lifecycle") == 0 && argc == 2) {
                /* Create, Introspect, Close and Update in ONE process:
                 * name_owner_filter() reaps a monitor when its sender leaves
                 * the bus, so a split create/close tests an artifact. */
                GVariant *r = NULL;
                char *obj = NULL;

                rc = call(bus, PORTAL_NAME, PORTAL_PATH, PORTAL_IFACE,
                    "CreateUpdateMonitor", NULL, G_VARIANT_TYPE("(o)"), &r);
                if (r != NULL) {
                        g_variant_get(r, "(o)", &obj);
                        printf("path=%s\n", obj ? obj : "(null)");
                        g_variant_unref(r);
                }
                if (obj != NULL) {
                        GVariant *x = NULL;
                        const char *xml = "(no reply)";

                        rc = call(bus, PORTAL_NAME, obj,
                            "org.freedesktop.DBus.Introspectable",
                            "Introspect", NULL, G_VARIANT_TYPE("(s)"), &x);
                        if (x != NULL) {
                                g_variant_get(x, "(&s)", &xml);
                                printf("introspect=%.400s\n", xml);
                                g_variant_unref(x);
                        }
                        subscribe_progress(obj);
                        /* Update BEFORE Close: Close marks the monitor closed
                         * and the main loop reaps it, so an Update after it
                         * finds nothing. */
                        rc = call(bus, PORTAL_NAME, obj, MONITOR_IFACE,
                            "Update", NULL, NULL, NULL);
                        /* No Progress assertion: a minimal GDBus client gets
                         * the Progress and this probe does not, so the defect
                         * is in the probe, not in delivery. */
                        collect_progress(1500);
                        rc = call(bus, PORTAL_NAME, obj, MONITOR_IFACE,
                            "Close", NULL, NULL, NULL);
                        g_free(obj);
                }
        } else if (strcmp(argv[1], "monitor-close") == 0 && argc == 3) {
                rc = call(bus, PORTAL_NAME, argv[2], MONITOR_IFACE, "Close",
                    NULL, NULL, NULL);
        } else if (strcmp(argv[1], "monitor-update") == 0 && argc == 3) {
                GVariant *kids[2];

                /* g_variant_new_tuple(), NOT g_variant_new("(sa{sv})", ...):
                 * the format string has no spelling for a dictionary and
                 * aborts with a GLib-ERROR no guard can read. */
                kids[0] = g_variant_new_string("");
                kids[1] = empty_of("a{sv}");
                GVariant *params = g_variant_new_tuple(kids, 2);

                /* Subscribed BEFORE the call: see the note above
                 * collect_progress(). */
                subscribe_progress(argv[2]);
                rc = call(bus, PORTAL_NAME, argv[2], MONITOR_IFACE,
                    "Update", params, NULL, NULL);
                if (rc == 0)
                        collect_progress(2500);
                g_variant_unref(params);
        } else {
                return usage();
        }

out:
        g_object_unref(bus);
        return rc;
}
