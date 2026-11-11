#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include "flatpkg.h"
#include "sandbox_internal.h"
#define FPKG_MODULE "extensions"
#include "fpkglog.h"

static void path_parent(const char* path, char* out, size_t outlen) {
	assert(path != NULL);
	assert(out != NULL);
	assert(outlen > 0);
	snprintf(out, outlen, "%s", path);
	char* slash = strrchr(out, '/');
	if (slash == out) slash[1] = '\0';
	else if (slash)
		*slash = '\0';
	else
		snprintf(out, outlen, "/");
}

static void strlist_append(strlist_t* l, const char* item) {
	assert(l != NULL);
	assert(item != NULL);
	l->items = realloc(l->items, (l->count + 1) * sizeof(char*));
	if (!l->items) {
		fpkg_errorf("out of memory");
		exit(1);
	}
	l->items[l->count++] = xstrdup(item);
}

static void strlist_free_items(strlist_t* l) {
	assert(l != NULL);
	for (size_t i = 0; i < l->count; i++) free(l->items[i]);
	free(l->items);
	l->items = NULL;
	l->count = 0;
}

static inline void dstr_append(fpkg_sbuf_t* d, const char* s) {
	assert(d != NULL);
	assert(s != NULL);
	if (fpkg_sbuf_append(d, s, strlen(s)) != FPKG_OK) {
		fpkg_errorf("out of memory");
		exit(1);
	}
}

static bool rel_path_ok(const char* p) {
	if (!p || !*p || p[0] == '/') return false;
	if (strcmp(p, "..") == 0 || strncmp(p, "../", 3) == 0 ||
	    strstr(p, "/../") != NULL)
		return false;
	size_t n = strlen(p);
	return !(n >= 3 && strcmp(p + n - 3, "/..") == 0);
}

static void split_ref(const char* ref, char* arch, size_t archlen, char* branch,
		      size_t branchlen) {
	assert(arch != NULL);
	assert(archlen > 0);
	assert(branch != NULL);
	assert(branchlen > 0);
	char a[64] = "", b[64] = "";
	if (ref && *ref) sscanf(ref, "%*[^/]/%63[^/]/%63s", a, b);
	snprintf(arch, archlen, "%s", a);
	snprintf(branch, branchlen, "%s", b);
}

static int ext_priority(const char* files_path) {
	assert(files_path != NULL);
	char meta[PATH_MAX];
	snprintf(meta, sizeof(meta), "%s/../metadata", files_path);
	fp_metadata_t m;
	if (fp_metadata_parse_any(meta, &m) != 0) return 0;
	int prio = (m.nextof_groups > 0) ? m.extof_groups[0].priority : 0;
	fp_metadata_free(&m);
	return prio;
}

static void ext_append(sandbox_t* sb, const char* id, const char* installed_id,
		       const char* target, const char* directory,
		       const char* subdir_suffix, const char* files_path,
		       bool needs_tmpfs, const char* add_ld_path,
		       const strlist_t* merge_dirs, bool from_app) {
	assert(sb != NULL);
	assert(id != NULL);
	assert(installed_id != NULL);
	assert(target != NULL);
	assert(directory != NULL);
	assert(files_path != NULL);
	for (size_t i = 0; i < sb->nextensions; i++) {
		const fp_extension_t* prev = &sb->extensions[i];
		if (strcmp(prev->target, target) == 0 &&
		    strcmp(prev->directory, directory) == 0)
			return;
	}
	sb->extensions = realloc(
		sb->extensions, (sb->nextensions + 1) * sizeof(fp_extension_t));
	if (!sb->extensions) {
		fpkg_errorf("out of memory");
		exit(1);
	}
	fp_extension_t* e = &sb->extensions[sb->nextensions++];
	memset(e, 0, sizeof(*e));
	e->id = xstrdup(id);
	e->installed_id = xstrdup(installed_id);
	e->target = xstrdup(target);
	e->directory = xstrdup(directory);
	e->subdir_suffix = subdir_suffix ? xstrdup(subdir_suffix) : NULL;
	e->files_path = xstrdup(files_path);
	e->needs_tmpfs = needs_tmpfs;
	e->add_ld_path = add_ld_path ? xstrdup(add_ld_path) : NULL;
	if (merge_dirs)
		for (size_t i = 0; i < merge_dirs->count; i++)
			strlist_append(&e->merge_dirs, merge_dirs->items[i]);
	e->from_app = from_app;
	e->priority = ext_priority(files_path);
}

