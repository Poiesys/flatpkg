#include <gio/gio.h>
#include <glib.h>

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "flatpkg.h"
#define FPKG_MODULE "session"
#include "fpkglog.h"

#define HELPER_BUS_NAME "org.freedesktop.Flatpak"
#define HELPER_DEV_PATH "/org/freedesktop/Flatpak/Development"
#define HELPER_DEV_IFACE "org.freedesktop.Flatpak.Development"

#define HELPER_DEV_VERSION 1u

#define HOSTCMD_CLEAR_ENV (1u << 0)
#define HOSTCMD_WATCH_BUS (1u << 1)
#define HOSTCMD_FLAGS_ALL (HOSTCMD_CLEAR_ENV | HOSTCMD_WATCH_BUS)

static GDBusConnection* session_bus;
static gchar** original_environ;
static GHashTable* client_pid_data_hash;

typedef struct {
	GPid pid;
	gchar* client;
	gboolean watch_bus;
} PidData;

typedef struct {
	gint from;
	gint to;
	gint final;
} FdMapEntry;

typedef struct {
	FdMapEntry* fd_map;
	gint fd_map_len;
	gboolean set_tty;
	gint tty;
} ChildSetupData;

static void free_pid_data(gpointer data) {
	PidData* pid_data = data;

	g_free(pid_data->client);
	g_free(pid_data);
}

// Tell the caller how the host command ended: the raw wait status, which
// flatpak-spawn decodes with WIFEXITED/WEXITSTATUS.
static void child_watch_died(GPid pid, gint status, gpointer user_data) {
	PidData* pid_data = user_data;

	g_dbus_connection_emit_signal(
		session_bus, pid_data->client, HELPER_DEV_PATH,
		HELPER_DEV_IFACE, "HostCommandExited",
		g_variant_new("(uu)", (guint32)pid, (guint32)status), NULL);
	g_hash_table_remove(client_pid_data_hash, GINT_TO_POINTER(pid));
}

static void child_setup_func(gpointer user_data) {
	ChildSetupData* data = user_data;
	FdMapEntry* fd_map = data->fd_map;
	sigset_t set;
	int i;

	sigemptyset(&set);
	(void)pthread_sigmask(SIG_SETMASK, &set, NULL);
	for (i = 1; i < NSIG; i++)
		if (i != SIGSTOP && i != SIGKILL) signal(i, SIG_DFL);

	for (i = 0; i < data->fd_map_len; i++)
		if (fd_map[i].from != fd_map[i].to) {
			dup2(fd_map[i].from, fd_map[i].to);
			close(fd_map[i].from);
		}
	for (i = 0; i < data->fd_map_len; i++)
		if (fd_map[i].to != fd_map[i].final) {
			dup2(fd_map[i].to, fd_map[i].final);
			close(fd_map[i].to);
		}

	setsid();
	setpgid(0, 0);

	if (data->set_tty)
		for (i = 0; i < data->fd_map_len; i++)
			if (fd_map[i].from == data->tty) {
				(void)ioctl(fd_map[i].final, TIOCSCTTY, 0);
				break;
			}
}

static void on_name_owner_changed(GDBusConnection* connection,
				  const gchar* sender_name,
				  const gchar* object_path,
				  const gchar* interface_name,
				  const gchar* signal_name,
				  GVariant* parameters, gpointer user_data) {
	const gchar *name = NULL, *from = NULL, *to = NULL;
	GHashTableIter iter;
	gpointer value;
	GList *list = NULL, *l;


	g_variant_get(parameters, "(&s&s&s)", &name, &from, &to);
	if (name[0] != ':' || strcmp(name, from) != 0 || to[0] != '\0') return;

	g_hash_table_iter_init(&iter, client_pid_data_hash);
	while (g_hash_table_iter_next(&iter, NULL, &value)) {
		PidData* pid_data = value;

		if (pid_data->watch_bus && g_str_equal(pid_data->client, name))
			list = g_list_prepend(list, pid_data);
	}
	for (l = list; l; l = l->next) killpg(((PidData*)l->data)->pid, SIGINT);
	g_list_free(list);
}

