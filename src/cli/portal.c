// org.freedesktop.portal.Flatpak v7 on libdbus; the sender is the app's
// xdg-dbus-proxy, or the app itself when it holds the raw grant.
#include <sys/file.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/wait.h>

#include <libutil.h>

#include <dbus/dbus.h>

#include <gio/gio.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "flatpkg.h"
#define FPKG_MODULE "portal"
#include "fpkglog.h"

extern void _dbus_message_get_unix_fds(DBusMessage* message, const int** fds,
				       unsigned* n_fds);

#define PORTAL_IFACE "org.freedesktop.portal.Flatpak"
#define PORTAL_PATH "/org/freedesktop/portal/Flatpak"
#define PORTAL_VERSION 7u

#define FILECHOOSER_IFACE "org.freedesktop.portal.FileChooser"
#define SETTINGS_IFACE "org.freedesktop.portal.Settings"
#define DESKTOP_PATH "/org/freedesktop/portal/desktop"

#define FC_RESPONSE_SUCCESS 0
#define FC_RESPONSE_CANCELLED 1
#define FC_RESPONSE_OTHER 2

#define FLAG_CLEAR_ENV (1u << 0)
#define FLAG_SANDBOX (1u << 2)
#define FLAG_NO_NETWORK (1u << 3)
#define FLAG_EXPOSE_PIDS (1u << 5)
#define FLAG_NOTIFY_START (1u << 6)
#define FLAG_SHARE_PIDS (1u << 7)
#define FLAG_EMPTY_APP (1u << 8)
#define FLAG_ALL (0x1ff)

#define SPAWN_POLL_MS 50
#define SPAWN_TIMEOUT_MS 180000
#define MAX_SPAWN_CHILDREN 64

#define SPAWN_MAX_FDS 64

static char portal_argv0[PATH_MAX] = "";

static void close_unforwarded_fds(const char* keep) {
	unsigned targets[SPAWN_MAX_FDS];
	size_t n = 0;

	if (keep && *keep) {
		const char* s = keep;
		while (*s && n < SPAWN_MAX_FDS) {
			char* end;
			long v = strtol(s, &end, 10);
			if (end == s) break;
			if (v >= 3) targets[n++] = (unsigned)v;
			s = (*end == ',') ? end + 1 : end;
		}
	}
	for (size_t i = 1; i < n; i++) {
		unsigned v = targets[i];
		size_t j = i;
		while (j > 0 && targets[j - 1] > v) {
			targets[j] = targets[j - 1];
			j--;
		}
		targets[j] = v;
	}
	unsigned low = 3;
	for (size_t i = 0; i < n; i++) {
		if (targets[i] > low) (void)close_range(low, targets[i] - 1, 0);
		low = targets[i] + 1;
	}
	(void)close_range(low, ~0u, 0);
}

static void free_strv(char** v) {
	if (!v) return;
	for (int i = 0; v[i]; i++) free(v[i]);
	free(v);
}

static void dict_add_uint(DBusMessageIter* arr, const char* key,
			  dbus_uint32_t val) {
	DBusMessageIter de, var;

	dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &de);
	dbus_message_iter_append_basic(&de, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&de, DBUS_TYPE_VARIANT, "u", &var);
	dbus_message_iter_append_basic(&var, DBUS_TYPE_UINT32, &val);
	dbus_message_iter_close_container(&de, &var);
	dbus_message_iter_close_container(arr, &de);
}

static void dict_add_string(DBusMessageIter* arr, const char* key,
			    const char* val) {
	DBusMessageIter de, var;

	dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &de);
	dbus_message_iter_append_basic(&de, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(&de, DBUS_TYPE_VARIANT, "s", &var);
	dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &val);
	dbus_message_iter_close_container(&de, &var);
	dbus_message_iter_close_container(arr, &de);
}

static bool dict_lookup(DBusMessageIter* entry, const char** key,
			DBusMessageIter* value) {
	*key = NULL;
	dbus_message_iter_recurse(entry, value);
	if (dbus_message_iter_get_arg_type(value) != DBUS_TYPE_STRING)
		return false;
	dbus_message_iter_get_basic(value, key);
	dbus_message_iter_next(value);
	return *key != NULL;
}

static char* proc_args(pid_t pid, size_t* len_out) {
	size_t len = 8192;
	for (;;) {
		char* buf = malloc(len);
		if (!buf) return NULL;
		int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ARGS, (int)pid};
		size_t got = len;
		if (sysctl(mib, 4, buf, &got, NULL, 0) == 0) {
			*len_out = got;
			return buf;
		}
		free(buf);
		if (errno == ENOMEM && len < (1u << 20)) {
			len *= 2;
			continue;
		}
		return NULL;
	}
}

static void instance_base(char* out, size_t n) {
	const char* env = cli_getenv("XDG_RUNTIME_DIR");
	if (env && env[0] == '/') {
		snprintf(out, n, "%s/.flatpak", env);
		return;
	}
	snprintf(out, n, "/var/run/user/%u/.flatpak", (unsigned)getuid());
}

static char** snapshot_instances(void) {
	char base[PATH_MAX];
	instance_base(base, sizeof(base));
	DIR* d = opendir(base);
	if (!d) return NULL;
	char** ids = NULL;
	size_t n = 0, cap = 0;
	struct dirent* de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.' || !de->d_name[0]) continue;
		bool numeric = true;
		for (const char* p = de->d_name; *p; p++)
			if (*p < '0' || *p > '9') numeric = false;
		if (!numeric) continue;
		if (n + 1 >= cap) {
			cap = cap ? cap * 2 : 16;
			char** new_ids = realloc(ids, cap * sizeof(char*));
			if (!new_ids) {
				closedir(d);
				return NULL;
			}
			ids = new_ids;
		}
		ids[n++] = xstrdup(de->d_name);
	}
	closedir(d);
	if (ids) ids[n] = NULL;
	return ids;
}

static bool in_snapshot(char** snap, const char* id) {
	if (!snap) return false;
	for (int i = 0; snap[i]; i++)
		if (strcmp(snap[i], id) == 0) return true;
	return false;
}

static void free_snapshot(char** snap) {
	if (!snap) return;
	for (int i = 0; snap[i]; i++) free(snap[i]);
	free(snap);
}

static int read_instance_info(const char* path, char* app_out, size_t app_n,
			      int* jid_out) {
	FILE* f = fopen(path, "r");
	if (!f) return -1;
	char line[512];
	char section[64] = "";
	app_out[0] = '\0';
	*jid_out = -1;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (line[0] == '[') {
			char* end = strchr(line, ']');
			if (end) {
				snprintf(section, sizeof(section), "%.*s",
					 (int)(end - line - 1), line + 1);
			}
			continue;
		}
		if (strcmp(section, "Application") == 0 &&
		    strncmp(line, "name=", 5) == 0) {
			snprintf(app_out, app_n, "%s", line + 5);
		} else if (strcmp(section, "Instance") == 0 &&
			   strncmp(line, "jid=", 4) == 0) {
			*jid_out = atoi(line + 4);
		}
	}
	fclose(f);
	return (app_out[0] && *jid_out > 0) ? 0 : -1;
}

static char* app_id_from_proxy_argv(pid_t pid) {
	size_t len = 0;
	char* args = proc_args(pid, &len);
	if (!args) return NULL;
	char* app = NULL;
	for (size_t i = 0; i < len;) {
		const char* a = args + i;
		if (strncmp(a, "--own=", 6) == 0) {
			const char* id = a + 6;
			size_t idlen = strlen(id);
			if (idlen > 2 && strcmp(id + idlen - 2, ".*") == 0)
				idlen -= 2;
			if (idlen > 0 && memchr(id, '.', idlen) != NULL) {
				app = xstrdup(id);
				app[idlen] = '\0';
				break;
			}
		}
		i += strlen(a) + 1;
	}
	free(args);
	return app;
}

static char* app_id_from_jail(int jid) {
	char base[PATH_MAX];
	instance_base(base, sizeof(base));
	DIR* d = opendir(base);
	if (!d) return NULL;
	char* app = NULL;
	struct dirent* de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.' || !de->d_name[0]) continue;
		char info[PATH_MAX];
		snprintf(info, sizeof(info), "%s/%s/info", base, de->d_name);
		char name[256];
		int j = -1;
		if (read_instance_info(info, name, sizeof(name), &j) == 0 &&
		    j == jid) {
			app = xstrdup(name);
			break;
		}
	}
	closedir(d);
	return app;
}

