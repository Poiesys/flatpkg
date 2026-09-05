#include <assert.h>
#include <dirent.h>
#include <jail.h>
#include <pwd.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/jail.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <gio/gio.h>
#include <glib.h>

#include <errno.h>
#include <fcntl.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "flatpkg.h"
#include "cli.h"
#define FPKG_MODULE "instance"
#include "fpkglog.h"

// Read side of the instance registry: listing, completion and usage checks.

int kf_get(const char* path, const char* group, const char* key, char* out,
	   size_t outlen) {
	assert(path != NULL);
	assert(group != NULL);
	assert(key != NULL);
	assert(out != NULL);
	assert(outlen > 0);
	FILE* fp = fopen(path, "r");
	if (!fp) return -1;
	char line[1024];
	char cur[128] = "";
	int found = -1;
	while (fgets(line, sizeof(line), fp)) {
		char* p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '#' || *p == '\0' || *p == '\n') continue;
		if (*p == '[') {
			char* end = strchr(p, ']');
			if (!end) continue;
			*end = '\0';
			snprintf(cur, sizeof(cur), "%s", p + 1);
			continue;
		}
		char* eq = strchr(p, '=');
		if (!eq) continue;
		*eq = '\0';
		char* k = p;
		char* v = eq + 1;
		char* ke = k + strlen(k);
		while (ke > k && (ke[-1] == ' ' || ke[-1] == '\t'))
			*--ke = '\0';
		while (*v == ' ' || *v == '\t') v++;
		char* ve = v + strlen(v);
		while (ve > v && (ve[-1] == '\n' || ve[-1] == '\r' ||
				  ve[-1] == ' ' || ve[-1] == '\t'))
			*--ve = '\0';
		if (strcmp(cur, group) == 0 && strcmp(k, key) == 0) {
			snprintf(out, outlen, "%s", v);
			found = 0;
			break;
		}
	}
	fclose(fp);
	return found;
}

char** instance_load_run_environ(const char* inst_dir) {
	char path[PATH_MAX];
	char* buf = NULL;
	gsize len = 0;
	GPtrArray* out;

	if (inst_dir == NULL || inst_dir[0] == '\0') return NULL;
	snprintf(path, sizeof(path), "%s-private/run-environ", inst_dir);
	if (!g_file_get_contents(path, &buf, &len, NULL)) return NULL;

	out = g_ptr_array_new_with_free_func(g_free);
	for (gsize off = 0; off < len;) {
		const char* entry = buf + off;
		gsize n = strlen(entry);

		if (n > 0) {
			if (memchr(entry, '=', n) != NULL)
				g_ptr_array_add(out, g_strdup(entry));
		}
		off += n + 1;
	}
	g_free(buf);
	g_ptr_array_add(out, NULL);
	return (char**)g_ptr_array_free(out, FALSE);
}

