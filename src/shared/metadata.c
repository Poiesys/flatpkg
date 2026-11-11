#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flatpkg.h"
#define FPKG_MODULE "metadata"
#include "fpkglog.h"

// A key that appears twice in a keyfile (or a field two groups both set)
// must not leak the first value.
static void set_str(char** dst, const char* val) {
	char* copy = xstrdup(val);
	free(*dst);
	*dst = copy;
}

static void strlist_push(strlist_t* l, const char* item) {
	assert(l != NULL);
	assert(item != NULL);
	l->items = realloc(l->items, (l->count + 1) * sizeof(char*));
	if (!l->items) {
		fpkg_errorf("out of memory");
		exit(1);
	}
	l->items[l->count++] = xstrdup(item);
}

static void strlist_from_semicolon_list(strlist_t* l, const char* value) {
	assert(l != NULL);
	assert(value != NULL);
	char* copy = xstrdup(value);
	char* save = NULL;
	for (char* tok = strtok_r(copy, ";", &save); tok;
	     tok = strtok_r(NULL, ";", &save)) {
		char* t = trim(tok);
		if (*t) strlist_push(l, t);
	}
	free(copy);
}

static void strlist_free(strlist_t* l) {
	assert(l != NULL);
	for (size_t i = 0; i < l->count; i++) free(l->items[i]);
	free(l->items);
	l->items = NULL;
	l->count = 0;
}

static bool key_bool(const char* val) {
	assert(val != NULL);
	return strcmp(val, "true") == 0 || strcmp(val, "1") == 0 ||
	       strcmp(val, "yes") == 0;
}

static int edit_distance_bounded(const char* a, const char* b, int limit) {
	assert(a != NULL);
	assert(b != NULL);
	assert(limit >= 0);
	size_t la = strlen(a), lb = strlen(b);
	if (la > lb) {
		const char* t = a;
		a = b;
		b = t;
		size_t l = la;
		la = lb;
		lb = l;
	}
	if (lb > 31) return limit + 1;
	if (lb - la > (size_t)limit) return limit + 1;
	int prev2[32], prev[32], cur[32];
	for (size_t j = 0; j <= lb; j++) prev[j] = (int)j;
	for (size_t i = 1; i <= la; i++) {
		cur[0] = (int)i;
		int best = cur[0];
		for (size_t j = 1; j <= lb; j++) {
			int sub = prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1);
			int del = prev[j] + 1;
			int ins = cur[j - 1] + 1;
			int m = sub < del ? sub : del;
			cur[j] = m < ins ? m : ins;
			if (i > 1 && j > 1 && a[i - 2] == b[j - 1] &&
			    a[i - 1] == b[j - 2] && prev2[j - 2] + 1 < cur[j])
				cur[j] = prev2[j - 2] + 1;
			if (cur[j] < best) best = cur[j];
		}
		if (best > limit) return limit + 1;
		memcpy(prev2, prev, (lb + 1) * sizeof(int));
		memcpy(prev, cur, (lb + 1) * sizeof(int));
	}
	return prev[lb];
}

static int key_edit_limit(const char* key) {
	assert(key != NULL);
	return strlen(key) >= 8 ? 2 : 1;
}

static void warn_key_near_miss(const char* group, const char* key,
			       const char* const* vocab, size_t nvocab) {
	assert(group != NULL);
	assert(key != NULL);
	assert(vocab != NULL || nvocab == 0);
	int limit = key_edit_limit(key);
	const char* best = NULL;
	int best_d = limit + 1;

	for (size_t i = 0; i < nvocab; i++) {
		int d = edit_distance_bounded(key, vocab[i], limit);
		if (d < best_d) {
			best_d = d;
			best = vocab[i];
			if (d == 0) break;
		}
	}
	if (best == NULL) return;
	fpkg_warnf("[%s] key '%s' in this metadata looks like a typo for "
		   "'%s': ignored",
		   group, key, best);
}

