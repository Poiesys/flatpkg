#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "flatpkg.h"
#define FPKG_MODULE "solver"
#include "fpkglog.h"

#define CLIENT_ID "flatpkg"

static void world_path(char* buf, size_t len) {
	assert(buf != NULL);
	assert(len > 0);
	snprintf(buf, len, "%s/clients/%s/world", store_root(), CLIENT_ID);
}

// The pre-clients/ spelling, still read once so an old store is not lost.
static void legacy_world_path(char* buf, size_t len) {
	snprintf(buf, len, "%s/world", store_root());
}

// Parse [!]name[@remote][op branch] (op = > < >= <= ~); a leading
// app/, runtime/ or extension/ declares the deploy kind.  A legacy plain id
// is FLATPKG_DEP_ANY with no remote and no branch.
int world_dep_parse(const char* line, flatpkg_dep_t* out) {
	assert(line != NULL);
	assert(out != NULL);
	memset(out, 0, sizeof(*out));
	out->kind = FLATPKG_DEP_KIND_APP;
	const char* p = line;
	if (*p == '!') {
		out->conflict = true;
		p++;
	}
	if (strncmp(p, "runtime/", 8) == 0) {
		out->kind = FLATPKG_DEP_KIND_RUNTIME;
		p += 8;
	} else if (strncmp(p, "extension/", 10) == 0) {
		out->kind = FLATPKG_DEP_KIND_EXTENSION;
		p += 10;
	} else if (strncmp(p, "app/", 4) == 0) {
		out->kind = FLATPKG_DEP_KIND_APP;
		p += 4;
	}
	const char* name_start = p;
	while (*p && *p != '@' && *p != '<' && *p != '>' && *p != '=' &&
	       *p != '~')
		p++;
	size_t name_len = p - name_start;
	if (name_len == 0) return -1;
	out->name = malloc(name_len + 1);
	if (!out->name) return -1;
	memcpy(out->name, name_start, name_len);
	out->name[name_len] = '\0';
	if (*p == '@') {
		p++;
		const char* remote_start = p;
		while (*p && *p != '<' && *p != '>' && *p != '=' && *p != '~')
			p++;
		size_t remote_len = p - remote_start;
		if (remote_len > 0) {
			out->remote = malloc(remote_len + 1);
			if (!out->remote) {
				free(out->name);
				out->name = NULL;
				return -1;
			}
			memcpy(out->remote, remote_start, remote_len);
			out->remote[remote_len] = '\0';
		}
	}
	if (*p == '=' || *p == '<' || *p == '>' || *p == '~') {
		if (*p == '=') out->op = FLATPKG_DEP_EQ;
		else if (*p == '<') {
			if (p[1] == '=') {
				out->op = FLATPKG_DEP_LE;
				p++;
			} else
				out->op = FLATPKG_DEP_LT;
		} else if (*p == '>') {
			if (p[1] == '=') {
				out->op = FLATPKG_DEP_GE;
				p++;
			} else
				out->op = FLATPKG_DEP_GT;
		} else
			out->op = FLATPKG_DEP_PREFIX;
		p++;
		if (*p) out->branch = xstrdup(p);
	}
	return 0;
}

char* world_dep_format(const flatpkg_dep_t* dep) {
	if (!dep || !dep->name) return xstrdup("");
	char buf[1024];
	size_t off = 0;
	if (dep->conflict) buf[off++] = '!';
	const char* kind_str = "";
	switch (dep->kind) {
	case FLATPKG_DEP_KIND_RUNTIME:
		kind_str = "runtime/";
		break;
	case FLATPKG_DEP_KIND_EXTENSION:
		kind_str = "extension/";
		break;
	case FLATPKG_DEP_KIND_APP:
	default:
		kind_str = "";
		break;
	}
	off += snprintf(buf + off, sizeof(buf) - off, "%s%s", kind_str,
			dep->name);
	if (dep->remote)
		off += snprintf(buf + off, sizeof(buf) - off, "@%s",
				dep->remote);
	if (dep->branch) {
		const char* op = "";
		switch (dep->op) {
		case FLATPKG_DEP_EQ:
			op = "=";
			break;
		case FLATPKG_DEP_GT:
			op = ">";
			break;
		case FLATPKG_DEP_LT:
			op = "<";
			break;
		case FLATPKG_DEP_GE:
			op = ">=";
			break;
		case FLATPKG_DEP_LE:
			op = "<=";
			break;
		case FLATPKG_DEP_PREFIX:
			op = "~";
			break;
		case FLATPKG_DEP_ANY:
			op = "";
			break;
		}
		off += snprintf(buf + off, sizeof(buf) - off, "%s%s", op,
				dep->branch);
	}
	return xstrdup(buf);
}