bool dir_has_live_instance(const char* base) {
	assert(base != NULL);
	char dir[PATH_MAX], infof[PATH_MAX], jidbuf[64];
	DIR* d = opendir(base);
	if (!d) return false;
	bool found = false;
	struct dirent* de;
	while (!found && (de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.') continue;
		bool numeric = de->d_name[0] != '\0';
		for (const char* c = de->d_name; *c; c++) {
			if (*c < '0' || *c > '9') {
				numeric = false;
				break;
			}
		}
		if (!numeric) continue;
		snprintf(dir, sizeof(dir), "%s/%s", base, de->d_name);
		struct stat st;
		if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
		snprintf(infof, sizeof(infof), "%s/info", dir);
		if (kf_get(infof, "Instance", "jid", jidbuf, sizeof(jidbuf)) !=
		    0)
			continue;
		int jid = atoi(jidbuf);
		if (jid > 0 && instance_alive(dir, jid)) found = true;
	}
	closedir(d);
	return found;
}

static bool path_prefix_matches(const char* value, const char* path) {
	assert(value != NULL);
	assert(path != NULL);
	size_t n = strlen(path);
	if (n == 0 || strncmp(value, path, n) != 0) return false;
	return value[n] == '\0' || value[n] == '/' ||
	       strchr(value, '=') != NULL;
}

static bool instance_info_uses(const char* infof, const char* path) {
	assert(infof != NULL);
	assert(path != NULL);
	char buf[4096];

	if (kf_get(infof, "Instance", "app-path", buf, sizeof(buf)) == 0 &&
	    path_prefix_matches(buf, path))
		return true;
	if (kf_get(infof, "Instance", "runtime-path", buf, sizeof(buf)) == 0 &&
	    path_prefix_matches(buf, path))
		return true;
	for (int i = 0; i < 2; i++) {
		const char* key =
			(i == 0) ? "app-extensions" : "runtime-extensions";
		if (kf_get(infof, "Instance", key, buf, sizeof(buf)) != 0)
			continue;
		for (char* p = buf; (p = strstr(p, path)) != NULL;
		     p += strlen(path)) {
			char before = (p == buf) ? '/' : p[-1];
			char after = p[strlen(path)];
			if (before == '=' &&
			    (after == '\0' || after == ';' || after == '/'))
				return true;
		}
	}
	return false;
}

static void collect_runtime_bases(const char* parent, char out[][PATH_MAX],
				  int* n, int max) {
	assert(parent != NULL);
	assert(out != NULL);
	assert(n != NULL);
	DIR* d = opendir(parent);
	if (d == NULL) return;
	struct dirent* de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.') continue;
		char p[PATH_MAX];
		if ((size_t)snprintf(p, sizeof(p), "%s/%s", parent,
				     de->d_name) >= sizeof(p))
			continue;
		struct stat st;
		if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
		if (*n >= max) break;
		snprintf(out[(*n)++], PATH_MAX, "%s/.flatpak", p);
	}
	closedir(d);
}

int instance_uses_path(const char* path) {
	assert(path != NULL);
	if (path == NULL || path[0] == '\0') return 0;

	char bases[64][PATH_MAX];
	int n = 0;
	const char* xdg = cli_getenv("XDG_RUNTIME_DIR");
	if (xdg != NULL && *xdg != '\0')
		snprintf(bases[n++], PATH_MAX, "%s/.flatpak", xdg);

	uid_t uid = getuid();
	const char* app_uid = cli_getenv("FLATPKG_APP_UID");
	if (app_uid != NULL && *app_uid != '\0')
		uid = (uid_t)strtoul(app_uid, NULL, 10);
	snprintf(bases[n++], PATH_MAX, "/var/run/user/%u/.flatpak",
		 (unsigned)uid);

	collect_runtime_bases("/var/run/user", bases, &n, 64);
	collect_runtime_bases("/var/run/xdg", bases, &n, 64);

	for (int b = 0; b < n; b++) {
		DIR* d = opendir(bases[b]);
		if (d == NULL) continue;
		struct dirent* de;
		while ((de = readdir(d)) != NULL) {
			if (de->d_name[0] < '0' || de->d_name[0] > '9')
				continue;
			char dir[PATH_MAX], infof[PATH_MAX], jidbuf[64];
			if ((size_t)snprintf(dir, sizeof(dir), "%s/%s",
					     bases[b],
					     de->d_name) >= sizeof(dir))
				continue;
			struct stat st;
			if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode))
				continue;
			snprintf(infof, sizeof(infof), "%s/info", dir);
			if (kf_get(infof, "Instance", "jid", jidbuf,
				   sizeof(jidbuf)) != 0)
				continue;
			int jid = atoi(jidbuf);
			if (jid <= 0 || !instance_alive(dir, jid)) continue;
			if (instance_info_uses(infof, path)) {
				closedir(d);
				return 1;
			}
		}
		closedir(d);
	}
	return 0;
}