static pid_t helper_caller_pid(GDBusConnection* connection,
			       const gchar* sender) {
	GVariant* reply;
	guint32 pid = 0;
	GError* err = NULL;

	if (sender == NULL) return 0;
	reply = g_dbus_connection_call_sync(
		connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "GetConnectionUnixProcessID",
		g_variant_new("(s)", sender), G_VARIANT_TYPE("(u)"),
		G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &err);
	if (reply == NULL) {
		g_clear_error(&err);
		return 0;
	}
	g_variant_get(reply, "(u)", &pid);
	g_variant_unref(reply);
	return (pid_t)pid;
}

static void handle_host_command(GDBusConnection* connection,
				const gchar* sender, const gchar* object_path,
				const gchar* interface_name,
				const gchar* method_name, GVariant* parameters,
				GDBusMethodInvocation* invocation,
				gpointer user_data) {
	ChildSetupData child_setup_data = {NULL, 0, FALSE, 0};
	GVariant *arg_fds = NULL, *arg_envs = NULL;
	GDBusMessage* message;
	GUnixFDList* fd_list;
	gchar* arg_cwd = NULL;
	gchar** arg_argv = NULL;
	const gint* fds = NULL;
	gsize n_fds = 0, i, j;
	guint flags = 0;
	gint max_fd;
	gchar** env;
	GPid pid;
	PidData* pid_data;
	GError* error = NULL;
	FdMapEntry* fd_map = NULL;
	gchar* cwd;


	// Who is asking, before anything else: without this any client on a
	// private session bus gets a process. The same question the portal's
	// Spawn asks.
	{
		pid_t caller = helper_caller_pid(connection, sender);
		char* exe = NULL;
		fp_caller_t who =
			(caller > 0) ? flatpkg_caller_check(caller, NULL, &exe)
				     : FP_CALLER_UNREADABLE;

		if (who != FP_CALLER_JAIL && who != FP_CALLER_PROXY) {
			if (who == FP_CALLER_UNREADABLE)
				fpkg_warnf(
					"host command REFUSED from %s: "
					"cannot read pid %ld's executable, so "
					"it is not a flatpkg sandbox",
					sender ? sender : "?", (long)caller);
			else
				fpkg_warnf("host command REFUSED from %s: "
					   "pid %ld ran '%s', which is not "
					   "flatpkg's "
					   "dbus proxy, so it is not a flatpkg "
					   "sandbox",
					   sender ? sender : "?", (long)caller,
					   exe ? exe : "?");
			g_free(exe);
			g_dbus_method_invocation_return_error(
				invocation, G_DBUS_ERROR,
				G_DBUS_ERROR_ACCESS_DENIED,
				"HostCommand only serves a flatpkg sandbox");
			return;
		}
		g_free(exe);
	}

	g_variant_get(parameters, "(^ay^aay@a{uh}@a{ss}u)", &arg_cwd, &arg_argv,
		      &arg_fds, &arg_envs, &flags);

	message = g_dbus_method_invocation_get_message(invocation);
	fd_list = g_dbus_message_get_unix_fd_list(message);
	if (fd_list != NULL) {
		n_fds = (gsize)g_unix_fd_list_get_length(fd_list);
		fds = g_unix_fd_list_peek_fds(fd_list, NULL);
	}

	if (arg_argv == NULL || arg_argv[0] == NULL || arg_argv[0][0] == '\0') {
		g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
						      G_DBUS_ERROR_INVALID_ARGS,
						      "No command given");
		goto out;
	}
	if ((flags & ~HOSTCMD_FLAGS_ALL) != 0) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Unexpected flags 0x%x", flags);
		goto out;
	}

	fd_map = g_new0(FdMapEntry, n_fds);
	for (i = 0; i < n_fds; i++) {
		gint32 handle, fd;

		g_variant_get_child(arg_fds, i, "{uh}", &fd, &handle);
		fd_map[i].from = fds[i];
		fd_map[i].to = fd;
		fd_map[i].final = fd;

		if ((fd == 0 || fd == 1 || fd == 2) &&
		    !child_setup_data.set_tty && isatty(fds[i])) {
			child_setup_data.set_tty = TRUE;
			child_setup_data.tty = fds[i];
		}
	}
	child_setup_data.fd_map = fd_map;
	child_setup_data.fd_map_len = (gint)n_fds;

	max_fd = -1;
	for (i = 0; i < n_fds; i++) {
		if (fd_map[i].from > max_fd) max_fd = fd_map[i].from;
		if (fd_map[i].to > max_fd) max_fd = fd_map[i].to;
	}
	for (i = 0; i < n_fds; i++) {
		gboolean conflict = FALSE;

		for (j = i + 1; j < n_fds; j++)
			if (fd_map[j].from == fd_map[i].to) {
				conflict = TRUE;
				break;
			}
		if (conflict) fd_map[i].to = ++max_fd;
	}

	if (flags & HOSTCMD_CLEAR_ENV) {
		gchar* empty[] = {NULL};
		env = g_strdupv(empty);
	} else {
		env = g_strdupv(original_environ);
	}
	{
		GVariantIter iter;
		const gchar *var = NULL, *val = NULL;

		g_variant_iter_init(&iter, arg_envs);
		while (g_variant_iter_next(&iter, "{&s&s}", &var, &val))
			env = g_environ_setenv(env, var, val, TRUE);
	}

	cwd = (arg_cwd != NULL && arg_cwd[0] != '\0') ? arg_cwd : NULL;
	// GSpawnFlags is a set of bit flags; the two are combined in a variable
	// rather than cast out of an int.
	GSpawnFlags spawn_flags = G_SPAWN_SEARCH_PATH;
	spawn_flags |= G_SPAWN_DO_NOT_REAP_CHILD;
	if (!g_spawn_async_with_pipes(cwd, arg_argv, env, spawn_flags,
				      child_setup_func, &child_setup_data, &pid,
				      NULL, NULL, NULL, &error)) {
		gint code = G_DBUS_ERROR_FAILED;

		if (g_error_matches(error, G_SPAWN_ERROR, G_SPAWN_ERROR_ACCES))
			code = G_DBUS_ERROR_ACCESS_DENIED;
		else if (g_error_matches(error, G_SPAWN_ERROR,
					 G_SPAWN_ERROR_NOENT))
			code = G_DBUS_ERROR_FILE_NOT_FOUND;
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, code,
			"Failed to start command: %s", error->message);
		g_error_free(error);
		g_strfreev(env);
		goto out;
	}
	g_strfreev(env);

	pid_data = g_new0(PidData, 1);
	pid_data->pid = pid;
	pid_data->client = g_strdup(sender);
	pid_data->watch_bus = (flags & HOSTCMD_WATCH_BUS) != 0;
	g_hash_table_replace(client_pid_data_hash, GINT_TO_POINTER((gint)pid),
			     pid_data);
	g_child_watch_add(pid, child_watch_died, pid_data);

	fpkg_infof("host command from %s: %s", sender ? sender : "?",
		   arg_argv[0]);
	g_dbus_method_invocation_return_value(
		invocation, g_variant_new("(u)", (guint32)pid));