char* world_to_text(const flatpkg_dep_array_t* world) {
	char* buf = NULL;
	size_t len = 0;
	FILE* f = open_memstream(&buf, &len);

	if (f == NULL) return NULL;
	for (size_t i = 0; i < world->count; i++) {
		char* line = world_dep_format(&world->items[i]);
		fprintf(f, "%s\n", line);
		free(line);
	}
	if (fclose(f) != 0) {
		free(buf);
		return NULL;
	}
	return buf;
}

// Load the worldfile; a line the parser drops warns, because the solver
// must not silently keep or remove something the caller did not write.
int world_load(flatpkg_dep_array_t* out) {
	assert(out != NULL);
	memset(out, 0, sizeof(*out));
	char path[PATH_MAX];
	world_path(path, sizeof(path));
	FILE* f = fopen(path, "r");
	if (f == NULL && errno == ENOENT) {
		legacy_world_path(path, sizeof(path));
		f = fopen(path, "r");
	}
	if (!f) {
		// Missing is empty: seed once, then read what appeared.  A file
		// that still will not open is an error - solving against an
		// empty world would plan to remove everything it references.
		if (errno != ENOENT) {
			fpkg_errorf("cannot open %s: %s", path,
				    strerror(errno));
			return -1;
		}
		if (world_seed() != 0) return -1;
		f = fopen(path, "r");
		if (!f) {
			if (errno != ENOENT) {
				fpkg_errorf("cannot open %s: %s", path,
					    strerror(errno));
				return -1;
			}
			return 0;
		}
	}
	char line[1024];
	unsigned long lineno = 0;
	while (fgets(line, sizeof(line), f)) {
		lineno++;
		size_t n = strlen(line);
		while (n > 0 && isspace((unsigned char)line[n - 1]))
			line[--n] = '\0';
		char* s = line;
		while (*s && isspace((unsigned char)*s)) s++;
		if (!*s || *s == '#') continue;
		flatpkg_dep_t dep;
		if (world_dep_parse(s, &dep) != 0) {
			// A line the parser drops changes what the solver
			// keeps, removes or installs - that must be loud.
			fpkg_warnf("%s:%lu: cannot parse '%s'; line ignored",
				   path, lineno, s);
			continue;
		}
		if (out->count == out->cap) {
			out->cap = out->cap ? out->cap * 2 : 16;
			out->items = realloc(out->items,
					     out->cap * sizeof(flatpkg_dep_t));
			if (!out->items) {
				fpkg_errorf("out of memory");
				exit(1);
			}
		}
		out->items[out->count++] = dep;
	}
	int bad = ferror(f);
	fclose(f);
	if (bad) {
		fpkg_errorf("cannot read %s: %s", path, strerror(errno));
		return -1;
	}
	return 0;
}

int world_save(const flatpkg_dep_array_t* world) {
	assert(world != NULL);
	char path[PATH_MAX];
	world_path(path, sizeof(path));
	char clients[PATH_MAX], dir[PATH_MAX];
	snprintf(clients, sizeof(clients), "%s/clients", store_root());
	snprintf(dir, sizeof(dir), "%s/clients/%s", store_root(), CLIENT_ID);
	(void)mkdir(clients, 0755);
	(void)mkdir(dir, 0755);
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE* f = fopen(tmp, "w");
	if (!f) {
		fpkg_errorf("cannot write %s: %s", tmp, strerror(errno));
		return -1;
	}
	fprintf(f,
		"# flatpkg worldfile - do not edit while flatpkg is running\n");
	fprintf(f, "# Format: [!]name[@remote][[<>=~]branch]\n");
	for (size_t i = 0; i < world->count; i++) {
		char* line = world_dep_format(&world->items[i]);
		fprintf(f, "%s\n", line);
		free(line);
	}
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	if (rename(tmp, path) != 0) {
		fpkg_errorf("rename %s -> %s: %s", tmp, path, strerror(errno));
		unlink(tmp);
		return -1;
	}
	return 0;
}