static unsigned long bus_get_owner_pid(DBusConnection* conn, const char* name) {
	DBusMessage* call = dbus_message_new_method_call(
		"org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "GetConnectionUnixProcessID");
	if (!call) return 0;
	dbus_message_append_args(call, DBUS_TYPE_STRING, &name,
				 DBUS_TYPE_INVALID);
	DBusError err;
	dbus_error_init(&err);
	DBusMessage* reply = dbus_connection_send_with_reply_and_block(
		conn, call, 10000, &err);
	dbus_message_unref(call);
	if (!reply) {
		dbus_error_free(&err);
		return 0;
	}
	dbus_uint32_t pid = 0;
	if (!dbus_message_get_args(reply, &err, DBUS_TYPE_UINT32, &pid,
				   DBUS_TYPE_INVALID))
		pid = 0;
	dbus_message_unref(reply);
	dbus_error_free(&err);
	return pid;
}

static char* resolve_app_id(DBusConnection* conn, const char* sender) {
	unsigned long pid = bus_get_owner_pid(conn, sender);
	if (pid == 0) return NULL;

	int jid = 0;
	char* exe = NULL;
	fp_caller_t who = flatpkg_caller_check((pid_t)pid, &jid, &exe);

	if (who == FP_CALLER_JAIL) return app_id_from_jail(jid);
	if (who == FP_CALLER_UNREADABLE)
		fpkg_warnf("spawn from pid %lu: cannot read what it "
			   "executed, so it is not a flatpkg sandbox",
			   pid);
	else if (who == FP_CALLER_NO)
		fpkg_warnf("spawn from pid %lu: '%s' is not flatpkg's "
			   "dbus proxy, so it is not a flatpkg sandbox",
			   pid, exe ? exe : "?");
	free(exe);
	if (who != FP_CALLER_PROXY) return NULL;
	return app_id_from_proxy_argv((pid_t)pid);
}

static const char* flatpkg_bin_path(char* buf, size_t n) {
	if (portal_argv0[0] && strchr(portal_argv0, '/')) {
		snprintf(buf, n, "%s", portal_argv0);
		if (access(buf, X_OK) == 0) return buf;
	}
	if (access("/usr/local/sbin/flatpkg", X_OK) == 0) {
		snprintf(buf, n, "/usr/local/sbin/flatpkg");
		return buf;
	}
	return NULL;
}

typedef struct {
	pid_t client_pid;
	pid_t app_pid;
	char* sender;
	bool notify_start;
	bool started_sent;
} spawn_child_t;

static spawn_child_t children[MAX_SPAWN_CHILDREN];
static size_t nchildren;

static void reap_children(DBusConnection* conn) {
	for (size_t i = 0; i < nchildren;) {
		spawn_child_t* c = &children[i];
		int status = 0;
		pid_t r = waitpid(c->client_pid, &status, WNOHANG);
		if (r == 0) {
			i++;
			continue;
		}
		if (r < 0 && errno == EINTR) continue;
		if (c->app_pid > 0) {
			fpkg_infof("spawn child %ld reaped: app pid %ld "
				   "status %d",
				   (long)c->client_pid, (long)c->app_pid,
				   status);
			DBusMessage* sig = dbus_message_new_signal(
				PORTAL_PATH, PORTAL_IFACE, "SpawnExited");
			if (sig) {
				dbus_message_set_destination(sig, c->sender);
				dbus_uint32_t apid = (dbus_uint32_t)c->app_pid;
				dbus_uint32_t wstatus = (dbus_uint32_t)status;
				dbus_message_append_args(
					sig, DBUS_TYPE_UINT32, &apid,
					DBUS_TYPE_UINT32, &wstatus,
					DBUS_TYPE_INVALID);
				dbus_connection_send(conn, sig, NULL);
				dbus_message_unref(sig);
			}
		}
		free(c->sender);
		children[i] = children[--nchildren];
	}
}

static void emit_spawn_started(DBusConnection* conn, spawn_child_t* c) {
	if (!c->notify_start || c->started_sent) return;
	DBusMessage* sig = dbus_message_new_signal(PORTAL_PATH, PORTAL_IFACE,
						   "SpawnStarted");
	if (sig) {
		dbus_message_set_destination(sig, c->sender);
		dbus_uint32_t apid = (dbus_uint32_t)c->app_pid;
		dbus_uint32_t relpid = 0;
		dbus_message_append_args(sig, DBUS_TYPE_UINT32, &apid,
					 DBUS_TYPE_UINT32, &relpid,
					 DBUS_TYPE_INVALID);
		dbus_connection_send(conn, sig, NULL);
		dbus_message_unref(sig);
	}
	c->started_sent = true;
}

static int iter_bytes_to_string(DBusMessageIter* it, char* out, size_t n) {
	DBusMessageIter sub;
	if (dbus_message_iter_get_arg_type(it) != DBUS_TYPE_ARRAY) return -1;
	dbus_message_iter_recurse(it, &sub);
	size_t pos = 0;
	while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
		if (pos + 1 >= n) return -1;
		unsigned char b;
		dbus_message_iter_get_basic(&sub, &b);
		out[pos++] = (char)b;
		dbus_message_iter_next(&sub);
	}
	out[pos] = '\0';
	return 0;
}

static int parse_spawn_args(DBusMessage* msg, char* cwd_out, size_t cwd_n,
			    char*** argv_out, char*** envs_out,
			    dbus_uint32_t* flags_out, int* fd_map_out) {
	DBusMessageIter it, sub;
	*cwd_out = '\0';
	*argv_out = NULL;
	*envs_out = NULL;
	*flags_out = 0;
	for (int i = 0; i < SPAWN_MAX_FDS; i++) fd_map_out[i] = -1;

	dbus_message_iter_init(msg, &it);

	if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
		if (iter_bytes_to_string(&it, cwd_out, cwd_n) != 0) return -1;
	} else {
		return -1;
	}
	dbus_message_iter_next(&it);

	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return -1;
	{
		char** argv = NULL;
		size_t n = 0, cap = 0;
		dbus_message_iter_recurse(&it, &sub);
		while (dbus_message_iter_get_arg_type(&sub) !=
		       DBUS_TYPE_INVALID) {
			if (n + 2 >= cap) {
				cap = cap ? cap * 2 : 16;
				char** new_argv =
					realloc(argv, cap * sizeof(char*));
				if (!new_argv) return -1;
				argv = new_argv;
			}
			char item[PATH_MAX * 2];
			if (iter_bytes_to_string(&sub, item, sizeof(item)) !=
			    0) {
				free_strv(argv);
				return -1;
			}
			argv[n++] = xstrdup(item);
			dbus_message_iter_next(&sub);
		}
		if (argv) argv[n] = NULL;
		*argv_out = argv;
	}
	dbus_message_iter_next(&it);

	// a{uh} fds: target fd -> index into the received unix-fd list.  An
	// index past the list falls back to positional mapping.
	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return -1;
	{
		const int* unix_fds = NULL;
		unsigned nfds = 0;
		_dbus_message_get_unix_fds(msg, &unix_fds, &nfds);
		struct {
			dbus_uint32_t key, h;
		} ents[16];
		int nents = 0;
		dbus_message_iter_recurse(&it, &sub);
		while (dbus_message_iter_get_arg_type(&sub) !=
			       DBUS_TYPE_INVALID &&
		       nents < 16) {
			DBusMessageIter de;
			dbus_uint32_t key = 0;
			dbus_uint32_t h = 0;
			dbus_message_iter_recurse(&sub, &de);
			if (dbus_message_iter_get_arg_type(&de) ==
			    DBUS_TYPE_UINT32)
				dbus_message_iter_get_basic(&de, &key);
			dbus_message_iter_next(&de);
			if (dbus_message_iter_get_arg_type(&de) ==
			    DBUS_TYPE_UNIX_FD)
				dbus_message_iter_get_basic(&de, &h);
			ents[nents].key = key;
			ents[nents].h = h;
			nents++;
			dbus_message_iter_next(&sub);
		}
		bool all_in_range = true;
		for (int i = 0; i < nents; i++)
			if (ents[i].h >= nfds) all_in_range = false;
		if (all_in_range) {
			for (int i = 0; i < nents; i++)
				if (ents[i].key < SPAWN_MAX_FDS)
					fd_map_out[ents[i].key] =
						unix_fds[ents[i].h];
		} else {
			for (int i = 0; i < nents; i++)
				for (int j = i + 1; j < nents; j++)
					if (ents[j].h < ents[i].h) {
						dbus_uint32_t tk = ents[i].key;
						dbus_uint32_t th = ents[i].h;
						ents[i] = ents[j];
						ents[j].key = tk;
						ents[j].h = th;
					}
			for (int i = 0; i < nents && i < (int)nfds; i++)
				if (ents[i].key < SPAWN_MAX_FDS)
					fd_map_out[ents[i].key] = unix_fds[i];
		}
	}
	dbus_message_iter_next(&it);

	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return -1;
	{
		char** envs = NULL;
		size_t n = 0, cap = 0;
		dbus_message_iter_recurse(&it, &sub);
		while (dbus_message_iter_get_arg_type(&sub) !=
		       DBUS_TYPE_INVALID) {
			DBusMessageIter de;
			const char *k = NULL, *v = NULL;
			dbus_message_iter_recurse(&sub, &de);
			if (dbus_message_iter_get_arg_type(&de) ==
			    DBUS_TYPE_STRING)
				dbus_message_iter_get_basic(&de, &k);
			dbus_message_iter_next(&de);
			if (dbus_message_iter_get_arg_type(&de) ==
			    DBUS_TYPE_STRING)
				dbus_message_iter_get_basic(&de, &v);
			if (k && v && strchr(k, '=') == NULL) {
				if (n + 2 >= cap) {
					cap = cap ? cap * 2 : 16;
					char** new_envs = realloc(
						envs, cap * sizeof(char*));
					if (!new_envs) return -1;
					envs = new_envs;
				}
				char* pair = malloc(strlen(k) + strlen(v) + 2);
				if (!pair) return -1;
				snprintf(pair, strlen(k) + strlen(v) + 2,
					 "%s=%s", k, v);
				envs[n++] = pair;
			}
			dbus_message_iter_next(&sub);
		}
		if (envs) envs[n] = NULL;
		*envs_out = envs;
	}
	dbus_message_iter_next(&it);

	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32) return -1;
	dbus_message_iter_get_basic(&it, flags_out);
	dbus_message_iter_next(&it);

	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return -1;
	// unset-env is accepted but not applied: Spawn starts from the
	// caller's environment.
	dbus_message_iter_recurse(&it, &sub);
	while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID)
		dbus_message_iter_next(&sub);
	return 0;
}

static void reply_error(DBusConnection* conn, DBusMessage* msg,
			const char* text) {
	DBusMessage* reply = dbus_message_new_error(
		msg, "org.freedesktop.DBus.Error.InvalidArgs", text);
	if (reply) {
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
	}
}

static void reply_uint32(DBusConnection* conn, DBusMessage* msg,
			 dbus_uint32_t value) {
	DBusMessage* reply = dbus_message_new_method_return(msg);
	if (!reply) return;
	dbus_message_append_args(reply, DBUS_TYPE_UINT32, &value,
				 DBUS_TYPE_INVALID);
	dbus_connection_send(conn, reply, NULL);
	dbus_message_unref(reply);
}

static DBusHandlerResult handle_message(DBusConnection* conn, DBusMessage* msg,
					void* ud);

