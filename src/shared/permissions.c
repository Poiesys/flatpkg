// Where flatpak emits bwrap args, flatpkg emits fs_grant_t entries.
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include "flatpkg.h"
#define FPKG_MODULE "permissions"
#include "fpkglog.h"

// Create the host-side ~/.var/app/<id> tree. Must run as the invoking user,
// so nothing under the user's home is ever root-owned.
int permissions_ensure_data_dirs(const char* app_data_dir) {
	assert(app_data_dir != NULL);
	static const char* subdirs[] = {
		"data",	     "cache",  "cache/fontconfig",
		"cache/tmp", "config", ".local/state",
		NULL,
	};
	if (mkdir_p(app_data_dir, 0755) != 0) return -1;
	for (size_t i = 0; subdirs[i]; i++) {
		char p[PATH_MAX];
		snprintf(p, sizeof(p), "%s/%s", app_data_dir, subdirs[i]);
		if (mkdir_p(p, 0755) != 0) return -1;
	}
	{
		const char* bn = strrchr(app_data_dir, '/');
		bn = bn ? bn + 1 : app_data_dir;
		char nested[PATH_MAX];
		snprintf(nested, sizeof(nested), "%s/.var/app/%s", app_data_dir,
			 bn);
		if (mkdir_p(nested, 0755) != 0) return -1;
		for (size_t i = 0; subdirs[i]; i++) {
			char p[PATH_MAX];
			snprintf(p, sizeof(p), "%s/%s", nested, subdirs[i]);
			if (mkdir_p(p, 0755) != 0) return -1;
		}
	}
	return 0;
}

typedef struct {
	char* path;
	fs_grant_mode_t mode;
	bool negated;
} fs_token_t;

static int parse_fs_token(const char* entry, fs_token_t* tok, char* buf,
			  size_t buflen) {
	assert(entry != NULL);
	assert(tok != NULL);
	assert(buf != NULL);
	assert(buflen > 0);
	memset(tok, 0, sizeof(*tok));
	tok->mode = FS_GRANT_RW;

	const char* p = entry;
	if (*p == '!') {
		tok->negated = true;
		p++;
	}

	char tmp[4096] = {0};
	size_t n = 0;
	while (*p && *p != ':') {
		if (*p == '\\' && (p[1] == ':' || p[1] == '\\')) p++;
		if (n + 1 < sizeof(tmp)) tmp[n++] = *p;
		p++;
	}
	tmp[n] = '\0';

	if (*p == ':') {
		const char* suffix = p + 1;
		if (strcmp(suffix, "ro") == 0) tok->mode = FS_GRANT_RO;
		else if (strcmp(suffix, "rw") == 0)
			tok->mode = FS_GRANT_RW;
		else if (strcmp(suffix, "create") == 0)
			tok->mode = FS_GRANT_CREATE;
		else if (*suffix != '\0')
			snprintf(buf, buflen,
				 "ignoring filesystem suffix ':%s'", suffix);
	}

	if (strstr(tmp, "/../") != NULL ||
	    (strlen(tmp) >= 3 && strcmp(tmp + strlen(tmp) - 3, "/..") == 0)) {
		snprintf(buf, buflen,
			 "filesystem location '%s' contains '..', skipping",
			 entry);
		return -1;
	}

	for (char* s = tmp; (s = strchr(s, '/'));) {
		while (s[1] == '/') {
			memmove(s + 1, s + 2, strlen(s + 2) + 1);
		}
		if (s[1] == '.' && s[2] == '/') {
			memmove(s + 1, s + 3, strlen(s + 3) + 1);
		} else {
			s++;
		}
	}
	while (strlen(tmp) > 1) {
		size_t len = strlen(tmp);
		if (len < 1) break;
		if (tmp[len - 1] == '/') {
			tmp[len - 1] = '\0';
		} else if (len >= 2 && tmp[len - 1] == '.' &&
			   tmp[len - 2] == '/') {
			tmp[len - 2] = '\0';
		} else {
			break;
		}
	}

	if (strcmp(tmp, "/") == 0) {
		snprintf(buf, buflen,
			 "--filesystem=/ is not available; use 'host' instead "
			 "(skipping '%s')",
			 entry);
		return -1;
	}

	tok->path = xstrdup(tmp);
	return 0;
}

static const char* xdg_env_or(const char* env, const char* fallback) {
	assert(env != NULL);
	const char* v = cli_getenv(env);
	return (v && *v) ? v : fallback;
}

