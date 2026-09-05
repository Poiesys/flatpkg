#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fts.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include "flatpkg.h"
#define FPKG_MODULE "store"
#include "fpkglog.h"

// Read-only half of the store: layout, listing and resolving.

bool flatpkg_client_name_ok(const char* name) {
	if (name == NULL || *name == '\0') return false;
	size_t n = strlen(name);
	if (n > 64 || (n == 1 && name[0] == '.') ||
	    (n == 2 && name[0] == '.' && name[1] == '.'))
		return false;
	for (size_t i = 0; i < n; i++) {
		char ch = name[i];
		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
		      (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' ||
		      ch == '-'))
			return false;
	}
	return true;
}

int flatpkg_client_path(const char* client, const char* file, char* out,
			size_t len) {
	if (!flatpkg_client_name_ok(client) || !flatpkg_client_name_ok(file))
		return -1;
	int n = snprintf(out, len, "%s/clients/%s/%s", store_root(), client,
			 file);
	return n > 0 && (size_t)n < len ? 0 : -1;
}

const char* store_root(void) {
	// $FLATPKG_STORE lets non-root invocations point at a private store for
	// testing; ignored when root, so a setuid binary cannot be redirected.
	if (geteuid() != 0) {
		const char* env = cli_getenv("FLATPKG_STORE");
		if (env && *env) return env;
	}
	return FLATPKG_STORE_DIR;
}

void store_pointer_path(char* out, size_t outlen, const char* id) {
	assert(out != NULL);
	assert(outlen > 0);
	assert(id != NULL);
	snprintf(out, outlen, "%s/app/%s/pointer", store_root(), id);
}

int store_pointer_export(const char* id, char* export_dir, size_t edirlen,
			 char* arch, size_t archlen, char* branch,
			 size_t branchlen) {
	assert(id != NULL);
	assert(export_dir != NULL);
	assert(edirlen > 0);
	assert(arch != NULL);
	assert(archlen > 0);
	assert(branch != NULL);
	assert(branchlen > 0);
	char pointer[PATH_MAX];
	store_pointer_path(pointer, sizeof(pointer), id);

	FILE* pf = fopen(pointer, "r");
	if (!pf) return -1;
	char line[PATH_MAX], app_files[PATH_MAX] = "";
	while (fgets(line, sizeof(line), pf)) {
		line[strcspn(line, "\n")] = '\0';
		if (strncmp(line, "app_files=", 10) == 0)
			snprintf(app_files, sizeof(app_files), "%s", line + 10);
	}
	fclose(pf);
	if (!app_files[0]) return -1;

	snprintf(export_dir, edirlen, "%s", app_files);
	char* slash = strrchr(export_dir, '/');
	if (!slash) return -1;
	snprintf(slash + 1, edirlen - (size_t)(slash + 1 - export_dir),
		 "export");

	snprintf(line, sizeof(line), "%s", app_files);
	char* p = strstr(line, "/active/files");
	if (!p) return -1;
	*p = '\0';
	p = strrchr(line, '/');
	if (!p) return -1;
	snprintf(branch, branchlen, "%s", p + 1);
	*p = '\0';
	p = strrchr(line, '/');
	if (!p) return -1;
	snprintf(arch, archlen, "%s", p + 1);
	return 0;
}

int find_active_dir(const char* base, const char* component, const char* id,
		    char* out, size_t outlen) {
	assert(base != NULL);
	assert(component != NULL);
	assert(out != NULL);
	assert(outlen > 0);
	char root[PATH_MAX];
	if (id) snprintf(root, sizeof(root), "%s/%s/%s", base, component, id);
	else
		snprintf(root, sizeof(root), "%s/%s", base, component);

	DIR* d1 = opendir(root);
	if (!d1) return -1;
	struct dirent* arch_ent;
	while ((arch_ent = readdir(d1))) {
		if (arch_ent->d_name[0] == '.') continue;
		char archdir[PATH_MAX];
		snprintf(archdir, sizeof(archdir), "%s/%s", root,
			 arch_ent->d_name);
		DIR* d2 = opendir(archdir);
		if (!d2) continue;
		struct dirent* branch_ent;
		while ((branch_ent = readdir(d2))) {
			if (branch_ent->d_name[0] == '.') continue;
			char active[PATH_MAX];
			snprintf(active, sizeof(active), "%s/%s/active",
				 archdir, branch_ent->d_name);
			struct stat st;
			if (stat(active, &st) == 0 && S_ISDIR(st.st_mode)) {
				snprintf(out, outlen, "%s", active);
				closedir(d2);
				closedir(d1);
				return 0;
			}
		}
		closedir(d2);
	}
	closedir(d1);
	return -1;
}

