#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dbus/dbus.h>

#include "flatpkg.h"
#define FPKG_MODULE "portal"
#include "fpkglog.h"
#include "portal_identity.h"

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

void instance_base(char* out, size_t n) {
	snprintf(out, n, "%s", session_instances_dir());
}

char** snapshot_instances(void) {
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

bool in_snapshot(char** snap, const char* id) {
	if (!snap) return false;
	for (int i = 0; snap[i]; i++)
		if (strcmp(snap[i], id) == 0) return true;
	return false;
}

void free_snapshot(char** snap) {
	if (!snap) return;
	for (int i = 0; snap[i]; i++) free(snap[i]);
	free(snap);
}

int read_instance_info(const char* path, char* app_out, size_t app_n,
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

static char* app_id_from_jail(int jid) { return instance_app_for_jid(jid); }

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

char* resolve_app_id(DBusConnection* conn, const char* sender) {
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