static int user_dirs_lookup(const char* home, const char* key, char* out,
			    size_t outlen) {
	assert(home != NULL);
	assert(key != NULL);
	assert(out != NULL);
	assert(outlen > 0);
	char path[4096];
	snprintf(path, sizeof(path), "%s/.config/user-dirs.dirs", home);
	FILE* fp = fopen(path, "r");
	if (!fp) return -1;
	char line[1024];
	int found = 0;
	while (fgets(line, sizeof(line), fp)) {
		if (strncmp(line, key, strlen(key)) != 0) continue;
		char* eq = strchr(line, '=');
		if (!eq) continue;
		char* v = eq + 1;
		char* q1 = strchr(v, '"');
		char* q2 = q1 ? strrchr(v, '"') : NULL;
		if (q1 && q2 && q2 > q1) {
			*q2 = '\0';
			v = q1 + 1;
		}
		if (strncmp(v, "$HOME", 5) == 0 && v[5] == '/') {
			snprintf(out, outlen, "%s%s", home, v + 5);
		} else {
			snprintf(out, outlen, "%s", v);
		}
		found = 1;
		break;
	}
	fclose(fp);
	return found ? 0 : -1;
}

static int resolve_xdg(const char* home, const char* token, char* out,
		       size_t outlen) {
	assert(home != NULL);
	assert(token != NULL);
	assert(out != NULL);
	assert(outlen > 0);
	static const struct {
		const char* prefix;
		const char* env;
		const char* fallback_sub;
		const char* dirs_key;
	} table[] = {
		{"xdg-desktop", NULL, "Desktop", "XDG_DESKTOP_DIR"},
		{"xdg-documents", NULL, "Documents", "XDG_DOCUMENTS_DIR"},
		{"xdg-download", NULL, "Downloads", "XDG_DOWNLOAD_DIR"},
		{"xdg-music", NULL, "Music", "XDG_MUSIC_DIR"},
		{"xdg-pictures", NULL, "Pictures", "XDG_PICTURES_DIR"},
		{"xdg-public-share", NULL, "Public", "XDG_PUBLICSHARE_DIR"},
		{"xdg-templates", NULL, "Templates", "XDG_TEMPLATES_DIR"},
		{"xdg-videos", NULL, "Videos", "XDG_VIDEOS_DIR"},
		{"xdg-config", "XDG_CONFIG_HOME", ".config", NULL},
		{"xdg-cache", "XDG_CACHE_HOME", ".cache", NULL},
		{"xdg-data", "XDG_DATA_HOME", ".local/share", NULL},
		{NULL, NULL, NULL, NULL},
	};

	const char* slash = strchr(token, '/');
	size_t plen = slash ? (size_t)(slash - token) : strlen(token);
	const char* rest = slash ? slash : token + plen;
	while (*rest == '/') rest++;

	char prefix[64];
	snprintf(prefix, sizeof(prefix), "%.*s",
		 (int)(plen < sizeof(prefix) - 1 ? plen : sizeof(prefix) - 1),
		 token);

	if (strcmp(prefix, "xdg-run") == 0) {
		if (*rest == '\0') return -1;
		const char* rundir = cli_getenv("XDG_RUNTIME_DIR");
		if (!rundir || !*rundir) {
			static char fb[64];
			snprintf(fb, sizeof(fb), "/var/run/user/%d", getuid());
			rundir = fb;
		}
		snprintf(out, outlen, "%s/%s", rundir, rest);
		return 0;
	}

	for (size_t i = 0; table[i].prefix; i++) {
		if (strcmp(prefix, table[i].prefix) != 0) continue;
		char base[4096];
		if (table[i].env) {
			snprintf(base, sizeof(base), "%s",
				 xdg_env_or(table[i].env, ""));
			if (base[0] == '\0')
				snprintf(base, sizeof(base), "%s/%s", home,
					 table[i].fallback_sub);
		} else if (table[i].dirs_key) {
			if (user_dirs_lookup(home, table[i].dirs_key, base,
					     sizeof(base)) != 0)
				snprintf(base, sizeof(base), "%s/%s", home,
					 table[i].fallback_sub);
		} else {
			snprintf(base, sizeof(base), "%s/%s", home,
				 table[i].fallback_sub);
		}

		if (strcmp(base, home) == 0) return -1;

		if (*rest) snprintf(out, outlen, "%s/%s", base, rest);
		else
			snprintf(out, outlen, "%s", base);
		return 0;
	}

	return -1;
}