out:
	g_free(fd_map);
	g_free(arg_cwd);
	g_strfreev(arg_argv);
	g_clear_pointer(&arg_fds, g_variant_unref);
	g_clear_pointer(&arg_envs, g_variant_unref);
}

static gboolean host_command_signal_allowed(const gchar* sender, pid_t pid) {
	PidData* pid_data;
	gpointer value;

	if (sender == NULL || pid <= 0) return FALSE;
	if (!g_hash_table_lookup_extended(client_pid_data_hash,
					  GINT_TO_POINTER((gint)pid), NULL,
					  &value))
		return FALSE;
	pid_data = value;
	return pid_data->client != NULL &&
	       strcmp(pid_data->client, sender) == 0;
}

static void handle_host_command_signal(
	GDBusConnection* connection, const gchar* sender,
	const gchar* object_path, const gchar* interface_name,
	const gchar* method_name, GVariant* parameters,
	GDBusMethodInvocation* invocation, gpointer user_data) {
	guint32 pid = 0, signal_num = 0;
	gboolean to_pgroup = FALSE;


	g_variant_get(parameters, "(uub)", &pid, &signal_num, &to_pgroup);
	if (!host_command_signal_allowed(sender, (pid_t)pid)) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
			"pid %u is not a host command this caller started",
			pid);
		return;
	}
	if (to_pgroup) killpg((pid_t)pid, (gint)signal_num);
	else
		kill((pid_t)pid, (gint)signal_num);

	g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant* dev_get_property(GDBusConnection* connection,
				  const gchar* sender, const gchar* object_path,
				  const gchar* interface_name,
				  const gchar* property_name, GError** error,
				  gpointer user_data) {

	if (g_strcmp0(property_name, "version") == 0)
		return g_variant_new_uint32(HELPER_DEV_VERSION);

	g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		    "No such property '%s'", property_name);
	return NULL;
}

