#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include "flatpkg.h"
#define FPKG_MODULE "overrides"
#include "fpkglog.h"

static void list_append(strlist_t* l, const char* item) {
	assert(l != NULL);
	assert(item != NULL);
	char** nb = realloc(l->items, (l->count + 1) * sizeof(char*));
	if (nb == NULL) {
		fpkg_errorf("out of memory");
		exit(1);
	}
	l->items = nb;
	l->items[l->count] = strdup(item);
	if (l->items[l->count] == NULL) {
		fpkg_errorf("out of memory");
		exit(1);
	}
	l->count++;
}

static void list_remove_at(strlist_t* l, size_t i) {
	assert(l != NULL);
	assert(i < l->count);
	free(l->items[i]);
	memmove(&l->items[i], &l->items[i + 1],
		(l->count - i - 1) * sizeof(char*));
	l->count--;
}

static void list_clear(strlist_t* l) {
	assert(l != NULL);
	for (size_t i = 0; i < l->count; i++) free(l->items[i]);
	free(l->items);
	l->items = NULL;
	l->count = 0;
}

static void entry_key(const char* entry, char* key, size_t keylen) {
	assert(entry != NULL);
	assert(key != NULL);
	assert(keylen > 0);
	static const char* suffixes[] = {":ro", ":rw", ":create", NULL};
	const char* p = entry;
	size_t n;

	if (*p == '!') p++;
	n = strlen(p);
	for (size_t i = 0; suffixes[i] != NULL; i++) {
		size_t sl = strlen(suffixes[i]);

		if (n > sl && strcmp(p + n - sl, suffixes[i]) == 0) {
			n -= sl;
			break;
		}
	}
	if (n >= keylen) n = keylen - 1;
	memcpy(key, p, n);
	key[n] = '\0';
}

static void merge_list(strlist_t* dst, const strlist_t* src, bool filesystems) {
	assert(dst != NULL);
	assert(src != NULL);
	for (size_t i = 0; i < src->count; i++) {
		const char* entry = src->items[i];
		char key[512];

		entry_key(entry, key, sizeof(key));

		if (filesystems && strcmp(key, "host-reset") == 0) {
			list_clear(dst);
			list_append(dst, entry);
			continue;
		}

		for (size_t j = 0; j < dst->count;) {
			char dkey[512];

			entry_key(dst->items[j], dkey, sizeof(dkey));
			if (strcmp(dkey, key) == 0) list_remove_at(dst, j);
			else
				j++;
		}
		list_append(dst, entry);
	}
}

static void merge_env(strlist_t* dst, const strlist_t* src) {
	assert(dst != NULL);
	assert(src != NULL);
	for (size_t i = 0; i < src->count; i++) {
		const char* kv = src->items[i];
		const char* eq = strchr(kv, '=');
		size_t klen = eq ? (size_t)(eq - kv) : strlen(kv);

		for (size_t j = 0; j < dst->count;) {
			const char* d = dst->items[j];

			if (strncmp(d, kv, klen) == 0 &&
			    (d[klen] == '=' || d[klen] == '\0'))
				list_remove_at(dst, j);
			else
				j++;
		}
		list_append(dst, kv);
	}
}

// [Session Bus Policy] and friends: "NAME=policy", keyed by NAME.
static void merge_policy(strlist_t* dst, const strlist_t* src) {
	assert(dst != NULL);
	assert(src != NULL);
	for (size_t i = 0; i < src->count; i++) {
		const char* e = src->items[i];
		const char* eq = strchr(e, '=');
		size_t klen = eq ? (size_t)(eq - e) : strlen(e);

		for (size_t j = 0; j < dst->count;) {
			const char* d = dst->items[j];

			if (strncmp(d, e, klen) == 0 && d[klen] == '=')
				list_remove_at(dst, j);
			else
				j++;
		}
		list_append(dst, e);
	}
}

static void merge_metadata(fp_metadata_t* dst, const fp_metadata_t* src) {
	assert(dst != NULL);
	assert(src != NULL);
	merge_list(&dst->shared, &src->shared, false);
	merge_list(&dst->sockets, &src->sockets, false);
	merge_list(&dst->devices, &src->devices, false);
	merge_list(&dst->features, &src->features, false);
	merge_list(&dst->filesystems, &src->filesystems, true);
	merge_list(&dst->persistent, &src->persistent, false);
	merge_env(&dst->env_pairs, &src->env_pairs);
	merge_list(&dst->unset_environment, &src->unset_environment, false);
	merge_policy(&dst->session_bus_policy, &src->session_bus_policy);
	merge_policy(&dst->system_bus_policy, &src->system_bus_policy);
	merge_policy(&dst->a11y_bus_policy, &src->a11y_bus_policy);
}