static void plan_add_grant(sandbox_plan_t* plan, const char* host_path,
			   const char* jail_path, fs_grant_mode_t mode,
			   bool persistent) {
	assert(plan != NULL);
	assert(jail_path != NULL);
	for (size_t i = 0; i < plan->ngrants; i++) {
		fs_grant_t* g = &plan->grants[i];
		if (strcmp(g->jail_path, jail_path) != 0) continue;
		if (mode == FS_GRANT_HIDE) {
			g->mode = FS_GRANT_HIDE;
			free(g->host_path);
			g->host_path = NULL;
			g->persistent = false;
		} else if (g->mode != FS_GRANT_HIDE &&
			   (int)mode > (int)g->mode) {
			g->mode = mode;
			free(g->host_path);
			g->host_path = xstrdup(host_path);
			g->persistent = persistent;
		}
		return;
	}

	plan->ngrants++;
	fs_grant_t* new_grants =
		realloc(plan->grants, plan->ngrants * sizeof(fs_grant_t));
	if (!new_grants) {
		fpkg_errorf("out of memory");
		exit(1);
	}
	plan->grants = new_grants;
	fs_grant_t* g = &plan->grants[plan->ngrants - 1];
	memset(g, 0, sizeof(*g));
	g->host_path = host_path ? xstrdup(host_path) : NULL;
	g->jail_path = xstrdup(jail_path);
	g->mode = mode;
	g->persistent = persistent;
}

static void expand_fs_entry(sandbox_plan_t* plan, const char* entry) {
	assert(plan != NULL);
	assert(entry != NULL);
	char msg[512] = "";
	fs_token_t tok;
	if (parse_fs_token(entry, &tok, msg, sizeof(msg)) != 0) {
		if (msg[0]) fpkg_warnf("%s", msg);
		return;
	}
	const char* home = plan->home_dir;
	char host_path[4096], jail_path[4096];

	if (tok.negated) {
		if (strcmp(tok.path, "host") == 0 ||
		    strcmp(tok.path, "host-os") == 0 ||
		    strcmp(tok.path, "host-etc") == 0 ||
		    strcmp(tok.path, "host-root") == 0) {
			const char* targets[] = {"/run/host", "/run/host/usr",
						 "/run/host/etc",
						 "/run/host/root", NULL};
			for (int i = 0; targets[i]; i++) {
				for (size_t j = 0; j < plan->ngrants; j++) {
					if (strcmp(plan->grants[j].jail_path,
						   targets[i]) == 0) {
						plan->grants[j].mode =
							FS_GRANT_HIDE;
						free(plan->grants[j].host_path);
						plan->grants[j].host_path =
							NULL;
					}
				}
			}
		} else if (strncmp(tok.path, "~/", 2) == 0) {
			snprintf(jail_path, sizeof(jail_path), "%s%s", home,
				 tok.path + 1);
			plan_add_grant(plan, NULL, jail_path, FS_GRANT_HIDE,
				       false);
		} else if (tok.path[0] == '/') {
			plan_add_grant(plan, NULL, tok.path, FS_GRANT_HIDE,
				       false);
		} else if (strncmp(tok.path, "xdg-", 4) == 0) {
			if (resolve_xdg(home, tok.path, host_path,
					sizeof(host_path)) == 0)
				plan_add_grant(plan, NULL, host_path,
					       FS_GRANT_HIDE, false);
			else
				fpkg_warnf("cannot negate an unsupported or "
					   "disabled xdg dir '%s', skipping",
					   tok.path);
		} else {
			fpkg_warnf("cannot negate filesystem '%s', skipping",
				   entry);
		}
		free(tok.path);
		return;
	}

	if (strcmp(tok.path, "host") == 0) {
		plan_add_grant(plan, "/", "/run/host", tok.mode, false);
	} else if (strcmp(tok.path, "host-os") == 0) {
		plan_add_grant(plan, "/usr", "/run/host/usr", FS_GRANT_RO,
			       false);
	} else if (strcmp(tok.path, "host-etc") == 0) {
		plan_add_grant(plan, "/etc", "/run/host/etc", FS_GRANT_RO,
			       false);
	} else if (strcmp(tok.path, "host-root") == 0) {
		plan_add_grant(plan, "/", "/run/host/root", FS_GRANT_RO, false);
	} else if (strcmp(tok.path, "host-reset") == 0) {
		const char* targets[] = {"/run/host", "/run/host/usr",
					 "/run/host/etc", "/run/host/root",
					 NULL};
		for (int i = 0; targets[i]; i++) {
			for (size_t j = 0; j < plan->ngrants; j++) {
				if (strcmp(plan->grants[j].jail_path,
					   targets[i]) == 0) {
					free(plan->grants[j].host_path);
					free(plan->grants[j].jail_path);
					plan->grants[j] =
						plan->grants[--plan->ngrants];
					j--;
				}
			}
		}
	} else if (strcmp(tok.path, "home") == 0 ||
		   strcmp(tok.path, "~") == 0) {
		plan_add_grant(plan, home, home, tok.mode, false);
	} else if (strncmp(tok.path, "home/", 5) == 0) {
		snprintf(jail_path, sizeof(jail_path), "%s/%s", home,
			 tok.path + 5);
		plan_add_grant(plan, jail_path, jail_path, tok.mode, false);
	} else if (strncmp(tok.path, "~/", 2) == 0) {
		snprintf(jail_path, sizeof(jail_path), "%s/%s", home,
			 tok.path + 2);
		plan_add_grant(plan, jail_path, jail_path, tok.mode, false);
	} else if (strncmp(tok.path, "xdg-", 4) == 0) {
		if (resolve_xdg(home, tok.path, host_path, sizeof(host_path)) ==
		    0) {
			plan_add_grant(plan, host_path, host_path, tok.mode,
				       false);
		} else {
			fpkg_warnf("unsupported or disabled xdg dir '%s', "
				   "skipping",
				   entry);
		}
	} else if (tok.path[0] == '/') {
		plan_add_grant(plan, tok.path, tok.path, tok.mode, false);
	} else {
		fpkg_warnf("unknown filesystem location '%s' (valid: host, "
			   "host-os, host-etc, host-root, home, xdg-*[/..], "
			   "~/dir, "
			   "/dir), skipping",
			   entry);
	}

	free(tok.path);
}

