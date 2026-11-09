#include <gio/gio.h>
#include <glib.h>

#include <grp.h>
#include <pwd.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "flatpkg.h"
#define FPKG_MODULE "syshelper"
#include "fpkglog.h"

#define SYSHELPER_BUS_NAME "org.freedesktop.Flatpak.SystemHelper"
#define SYSHELPER_PATH "/org/freedesktop/Flatpak/SystemHelper"

// Upstream advertises version 2.
#define SYSHELPER_VERSION 2u

static gboolean helper_run_mapped(const char* request,
				  fpkg_helper_args_t* args,
				  GDBusMethodInvocation* invocation);

static GDBusConnection* system_bus;

static gboolean uid_allowed(uid_t uid) {
	struct passwd* pw;
	struct group* g;
	gid_t groups[NGROUPS_MAX];
	int ngroups = NGROUPS_MAX;

	if (uid == 0) return TRUE;
	pw = getpwuid(uid);
	if (pw == NULL) return FALSE;
	g = getgrnam("flatpkg");
	if (g == NULL) return FALSE;
	if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) < 0)
		return FALSE;
	for (int i = 0; i < ngroups; i++)
		if (groups[i] == g->gr_gid) return TRUE;
	return FALSE;
}

static gboolean caller_uid(GDBusConnection* connection, const gchar* sender,
			   uid_t* uid, GError** error) {
	GMainContext* ctx = g_main_context_new();
	GVariant* reply;
	guint32 u = 0;

	g_main_context_push_thread_default(ctx);
	reply = g_dbus_connection_call_sync(
		connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		"org.freedesktop.DBus", "GetConnectionUnixUser",
		g_variant_new("(s)", sender), G_VARIANT_TYPE("(u)"),
		G_DBUS_CALL_FLAGS_NONE, -1, NULL, error);
	g_main_context_pop_thread_default(ctx);
	g_main_context_unref(ctx);

	if (reply == NULL) return FALSE;
	g_variant_get(reply, "(u)", &u);
	g_variant_unref(reply);
	*uid = (uid_t)u;
	return TRUE;
}

static gboolean authorize(GDBusConnection* connection, const gchar* sender,
			  GDBusMethodInvocation* invocation,
			  const gchar* method) {
	GError* error = NULL;
	uid_t uid = 0;

	if (!caller_uid(connection, sender, &uid, &error)) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
			"cannot resolve caller: %s", error->message);
		g_error_free(error);
		return FALSE;
	}
	if (!uid_allowed(uid)) {
		fpkg_warnf("%s from uid %lu denied (not in the flatpkg "
			   "group)",
			   method, (unsigned long)uid);
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
			"Not authorized to use the flatpkg system helper "
			"(join the 'flatpkg' group)");
		return FALSE;
	}
	return TRUE;
}

static gchar* variant_path(GVariant* v) {
	gsize len = 0;
	const gchar* raw;

	if (v == NULL) return g_strdup("");
	raw = g_variant_get_fixed_array(v, &len, 1);
	if (raw == NULL) return g_strdup("");
	return g_strndup(raw, len);
}

static gboolean valid_arch(const gchar* arch) {
	static const gchar* known[] = {"x86_64", "i386",    "aarch64",
				       "arm",	 "riscv64", NULL};

	for (int i = 0; known[i]; i++)
		if (g_str_equal(arch, known[i])) return TRUE;
	return FALSE;
}

// The rule lives in ostree.c as remote_name_valid(), declared in flatpkg.h,
// so the CLI and this side enforce the same one.
#define valid_remote_name(name) remote_name_valid(name)