static long long now_ms(void) {
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// While handle_spawn() polls it must not dispatch (a nested Spawn would re-enter
// the poll), but it must answer flatpak-spawn's read-only `version` query.
static void serve_queries_during_spawn(DBusConnection* conn) {
	for (;;) {
		DBusMessage* msg = dbus_connection_borrow_message(conn);
		if (msg == NULL) return;
		bool serve = false;
		if (dbus_message_get_type(msg) ==
		    DBUS_MESSAGE_TYPE_METHOD_CALL) {
			const char* iface = dbus_message_get_interface(msg);
			if (iface != NULL &&
			    (strcmp(iface, "org.freedesktop.DBus.Properties") ==
				     0 ||
			     strcmp(iface,
				    "org.freedesktop.DBus.Introspectable") ==
				     0))
				serve = true;
		}
		if (!serve) {
			dbus_connection_return_message(conn, msg);
			return;
		}
		dbus_connection_steal_borrowed_message(conn, msg);
		handle_message(conn, msg, NULL);
		dbus_message_unref(msg);
	}
}

static bool spawn_parse_request(DBusConnection* conn, DBusMessage* msg,
				char* cwd, size_t cwd_n, char*** argv,
				char*** envs, dbus_uint32_t* flags,
				int* fd_map) {
	if (parse_spawn_args(msg, cwd, cwd_n, argv, envs, flags, fd_map) != 0 ||
	    !*argv || !(*argv)[0]) {
		reply_error(conn, msg, "No command given");
		return false;
	}
	return true;
}

static bool spawn_validate_request(DBusConnection* conn, DBusMessage* msg,
				   const char* app_id, dbus_uint32_t flags,
				   const char* cwd) {
	if ((flags & ~FLAG_ALL) != 0) {
		fpkg_warnf("spawn from %s: unsupported flags 0x%x", app_id,
			   flags & ~FLAG_ALL);
		reply_error(conn, msg, "Unsupported flags enabled");
		return false;
	}
	if ((flags & (FLAG_EXPOSE_PIDS | FLAG_SHARE_PIDS)) != 0)
		fpkg_infof("spawn from %s: expose/share-pids accepted "
			   "(FreeBSD pids are global - the spawned pid is "
			   "already visible in the caller's namespace)",
			   app_id);
	if (flags & FLAG_EMPTY_APP)
		fpkg_warnf("spawn from %s: empty-app flag ignored", app_id);
	if (cwd[0])
		fpkg_warnf("spawn from %s: cwd '%s' ignored "
			   "(flatpkg run has no --cwd)",
			   app_id, cwd);
	return true;
}

// Per-spawn nonce: --env=FLATPKG_SPAWN_NONCE=<nonce> lands in the sandbox
// env, and the pid-file candidate is verified against it.
static void spawn_make_nonce(char* out, size_t n, DBusMessage* msg) {
	snprintf(out, n, "FLATPKG_SPAWN_NONCE=%lx%lx%lx",
		 (unsigned long)getpid(), (unsigned long)time(NULL),
		 (unsigned long)(uintptr_t)msg ^ arc4random());
}

// Build the re-exec argv in one pass; the nonce must be in the request
// frame.  Every entry is malloc'd (free_strv must not touch a literal).
static char** spawn_build_run(const char* bin, const char* app_id,
			      char** argv, char** envs, dbus_uint32_t flags,
			      const char* nonce) {
	size_t nargs = 0;

	while (argv[nargs]) nargs++;
	size_t nrun = 3 + 3 + 1 + 1;
	for (int i = 0; envs && envs[i]; i++) nrun++;
	nrun += 1 + nargs + 1;
	char** run = calloc(nrun, sizeof(char*));
	if (!run) return NULL;
	size_t r = 0;
	run[r++] = xstrdup(bin);
	run[r++] = xstrdup("run");
	if (flags & FLAG_CLEAR_ENV) run[r++] = xstrdup("--clearenv");
	if (flags & (FLAG_NO_NETWORK | FLAG_SANDBOX))
		run[r++] = xstrdup("--unshare=network");
	if (flags & FLAG_SANDBOX) run[r++] = xstrdup("--unshare=ipc");
	run[r++] = xasprintf("--env=%s", nonce);
	for (int i = 0; envs && envs[i]; i++)
		run[r++] = xasprintf("--env=%s", envs[i]);
	run[r++] = xasprintf("--command=%s", argv[0]);
	run[r++] = xstrdup(app_id);
	run[r++] = xstrdup("--");
	for (size_t i = 1; i < nargs; i++) run[r++] = xstrdup(argv[i]);
	run[r] = NULL;
	return run;
}

static void spawn_child_exec(const char* bin, char** run, const int* fd_map) {
	char keep[512];
	size_t k = 0;

	keep[0] = '\0';
	for (int i = 0; i < SPAWN_MAX_FDS; i++) {
		if (fd_map[i] < 0) continue;
		if (fd_map[i] != i) {
			if (dup2(fd_map[i], i) < 0) continue;
			close(fd_map[i]);
		}
		if (i >= 3 && k + 8 < sizeof(keep))
			k += (size_t)snprintf(keep + k, sizeof(keep) - k, "%s%d",
					      k ? "," : "", i);
	}
	close_unforwarded_fds(keep);
	if (k > 0) setenv("FLATPKG_FORWARD_FDS", keep, 1);
	// Its own process group, as sessionhelper.c does: killpg() reads its
	// argument as a group id, so a pid leading no group fails with ESRCH.
	setsid();
	setpgid(0, 0);
	execv(bin, run);
	_exit(127);
}

static void spawn_track_child(pid_t child, const char* sender,
			      dbus_uint32_t flags) {
	spawn_child_t* c = &children[nchildren++];

	memset(c, 0, sizeof(*c));
	c->client_pid = child;
	c->app_pid = -1;
	c->sender = xstrdup(sender);
	c->notify_start = (flags & FLAG_NOTIFY_START) != 0;
}

// The table filled between the check and the fork: the child is already
// running, so kill it rather than return a pid nothing can manage.
static void spawn_discard_child(pid_t child) {
	fpkg_errorf("spawn child table full after the fork;"
		    " killing %ld rather than returning an untrackable pid",
		    (long)child);
	kill(child, SIGTERM);
	waitpid(child, NULL, 0);
}

// One new instance dir: true when it names this app, carries this spawn's nonce
// and has a pid file. A candidate that never gets there is reported, since the reply waits for it.
static bool spawn_candidate_matches(const char* base, const char* id,
				    const char* app_id, const char* nonce,
				    long long deadline) {
	char info[PATH_MAX], noncef[PATH_MAX];
	bool cand_info = false, cand_name = false, cand_nonce = false;
	bool matched = false;

	snprintf(info, sizeof(info), "%s/%s/info", base, id);
	snprintf(noncef, sizeof(noncef), "%s/%s/spawn-nonce", base, id);
	long long cand_deadline = now_ms() + 2000;
	if (cand_deadline > deadline) cand_deadline = deadline;
	while (!matched && now_ms() < cand_deadline) {
		char name[256];
		int jid = -1;
		if (read_instance_info(info, name, sizeof(name), &jid) == 0) {
			cand_info = true;
			if (strcmp(name, app_id) == 0) {
				cand_name = true;
				bool nonce_ok = false;
				FILE* nf = fopen(noncef, "r");
				if (nf) {
					char nb[160];
					if (fgets(nb, sizeof(nb), nf)) {
						nb[strcspn(nb, "\r\n")] = '\0';
						nonce_ok =
							(strcmp(nb, nonce) == 0);
					}
					fclose(nf);
				}
				if (nonce_ok) {
					cand_nonce = true;
					char pidf[PATH_MAX];
					long p = 0;
					long long pd = now_ms() + 1000;

					snprintf(pidf, sizeof(pidf), "%s/%s/pid",
						 base, id);
					if (pd > cand_deadline)
						pd = cand_deadline;
					while (now_ms() < pd) {
						FILE* pf = fopen(pidf, "r");
						if (pf) {
							if (fscanf(pf, "%ld", &p) ==
								    1 &&
							    p > 0) {
								fclose(pf);
								matched = true;
								break;
							}
							fclose(pf);
						}
						usleep(2000);
					}
				}
			}
		}
		if (!matched) usleep(2000);
	}
	if (!matched && cand_info)
		fpkg_warnf("poll cand %s: info=%d name=%d nonce=%d", id,
			   cand_info, cand_name, cand_nonce);
	return matched;
}

// Poll for the instance dir the re-exec'd child creates.  The reply pid is
// the re-exec'd child, not the app's, matching SpawnStarted.
static dbus_uint32_t spawn_wait_for_instance(DBusConnection* conn,
					     const char* app_id, char** snap,
					     const char* nonce, pid_t child) {
	long long deadline = now_ms() + SPAWN_TIMEOUT_MS;

	while (now_ms() < deadline) {
		char base[PATH_MAX];
		instance_base(base, sizeof(base));
		DIR* d = opendir(base);
		if (d) {
			struct dirent* de;
			while ((de = readdir(d)) != NULL) {
				if (de->d_name[0] == '.' || !de->d_name[0])
					continue;
				bool numeric = true;
				for (const char* p = de->d_name; *p; p++)
					if (*p < '0' || *p > '9')
						numeric = false;
				if (!numeric) continue;
				if (in_snapshot(snap, de->d_name)) continue;
				if (spawn_candidate_matches(base, de->d_name,
							    app_id, nonce,
							    deadline)) {
					closedir(d);
					return (dbus_uint32_t)child;
				}
			}
			closedir(d);
		}
		serve_queries_during_spawn(conn);
		dbus_connection_read_write(conn, SPAWN_POLL_MS);
	}
	return 0;
}

static void spawn_set_app_pid(DBusConnection* conn, pid_t child,
			      dbus_uint32_t app_pid) {
	for (size_t i = 0; i < nchildren; i++) {
		if (children[i].client_pid == child) {
			children[i].app_pid = (pid_t)app_pid;
			emit_spawn_started(conn, &children[i]);
			break;
		}
	}
}

static void handle_spawn(DBusConnection* conn, DBusMessage* msg) {
	const char* sender = dbus_message_get_sender(msg);
	if (!sender) return;

	char* app_id = resolve_app_id(conn, sender);
	if (!app_id) {
		reply_error(conn, msg,
			    PORTAL_IFACE ".Spawn only works in a flatpak");
		return;
	}

	char cwd[PATH_MAX];
	char **argv = NULL, **envs = NULL;
	dbus_uint32_t flags = 0;
	int fd_map[SPAWN_MAX_FDS];
	if (!spawn_parse_request(conn, msg, cwd, sizeof(cwd), &argv, &envs,
				 &flags, fd_map) ||
	    !spawn_validate_request(conn, msg, app_id, flags, cwd)) {
		free(app_id);
		free_strv(argv);
		free_strv(envs);
		return;
	}

	char nonce[80];
	spawn_make_nonce(nonce, sizeof(nonce), msg);

	char bin[PATH_MAX];
	if (!flatpkg_bin_path(bin, sizeof(bin))) {
		fpkg_errorf("spawn from %s: flatpkg binary not found", app_id);
		free(app_id);
		free_strv(argv);
		free_strv(envs);
		reply_error(conn, msg, "flatpkg binary not found");
		return;
	}

	char** run = spawn_build_run(bin, app_id, argv, envs, flags, nonce);
	if (!run) {
		free(app_id);
		free_strv(argv);
		free_strv(envs);
		reply_error(conn, msg, "out of memory");
		return;
	}

	// The table must have room before the fork: otherwise the caller gets a pid it
	// could never SpawnSignal or hear SpawnExited for, behind a reply that says ok.
	if (nchildren >= MAX_SPAWN_CHILDREN) {
		fpkg_warnf("spawn from %s: %d children already tracked,"
			   " refusing rather than returning a pid that could"
			   " not be signalled or waited for",
			   sender, MAX_SPAWN_CHILDREN);
		reply_error(conn, msg,
			    "too many spawned processes"
			    " are still running; try again when one exits");
		free(app_id);
		free_strv(argv);
		free_strv(envs);
		free_strv(run);
		return;
	}

	char** snap = snapshot_instances();

	pid_t child = fork();
	if (child < 0) {
		free_strv(run);
		free_snapshot(snap);
		reply_error(conn, msg, "fork failed");
		return;
	}
	if (child == 0) spawn_child_exec(bin, run, fd_map);
	free_strv(run);
	free_strv(argv);
	free_strv(envs);

	// The room was checked before the fork; if it is somehow full the child is
	// running, so kill it rather than return an untrackable pid.
	if (nchildren < MAX_SPAWN_CHILDREN) {
		spawn_track_child(child, sender, flags);
	} else {
		spawn_discard_child(child);
		free_snapshot(snap);
		free(app_id);
		reply_error(conn, msg,
			    "internal error: the spawn"
			    " table filled after the process started");
		return;
	}

	dbus_uint32_t result_pid =
		spawn_wait_for_instance(conn, app_id, snap, nonce, child);
	free_snapshot(snap);

	if (result_pid == 0) {
		fpkg_warnf("spawn for %s: no instance appeared within %d ms",
			   sender, SPAWN_TIMEOUT_MS);
		free(app_id);
		reply_error(conn, msg, "Spawn failed: sandbox did not start");
		return;
	}

	spawn_set_app_pid(conn, child, result_pid);
	fpkg_infof("spawned %s pid=%u (via %s)", app_id, (unsigned)result_pid,
		   sender);
	free(app_id);
	reply_uint32(conn, msg, result_pid);
}

static bool spawn_signal_allowed(const char* sender, pid_t pid) {
	if (sender == NULL || pid <= 0) return false;
	for (size_t i = 0; i < nchildren; i++) {
		if (children[i].app_pid == pid && children[i].sender != NULL &&
		    strcmp(children[i].sender, sender) == 0)
			return true;
	}
	return false;
}

static void handle_spawn_signal(DBusConnection* conn, DBusMessage* msg) {
	dbus_uint32_t pid = 0, signal = 0;
	dbus_bool_t to_pgroup = FALSE;
	const char* sender;
	if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT32, &pid,
				   DBUS_TYPE_UINT32, &signal, DBUS_TYPE_BOOLEAN,
				   &to_pgroup, DBUS_TYPE_INVALID)) {
		reply_error(conn, msg, "Bad SpawnSignal arguments");
		return;
	}
	sender = dbus_message_get_sender(msg);
	if (!spawn_signal_allowed(sender, (pid_t)pid)) {
		reply_error(conn, msg,
			    "pid is not a process this caller "
			    "spawned through this portal");
		return;
	}
	int rc;
	if (to_pgroup) rc = killpg((pid_t)pid, (int)signal);
	else
		rc = kill((pid_t)pid, (int)signal);
	if (rc != 0) {
		fpkg_warnf("SpawnSignal: %s(%u, %u) failed: %s - the"
			   " process is still running",
			   to_pgroup ? "killpg" : "kill", (unsigned)pid,
			   (unsigned)signal, strerror(errno));
		char why[128];
		snprintf(why, sizeof(why), "signal was not delivered: %s",
			 strerror(errno));
		reply_error(conn, msg, why);
		return;
	}

	DBusMessage* reply = dbus_message_new_method_return(msg);
	if (reply) {
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
	}
}