static void strlist_push(strlist_t* l, const char* item) {
	assert(l != NULL);
	assert(item != NULL);
	l->count++;
	char** new_items = realloc(l->items, l->count * sizeof(char*));
	if (!new_items) {
		fpkg_errorf("out of memory");
		exit(1);
	}
	l->items = new_items;
	l->items[l->count - 1] = xstrdup(item);
}

static void strlist_copy(strlist_t* dst, const strlist_t* src) {
	assert(dst != NULL);
	assert(src != NULL);
	for (size_t i = 0; i < src->count; i++)
		strlist_push(dst, src->items[i]);
}

static void drop_grant(sandbox_plan_t* plan, const char* jail_path) {
	assert(plan != NULL);
	assert(jail_path != NULL);
	for (size_t j = 0; j < plan->ngrants; j++) {
		if (strcmp(plan->grants[j].jail_path, jail_path) != 0) continue;
		free(plan->grants[j].host_path);
		free(plan->grants[j].jail_path);
		plan->grants[j] = plan->grants[--plan->ngrants];
		j--;
	}
}

static void remove_fs_entry(sandbox_plan_t* plan, const char* entry) {
	assert(plan != NULL);
	assert(entry != NULL);
	char msg[512] = "";
	fs_token_t tok;
	if (parse_fs_token(entry, &tok, msg, sizeof(msg)) != 0) {
		if (msg[0]) fpkg_warnf("%s", msg);
		return;
	}
	const char* home = plan->home_dir;
	char host_path[4096], jail_path[4096];

	if (strcmp(tok.path, "host") == 0 || strcmp(tok.path, "host-os") == 0 ||
	    strcmp(tok.path, "host-etc") == 0 ||
	    strcmp(tok.path, "host-root") == 0) {
		const char* targets[] = {"/run/host", "/run/host/usr",
					 "/run/host/etc", "/run/host/root",
					 NULL};
		for (int i = 0; targets[i]; i++) drop_grant(plan, targets[i]);
	} else if (strcmp(tok.path, "home") == 0 ||
		   strcmp(tok.path, "~") == 0) {
		drop_grant(plan, home);
	} else if (strncmp(tok.path, "home/", 5) == 0) {
		snprintf(jail_path, sizeof(jail_path), "%s/%s", home,
			 tok.path + 5);
		drop_grant(plan, jail_path);
	} else if (strncmp(tok.path, "~/", 2) == 0) {
		snprintf(jail_path, sizeof(jail_path), "%s/%s", home,
			 tok.path + 2);
		drop_grant(plan, jail_path);
	} else if (strncmp(tok.path, "xdg-", 4) == 0) {
		if (resolve_xdg(home, tok.path, host_path, sizeof(host_path)) ==
		    0)
			drop_grant(plan, host_path);
		else
			fpkg_warnf("cannot remove an unsupported or "
				   "disabled xdg dir '%s', skipping",
				   tok.path);
	} else if (tok.path[0] == '/') {
		drop_grant(plan, tok.path);
	} else {
		fpkg_warnf("cannot remove filesystem '%s', skipping", entry);
	}

	free(tok.path);
}