const char* instances_dir(void) {
	static char envbuf[PATH_MAX];
	static char fallback[PATH_MAX];
	const char* xdg = cli_getenv("XDG_RUNTIME_DIR");

	uid_t uid = getuid();
	const char* app_uid = cli_getenv("FLATPKG_APP_UID");
	if (app_uid != NULL && *app_uid != '\0')
		uid = (uid_t)strtoul(app_uid, NULL, 10);

	snprintf(fallback, sizeof(fallback), "/var/run/user/%u/.flatpak",
		 (unsigned)uid);
	if (xdg == NULL || *xdg == '\0') return fallback;

	snprintf(envbuf, sizeof(envbuf), "%s/.flatpak", xdg);
	if (dir_has_live_instance(envbuf)) return envbuf;
	if (dir_has_live_instance(fallback)) return fallback;
	return envbuf;
}

int jail_exists(int jid) {
	assert(jid > 0);
	char jidstr[16];
	snprintf(jidstr, sizeof(jidstr), "%d", jid);
	pid_t pid = fork();
	if (pid < 0) return 0;
	if (pid == 0) {
		char* argv[] = {"/usr/sbin/jls", "-q", "-j", jidstr, NULL};
		int devnull = open("/dev/null", O_WRONLY);

		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO) close(devnull);
		}
		execv(argv[0], argv);
		_exit(127);
	}
	int status = 0;
	if (waitpid(pid, &status, 0) < 0) return 0;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int instance_alive(const char* dir, int jid) {
	assert(dir != NULL);
	char p[PATH_MAX], buf[32];
	FILE* fp;
	int pid = 0;

	if (jid > 0 && jail_exists(jid)) return 1;

	snprintf(p, sizeof(p), "%s/pid", dir);
	fp = fopen(p, "r");
	if (fp) {
		if (fgets(buf, sizeof(buf), fp)) pid = atoi(buf);
		fclose(fp);
	}
	if (pid > 0) return kill(pid, 0) == 0;
	return 0;
}

void instance_complete_list(void) {
	const char* base = instances_dir();
	DIR* d = opendir(base);
	if (!d) return;

	struct dirent* de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.') continue;
		bool numeric = de->d_name[0] != '\0';
		for (const char* c = de->d_name; *c; c++) {
			if (*c < '0' || *c > '9') {
				numeric = false;
				break;
			}
		}
		if (!numeric) continue;

		char dir[PATH_MAX], infof[PATH_MAX], jidbuf[64] = "";
		snprintf(dir, sizeof(dir), "%s/%s", base, de->d_name);
		struct stat st;
		if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
		snprintf(infof, sizeof(infof), "%s/info", dir);
		if (kf_get(infof, "Instance", "jid", jidbuf, sizeof(jidbuf)) !=
		    0)
			continue;
		int jid = atoi(jidbuf);
		if (jid <= 0 || !instance_alive(dir, jid)) continue;

		fp_outl("%s", de->d_name);
		fp_outl("%d", jid);
	}
	closedir(d);
}