static void
handle_deploy_appstream(GDBusConnection* connection, const gchar* sender,
			const gchar* object_path, const gchar* interface_name,
			const gchar* method_name, GVariant* parameters,
			GDBusMethodInvocation* invocation, gpointer user_data) {
	GVariant* path_v = NULL;
	gchar* repo_path = NULL;
	const gchar *origin = NULL, *arch = NULL, *installation = NULL;
	guint flags = 0;


	g_variant_get(parameters, "(@ayu&s&s&s)", &path_v, &flags, &origin,
		      &arch, &installation);
	repo_path = variant_path(path_v);

	if (!authorize(connection, sender, invocation, "DeployAppstream")) {
		g_variant_unref(path_v);
		g_free(repo_path);
		return;
	}

	if (*installation != '\0' && !g_str_equal(installation, "default")) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Unknown installation '%s' (flatpkg has only the "
			"system installation)",
			installation);
		g_variant_unref(path_v);
		g_free(repo_path);
		return;
	}
	if (!valid_remote_name(origin)) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Invalid remote name '%s'", origin);
		g_variant_unref(path_v);
		g_free(repo_path);
		return;
	}
	if (!valid_arch(arch)) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Invalid architecture '%s'", arch);
		g_variant_unref(path_v);
		g_free(repo_path);
		return;
	}
	if (*repo_path != '\0' && !g_file_test(repo_path, G_FILE_TEST_EXISTS)) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Path does not exist: %s", repo_path);
		g_variant_unref(path_v);
		g_free(repo_path);
		return;
	}

	fpkg_helper_args_t hargs = {
		.repo = repo_path,
		.origin = origin,
		.arch = arch,
		.installation = installation,
		.flags = flags,
	};
	if (!helper_run_mapped("deploy-appstream", &hargs, invocation)) {
		g_variant_unref(path_v);
		g_free(repo_path);
		return;
	}
	g_dbus_method_invocation_return_value(invocation, NULL);
	g_variant_unref(path_v);
	g_free(repo_path);
	return;

}

static void handle_deploy(GDBusConnection* connection, const gchar* sender,
			  const gchar* object_path, const gchar* interface_name,
			  const gchar* method_name, GVariant* parameters,
			  GDBusMethodInvocation* invocation,
			  gpointer user_data) {
	GVariant* path_v = NULL;
	gchar* repo_path = NULL;
	const gchar *ref = NULL, *origin = NULL, *installation = NULL;
	gchar **subpaths = NULL, **previous_ids = NULL;
	guint flags = 0;


	g_variant_get(parameters, "(@ayu&s&s^as^as&s)", &path_v, &flags, &ref,
		      &origin, &subpaths, &previous_ids, &installation);
	repo_path = variant_path(path_v);

	if (!authorize(connection, sender, invocation, "Deploy")) goto out;
	if (*installation != '\0' && !g_str_equal(installation, "default")) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Unknown installation '%s' (flatpkg has only the "
			"system installation)",
			installation);
		goto out;
	}
	if (ref == NULL || *ref == '\0' || origin == NULL || *origin == '\0') {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Deploy needs a ref and an origin");
		goto out;
	}
	if (*repo_path != '\0' && !g_file_test(repo_path, G_FILE_TEST_EXISTS)) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Path does not exist: %s", repo_path);
		goto out;
	}

	fpkg_infof("Deploy %s from %s (origin %s)", ref,
		   *repo_path ? repo_path : "the remote", origin);

	if (previous_ids != NULL && previous_ids[0] != NULL)
		fpkg_infof("  previous ids: %s", previous_ids[0]);
	if ((flags & ~(FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE |
		       FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY |
		       FLATPAK_HELPER_DEPLOY_FLAGS_LOCAL_PULL |
		       FLATPAK_HELPER_DEPLOY_FLAGS_REINSTALL |
		       FLATPAK_HELPER_DEPLOY_FLAGS_NO_INTERACTION |
		       FLATPAK_HELPER_DEPLOY_FLAGS_APP_HINT |
		       FLATPAK_HELPER_DEPLOY_FLAGS_INSTALL_HINT |
		       FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE_PINNED |
		       FLATPAK_HELPER_DEPLOY_FLAGS_UPDATE_PREINSTALLED |
		       FLATPAK_HELPER_DEPLOY_FLAGS_ALLOW_DOWNGRADE)) != 0) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Unsupported flags enabled: 0x%x", flags);
		goto out;
	}

	char* subs = NULL;
	if (subpaths != NULL) {
		size_t len = 1;
		for (size_t i = 0; subpaths[i] != NULL; i++)
			len += strlen(subpaths[i]) + 1;
		subs = calloc(len, 1);
		if (subs != NULL)
			for (size_t i = 0; subpaths[i] != NULL; i++) {
				if (i > 0) strlcat(subs, ",", len);
				strlcat(subs, subpaths[i], len);
			}
	}
	char* prev = NULL;
	if (previous_ids != NULL) {
		size_t len = 1;
		for (size_t i = 0; previous_ids[i] != NULL; i++)
			len += strlen(previous_ids[i]) + 1;
		prev = calloc(len, 1);
		if (prev != NULL)
			for (size_t i = 0; previous_ids[i] != NULL; i++) {
				if (i > 0) strlcat(prev, ",", len);
				strlcat(prev, previous_ids[i], len);
			}
	}
	fpkg_helper_args_t hargs = {
		.repo = repo_path,
		.ref = ref,
		.origin = origin,
		.subpaths = subs,
		.previous = prev,
		.installation = installation,
		.flags = flags,
	};
	gboolean mapped = helper_run_mapped("deploy", &hargs, invocation);
	free(subs);
	free(prev);
	if (!mapped) {
		g_variant_unref(path_v);
		g_free(repo_path);
		g_strfreev(subpaths);
		g_strfreev(previous_ids);
		return;
	}
	g_dbus_method_invocation_return_value(invocation, NULL);
	g_variant_unref(path_v);
	g_free(repo_path);
	g_strfreev(subpaths);
	g_strfreev(previous_ids);
	return;