static bool condition_holds(const char* cond) {
	if (!cond || !*cond) return false;
	bool negate = false;
	if (cond[0] == '!') {
		negate = true;
		cond++;
	}
	bool v = false;
	if (strcmp(cond, "true") == 0) {
		v = true;
	} else if (strcmp(cond, "false") == 0) {
		v = false;
	} else if (strcmp(cond, "has-input-device") == 0) {
		struct stat st;
		v = (stat("/dev/input", &st) == 0 && S_ISDIR(st.st_mode));
	} else if (strcmp(cond, "has-usb-device") == 0) {
		struct stat st;
		v = (stat("/dev/bus/usb", &st) == 0 && S_ISDIR(st.st_mode));
	} else if (strcmp(cond, "has-wayland") == 0) {
		const char* wd = cli_getenv("WAYLAND_DISPLAY");
		const char* rundir = cli_getenv("XDG_RUNTIME_DIR");
		if (wd && *wd && rundir && *rundir) {
			char p[PATH_MAX];
			snprintf(p, sizeof(p), "%s/%s", rundir, wd);
			struct stat st;
			v = (stat(p, &st) == 0 && S_ISSOCK(st.st_mode));
		}
	} else if (strcmp(cond, "has-usb-portal") == 0) {
		v = (access("/usr/local/libexec/xdg-desktop-portal", X_OK) ==
		     0);
	}
	return negate ? !v : v;
}

static void warn_unknown_context_entry(const char* list, const char* item) {
	assert(list != NULL);
	if (item == NULL || item[0] == '\0') return;
	fpkg_warnf("unknown %s entry '%s' in this app's [Context]: ignored",
		   list, item);
}

void evaluate_conditionals_in_lists(const fp_metadata_t* meta) {
	assert(meta != NULL);
	strlist_t* lists[] = {
		(strlist_t*)&meta->shared, (strlist_t*)&meta->sockets,
		(strlist_t*)&meta->devices, (strlist_t*)&meta->features, NULL};
	for (int li = 0; lists[li]; li++) {
		strlist_t* l = lists[li];
		for (size_t i = 0; i < l->count; i++) {
			const char* e = l->items[i];
			if (strncmp(e, "if:", 3) != 0) continue;
			const char* name_start = e + 3;
			const char* sep = strchr(name_start, ':');
			if (!sep) {
				l->items[i][0] = '\0';
				continue;
			}
			size_t name_len = sep - name_start;
			const char* cond = sep + 1;
			bool ok = condition_holds(cond);
			if (ok) {
				char* tmp = malloc(name_len + 1);
				if (tmp) {
					memcpy(tmp, name_start, name_len);
					tmp[name_len] = '\0';
					free(l->items[i]);
					l->items[i] = tmp;
				}
			} else {
				l->items[i][0] = '\0';
			}
		}
	}
}