#define UPDATE_IFACE "org.freedesktop.portal.Flatpak.UpdateMonitor"
#define UPDATE_MONITOR_PREFIX "/org/freedesktop/portal/Flatpak/update_monitor/"
#define MAX_UPDATE_MONITORS 64
#define MAX_UPDATE_CHILDREN 32

typedef struct {
	char* handle;
	char* sender;
	char* app_id;
	bool closed;
} update_monitor_t;

static update_monitor_t* monitors[MAX_UPDATE_MONITORS];
static size_t nmonitors;

typedef struct {
	pid_t pid;
	char* handle;
	char* sender;
} update_child_t;

static update_child_t update_children[MAX_UPDATE_CHILDREN];
static size_t n_update_children;

static void emit_progress(DBusConnection* conn, const char* handle,
			  const char* sender, dbus_uint32_t status,
			  dbus_uint32_t n_ops, dbus_uint32_t op,
			  dbus_uint32_t progress, const char* error,
			  const char* error_message) {
	DBusMessage* sig =
		dbus_message_new_signal(handle, UPDATE_IFACE, "Progress");
	if (sig == NULL) return;
	if (sender != NULL && *sender != '\0')
		dbus_message_set_destination(sig, sender);

	DBusMessageIter it, arr;
	dbus_message_iter_init_append(sig, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
	dict_add_uint(&arr, "n_ops", n_ops);
	dict_add_uint(&arr, "op", op);
	dict_add_uint(&arr, "progress", progress);
	dict_add_uint(&arr, "status", status);
	if (error != NULL) dict_add_string(&arr, "error", error);
	if (error_message != NULL)
		dict_add_string(&arr, "error_message", error_message);
	dbus_message_iter_close_container(&it, &arr);
	dbus_connection_send(conn, sig, NULL);
	dbus_message_unref(sig);
}

static DBusHandlerResult name_owner_filter(DBusConnection* conn,
					   DBusMessage* msg, void* ud) {
	if (dbus_message_is_signal(msg, DBUS_INTERFACE_DBUS,
				   "NameOwnerChanged")) {
		const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
		if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &name,
					  DBUS_TYPE_STRING, &old_owner,
					  DBUS_TYPE_STRING, &new_owner,
					  DBUS_TYPE_INVALID) &&
		    new_owner != NULL && *new_owner == '\0') {
			for (size_t i = 0; i < nmonitors; i++) {
				if (monitors[i]->sender != NULL &&
				    strcmp(monitors[i]->sender, name) == 0)
					monitors[i]->closed = true;
			}
		}
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void cleanup_monitors(DBusConnection* conn) {
	for (size_t i = 0; i < nmonitors;) {
		if (monitors[i]->closed) {
			update_monitor_t* m = monitors[i];
			monitors[i] = monitors[--nmonitors];
			dbus_connection_unregister_object_path(conn, m->handle);
			free(m->handle);
			free(m->sender);
			free(m->app_id);
			free(m);
			continue;
		}
		i++;
	}
}

static void reap_update_children(DBusConnection* conn) {
	for (size_t i = 0; i < n_update_children;) {
		update_child_t* u = &update_children[i];
		int status = 0;
		pid_t r = waitpid(u->pid, &status, WNOHANG);
		if (r == 0) {
			i++;
			continue;
		}
		if (r < 0 && errno == EINTR) continue;
		if (r == u->pid && WIFEXITED(status) &&
		    WEXITSTATUS(status) == 0) {
			emit_progress(conn, u->handle, u->sender, 2, 1, 0, 100,
				      NULL, NULL);
		} else {
			char msg[80];
			snprintf(msg, sizeof(msg),
				 "flatpkg update failed (wait status %d)",
				 status);
			emit_progress(conn, u->handle, u->sender, 3, 1, 0, 0,
				      "org.freedesktop.portal.Error.Failed",
				      msg);
		}
		free(u->handle);
		free(u->sender);
		update_children[i] = update_children[--n_update_children];
	}
}

static void handle_update(DBusConnection* conn, update_monitor_t* m,
			  DBusMessage* msg) {
	DBusMessage* reply = dbus_message_new_method_return(msg);
	if (reply != NULL) {
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
	}

	if (m->app_id == NULL) {
		emit_progress(conn, m->handle, m->sender, 3, 1, 0, 0,
			      "org.freedesktop.portal.Error.Failed",
			      "could not determine the calling application");
		return;
	}
	if (n_update_children >= MAX_UPDATE_CHILDREN) {
		emit_progress(conn, m->handle, m->sender, 3, 1, 0, 0,
			      "org.freedesktop.portal.Error.Failed",
			      "too many concurrent updates");
		return;
	}
	char bin[PATH_MAX];
	if (flatpkg_bin_path(bin, sizeof(bin)) == NULL) {
		emit_progress(conn, m->handle, m->sender, 3, 1, 0, 0,
			      "org.freedesktop.portal.Error.Failed",
			      "flatpkg binary not found");
		return;
	}
	pid_t pid = fork();
	if (pid < 0) {
		emit_progress(conn, m->handle, m->sender, 3, 1, 0, 0,
			      "org.freedesktop.portal.Error.Failed",
			      strerror(errno));
		return;
	}
	if (pid == 0) {
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO) close(devnull);
		}
		int busfd = -1;
		if (dbus_connection_get_unix_fd(conn, &busfd) && busfd >= 0)
			fcntl(busfd, F_SETFD, FD_CLOEXEC);
		execl(bin, "flatpkg", "update", m->app_id, (char*)NULL);
		_exit(127);
	}
	update_children[n_update_children].pid = pid;
	update_children[n_update_children].handle = strdup(m->handle);
	update_children[n_update_children].sender = strdup(m->sender);
	if (update_children[n_update_children].handle == NULL ||
	    update_children[n_update_children].sender == NULL) {
		kill(pid, SIGTERM);
		free(update_children[n_update_children].handle);
		free(update_children[n_update_children].sender);
		emit_progress(conn, m->handle, m->sender, 3, 1, 0, 0,
			      "org.freedesktop.portal.Error.Failed",
			      "out of memory");
		return;
	}
	n_update_children++;
	emit_progress(conn, m->handle, m->sender, 0, 1, 0, 0, NULL, NULL);
}

static const char monitor_xml[] =
	"<node>\n"
	"  <interface name=\"org.freedesktop.portal.Flatpak.UpdateMonitor\">\n"
	"    <method name=\"Close\"/>\n"
	"    <method name=\"Update\">\n"
	"      <arg type=\"s\" name=\"parent_window\" direction=\"in\"/>\n"
	"      <arg type=\"a{sv}\" name=\"options\" direction=\"in\"/>\n"
	"    </method>\n"
	"    <signal name=\"UpdateAvailable\">\n"
	"      <arg type=\"a{sv}\" name=\"update_info\"/>\n"
	"    </signal>\n"
	"    <signal name=\"Progress\">\n"
	"      <arg type=\"a{sv}\" name=\"info\"/>\n"
	"    </signal>\n"
	"  </interface>\n"
	"</node>\n";