int cmd_ps(const char* columns) {
	// columns is optional (NULL = default table).
	const char* base = instances_dir();
	DIR* d = opendir(base);
	if (!d) {
		if (columns == NULL) fpkg_infof("no running instances");
		return 0;
	}

	const char* headers[] = {"Instance", "PID", "Application", "Jail",
				 "Root"};
	enum { PS_ROWS_MAX = 512 };
	char* cells[PS_ROWS_MAX][5];
	const char** rows[PS_ROWS_MAX];
	size_t m = 0;
	bool truncated = false;

	struct dirent* de;
	while ((de = readdir(d)) != NULL) {
		if (m == PS_ROWS_MAX) {
			truncated = true;
			break;
		}
		if (de->d_name[0] == '.') continue;
		size_t nlen = strlen(de->d_name);
		if (nlen >= 8 && strcmp(de->d_name + nlen - 8, "-private") == 0)
			continue;
		bool numeric = de->d_name[0] != '\0';
		for (const char* c = de->d_name; *c; c++) {
			if (*c < '0' || *c > '9') {
				numeric = false;
				break;
			}
		}
		if (!numeric) continue;

		char dir[PATH_MAX];
		snprintf(dir, sizeof(dir), "%s/%s", base, de->d_name);
		struct stat st;
		if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) continue;

		char infof[PATH_MAX];
		snprintf(infof, sizeof(infof), "%s/info", dir);
		char name[256] = "", jidbuf[64] = "", root[PATH_MAX] = "",
		     pidbuf[64] = "";
		kf_get(infof, "Application", "name", name, sizeof(name));
		if (!name[0])
			kf_get(infof, "Runtime", "name", name, sizeof(name));
		kf_get(infof, "Instance", "jid", jidbuf, sizeof(jidbuf));
		kf_get(infof, "Instance", "path", root, sizeof(root));
		int jid = atoi(jidbuf);
		if (jid <= 0) continue;
		if (!instance_alive(dir, jid)) continue;

		char pidf[PATH_MAX];
		snprintf(pidf, sizeof(pidf), "%s/pid", dir);
		FILE* pf = fopen(pidf, "r");
		if (pf) {
			if (fgets(pidbuf, sizeof(pidbuf), pf))
				pidbuf[strcspn(pidbuf, "\n")] = '\0';
			fclose(pf);
		}

		cells[m][0] = strdup(de->d_name);
		cells[m][1] = strdup(pidbuf[0] ? pidbuf : "-");
		cells[m][2] = strdup(name[0] ? name : "(unknown)");
		cells[m][3] = strdup(jidbuf);
		cells[m][4] = strdup(root[0] ? root : "?");
		rows[m] = (const char**)cells[m];
		m++;
	}
	closedir(d);

	if (columns != NULL && *columns != '\0') {
		static const char* all_names[5] = {
			"instance", "pid", "application", "jail", "root"};
		int sel[32];
		int nsel = 0;
		const char* p = columns;
		while (*p) {
			const char* comma = strchr(p, ',');
			size_t len = comma ? (size_t)(comma - p) : strlen(p);
			if (len == 0) {
				p = comma ? comma + 1 : p + len;
				continue;
			}
			if (nsel >= (int)(sizeof(sel) / sizeof(sel[0]))) {
				fpkg_errorf(
					"too many ps columns (max %d)",
					(int)(sizeof(sel) / sizeof(sel[0])));
				for (size_t i = 0; i < m; i++)
					for (int c = 0; c < 5; c++)
						free(cells[i][c]);
				return 1;
			}
			int idx = -1;
			for (int c = 0; c < 5; c++)
				if (strlen(all_names[c]) == len &&
				    strncmp(all_names[c], p, len) == 0) {
					idx = c;
					break;
				}
			if (idx < 0) {
				fpkg_errorf(
					"unknown ps column \"%.*s\" "
					"(instance, pid, application, jail, "
					"root)",
					(int)len, p);
				for (size_t i = 0; i < m; i++)
					for (int c = 0; c < 5; c++)
						free(cells[i][c]);
				return 1;
			}
			sel[nsel++] = idx;
			if (!comma) break;
			p = comma + 1;
		}
		for (size_t i = 0; i < m; i++) {
			for (int s = 0; s < nsel; s++)
				fp_outl("%s%s", cells[i][sel[s]],
					s + 1 < nsel ? "\t" : "");
			if (nsel == 0) fp_outl("%s", "");
		}
		for (size_t i = 0; i < m; i++)
			for (int c = 0; c < 5; c++) free(cells[i][c]);
		return 0;
	}

	if (m > 0) {
		fp_print_table(headers, 5, (const char* const* const*)rows, m,
			       FP_TABLE_OUTPUT);
	} else {
		fpkg_infof("no running instances");
	}
	if (truncated)
		fpkg_warnf("more than %d instances; not all are listed",
			   PS_ROWS_MAX);
	for (size_t i = 0; i < m; i++)
		for (int c = 0; c < 5; c++) free(cells[i][c]);
	return 0;
}