void world_free(flatpkg_dep_array_t* world) {
	assert(world != NULL);
	for (size_t i = 0; i < world->count; i++) {
		free(world->items[i].name);
		free(world->items[i].remote);
		free(world->items[i].branch);
	}
	free(world->items);
	memset(world, 0, sizeof(*world));
}

int world_dep_add(flatpkg_dep_array_t* world, const flatpkg_dep_t* dep) {
	assert(world != NULL);
	assert(dep != NULL);
	assert(dep->name != NULL);
	for (size_t i = 0; i < world->count; i++) {
		if (strcmp(world->items[i].name, dep->name) == 0) {
			free(world->items[i].remote);
			free(world->items[i].branch);
			world->items[i].remote =
				dep->remote ? xstrdup(dep->remote) : NULL;
			world->items[i].branch =
				dep->branch ? xstrdup(dep->branch) : NULL;
			world->items[i].op = dep->op;
			world->items[i].conflict = dep->conflict;
			world->items[i].kind = dep->kind;
			return 0;
		}
	}
	if (world->count == world->cap) {
		world->cap = world->cap ? world->cap * 2 : 16;
		world->items = realloc(world->items,
				       world->cap * sizeof(flatpkg_dep_t));
		if (!world->items) {
			fpkg_errorf("out of memory");
			exit(1);
		}
	}
	flatpkg_dep_t* e = &world->items[world->count++];
	e->name = xstrdup(dep->name);
	e->remote = dep->remote ? xstrdup(dep->remote) : NULL;
	e->branch = dep->branch ? xstrdup(dep->branch) : NULL;
	e->op = dep->op;
	e->conflict = dep->conflict;
	e->kind = dep->kind;
	return 0;
}

int world_dep_del(flatpkg_dep_array_t* world, const char* name) {
	assert(world != NULL);
	assert(name != NULL);
	for (size_t i = 0; i < world->count; i++) {
		if (strcmp(world->items[i].name, name) == 0) {
			free(world->items[i].name);
			free(world->items[i].remote);
			free(world->items[i].branch);
			memmove(&world->items[i], &world->items[i + 1],
				(world->count - i - 1) * sizeof(flatpkg_dep_t));
			world->count--;
			return 0;
		}
	}
	return -1;
}

bool world_dep_has(const flatpkg_dep_array_t* world, const char* name) {
	assert(world != NULL);
	assert(name != NULL);
	for (size_t i = 0; i < world->count; i++)
		if (strcmp(world->items[i].name, name) == 0) return true;
	return false;
}

static bool id_is_import(const char* id) {
	assert(id != NULL);
	char pointer[PATH_MAX];
	store_pointer_path(pointer, sizeof(pointer), id);
	struct stat st;
	return stat(pointer, &st) == 0 && S_ISREG(st.st_mode);
}