static DBusHandlerResult monitor_message(DBusConnection* conn, DBusMessage* msg,
					 void* ud) {
	update_monitor_t* m = ud;
	if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	const char* iface = dbus_message_get_interface(msg);
	const char* member = dbus_message_get_member(msg);
	if (iface == NULL || member == NULL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (strcmp(iface, "org.freedesktop.DBus.Introspectable") == 0 &&
	    strcmp(member, "Introspect") == 0) {
		DBusMessage* reply = dbus_message_new_method_return(msg);
		if (reply != NULL) {
			const char* x = monitor_xml;
			dbus_message_append_args(reply, DBUS_TYPE_STRING, &x,
						 DBUS_TYPE_INVALID);
			dbus_connection_send(conn, reply, NULL);
			dbus_message_unref(reply);
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (strcmp(iface, "org.freedesktop.DBus.Properties") == 0) {
		if (strcmp(member, "GetAll") == 0) {
			DBusMessage* reply =
				dbus_message_new_method_return(msg);
			if (reply != NULL) {
				DBusMessageIter it, arr;
				dbus_message_iter_init_append(reply, &it);
				dbus_message_iter_open_container(
					&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
				dbus_message_iter_close_container(&it, &arr);
				dbus_connection_send(conn, reply, NULL);
				dbus_message_unref(reply);
			}
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		DBusMessage* err = dbus_message_new_error(
			msg, DBUS_ERROR_UNKNOWN_PROPERTY, "No such property");
		if (err != NULL) {
			dbus_connection_send(conn, err, NULL);
			dbus_message_unref(err);
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (strcmp(iface, UPDATE_IFACE) == 0) {
		if (strcmp(member, "Close") == 0) {
			DBusMessage* reply =
				dbus_message_new_method_return(msg);
			if (reply != NULL) {
				dbus_connection_send(conn, reply, NULL);
				dbus_message_unref(reply);
			}
			m->closed = true;
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		if (strcmp(member, "Update") == 0) {
			handle_update(conn, m, msg);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void handle_create_update_monitor(DBusConnection* conn,
					 DBusMessage* msg) {
	const char* sender = dbus_message_get_sender(msg);
	if (sender == NULL) sender = "";

	char who[128];
	size_t w = 0;
	const char* s = (*sender == ':') ? sender + 1 : sender;
	for (; *s != '\0' && w + 1 < sizeof(who); s++)
		who[w++] = (*s == '.') ? '_' : *s;
	who[w] = '\0';

	static unsigned monitor_token = 0;
	char handle[320];
	snprintf(handle, sizeof(handle),
		 "/org/freedesktop/portal/Flatpak/update_monitor/%s/%u", who,
		 __atomic_fetch_add(&monitor_token, 1, __ATOMIC_SEQ_CST));

	if (nmonitors >= MAX_UPDATE_MONITORS) {
		reply_error(conn, msg, "Too many update monitors");
		return;
	}
	update_monitor_t* m = calloc(1, sizeof(*m));
	if (m == NULL) {
		reply_error(conn, msg, "Out of memory");
		return;
	}
	m->handle = strdup(handle);
	m->sender = strdup(sender);
	m->app_id = resolve_app_id(conn, sender);
	m->closed = false;
	if (m->handle == NULL || m->sender == NULL) {
		reply_error(conn, msg, "Cannot create update monitor");
		free(m->handle);
		free(m->sender);
		free(m->app_id);
		free(m);
		return;
	}
	monitors[nmonitors++] = m;

	DBusMessage* reply = dbus_message_new_method_return(msg);
	if (reply != NULL) {
		const char* h = m->handle;
		dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &h,
					 DBUS_TYPE_INVALID);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
	}
}

static void reply_property_get(DBusConnection* conn, DBusMessage* msg,
			       const char* prop) {
	dbus_uint32_t v;

	if (strcmp(prop, "version") == 0) v = PORTAL_VERSION;
	else if (strcmp(prop, "supports") == 0)
		v = 1; // FreeBSD pids are global, so expose/share-pids work
		       // without namespaces.
	else {
		reply_error(conn, msg, "No such property");
		return;
	}
	DBusMessage* reply = dbus_message_new_method_return(msg);
	if (!reply) return;
	DBusMessageIter it, sub;
	dbus_message_iter_init_append(reply, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "u", &sub);
	dbus_message_iter_append_basic(&sub, DBUS_TYPE_UINT32, &v);
	dbus_message_iter_close_container(&it, &sub);
	dbus_connection_send(conn, reply, NULL);
	dbus_message_unref(reply);
}

static void reply_property_get_all(DBusConnection* conn, DBusMessage* msg) {
	DBusMessage* reply = dbus_message_new_method_return(msg);
	if (!reply) return;
	DBusMessageIter it, arr;
	dbus_message_iter_init_append(reply, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);

	static const struct {
		const char* name;
		dbus_uint32_t value;
	} props[] = {
		{"version", PORTAL_VERSION},
		{"supports", 1},
	};
	for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); i++)
		dict_add_uint(&arr, props[i].name, props[i].value);
	dbus_message_iter_close_container(&it, &arr);
	dbus_connection_send(conn, reply, NULL);
	dbus_message_unref(reply);
}

static void reply_settings_version(DBusConnection* conn, DBusMessage* msg) {
	DBusMessage* reply = dbus_message_new_method_return(msg);
	DBusMessageIter it, arr;

	if (!reply) return;
	dbus_message_iter_init_append(reply, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
	dict_add_uint(&arr, "version", 2);
	dbus_message_iter_close_container(&it, &arr);
	dbus_connection_send(conn, reply, NULL);
	dbus_message_unref(reply);
}

static void reply_introspect(DBusConnection* conn, DBusMessage* msg) {
	static const char xml[] =
		"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object "
		"Introspection 1.0//EN\"\n"
		"\"http://www.freedesktop.org/standards/dbus/1.0/"
		"introspect.dtd\">\n"
		"<node>\n"
		"  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
		"    <method name=\"Introspect\">\n"
		"      <arg name=\"xml_data\" type=\"s\" direction=\"out\"/>\n"
		"    </method>\n"
		"  </interface>\n"
		"  <interface name=\"org.freedesktop.DBus.Properties\">\n"
		"    <method name=\"Get\">\n"
		"      <arg name=\"interface_name\" type=\"s\" "
		"direction=\"in\"/>\n"
		"      <arg name=\"property_name\" type=\"s\" "
		"direction=\"in\"/>\n"
		"      <arg name=\"value\" type=\"v\" direction=\"out\"/>\n"
		"    </method>\n"
		"    <method name=\"GetAll\">\n"
		"      <arg name=\"interface_name\" type=\"s\" "
		"direction=\"in\"/>\n"
		"      <arg name=\"properties\" type=\"a{sv}\" "
		"direction=\"out\"/>\n"
		"    </method>\n"
		"  </interface>\n"
		"  <interface name=\"" PORTAL_IFACE "\">\n"
		"    <property name=\"version\" type=\"u\" access=\"read\"/>\n"
		"    <property name=\"supports\" type=\"u\" access=\"read\"/>\n"
		"    <method name=\"Spawn\">\n"
		"      <arg type=\"ay\" name=\"cwd_path\" direction=\"in\"/>\n"
		"      <arg type=\"aay\" name=\"argv\" direction=\"in\"/>\n"
		"      <arg type=\"a{uh}\" name=\"fds\" direction=\"in\"/>\n"
		"      <arg type=\"a{ss}\" name=\"envs\" direction=\"in\"/>\n"
		"      <arg type=\"u\" name=\"flags\" direction=\"in\"/>\n"
		"      <arg type=\"a{sv}\" name=\"options\" "
		"direction=\"in\"/>\n"
		"      <arg type=\"u\" name=\"pid\" direction=\"out\"/>\n"
		"    </method>\n"
		"    <method name=\"SpawnSignal\">\n"
		"      <arg type=\"u\" name=\"pid\" direction=\"in\"/>\n"
		"      <arg type=\"u\" name=\"signal\" direction=\"in\"/>\n"
		"      <arg type=\"b\" name=\"to_process_group\" "
		"direction=\"in\"/>\n"
		"    </method>\n"
		"    <signal name=\"SpawnStarted\">\n"
		"      <arg type=\"u\" name=\"pid\"/>\n"
		"      <arg type=\"u\" name=\"relpid\"/>\n"
		"    </signal>\n"
		"    <signal name=\"SpawnExited\">\n"
		"      <arg type=\"u\" name=\"pid\"/>\n"
		"      <arg type=\"u\" name=\"exit_status\"/>\n"
		"    </signal>\n"
		"    <method name=\"CreateUpdateMonitor\">\n"
		"      <arg type=\"a{sv}\" name=\"options\" "
		"direction=\"in\"/>\n"
		"      <arg type=\"o\" name=\"handle\" direction=\"out\"/>\n"
		"    </method>\n"
		"  </interface>\n"
		"</node>\n";

	DBusMessage* reply = dbus_message_new_method_return(msg);
	if (!reply) return;
	const char* x = xml;
	dbus_message_append_args(reply, DBUS_TYPE_STRING, &x,
				 DBUS_TYPE_INVALID);
	dbus_connection_send(conn, reply, NULL);
	dbus_message_unref(reply);
}

static DBusHandlerResult handle_message(DBusConnection* conn, DBusMessage* msg,
					void* ud) {
	if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	const char* iface = dbus_message_get_interface(msg);
	const char* member = dbus_message_get_member(msg);
	if (!iface || !member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	const char* path = dbus_message_get_path(msg);
	if (path != NULL && strncmp(path, UPDATE_MONITOR_PREFIX,
				    strlen(UPDATE_MONITOR_PREFIX)) == 0) {
		for (size_t i = 0; i < nmonitors; i++)
			if (monitors[i]->handle != NULL &&
			    strcmp(monitors[i]->handle, path) == 0)
				return monitor_message(conn, msg, monitors[i]);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (strcmp(iface, "org.freedesktop.DBus.Introspectable") == 0 &&
	    strcmp(member, "Introspect") == 0) {
		reply_introspect(conn, msg);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (strcmp(iface, "org.freedesktop.DBus.Properties") == 0) {
		if (strcmp(member, "GetAll") == 0) {
			DBusError gerr;
			const char* target = NULL;

			dbus_error_init(&gerr);
			if (dbus_message_get_args(msg, &gerr, DBUS_TYPE_STRING,
						  &target, DBUS_TYPE_INVALID) &&
			    strcmp(target, SETTINGS_IFACE) == 0) {
				reply_settings_version(conn, msg);
				dbus_error_free(&gerr);
				return DBUS_HANDLER_RESULT_HANDLED;
			}
			dbus_error_free(&gerr);
		}
		if (strcmp(member, "Get") == 0) {
			const char *in_iface = NULL, *prop = NULL;
			if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING,
						  &in_iface, DBUS_TYPE_STRING,
						  &prop, DBUS_TYPE_INVALID)) {
				reply_property_get(conn, msg, prop);
			}
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		if (strcmp(member, "GetAll") == 0) {
			reply_property_get_all(conn, msg);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	if (strcmp(iface, PORTAL_IFACE) == 0) {
		if (strcmp(member, "Spawn") == 0) {
			handle_spawn(conn, msg);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		if (strcmp(member, "SpawnSignal") == 0) {
			handle_spawn_signal(conn, msg);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		if (strcmp(member, "CreateUpdateMonitor") == 0) {
			handle_create_update_monitor(conn, msg);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

typedef struct {
	pid_t pid;
	char* path;
	char* sender;
	char* cwd;
	bool multiple;
	bool save;
	int fd;
	time_t deadline;
} fc_request_t;

static fc_request_t* fc_requests;
static size_t fc_nrequests;

static long fc_timeout = 600;

static fc_request_t* fc_track(pid_t pid, const char* path, const char* sender,
			      const char* cwd, bool multiple, bool save) {
	fc_request_t* r = realloc(fc_requests, (fc_nrequests + 1) * sizeof(*r));
	if (!r) return NULL;
	fc_requests = r;
	fc_request_t* e = &fc_requests[fc_nrequests++];
	memset(e, 0, sizeof(*e));
	e->pid = pid;
	e->path = xstrdup(path ? path : "");
	e->sender = xstrdup(sender ? sender : "");
	e->cwd = cwd ? xstrdup(cwd) : NULL;
	e->multiple = multiple;
	e->save = save;
	e->fd = -1;
	e->deadline = fc_timeout ? time(NULL) + fc_timeout : 0;
	return e;
}

static void fc_parse_filters(DBusMessageIter* v, char*** filters,
			     size_t* nfilters) {
	DBusMessageIter arr;

	dbus_message_iter_recurse(v, &arr);
	while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) {
		DBusMessageIter name, rules;
		const char* nm = NULL;
		char* acc = NULL;
		size_t alen = 0;

		dbus_message_iter_recurse(&arr, &name);
		dbus_message_iter_recurse(&arr, &rules);
		dbus_message_iter_get_basic(&name, &nm);
		while (dbus_message_iter_get_arg_type(&rules) ==
		       DBUS_TYPE_STRUCT) {
			DBusMessageIter t, pat;
			const char* patstr = NULL;

			dbus_message_iter_recurse(&rules, &t);
			dbus_message_iter_recurse(&rules, &pat);
			dbus_message_iter_get_basic(&pat, &patstr);
			if (patstr) {
				size_t pl = strlen(patstr);
				char* grown = realloc(acc, alen + pl + 2);
				if (!grown) break;
				acc = grown;
				if (alen) acc[alen++] = ' ';
				memcpy(acc + alen, patstr, pl + 1);
				alen += pl;
			}
			dbus_message_iter_next(&rules);
		}
		size_t need = (nm ? strlen(nm) : 0) + alen + 8;
		char* spec = malloc(need);
		if (spec) {
			snprintf(spec, need, "%s | %s", nm ? nm : "",
				 acc ? acc : "*");
			char** g = realloc(*filters,
					   (*nfilters + 1) * sizeof(char*));
			if (g) {
				*filters = g;
				(*filters)[(*nfilters)++] = spec;
			} else {
				free(spec);
			}
		}
		free(acc);
		dbus_message_iter_next(&arr);
	}
}

static bool fc_parse_options(DBusMessageIter* opts, char** title_out,
			     bool* multiple, bool* dir_select, char** folder,
			     char*** filters, size_t* nfilters) {
	DBusMessageIter entry;

	*title_out = NULL;
	*multiple = false;
	*dir_select = false;
	*folder = NULL;
	*filters = NULL;
	*nfilters = 0;

	dbus_message_iter_recurse(opts, &entry);
	while (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter v;
		const char* key = NULL;

		if (!dict_lookup(&entry, &key, &v)) {
			dbus_message_iter_next(&entry);
			continue;
		}
		if (strcmp(key, "multiple") == 0)
			dbus_message_iter_get_basic(&v, multiple);
		else if (strcmp(key, "directory_select") == 0)
			dbus_message_iter_get_basic(&v, dir_select);
		else if (strcmp(key, "current_folder") == 0)
			dbus_message_iter_get_basic(&v, folder);
		else if (strcmp(key, "filters") == 0)
			fc_parse_filters(&v, filters, nfilters);
		dbus_message_iter_next(&entry);
	}
	return true;
}

static void fc_free_request(fc_request_t* r) {
	free(r->path);
	free(r->sender);
	free(r->cwd);
}

static char* fc_path_to_uri(const char* path) {
	size_t n = strlen(path);
	char* u = malloc(n * 3 + 10);
	if (!u) return NULL;
	strlcpy(u, "file://", n * 3 + 10);
	char* o = u + 7;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)path[i];
		if (isalnum(c) || strchr("/-_.~!$&'()*+,;=:@", c)) {
			*o++ = (char)c;
		} else {
			snprintf(o, 4, "%%%02X", c);
			o += 3;
		}
	}
	*o = '\0';
	return u;
}

static char* fc_basename(const char* path) {
	const char* slash = strrchr(path, '/');
	return xstrdup(slash ? slash + 1 : path);
}

static void fc_emit_response(DBusConnection* conn, const char* path,
			     uint32_t response) {
	DBusMessage* sig =
		dbus_message_new_signal(path, FILECHOOSER_IFACE, "Response");
	if (!sig) return;
	dbus_uint32_t r = (dbus_uint32_t)response;
	dbus_message_append_args(sig, DBUS_TYPE_UINT32, &r, DBUS_TYPE_INVALID);
	DBusMessageIter va, arr;
	dbus_message_iter_init_append(sig, &va);
	if (dbus_message_iter_open_container(&va, DBUS_TYPE_VARIANT, "a{sv}",
					     &arr))
		dbus_message_iter_close_container(&va, &arr);
	dbus_connection_send(conn, sig, NULL);
	dbus_message_unref(sig);
}

static void fc_emit_results(DBusConnection* conn, const char* path,
			    const char* chosen, bool multiple) {
	DBusMessage* sig =
		dbus_message_new_signal(path, FILECHOOSER_IFACE, "Response");
	if (!sig) return;
	dbus_uint32_t ok = (dbus_uint32_t)FC_RESPONSE_SUCCESS;
	dbus_message_append_args(sig, DBUS_TYPE_UINT32, &ok, DBUS_TYPE_INVALID);

	DBusMessageIter va, arr, item;
	dbus_message_iter_init_append(sig, &va);
	const char* sig_name = multiple ? "a(ss)" : "(ss)";
	if (dbus_message_iter_open_container(&va, DBUS_TYPE_VARIANT, sig_name,
					     &arr)) {
		if (multiple) {
			char* copy = xstrdup(chosen);
			for (char* line = strtok(copy, "\n"); line;
			     line = strtok(NULL, "\n")) {
				if (!*line) continue;
				char* uri = fc_path_to_uri(line);
				char* base = fc_basename(line);
				dbus_message_iter_open_container(
					&arr, DBUS_TYPE_STRUCT, NULL, &item);
				dbus_message_iter_append_basic(&item,
							       DBUS_TYPE_STRING,
							       uri ? uri : "");
				dbus_message_iter_append_basic(
					&item, DBUS_TYPE_STRING,
					base ? base : "");
				dbus_message_iter_close_container(&arr, &item);
				free(uri);
				free(base);
			}
			free(copy);
		} else {
			char* uri = fc_path_to_uri(chosen);
			char* base = fc_basename(chosen);
			dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT,
							 NULL, &item);
			dbus_message_iter_append_basic(&item, DBUS_TYPE_STRING,
						       uri ? uri : "");
			dbus_message_iter_append_basic(&item, DBUS_TYPE_STRING,
						       base ? base : "");
			dbus_message_iter_close_container(&arr, &item);
			free(uri);
			free(base);
		}
		dbus_message_iter_close_container(&va, &arr);
	}
	dbus_connection_send(conn, sig, NULL);
	dbus_message_unref(sig);
}

static void fc_reap(DBusConnection* conn) {
	for (size_t i = 0; i < fc_nrequests;) {
		int st = 0;
		pid_t p = fc_requests[i].pid;
		if (waitpid(p, &st, WNOHANG) != p) {
			if (fc_requests[i].deadline &&
			    time(NULL) > fc_requests[i].deadline) {
				kill(p, SIGKILL);
				waitpid(p, &st, 0);
				if (fc_requests[i].fd >= 0)
					close(fc_requests[i].fd);
				fpkg_warnf(
					"file chooser did not answer"
					" within %lds; answering with an error",
					(long)fc_timeout);
				fc_emit_response(conn, fc_requests[i].path,
						 FC_RESPONSE_OTHER);
				fc_free_request(&fc_requests[i]);
				memmove(&fc_requests[i], &fc_requests[i + 1],
					(fc_nrequests - i - 1) *
						sizeof(*fc_requests));
				fc_nrequests--;
				continue;
			}
			i++;
			continue;
		}
		char buf[8192];
		ssize_t n = 0;
		int fd = fc_requests[i].fd;
		if (fd >= 0) {
			n = read(fd, buf, sizeof(buf) - 1);
			close(fd);
			fc_requests[i].fd = -1;
		}
		if (n > 0) buf[n] = '\0';
		else
			buf[0] = '\0';

		if (WIFEXITED(st) && WEXITSTATUS(st) == 0 && buf[0])
			fc_emit_results(conn, fc_requests[i].path, buf,
					fc_requests[i].multiple);
		else if (WIFEXITED(st) && WEXITSTATUS(st) == 1)
			fc_emit_response(conn, fc_requests[i].path,
					 FC_RESPONSE_CANCELLED);
		else
			fc_emit_response(conn, fc_requests[i].path,
					 FC_RESPONSE_OTHER);

		fc_free_request(&fc_requests[i]);
		memmove(&fc_requests[i], &fc_requests[i + 1],
			(fc_nrequests - i - 1) * sizeof(*fc_requests));
		fc_nrequests--;
	}
}

static void handle_file_chooser(DBusConnection* conn, DBusMessage* msg,
				bool save, bool save_files) {
	DBusMessageIter it;
	const char *parent = NULL, *title = NULL;
	if (!dbus_message_iter_init(msg, &it)) {
		reply_error(conn, msg, "cannot read the request");
		return;
	}
	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_OBJECT_PATH) {
		reply_error(conn, msg, "bad parent_window");
		return;
	}
	dbus_message_iter_get_basic(&it, &parent);
	dbus_message_iter_next(&it);
	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) {
		reply_error(conn, msg, "bad title");
		return;
	}
	dbus_message_iter_get_basic(&it, &title);
	if (!title) title = "";
	dbus_message_iter_next(&it);
	DBusMessageIter opts;
	dbus_message_iter_next(&it);
	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) {
		reply_error(conn, msg, "bad options");
		return;
	}

	char *own_title = NULL, *folder = NULL, **filters = NULL;
	size_t nfilters = 0;
	bool multiple = false, dir_select = false;
	dbus_message_iter_recurse(&it, &opts);
	fc_parse_options(&opts, &own_title, &multiple, &dir_select, &folder,
			 &filters, &nfilters);
	if (save_files) dir_select = true;

	const char* sender = dbus_message_get_sender(msg);
	if (!sender) sender = "";
	static unsigned long token;
	char path[512];
	snprintf(path, sizeof(path), "%s/request/%s/%lu", DESKTOP_PATH,
		 (*sender == ':') ? sender + 1 : sender, token++);

	if (access("/usr/local/bin/zenity", X_OK) != 0) {
		fpkg_warnf("no file chooser backend "
			   "(/usr/local/bin/zenity is not executable) - "
			   "FileChooser will answer with an error rather than "
			   "open a dialog");
	}

	int pipefd[2];
	if (pipe(pipefd) != 0) {
		free(own_title);
		free(folder);
		for (size_t i = 0; i < nfilters; i++) free(filters[i]);
		free(filters);
		reply_error(conn, msg, "cannot create a pipe");
		return;
	}

	pid_t pid = fork();
	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[1]);
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDERR_FILENO);
			close(devnull);
		}
		{
			const char* xrd = cli_getenv("XDG_RUNTIME_DIR");
			char* wl = fp_wayland_display_name(xrd);
			if (!cli_getenv("WAYLAND_DISPLAY") && wl)
				setenv("WAYLAND_DISPLAY", wl, 1);
			free(wl);
			if (!cli_getenv("DISPLAY") &&
			    access("/tmp/.X11-unix/X0", F_OK) == 0)
				setenv("DISPLAY", ":0", 1);
		}
		if (folder && *folder)
			if (chdir(folder) != 0) {
			}
		const char* argv[24];
		int a = 0;
		argv[a++] = "/usr/local/bin/zenity";
		argv[a++] = "--file-selection";
		if (save || save_files) argv[a++] = "--save";
		if (save_files) argv[a++] = "--directory";
		if (dir_select) argv[a++] = "--directory";
		if (multiple) argv[a++] = "--multiple";
		if (title && *title) {
			argv[a++] = "--title";
			argv[a++] = title;
		} else if (own_title && *own_title) {
			argv[a++] = "--title";
			argv[a++] = own_title;
		}
		for (size_t i = 0; i < nfilters && a < 20; i++) {
			argv[a++] = "--file-filter";
			argv[a++] = filters[i];
		}
		argv[a] = NULL;
		execv("/usr/local/bin/zenity", (char* const*)argv);
		_exit(127);
	}
	close(pipefd[1]);
	fc_track(pid, path, sender, folder, multiple, save);
	fc_requests[fc_nrequests - 1].fd = pipefd[0];

	DBusMessage* reply = dbus_message_new_method_return(msg);
	if (reply) {
		const char* p = path;
		dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &p,
					 DBUS_TYPE_INVALID);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
	}

	free(own_title);
	free(folder);
	for (size_t i = 0; i < nfilters; i++) free(filters[i]);
	free(filters);
}

static void handle_open_file(DBusConnection* conn, DBusMessage* msg) {
	handle_file_chooser(conn, msg, false, false);
}

static void handle_save_file(DBusConnection* conn, DBusMessage* msg) {
	handle_file_chooser(conn, msg, true, false);
}

static void handle_save_files(DBusConnection* conn, DBusMessage* msg) {
	handle_file_chooser(conn, msg, true, true);
}

// A shim that owns the name must serve Settings: otherwise libadwaita gets
// UnknownMethod and every GTK4 app renders light on a dark host.

static const char* settings_dbus_sig(GVariant* v) {
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) return "s";
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) return "b";
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT32)) return "i";
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32)) return "u";
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE)) return "d";
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING_ARRAY)) return "as";
	return NULL;
}