out:
	g_variant_unref(path_v);
	g_free(repo_path);
	g_strfreev(subpaths);
	g_strfreev(previous_ids);
}

static void handle_uninstall(GDBusConnection* connection, const gchar* sender,
			     const gchar* object_path,
			     const gchar* interface_name,
			     const gchar* method_name, GVariant* parameters,
			     GDBusMethodInvocation* invocation,
			     gpointer user_data) {
	const gchar *ref = NULL, *installation = NULL;
	guint flags = 0;


	g_variant_get(parameters, "(u&s&s)", &flags, &ref, &installation);
	if (!authorize(connection, sender, invocation, "Uninstall")) return;
	if (*installation != '\0' && !g_str_equal(installation, "default")) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Unknown installation '%s'", installation);
		return;
	}
	if ((flags & ~(FLATPAK_HELPER_UNINSTALL_FLAGS_KEEP_REF |
		       FLATPAK_HELPER_UNINSTALL_FLAGS_FORCE_REMOVE |
		       FLATPAK_HELPER_UNINSTALL_FLAGS_NO_INTERACTION)) != 0) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Unsupported flags enabled: 0x%x", flags);
		return;
	}
	// The map is the contract: no entry means refused, not silently handled by a
	// daemon fallback. That keeps behaviour with the client that configured it.
	char** argv = NULL;
	size_t argc = 0;
	fpkg_helper_args_t hargs = {
		.ref = ref,
		.installation = installation,
	};
	int mapped = fpkg_helper_command("uninstall", &hargs, &argv, &argc);
	if (mapped < 0) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Invalid uninstall command in helper.conf");
		return;
	}
	if (mapped == 0) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
			"No uninstall command is configured in helper.conf");
		return;
	}
	if (run_program(argv, -1, -1, -1) != 0) {
		for (size_t i = 0; i < argc; i++) free(argv[i]);
		free(argv);
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
			"Could not uninstall %s", ref);
		return;
	}
	for (size_t i = 0; i < argc; i++) free(argv[i]);
	free(argv);
	store_mark_changed();
	g_dbus_method_invocation_return_value(invocation, NULL);
}

static gboolean helper_call_ok(GDBusConnection* connection, const gchar* sender,
			       GDBusMethodInvocation* invocation,
			       const gchar* method, const gchar* installation,
			       guint flags, guint known) {
	if (!authorize(connection, sender, invocation, method)) return FALSE;

	if (*installation != '\0' && !g_str_equal(installation, "default")) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Unknown installation '%s' (flatpkg has only the "
			"system installation)",
			installation);
		return FALSE;
	}

	if ((flags & ~known) != 0) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Unsupported flags enabled: 0x%x", flags);
		return FALSE;
	}
	return TRUE;
}

static void handle_ensure_repo(GDBusConnection* connection, const gchar* sender,
			       const gchar* object_path,
			       const gchar* interface_name,
			       const gchar* method_name, GVariant* parameters,
			       GDBusMethodInvocation* invocation,
			       gpointer user_data) {
	const gchar* installation = NULL;
	guint flags = 0;


	g_variant_get(parameters, "(u&s)", &flags, &installation);
	if (!helper_call_ok(connection, sender, invocation, "EnsureRepo",
			    installation, flags,
			    FLATPAK_HELPER_NO_INTERACTION_FLAG))
		return;

	fpkg_helper_args_t hargs = {
		.installation = installation,
		.flags = flags,
	};
	if (!helper_run_mapped("ensure-repo", &hargs, invocation)) return;
	g_dbus_method_invocation_return_value(invocation, NULL);

}

static void
handle_prune_local_repo(GDBusConnection* connection, const gchar* sender,
			const gchar* object_path, const gchar* interface_name,
			const gchar* method_name, GVariant* parameters,
			GDBusMethodInvocation* invocation, gpointer user_data) {
	const gchar* installation = NULL;
	guint flags = 0;


	g_variant_get(parameters, "(u&s)", &flags, &installation);
	if (!helper_call_ok(connection, sender, invocation, "PruneLocalRepo",
			    installation, flags,
			    FLATPAK_HELPER_NO_INTERACTION_FLAG))
		return;

	fpkg_helper_args_t hargs = {
		.installation = installation,
		.flags = flags,
	};
	if (!helper_run_mapped("prune-local-repo", &hargs, invocation))
		return;
	g_dbus_method_invocation_return_value(invocation, NULL);

}

