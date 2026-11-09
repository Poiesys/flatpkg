#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include "flatpkg.h"
#include "protocol.h"
#define FPKG_MODULE "store"
#include "fpkglog.h"


int store_import(const char* src_root, char*** imported_out, size_t* n_out) {
	assert(src_root != NULL);
	assert(imported_out != NULL);
	assert(n_out != NULL);
	char apps_root[PATH_MAX];
	snprintf(apps_root, sizeof(apps_root), "%s/app", src_root);
	DIR* apps = opendir(apps_root);
	if (!apps) {
		fpkg_errorf("'%s' has no app/ directory - is this a Flatpak "
			    "deploy root?",
			    apps_root);
		return -1;
	}

	const char* store = store_root();
	if (mkdir_p(store, 0755) != 0) {
		fpkg_errorf("mkdir %s: %s", store, strerror(errno));
		closedir(apps);
		return -1;
	}

	char** imported = NULL;
	size_t nimported = 0, cap = 0;
	struct dirent* ent;
	while ((ent = readdir(apps))) {
		if (ent->d_name[0] == '.') continue;

		char active[PATH_MAX];
		if (find_active_dir(apps_root, ent->d_name, NULL, active,
				    sizeof(active)) != 0) {
			fpkg_warnf("skipping '%s': no active/ deploy found",
				   ent->d_name);
			continue;
		}

		char meta_path[PATH_MAX], files_path[PATH_MAX];
		snprintf(meta_path, sizeof(meta_path), "%s/metadata", active);
		snprintf(files_path, sizeof(files_path), "%s/files", active);

		// The tree is the caller's and these paths are mounted as root later: only a
		// regular file and a directory, no symlink and no FIFO that would hold the worker.
		struct stat st;
		if (lstat(meta_path, &st) != 0 || !S_ISREG(st.st_mode)) {
			fpkg_warnf("skipping '%s': metadata is not a regular file",
				   ent->d_name);
			continue;
		}
		if (lstat(files_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
			fpkg_warnf("skipping '%s': files is not a directory",
				   ent->d_name);
			continue;
		}

		fp_metadata_t meta;
		if (fp_metadata_parse(meta_path, &meta) != 0) {
			fpkg_warnf("skipping '%s': bad metadata", ent->d_name);
			continue;
		}

		char runtime_id[256] = "", runtime_arch[64] = "",
		     runtime_branch[64] = "";
		sscanf(meta.runtime_ref ? meta.runtime_ref : "",
		       "%255[^/]/%63[^/]/%63s", runtime_id, runtime_arch,
		       runtime_branch);

		char runtime_active[PATH_MAX] = "";
		if (runtime_id[0]) {
			char exact[PATH_MAX];
			snprintf(exact, sizeof(exact),
				 "%s/runtime/%s/%s/%s/"
				 "active",
				 src_root, runtime_id, runtime_arch,
				 runtime_branch);
			struct stat st;
			if (stat(exact, &st) == 0 && S_ISDIR(st.st_mode)) {
				snprintf(runtime_active, sizeof(runtime_active),
					 "%s", exact);
			} else {
				char rt_component[PATH_MAX];
				snprintf(rt_component, sizeof(rt_component),
					 "runtime/%s", runtime_id);
				find_active_dir(src_root, rt_component, NULL,
						runtime_active,
						sizeof(runtime_active));
			}
		}
		if (!runtime_active[0]) {
			fpkg_warnf(
				"skipping '%s': runtime '%s' not found under "
				"%s/runtime/ - import the runtime tree too",
				ent->d_name,
				meta.runtime_ref ? meta.runtime_ref : "?",
				src_root);
			fp_metadata_free(&meta);
			continue;
		}

		char runtime_files[PATH_MAX];
		snprintf(runtime_files, sizeof(runtime_files), "%s/files",
			 runtime_active);
		if (lstat(runtime_files, &st) != 0 || !S_ISDIR(st.st_mode)) {
			fpkg_warnf("skipping '%s': runtime files is not a "
				   "directory",
				   ent->d_name);
			fp_metadata_free(&meta);
			continue;
		}

		char store_app_dir[PATH_MAX];
		snprintf(store_app_dir, sizeof(store_app_dir), "%s/app/%s",
			 store, ent->d_name);
		mkdir_p(store_app_dir, 0755);

		char pointer_path[PATH_MAX];
		snprintf(pointer_path, sizeof(pointer_path), "%s/pointer",
			 store_app_dir);
		FILE* pf = fopen(pointer_path, "w");
		if (!pf) {
			fpkg_errorf("fopen %s: %s", pointer_path,
				    strerror(errno));
			fp_metadata_free(&meta);
			continue;
		}
		fprintf(pf, "app_files=%s\n", files_path);
		fprintf(pf, "runtime_files=%s\n", runtime_files);
		fprintf(pf, "metadata=%s\n", meta_path);
		fclose(pf);

		if (nimported == cap) {
			cap = cap ? cap * 2 : 8;
			char** new_imported =
				realloc(imported, cap * sizeof(char*));
			if (!new_imported) {
				fpkg_errorf("out of memory");
				exit(1);
			}
			imported = new_imported;
		}
		imported[nimported++] = strdup(ent->d_name);

		fp_metadata_free(&meta);
	}
	closedir(apps);

	if (nimported == 0) {
		fpkg_errorf("nothing imported from '%s'", src_root);
		return -1;
	}

	if (imported_out) {
		*imported_out = imported;
		*n_out = nimported;
	} else {
		for (size_t i = 0; i < nimported; i++) free(imported[i]);
		free(imported);
	}
	return 0;
}

int world_add(const char* id) {
	assert(id != NULL);
	// world_load() seeds a missing worldfile, which covers first run.
	flatpkg_dep_array_t world = {0};
	if (world_load(&world) != 0) return -1;
	int rc = 0;
	if (!world_dep_has(&world, id)) {
		flatpkg_dep_t dep = {0};
		dep.kind = FLATPKG_DEP_KIND_APP;
		if (world_dep_parse(id, &dep) != 0) {
			fpkg_errorf("'%s' is not a world entry", id);
			rc = -1;
		} else {
			rc = world_dep_add(&world, &dep);
			if (rc == 0) rc = world_save(&world);
			free(dep.name);
			free(dep.remote);
			free(dep.branch);
		}
	}
	world_free(&world);
	return rc;
}

int world_seed(void) {
	char path[PATH_MAX], legacy[PATH_MAX];
	if (flatpkg_client_path("flatpkg", "world", path, sizeof(path)) != 0)
		return -1;
	snprintf(legacy, sizeof(legacy), "%s/world", store_root());
	struct stat st;
	if (stat(path, &st) == 0) return 0;
	if (stat(legacy, &st) == 0) return 0;
	char** ids = NULL;
	size_t n = 0;
	if (store_list(&ids, &n) != 0 || n == 0) return 0;
	flatpkg_dep_array_t world = {0};
	for (size_t i = 0; i < n; i++) {
		// world_dep_add() copies, so the borrow is safe.
		flatpkg_dep_t dep = {0};
		dep.kind = FLATPKG_DEP_KIND_APP;
		dep.name = ids[i];
		world_dep_add(&world, &dep);
	}
	int rc = world_save(&world);
	world_free(&world);
	for (size_t i = 0; i < n; i++) free(ids[i]);
	free(ids);
	return rc;
}

// Remove a tree with a determinate progress bar (fts(3)+FTS_NOSTAT, as rm -rf
// uses). Refuses a tree a live instance has mounted: nullfs holds the vnodes.
int store_remove_tree(const char* path, const char* label,
		      const char* done_msg) {
	assert(path != NULL);
	if (instance_uses_path(path)) {
		fpkg_errorf("%s is in use by a running app; stop it first",
			    path);
		return -1;
	}

	char* paths[2] = {(char*)path, NULL};
	FTS* ft = fts_open(paths, FTS_PHYSICAL | FTS_NOSTAT | FTS_XDEV, NULL);
	if (!ft) {
		fpkg_warnf("cannot open %s: %s", path, strerror(errno));
		return -1;
	}

	unsigned total = 0;
	FTSENT* e;
	while ((e = fts_read(ft)) != NULL)
		if (e->fts_info != FTS_D) total++;
	fts_close(ft);

	ft = fts_open(paths, FTS_PHYSICAL | FTS_NOSTAT | FTS_XDEV, NULL);
	if (!ft) {
		fpkg_warnf("cannot open %s: %s", path, strerror(errno));
		return -1;
	}

	// No label: routine housekeeping that gets no bar and no result line.
	fp_bar_t pb = fp_bar_init(FPKG_MODULE, "", label ? label : "", 100);
	fp_bar_set_throttle(&pb, 100);
	if (label) fp_bar_update(&pb, 0);

	unsigned n = 0;
	int rc = 0;
	while ((e = fts_read(ft)) != NULL) {
		switch (e->fts_info) {
		case FTS_DP:
		case FTS_DNR:
			if (rmdir(e->fts_accpath) != 0 && errno != ENOENT)
				rc = -1;
			break;
		case FTS_D:
			break;
		default:
			if (unlink(e->fts_accpath) != 0 && errno != ENOENT)
				rc = -1;
			break;
		}
		n++;
		if (label && total > 0) fp_bar_update(&pb, (n * 100) / total);
	}
	fts_close(ft);

	if (!label) {
		if (rc != 0) fpkg_warnf("could not remove all of %s", path);
		return rc;
	}
	fp_bar_finishf(&pb, rc == 0, "%s", done_msg ? done_msg : "done");
	return rc;
}

#define FPKG_FLATPAK_BIN "/usr/local/bin/flatpak"

static int mkdir_parents_for(const char* path) {
	assert(path != NULL);
	char buf[PATH_MAX];
	snprintf(buf, sizeof(buf), "%s", path);
	char* slash = strrchr(buf, '/');
	if (!slash || slash == buf) return 0;
	*slash = '\0';
	return mkdir_p(buf, 0755);
}

static int export_link(const char* store, const char* app_id, const char* rel,
		       const char* abs_base) {
	assert(store != NULL);
	assert(app_id != NULL);
	assert(rel != NULL);
	char link[PATH_MAX], target[PATH_MAX], prefix[64] = "";
	size_t up = 1, p = 0;

	snprintf(link, sizeof(link), "%s/exports/%s", store, rel);
	if (abs_base) {
		snprintf(target, sizeof(target), "%s/%s", abs_base, rel);
	} else {
		for (const char* c = rel; *c; c++)
			if (*c == '/') up++;
		for (size_t i = 0; i < up && p + 4 < sizeof(prefix); i++)
			p += (size_t)snprintf(prefix + p, sizeof(prefix) - p,
					      "../");

		snprintf(target, sizeof(target),
			 "%sapp/%s/current/active/export/%s", prefix, app_id,
			 rel);
	}
	if (mkdir_parents_for(link) != 0) return -1;
	struct stat est;
	if (lstat(link, &est) == 0 && S_ISDIR(est.st_mode)) {
		fpkg_warnf("exports: cannot export '%s' of %s as a file: "
			   "another app already owns that name as a directory; "
			   "skipping it",
			   rel, app_id);
		return -1;
	}
	(void)unlink(link);
	return symlink(target, link) == 0 ? 0 : -1;
}

static int export_desktop(const char* store, const char* src, const char* rel,
			  const char* app_id, const char* arch,
			  const char* branch) {
	assert(store != NULL);
	assert(src != NULL);
	assert(rel != NULL);
	assert(app_id != NULL);
	assert(arch != NULL);
	assert(branch != NULL);
	char dst[PATH_MAX], line[4096];
	FILE *in, *out;

	snprintf(dst, sizeof(dst), "%s/exports/%s", store, rel);
	if (mkdir_parents_for(dst) != 0) return -1;
	in = fopen(src, "r");
	if (!in) return -1;
	out = fopen(dst, "w");
	if (!out) {
		fclose(in);
		return -1;
	}

	while (fgets(line, sizeof(line), in)) {
		if (strncmp(line, "TryExec=", 8) == 0) {
			// The binary only exists inside the sandbox; a launcher
			// that honours TryExec must not hide the entry.
			continue;
		}
		if (strncmp(line, "Exec=", 5) != 0) {
			fputs(line, out);
			continue;
		}

		char rest[3584] = "";
		char cmd[512] = "";
		{
			const char* v = line + 5;
			while (*v == ' ' || *v == '\t') v++;
			size_t ci = 0;
			while (*v && *v != ' ' && *v != '\t' && *v != '\n' &&
			       ci + 1 < sizeof(cmd))
				cmd[ci++] = *v++;
			cmd[ci] = '\0';
			while (*v == ' ' || *v == '\t') v++;
			snprintf(rest, sizeof(rest), "%s", v);
			size_t rl = strlen(rest);
			while (rl > 0 &&
			       (rest[rl - 1] == '\n' || rest[rl - 1] == '\r'))
				rest[--rl] = '\0';
		}
		if (cmd[0] == '\0') {
			fputs(line, out);
			continue;
		}

		bool file_forward = false;
		for (const char* c = rest; *c; c++)
			if ((c[0] == '%' && (c[1] == 'f' || c[1] == 'F' ||
					     c[1] == 'u' || c[1] == 'U')) &&
			    (c == rest || c[-1] == ' '))
				file_forward = true;

		fprintf(out,
			"Exec=%s run --branch=%s --arch=%s "
			"--command=%s%s %s",
			FPKG_FLATPAK_BIN, branch, arch, cmd,
			file_forward ? " --file-forwarding" : "", app_id);

		if (file_forward) {
			char tok[4096];
			snprintf(tok, sizeof(tok), "%s", rest);
			char* save = NULL;
			for (char* t = strtok_r(tok, " \t", &save); t;
			     t = strtok_r(NULL, " \t", &save)) {
				if (strcmp(t, "%f") == 0 ||
				    strcmp(t, "%F") == 0)
					fprintf(out, " @@ %s @@", t);
				else if (strcmp(t, "%u") == 0 ||
					 strcmp(t, "%U") == 0)
					fprintf(out, " @@u %s @@", t);
				else
					fprintf(out, " %s", t);
			}
		} else if (rest[0]) {
			fprintf(out, " %s", rest);
		}
		fputc('\n', out);
	}
	fclose(in);
	if (fclose(out) != 0) return -1;
	return 0;
}

// Make <store>/exports/<rel> a directory, or warn naming the app and path:
// mkdir_p() would quietly mean "make one somewhere else".
static int export_mkdir(const char* store, const char* path, const char* app_id,
			const char* rel) {
	assert(store != NULL);
	assert(path != NULL);
	assert(app_id != NULL);
	struct stat st;
	if (lstat(path, &st) == 0) {
		if (S_ISDIR(st.st_mode)) return 0;
		fpkg_warnf("exports: cannot export '%s' of %s as a "
			   "directory: another app already owns that name as a "
			   "file; skipping it",
			   rel, app_id);
		return -1;
	}
	if (mkdir(path, 0755) != 0 && errno != EEXIST) {
		fpkg_warnf("exports: cannot create %s: %s", path,
			   strerror(errno));
		return -1;
	}
	return 0;
}

static int export_walk(const char* store, const char* app_id,
		       const char* export_dir, const char* rel,
		       const char* arch, const char* branch,
		       const char* abs_base) {
	assert(store != NULL);
	assert(app_id != NULL);
	assert(export_dir != NULL);
	assert(arch != NULL);
	assert(branch != NULL);
	char dir[PATH_MAX];
	DIR* d;
	int rc = 0;

	snprintf(dir, sizeof(dir), "%s/%s", export_dir, rel);
	d = opendir(dir);
	if (!d) return -1;

	struct dirent* de;
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		char child_rel[PATH_MAX], child_src[PATH_MAX];
		snprintf(child_rel, sizeof(child_rel), "%s%s%s", rel,
			 rel[0] ? "/" : "", de->d_name);
		snprintf(child_src, sizeof(child_src), "%s/%s", dir,
			 de->d_name);

		struct stat st;
		if (lstat(child_src, &st) != 0) continue;

		if (S_ISDIR(st.st_mode)) {
			char dst[PATH_MAX];
			snprintf(dst, sizeof(dst), "%s/exports/%s", store,
				 child_rel);
			if (export_mkdir(store, dst, app_id, child_rel) != 0)
				rc = -1;
			else if (export_walk(store, app_id, export_dir,
					     child_rel, arch, branch,
					     abs_base) != 0)
				rc = -1;
			continue;
		}

		// Rewrite .desktop and .service Exec= lines: both would
		// otherwise exec a binary that only exists inside the sandbox.
		size_t rl = strlen(child_rel);
		bool is_desktop =
			(rl > 8 &&
			 strcmp(child_rel + rl - 8, ".desktop") == 0 &&
			 strstr(child_rel, "applications/") != NULL) ||
			(rl > 8 &&
			 strcmp(child_rel + rl - 8, ".service") == 0 &&
			 strstr(child_rel, "dbus-1/services/") != NULL);
		if (is_desktop && S_ISREG(st.st_mode)) {
			if (export_desktop(store, child_src, child_rel, app_id,
					   arch, branch) != 0)
				rc = -1;
		} else if (export_link(store, app_id, child_rel, abs_base) != 0)
			rc = -1;
	}
	closedir(d);
	return rc;
}

