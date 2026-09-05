#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include "flatpkg.h"
#define FPKG_MODULE "repair"
#include "fpkglog.h"

static int check_deploy(const char* type, const char* id, char* arch_out,
			size_t arch_len, char* branch_out, size_t branch_len) {
	assert(type != NULL);
	assert(id != NULL);
	char type_dir[PATH_MAX];
	snprintf(type_dir, sizeof(type_dir), "%s/%s/%s", store_root(), type,
		 id);
	DIR* d = opendir(type_dir);
	if (!d) return -1;
	struct dirent* arch_ent;
	bool any_healthy = false;
	while ((arch_ent = readdir(d))) {
		if (arch_ent->d_name[0] == '.') continue;
		char arch_dir[PATH_MAX];
		snprintf(arch_dir, sizeof(arch_dir), "%s/%s", type_dir,
			 arch_ent->d_name);
		DIR* d2 = opendir(arch_dir);
		if (!d2) continue;
		struct dirent* branch_ent;
		while ((branch_ent = readdir(d2))) {
			if (branch_ent->d_name[0] == '.') continue;
			char active[PATH_MAX];
			snprintf(active, sizeof(active), "%s/%s/active",
				 arch_dir, branch_ent->d_name);
			struct stat st;
			if (lstat(active, &st) != 0) continue;
			if (!S_ISLNK(st.st_mode)) continue;
			if (arch_out != NULL && arch_out[0] == '\0') {
				snprintf(arch_out, arch_len, "%s",
					 arch_ent->d_name);
				snprintf(branch_out, branch_len, "%s",
					 branch_ent->d_name);
			}
			char target[PATH_MAX];
			ssize_t l =
				readlink(active, target, sizeof(target) - 1);
			if (l <= 0) continue;
			target[l] = '\0';
			// active is a relative symlink to the commit name, so
			// resolve it against the branch dir; stat() from the
			// CWD breaks every deploy.
			char resolved[PATH_MAX];
			if (target[0] == '/') {
				snprintf(resolved, sizeof(resolved), "%s",
					 target);
			} else {
				// <arch>/<branch>/<commit>; keep the branch
				// component.
				snprintf(resolved, sizeof(resolved), "%s/%s/%s",
					 arch_dir, branch_ent->d_name, target);
			}
			char files[PATH_MAX];
			snprintf(files, sizeof(files), "%s/files", resolved);
			if (stat(files, &st) != 0 || !S_ISDIR(st.st_mode))
				continue;
			char meta[PATH_MAX];
			snprintf(meta, sizeof(meta), "%s/metadata", resolved);
			fp_metadata_t m;
			if (fp_metadata_parse_any(meta, &m) != 0)
				fpkg_warnf("%s: metadata not parseable",
					   resolved);
			else
				fp_metadata_free(&m);
			any_healthy = true;
		}
		closedir(d2);
	}
	closedir(d);
	return any_healthy ? 0 : -1;
}

// Like `apk fix`: plan what the world implies and the store lacks - a runtime,
// an extension an app asks for - and nothing else.  Report only, never repair:
// a repair that repairs can destroy a working tree.
static int report_world(const flatpkg_dep_array_t* world, unsigned flags) {
	flatpkg_changeset_t cs = {0};
	(void)flags;
	if (solver_solve(world, FLATPKG_SOLVERF_REPAIR, &cs, NULL, 0) != 0) {
		fpkg_errorf("solver failed");
		return 1;
	}
	if (changeset_has_work(&cs)) {
		changeset_show_full(&cs, NULL);
		fpkg_infof("nothing was changed; install --reinstall or upgrade "
			   "the refs above when you are ready");
	}
	changeset_free(&cs);
	return 0;
}

static int repair_locked(unsigned flags) {
	// Zeroed up front so the single out: path can free them from any
	// branch, including the ones that never filled them.
	flatpkg_dep_array_t world = {0};
	char** broken = NULL;
	size_t nbroken = 0, cap_broken = 0;
	int rc;

	if (world_load(&world) != 0) {
		rc = 1;
		goto out;
	}

	static const char* types[] = {"app", "runtime", NULL};
	for (int t = 0; types[t]; t++) {
		char root[PATH_MAX];
		snprintf(root, sizeof(root), "%s/%s", store_root(), types[t]);
		DIR* d1 = opendir(root);
		if (!d1) continue;
		struct dirent* id_ent;
		while ((id_ent = readdir(d1))) {
			if (id_ent->d_name[0] == '.') continue;
			char arch[64] = "", branch[128] = "";
			if (check_deploy(types[t], id_ent->d_name, arch,
					 sizeof(arch), branch,
					 sizeof(branch)) != 0) {
				if (nbroken == cap_broken) {
					size_t ncap = cap_broken
							      ? cap_broken * 2
							      : 16;
					char** nb = realloc(
						broken, ncap * sizeof(char*));
					if (!nb) {
						fpkg_errorf("out of memory");
						closedir(d1);
						rc = 1;
						goto out;
					}
					cap_broken = ncap;
					broken = nb;
				}
				char ref[512];
				if (arch[0] != '\0' && branch[0] != '\0')
					snprintf(ref, sizeof(ref),
						 "%s/%s/%s/%s", types[t],
						 id_ent->d_name, arch, branch);
				else
					snprintf(ref, sizeof(ref), "%s/%s",
						 types[t], id_ent->d_name);
				broken[nbroken++] = xstrdup(ref);
				fpkg_warnf("broken: %s", ref);
			}
		}
		closedir(d1);
	}

	int heal_rc = report_world(&world, flags);

	// The refs were named as they were found (the heal can take a
	// while); the summary carries the advice, not a second list.
	if (nbroken > 0)
		fpkg_warnf("%zu deploy(s) look broken; repair does not touch "
			   "them. Reinstall the refs you want with "
			   "`flatpkg install --reinstall <ref>` or "
			   "`flatpkg upgrade`.",
			   nbroken);
	else if (heal_rc == 0)
		fpkg_okf("no broken deploys found");
	rc = heal_rc == 0 ? 0 : 1;

out:
	for (size_t i = 0; i < nbroken; i++) free(broken[i]);
	free(broken);
	world_free(&world);
	return rc;
}

int cmd_repair(bool simulate) {
	flatpkg_lock();
	int rc = repair_locked(simulate ? FLATPKG_SOLVERF_SIMULATE : 0);
	flatpkg_unlock();
	return rc;
}