static void
handle_remove_local_ref(GDBusConnection* connection, const gchar* sender,
			const gchar* object_path, const gchar* interface_name,
			const gchar* method_name, GVariant* parameters,
			GDBusMethodInvocation* invocation, gpointer user_data) {
	const gchar *remote = NULL, *ref = NULL, *installation = NULL;
	guint flags = 0;


	g_variant_get(parameters, "(u&s&s&s)", &flags, &remote, &ref,
		      &installation);
	if (!helper_call_ok(connection, sender, invocation, "RemoveLocalRef",
			    installation, flags,
			    FLATPAK_HELPER_NO_INTERACTION_FLAG))
		return;

	if (!valid_remote_name(remote)) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Invalid remote name '%s'", remote);
		return;
	}

	fpkg_helper_args_t hargs = {
		.remote = remote,
		.ref = ref,
		.installation = installation,
		.flags = flags,
	};
	if (!helper_run_mapped("remove-local-ref", &hargs, invocation))
		return;
	g_dbus_method_invocation_return_value(invocation, NULL);

}

static void handle_run_triggers(GDBusConnection* connection,
				const gchar* sender, const gchar* object_path,
				const gchar* interface_name,
				const gchar* method_name, GVariant* parameters,
				GDBusMethodInvocation* invocation,
				gpointer user_data) {
	const gchar* installation = NULL;
	guint flags = 0;


	g_variant_get(parameters, "(u&s)", &flags, &installation);
	if (!helper_call_ok(connection, sender, invocation, "RunTriggers",
			    installation, flags,
			    FLATPAK_HELPER_NO_INTERACTION_FLAG))
		return;

	fpkg_helper_args_t hargs = {
		.installation = installation,
		.flags = flags,
	};
	if (!helper_run_mapped("run-triggers", &hargs, invocation)) return;
	g_dbus_method_invocation_return_value(invocation, NULL);

}

static void
handle_update_summary(GDBusConnection* connection, const gchar* sender,
		      const gchar* object_path, const gchar* interface_name,
		      const gchar* method_name, GVariant* parameters,
		      GDBusMethodInvocation* invocation, gpointer user_data) {
	const gchar* installation = NULL;
	guint flags = 0;


	g_variant_get(parameters, "(u&s)", &flags, &installation);
	if (!helper_call_ok(connection, sender, invocation, "UpdateSummary",
			    installation, flags,
			    FLATPAK_HELPER_UPDATE_SUMMARY_FLAGS_DELETE |
				    FLATPAK_HELPER_NO_INTERACTION_FLAG))
		return;

	fpkg_helper_args_t hargs = {
		.installation = installation,
		.flags = flags,
	};
	if (!helper_run_mapped("update-summary", &hargs, invocation))
		return;
	g_dbus_method_invocation_return_value(invocation, NULL);
}

static void handle_configure(GDBusConnection* connection, const gchar* sender,
			     const gchar* object_path,
			     const gchar* interface_name,
			     const gchar* method_name, GVariant* parameters,
			     GDBusMethodInvocation* invocation,
			     gpointer user_data) {
	const gchar *key = NULL, *value = NULL, *installation = NULL;
	guint flags = 0;


	g_variant_get(parameters, "(u&s&s&s)", &flags, &key, &value,
		      &installation);
	if (!helper_call_ok(
		    connection, sender, invocation, "Configure", installation,
		    flags,
		    FLATPAK_HELPER_CONFIGURE_FLAGS_UNSET |
			    FLATPAK_HELPER_CONFIGURE_FLAGS_NO_INTERACTION))
		return;

	if (!g_str_equal(key, "languages") &&
	    !g_str_equal(key, "extra-languages") &&
	    !g_str_equal(key, "masked") && !g_str_equal(key, "pinned")) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Unsupported key: %s", key);
		return;
	}

	fpkg_helper_args_t hargs = {
		.installation = installation,
		.key = key,
		.value = value,
		.flags = flags,
	};
	if (!helper_run_mapped("configure", &hargs, invocation)) return;
	g_dbus_method_invocation_return_value(invocation, NULL);
}