// The vocabularies, one per section that has one; open-ended sections
// ([Environment], Bus Policy) have no table and no typo to catch.
static const char* const app_keys[] = {
	"name",	   "version", "runtime",	  "sdk",
	"command", "branch",  "required-flatpak", "tags",
};
static const char* const context_keys[] = {
	"shared",     "sockets",  "devices",	       "filesystems",
	"persistent", "features", "unset-environment",
};
static const char* const ext_keys[] = {
	"version",	   "versions",	  "directory",	 "subdirectory-suffix",
	"subdirectories",  "needs-tmpfs", "add-ld-path", "download-if",
	"enable-if",	   "enable_if",	  "merge-dirs",	 "autodelete",
	"no-autodownload",
};
static const char* const extof_keys[] = {
	"priority",
	"ref",
};
static const char* const runtime_keys[] = {
	"sdk",
};

static int fp_metadata_parse_internal(const char* path, fp_metadata_t* out,
				      bool require_app);

int fp_metadata_parse(const char* path, fp_metadata_t* out) {
	assert(path != NULL);
	assert(out != NULL);
	return fp_metadata_parse_internal(path, out, true);
}

int fp_metadata_parse_any(const char* path, fp_metadata_t* out) {
	assert(path != NULL);
	assert(out != NULL);
	return fp_metadata_parse_internal(path, out, false);
}