static int prune_dangling(const char* dir) {
	assert(dir != NULL);
	DIR* d = opendir(dir);
	if (!d) return -1;
	int dfd = dirfd(d);
	if (dfd < 0) {
		closedir(d);
		return -1;
	}
	struct dirent* de;
	while ((de = readdir(d)) != NULL) {
		if (strcmp(de->d_name, ".") == 0 ||
		    strcmp(de->d_name, "..") == 0)
			continue;
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
		struct stat st;
		if (fstatat(dfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			prune_dangling(path);
			continue;
		}
		// A symlink whose target no longer resolves: drop the link, not the
		// path it names, and keep both operations on the open directory.
		if (S_ISLNK(st.st_mode) && fstatat(dfd, de->d_name, &st, 0) != 0)
			(void)unlinkat(dfd, de->d_name, 0);
	}
	closedir(d);
	return 0;
}

int store_update_exports(void) {
	const char* store = store_root();
	char** ids = NULL;
	size_t n = 0;
	int exported = 0;

	char farm[PATH_MAX], root[PATH_MAX];
	snprintf(farm, sizeof(farm), "%s/exports", store);
	snprintf(root, sizeof(root), "%s/share", farm);

	if (access(farm, F_OK) == 0 && store_remove_tree(farm, NULL, NULL) != 0)
		fpkg_warnf("could not clear the old exports farm; stale "
			   "entries may remain");
	if (mkdir_p(root, 0755) != 0) {
		fpkg_errorf("cannot create %s: %s", root, strerror(errno));
		return -1;
	}

	if (store_list(&ids, &n) != 0) return -1;
	for (size_t i = 0; i < n; i++) {
		char* id = ids[i];
		ids[i] = NULL;

		char cur[PATH_MAX], link[PATH_MAX], export_dir[PATH_MAX];
		const char *arch = NULL, *branch = NULL;
		char iarch[64] = "", ibranch[64] = "";
		const char* import_base = NULL;
		snprintf(cur, sizeof(cur), "%s/app/%s/current", store, id);
		ssize_t ll = readlink(cur, link, sizeof(link) - 1);
		if (ll > 0) {
			link[ll] = '\0';
			char* slash = strchr(link, '/');
			if (slash == NULL) {
				free(id);
				continue;
			}
			*slash = '\0';
			arch = link;
			branch = slash + 1;
			snprintf(export_dir, sizeof(export_dir),
				 "%s/active/export", cur);
		} else {
			if (store_pointer_export(id, export_dir,
						 sizeof(export_dir), iarch,
						 sizeof(iarch), ibranch,
						 sizeof(ibranch)) != 0) {
				free(id);
				continue;
			}
			arch = iarch;
			branch = ibranch;
			import_base = export_dir;
		}
		struct stat est;
		if (stat(export_dir, &est) != 0 || !S_ISDIR(est.st_mode)) {
			free(id);
			continue;
		}

		if (export_walk(store, id, export_dir, "", arch, branch,
				import_base) == 0)
			exported++;
		else
			fpkg_warnf("exports: %s did not export completely", id);
		free(id);
	}
	free(ids);
	prune_dangling(root);
	if (flatpkg_txn_active())
		fpkg_debugf("exports refreshed: %d app%s", exported,
			    exported == 1 ? "" : "s");
	else
		fpkg_okf("exports refreshed: %d app%s", exported,
			 exported == 1 ? "" : "s");
	return 0;
}

int cmd_exports(void) {
	char** ids = NULL;
	size_t n = 0;
	bool any = store_list(&ids, &n) == 0 && n > 0;
	if (ids) {
		for (size_t i = 0; i < n; i++) free(ids[i]);
		free(ids);
	}
	if (!any) {
		fpkg_infof("nothing installed yet (try: flatpkg import "
			   "<path> or flatpkg install <ref>)");
		return 0;
	}
	return store_update_exports();
}

// Change or reset the SYSTEM override <store>/overrides/<id>. overrides_path()
// checks the app id, and the caller's text is re-serialised to keys the parser knows.
int cmd_override_system(const char* app_id, unsigned flags,
			const char* changes) {
	const char* id = (app_id != NULL && *app_id != '\0') ? app_id : NULL;
	if ((flags & FP_OVERRIDE_RESET) != 0)
		return overrides_reset(false, id) == 0 ? 0 : 1;
	if (changes == NULL || strlen(changes) > FP_OVERRIDE_MAX) {
		fpkg_errorf("the override is larger than %u bytes",
			    (unsigned)FP_OVERRIDE_MAX);
		return 1;
	}
	fp_metadata_t over;
	memset(&over, 0, sizeof(over));
	if (overrides_from_text(changes, store_root(), &over) != 0) {
		fpkg_errorf("the override could not be read");
		fp_metadata_free(&over);
		return 1;
	}
	fp_metadata_t file;
	overrides_load(false, id, &file);
	overrides_merge(&file, &over);
	int rc = overrides_save(false, id, &file) == 0 ? 0 : 1;
	fp_metadata_free(&file);
	fp_metadata_free(&over);
	return rc;
}