int store_list_all(store_list_entry_t*** entries_out, size_t* n_out) {
	assert(entries_out != NULL);
	assert(n_out != NULL);
	DIR* d = opendir(store_root());
	if (!d) {
		fpkg_errorf("store not initialized (try: flatpkg import "
			    "<path> or flatpkg install <ref>)");
		return -1;
	}

	store_list_entry_t** entries = NULL;
	size_t n = 0, cap = 0;

	static const struct {
		const char* dir;
		const char* kind;
	} kinds[] = {{"app", "app"},
		     {"runtime", "runtime"},
		     {"extension", "extension"},
		     {NULL, NULL}};
	for (int k = 0; kinds[k].dir; k++) {
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/%s", store_root(),
			 kinds[k].dir);
		DIR* sd = opendir(path);
		if (!sd) continue;
		struct dirent* ent;
		while ((ent = readdir(sd))) {
			if (ent->d_name[0] == '.') continue;
			char iddir[PATH_MAX];
			snprintf(iddir, sizeof(iddir), "%s/%s", path,
				 ent->d_name);
			DIR* idir = opendir(iddir);
			if (!idir) continue;
			struct dirent* arch_ent;
			bool has_deploy = false;
			while ((arch_ent = readdir(idir))) {
				if (arch_ent->d_name[0] == '.') continue;
				char archdir[PATH_MAX];
				snprintf(archdir, sizeof(archdir), "%s/%s",
					 iddir, arch_ent->d_name);
				DIR* adir = opendir(archdir);
				if (!adir) continue;
				struct dirent* br_ent;
				while ((br_ent = readdir(adir))) {
					if (br_ent->d_name[0] == '.') continue;
					char active[PATH_MAX];
					snprintf(active, sizeof(active),
						 "%s/%s/active", archdir,
						 br_ent->d_name);
					struct stat st;
					if (stat(active, &st) == 0 &&
					    S_ISDIR(st.st_mode)) {
						has_deploy = true;
						break;
					}
				}
				closedir(adir);
				if (has_deploy) break;
			}
			closedir(idir);
			if (!has_deploy && strcmp(kinds[k].kind, "app") == 0) {
				char pointer[PATH_MAX];
				store_pointer_path(pointer, sizeof(pointer),
						   ent->d_name);
				struct stat pst;
				has_deploy = stat(pointer, &pst) == 0 &&
					     S_ISREG(pst.st_mode);
			}
			if (!has_deploy) continue;
			if (n == cap) {
				cap = cap ? cap * 2 : 32;
				store_list_entry_t** new_entries = realloc(
					entries, cap * sizeof(*entries));
				if (!new_entries) {
					fpkg_errorf("out of memory");
					exit(1);
				}
				entries = new_entries;
			}
			entries[n] = malloc(sizeof(store_list_entry_t));
			entries[n]->id = strdup(ent->d_name);
			entries[n]->kind = strdup(kinds[k].kind);
			n++;
		}
		closedir(sd);
	}
	closedir(d);

	if (n == 0) {
		free(entries);
		*entries_out = NULL;
		*n_out = 0;
		return 0;
	}

	*entries_out = entries;
	*n_out = n;
	return 0;
}

void store_list_all_free(store_list_entry_t** entries, size_t n) {
	assert(entries != NULL || n == 0);
	for (size_t i = 0; i < n; i++) {
		free(entries[i]->id);
		free(entries[i]->kind);
		free(entries[i]);
	}
	free(entries);
}