static int installed_list(char*** out, size_t* n_out) {
	assert(out != NULL);
	assert(n_out != NULL);
	*out = NULL;
	*n_out = 0;
	static const char* types[] = {"app", "runtime", "extension", NULL};
	char** ids = NULL;
	size_t n = 0, cap = 0;
	for (int t = 0; types[t]; t++) {
		char root[PATH_MAX];
		snprintf(root, sizeof(root), "%s/%s", store_root(), types[t]);
		DIR* d1 = opendir(root);
		if (!d1) continue;
		struct dirent* id_ent;
		while ((id_ent = readdir(d1))) {
			if (id_ent->d_name[0] == '.') continue;
			char iddir[PATH_MAX];
			snprintf(iddir, sizeof(iddir), "%s/%s", root,
				 id_ent->d_name);
			DIR* d2 = opendir(iddir);
			if (!d2) continue;
			struct dirent* arch_ent;
			while ((arch_ent = readdir(d2))) {
				if (arch_ent->d_name[0] == '.') continue;
				char archdir[PATH_MAX];
				snprintf(archdir, sizeof(archdir), "%s/%s",
					 iddir, arch_ent->d_name);
				DIR* d3 = opendir(archdir);
				if (!d3) continue;
				struct dirent* branch_ent;
				while ((branch_ent = readdir(d3))) {
					if (branch_ent->d_name[0] == '.')
						continue;
					char active[PATH_MAX];
					snprintf(active, sizeof(active),
						 "%s/%s/active", archdir,
						 branch_ent->d_name);
					struct stat st;
					if (stat(active, &st) != 0) continue;
					char ref[512];
					snprintf(ref, sizeof(ref),
						 "%s/%s/%s/%s", types[t],
						 id_ent->d_name,
						 arch_ent->d_name,
						 branch_ent->d_name);
					if (n == cap) {
						cap = cap ? cap * 2 : 64;
						ids = realloc(
							ids,
							cap * sizeof(char*));
						if (!ids) {
							closedir(d3);
							closedir(d2);
							closedir(d1);
							return -1;
						}
					}
					ids[n++] = xstrdup(ref);
				}
				closedir(d3);
			}
			closedir(d2);
		}
		closedir(d1);
	}

	{
		char app_root[PATH_MAX];
		snprintf(app_root, sizeof(app_root), "%s/app", store_root());
		DIR* ad = opendir(app_root);
		if (ad) {
			struct dirent* ae;
			while ((ae = readdir(ad))) {
				if (ae->d_name[0] == '.') continue;
				char pointer[PATH_MAX];
				store_pointer_path(pointer, sizeof(pointer),
						   ae->d_name);
				struct stat pst;
				if (stat(pointer, &pst) != 0 ||
				    !S_ISREG(pst.st_mode))
					continue;
				bool dup = false;
				for (size_t i = 0; i < n; i++)
					if (strncmp(ids[i], "app/", 4) == 0 &&
					    strstr(ids[i], ae->d_name) != NULL)
						dup = true;
				if (dup) continue;
				char edir[PATH_MAX], arch[64], branch[64];
				if (store_pointer_export(ae->d_name, edir,
							 sizeof(edir), arch,
							 sizeof(arch), branch,
							 sizeof(branch)) != 0)
					continue;
				if (n == cap) {
					cap = cap ? cap * 2 : 64;
					ids = realloc(ids, cap * sizeof(char*));
					if (!ids) {
						closedir(ad);
						return -1;
					}
				}
				char ref[512];
				snprintf(ref, sizeof(ref), "app/%s/%s/%s",
					 ae->d_name, arch, branch);
				ids[n++] = xstrdup(ref);
			}
			closedir(ad);
		}
	}
	*out = ids;
	*n_out = n;
	return 0;
}

static bool ref_is_referenced(const char* ref,
			      const flatpkg_dep_array_t* world) {
	char type[64], id[256], arch[64], branch[64];
	if (sscanf(ref, "%63[^/]/%255[^/]/%63[^/]/%63s", type, id, arch,
		   branch) != 4)
		return false;
	if (strcmp(type, "app") == 0) {
		for (size_t i = 0; i < world->count; i++)
			if (strcmp(world->items[i].name, id) == 0) return true;
		return false;
	}
	// A runtime can declare and be declared by other runtimes, so scanning only the
	// world apps' metadata would read the extensions they pulled in as orphans.
	if (idset_has(&g_closure, id)) return true;
	return ostree_deploy_referenced(ref);
}
static void changeset_add(flatpkg_changeset_t* cs, flatpkg_change_type_t type,
			  const char* ref, const char* old_version,
			  const char* new_version) {
	assert(cs != NULL);
	assert(ref != NULL);
	if (cs->count == cs->cap) {
		cs->cap = cs->cap ? cs->cap * 2 : 16;
		cs->items =
			realloc(cs->items, cs->cap * sizeof(flatpkg_change_t));
		if (!cs->items) {
			fpkg_errorf("out of memory");
			exit(1);
		}
	}
	flatpkg_change_t* c = &cs->items[cs->count++];
	memset(c, 0, sizeof(*c));
	c->type = type;
	c->ref = xstrdup(ref);
	c->old_version = old_version ? xstrdup(old_version) : NULL;
	c->new_version = new_version ? xstrdup(new_version) : NULL;
}