// ConfigureRemote: the remote's whole [remote "<name>"] group as keyfile text;
// a config without that group means "remove this remote".
static void
handle_configure_remote(GDBusConnection* connection, const gchar* sender,
			const gchar* object_path, const gchar* interface_name,
			const gchar* method_name, GVariant* parameters,
			GDBusMethodInvocation* invocation, gpointer user_data) {
	GVariant* gpg_v = NULL;
	const gchar *remote = NULL, *config = NULL, *installation = NULL;
	guint flags = 0;
	GKeyFile* kf = NULL;
	gchar* group = NULL;
	gchar** keys = NULL;


	g_variant_get(parameters, "(u&s&s@ay&s)", &flags, &remote, &config,
		      &gpg_v, &installation);
	if (!helper_call_ok(
		    connection, sender, invocation, "ConfigureRemote",
		    installation, flags,
		    FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_FORCE_REMOVE |
			    FLATPAK_HELPER_CONFIGURE_REMOTE_FLAGS_NO_INTERACTION))
		goto out;

	if (!valid_remote_name(remote)) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Invalid remote name '%s'", remote);
		goto out;
	}

	kf = g_key_file_new();
	if (!g_key_file_load_from_data(kf, config, -1, G_KEY_FILE_NONE, NULL)) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Invalid config for remote '%s'", remote);
		goto out;
	}
	group = g_strdup_printf("remote \"%s\"", remote);
	fpkg_helper_args_t hargs = {
		.remote = remote,
		.config = config,
		.installation = installation,
		.flags = flags,
	};
	const char* what;
	if (!g_key_file_has_group(kf, group)) {
		what = "configure-remote-remove";
	} else {
		gchar* url = g_key_file_get_string(kf, group, "url", NULL);
		hargs.url = url;
		what = "configure-remote-add";
		gboolean mapped = helper_run_mapped(what, &hargs, invocation);
		g_free(url);
		if (!mapped) goto out;
		g_dbus_method_invocation_return_value(invocation, NULL);
		goto out;
	}
	if (!helper_run_mapped(what, &hargs, invocation)) goto out;
	g_dbus_method_invocation_return_value(invocation, NULL);

out:
	g_strfreev(keys);
	g_free(group);
	if (kf != NULL) g_key_file_free(kf);
	g_variant_unref(gpg_v);
}

static void handle_update_remote(GDBusConnection* connection,
				 const gchar* sender, const gchar* object_path,
				 const gchar* interface_name,
				 const gchar* method_name, GVariant* parameters,
				 GDBusMethodInvocation* invocation,
				 gpointer user_data) {
	guint flags = 0;
	const gchar *remote = NULL, *installation = NULL;
	GVariant *summary_v = NULL, *sig_v = NULL;
	gchar *summary_path = NULL, *sig_path = NULL;
	char* error_msg = NULL;


	g_variant_get(parameters, "(u&s&s@ay@ay)", &flags, &remote,
		      &installation, &summary_v, &sig_v);
	if (!helper_call_ok(connection, sender, invocation, "UpdateRemote",
			    installation, flags,
			    FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_ALL))
		goto out;

	if (flags & FLATPAK_HELPER_UPDATE_REMOTE_FLAGS_SUMMARY_IS_INDEX) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
			"flatpkg reads classic OSTree summaries; an index "
			"summary cannot be applied to remote '%s'",
			remote);
		goto out;
	}
	if (!valid_remote_name(remote)) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Invalid remote name '%s'", remote);
		goto out;
	}

	summary_path = variant_path(summary_v);
	sig_path = variant_path(sig_v);
	if (summary_path == NULL || *summary_path == '\0') {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"UpdateRemote needs a summary path");
		goto out;
	}

	fpkg_helper_args_t hargs = {
		.remote = remote,
		.summary = summary_path,
		.summary_sig = sig_path,
		.installation = installation,
		.flags = flags,
	};
	if (!helper_run_mapped("update-remote", &hargs, invocation))
		goto out;
	g_dbus_method_invocation_return_value(invocation, NULL);

out:
	g_free(error_msg);
	g_free(summary_path);
	g_free(sig_path);
	if (summary_v != NULL) g_variant_unref(summary_v);
	if (sig_v != NULL) g_variant_unref(sig_v);
}

static gboolean helper_run_mapped(const char* request,
				  fpkg_helper_args_t* args,
				  GDBusMethodInvocation* invocation) {
	char** argv = NULL;
	size_t argc = 0;
	int mapped = fpkg_helper_command(request, args, &argv, &argc);
	if (mapped < 0) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Invalid %s command in helper.conf", request);
		return FALSE;
	}
	if (mapped == 0) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
			"No %s command is configured in helper.conf", request);
		return FALSE;
	}
	int rc = run_program(argv, -1, -1, -1);
	for (size_t i = 0; i < argc; i++) free(argv[i]);
	free(argv);
	if (rc != 0) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
			"%s failed", request);
		return FALSE;
	}
	return TRUE;
}