static bool settings_append_value(GVariant* v, DBusMessageIter* it) {
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
		const char* s = g_variant_get_string(v, NULL);
		return dbus_message_iter_append_basic(it, DBUS_TYPE_STRING, &s);
	}
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) {
		dbus_bool_t b = g_variant_get_boolean(v) ? TRUE : FALSE;
		return dbus_message_iter_append_basic(it, DBUS_TYPE_BOOLEAN,
						      &b);
	}
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT32)) {
		dbus_int32_t i = g_variant_get_int32(v);
		return dbus_message_iter_append_basic(it, DBUS_TYPE_INT32, &i);
	}
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32)) {
		dbus_uint32_t u = g_variant_get_uint32(v);
		return dbus_message_iter_append_basic(it, DBUS_TYPE_UINT32, &u);
	}
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE)) {
		double d = g_variant_get_double(v);
		return dbus_message_iter_append_basic(it, DBUS_TYPE_DOUBLE, &d);
	}
	if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING_ARRAY)) {
		DBusMessageIter arr;
		gsize n = 0, k;
		gchar** strv;

		if (!dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "s",
						      &arr))
			return false;
		strv = g_variant_dup_strv(v, &n);
		for (k = 0; k < n; k++) {
			const char* s = strv[k];
			if (!dbus_message_iter_append_basic(
				    &arr, DBUS_TYPE_STRING, &s)) {
				g_strfreev(strv);
				return false;
			}
		}
		g_strfreev(strv);
		return dbus_message_iter_close_container(it, &arr);
	}
	return false;
}