void changeset_add_noop(flatpkg_changeset_t* cs, const char* ref) {
	assert(cs != NULL);
	assert(ref != NULL);
	changeset_add(cs, FP_CHANGE_NOOP, ref, NULL, NULL);
}

void changeset_add_reinstall(flatpkg_changeset_t* cs, const char* ref) {
	assert(cs != NULL);
	assert(ref != NULL);
	changeset_add(cs, FP_CHANGE_REINSTALL, ref, NULL, NULL);
}

static void changeset_add_once(flatpkg_changeset_t* cs,
			       flatpkg_change_type_t type, const char* ref,
			       const char* old_version,
			       const char* new_version) {
	assert(cs != NULL);
	assert(ref != NULL);
	for (size_t i = 0; i < cs->count; i++)
		if (cs->items[i].type == type &&
		    strcmp(cs->items[i].ref, ref) == 0)
			return;
	changeset_add(cs, type, ref, old_version, new_version);
}

static bool installed_has(char** installed, size_t n, const char* type,
			  const char* id, const char* branch) {
	assert(installed != NULL || n == 0);
	for (size_t j = 0; j < n; j++) {
		char t[64], i[256], a[64], b[64];
		if (sscanf(installed[j], "%63[^/]/%255[^/]/%63[^/]/%63s", t, i,
			   a, b) != 4)
			continue;
		if (strcmp(t, type) == 0 && strcmp(i, id) == 0 &&
		    (branch == NULL || strcmp(b, branch) == 0))
			return true;
	}
	return false;
}