static void
handle_install_bundle(GDBusConnection* connection, const gchar* sender,
		      const gchar* object_path, const gchar* interface_name,
		      const gchar* method_name, GVariant* parameters,
		      GDBusMethodInvocation* invocation, gpointer user_data) {
	GVariant* path_v = NULL;
	gchar* bundle_path = NULL;
	const gchar *remote = NULL, *installation = NULL;
	guint flags = 0;
	char* ref = NULL;


	g_variant_get(parameters, "(@ayu&s&s)", &path_v, &flags, &remote,
		      &installation);
	bundle_path = variant_path(path_v);

	if (!helper_call_ok(
		    connection, sender, invocation, "InstallBundle",
		    installation, flags,
		    FLATPAK_HELPER_INSTALL_BUNDLE_FLAGS_REINSTALL |
			    FLATPAK_HELPER_INSTALL_BUNDLE_FLAGS_NO_INTERACTION))
		goto out;

	if (*bundle_path == '\0' ||
	    !g_file_test(bundle_path, G_FILE_TEST_IS_REGULAR)) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Bundle %s does not exist", bundle_path);
		goto out;
	}
	if (*remote != '\0' && !valid_remote_name(remote)) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Invalid remote name '%s'", remote);
		goto out;
	}

	fpkg_helper_args_t hargs = {
		.bundle = bundle_path,
		.remote = remote,
		.installation = installation,
		.flags = flags,
	};
	char** argv = NULL;
	size_t argc = 0;
	int mapped = fpkg_helper_command("install-bundle", &hargs, &argv,
					 &argc);
	if (mapped < 0) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS,
			"Invalid install-bundle command in helper.conf");
		goto out;
	}
	if (mapped == 0) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
			"No install-bundle command is configured in helper.conf");
		goto out;
	}
	int hrc = run_program(argv, -1, -1, -1);
	for (size_t i = 0; i < argc; i++) free(argv[i]);
	free(argv);
	if (hrc != 0) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
			"Could not install the bundle %s", bundle_path);
		goto out;
	}
	// The CLI's log line is width-truncated, so read the ref from the
	// bundle itself for the D-Bus return value.
	if (ostree_bundle_ref(bundle_path, &ref) != 0) {
		g_dbus_method_invocation_return_error(
			invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
			"Could not read the ref from the bundle %s",
			bundle_path);
		goto out;
	}
	g_dbus_method_invocation_return_value(
		invocation, g_variant_new("(s)", ref != NULL ? ref : ""));
	goto out;

out:
	free(ref);
	g_free(bundle_path);
	g_variant_unref(path_v);
}

static void handle_unimplemented(GDBusConnection* connection,
				 const gchar* sender, const gchar* object_path,
				 const gchar* interface_name,
				 const gchar* method_name, GVariant* parameters,
				 GDBusMethodInvocation* invocation,
				 gpointer user_data) {

	if (g_str_equal(method_name, "GenerateOciSummary")) {
		const gchar *origin = NULL, *installation = NULL;
		guint flags = 0;
		g_variant_get(parameters, "(u&s&s)", &flags, &origin,
			      &installation);
		if (!authorize(connection, sender, invocation,
			       "GenerateOciSummary"))
			return;
		fpkg_helper_args_t hargs = {
			.origin = origin,
			.installation = installation,
			.flags = flags,
		};
		if (!helper_run_mapped("generate-oci-summary", &hargs,
				       invocation))
			return;
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}
	if (g_str_equal(method_name, "CancelPull")) {
		const gchar *installation = NULL, *src_dir = NULL;
		guint flags = 0;
		g_variant_get(parameters, "(u&s&s)", &flags, &installation,
			      &src_dir);
		if (!authorize(connection, sender, invocation, "CancelPull"))
			return;
		fpkg_helper_args_t hargs = {
			.installation = installation,
			.src_dir = src_dir,
			.flags = flags,
		};
		if (!helper_run_mapped("cancel-pull", &hargs, invocation))
			return;
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}

	const gchar* why;

	if (g_str_equal(method_name, "GetRevokefsFd"))
		why = "flatpkg pulls through a child repository instead of "
		      "revokefs (a FUSE filesystem FreeBSD's jails do not "
		      "need); clients that ask for the fd fall back to the "
		      "child-repo path when this call fails";
	else if (g_str_equal(method_name, "GenerateOciSummary"))
		why = "flatpkg has no OCI remote support";
	else
		why = "install and update through the host CLI instead "
		      "(`flatpak-spawn --host flatpak install ...`)";

	g_dbus_method_invocation_return_error(
		invocation, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED,
		"%s is not implemented by flatpkg's system helper: %s",
		method_name, why);
}