int store_list(char*** ids_out, size_t* n_out) {
	assert(ids_out != NULL);
	assert(n_out != NULL);
	store_list_entry_t** entries = NULL;
	size_t n = 0;
	int rc = store_list_all(&entries, &n);
	if (rc != 0) return rc;

	char** ids = malloc((n > 0 ? n : 1) * sizeof(char*));
	if (!ids) {
		fpkg_errorf("out of memory");
		exit(1);
	}
	size_t j = 0;
	for (size_t i = 0; i < n; i++) {
		if (strcmp(entries[i]->kind, "app") == 0)
			ids[j++] = strdup(entries[i]->id);
	}
	store_list_all_free(entries, n);

	if (j == 0) {
		free(ids);
		*ids_out = NULL;
		*n_out = 0;
		return 0;
	}

	*ids_out = ids;
	*n_out = j;
	return 0;
}

void parse_app_ref(const char* ref, char* id, size_t idlen, char* arch,
		   size_t archlen, char* branch, size_t branchlen) {
	assert(ref != NULL);
	assert(id != NULL);
	assert(idlen > 0);
	assert(arch != NULL);
	assert(archlen > 0);
	assert(branch != NULL);
	assert(branchlen > 0);
	id[0] = arch[0] = branch[0] = '\0';

	const char* rest = ref;
	const char* colon = strchr(ref, ':');
	const char* slash = strchr(ref, '/');
	if (colon && (!slash || colon < slash)) rest = colon + 1;

	const char* p1 = strchr(rest, '/');
	if (!p1) {
		snprintf(id, idlen, "%s", rest);
		return;
	}
	snprintf(id, idlen, "%.*s", (int)(p1 - rest), rest);
	const char* p2 = strchr(p1 + 1, '/');
	if (!p2) {
		snprintf(arch, archlen, "%s", p1 + 1);
		return;
	}
	if (p2 > p1 + 1)
		snprintf(arch, archlen, "%.*s", (int)(p2 - p1 - 1), p1 + 1);
	snprintf(branch, branchlen, "%s", p2 + 1);
}

static int resolve_runtime_deploy(const fp_metadata_t* meta, char* rt_files_out,
				  size_t outlen) {
	assert(meta != NULL);
	assert(rt_files_out != NULL);
	assert(outlen > 0);
	char rid[256] = "", rarch[64] = "", rbranch[64] = "";
	sscanf(meta->runtime_ref ? meta->runtime_ref : "",
	       "%255[^/]/%63[^/]/%63s", rid, rarch, rbranch);
	if (!rid[0]) return -1;

	char rt_active[PATH_MAX] = "";
	if (rbranch[0]) {
		snprintf(rt_active, sizeof(rt_active),
			 "%s/runtime/%s/%s/%s/"
			 "active",
			 store_root(), rid, rarch[0] ? rarch : "x86_64",
			 rbranch);
		struct stat st;
		if (stat(rt_active, &st) != 0 || !S_ISDIR(st.st_mode))
			rt_active[0] = '\0';
	}
	if (!rt_active[0]) {
		char rt_component[PATH_MAX];
		snprintf(rt_component, sizeof(rt_component), "runtime/%s", rid);
		find_active_dir(store_root(), rt_component, NULL, rt_active,
				sizeof(rt_active));
	}
	if (!rt_active[0]) return -1;

	snprintf(rt_files_out, outlen, "%s/files", rt_active);
	return 0;
}