static bool settings_append_variant(GVariant* v, DBusMessageIter* it) {
	const char* sig = settings_dbus_sig(v);
	DBusMessageIter var;

	if (!sig) return false;
	if (!dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, sig, &var))
		return false;
	if (!settings_append_value(v, &var)) return false;
	return dbus_message_iter_close_container(it, &var);
}

static bool dict_add_settings(DBusMessageIter* arr, const char* key,
			      GVariant* v) {
	DBusMessageIter de, var;
	const char* sig = settings_dbus_sig(v);

	if (!sig) return false;
	if (!dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL,
					      &de))
		return false;
	if (!dbus_message_iter_append_basic(&de, DBUS_TYPE_STRING, &key) ||
	    !dbus_message_iter_open_container(&de, DBUS_TYPE_VARIANT, sig,
					      &var))
		return false;
	if (!settings_append_value(v, &var)) return false;
	dbus_message_iter_close_container(&de, &var);
	return dbus_message_iter_close_container(arr, &de);
}

static void dict_open_subdict(DBusMessageIter* dict, const char* key,
			      DBusMessageIter* de, DBusMessageIter* inner) {
	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, de);
	dbus_message_iter_append_basic(de, DBUS_TYPE_STRING, &key);
	dbus_message_iter_open_container(de, DBUS_TYPE_ARRAY, "{sv}", inner);
}

static GVariant* appearance_value(const char* key) {
	GSettingsSchemaSource* src = g_settings_schema_source_get_default();

	if (strcmp(key, "color-scheme") == 0) {
		GSettingsSchema* sch;
		GSettings* set;
		gchar* str;
		guint32 u = 0;

		sch = src ? g_settings_schema_source_lookup(
				    src, "org.gnome.desktop.interface", TRUE)
			  : NULL;
		if (!sch || !g_settings_schema_has_key(sch, "color-scheme")) {
			if (sch) g_settings_schema_unref(sch);
			return NULL;
		}
		set = g_settings_new("org.gnome.desktop.interface");
		str = g_settings_get_string(set, "color-scheme");
		if (str && strcmp(str, "prefer-dark") == 0) u = 1;
		else if (str && strcmp(str, "prefer-light") == 0)
			u = 2;
		g_free(str);
		g_object_unref(set);
		g_settings_schema_unref(sch);
		return g_variant_new_uint32(u);
	}
	if (strcmp(key, "contrast") == 0) {
		GSettingsSchema* sch;
		GSettings* set;
		gboolean b = FALSE;

		sch = src ? g_settings_schema_source_lookup(
				    src, "org.gnome.desktop.a11y.interface",
				    TRUE)
			  : NULL;
		if (sch && g_settings_schema_has_key(sch, "high-contrast")) {
			set = g_settings_new(
				"org.gnome.desktop.a11y.interface");
			b = g_settings_get_boolean(set, "high-contrast");
			g_object_unref(set);
		}
		if (sch) g_settings_schema_unref(sch);
		return g_variant_new_uint32(b ? 1 : 0);
	}
	return NULL;
}

static void handle_settings_read(DBusConnection* conn, DBusMessage* msg,
				 bool all) {
	DBusError derr;
	const char *ns = NULL, *key = NULL;
	GSettingsSchemaSource* src;
	GSettingsSchema* schema;
	GSettings* settings;
	GVariant* v;
	DBusMessage* reply;
	DBusMessageIter it;

	src = g_settings_schema_source_get_default();

	dbus_error_init(&derr);
	if (all) {
		char** namespaces = NULL;
		DBusMessageIter dict;
		int nns = 0, i;

		if (!dbus_message_get_args(msg, &derr, DBUS_TYPE_ARRAY,
					   DBUS_TYPE_STRING, &namespaces, &nns,
					   DBUS_TYPE_INVALID)) {
			reply_error(conn, msg, "ReadAll wants (as)");
			dbus_error_free(&derr);
			return;
		}
		dbus_error_free(&derr);

		reply = dbus_message_new_method_return(msg);
		if (!reply) return;
		dbus_message_iter_init_append(reply, &it);
		if (!dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY,
						      "{sa{sv}}", &dict)) {
			dbus_message_unref(reply);
			return;
		}
		for (i = 0; i < nns; i++) {
			DBusMessageIter e1, inner;
			GSettingsSchema* sch;
			GSettings* set;
			gchar** ks;
			gsize nk = 0, m;

			if (strcmp(namespaces[i],
				   "org.freedesktop.appearance") == 0) {
				static const char* akeys[] = {"color-scheme",
							      "contrast", NULL};
				int ak;

				dict_open_subdict(&dict, namespaces[i], &e1,
						  &inner);
				for (ak = 0; akeys[ak]; ak++) {
					GVariant* av =
						appearance_value(akeys[ak]);

					if (!av) continue;
					dict_add_settings(&inner, akeys[ak], av);
					g_variant_unref(av);
				}
				dbus_message_iter_close_container(&e1, &inner);
				dbus_message_iter_close_container(&dict, &e1);
				continue;
			}

			sch = src ? g_settings_schema_source_lookup(
					    src, namespaces[i], TRUE)
				  : NULL;
			if (!sch) continue;
			set = g_settings_new(namespaces[i]);
			ks = g_settings_schema_list_keys(sch);
			while (ks && ks[nk]) nk++;

			dict_open_subdict(&dict, namespaces[i], &e1, &inner);
			for (m = 0; m < nk; m++) {
				const char* kname = ks[m];
				GVariant* val;

				val = g_settings_get_value(set, kname);
				if (settings_dbus_sig(val))
					dict_add_settings(&inner, kname, val);
				g_variant_unref(val);
			}
			dbus_message_iter_close_container(&e1, &inner);
			dbus_message_iter_close_container(&dict, &e1);
			g_strfreev(ks);
			g_object_unref(set);
			g_settings_schema_unref(sch);
		}
		dbus_message_iter_close_container(&it, &dict);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return;
	} else if (!dbus_message_get_args(msg, &derr, DBUS_TYPE_STRING, &ns,
					  DBUS_TYPE_STRING, &key,
					  DBUS_TYPE_INVALID)) {
		reply_error(conn, msg, "ReadOne wants (ss)");
		dbus_error_free(&derr);
		return;
	}
	dbus_error_free(&derr);

	if (strcmp(ns, "org.freedesktop.appearance") == 0) {
		GVariant* av = appearance_value(key);

		if (!av) {
			reply_error(conn, msg, "no such appearance key");
			return;
		}
		reply = dbus_message_new_method_return(msg);
		if (!reply) {
			g_variant_unref(av);
			return;
		}
		dbus_message_iter_init_append(reply, &it);
		settings_append_variant(av, &it);
		g_variant_unref(av);
		dbus_connection_send(conn, reply, NULL);
		dbus_message_unref(reply);
		return;
	}

	schema = src ? g_settings_schema_source_lookup(src, ns, TRUE) : NULL;
	if (!schema) {
		reply_error(conn, msg, "no such settings schema");
		return;
	}
	if (!g_settings_schema_has_key(schema, key)) {
		g_settings_schema_unref(schema);
		reply_error(conn, msg, "no such settings key");
		return;
	}

	reply = dbus_message_new_method_return(msg);
	if (!reply) {
		g_settings_schema_unref(schema);
		return;
	}
	dbus_message_iter_init_append(reply, &it);

	settings = g_settings_new(ns);
	v = g_settings_get_value(settings, key);
	if (!settings_append_variant(v, &it)) {
		g_variant_unref(v);
		g_object_unref(settings);
		g_settings_schema_unref(schema);
		dbus_message_unref(reply);
		reply_error(conn, msg,
			    "settings value has a type this"
			    " portal does not marshal");
		return;
	}
	g_variant_unref(v);
	g_object_unref(settings);

	g_settings_schema_unref(schema);
	dbus_connection_send(conn, reply, NULL);
	dbus_message_unref(reply);
}