static GVariant* syshelper_get_property(GDBusConnection* connection,
					const gchar* sender,
					const gchar* object_path,
					const gchar* interface_name,
					const gchar* property_name,
					GError** error, gpointer user_data) {

	if (g_str_equal(property_name, "version"))
		return g_variant_new_uint32(SYSHELPER_VERSION);

	g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
		    "No such property '%s'", property_name);
	return NULL;
}

static const char syshelper_xml[] =
	"<node>"
	"  <interface name=\"org.freedesktop.Flatpak.SystemHelper\">"
	"    <property name=\"version\" type=\"u\" access=\"read\"/>"
	"    <method name=\"Deploy\">"
	"      <arg type=\"ay\" name=\"repo_path\" direction=\"in\"/>"
	"      <arg type=\"u\" name=\"flags\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"ref\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"origin\" direction=\"in\"/>"
	"      <arg type=\"as\" name=\"subpaths\" direction=\"in\"/>"
	"      <arg type=\"as\" name=\"previous_ids\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"installation\" direction=\"in\"/>"
	"    </method>"
	"    <method name=\"DeployAppstream\">"
	"      <arg type=\"ay\" name=\"repo_path\" direction=\"in\"/>"
	"      <arg type=\"u\" name=\"flags\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"origin\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"arch\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"installation\" direction=\"in\"/>"
	"    </method>"
	"    <method name=\"Uninstall\">"
	"      <arg type=\"u\" name=\"flags\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"ref\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"installation\" direction=\"in\"/>"
	"    </method>"
	"    <method name=\"InstallBundle\">"
	"      <arg type=\"ay\" name=\"bundle_path\" direction=\"in\"/>"
	"      <arg type=\"u\" name=\"flags\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"remote\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"installation\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"ref\" direction=\"out\"/>"
	"    </method>"
	"    <method name=\"ConfigureRemote\">"
	"      <arg type=\"u\" name=\"flags\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"remote\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"config\" direction=\"in\"/>"
	"      <arg type=\"ay\" name=\"gpg_key\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"installation\" direction=\"in\"/>"
	"    </method>"
	"    <method name=\"Configure\">"
	"      <arg type=\"u\" name=\"flags\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"key\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"value\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"installation\" direction=\"in\"/>"
	"    </method>"
	"    <method name=\"UpdateRemote\">"
	"      <arg type=\"u\" name=\"flags\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"remote\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"installation\" direction=\"in\"/>"
	"      <arg type=\"ay\" name=\"summary_path\" direction=\"in\"/>"
	"      <arg type=\"ay\" name=\"summary_sig_path\" direction=\"in\"/>"
	"    </method>"
	"    <method name=\"RemoveLocalRef\">"
	"      <arg type=\"u\" name=\"flags\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"remote\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"ref\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"installation\" direction=\"in\"/>"
	"    </method>"
	"    <method name=\"PruneLocalRepo\">"
	"      <arg type=\"u\" name=\"flags\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"installation\" direction=\"in\"/>"
	"    </method>"
	"    <method name=\"RunTriggers\">"
	"      <arg type=\"u\" name=\"flags\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"installation\" direction=\"in\"/>"
	"    </method>"
	"    <method name=\"EnsureRepo\">"
	"      <arg type=\"u\" name=\"flags\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"installation\" direction=\"in\"/>"
	"    </method>"
	"    <method name=\"UpdateSummary\">"
	"      <arg type=\"u\" name=\"flags\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"installation\" direction=\"in\"/>"
	"    </method>"
	"    <method name=\"GenerateOciSummary\">"
	"      <arg type=\"u\" name=\"flags\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"origin\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"installation\" direction=\"in\"/>"
	"    </method>"
	"    <method name=\"CancelPull\">"
	"      <arg type=\"u\" name=\"flags\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"installation\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"src_dir\" direction=\"in\"/>"
	"    </method>"
	"    <method name=\"GetRevokefsFd\">"
	"      <arg type=\"u\" name=\"flags\" direction=\"in\"/>"
	"      <arg type=\"s\" name=\"installation\" direction=\"in\"/>"
	"      <arg type=\"h\" name=\"fd_index\" direction=\"out\"/>"
	"      <arg type=\"s\" name=\"src_dir\" direction=\"out\"/>"
	"    </method>"
	"  </interface>"
	"</node>";