void permissions_apply_overrides(sandbox_plan_t* plan, const run_opts_t* opts) {
	assert(plan != NULL);
	assert(opts != NULL);
	// socket=NAME / socket=!NAME from run(1).
	for (size_t i = 0; i < opts->sockets.count; i++) {
		const char* s = opts->sockets.items[i];
		bool on = s[0] != '!';
		const char* n = on ? s : s + 1;
		if (strcmp(n, "x11") == 0) plan->sock_x11 = on;
		else if (strcmp(n, "fallback-x11") == 0)
			plan->sock_fallback_x11 = on;
		else if (strcmp(n, "wayland") == 0)
			plan->sock_wayland = on;
		else if (strcmp(n, "inherit-wayland-socket") == 0)
			plan->sock_inherit_wayland = on;
		else if (strcmp(n, "pulseaudio") == 0)
			plan->sock_pulseaudio = on;
		else if (strcmp(n, "session-bus") == 0)
			plan->sock_session_bus = on;
		else if (strcmp(n, "system-bus") == 0)
			plan->sock_system_bus = on;
		else if (strcmp(n, "ssh-auth") == 0)
			plan->sock_ssh_auth = on;
		else if (strcmp(n, "pcsc") == 0)
			plan->sock_pcsc = on;
		else if (strcmp(n, "cups") == 0)
			plan->sock_cups = on;
		else if (strcmp(n, "gpg-agent") == 0)
			plan->sock_gpg_agent = on;
	}

	// device=all|!NAME from run(1).
	for (size_t i = 0; i < opts->devices.count; i++) {
		const char* d = opts->devices.items[i];
		bool on = d[0] != '!';
		const char* n = on ? d : d + 1;
		if (strcmp(n, "all") == 0) plan->dev_all = on;
		else if (strcmp(n, "dri") == 0)
			plan->dev_dri = on;
		else if (strcmp(n, "kvm") == 0)
			plan->dev_kvm = on;
		else if (strcmp(n, "input") == 0)
			plan->dev_input = on;
		else if (strcmp(n, "usb") == 0)
			plan->dev_usb = on;
		else if (strcmp(n, "shm") == 0)
			plan->dev_shm = on;
		else if (strcmp(n, "snd") == 0)
			plan->dev_snd = on;
	}

	// share=network|ipc from run(1); --unshare=all arrives already expanded
	// to !network and !ipc, and `process` is inherent.
	for (size_t i = 0; i < opts->shares.count; i++) {
		const char* s = opts->shares.items[i];
		bool on = s[0] != '!';
		const char* n = on ? s : s + 1;
		if (strcmp(n, "network") == 0) plan->want_network = on;
		else if (strcmp(n, "ipc") == 0)
			plan->want_ipc = on;

		else if (strcmp(n, "process") == 0)
			;
	}

	for (size_t i = 0; i < opts->fs_add.count; i++)
		expand_fs_entry(plan, opts->fs_add.items[i]);

	for (size_t i = 0; i < opts->fs_remove.count; i++)
		remove_fs_entry(plan, opts->fs_remove.items[i]);

	for (size_t i = 0; i < opts->envs.count; i++) {
		strlist_push(&plan->env_overrides, opts->envs.items[i]);
		if (strncmp(opts->envs.items[i], "FLATPKG_SPAWN_NONCE=", 20) ==
		    0)
			plan->spawned_via_portal = true;
	}

	if (opts->devel) plan->devel = true;
	if (opts->clearenv) plan->clearenv = true;
	if (opts->die_with_parent) plan->die_with_parent = true;
}

static void warn_bus_policy(const char* which, const strlist_t* list) {
	assert(which != NULL);
	assert(list != NULL);
	for (size_t i = 0; i < list->count; i++) {
		const char* entry = list->items[i];
		const char* eq = strchr(entry, '=');
		const char* pol;
		char name[256];
		size_t nlen;

		if (eq == NULL || eq == entry) continue;
		nlen = (size_t)(eq - entry);
		pol = eq + 1;
		if (strcmp(pol, "none") != 0 && strcmp(pol, "see") != 0 &&
		    strcmp(pol, "talk") != 0 && strcmp(pol, "own") != 0) {
			fpkg_warnf("[%s Bus Policy] unknown policy '%s' "
				   "in '%s', ignoring",
				   which, pol, entry);
			continue;
		}
		if (strcmp(pol, "talk") != 0 && strcmp(pol, "own") != 0)
			continue;
		if (nlen == 0 || nlen >= sizeof(name)) continue;
		memcpy(name, entry, nlen);
		name[nlen] = '\0';
		if (strcmp(name, "org.freedesktop.Flatpak") == 0)
			fpkg_warnf(
				"[%s Bus Policy] '%s' on flatpkg's own "
				"session helper (%s): it runs host commands as "
				"you",
				which, pol, name);
		else if (strcmp(name, "org.freedesktop.portal.Flatpak") == 0)
			fpkg_warnf("[%s Bus Policy] '%s' on flatpkg's own "
				   "portal (%s): it builds sandboxes, and "
				   "'own' also "
				   "impersonates it",
				   which, pol, name);
	}
}

