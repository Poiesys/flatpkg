#include <assert.h>
#include <dirent.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/user.h>
#include <sys/wait.h>

#include <libutil.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "flatpkg.h"
#define FPKG_MODULE "dbus"
#include "fpkglog.h"

static const char* proxy_candidates[] = {
	"/usr/local/bin/xdg-dbus-proxy",
	"/usr/bin/xdg-dbus-proxy",
	NULL,
};

const char* dbus_proxy_binary(void) {
	const char* env = cli_getenv("FLATPAK_DBUSPROXY");
	if (env && *env && access(env, X_OK) == 0) return env;
	for (int i = 0; proxy_candidates[i]; i++) {
		if (access(proxy_candidates[i], X_OK) == 0)
			return proxy_candidates[i];
	}
	return NULL;
}

bool dbus_proxy_is_path(const char* path) {
	const char* env = cli_getenv("FLATPAK_DBUSPROXY");

	if (path == NULL || path[0] == '\0') return false;
	if (env && *env && strcmp(path, env) == 0) return true;
	for (int i = 0; proxy_candidates[i]; i++) {
		if (strcmp(path, proxy_candidates[i]) == 0) return true;
	}
	return false;
}

static char* proc_pathname(pid_t pid) {
	size_t len = 1024;
	for (;;) {
		char* buf = malloc(len);
		if (!buf) return NULL;
		int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME,
			      (int)pid};
		size_t got = len;
		if (sysctl(mib, 4, buf, &got, NULL, 0) == 0) {
			if (got > 0 && buf[got - 1] == '\0') got--;
			buf[got] = '\0';
			return buf;
		}
		free(buf);
		if (errno == ENOMEM && len < (1u << 16)) {
			len *= 2;
			continue;
		}
		return NULL;
	}
}

fp_caller_t flatpkg_caller_check(pid_t pid, int* jid_out, char** path_out) {
	struct kinfo_proc* kp;
	char* exe;

	if (jid_out != NULL) *jid_out = 0;
	if (path_out != NULL) *path_out = NULL;

	kp = kinfo_getproc(pid);
	if (kp != NULL) {
		int jid = kp->ki_jid;
		free(kp);
		if (jid > 0) {
			if (jid_out != NULL) *jid_out = jid;
			return FP_CALLER_JAIL;
		}
	}

	exe = proc_pathname(pid);
	if (exe == NULL)
		// ESRCH or EPERM: a caller we cannot identify is not one we
		// serve.
		return FP_CALLER_UNREADABLE;
	bool is_proxy = dbus_proxy_is_path(exe);
	if (path_out != NULL) *path_out = exe;
	else
		free(exe);
	return is_proxy ? FP_CALLER_PROXY : FP_CALLER_NO;
}

char* fp_wayland_display_name(const char* xdg_runtime_dir) {
	const char* wl = cli_getenv("WAYLAND_DISPLAY");
	if (wl && *wl && wl[0] != '/') return xstrdup(wl);

	if (xdg_runtime_dir && *xdg_runtime_dir) {
		DIR* d = opendir(xdg_runtime_dir);
		if (d) {
			struct dirent* de;
			while ((de = readdir(d)) != NULL) {
				const char* nm = de->d_name;
				size_t len = strlen(nm);
				if (strncmp(nm, "wayland-", 8) != 0 || len <= 8)
					continue;
				bool digits = true;
				for (size_t i = 8; i < len; i++)
					if (nm[i] < '0' || nm[i] > '9')
						digits = false;
				if (!digits) continue;
				char p[PATH_MAX];
				struct stat st;
				snprintf(p, sizeof(p), "%s/%s", xdg_runtime_dir,
					 nm);
				if (stat(p, &st) == 0 && S_ISSOCK(st.st_mode)) {
					closedir(d);
					return xstrdup(nm);
				}
			}
			closedir(d);
		}
	}
	return NULL;
}