// Where one override file lives. app_id NULL/"" is the global one; NULL is
// returned if the app id is not a usable file name.
char* overrides_path(bool user, const char* app_id) {
	char path[PATH_MAX];
	const char* name =
		(app_id != NULL && *app_id != '\0') ? app_id : "global";

	if (app_id != NULL && *app_id != '\0' && !remote_name_valid(app_id)) {
		fpkg_errorf("invalid application id '%s' (expected a "
			    "dotted name like org.example.App, not a path)",
			    app_id);
		return NULL;
	}

	if (user) {
		const char* home = cli_getenv("HOME");

		if (home == NULL || *home == '\0') home = "/";
		snprintf(path, sizeof(path),
			 "%s/.local/share/flatpak/overrides/%s", home, name);
	} else {
		snprintf(path, sizeof(path), "%s/overrides/%s", store_root(),
			 name);
	}
	return strdup(path);
}

int overrides_apply(fp_metadata_t* meta, const char* app_id) {
	assert(meta != NULL);
	static const bool users[] = {false, true};
	int applied = 0;

	for (size_t u = 0; u < sizeof(users) / sizeof(users[0]); u++) {
		for (int per_app = 0; per_app < 2; per_app++) {
			char* path = overrides_path(users[u],
						    per_app ? app_id : NULL);
			struct stat st;

			if (path == NULL) return -1;
			if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
				fp_metadata_t ov;

				if (fp_metadata_parse_any(path, &ov) == 0) {
					merge_metadata(meta, &ov);
					fp_metadata_free(&ov);
					applied++;
					fpkg_infof("override: %s", path);
				}
			}
			free(path);
		}
	}
	return applied;
}

// Write a "key=a;b;c;" line, flatpak's serialization for these lists.
// Empty lists are left out, which makes the override idempotent.
static void write_list(FILE* fp, const char* key, const strlist_t* l) {
	assert(fp != NULL);
	assert(key != NULL);
	assert(l != NULL);
	if (l->count == 0) return;
	fprintf(fp, "%s=", key);
	for (size_t i = 0; i < l->count; i++) fprintf(fp, "%s;", l->items[i]);
	fprintf(fp, "\n");
}

// The override file's text for `ctx`: what overrides_save() writes, and what a
// client sends the daemon for a system-scope change.
static void overrides_write(FILE* fp, const fp_metadata_t* ctx) {
	assert(fp != NULL);
	assert(ctx != NULL);
	fprintf(fp, "[Context]\n");
	write_list(fp, "shared", &ctx->shared);
	write_list(fp, "sockets", &ctx->sockets);
	write_list(fp, "devices", &ctx->devices);
	write_list(fp, "filesystems", &ctx->filesystems);
	write_list(fp, "persistent", &ctx->persistent);
	write_list(fp, "features", &ctx->features);
	write_list(fp, "unset-environment", &ctx->unset_environment);

	if (ctx->env_pairs.count > 0) {
		fprintf(fp, "\n[Environment]\n");
		for (size_t i = 0; i < ctx->env_pairs.count; i++)
			fprintf(fp, "%s\n", ctx->env_pairs.items[i]);
	}
	if (ctx->session_bus_policy.count > 0) {
		fprintf(fp, "\n[Session Bus Policy]\n");
		for (size_t i = 0; i < ctx->session_bus_policy.count; i++)
			fprintf(fp, "%s\n", ctx->session_bus_policy.items[i]);
	}
	if (ctx->system_bus_policy.count > 0) {
		fprintf(fp, "\n[System Bus Policy]\n");
		for (size_t i = 0; i < ctx->system_bus_policy.count; i++)
			fprintf(fp, "%s\n", ctx->system_bus_policy.items[i]);
	}
	if (ctx->a11y_bus_policy.count > 0) {
		fprintf(fp, "\n[A11y Bus Policy]\n");
		for (size_t i = 0; i < ctx->a11y_bus_policy.count; i++)
			fprintf(fp, "%s\n", ctx->a11y_bus_policy.items[i]);
	}
}