int solver_solve(const flatpkg_dep_array_t* world, unsigned flags,
		 flatpkg_changeset_t* cs_out, const solver_incoming_t* incoming,
		 size_t nincoming) {
	assert(world != NULL);
	assert(cs_out != NULL);
	memset(cs_out, 0, sizeof(*cs_out));
	char** installed = NULL;
	size_t ninst = 0;
	// The world may have just changed (an install, a commit), so the
	// closure is rebuilt rather than reused.
	ostree_closure_reset();
	if (installed_list(&installed, &ninst) != 0) return -1;

	// Incoming apps are intended, so their runtime and extensions must not
	// read as orphans in Phase 2.
	for (size_t i = 0; i < nincoming; i++) {
		const solver_incoming_t* inc = &incoming[i];
		idset_add(&g_closure, inc->ref.id);
		closure_follow(&g_closure, inc->ref.id);
		if (inc->meta.runtime_ref && *inc->meta.runtime_ref) {
			fpkg_ref_t rt;
			if (ref_parse(inc->meta.runtime_ref, &rt) == 0) {
				idset_add(&g_closure, rt.id);
				closure_follow(&g_closure, rt.id);
				ref_free(&rt);
			}
		}
		for (size_t g = 0; g < inc->meta.next_groups; g++) {
			const char* gid = inc->meta.ext_groups[g].id;
			if (gid && *gid) idset_add(&g_closure, gid);
		}
	}

	for (size_t i = 0; i < world->count; i++) {
		const flatpkg_dep_t* wdep = &world->items[i];
		if (wdep->conflict) continue;
		idset_add(&g_closure, wdep->name);
		closure_follow(&g_closure, wdep->name);
	}

	// Ready, so ostree_deploy_referenced() does not rebuild the closure and
	// drop the IDs added above; Phase 2 would then remove what we just
	// scheduled for install.
	ostree_closure_mark_ready();

	for (size_t i = 0; i < world->count; i++) {
		const flatpkg_dep_t* dep = &world->items[i];
		// Extract the bare id from dep->name (legacy world entries may
		// still carry arch/branch after the kind prefix was stripped).
		char dep_id[256];
		{
			const char* slash = strchr(dep->name, '/');
			if (slash != NULL)
				snprintf(dep_id, sizeof(dep_id), "%.*s",
					 (int)(slash - dep->name), dep->name);
			else
				snprintf(dep_id, sizeof(dep_id), "%s",
					 dep->name);
		}
		if (dep->conflict) {
			for (size_t j = 0; j < ninst; j++) {
				char type[64], id[256], arch[64], branch[64];
				if (sscanf(installed[j],
					   "%63[^/]/%255[^/]/%63[^/]/%63s",
					   type, id, arch, branch) == 4 &&
				    strcmp(id, dep_id) == 0) {
					changeset_add(cs_out, FP_CHANGE_REMOVE,
						      installed[j], branch,
						      NULL);
				}
			}
			continue;
		}
		bool found = false;
		for (size_t j = 0; j < ninst; j++) {
			char type[64], id[256], arch[64], branch[64];
			if (sscanf(installed[j],
				   "%63[^/]/%255[^/]/%63[^/]/%63s", type, id,
				   arch, branch) != 4)
				continue;
			if (strcmp(id, dep_id) != 0) continue;
			found = true;
			if (id_is_import(id)) {
				changeset_add(cs_out, FP_CHANGE_KEEP,
					      installed[j], branch, branch);
				fpkg_infof("%s was imported from a local "
					   "deploy root; not upgraded from a "
					   "remote",
					   id);
			} else if (flags & FLATPKG_SOLVERF_UPGRADE) {
				changeset_add(cs_out, FP_CHANGE_UPGRADE,
					      installed[j], branch, "latest");
			} else if (dep->branch &&
				   strcmp(dep->branch, branch) != 0) {
				if (flags & FLATPKG_SOLVERF_AVAILABLE) {
					changeset_add(cs_out, FP_CHANGE_UPGRADE,
						      installed[j], branch,
						      "available");
				} else {
					changeset_add(cs_out,
						      FP_CHANGE_DOWNGRADE,
						      installed[j], branch,
						      dep->branch);
				}
			} else {
				changeset_add(cs_out, FP_CHANGE_KEEP,
					      installed[j], branch, branch);
			}
			break;
		}
		if (!found && !dep->conflict) {
			char ref[512];
			const char* kind_str = "app";
			switch (dep->kind) {
			case FLATPKG_DEP_KIND_RUNTIME:
				kind_str = "runtime";
				break;
			case FLATPKG_DEP_KIND_EXTENSION:
				kind_str = "extension";
				break;
			case FLATPKG_DEP_KIND_APP:
			default:
				kind_str = "app";
				break;
			}
			const char* ibranch =
				dep->branch ? dep->branch : "stable";
			snprintf(ref, sizeof(ref), "%s/%s", kind_str, dep_id);
			for (size_t k = 0; k < nincoming; k++) {
				const fpkg_ref_t* ir = &incoming[k].ref;
				if (strcmp(ir->id, dep_id) != 0 || !ir->arch)
					continue;
				if (!dep->branch && ir->branch)
					ibranch = ir->branch;
				snprintf(ref, sizeof(ref), "%s/%s/%s/%s",
					 kind_str, dep_id, ir->arch, ibranch);
				break;
			}
			changeset_add(cs_out, FP_CHANGE_INSTALL, ref, NULL,
				      ibranch);
		}
	}

	// The closure holds only what is installed, so a missing runtime or extension is
	// planned here under the launch-time resolver's rules (.Debug/no-autodownload skipped).
	for (size_t i = 0; i < nincoming; i++) {
		const solver_incoming_t* inc = &incoming[i];
		const char* iarch = inc->ref.arch ? inc->ref.arch : "";
		const char* ibranch =
			inc->ref.branch ? inc->ref.branch : "stable";

		fpkg_ref_t rt;
		if (inc->meta.runtime_ref && *inc->meta.runtime_ref &&
		    ref_parse(inc->meta.runtime_ref, &rt) == 0) {
			const char* rbranch = rt.branch ? rt.branch : "stable";
			if (!installed_has(installed, ninst, "runtime", rt.id,
					   rt.branch)) {
				char rref[512];
				snprintf(rref, sizeof(rref), "runtime/%s/%s/%s",
					 rt.id, rt.arch, rbranch);
				changeset_add_once(cs_out, FP_CHANGE_INSTALL,
						   rref, NULL, rbranch);
			}
			ref_free(&rt);
		}

		for (size_t g = 0; g < inc->meta.next_groups; g++) {
			const fp_ext_group_t* grp = &inc->meta.ext_groups[g];
			if (!grp->id || !*grp->id) continue;
			const char* branches[16];
			size_t nb = ostree_group_install_branches(
				grp, iarch, ibranch, branches, 16);
			for (size_t b = 0; b < nb; b++) {
				char eref[512];
				snprintf(eref, sizeof(eref),
					 "extension/%s/%s/%s", grp->id, iarch,
					 branches[b]);
				changeset_add_once(cs_out, FP_CHANGE_INSTALL,
						   eref, NULL, branches[b]);
			}
		}
	}

	// Heal: the world implies a complete set, so a missing dependency of an installed
	// app or runtime is planned too. A remote is only asked when a gap exists.
	for (size_t j = 0; j < ninst; j++) {
		char type[64];
		if (sscanf(installed[j], "%63[^/]", type) != 1) continue;
		if (strcmp(type, "app") != 0 && strcmp(type, "runtime") != 0)
			continue;
		if (!ref_is_referenced(installed[j], world)) continue;
		size_t nm = 0;
		char** miss = ostree_missing_deps(
			installed[j], (flags & FLATPKG_SOLVERF_REPAIR) != 0,
			&nm);
		for (size_t m = 0; m < nm; m++) {
			// No version: a runtime row's version is a commit pin
			// when it reaches the executor, and the branch is in
			// the ref anyway.
			changeset_add_once(cs_out, FP_CHANGE_INSTALL, miss[m],
					   NULL, NULL);
			free(miss[m]);
		}
		free(miss);
	}

	// Phase 2: an installed ref not in the world and not referenced by one is removed
	// (apk-style). An upgrade only sweeps with --prune; install and uninstall always do.
	bool sweep =
		!(flags & (FLATPKG_SOLVERF_UPGRADE | FLATPKG_SOLVERF_REPAIR)) ||
		(flags & FLATPKG_SOLVERF_PRUNE);
	for (size_t j = 0; sweep && j < ninst; j++) {
		char type[64], id[256], arch[64], branch[64];
		if (sscanf(installed[j], "%63[^/]/%255[^/]/%63[^/]/%63s", type,
			   id, arch, branch) != 4)
			continue;
		if (world_dep_has(world, id)) continue;
		if (ref_is_referenced(installed[j], world)) continue;
		if (id_is_import(id))
			fpkg_infof(
				"%s was imported from a local deploy "
				"root: removing it drops the only reference, "
				"and `flatpkg install` cannot restore it",
				id);
		changeset_add(cs_out, FP_CHANGE_REMOVE, installed[j], branch,
			      NULL);
	}

	for (size_t j = 0; j < ninst; j++) free(installed[j]);
	free(installed);
	return 0;
}