static const char dev_xml[] =
	"<node>"
	"  <interface name=\"" HELPER_DEV_IFACE "\">"
	"    <property name=\"version\" type=\"u\" access=\"read\"/>"
	"    <method name=\"HostCommand\">"
	"      <arg type=\"ay\" name=\"cwd_path\" direction=\"in\"/>"
	"      <arg type=\"aay\" name=\"argv\" direction=\"in\"/>"
	"      <arg type=\"a{uh}\" name=\"fds\" direction=\"in\"/>"
	"      <arg type=\"a{ss}\" name=\"envs\" direction=\"in\"/>"
	"      <arg type=\"u\" name=\"flags\" direction=\"in\"/>"
	"      <arg type=\"u\" name=\"pid\" direction=\"out\"/>"
	"    </method>"
	"    <method name=\"HostCommandSignal\">"
	"      <arg type=\"u\" name=\"pid\" direction=\"in\"/>"
	"      <arg type=\"u\" name=\"signal\" direction=\"in\"/>"
	"      <arg type=\"b\" name=\"to_process_group\" direction=\"in\"/>"
	"    </method>"
	"    <signal name=\"HostCommandExited\">"
	"      <arg type=\"u\" name=\"pid\"/>"
	"      <arg type=\"u\" name=\"exit_status\"/>"
	"    </signal>"
	"  </interface>"
	"</node>";

static void dev_method_call(GDBusConnection* connection, const gchar* sender,
			    const gchar* object_path,
			    const gchar* interface_name,
			    const gchar* method_name, GVariant* parameters,
			    GDBusMethodInvocation* invocation,
			    gpointer user_data) {
	if (g_strcmp0(method_name, "HostCommand") == 0)
		handle_host_command(connection, sender, object_path,
				    interface_name, method_name, parameters,
				    invocation, user_data);
	else if (g_strcmp0(method_name, "HostCommandSignal") == 0)
		handle_host_command_signal(connection, sender, object_path,
					   interface_name, method_name,
					   parameters, invocation, user_data);
	else
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
			"No such method '%s'", method_name);
}

static const GDBusInterfaceVTable dev_interface_vtable = {
	.method_call = dev_method_call,
	.get_property = dev_get_property,
	.set_property = NULL,
};

static void on_bus_acquired(GDBusConnection* connection, const gchar* name,
			    gpointer user_data) {
	GDBusNodeInfo* info;
	GError* error = NULL;
	guint id;


	session_bus = connection;
	info = g_dbus_node_info_new_for_xml(dev_xml, &error);
	if (info == NULL) {
		fpkg_errorf("session helper interface XML: %s", error->message);
		g_error_free(error);
		return;
	}
	id = g_dbus_connection_register_object(
		connection, HELPER_DEV_PATH, info->interfaces[0],
		&dev_interface_vtable, NULL, NULL, &error);
	if (id == 0) {
		fpkg_errorf("cannot register %s: %s", HELPER_DEV_PATH,
			    error->message);
		g_error_free(error);
	} else {
		fpkg_infof("owning %s (%s, version %u)", name, HELPER_DEV_IFACE,
			   HELPER_DEV_VERSION);
	}
	g_dbus_node_info_unref(info);

	g_dbus_connection_signal_subscribe(
		connection, "org.freedesktop.DBus", "org.freedesktop.DBus",
		"NameOwnerChanged", "/org/freedesktop/DBus", NULL,
		G_DBUS_SIGNAL_FLAGS_NONE, on_name_owner_changed, NULL, NULL);
}

static void on_name_lost(GDBusConnection* connection, const gchar* name,
			 gpointer user_data) {
	dprintf(2,
		"flatpkg session-helper: cannot own %s (another "
		"session helper?)\n",
		name);
	_exit(1);
}

int session_helper_main(void) {
	GMainLoop* loop;


	original_environ = g_get_environ();
	client_pid_data_hash = g_hash_table_new_full(
		g_direct_hash, g_direct_equal, NULL, free_pid_data);

	loop = g_main_loop_new(NULL, FALSE);

	g_bus_own_name(G_BUS_TYPE_SESSION, HELPER_BUS_NAME,
		       G_BUS_NAME_OWNER_FLAGS_NONE, on_bus_acquired, NULL,
		       on_name_lost, NULL, NULL);

	g_main_loop_run(loop);
	return 0;
}