int store_resolve(const char* app_id, sandbox_t* sb_out,
		  fp_metadata_t* meta_out) {
	assert(app_id != NULL);
	assert(sb_out != NULL);
	assert(meta_out != NULL);
	char id[256], arch[64], branch[64];
	parse_app_ref(app_id, id, sizeof(id), arch, sizeof(arch), branch,
		      sizeof(branch));
	if (!id[0]) {
		fpkg_errorf("invalid app id '%s'", app_id);
		return -1;
	}

	char active[PATH_MAX] = "";
	if (branch[0]) {
		snprintf(active, sizeof(active), "%s/app/%s/%s/%s/active",
			 store_root(), id, arch[0] ? arch : "x86_64", branch);
		struct stat st;
		if (stat(active, &st) != 0 || !S_ISDIR(st.st_mode))
			active[0] = '\0';
	} else {
		find_active_dir(store_root(), "app", id, active,
				sizeof(active));
	}

	if (active[0]) {
		char meta_path[PATH_MAX], files_path[PATH_MAX];
		snprintf(meta_path, sizeof(meta_path), "%s/metadata", active);
		snprintf(files_path, sizeof(files_path), "%s/files", active);
		if (fp_metadata_parse(meta_path, meta_out) != 0) {
			fpkg_errorf("cannot parse metadata at %s", meta_path);
			return -1;
		}

		memset(sb_out, 0, sizeof(*sb_out));
		sb_out->app_id = strdup(id);
		sb_out->app_files_dir = strdup(files_path);

		char rt_files[PATH_MAX];
		if (resolve_runtime_deploy(meta_out, rt_files,
					   sizeof(rt_files)) != 0) {
			fpkg_errorf("runtime '%s' for '%s' is not installed "
				    "(flatpkg install the runtime first)",
				    meta_out->runtime_ref
					    ? meta_out->runtime_ref
					    : "?",
				    id);
			fp_metadata_free(meta_out);
			free(sb_out->app_id);
			free(sb_out->app_files_dir);
			return -1;
		}
		sb_out->runtime_dir = strdup(rt_files);
		return 0;
	}

	char pointer_path[PATH_MAX];
	store_pointer_path(pointer_path, sizeof(pointer_path), id);

	FILE* pf = fopen(pointer_path, "r");
	if (!pf) {
		fpkg_errorf("unknown app '%s' (try: flatpkg list, or "
			    "flatpkg install %s)",
			    id, id);
		return -1;
	}

	char line[PATH_MAX];
	char app_files[PATH_MAX] = "", runtime_files[PATH_MAX] = "";
	char metadata_path[PATH_MAX] = "";
	while (fgets(line, sizeof(line), pf)) {
		line[strcspn(line, "\n")] = '\0';
		if (strncmp(line, "app_files=", 10) == 0)
			snprintf(app_files, sizeof(app_files), "%s", line + 10);
		else if (strncmp(line, "runtime_files=", 14) == 0)
			snprintf(runtime_files, sizeof(runtime_files), "%s",
				 line + 14);
		else if (strncmp(line, "metadata=", 9) == 0)
			snprintf(metadata_path, sizeof(metadata_path), "%s",
				 line + 9);
	}
	fclose(pf);

	if (!app_files[0] || !runtime_files[0] || !metadata_path[0]) {
		fpkg_errorf("corrupt store entry for '%s'", id);
		return -1;
	}

	// An imported entry points into the tree the caller named.  The import
	// checked the types; check them again here, because that tree can be
	// changed after the import and these paths are mounted as root.
	struct stat st;
	if (lstat(app_files, &st) != 0 || !S_ISDIR(st.st_mode) ||
	    lstat(runtime_files, &st) != 0 || !S_ISDIR(st.st_mode) ||
	    lstat(metadata_path, &st) != 0 || !S_ISREG(st.st_mode)) {
		fpkg_errorf("imported entry '%s' changed on disk; re-import it",
			    id);
		return -1;
	}

	if (fp_metadata_parse(metadata_path, meta_out) != 0) return -1;

	memset(sb_out, 0, sizeof(*sb_out));
	sb_out->app_id = strdup(id);
	sb_out->app_files_dir = strdup(app_files);
	sb_out->runtime_dir = strdup(runtime_files);
	return 0;
}

// The store-facing world API: thin wrappers over world_load()/world_save(),
// so the worldfile has one parser and one save path.  A missing file is an
// empty world; a file that exists but cannot be read is an error.