// KEEP means "leave it alone": the only rows that are not work.
bool changeset_has_work(const flatpkg_changeset_t* cs) {
	assert(cs != NULL);
	for (size_t i = 0; i < cs->count; i++)
		if (cs->items[i].type != FP_CHANGE_KEEP) return true;
	return false;
}

int64_t fp_now_ns(void);

static int g_txn_depth;

bool flatpkg_txn_active(void) { return g_txn_depth > 0; }

static void txn_label(flatpkg_txn_t* t) {
	char label[48];

	snprintf(label, sizeof(label), "overall %zu/%zu", t->done, t->total);
	fp_bar_set_label(&t->bar, label);
	fp_bar_update(&t->bar, t->done);
}

void flatpkg_txn_begin(flatpkg_txn_t* t, size_t total) {
	assert(t != NULL);
	memset(t, 0, sizeof(*t));
	t->total = total;
	t->nested = g_txn_depth > 0;
	g_txn_depth++;
	if (t->nested || total == 0) return;
	t->start_ns = fp_now_ns();
	t->bar = fp_bar_init("transaction", "", "overall", total);
	if (!fp_bar_can_pin()) return;
	fp_bar_update(&t->bar, 0);
	t->pinned = fp_bar_pinned(&t->bar);
	if (t->pinned) txn_label(t);
}

static void txn_took(const flatpkg_txn_t* t, char* out, size_t len) {
	// A no-op change list begins no timer: flatpkg_txn_begin() returns before
	// it starts one, and the zero would read as "took 55 years".
	double sec = 0.0;

	if (t->start_ns != 0)
		sec = (double)(fp_now_ns() - t->start_ns) / 1e9;
	if (sec < 60.0) snprintf(out, len, "%.1fs", sec);
	else
		snprintf(out, len, "%dm %ds", (int)(sec / 60.0), (int)sec % 60);
}