static int fp_metadata_parse_internal(const char* path, fp_metadata_t* out,
				      bool require_app) {
	assert(path != NULL);
	assert(out != NULL);
	memset(out, 0, sizeof(*out));

	FILE* fp = fopen(path, "r");
	if (!fp) {
		fpkg_errorf("cannot open metadata '%s': %s", path,
			    strerror(errno));
		return -1;
	}

	char line[4096];
	char group[128] = "";
	char cur_ext[128] = "";
	char cur_extof[128] = "";
	size_t total = 0;

	while (fgets(line, sizeof(line), fp)) {
		char* l;
		size_t len = strlen(line);

		// A value longer than the buffer used to be read in two pieces,
		// and the tail parsed as a fresh key=value pair.  The whole line
		// is dropped, never the tail.
		if (len + 1 == sizeof(line) && line[len - 1] != '\n' &&
		    !feof(fp)) {
			int ch;

			while ((ch = fgetc(fp)) != EOF && ch != '\n')
				continue;
			fpkg_warnf("%s: a line longer than %zu bytes, ignored",
				   path, sizeof(line) - 1);
			continue;
		}
		// Metadata is whatever a remote published; a handful of megabytes
		// is already far more than the format uses.
		total += len;
		if (total > 16u << 20) {
			fpkg_warnf("%s: larger than 16 MiB, the rest ignored",
				   path);
			break;
		}
		l = trim(line);
		if (*l == '\0' || *l == '#') continue;

		if (*l == '[') {
			char* end = strchr(l, ']');
			if (!end) continue;
			*end = '\0';
			// A name that does not fit is not truncated into another
			// group: the group is dropped instead.
			if (strlen(l + 1) >= sizeof(group)) {
				fpkg_warnf("%s: group name longer than %zu "
					   "bytes, ignored",
					   path, sizeof(group) - 1);
				group[0] = '\0';
				cur_ext[0] = '\0';
				continue;
			}
			snprintf(group, sizeof(group), "%s", l + 1);
			continue;
		}

		char* eq = strchr(l, '=');
		if (!eq) continue;
		*eq = '\0';
		char* key = trim(l);
		char* val = trim(eq + 1);

		if (strcmp(group, "Application") == 0) {
			if (strcmp(key, "name") == 0)
				set_str(&out->app_id, val);
			else if (strcmp(key, "version") == 0)
				set_str(&out->version, val);
			else if (strcmp(key, "runtime") == 0)
				set_str(&out->runtime_ref, val);
			else if (strcmp(key, "sdk") == 0)
				set_str(&out->sdk_ref, val);
			else if (strcmp(key, "command") == 0)
				set_str(&out->command, val);
			else if (strcmp(key, "branch") == 0)
				set_str(&out->branch, val);
			else if (strcmp(key, "required-flatpak") == 0)
				strlist_from_semicolon_list(
					&out->required_flatpak, val);
			else if (strcmp(key, "tags") == 0)
				strlist_from_semicolon_list(&out->tags, val);
			else
				warn_key_near_miss(group, key, app_keys,
						   sizeof(app_keys) /
							   sizeof(app_keys[0]));
		} else if (strcmp(group, "Context") == 0) {
			if (strcmp(key, "shared") == 0)
				strlist_from_semicolon_list(&out->shared, val);
			else if (strcmp(key, "sockets") == 0)
				strlist_from_semicolon_list(&out->sockets, val);
			else if (strcmp(key, "devices") == 0)
				strlist_from_semicolon_list(&out->devices, val);
			else if (strcmp(key, "filesystems") == 0)
				strlist_from_semicolon_list(&out->filesystems,
							    val);
			else if (strcmp(key, "persistent") == 0)
				strlist_from_semicolon_list(&out->persistent,
							    val);
			else if (strcmp(key, "features") == 0)
				strlist_from_semicolon_list(&out->features,
							    val);
			else if (strcmp(key, "unset-environment") == 0)
				strlist_from_semicolon_list(
					&out->unset_environment, val);
			else
				warn_key_near_miss(
					group, key, context_keys,
					sizeof(context_keys) /
						sizeof(context_keys[0]));
		} else if (strcmp(group, "Environment") == 0) {
			char kv[4224];

			if (out->env_pairs.count >= 1024) {
				if (out->env_pairs.count == 1024)
					fpkg_warnf("%s: more than 1024 "
						   "environment entries, the "
						   "rest ignored",
						   path);
				continue;
			}
			snprintf(kv, sizeof(kv), "%s=%s", key, val);
			strlist_push(&out->env_pairs, kv);
		} else if (strcmp(group, "Session Bus Policy") == 0 ||
			   strcmp(group, "System Bus Policy") == 0 ||
			   strcmp(group, "A11y Bus Policy") == 0) {
			char kv[4224];
			snprintf(kv, sizeof(kv), "%s=%s", key, val);
			if (strcmp(group, "Session Bus Policy") == 0) {
				if (strcmp(key, "talk") == 0 ||
				    strcmp(key, "own") == 0) {
					fpkg_warnf("[Session Bus Policy] "
						   "'%s=%s' is wrong; write "
						   "'%s=%s' instead",
						   key, val, val, key);
					strlist_push(
						strcmp(key, "talk") == 0
							? &out->session_bus_talk
							: &out->session_bus_own,
						val);
				} else {
					strlist_push(&out->session_bus_policy,
						     kv);
				}
			} else if (strcmp(group, "System Bus Policy") == 0) {
				strlist_push(&out->system_bus_policy, kv);
			} else {
				strlist_push(&out->a11y_bus_policy, kv);
			}
		} else if (strncmp(group, "Extension ", 10) == 0) {
			fp_ext_group_t* g;
			if (cur_ext[0] == '\0' || strcmp(cur_ext, group) != 0) {
				snprintf(cur_ext, sizeof(cur_ext), "%s", group);
				out->ext_groups =
					realloc(out->ext_groups,
						(out->next_groups + 1) *
							sizeof(fp_ext_group_t));
				if (!out->ext_groups) {
					fpkg_errorf("out of memory");
					exit(1);
				}
				g = &out->ext_groups[out->next_groups++];
				memset(g, 0, sizeof(*g));
				g->id = xstrdup(group + 10);
			} else {
				g = &out->ext_groups[out->next_groups - 1];
			}
			if (strcmp(key, "version") == 0)
				set_str(&g->version, val);
			else if (strcmp(key, "versions") == 0)
				strlist_from_semicolon_list(&g->versions, val);
			else if (strcmp(key, "directory") == 0)
				set_str(&g->directory, val);
			else if (strcmp(key, "subdirectory-suffix") == 0)
				set_str(&g->subdir_suffix, val);
			else if (strcmp(key, "subdirectories") == 0)
				g->subdirectories = key_bool(val);
			else if (strcmp(key, "needs-tmpfs") == 0)
				g->needs_tmpfs = key_bool(val);
			else if (strcmp(key, "add-ld-path") == 0)
				set_str(&g->add_ld_path, val);
			else if (strcmp(key, "download-if") == 0)
				set_str(&g->download_if, val);
			else if (strcmp(key, "enable-if") == 0 ||
				 strcmp(key, "enable_if") == 0)
				set_str(&g->enable_if, val);
			else if (strcmp(key, "merge-dirs") == 0)
				strlist_from_semicolon_list(&g->merge_dirs,
							    val);
			else if (strcmp(key, "autodelete") == 0)
				g->autodelete = key_bool(val);
			else if (strcmp(key, "no-autodownload") == 0)
				g->no_autodownload = key_bool(val);
			else
				warn_key_near_miss(group, key, ext_keys,
						   sizeof(ext_keys) /
							   sizeof(ext_keys[0]));
		} else if (strcmp(group, "Runtime") == 0) {
			if (strcmp(key, "sdk") == 0)
				set_str(&out->sdk_ref, val);
			else
				warn_key_near_miss(
					group, key, runtime_keys,
					sizeof(runtime_keys) /
						sizeof(runtime_keys[0]));
		} else if (strncmp(group, "ExtensionOf", 11) == 0) {
			fp_extof_t* e;
			if (cur_extof[0] == '\0' ||
			    strcmp(cur_extof, group) != 0) {
				snprintf(cur_extof, sizeof(cur_extof), "%s",
					 group);
				out->extof_groups =
					realloc(out->extof_groups,
						(out->nextof_groups + 1) *
							sizeof(fp_extof_t));
				if (!out->extof_groups) {
					fpkg_errorf("out of memory");
					exit(1);
				}
				e = &out->extof_groups[out->nextof_groups++];
				memset(e, 0, sizeof(*e));
				if (group[11] != '\0')
					e->parent = xstrdup(trim(group + 11));
			} else {
				e = &out->extof_groups[out->nextof_groups - 1];
			}
			if (strcmp(key, "priority") == 0)
				e->priority = atoi(val);
			else if (strcmp(key, "ref") == 0)
				set_str(&e->ref, val);
			else
				warn_key_near_miss(
					group, key, extof_keys,
					sizeof(extof_keys) /
						sizeof(extof_keys[0]));
		}
	}

	fclose(fp);

	if (require_app && (!out->app_id || !out->command)) {
		fpkg_errorf("metadata '%s' is missing %s in [Application]",
			    path, !out->app_id ? "name=" : "command=");
		fp_metadata_free(out);
		return -1;
	}
	return 0;
}