char* overrides_to_text(const fp_metadata_t* ctx) {
	assert(ctx != NULL);
	char* buf = NULL;
	size_t len = 0;
	FILE* fp = open_memstream(&buf, &len);
	if (fp == NULL) return NULL;
	overrides_write(fp, ctx);
	if (fclose(fp) != 0) {
		free(buf);
		return NULL;
	}
	return buf;
}

// The text goes to a private file in `dir` and through the launch path's
// parser, so a caller's text can only become keys that parser knows.
int overrides_from_text(const char* text, const char* dir, fp_metadata_t* out) {
	assert(text != NULL);
	assert(dir != NULL);
	assert(out != NULL);
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s/.override.XXXXXX", dir);
	int fd = mkstemp(tmp);
	if (fd < 0) return -1;
	size_t n = strlen(text), off = 0;
	while (off < n) {
		ssize_t w = write(fd, text + off, n - off);
		if (w < 0 && errno == EINTR) continue;
		if (w <= 0) {
			close(fd);
			unlink(tmp);
			return -1;
		}
		off += (size_t)w;
	}
	close(fd);
	int rc = fp_metadata_parse_any(tmp, out);
	unlink(tmp);
	return rc;
}

int overrides_save(bool user, const char* app_id, const fp_metadata_t* ctx) {
	assert(ctx != NULL);
	char* path = overrides_path(user, app_id);
	char dir[PATH_MAX];
	FILE* fp;

	if (path == NULL) return -1;
	snprintf(dir, sizeof(dir), "%s", path);
	char* slash = strrchr(dir, '/');
	if (slash != NULL) {
		*slash = '\0';
		if (mkdir_p(dir, 0755) != 0) {
			fpkg_errorf("mkdir %s: %s", dir, strerror(errno));
			free(path);
			return -1;
		}
	}

	fp = fopen(path, "w");
	if (fp == NULL) {
		fpkg_errorf("cannot write %s: %s", path, strerror(errno));
		free(path);
		return -1;
	}

	overrides_write(fp, ctx);

	if (fclose(fp) != 0) {
		fpkg_errorf("write %s: %s", path, strerror(errno));
		free(path);
		return -1;
	}

	// Hand the file back to the user: the worker runs as root, so it would
	// otherwise land root-owned and be uneditable.
	if (user) {
		const char* eu = cli_getenv("FLATPKG_APP_UID");
		const char* eg = cli_getenv("FLATPKG_APP_GID");
		if (eu != NULL && *eu != '\0') {
			uid_t uid = (uid_t)strtoul(eu, NULL, 10);
			gid_t gid = (eg != NULL && *eg != '\0')
					    ? (gid_t)strtoul(eg, NULL, 10)
					    : uid;
			if (chown(path, uid, gid) != 0)
				fpkg_warnf(
					"%s is %s-owned after writing; "
					"the user will not be able to edit it: "
					"%s",
					path,
					uid == 0 ? "still root" : "wrongly",
					strerror(errno));
		}
	}

	fpkg_okf("wrote %s", path);
	free(path);
	return 0;
}

int overrides_show(bool user, const char* app_id) {
	char* path = overrides_path(user, app_id);
	if (path == NULL) return -1;
	FILE* fp = fopen(path, "r");

	if (fp == NULL) {
		free(path);
		return 0;
	}
	char line[4096];
	while (fgets(line, sizeof(line), fp)) fp_out_raw(line, strlen(line));
	fclose(fp);
	free(path);
	return 0;
}

int overrides_reset(bool user, const char* app_id) {
	char* path = overrides_path(user, app_id);

	if (path == NULL) return -1;
	if (unlink(path) != 0 && errno != ENOENT) {
		fpkg_errorf("cannot remove %s: %s", path, strerror(errno));
		free(path);
		return -1;
	}
	fpkg_okf("removed %s", path);
	free(path);
	return 0;
}

void overrides_merge(fp_metadata_t* dst, const fp_metadata_t* src) {
	assert(dst != NULL);
	assert(src != NULL);
	merge_metadata(dst, src);
}

int overrides_load(bool user, const char* app_id, fp_metadata_t* out) {
	assert(out != NULL);
	char* path = overrides_path(user, app_id);
	struct stat st;

	memset(out, 0, sizeof(*out));
	if (path == NULL) return -1;
	if (stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
	    fp_metadata_parse_any(path, out) == 0) {
		free(path);
		return 0;
	}
	free(path);
	memset(out, 0, sizeof(*out));
	return 0;
}