int world_has(const char* id) {
	assert(id != NULL);
	flatpkg_dep_array_t world = {0};
	if (world_load(&world) != 0) return 0;
	int found = world_dep_has(&world, id) ? 1 : 0;
	world_free(&world);
	return found;
}

int store_deploy_in_use(const char* kind, const char* id) {
	assert(kind != NULL);
	assert(id != NULL);
	char path[PATH_MAX];
	if ((size_t)snprintf(path, sizeof(path), "%s/%s/%s", store_root(), kind,
			     id) >= sizeof(path))
		return 0;
	return instance_uses_path(path);
}

int store_find_extension(const char* id, const char* arch, const char* branch,
			 char* files_out, size_t outlen) {
	assert(id != NULL);
	assert(arch != NULL);
	assert(branch != NULL);
	assert(files_out != NULL);
	assert(outlen > 0);
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/extension/%s/%s/%s/active/files",
		 store_root(), id, arch, branch);
	struct stat st;
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
		snprintf(files_out, outlen, "%s", path);
		return 0;
	}
	snprintf(path, sizeof(path), "%s/runtime/%s/%s/%s/active/files",
		 store_root(), id, arch, branch);
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
		snprintf(files_out, outlen, "%s", path);
		return 0;
	}
	return -1;
}

bool store_extension_any_branch(const char* id, const char* arch) {
	assert(id != NULL);
	assert(arch != NULL);
	static const char* dirs[] = {"extension", "runtime", NULL};

	for (int i = 0; dirs[i]; i++) {
		char dir[PATH_MAX];
		snprintf(dir, sizeof(dir), "%s/%s/%s/%s", store_root(), dirs[i],
			 id, arch);
		DIR* d = opendir(dir);
		if (!d) continue;
		struct dirent* ent;
		while ((ent = readdir(d))) {
			if (ent->d_name[0] == '.') continue;
			char active[PATH_MAX];
			snprintf(active, sizeof(active), "%s/%s/active", dir,
				 ent->d_name);
			struct stat st;
			if (stat(active, &st) == 0 && S_ISDIR(st.st_mode)) {
				closedir(d);
				return true;
			}
		}
		closedir(d);
	}
	return false;
}

int store_list_extensions(const char* id_prefix, const char* arch,
			  const char* branch, char*** ids_out, size_t* n_out) {
	assert(id_prefix != NULL);
	assert(arch != NULL);
	assert(branch != NULL);
	assert(ids_out != NULL);
	assert(n_out != NULL);
	*ids_out = NULL;
	*n_out = 0;

	const char* dirs[] = {"extension", "runtime", NULL};
	char** ids = NULL;
	size_t n = 0, cap = 0;

	for (int di = 0; dirs[di]; di++) {
		char root[PATH_MAX];
		snprintf(root, sizeof(root), "%s/%s", store_root(), dirs[di]);
		DIR* d = opendir(root);
		if (!d) continue;

		size_t plen = strlen(id_prefix);
		struct dirent* ent;
		while ((ent = readdir(d))) {
			if (ent->d_name[0] == '.') continue;
			if (strncmp(ent->d_name, id_prefix, plen) != 0)
				continue;
			if (ent->d_name[plen] != '.') continue;
			char active[PATH_MAX];
			snprintf(active, sizeof(active), "%s/%s/%s/%s/active",
				 root, ent->d_name, arch, branch);
			struct stat st;
			if (stat(active, &st) != 0 || !S_ISDIR(st.st_mode))
				continue;
			bool dup = false;
			for (size_t i = 0; i < n; i++) {
				if (strcmp(ids[i], ent->d_name) == 0) {
					dup = true;
					break;
				}
			}
			if (dup) continue;
			if (n == cap) {
				cap = cap ? cap * 2 : 8;
				char** new_ids =
					realloc(ids, cap * sizeof(char*));
				if (!new_ids) {
					fpkg_errorf("out of memory");
					exit(1);
				}
				ids = new_ids;
			}
			ids[n++] = strdup(ent->d_name);
		}
		closedir(d);
	}

	*ids_out = ids;
	*n_out = n;
	return 0;
}