static void handle_settings_message(DBusConnection* conn, DBusMessage* msg,
				    const char* member) {
	if (strcmp(member, "ReadOne") == 0 || strcmp(member, "Read") == 0) {
		handle_settings_read(conn, msg, false);
		return;
	}
	if (strcmp(member, "ReadAll") == 0) {
		handle_settings_read(conn, msg, true);
		return;
	}
	reply_error(conn, msg, "unsupported Settings method");
}

static DBusHandlerResult handle_desktop_message(DBusConnection* conn,
						DBusMessage* msg, void* data) {
	const char* iface = dbus_message_get_interface(msg);
	const char* member = dbus_message_get_member(msg);
	if (!iface || !member) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	// Properties.Get is implemented by every conforming D-Bus object, so
	// refusing it makes the owner look like it is not one.
	if (strcmp(iface, "org.freedesktop.DBus.Properties") == 0) {
		if (strcmp(member, "GetAll") == 0) {
			DBusError gerr;
			const char* target = NULL;

			dbus_error_init(&gerr);
			if (dbus_message_get_args(msg, &gerr, DBUS_TYPE_STRING,
						  &target, DBUS_TYPE_INVALID) &&
			    strcmp(target, SETTINGS_IFACE) == 0) {
				reply_settings_version(conn, msg);
				dbus_error_free(&gerr);
				return DBUS_HANDLER_RESULT_HANDLED;
			}
			dbus_error_free(&gerr);
		}
		if (strcmp(member, "Get") == 0) {
			DBusError perr;
			const char *target = NULL, *prop = NULL;

			dbus_error_init(&perr);
			if (dbus_message_get_args(msg, &perr, DBUS_TYPE_STRING,
						  &target, DBUS_TYPE_STRING,
						  &prop, DBUS_TYPE_INVALID) &&
			    strcmp(target, SETTINGS_IFACE) == 0 &&
			    strcmp(prop, "version") == 0) {
				DBusMessage* prep =
					dbus_message_new_method_return(msg);
				if (prep) {
					DBusMessageIter pit, pvar;
					dbus_uint32_t ver = 2;

					dbus_message_iter_init_append(prep,
								      &pit);
					dbus_message_iter_open_container(
						&pit, DBUS_TYPE_VARIANT, "u",
						&pvar);
					dbus_message_iter_append_basic(
						&pvar, DBUS_TYPE_UINT32, &ver);
					dbus_message_iter_close_container(
						&pit, &pvar);
					dbus_connection_send(conn, prep, NULL);
					dbus_message_unref(prep);
				}
				dbus_error_free(&perr);
				return DBUS_HANDLER_RESULT_HANDLED;
			}
			dbus_error_free(&perr);
		}
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	if (strcmp(iface, SETTINGS_IFACE) == 0) {
		if (strcmp(member, "ReadAll") == 0) {
			handle_settings_message(conn, msg, member);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		reply_error(conn, msg, "unsupported Settings method");
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (strcmp(iface, FILECHOOSER_IFACE) != 0)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if (strcmp(member, "OpenFile") == 0) {
		handle_open_file(conn, msg);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (strcmp(member, "SaveFile") == 0) {
		handle_save_file(conn, msg);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (strcmp(member, "SaveFiles") == 0) {
		handle_save_files(conn, msg);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static const DBusObjectPathVTable portal_vtable = {
	.unregister_function = NULL,
	.message_function = handle_message,
};

static bool fc_own_desktop_name;

static const DBusObjectPathVTable desktop_vtable = {
	.unregister_function = NULL,
	.message_function = handle_desktop_message,
};

// Remove leaked instance dirs: a SIGKILLed worker skips instance_unregister(),
// and the Spawn poll would chase the orphan forever. Stale = no flock, no pid.
static void sweep_stale_instances(void) {
	char base[PATH_MAX];
	instance_base(base, sizeof(base));
	time_t now = time(NULL);
	DIR* d = opendir(base);
	if (!d) return;
	struct dirent* de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.' || !de->d_name[0]) continue;
		bool numeric = true;
		for (const char* p = de->d_name; *p; p++)
			if (*p < '0' || *p > '9') {
				numeric = false;
				break;
			}
		if (!numeric) continue;
		char dir[PATH_MAX], ref[PATH_MAX], pidf[PATH_MAX];
		snprintf(dir, sizeof(dir), "%s/%s", base, de->d_name);
		struct stat st;
		if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
		if (now - st.st_mtime < 60) continue;
		snprintf(ref, sizeof(ref), "%s/.ref", dir);
		int rfd = open(ref, O_RDONLY);
		if (rfd >= 0) {
			bool locked = flock(rfd, LOCK_EX | LOCK_NB) != 0;
			if (!locked) flock(rfd, LOCK_UN);
			close(rfd);
			if (locked) continue;
		}
		snprintf(pidf, sizeof(pidf), "%s/pid", dir);
		FILE* pf = fopen(pidf, "r");
		long p = 0;
		bool have = pf && fscanf(pf, "%ld", &p) == 1 && p > 0;
		if (pf) fclose(pf);
		if (have && kill((pid_t)p, 0) == 0) continue;
		DIR* id = opendir(dir);
		if (id) {
			struct dirent* e;
			while ((e = readdir(id)) != NULL) {
				if (strcmp(e->d_name, ".") == 0 ||
				    strcmp(e->d_name, "..") == 0)
					continue;
				char f[PATH_MAX];
				snprintf(f, sizeof(f), "%s/%s", dir, e->d_name);
				unlink(f);
			}
			closedir(id);
		}
		if (rmdir(dir) == 0)
			fpkg_infof("removed stale instance %s", de->d_name);
	}
	closedir(d);
}

int portal_main(int argc, char* argv[]) {
	if (argv[0] && argv[0][0])
		snprintf(portal_argv0, sizeof(portal_argv0), "%s", argv[0]);

	DBusError err;
	dbus_error_init(&err);
	DBusConnection* conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
	if (!conn) {
		fpkg_errorf("cannot connect to the session bus: %s",
			    err.message ? err.message : "unknown error");
		dbus_error_free(&err);
		return 1;
	}

	int rv = dbus_bus_request_name(conn, PORTAL_IFACE,
				       DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
	if (rv != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		fpkg_errorf("cannot own %s: %s", PORTAL_IFACE,
			    err.message ? err.message : "name taken");
		dbus_error_free(&err);
		return 1;
	}

	if (!dbus_connection_register_fallback(conn, PORTAL_PATH,
					       &portal_vtable, NULL)) {
		fpkg_errorf("cannot register %s", PORTAL_PATH);
		return 1;
	}

	{
		DBusError derr;
		dbus_error_init(&derr);
		int drv = dbus_bus_request_name(
			conn, "org.freedesktop.portal.Desktop",
			DBUS_NAME_FLAG_DO_NOT_QUEUE, &derr);
		if (drv == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
			fc_own_desktop_name = true;
		else
			fpkg_warnf(
				"not owning org.freedesktop.portal.Desktop"
				" (%s) - FileChooser calls will go to whatever"
				" does own it",
				derr.message ? derr.message : "name taken");
		dbus_error_free(&derr);
	}
	if (fc_own_desktop_name &&
	    !dbus_connection_register_fallback(conn, DESKTOP_PATH,
					       &desktop_vtable, NULL))
		fpkg_errorf("cannot register %s", DESKTOP_PATH);

	dbus_bus_add_match(
		conn,
		"type='signal',"
		"interface='org.freedesktop.DBus',member='NameOwnerChanged'",
		NULL);
	dbus_connection_add_filter(conn, name_owner_filter, NULL, NULL);

	fpkg_infof("owning %s on %s (interface version %u)", PORTAL_IFACE,
		   cli_getenv("DBUS_SESSION_BUS_ADDRESS")
			   ? cli_getenv("DBUS_SESSION_BUS_ADDRESS")
			   : "session bus",
		   PORTAL_VERSION);

	{
		const char* t = cli_getenv("FLATPKG_FILECHOOSER_TIMEOUT");
		if (t && *t) fc_timeout = strtol(t, NULL, 10);
	}

	for (;;) {
		dbus_connection_read_write_dispatch(conn, 100);
		fc_reap(conn);
		reap_children(conn);
		reap_update_children(conn);
		cleanup_monitors(conn);
		static time_t last_sweep;
		time_t nowt = time(NULL);
		if (nowt - last_sweep >= 30) {
			last_sweep = nowt;
			sweep_stale_instances();
		}
	}
	return 0;
}