int permissions_plan(const fp_metadata_t* meta, const char* home_dir,
		     sandbox_plan_t* plan) {
	assert(meta != NULL);
	assert(home_dir != NULL);
	assert(plan != NULL);
	memset(plan, 0, sizeof(*plan));
	plan->home_dir = xstrdup(home_dir);
	plan->dev_snd = false;

	const char* eu = cli_getenv("FLATPKG_APP_UID");
	if (eu && *eu) {
		plan->uid = (uid_t)strtoul(eu, NULL, 10);
		const char* eg = cli_getenv("FLATPKG_APP_GID");
		plan->gid = eg && *eg ? (gid_t)strtoul(eg, NULL, 10)
				      : (gid_t)plan->uid;
		const char* en = cli_getenv("FLATPKG_APP_USER");
		plan->user_name = xstrdup(en && *en ? en : "user");
		plan->real_name = xstrdup("");
		const char* eh = cli_getenv("FLATPKG_APP_HOME");
		if (eh && *eh) {
			free(plan->home_dir);
			plan->home_dir = xstrdup(eh);
		}
	} else {
		struct passwd* pw = getpwuid(getuid());
		if (pw) {
			plan->user_name = xstrdup(pw->pw_name);
			plan->real_name =
				xstrdup(pw->pw_gecos ? pw->pw_gecos : "");
			plan->uid = pw->pw_uid;
			plan->gid = pw->pw_gid;
		} else {
			plan->user_name = xstrdup("user");
			plan->real_name = xstrdup("");
			plan->uid = getuid();
			plan->gid = getgid();
		}
	}
	struct group* gr = getgrgid(plan->gid);
	plan->group_name = xstrdup(gr ? gr->gr_name : "users");

	const char* rundir = cli_getenv("XDG_RUNTIME_DIR");
	plan->xdg_runtime_dir =
		xstrdup((rundir && *rundir) ? rundir : "/var/run/user/1000");
	if (!cli_getenv("XDG_RUNTIME_DIR")) {
		char fb[64];
		snprintf(fb, sizeof(fb), "/var/run/user/%u", plan->uid);
		free(plan->xdg_runtime_dir);
		plan->xdg_runtime_dir = xstrdup(fb);
	}

	// Build the per-app data dir from plan->home_dir, NOT the parameter: in
	// daemon mode that is the worker's HOME.
	char data_dir[4096];
	snprintf(data_dir, sizeof(data_dir), "%s/.var/app/%s", plan->home_dir,
		 meta->app_id);
	plan->app_data_dir = xstrdup(data_dir);

	evaluate_conditionals_in_lists(meta);

	// share=network|ipc from [Context]; anything else is a typo and warns.
	for (size_t i = 0; i < meta->shared.count; i++) {
		if (strcmp(meta->shared.items[i], "network") == 0)
			plan->want_network = true;
		else if (strcmp(meta->shared.items[i], "ipc") == 0)
			plan->want_ipc = true;
		else
			warn_unknown_context_entry("shared",
						   meta->shared.items[i]);
	}

	for (size_t i = 0; i < meta->devices.count; i++) {
		if (strcmp(meta->devices.items[i], "dri") == 0)
			plan->dev_dri = true;
		else if (strcmp(meta->devices.items[i], "kvm") == 0)
			plan->dev_kvm = true;
		else if (strcmp(meta->devices.items[i], "input") == 0)
			plan->dev_input = true;
		else if (strcmp(meta->devices.items[i], "usb") == 0)
			plan->dev_usb = true;
		else if (strcmp(meta->devices.items[i], "shm") == 0)
			plan->dev_shm = true;
		else if (strcmp(meta->devices.items[i], "snd") == 0)
			plan->dev_snd = true;
		else if (strcmp(meta->devices.items[i], "all") == 0)
			plan->dev_all = true;
		else
			warn_unknown_context_entry("devices",
						   meta->devices.items[i]);
	}

	// strcmp() rather than a table, since it must accept the ! prefix.
	for (size_t i = 0; i < meta->sockets.count; i++) {
		const char* s = meta->sockets.items[i];
		if (strcmp(s, "x11") == 0) plan->sock_x11 = true;
		else if (strcmp(s, "fallback-x11") == 0)
			plan->sock_fallback_x11 = true;
		else if (strcmp(s, "wayland") == 0)
			plan->sock_wayland = true;
		else if (strcmp(s, "inherit-wayland-socket") == 0)
			plan->sock_inherit_wayland = true;
		else if (strcmp(s, "pulseaudio") == 0)
			plan->sock_pulseaudio = true;
		else if (strcmp(s, "session-bus") == 0)
			plan->sock_session_bus = true;
		else if (strcmp(s, "system-bus") == 0)
			plan->sock_system_bus = true;
		else if (strcmp(s, "ssh-auth") == 0)
			plan->sock_ssh_auth = true;
		else if (strcmp(s, "pcsc") == 0)
			plan->sock_pcsc = true;
		else if (strcmp(s, "cups") == 0)
			plan->sock_cups = true;
		else if (strcmp(s, "gpg-agent") == 0)
			plan->sock_gpg_agent = true;
		else
			warn_unknown_context_entry("sockets", s);
	}

	// features=: every accepted name sets a plan flag sandbox.c reads;
	// `multiarch` is accepted and ignored (the Linuxulator is 64-bit only).
	for (size_t i = 0; i < meta->features.count; i++) {
		const char* f = meta->features.items[i];
		if (strcmp(f, "devel") == 0) plan->devel = true;
		else if (strcmp(f, "multiarch") == 0)
			plan->multiarch = true;
		else if (strcmp(f, "bluetooth") == 0)
			plan->bluetooth = true;
		else if (strcmp(f, "canbus") == 0)
			plan->canbus = true;
		else if (strcmp(f, "per-app-dev-shm") == 0)
			plan->per_app_dev_shm = true;
		else
			warn_unknown_context_entry("features", f);
	}
	if (plan->multiarch)
		fpkg_warnf("features=multiarch is not implemented: this "
			   "app asked for 32-bit support and will get 64-bit "
			   "only");

	for (size_t i = 0; i < meta->filesystems.count; i++)
		expand_fs_entry(plan, meta->filesystems.items[i]);

	for (size_t i = 0; i < meta->persistent.count; i++) {
		const char* p = meta->persistent.items[i];
		while (*p == '/') p++;
		if (strstr(p, "..") != NULL) {
			fpkg_warnf("persistent path '%s' rejected (no '..')",
				   meta->persistent.items[i]);
			continue;
		}
		char host_path[4096], jail_path[4096];
		snprintf(host_path, sizeof(host_path), "%s/%s",
			 plan->app_data_dir, p);
		snprintf(jail_path, sizeof(jail_path), "%s/%s", plan->home_dir,
			 p);
		plan_add_grant(plan, host_path, jail_path, FS_GRANT_RW, true);
	}

	warn_bus_policy("session", &meta->session_bus_policy);
	warn_bus_policy("system", &meta->system_bus_policy);
	warn_bus_policy("a11y", &meta->a11y_bus_policy);
	strlist_copy(&plan->session_bus_policy, &meta->session_bus_policy);
	strlist_copy(&plan->system_bus_policy, &meta->system_bus_policy);
	strlist_copy(&plan->a11y_bus_policy, &meta->a11y_bus_policy);

	return 0;
}

void permissions_free_plan(sandbox_plan_t* plan) {
	assert(plan != NULL);
	for (size_t i = 0; i < plan->ngrants; i++) {
		free(plan->grants[i].host_path);
		free(plan->grants[i].jail_path);
	}
	free(plan->grants);
	free(plan->user_name);
	free(plan->group_name);
	free(plan->real_name);
	free(plan->home_dir);
	free(plan->xdg_runtime_dir);
	free(plan->app_data_dir);
	for (size_t i = 0; i < plan->env_overrides.count; i++)
		free(plan->env_overrides.items[i]);
	free(plan->env_overrides.items);
	for (size_t i = 0; i < plan->session_bus_policy.count; i++)
		free(plan->session_bus_policy.items[i]);
	free(plan->session_bus_policy.items);
	for (size_t i = 0; i < plan->system_bus_policy.count; i++)
		free(plan->system_bus_policy.items[i]);
	free(plan->system_bus_policy.items);
	for (size_t i = 0; i < plan->a11y_bus_policy.count; i++)
		free(plan->a11y_bus_policy.items[i]);
	free(plan->a11y_bus_policy.items);
	memset(plan, 0, sizeof(*plan));
}