static void warn_extension_keys(const fp_ext_group_t* g) {
	assert(g != NULL);
	if (g->enable_if && *g->enable_if)
		fpkg_warnf("extension %s sets enable-if=, which flatpkg "
			   "ignores; it will be enabled unconditionally - use "
			   "download-if= instead",
			   g->id);
}

static void resolve_extension(sandbox_t* sb, const fp_ext_group_t* g,
			      const char* target, const char* arch,
			      const char* branch, bool from_app) {
	assert(sb != NULL);
	assert(g != NULL);
	assert(target != NULL);
	assert(arch != NULL);
	assert(branch != NULL);
	if (!rel_path_ok(g->directory)) {
		fpkg_warnf("extension %s has no usable directory=, skipping",
			   g->id);
		return;
	}
	if (g->add_ld_path && !rel_path_ok(g->add_ld_path)) {
		fpkg_warnf("extension %s has an invalid add-ld-path, skipping",
			   g->id);
		return;
	}
	if (g->subdir_suffix && *g->subdir_suffix &&
	    !rel_path_ok(g->subdir_suffix)) {
		fpkg_warnf("extension %s has an invalid subdirectory-suffix, "
			   "skipping",
			   g->id);
		return;
	}
	if (g->merge_dirs.count > 0) {
		for (size_t i = 0; i < g->merge_dirs.count; i++) {
			if (!rel_path_ok(g->merge_dirs.items[i])) {
				fpkg_warnf("extension %s has an invalid "
					   "merge-dirs entry, skipping",
					   g->id);
				return;
			}
		}
	}

	bool exp_subdirs = g->subdirectories || g->versions.count > 1;

	char files[PATH_MAX];
	if (!exp_subdirs && store_find_extension(g->id, arch, branch, files,
						 sizeof(files)) == 0) {
		ext_append(sb, g->id, g->id, target, g->directory,
			   g->subdir_suffix, files, g->needs_tmpfs,
			   g->add_ld_path, &g->merge_dirs, from_app);
		return;
	}

	if (!exp_subdirs && !g->subdirectories) return;

	char** subs = NULL;
	size_t nsubs = 0;
	store_list_extensions(g->id, arch, branch, &subs, &nsubs);
	for (size_t i = 0; i < nsubs; i++) {
		char subdir[PATH_MAX];
		snprintf(subdir, sizeof(subdir), "%s/%s", g->directory,
			 subs[i] + strlen(g->id) + 1);
		if (store_find_extension(subs[i], arch, branch, files,
					 sizeof(files)) == 0)
			ext_append(sb, g->id, subs[i], target, subdir,
				   g->subdir_suffix, files, true,
				   g->add_ld_path, &g->merge_dirs, from_app);
		free(subs[i]);
	}
	free(subs);
}

static int ext_cmp_mount(const void* a, const void* b) {
	assert(a != NULL);
	assert(b != NULL);
	const fp_extension_t *ea = a, *eb = b;
	if (ea->from_app != eb->from_app) return ea->from_app ? 1 : -1;
	return strcmp(ea->directory, eb->directory);
}

static int ext_cmp_prio(const void* a, const void* b) {
	assert(a != NULL);
	assert(b != NULL);
	const fp_extension_t* ea = *(fp_extension_t* const*)a;
	const fp_extension_t* eb = *(fp_extension_t* const*)b;
	if (ea->from_app != eb->from_app) return ea->from_app ? 1 : -1;
	if (ea->priority != eb->priority) return eb->priority - ea->priority;
	return strcmp(ea->installed_id, eb->installed_id);
}

static void ext_ld_path(const fp_extension_t* e, char* out, size_t outlen) {
	assert(e != NULL);
	assert(out != NULL);
	assert(outlen > 0);
	snprintf(out, outlen, "%s/%s", e->target, e->directory);
	if (e->subdir_suffix && *e->subdir_suffix) {
		char tmp[PATH_MAX];
		snprintf(tmp, sizeof(tmp), "%s/%s", out, e->subdir_suffix);
		snprintf(out, outlen, "%s", tmp);
	}
	if (e->add_ld_path && *e->add_ld_path &&
	    strcmp(e->add_ld_path, "true") != 0 &&
	    strcmp(e->add_ld_path, "1") != 0) {
		char tmp[PATH_MAX];
		snprintf(tmp, sizeof(tmp), "%s/%s", out, e->add_ld_path);
		snprintf(out, outlen, "%s", tmp);
	}
}