static void
syshelper_method_call(GDBusConnection* connection, const gchar* sender,
		      const gchar* object_path, const gchar* interface_name,
		      const gchar* method_name, GVariant* parameters,
		      GDBusMethodInvocation* invocation, gpointer user_data) {
	if (g_str_equal(method_name, "DeployAppstream"))
		handle_deploy_appstream(connection, sender, object_path,
					interface_name, method_name, parameters,
					invocation, user_data);
	else if (g_str_equal(method_name, "Deploy"))
		handle_deploy(connection, sender, object_path, interface_name,
			      method_name, parameters, invocation, user_data);
	else if (g_str_equal(method_name, "Uninstall"))
		handle_uninstall(connection, sender, object_path,
				 interface_name, method_name, parameters,
				 invocation, user_data);
	else if (g_str_equal(method_name, "InstallBundle"))
		handle_install_bundle(connection, sender, object_path,
				      interface_name, method_name, parameters,
				      invocation, user_data);
	else if (g_str_equal(method_name, "EnsureRepo"))
		handle_ensure_repo(connection, sender, object_path,
				   interface_name, method_name, parameters,
				   invocation, user_data);
	else if (g_str_equal(method_name, "PruneLocalRepo"))
		handle_prune_local_repo(connection, sender, object_path,
					interface_name, method_name, parameters,
					invocation, user_data);
	else if (g_str_equal(method_name, "RemoveLocalRef"))
		handle_remove_local_ref(connection, sender, object_path,
					interface_name, method_name, parameters,
					invocation, user_data);
	else if (g_str_equal(method_name, "RunTriggers"))
		handle_run_triggers(connection, sender, object_path,
				    interface_name, method_name, parameters,
				    invocation, user_data);
	else if (g_str_equal(method_name, "UpdateSummary"))
		handle_update_summary(connection, sender, object_path,
				      interface_name, method_name, parameters,
				      invocation, user_data);
	else if (g_str_equal(method_name, "Configure"))
		handle_configure(connection, sender, object_path,
				 interface_name, method_name, parameters,
				 invocation, user_data);
	else if (g_str_equal(method_name, "ConfigureRemote"))
		handle_configure_remote(connection, sender, object_path,
					interface_name, method_name, parameters,
					invocation, user_data);
	else if (g_str_equal(method_name, "UpdateRemote"))
		handle_update_remote(connection, sender, object_path,
				     interface_name, method_name, parameters,
				     invocation, user_data);
	else if (g_str_equal(method_name, "CancelPull"))
		g_dbus_method_invocation_return_value(invocation, NULL);
	else
		handle_unimplemented(connection, sender, object_path,
				     interface_name, method_name, parameters,
				     invocation, user_data);
}

static const GDBusInterfaceVTable syshelper_vtable = {
	.method_call = syshelper_method_call,
	.get_property = syshelper_get_property,
	.set_property = NULL,
};

static void on_bus_acquired(GDBusConnection* connection, const gchar* name,
			    gpointer user_data) {
	GDBusNodeInfo* info;
	GError* error = NULL;

	system_bus = connection;

	info = g_dbus_node_info_new_for_xml(syshelper_xml, &error);
	if (info == NULL) {
		fpkg_errorf("system helper interface XML: %s", error->message);
		g_error_free(error);
		return;
	}
	if (g_dbus_connection_register_object(
		    connection, SYSHELPER_PATH, info->interfaces[0],
		    &syshelper_vtable, NULL, NULL, &error) == 0) {
		fpkg_errorf("cannot register %s: %s", SYSHELPER_PATH,
			    error->message);
		g_error_free(error);
	} else {
		fpkg_infof("owning %s on the system bus (version %u)", name,
			   SYSHELPER_VERSION);
	}
	g_dbus_node_info_unref(info);
}

static void on_name_lost(GDBusConnection* connection, const gchar* name,
			 gpointer user_data) {
	dprintf(2,
		"flatpkg system-helper: cannot own %s on the "
		"system bus\n",
		name);
	_exit(1);
}

int system_helper_main(int argc, char* argv[]) {
	GMainLoop* loop;


	loop = g_main_loop_new(NULL, FALSE);
	g_bus_own_name(G_BUS_TYPE_SYSTEM, SYSHELPER_BUS_NAME,
		       G_BUS_NAME_OWNER_FLAGS_NONE, on_bus_acquired, NULL,
		       on_name_lost, NULL, NULL);
	g_main_loop_run(loop);
	return 0;
}