static bool group_covers_branch(const fp_ext_group_t* g, const char* branch) {
	assert(g != NULL);
	if (branch == NULL || branch[0] == '\0') return false;
	if (g->version != NULL && *g->version != '\0' &&
	    strcmp(g->version, branch) == 0)
		return true;
	for (size_t i = 0; i < g->versions.count; i++)
		if (g->versions.items[i] != NULL &&
		    strcmp(g->versions.items[i], branch) == 0)
			return true;
	return false;
}

bool ext_id_matches_declared(const char* id, const char* branch,
			     const fp_ext_group_t* g) {
	if (id == NULL || g == NULL || g->id == NULL) return false;
	if (strcmp(id, g->id) == 0) return true;
	size_t n = strlen(g->id);
	if (strncmp(id, g->id, n) != 0 || id[n] != '.') return false;
	if (branch == NULL || branch[0] == '\0') return true;
	return group_covers_branch(g, branch);
}

void fp_metadata_free(fp_metadata_t* m) {
	assert(m != NULL);
	free(m->app_id);
	free(m->version);
	free(m->runtime_ref);
	free(m->sdk_ref);
	free(m->command);
	free(m->branch);
	strlist_free(&m->required_flatpak);
	strlist_free(&m->tags);
	strlist_free(&m->shared);
	strlist_free(&m->sockets);
	strlist_free(&m->devices);
	strlist_free(&m->filesystems);
	strlist_free(&m->persistent);
	strlist_free(&m->features);
	strlist_free(&m->env_pairs);
	strlist_free(&m->unset_environment);
	strlist_free(&m->session_bus_talk);
	strlist_free(&m->session_bus_own);
	strlist_free(&m->session_bus_policy);
	strlist_free(&m->system_bus_policy);
	strlist_free(&m->a11y_bus_policy);
	for (size_t i = 0; i < m->next_groups; i++) {
		fp_ext_group_t* g = &m->ext_groups[i];
		free(g->id);
		free(g->version);
		strlist_free(&g->versions);
		free(g->directory);
		free(g->subdir_suffix);
		free(g->add_ld_path);
		free(g->download_if);
		free(g->enable_if);
		strlist_free(&g->merge_dirs);
	}
	free(m->ext_groups);
	for (size_t i = 0; i < m->nextof_groups; i++) {
		fp_extof_t* e = &m->extof_groups[i];
		free(e->parent);
		free(e->ref);
	}
	free(m->extof_groups);
	memset(m, 0, sizeof(*m));
}