void flatpkg_txn_op(flatpkg_txn_t* t, const char* verb, const char* ref) {
	assert(t != NULL);
	assert(verb != NULL);
	assert(ref != NULL);
	if (t->started) t->done++;
	t->started = true;
	if (t->nested) return;
	// One bar spans the transaction: pulls then installs. It stays at finished work
	// while the label names the current op, so the label must count done+1.
	if (t->pinned) {
		char label[48];
		size_t cur = t->done + 1;
		if (cur > t->total) cur = t->total;
		snprintf(label, sizeof(label), "Installed: %zu/%zu", cur,
			 t->total);
		fp_bar_set_label(&t->bar, label);
		fp_bar_update(&t->bar, t->done);
	} else
		fpkg_infof("(%zu/%zu) %s %s", t->done + 1, t->total, verb, ref);
}

void flatpkg_txn_pull_begin(flatpkg_txn_t* t, size_t n) {
	assert(t != NULL);
	if (t->nested || !t->pinned) return;
	t->pulling = true;
	t->pull_total = n;
	char label[48];
	snprintf(label, sizeof(label), "pulled: 0/%zu", n);
	fp_bar_set_label(&t->bar, label);
	fp_bar_update(&t->bar, 0);
}

void flatpkg_txn_pull_progress(flatpkg_txn_t* t, size_t n) {
	assert(t != NULL);
	if (t->nested || !t->pinned || !t->pulling) return;
	char label[48];
	snprintf(label, sizeof(label), "pulled: %zu/%zu", n, t->pull_total);
	fp_bar_set_label(&t->bar, label);
	fp_bar_update(&t->bar, n);
}

void flatpkg_txn_pull_end(flatpkg_txn_t* t) {
	assert(t != NULL);
	if (t->nested || !t->pulling) return;
	t->pulling = false;
	if (!t->pinned) return;
	char label[48];
	snprintf(label, sizeof(label), "Installed: 0/%zu", t->total);
	fp_bar_set_label(&t->bar, label);
	fp_bar_update(&t->bar, 0);
}

void flatpkg_txn_end(flatpkg_txn_t* t, int rc) {
	assert(t != NULL);
	if (t->started) t->done++;
	t->started = false;
	if (g_txn_depth > 0) g_txn_depth--;
	if (t->nested) return;
	char took[24];
	txn_took(t, took, sizeof(took));
	if (rc == 0) {
		if (t->pinned)
			fp_bar_finishf(&t->bar, 1, "%zu change%s applied",
				       t->total, t->total == 1 ? "" : "s");
		else
			fp_logf("transaction", "", FP_LEVEL_OKAY,
				"%zu change%s applied (took %s)", t->total,
				t->total == 1 ? "" : "s", took);
		return;
	}
	if (t->pinned)
		fp_bar_finishf(&t->bar, 0,
			       "transaction failed after %zu of %zu changes",
			       t->done, t->total);
	else
		fp_logf("transaction", "", FP_LEVEL_ERROR,
			"transaction failed after %zu of %zu changes", t->done,
			t->total);
	// The world already holds the request: this only has to be finished.
	fpkg_infof("the world file keeps your request; `flatpkg repair` "
		   "finishes it");
}

static int g_lock_fd = -1;
static int g_lock_depth;

void flatpkg_lock(void) {
	if (g_lock_depth++ > 0) return;
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/db.lock", store_root());
	// CLOEXEC: the tools we start must not keep the lock alive after us.
	int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (fd < 0) return;
	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		fpkg_infof("waiting for another flatpkg operation to finish");
		(void)flock(fd, LOCK_EX);
	}
	g_lock_fd = fd;
}

void flatpkg_unlock(void) {
	if (g_lock_depth == 0) return;
	if (--g_lock_depth > 0) return;
	if (g_lock_fd >= 0) {
		(void)flock(g_lock_fd, LOCK_UN);
		close(g_lock_fd);
		g_lock_fd = -1;
	}
}

void changeset_free(flatpkg_changeset_t* cs) {
	assert(cs != NULL);
	for (size_t i = 0; i < cs->count; i++) {
		free(cs->items[i].ref);
		free(cs->items[i].old_version);
		free(cs->items[i].new_version);
	}
	free(cs->items);
	memset(cs, 0, sizeof(*cs));
}