int fp_extensions_apply(const fp_metadata_t* app_meta, sandbox_t* sb) {
	assert(app_meta != NULL);
	assert(sb != NULL);
	fp_metadata_t rmeta;
	memset(&rmeta, 0, sizeof(rmeta));
	char rmeta_path[PATH_MAX];
	snprintf(rmeta_path, sizeof(rmeta_path), "%s/../metadata",
		 sb->runtime_dir);
	bool have_rmeta = fp_metadata_parse_any(rmeta_path, &rmeta) == 0;

	char app_arch[64] = "", app_ref_branch[64] = "";
	split_ref(app_meta->runtime_ref, app_arch, sizeof(app_arch),
		  app_ref_branch, sizeof(app_ref_branch));
	const char* app_branch = (app_meta->branch && *app_meta->branch)
					 ? app_meta->branch
					 : app_ref_branch;

	char rt_arch[64] = "", rt_branch[64] = "";
	if (have_rmeta)
		split_ref(rmeta.runtime_ref, rt_arch, sizeof(rt_arch),
			  rt_branch, sizeof(rt_branch));
	if (!rt_arch[0]) {
		snprintf(rt_arch, sizeof(rt_arch), "%s", app_arch);
		snprintf(rt_branch, sizeof(rt_branch), "%s", app_ref_branch);
	}

	for (size_t i = 0; i < app_meta->next_groups; i++) {
		const fp_ext_group_t* g = &app_meta->ext_groups[i];
		warn_extension_keys(g);
		bool aexp = g->subdirectories || g->versions.count > 1;
		if (aexp && g->versions.count > 0) {
			for (size_t v = 0; v < g->versions.count; v++)
				resolve_extension(sb, g, "/app", app_arch,
						  g->versions.items[v], true);
		} else {
			resolve_extension(sb, g, "/app", app_arch,
					  g->version && *g->version
						  ? g->version
						  : app_branch,
					  true);
		}
	}
	if (have_rmeta) {
		for (size_t i = 0; i < rmeta.next_groups; i++) {
			const fp_ext_group_t* g = &rmeta.ext_groups[i];
			warn_extension_keys(g);
			bool rexp = g->subdirectories || g->versions.count > 1;
			if (rexp && g->versions.count > 0) {
				for (size_t v = 0; v < g->versions.count; v++)
					resolve_extension(
						sb, g, "/usr", rt_arch,
						g->versions.items[v], false);
			} else {
				resolve_extension(sb, g, "/usr", rt_arch,
						  g->version && *g->version
							  ? g->version
							  : rt_branch,
						  false);
			}
		}
	}

	qsort(sb->extensions, sb->nextensions, sizeof(fp_extension_t),
	      ext_cmp_mount);

	static const char* ldconfig_rel[] = {"/usr/bin/ldconfig",
					     "/usr/sbin/ldconfig", NULL};
	bool use_ld_so_cache = false;
	char ldconfig[PATH_MAX] = {0};
	for (int i = 0; ldconfig_rel[i]; i++) {
		snprintf(ldconfig, sizeof(ldconfig), "%s%s", sb->jailroot,
			 ldconfig_rel[i]);
		if (access(ldconfig, X_OK) == 0 &&
		    ldconfig_trusted(ldconfig)) {
			use_ld_so_cache = true;
			break;
		}
	}
	sb->use_ld_so_cache = use_ld_so_cache;
	sb->ldconfig_path = use_ld_so_cache ? xstrdup(ldconfig) : NULL;

	fp_extension_t** order = calloc(sb->nextensions, sizeof(*order));
	if (!order && sb->nextensions > 0) {
		fpkg_errorf("out of memory");
		exit(1);
	}
	for (size_t i = 0; i < sb->nextensions; i++)
		order[i] = &sb->extensions[i];
	qsort(order, sb->nextensions, sizeof(*order), ext_cmp_prio);

	fpkg_sbuf_t app_ld = {0}, rt_ld = {0};
	int app_count = 0, rt_count = 0;
	for (size_t i = 0; i < sb->nextensions; i++) {
		fp_extension_t* e = order[i];
		if (!e->add_ld_path || !*e->add_ld_path) continue;
		char ldpath[PATH_MAX];
		ext_ld_path(e, ldpath, sizeof(ldpath));
		if (use_ld_so_cache) {
			char dir[PATH_MAX], conf[PATH_MAX];
			snprintf(dir, sizeof(dir),
				 "%s/run/flatpak/ld.so.conf.d", sb->jailroot);
			if (sandbox_ensure_writable_dir(sb, dir) != 0) {
				fpkg_warnf("cannot create %s: %s", dir,
					   strerror(errno));
				continue;
			}
			int n = e->from_app ? ++app_count : ++rt_count;
			snprintf(conf, sizeof(conf), "%s/%s-%03d-%s.conf", dir,
				 e->from_app ? "app" : "runtime", n,
				 e->installed_id);
			char content[PATH_MAX + 4];
			snprintf(content, sizeof(content), "%s\n", ldpath);
			if (write_file(conf, content, 0644) != 0)
				fpkg_warnf("cannot write %s: %s", conf,
					   strerror(errno));
		} else {
			fpkg_sbuf_t* d = e->from_app ? &app_ld : &rt_ld;
			if (d->len > 0) dstr_append(d, ":");
			dstr_append(d, ldpath);
		}
	}

	char** created = NULL;
	size_t ncreated = 0;
	for (size_t i = 0; i < sb->nextensions; i++) {
		fp_extension_t* e = order[i];
		if (e->merge_dirs.count == 0) continue;
		for (size_t m = 0; m < e->merge_dirs.count; m++) {
			const char* md = e->merge_dirs.items[m];
			char src[PATH_MAX];
			snprintf(src, sizeof(src), "%s/%s", e->files_path, md);
			DIR* d = opendir(src);
			if (!d) continue;
			char dir_base[PATH_MAX], parent[PATH_MAX];
			snprintf(dir_base, sizeof(dir_base), "%s/%s", e->target,
				 e->directory);
			path_parent(dir_base, parent, sizeof(parent));
			struct dirent* ent;
			while ((ent = readdir(d))) {
				if (ent->d_name[0] == '.') continue;
				char link_path[PATH_MAX], target[PATH_MAX];
				snprintf(link_path, sizeof(link_path),
					 "%s/%s/%s", parent, md, ent->d_name);
				bool seen = false;
				for (size_t c = 0; c < ncreated; c++) {
					if (strcmp(created[c], link_path) ==
					    0) {
						seen = true;
						break;
					}
				}
				if (seen) continue;
				snprintf(target, sizeof(target), "%s/%s/%s",
					 dir_base, md, ent->d_name);
				char host_link[PATH_MAX], host_parent[PATH_MAX];
				snprintf(host_link, sizeof(host_link), "%s%s",
					 sb->jailroot, link_path);
				path_parent(host_link, host_parent,
					    sizeof(host_parent));
				if (sandbox_ensure_writable_dir(
					    sb, host_parent) != 0) {
					fpkg_warnf("cannot create %s: %s",
						   host_parent,
						   strerror(errno));
					continue;
				}
				(void)unlink(host_link);
				if (symlink(target, host_link) != 0) {
					fpkg_warnf("symlink %s -> %s: %s",
						   host_link, target,
						   strerror(errno));
					continue;
				}
				created = realloc(created, (ncreated +
							    1) * sizeof(char*));
				if (!created) {
					fpkg_errorf("out of memory");
					exit(1);
				}
				created[ncreated++] = xstrdup(link_path);
			}
			closedir(d);
		}
	}

	// The child's LD_LIBRARY_PATH: app extension paths prepend, runtime
	// paths append, the no-cache /app/lib export between them.
	if (use_ld_so_cache) {
		sb->ld_library_path = xstrdup("");
	} else {
		fpkg_sbuf_t ld = {0};
		if (app_ld.len > 0) {
			dstr_append(&ld, app_ld.buf);
			dstr_append(&ld, ":");
		}
		dstr_append(&ld, "/app/lib");
		if (rt_ld.len > 0) {
			dstr_append(&ld, ":");
			dstr_append(&ld, rt_ld.buf);
		}
		sb->ld_library_path = ld.buf ? ld.buf : xstrdup("");
	}

	fpkg_sbuf_t egl = {0}, vk = {0}, layers = {0};
	char seen[32][128];
	size_t nseen = 0;
	for (size_t i = 0; i < sb->nextensions; i++) {
		fp_extension_t* e = &sb->extensions[i];
		if (strncmp(e->id, "org.freedesktop.Platform.GL", 27) != 0)
			continue;
		const struct {
			const char* sub;
			int kind;
		} subdirs[] = {{"share/glvnd/egl_vendor.d", 0},
			       {"lib/vulkan/icd.d", 1},
			       {"vulkan/icd.d", 1},
			       {"share/vulkan/explicit_layer.d", 2},
			       {"share/vulkan/implicit_layer.d", 2}};
		for (size_t s = 0; s < sizeof(subdirs) / sizeof(subdirs[0]);
		     s++) {
			char host_dir[PATH_MAX];
			snprintf(host_dir, sizeof(host_dir), "%s/%s",
				 e->files_path, subdirs[s].sub);
			DIR* dd = opendir(host_dir);
			if (!dd) continue;
			struct dirent* de;
			while ((de = readdir(dd)) != NULL) {
				if (de->d_name[0] == '.') continue;
				size_t nlen = strlen(de->d_name);
				if (nlen < 5 ||
				    strcmp(de->d_name + nlen - 5, ".json") != 0)
					continue;
				int dup = 0;
				for (size_t k = 0; k < nseen; k++) {
					if (strcmp(seen[k], de->d_name) == 0) {
						dup = 1;
						break;
					}
				}
				if (dup) continue;
				if (nseen < sizeof(seen) / sizeof(seen[0]))
					snprintf(seen[nseen++], sizeof(seen[0]),
						 "%s", de->d_name);
				char jail_path[PATH_MAX];
				char base[PATH_MAX];
				ext_ld_path(e, base, sizeof(base));
				snprintf(base, sizeof(base), "%s/%s", e->target,
					 e->directory);
				if (e->subdir_suffix && *e->subdir_suffix) {
					char tmp[PATH_MAX];
					snprintf(tmp, sizeof(tmp), "%s/%s",
						 base, e->subdir_suffix);
					snprintf(base, sizeof(base), "%s", tmp);
				}
				snprintf(jail_path, sizeof(jail_path),
					 "%s/%s/%s", base, subdirs[s].sub,
					 de->d_name);
				fpkg_sbuf_t* dst =
					subdirs[s].kind == 0
						? &egl
						: (subdirs[s].kind == 1
							   ? &vk
							   : &layers);
				if (dst->len > 0) dstr_append(dst, ":");
				dstr_append(dst, jail_path);
			}
			closedir(dd);
		}
	}
	sb->egl_vendor_files = egl.buf ? egl.buf : xstrdup("");
	sb->vulkan_icd_files = vk.buf ? vk.buf : xstrdup("");
	sb->vulkan_layer_paths = layers.buf ? layers.buf : xstrdup("");

	fpkg_sbuf_free(&app_ld);
	fpkg_sbuf_free(&rt_ld);
	for (size_t i = 0; i < ncreated; i++) free(created[i]);
	free(created);
	free(order);
	if (have_rmeta) fp_metadata_free(&rmeta);
	return 0;
}

void fp_extensions_free(sandbox_t* sb) {
	assert(sb != NULL);
	for (size_t i = 0; i < sb->nextensions; i++) {
		fp_extension_t* e = &sb->extensions[i];
		free(e->id);
		free(e->installed_id);
		free(e->target);
		free(e->directory);
		free(e->subdir_suffix);
		free(e->files_path);
		free(e->add_ld_path);
		strlist_free_items(&e->merge_dirs);
	}
	free(sb->extensions);
	sb->extensions = NULL;
	sb->nextensions = 0;
	free(sb->ld_library_path);
	sb->ld_library_path = NULL;
	free(sb->egl_vendor_files);
	sb->egl_vendor_files = NULL;
	free(sb->vulkan_icd_files);
	sb->vulkan_icd_files = NULL;
	free(sb->vulkan_layer_paths);
	sb->vulkan_layer_paths = NULL;
	// Made by fp_extensions_apply() above; nothing else owns it.
	free(sb->ldconfig_path);
	sb->ldconfig_path = NULL;
}
