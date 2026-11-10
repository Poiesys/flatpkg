#include "ostree_internal.h"

// Forward declarations for helpers used before their definition.
static void related_refs_for_metadata(const fp_metadata_t* meta,
				      const char* parent_branch,
				      const char* arch, char** srefs,
				      size_t nsrefs, fpkg_ref_t** out,
				      size_t* nout);
static const char* plan_meta_text(const char* ref);
static void uninstall_collect_reverse_deps(const char* ref_id,
					   flatpkg_dep_array_t* world,
					   flatpkg_dep_array_t* extras);

static int install_one_checkout(const fpkg_ref_t* ref, const char* pull_type,
				const char* deploy_type, const char* remote,
				bool force) {
	char refstr[512];
	ref_full(ref, pull_type, refstr, sizeof(refstr));
	tool_ctx_t tc;
	tool_ctx_init(&tc);
	char commit[128];
	if (tool_rev_parse(&tc, refstr, commit, sizeof(commit)) != 0) return -1;
	if (!force && ref->branch && *ref->branch) {
		char link[PATH_MAX], tgt[PATH_MAX];
		snprintf(link, sizeof(link), "%s/%s/%s/%s/%s/active",
			 store_root(), deploy_type, ref->id, ref->arch,
			 ref->branch);
		ssize_t n = readlink(link, tgt, sizeof(tgt) - 1);
		if (n > 0) {
			tgt[n] = '\0';
			if (strcmp(tgt, commit) == 0) return 0;
		}
	}
	char deploy[PATH_MAX];
	if (deploy_checkout(&tc, deploy_type, ref, commit, deploy,
			    sizeof(deploy)) != 0)
		return -1;
	if (make_active(deploy) != 0) return -1;
	write_origin(deploy, remote);
	{
		char bdir[PATH_MAX];
		if (!deploy_branch_dir(deploy, bdir, sizeof(bdir)))
			fpkg_warnf("deploy data: no '/' in %s, "
				   "skipping deploy data",
				   deploy);
		else
			write_deploy_data(bdir, commit, remote, NULL, 0);
	}
	return 0;
}
int install_one(const fpkg_ref_t* ref, const char* pull_type,
		       const char* deploy_type, const char* remote) {
	char refstr[512];
	ref_full(ref, pull_type, refstr, sizeof(refstr));

	if (tool_pull(remote, refstr) != 0) return -1;
	return install_one_checkout(ref, pull_type, deploy_type, remote, false);
}
static bool str_has_suffix(const char* s, const char* suffix) {
	size_t sl = strlen(s), fl = strlen(suffix);
	return sl >= fl && strcmp(s + sl - fl, suffix) == 0;
}
// Writes the local filesystem path of a file:// remote into out.
bool remote_local_path(const char* remote, char* out, size_t len) {
	assert(remote != NULL);
	assert(out != NULL);
	assert(len > 0);
	char* u = remote_setting(remote, "url");
	if (!u) return false;
	if (strncmp(u, "file://", 7) != 0) {
		free(u);
		return false;
	}
	snprintf(out, len, "%s", u + 7);
	free(u);
	return true;
}
static int levenshtein(const char* a, const char* b) {
	assert(a != NULL);
	assert(b != NULL);
	size_t la = strlen(a), lb = strlen(b);
	if (la == 0) return (int)lb;
	if (lb == 0) return (int)la;
	int* row = malloc((lb + 1) * sizeof(int));
	if (!row) {
		fpkg_errorf("out of memory");
		exit(1);
	}
	for (size_t j = 0; j <= lb; j++) row[j] = (int)j;
	for (size_t i = 1; i <= la; i++) {
		int prev = row[0];
		row[0] = (int)i;
		for (size_t j = 1; j <= lb; j++) {
			int old = row[j];
			int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
			int m = row[j - 1] + 1;
			int d = prev + cost;
			int u = old + 1;
			if (d < m) m = d;
			if (u < m) m = u;
			row[j] = m;
			prev = old;
		}
	}
	int r = row[lb];
	free(row);
	return r;
}
// A remote's ref list takes seconds to fetch, and one install wants it for
// the match, the solver's missing-dependency check and the runtime's
// extensions: keep it for a short while.
#define REMOTE_REFS_TTL_S 120
#define REMOTE_REFS_RETRY_S 10
static remote_refs_t* g_rrefs;
static size_t g_nrrefs;
static void remote_refs_drop(remote_refs_t* e) {
	for (size_t k = 0; k < e->n; k++) free(e->refs[k]);
	free(e->refs);
	e->refs = NULL;
	e->n = 0;
}
remote_refs_t* remote_refs_cached(const char* remote,
					 const char* label) {
	time_t now = time(NULL);
	remote_refs_t* e = NULL;
	for (size_t i = 0; i < g_nrrefs; i++)
		if (strcmp(g_rrefs[i].remote, remote) == 0) {
			e = &g_rrefs[i];
			break;
		}
	if (e &&
	    now - e->at < (e->failed ? REMOTE_REFS_RETRY_S : REMOTE_REFS_TTL_S))
		return e;
	if (!e) {
		remote_refs_t* grown =
			realloc(g_rrefs, (g_nrrefs + 1) * sizeof(*grown));
		if (!grown) return NULL;
		g_rrefs = grown;
		e = &g_rrefs[g_nrrefs++];
		memset(e, 0, sizeof(*e));
		e->remote = xstrdup(remote);
	} else {
		remote_refs_drop(e);
	}

	fp_spin_t sp;
	bool spinning = fpkg_interactive();
	if (spinning) {
		sp = fp_spin_init(FPKG_MODULE, "", label);
		fp_spin_tick(&sp);
	}
	g_fetch_quiet = true;
	e->refs = fetch_remote_refs(remote, &e->n, spinning ? &sp : NULL);
	g_fetch_quiet = false;
	e->failed = e->refs == NULL;
	e->warned = false;
	e->at = now;
	return e;
}
static void install_match_free(install_match_t* m) {
	free(m->ref);
	free(m->id);
	free(m->arch);
	free(m->branch);
}
static int install_match_cmp(const void* pa, const void* pb) {
	const install_match_t *a = pa, *b = pb;
	return strcmp(a->ref, b->ref);
}
// flatpak install(1)'s branch-less resolution: fetch the summary, fuzzy-match
// the id, let the user pick. Fills *out and *out_kind.
static int resolve_install_ref(const char* remote, int runtime_only,
			       fpkg_ref_t* out, const char** out_kind,
			       char*** srefs_out, size_t* nsrefs_out) {
	*srefs_out = NULL;
	*nsrefs_out = 0;

	const remote_refs_t* rr =
		remote_refs_cached(remote, "Looking for matches");
	if (!rr || rr->failed) {
		fpkg_errorf("cannot fetch refs for remote '%s'", remote);
		return -1;
	}
	char** srefs = rr->refs;
	size_t nrefs = rr->n;

	install_match_t* matches = NULL;
	size_t nm = 0, cap = 0;
	bool fuzzy =
		strchr(out->id, '.') == NULL && strchr(out->id, '/') == NULL;
	bool any_exact = false, any_arch = false, any_branch = false;

	for (size_t i = 0; i < nrefs; i++) {
		char kind[16], id[256], arch[64], branch[64];
		if (sscanf(srefs[i], "%15[^/]/%255[^/]/%63[^/]/%63s", kind, id,
			   arch, branch) != 4)
			continue;
		bool is_runtime = strcmp(kind, "runtime") == 0;
		// A summary also carries kinds that are not an installable ref for
		// this lookup - NVIDIA publishes internal/<id>/<arch>/pinned next to
		// the app - and without --runtime the answer is one app.
		if (runtime_only ? !is_runtime : strcmp(kind, "app") != 0) continue;

		bool exact = strcmp(id, out->id) == 0;
		if (exact) {
			any_exact = true;
		} else if (fuzzy) {
			if (is_runtime && (str_has_suffix(id, ".Debug") ||
					   str_has_suffix(id, ".Sources") ||
					   str_has_suffix(id, ".Locale")))
				continue;
			char idl[256], terml[256];
			snprintf(idl, sizeof(idl), "%s", id);
			for (char* c = idl; *c; c++)
				*c = (char)tolower((unsigned char)*c);
			snprintf(terml, sizeof(terml), "%s", out->id);
			for (char* c = terml; *c; c++)
				*c = (char)tolower((unsigned char)*c);
			if (strstr(idl, terml) == NULL &&
			    levenshtein(out->id, id) > 2)
				continue;
		} else {
			continue;
		}
		if (strcmp(arch, out->arch) == 0) any_arch = true;
		if (out->branch && *out->branch &&
		    strcmp(branch, out->branch) == 0)
			any_branch = true;

		if (nm == cap) {
			cap = cap ? cap * 2 : 16;
			install_match_t* new_matches =
				realloc(matches, cap * sizeof(install_match_t));
			if (!new_matches) {
				fpkg_errorf("out of memory");
				exit(1);
			}
			matches = new_matches;
		}
		install_match_t* m = &matches[nm++];
		m->ref = xstrdup(srefs[i]);
		m->id = xstrdup(id);
		m->arch = xstrdup(arch);
		m->branch = xstrdup(branch);
		m->runtime = is_runtime;
	}

	if (any_exact) {
		size_t w = 0;
		for (size_t i = 0; i < nm; i++) {
			if (strcmp(matches[i].id, out->id) == 0) {
				if (w != i) matches[w] = matches[i];
				w++;
			} else {
				install_match_free(&matches[i]);
			}
		}
		nm = w;
	}
	if (any_arch) {
		size_t w = 0;
		for (size_t i = 0; i < nm; i++) {
			if (strcmp(matches[i].arch, out->arch) == 0) {
				if (w != i) matches[w] = matches[i];
				w++;
			} else {
				install_match_free(&matches[i]);
			}
		}
		nm = w;
	}
	if (any_branch) {
		size_t w = 0;
		for (size_t i = 0; i < nm; i++) {
			if (strcmp(matches[i].branch, out->branch) == 0) {
				if (w != i) matches[w] = matches[i];
				w++;
			} else {
				install_match_free(&matches[i]);
			}
		}
		nm = w;
	}
	qsort(matches, nm, sizeof(install_match_t), install_match_cmp);

	int rc = -1;
	if (nm == 0) {
		fpkg_errorf("nothing matches %s in remote %s", out->id, remote);
		goto out;
	}

	int chosen = -1;
	if (nm == 1) {
		bool exact = strcmp(matches[0].id, out->id) == 0;
		if (!fpkg_interactive() || exact) {
			chosen = 0;
		} else {
			fpkg_infof("Found ref '%s' in remote '%s' (system).",
				   matches[0].ref, remote);
			if (fp_prompt_confirm_default("Use this ref?", true))
				chosen = 0;
			else
				fpkg_errorf("no ref chosen to resolve "
					    "matches for '%s'",
					    out->id);
		}
	} else {
		if (!fpkg_interactive()) {
			fpkg_errorf("multiple refs match '%s', unable to "
				    "proceed in non-interactive mode",
				    out->id);
			goto out;
		}
		{
			char hdr[512];
			snprintf(hdr, sizeof(hdr),
				 "Similar refs found for '%s' in remote '%s' "
				 "(system):",
				 out->id, remote);
			fpkg_infof("%s", hdr);
		}
		const char** items = malloc(nm * sizeof(char*));
		if (!items) {
			fpkg_errorf("out of memory");
			goto out;
		}
		for (size_t i = 0; i < nm; i++) items[i] = matches[i].ref;
		int pick = fp_prompt_menu("choose a ref", items, nm);
		free(items);
		if (pick < 0)
			fpkg_errorf("no ref chosen to resolve matches "
				    "for '%s'",
				    out->id);
		else
			chosen = pick;
	}
	if (chosen < 0) goto out;

	free(out->id);
	free(out->arch);
	free(out->branch);
	out->id = xstrdup(matches[chosen].id);
	out->arch = xstrdup(matches[chosen].arch);
	out->branch = xstrdup(matches[chosen].branch);
	*out_kind = matches[chosen].runtime ? "runtime" : "app";
	rc = 0;

out:
	for (size_t i = 0; i < nm; i++) install_match_free(&matches[i]);
	free(matches);
	return rc;
}
// Resolve an install request where the choice belongs: in the caller.
// The daemon takes exact refs only, so a partial name or a missing
// branch is a question, and only a terminal can answer it.
char* ostree_resolve_install_request(const char* request, bool runtime_only) {
	assert(request != NULL);
	fpkg_ref_t r;
	char* remote = NULL;
	char* out = NULL;
	const char* kind = NULL;
	char** unused = NULL;
	size_t nunused = 0;
	bool named_remote;

	if (ref_parse(request, &r) != 0) {
		fpkg_errorf("invalid ref '%s' "
			    "(want [remote:]id[/arch][//branch])",
			    request);
		return NULL;
	}
	named_remote = r.remote != NULL;
	if (named_remote) {
		remote = xstrdup(r.remote);
	} else {
		remote = first_remote();
		if (remote == NULL) {
			fpkg_errorf("no remotes configured - use "
				    "remote:REF or add a remote first");
			ref_free(&r);
			return NULL;
		}
	}
	if (r.branch == NULL) {
		char* b = remote_setting(remote, "flatpkg.branch");
		if (b != NULL && *b != '\0') {
			r.branch = xstrdup(b);
			free(b);
		} else {
			free(b);
			r.branch = xstrdup("stable");
		}
	}
	if (resolve_install_ref(remote, runtime_only, &r, &kind, &unused,
				&nunused) != 0) {
		ref_free(&r);
		free(remote);
		return NULL;
	}
	if (r.arch != NULL && *r.arch != '\0') {
		// With an arch the spelling is id/arch/branch; "//branch" only
		// means "no arch" and would put a slash in the branch.
		if (asprintf(&out, "%s%s%s/%s/%s",
			     named_remote ? remote : "",
			     named_remote ? ":" : "", r.id, r.arch,
			     r.branch) < 0)
			out = NULL;
	} else {
		if (asprintf(&out, "%s%s%s//%s", named_remote ? remote : "",
			     named_remote ? ":" : "", r.id, r.branch) < 0)
			out = NULL;
	}
	ref_free(&r);
	free(remote);
	return out;
}
static bool summary_has(char** srefs, size_t nsrefs, const char* ref) {
	assert(ref != NULL);
	for (size_t i = 0; i < nsrefs; i++) {
		if (strcmp(srefs[i], ref) == 0) return true;
	}
	return false;
}
// Append one item to a ", "-joined buffer sized by the caller.
static size_t perms_append(char* s, size_t off, const char* item, bool first) {
	assert(s != NULL);
	assert(item != NULL);
	if (!first) {
		memcpy(s + off, ", ", 2);
		off += 2;
	}
	size_t len = strlen(item);
	memcpy(s + off, item, len);
	return off + len;
}
// Join a strlist into a heap-allocated ", "-separated string; NULL if empty.
static char* perms_join(const strlist_t* l) {
	if (!l || !l->count) return NULL;
	size_t sz = 1;
	for (size_t i = 0; i < l->count; i++) sz += strlen(l->items[i]) + 2;
	char* s = malloc(sz);
	if (!s) return NULL;
	size_t off = 0;
	for (size_t i = 0; i < l->count; i++)
		off = perms_append(s, off, l->items[i], i == 0);
	s[off] = '\0';
	return s;
}
// Join two strlists into one heap-allocated ", "-separated string.
static char* perms_join2(const strlist_t* a, const strlist_t* b) {
	size_t na = a ? a->count : 0;
	size_t nb = b ? b->count : 0;
	if (!na && !nb) return NULL;
	size_t sz = 1;
	for (size_t i = 0; i < na; i++) sz += strlen(a->items[i]) + 2;
	for (size_t i = 0; i < nb; i++) sz += strlen(b->items[i]) + 2;
	char* s = malloc(sz);
	if (!s) return NULL;
	size_t off = 0;
	bool first = true;
	for (size_t i = 0; i < na; i++) {
		off = perms_append(s, off, a->items[i], first);
		first = false;
	}
	for (size_t i = 0; i < nb; i++) {
		off = perms_append(s, off, b->items[i], first);
		first = false;
	}
	s[off] = '\0';
	return s;
}
// The permissions an app's metadata declares, as a 2-column table
// before the changeset; apps only, and silent when none are declared.
static void perms_print(const fp_metadata_t* m) {
	assert(m != NULL);
	// Category labels (string literals) and matching heap-allocated values.
	static const char* const klabels[] = {
		"Shared",   "Sockets",	   "Devices",	 "Filesystems",
		"Features", "Session bus", "System bus", "Persistent",
	};
	char* kvals[FP_TABLE_COLS(klabels)];
	size_t np = 0;

	kvals[np] = perms_join(&m->shared);
	if (kvals[np]) np++;
	kvals[np] = perms_join(&m->sockets);
	if (kvals[np]) np++;
	kvals[np] = perms_join(&m->devices);
	if (kvals[np]) np++;
	kvals[np] = perms_join(&m->filesystems);
	if (kvals[np]) np++;
	kvals[np] = perms_join(&m->features);
	if (kvals[np]) np++;
	kvals[np] = perms_join2(&m->session_bus_talk, &m->session_bus_own);
	if (kvals[np]) np++;
	kvals[np] = perms_join(&m->system_bus_policy);
	if (kvals[np]) np++;
	kvals[np] = perms_join(&m->persistent);
	if (kvals[np]) np++;

	if (!np) return;

	const char* headers[] = {"Permission", "Access"};
	enum { PERM_KEY, PERM_VAL, PERM_COLS = FP_TABLE_COLS(headers) };
	char*** rows = calloc(np, sizeof(char**));
	char** cells = calloc(np * PERM_COLS, sizeof(char*));
	if (!rows || !cells) {
		for (size_t i = 0; i < np; i++) free(kvals[i]);
		free(rows);
		free(cells);
		return;
	}
	for (size_t i = 0; i < np; i++) {
		rows[i] = &cells[i * PERM_COLS];
		cells[i * PERM_COLS + PERM_KEY] = xstrdup(klabels[i]);
		cells[i * PERM_COLS + PERM_VAL] = kvals[i];
	}
	fp_print_table(headers, PERM_COLS, (const char* const* const*)rows, np,
		       FP_TABLE_OUTPUT);
	for (size_t i = 0; i < np * PERM_COLS; i++) free(cells[i]);
	free(rows);
	free(cells);
}
void changeset_show_full(const flatpkg_changeset_t* cs, const char* remote) {
	assert(cs != NULL);

	enum {
		CH_ID,
		CH_ARCH,
		CH_BRANCH,
		CH_OP,
		CH_REMOTE,
		CH_COLS,
	};
	const char* headers[CH_COLS] = {
		[CH_ID] = "ID",	      [CH_ARCH] = "Arch",
		[CH_BRANCH] = "Branch", [CH_OP] = "Operation",
		[CH_REMOTE] = "Remote",
	};
	char*** rows = NULL;
	char** cells = NULL;
	size_t nrows = cs->count;
	size_t nr = 0;

	rows = calloc(nrows, sizeof(char**));
	cells = calloc(nrows * CH_COLS, sizeof(char*));
	if (!rows || !cells) {
		free(rows);
		free(cells);
		return;
	}

	for (size_t i = 0; i < cs->count; i++) {
		const flatpkg_change_t* c = &cs->items[i];
		if (c->type == FP_CHANGE_KEEP) continue;

		fpkg_ref_t r;
		if (ref_parse(c->ref, &r) != 0) {
			ref_free(&r);
			continue;
		}

		const char* op; // every case below, and the default, assigns it
		switch (c->type) {
		case FP_CHANGE_INSTALL:
			op = "Install";
			break;
		case FP_CHANGE_UPGRADE:
			op = "Upgrade";
			break;
		case FP_CHANGE_DOWNGRADE:
			op = "Downgrade";
			break;
		case FP_CHANGE_REMOVE:
			op = "Remove";
			break;
		case FP_CHANGE_REINSTALL:
			op = "Reinstall";
			break;
		// The caller asked for this ref and there is nothing to do:
		// name it rather than printing nothing at all.
		case FP_CHANGE_NOOP:
			op = "No-op";
			break;
		default:
			op = "???";
			break;
		}

		rows[nr] = &cells[nr * CH_COLS];
		cells[nr * CH_COLS + CH_ID] = xstrdup(r.id);
		cells[nr * CH_COLS + CH_ARCH] =
			xstrdup(r.arch ? r.arch : "");
		cells[nr * CH_COLS + CH_BRANCH] =
			xstrdup(r.branch ? r.branch : "");
		cells[nr * CH_COLS + CH_OP] = xstrdup(op);
		cells[nr * CH_COLS + CH_REMOTE] =
			xstrdup(remote ? remote : "-");
		nr++;
		ref_free(&r);
	}

	if (nr > 0) {
		fp_print_table(headers, CH_COLS,
			       (const char* const* const*)rows, nr,
			       FP_TABLE_OUTPUT);
	}

	for (size_t i = 0; i < nr * CH_COLS; i++) free(cells[i]);
	free(cells);
	free(rows);
}
static size_t ext_branches(const fp_ext_group_t* g, const char* parent_branch,
			   const char** out, size_t max) {
	assert(g != NULL);
	if (g->versions.count > 0) {
		size_t n = g->versions.count < max ? g->versions.count : max;
		for (size_t i = 0; i < n; i++) out[i] = g->versions.items[i];
		return n;
	}
	out[0] = (g->version && *g->version) ? g->version : parent_branch;
	return 1;
}
// The remote lists one GL extension per nvidia driver version (thousands of
// refs), so this is asked per candidate: look it up once, in-process, rather
// than spawning sysctl for each.
static const char* nvidia_driver_version(void) {
	static char ver[128];
	static bool done;
	if (done) return ver;
	done = true;
	FILE* f = fopen("/sys/module/nvidia/version", "r");
	if (f) {
		if (fgets(ver, sizeof(ver), f)) ver[strcspn(ver, "\n")] = 0;
		fclose(f);
	}
	if (!ver[0]) {
		size_t len = sizeof(ver) - 1;
		if (sysctlbyname("hw.nvidia.version", ver, &len, NULL, 0) !=
			    0 &&
		    errno != ENOMEM)
			len = 0;
		ver[len] = 0;
		ver[strcspn(ver, "\n")] = 0;
	}
	return ver;
}
static bool gl_driver_name_matches(const char* basename) {
	assert(basename != NULL);
	const char* env = cli_getenv("FLATPAK_GL_DRIVERS");
	if (env && *env) {
		char* dup = strdup(env);
		char* save = NULL;
		for (char* t = strtok_r(dup, ":", &save); t;
		     t = strtok_r(NULL, ":", &save)) {
			if (strcmp(t, basename) == 0) {
				free(dup);
				return true;
			}
		}
		free(dup);
		return false;
	}
	if (strcmp(basename, "default") == 0 || strcmp(basename, "host") == 0)
		return true;
	const char* ver = nvidia_driver_version();
	if (!ver[0]) return false;
	char want[160];
	snprintf(want, sizeof(want), "nvidia-%s", ver);
	for (char* d = want; *d; d++)
		if (*d == '.') *d = '-';
	return strcmp(basename, want) == 0;
}
// flatpak --gl-drivers: the drivers flatpak would consider active here, one
// per line.  FLATPAK_GL_DRIVERS overrides the choice, and the only driver this
// host can contribute is the NVIDIA one (everything else is the runtime's
// default Mesa).
void cmd_gl_drivers(void) {
	const char* env = cli_getenv("FLATPAK_GL_DRIVERS");

	if (env != NULL && *env != '\0') {
		char* dup = xstrdup(env);

		for (char* t = strtok(dup, ":"); t != NULL;
		     t = strtok(NULL, ":"))
			fp_outl("%s", t);
		free(dup);
		return;
	}
	fp_outl("default");
	const char* ver = nvidia_driver_version();
	if (ver[0] != '\0') {
		char want[160];

		snprintf(want, sizeof(want), "nvidia-%s", ver);
		for (char* d = want; *d != '\0'; d++)
			if (*d == '.') *d = '-';
		fp_outl("%s", want);
	}
}
// `remote-info`: which ref the remote advertises for a request.  The match is
// install's, against the named remote, and an ambiguous one is an error the
// caller reports rather than a question nobody can answer mid-report.
int remote_info_ref(const char* remote, const char* request, bool runtime_only,
		    char* out, size_t outlen) {
	fpkg_ref_t r;
	const char* kind = NULL;
	char** srefs = NULL;
	size_t nsrefs = 0;

	if (remote == NULL || !remote_name_valid(remote)) return -1;
	if (ref_parse(request, &r) != 0) return -1;
	if (r.branch == NULL) r.branch = xstrdup("stable");
	if (resolve_install_ref(remote, runtime_only, &r, &kind, &srefs,
				&nsrefs) != 0) {
		ref_free(&r);
		return -1;
	}
	snprintf(out, outlen, "%s/%s/%s/%s", kind, r.id,
		 r.arch != NULL ? r.arch : host_arch(),
		 r.branch != NULL ? r.branch : "stable");
	ref_free(&r);
	return 0;
}
// Once per process, like the GL driver: every Gtk3theme candidate asks.
static const char* active_gtk_theme(void) {
	static char theme[128];
	static bool done;
	if (done) return theme;
	done = true;

	int fds[2];
	if (pipe(fds) != 0) return theme;
	int null = open("/dev/null", O_WRONLY);
	char* argv[] = {"/usr/local/bin/dconf", "read",
			"/org/gnome/desktop/interface/gtk-theme", NULL};
	int rc = run_program(argv, -1, fds[1], null);
	close(fds[1]);
	if (null >= 0) close(null);
	if (rc != 0) {
		close(fds[0]);
		return theme;
	}

	FILE* f = fdopen(fds[0], "r");
	if (!f) {
		close(fds[0]);
		return theme;
	}
	char raw[128] = "";
	if (fgets(raw, sizeof(raw), f)) {
		raw[strcspn(raw, "\n")] = 0;
		char* q = strchr(raw, '\'');
		if (q++) {
			char* e = strchr(q, '\'');
			if (e) *e = '\0';
			snprintf(theme, sizeof(theme), "%s", q);
		}
	}
	fclose(f);
	return theme;
}
static bool download_if_matches(const char* download_if,
				const char* extension_id, bool default_value) {
	assert(extension_id != NULL);
	if (download_if == NULL || !*download_if) return default_value;

	char* dup = strdup(extension_id);
	char* basename = strrchr(dup, '.');
	basename = basename ? basename + 1 : dup;

	char* reasons = strdup(download_if);
	bool match = false;
	char* saveptr;
	char* reason = strtok_r(reasons, ";", &saveptr);
	while (reason) {
		if (strcmp(reason, "active-gl-driver") == 0) {
			if (gl_driver_name_matches(basename)) match = true;
		} else if (strcmp(reason, "active-gtk-theme") == 0) {
			if (strcmp(active_gtk_theme(), basename) == 0)
				match = true;
		} else if (strncmp(reason, "on-xdg-desktop-", 15) == 0) {
			const char* desktop = cli_getenv("XDG_CURRENT_DESKTOP");
			if (desktop) {
				char* dd = strdup(desktop);
				char* dsave = NULL;
				for (char* t = strtok_r(dd, ":", &dsave); t;
				     t = strtok_r(NULL, ":", &dsave)) {
					if (strcasecmp(t, reason + 15) == 0)
						match = true;
				}
				free(dd);
			}
		} else if (strcmp(reason, "have-intel-gpu") == 0) {
			if (flatpkg_kernel_module_loaded("i915") ||
			    flatpkg_kernel_module_loaded("i915kms") ||
			    flatpkg_kernel_module_loaded("xe"))
				match = true;
		} else if (strcmp(reason, "have-nvidia-gpu") == 0) {
			if (flatpkg_kernel_module_loaded("nvidia"))
				match = true;
		} else if (strncmp(reason, "have-kernel-module-", 18) == 0) {
			if (flatpkg_kernel_module_loaded(reason + 18))
				match = true;
		}
		reason = strtok_r(NULL, ";", &saveptr);
	}
	free(dup);
	free(reasons);
	return match;
}
// Should this extension id be auto-installed alongside its parent?
static bool extension_should_install(const fp_ext_group_t* g,
				     const char* ext_id, const char* arch,
				     const char* branch) {
	assert(g != NULL);
	assert(ext_id != NULL);
	char files[PATH_MAX];
	if (store_find_extension(ext_id, arch, branch, files, sizeof(files)) ==
	    0)
		return false;
	// ".Debug" is never auto-installed.
	if (str_has_suffix(g->id, ".Debug")) return false;
	if (download_if_matches(g->download_if, ext_id, !g->no_autodownload))
		return true;
	// Automatic branch following: an extension is still installed when some
	// branch of the same id/arch is already deployed.
	if (store_extension_any_branch(ext_id, arch)) return true;
	return false;
}
// Branches of an extension group that should be installed now for a parent
// on parent_branch: the solver's view of extension_should_install().
size_t ostree_group_install_branches(const fp_ext_group_t* g, const char* arch,
				     const char* parent_branch,
				     const char** out, size_t max) {
	assert(g != NULL);
	assert(arch != NULL);
	assert(out != NULL);
	const char* all[16];
	size_t nb = ext_branches(g, parent_branch, all, 16);
	size_t n = 0;
	for (size_t i = 0; i < nb && n < max; i++)
		if (extension_should_install(g, g->id, arch, all[i]))
			out[n++] = all[i];
	return n;
}
void install_extensions(const char* remote, const fpkg_ref_t* rt,
			       char** srefs, size_t nsrefs) {
	char rmeta_path[PATH_MAX];
	snprintf(rmeta_path, sizeof(rmeta_path),
		 "%s/runtime/%s/%s/%s/"
		 "active/metadata",
		 store_root(), rt->id, rt->arch, rt->branch ? rt->branch : "");
	fp_metadata_t rmeta;
	if (fp_metadata_parse_any(rmeta_path, &rmeta) != 0) return;
	fpkg_ref_t* refs = NULL;
	size_t nrefs = 0;
	related_refs_for_metadata(&rmeta, rt->branch, rt->arch, srefs, nsrefs,
				  &refs, &nrefs);
	fp_metadata_free(&rmeta);
	if (nrefs > 1) {
		// A runtime brings several extensions (GL, Locale, codecs...):
		// pull them together, then check each out in turn.
		pull_job_t* jobs = calloc(nrefs, sizeof(*jobs));
		if (jobs) {
			for (size_t i = 0; i < nrefs; i++) {
				char refstr[512];
				ref_full(&refs[i], "runtime", refstr,
					 sizeof(refstr));
				pull_job_init(&jobs[i], remote, refstr);
			}
			(void)parallel_pull(jobs, nrefs, NULL);
			for (size_t i = 0; i < nrefs; i++)
				if (jobs[i].rc == 0)
					prefetched_add(jobs[i].refstr);
			pull_jobs_free(jobs, nrefs);
			free(jobs);
		}
	}
	for (size_t i = 0; i < nrefs; i++) {
		if (install_one(&refs[i], "runtime", "extension", remote) ==
		    0) {
			char eref[512];
			snprintf(eref, sizeof(eref), "extension/%s/%s/%s",
				 refs[i].id, refs[i].arch,
				 refs[i].branch ? refs[i].branch : "");
			history_append("install", eref, NULL, refs[i].branch);
			fpkg_okf("installed extension %s", refs[i].id);
		}
		ref_free(&refs[i]);
	}
	free(refs);
}
// Solver-planned extension rows arrive as "extension/<id>/<arch>/<branch>"
// but the store keeps no such ref: flathub publishes the extension under the
// runtime prefix, so pull as runtime and check out as extension.
int ostree_install_extension_ref(const char* ref) {
	assert(ref != NULL);
	fpkg_ref_t r;
	if (ref_parse(ref, &r) != 0) {
		fpkg_errorf("invalid ref '%s'", ref);
		return -1;
	}
	char* remote = NULL;
	if (r.remote) {
		remote = xstrdup(r.remote);
	} else {
		remote = first_remote();
		if (!remote) {
			fpkg_errorf("no remotes configured - use "
				    "remote:REF or add a remote first");
			ref_free(&r);
			return -1;
		}
	}
	if (!r.arch) {
		free(r.arch);
		r.arch = xstrdup(host_arch());
	}
	if (!r.branch) r.branch = xstrdup("stable");
	int rc = install_one(&r, "runtime", "extension", remote);
	if (rc == 0) fpkg_okf("installed extension %s", r.id);
	free(remote);
	ref_free(&r);
	return rc;
}
static int parse_metadata_text(const char* body, fp_metadata_t* out) {
	char tmpl[] = "/tmp/fpkgmeta.XXXXXX";
	int fd = mkstemp(tmpl);
	if (fd < 0) {
		fpkg_errorf("mkstemp: %s", strerror(errno));
		return -1;
	}
	ssize_t nw = write(fd, body, strlen(body));
	close(fd);
	if (nw < 0) {
		(void)unlink(tmpl);
		fpkg_errorf("write %s: %s", tmpl, strerror(errno));
		return -1;
	}
	// Runtimes use [Runtime] (no name=/command=), so parse them leniently
	// or their metadata is rejected and the deploy skipped.
	int rc = fp_metadata_parse_any(tmpl, out);
	(void)unlink(tmpl);
	return rc;
}
static int read_remote_metadata(const char* commit, fp_metadata_t* out) {
	assert(commit != NULL);
	assert(out != NULL);
	char* body = commit_xa_metadata(commit);
	if (!body) body = cat_commit_metadata(commit);
	if (!body) return -1;
	int rc = parse_metadata_text(body, out);
	free(body);
	return rc;
}
static int summary_ref_metadata(const char* remote, const char* refstr,
				fp_metadata_t* out) {
	char* body = summary_ref_metadata_text(remote, refstr);
	if (body == NULL) return -1;
	int rc = parse_metadata_text(body, out);
	free(body);
	return rc;
}
// Remotes without xa.cache (hand-made or file:// repos) have to hand over
// the commit instead.
static int remote_ref_metadata(const char* remote, const char* refstr,
			       fp_metadata_t* out) {
	if (summary_ref_metadata(remote, refstr, out) == 0) return 0;
	if (tool_pull_metadata(remote, refstr) != 0) return -1;
	tool_ctx_t tc;
	tool_ctx_init(&tc);
	char commit[128];
	if (tool_rev_parse(&tc, refstr, commit, sizeof(commit)) != 0) return -1;
	return read_remote_metadata(commit, out);
}
// Is this ref already in an extension set? Dedups aexts/rextexts when a
// subdirectory extension has versions=25.08;25.08-extra.
static bool ext_already_in_set(const fpkg_ref_t* set, size_t n,
			       const fpkg_ref_t* e) {
	assert(set != NULL || n == 0);
	assert(e != NULL || n == 0);
	for (size_t i = 0; i < n; i++) {
		if (strcmp(set[i].id, e->id) == 0 &&
		    strcmp(set[i].arch ? set[i].arch : "",
			   e->arch ? e->arch : "") == 0 &&
		    strcmp(set[i].branch ? set[i].branch : "",
			   e->branch ? e->branch : "") == 0)
			return true;
	}
	return false;
}
static void ref_set_add(fpkg_ref_t** set, size_t* n, const char* id,
			const char* arch, const char* branch) {
	assert(set != NULL);
	assert(n != NULL);
	assert(id != NULL);
	assert(arch != NULL);
	assert(branch != NULL);
	fpkg_ref_t e;
	memset(&e, 0, sizeof(e));
	e.id = xstrdup(id);
	e.arch = xstrdup(arch);
	e.branch = xstrdup(branch);
	if (ext_already_in_set(*set, *n, &e)) {
		ref_free(&e);
		return;
	}
	fpkg_ref_t* ns = realloc(*set, (*n + 1) * sizeof(**set));
	if (!ns) {
		fpkg_errorf("out of memory");
		exit(1);
	}
	ns[*n] = e;
	*set = ns;
	(*n)++;
}
static void related_refs_for_metadata(const fp_metadata_t* meta,
				      const char* parent_branch,
				      const char* arch, char** srefs,
				      size_t nsrefs, fpkg_ref_t** out,
				      size_t* nout) {
	*out = NULL;
	*nout = 0;
	for (size_t i = 0; i < meta->next_groups; i++) {
		const fp_ext_group_t* g = &meta->ext_groups[i];
		bool expand_subdirs =
			g->subdirectories || g->versions.count > 1;
		const char* branches[16];
		size_t nb = ext_branches(g, parent_branch, branches, 16);
		for (size_t b = 0; b < nb; b++) {
			char exref[512];
			{
				fpkg_ref_t ex;
				memset(&ex, 0, sizeof(ex));
				ex.id = (char*)g->id;
				ex.arch = (char*)arch;
				ex.branch = (char*)branches[b];
				ref_full(&ex, "runtime", exref, sizeof(exref));
			}
			if (summary_has(srefs, nsrefs, exref)) {
				if (extension_should_install(g, g->id, arch,
							     branches[b]))
					ref_set_add(out, nout, g->id, arch,
						    branches[b]);
				continue;
			}
			if (!expand_subdirs) continue;
			size_t idlen = strlen(g->id);
			for (size_t s = 0; s < nsrefs; s++) {
				const char* refpart = strchr(srefs[s], '/');
				if (!refpart) continue;
				refpart++;
				if (strncmp(refpart, g->id, idlen) != 0 ||
				    refpart[idlen] != '.')
					continue;
				char subname[256];
				snprintf(subname, sizeof(subname), "%s",
					 refpart + idlen + 1);
				char* slash = strchr(subname, '/');
				if (slash) *slash = '\0';
				if (strchr(subname, '.')) continue;
				char full_id[512];
				snprintf(full_id, sizeof(full_id), "%s.%s",
					 g->id, subname);
				if (!extension_should_install(g, full_id, arch,
							      branches[b]))
					continue;
				char refstr[512];
				{
					fpkg_ref_t e;
					memset(&e, 0, sizeof(e));
					e.id = full_id;
					e.arch = (char*)arch;
					e.branch = (char*)branches[b];
					ref_full(&e, "runtime", refstr,
						 sizeof(refstr));
				}
				if (!summary_has(srefs, nsrefs, refstr))
					continue;
				ref_set_add(out, nout, full_id, arch,
					    branches[b]);
			}
		}
	}
}
static void missing_push(char*** out, size_t* n, size_t* cap, const char* type,
			 const char* id, const char* arch, const char* branch) {
	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 8;
		char** grown = realloc(*out, *cap * sizeof(char*));
		if (!grown) return;
		*out = grown;
	}
	if (*out == NULL) return;
	char buf[640];
	snprintf(buf, sizeof(buf), "%s/%s/%s/%s", type, id, arch, branch);
	(*out)[(*n)++] = xstrdup(buf);
}
char** ostree_missing_deps(const char* ref, bool full, size_t* n_out) {
	assert(ref != NULL);
	assert(n_out != NULL);
	*n_out = 0;
	char type[32], id[256], arch[64], branch[64];
	if (sscanf(ref, "%31[^/]/%255[^/]/%63[^/]/%63s", type, id, arch,
		   branch) != 4)
		return NULL;
	if (strcmp(type, "app") != 0 && strcmp(type, "runtime") != 0)
		return NULL;

	char bdir[PATH_MAX], mpath[PATH_MAX];
	snprintf(bdir, sizeof(bdir), "%s/%s/%s/%s/%s", store_root(), type, id,
		 arch, branch);
	snprintf(mpath, sizeof(mpath), "%s/active/metadata", bdir);
	fp_metadata_t meta;
	if (fp_metadata_parse_any(mpath, &meta) != 0) return NULL;

	// What the local store can tell is missing, without asking anyone.
	fpkg_ref_t rt;
	memset(&rt, 0, sizeof(rt));
	bool have_rt = false, rt_missing = false, gap = false;
	if (strcmp(type, "app") == 0 && meta.runtime_ref && *meta.runtime_ref &&
	    ref_parse(meta.runtime_ref, &rt) == 0) {
		have_rt = true;
		char rdir[PATH_MAX];
		snprintf(rdir, sizeof(rdir), "%s/runtime/%s/%s/%s/active",
			 store_root(), rt.id, rt.arch,
			 rt.branch ? rt.branch : "");
		struct stat st;
		rt_missing = stat(rdir, &st) != 0;
		gap = rt_missing;
	}
	for (size_t g = 0; g < meta.next_groups && !gap; g++) {
		const fp_ext_group_t* grp = &meta.ext_groups[g];
		if (!grp->id || !*grp->id) continue;
		// A runtime's own groups are often optional or unpublished, which would
		// send every solve to the remote for nothing: a full repair is where
		// those are resolved.
		if (!full && strcmp(type, "app") != 0) continue;
		bool open = grp->subdirectories || grp->versions.count > 1;
		if (open) {
			// Only the remote can say which of those exist.
			gap = full;
			continue;
		}
		const char* br[16];
		if (ostree_group_install_branches(grp, arch, branch, br, 16) >
		    0)
			gap = true;
	}

	char** out = NULL;
	size_t n = 0, cap = 0;
	if (gap) {
		char opath[PATH_MAX];
		snprintf(opath, sizeof(opath), "%s/.origin", bdir);
		char* remote = slurp_file(opath);
		if (remote) trim(remote);
		if (!remote || !*remote) {
			free(remote);
			remote = first_remote();
		}
		char label[160];
		snprintf(label, sizeof(label),
			 "checking %s for missing dependencies",
			 remote ? remote : "the remote");
		remote_refs_t* rr =
			remote ? remote_refs_cached(remote, label) : NULL;
		if (rr && rr->failed && !rr->warned) {
			rr->warned = true;
			fpkg_warnf("cannot reach %s: dependencies that may be "
				   "missing were not checked",
				   remote);
		}
		if (rr && !rr->failed) {
			if (rt_missing) {
				char rstr[512];
				ref_full(&rt, "runtime", rstr, sizeof(rstr));
				if (summary_has(rr->refs, rr->n, rstr))
					missing_push(&out, &n, &cap, "runtime",
						     rt.id, rt.arch,
						     rt.branch ? rt.branch
							       : "");
				else
					fpkg_warnf("%s needs %s, which %s does "
						   "not publish",
						   id, rt.id, remote);
			}
			fpkg_ref_t* refs = NULL;
			size_t nrefs = 0;
			related_refs_for_metadata(&meta, branch, arch, rr->refs,
						  rr->n, &refs, &nrefs);
			for (size_t i = 0; i < nrefs; i++) {
				missing_push(&out, &n, &cap, "extension",
					     refs[i].id, refs[i].arch,
					     refs[i].branch ? refs[i].branch
							    : "");
				ref_free(&refs[i]);
			}
			free(refs);
		}
		free(remote);
	}
	if (have_rt) ref_free(&rt);
	fp_metadata_free(&meta);
	*n_out = n;
	return out;
}
// A changeset of only KEEP entries asks for nothing, so say so rather
// than print nothing at all; NOOP counts as actionable, since the
// table names the explicit request.
static bool changeset_is_actionable(const flatpkg_changeset_t* cs) {
	assert(cs != NULL);
	for (size_t i = 0; i < cs->count; i++)
		if (cs->items[i].type != FP_CHANGE_KEEP) return true;
	return false;
}
static void cs_add_install_once(flatpkg_changeset_t* cs, const char* ref,
				const char* new_version) {
	assert(cs != NULL);
	assert(ref != NULL);
	for (size_t i = 0; i < cs->count; i++)
		if (cs->items[i].type == FP_CHANGE_INSTALL &&
		    cs->items[i].ref && strcmp(cs->items[i].ref, ref) == 0)
			return;
	if (cs->count == cs->cap) {
		size_t ncap = cs->cap ? cs->cap * 2 : 16;
		flatpkg_change_t* ng = realloc(cs->items, ncap * sizeof(*ng));
		if (!ng) {
			fpkg_errorf("out of memory");
			exit(1);
		}
		cs->items = ng;
		cs->cap = ncap;
	}
	flatpkg_change_t* c = &cs->items[cs->count++];
	memset(c, 0, sizeof(*c));
	c->type = FP_CHANGE_INSTALL;
	c->ref = xstrdup(ref);
	c->new_version = new_version ? xstrdup(new_version) : NULL;
}
// The solver plans the app's runtime and its own extensions, not the
// runtime's (Locale, GL, codecs), which only turn up in install_extensions()
// after it deploys: expand them here so the table and total agree.
// A runtime's own [Extension] groups, resolved against the remote that has the
// runtime - the app's remote usually does not - and filtered by each group's
// download-if/enable-if rules.  Called for every runtime the plan installs and
// for the runtime an incoming app declares, because an installed runtime does
// not mean its extensions are there: GeForce NOW's 24.08 runtime was deployed
// without the GL extension that belongs to it, and every EGL lookup failed.
static size_t expand_one_runtime(flatpkg_changeset_t* cs, const char* remote,
				 const char* runtime_ref) {
	assert(cs != NULL);
	size_t added = 0;
	fpkg_ref_t ir;

	if (runtime_ref == NULL || *runtime_ref == '\0') return 0;
	if (ref_parse(runtime_ref, &ir) != 0) return 0;
	if (!ir.arch) {
		free(ir.arch);
		ir.arch = xstrdup(host_arch());
	}
	if (!ir.branch) ir.branch = xstrdup("stable");
	char refstr[512];
	ref_full(&ir, "runtime", refstr, sizeof(refstr));
	// The lookup wants the kind word: `remote_has_ref` matches a summary's
	// `runtime/<id>/<arch>/<branch>` against a full ref.
	char* use_remote =
		ir.remote ? xstrdup(ir.remote)
			  : remote_for_ref(remote, refstr);
	fp_metadata_t rmeta;
	if (use_remote == NULL ||
	    remote_ref_metadata(use_remote, refstr, &rmeta) != 0) {
		free(use_remote);
		ref_free(&ir);
		return 0;
	}
	remote_refs_t* rr =
		remote_refs_cached(use_remote, "Looking up extensions");
	if (!rr || rr->failed) {
		free(use_remote);
		fp_metadata_free(&rmeta);
		ref_free(&ir);
		return 0;
	}
	fpkg_ref_t* refs = NULL;
	size_t nrefs = 0;
	related_refs_for_metadata(&rmeta, ir.branch, ir.arch, rr->refs, rr->n,
				  &refs, &nrefs);
	fp_metadata_free(&rmeta);
	for (size_t k = 0; k < nrefs; k++) {
		char eref[512];
		snprintf(eref, sizeof(eref), "extension/%s/%s/%s", refs[k].id,
			 refs[k].arch, refs[k].branch ? refs[k].branch : "");
		size_t before = cs->count;
		cs_add_install_once(cs, eref, refs[k].branch);
		if (cs->count != before) added++;
		ref_free(&refs[k]);
	}
	free(refs);
	free(use_remote);
	ref_free(&ir);
	return added;
}

static size_t expand_runtime_extensions(flatpkg_changeset_t* cs,
					const char* remote) {
	assert(cs != NULL);
	size_t added = 0;
	size_t orig = cs->count;

	for (size_t i = 0; i < orig; i++) {
		flatpkg_change_t* c = &cs->items[i];

		if (c->type != FP_CHANGE_INSTALL) continue;
		if (strncmp(c->ref, "runtime/", 8) != 0) continue;
		added += expand_one_runtime(cs, remote, c->ref);
	}
	return added;
}
static void install_req_free(install_req_t* q) {
	assert(q != NULL);
	fp_metadata_free(&q->meta);
	free(q->remote);
	ref_free(&q->ref);
	memset(q, 0, sizeof(*q));
}
static int install_req_prepare(const char* request, flatpkg_dep_kind_t kind,
			       const char* commit, install_req_t* out) {
	assert(request != NULL);
	assert(out != NULL);
	memset(out, 0, sizeof(*out));
	out->kind = kind;
	out->is_runtime = kind == FLATPKG_DEP_KIND_RUNTIME;
	out->is_ext = kind == FLATPKG_DEP_KIND_EXTENSION;
	out->dep_type =
		out->is_ext ? "extension" : out->is_runtime ? "runtime" : "app";

	if (ref_parse(request, &out->ref) != 0) {
		fpkg_errorf("invalid ref '%s' "
			    "(want [remote:]id[/arch][//branch])",
			    request);
		return 1;
	}
	fpkg_ref_t* r = &out->ref;

	// A runtime/extension may have no remote or branch in the ref: fall
	// back to the first configured remote and its default branch.
	if (r->remote)
		out->remote = xstrdup(r->remote);
	else {
		out->remote = first_remote();
		if (!out->remote) {
			fpkg_errorf("no remotes configured - use "
				    "remote:REF or add a remote first");
			install_req_free(out);
			return 1;
		}
	}
	bool had_branch = r->branch != NULL;
	if (!r->branch) {
		char* b = remote_setting(out->remote, "flatpkg.branch");
		if (b && *b) {
			r->branch = xstrdup(b);
			free(b);
		} else {
			r->branch = xstrdup("stable");
		}
	}

	// flatpak install(1): a name or an id without a branch is looked up in
	// the remote and a partial match confirmed before anything happens;
	// a plan nested in another one has nothing to look up.
	if (!out->is_ext && !flatpkg_txn_active()) {
		bool partial =
			strchr(r->id, '.') == NULL && strchr(r->id, '/') == NULL;
		// A complete id that is already installed needs no lookup: the answer
		// is "already installed", and asking the remote costs seconds.
		bool have = !partial && (deploy_exists("app", r->id) ||
					 deploy_exists("runtime", r->id));
		if ((partial || !had_branch) && !have) {
			const char* rkind = NULL;
			char** unused = NULL;
			size_t nunused = 0;
			fpkg_span_t sp_resolve = fpkg_span_begin("resolve");
			int resolve_rc = resolve_install_ref(
				out->remote, out->is_runtime, r, &rkind, &unused,
				&nunused);
			fpkg_span_end(&sp_resolve);
			if (resolve_rc != 0) {
				install_req_free(out);
				return 1;
			}
			if (strcmp(rkind, "runtime") == 0) {
				out->is_runtime = true;
				out->kind = FLATPKG_DEP_KIND_RUNTIME;
				out->dep_type = "runtime";
			}
		}
	}

	// For an app install, fetch the app's metadata early so the solver
	// can build the correct closure including its runtime and extensions.
	if (!out->is_runtime && !out->is_ext) {
		char refstr[512];
		ref_full(r, "app", refstr, sizeof(refstr));
		// An explicit --commit may predate what the summary describes.
		bool meta_from_summary = false;
		fpkg_span_t phase_meta = fpkg_span_begin("metadata");
		const char* fetched = plan_meta_text(refstr);
		if (fetched != NULL)
			meta_from_summary =
				parse_metadata_text(fetched, &out->meta) == 0;
		else if (!(commit && *commit))
			meta_from_summary =
				summary_ref_metadata(out->remote, refstr,
						     &out->meta) == 0;
		if (!meta_from_summary) {
			tool_ctx_t tc;
			tool_ctx_init(&tc);
			char mcommit[256];
			if (commit && *commit) {
				snprintf(mcommit, sizeof(mcommit), "%s",
					 commit);
			} else {
				if (tool_rev_probe(&tc, refstr, mcommit,
						   sizeof(mcommit)) != 0) {
					// Ref not found locally, pull it.
					tool_ctx_t tc2;
					tool_ctx_init(&tc2);
					char repo_arg[PATH_MAX + 32];
					snprintf(repo_arg, sizeof(repo_arg),
						 "--repo=%s", tc2.repo);
					char refhdr[600];
					flatpak_ref_header(refstr, refhdr,
							   sizeof(refhdr));
					char* argv[] = {
						"ostree", "pull",
						"--disable-fsync", out->remote,
						refstr, repo_arg, refhdr, NULL};
					tool_res_t res;
					if (ostree_run(argv, &res) != 0 ||
					    res.rc != 0) {
						tool_res_free(&res);
						fpkg_span_end(&phase_meta);
						install_req_free(out);
						return 1;
					}
					tool_res_free(&res);
					if (tool_rev_parse(&tc, refstr, mcommit,
							   sizeof(mcommit)) !=
					    0) {
						fpkg_span_end(&phase_meta);
						install_req_free(out);
						return 1;
					}
				}
			}
			if (tool_pull_metadata(out->remote, refstr) != 0 ||
			    read_remote_metadata(mcommit, &out->meta) != 0) {
				fpkg_errorf("cannot read the metadata of %s",
					    refstr);
				fpkg_span_end(&phase_meta);
				install_req_free(out);
				return 1;
			}
		}
		fpkg_span_end(&phase_meta);
	}
	return 0;
}
// Does the changeset already carry a row for this id? A request the
// solver kept gets a NOOP row so the one table names it; one it
// planned gets no second row.
static bool changeset_has_ref_id(const flatpkg_changeset_t* cs, const char* id) {
	assert(cs != NULL);
	assert(id != NULL);
	for (size_t i = 0; i < cs->count; i++) {
		if (cs->items[i].type == FP_CHANGE_KEEP) continue;
		fpkg_ref_t r;
		if (ref_parse(cs->items[i].ref, &r) != 0) continue;
		bool same = strcmp(r.id, id) == 0;
		ref_free(&r);
		if (same) return true;
	}
	return false;
}
static void install_req_add_row(flatpkg_changeset_t* cs,
				const install_req_t* q, bool reinstall) {
	assert(cs != NULL);
	assert(q != NULL);
	if (changeset_has_ref_id(cs, q->ref.id)) return;
	char nref[512];
	ref_full(&q->ref, q->dep_type, nref, sizeof(nref));
	if (reinstall)
		changeset_add_reinstall(cs, nref);
	else
		changeset_add_noop(cs, nref);
}
static plan_meta_t g_plan_meta[128];
static size_t g_nplan_meta;
void ostree_plan_meta_reset(void) {
	for (size_t i = 0; i < g_nplan_meta; i++) {
		free(g_plan_meta[i].ref);
		free(g_plan_meta[i].text);
	}
	memset(g_plan_meta, 0, sizeof(g_plan_meta));
	g_nplan_meta = 0;
}
int ostree_plan_meta_add(const char* ref, const char* metadata) {
	assert(ref != NULL);
	if (metadata == NULL) return -1;
	if (g_nplan_meta == sizeof(g_plan_meta) / sizeof(g_plan_meta[0]))
		return -1;
	g_plan_meta[g_nplan_meta].ref = xstrdup(ref);
	g_plan_meta[g_nplan_meta].text = xstrdup(metadata);
	g_nplan_meta++;
	return 0;
}
static const char* plan_meta_text(const char* ref) {
	for (size_t i = 0; i < g_nplan_meta; i++)
		if (strcmp(g_plan_meta[i].ref, ref) == 0)
			return g_plan_meta[i].text;
	return NULL;
}
// Build the changeset in the CALLER: the world and the solver are
// policy and run unprivileged. The metadata the solver needs came
// from a PREFETCH request, because only the daemon writes the repo.
int ostree_plan_install(const char** requests, const char* const* commits,
			const flatpkg_dep_kind_t* kinds, size_t n,
			bool reinstall, fpkg_plan_t* out) {
	assert(requests != NULL);
	assert(out != NULL);
	memset(out, 0, sizeof(*out));
	if (n == 0) return 0;

	install_req_t* reqs = calloc(n, sizeof(*reqs));
	solver_incoming_t* incoming = calloc(n, sizeof(*incoming));
	size_t nincoming = 0;
	int rc = 1;

	if (reqs == NULL || incoming == NULL) {
		fpkg_errorf("out of memory");
		goto out;
	}
	for (size_t i = 0; i < n; i++) {
		flatpkg_dep_kind_t kind =
			kinds != NULL ? kinds[i] : FLATPKG_DEP_KIND_APP;
		const char* commit = commits != NULL ? commits[i] : NULL;
		if (install_req_prepare(requests[i], kind, commit, &reqs[i]) !=
		    0)
			goto out;
		if (!reqs[i].is_runtime && !reqs[i].is_ext) {
			incoming[nincoming].ref = reqs[i].ref;
			incoming[nincoming].meta = reqs[i].meta;
			nincoming++;
		}
	}
	if (world_load(&out->world) != 0) goto out;
	for (size_t i = 0; i < n; i++) {
		flatpkg_dep_t dep = {0};
		dep.kind = reqs[i].kind;
		dep.name = reqs[i].ref.id;
		world_dep_add(&out->world, &dep);
	}
	fpkg_span_t phase_solve = fpkg_span_begin("solve");
	if (solver_solve(&out->world, 0, &out->cs, incoming, nincoming) != 0) {
		fpkg_span_end(&phase_solve);
		fpkg_errorf("solver failed");
		goto out;
	}
	fpkg_span_end(&phase_solve);
	for (size_t i = 0; i < n; i++) {
		expand_runtime_extensions(&out->cs, reqs[i].remote);
		// The app's own runtime may already be installed; its extensions
		// still have to be there, and only its metadata says which.
		if (!reqs[i].is_runtime && !reqs[i].is_ext)
			expand_one_runtime(&out->cs, reqs[i].remote,
					   reqs[i].meta.runtime_ref);
	}
	for (size_t i = 0; i < n; i++)
		install_req_add_row(&out->cs, &reqs[i], reinstall);

	const char* table_remote = n == 1 ? reqs[0].remote : NULL;
	for (size_t i = 0; i < n; i++)
		if (!reqs[i].is_runtime && !reqs[i].is_ext &&
		    reqs[i].meta.app_id)
			perms_print(&reqs[i].meta);
	changeset_show_full(&out->cs, table_remote);
	out->remote = n == 1 && reqs[0].remote != NULL
			      ? xstrdup(reqs[0].remote)
			      : (n > 0 ? first_remote() : NULL);
	rc = 0;
out:
	if (reqs != NULL)
		for (size_t i = 0; i < n; i++) install_req_free(&reqs[i]);
	free(reqs);
	free(incoming);
	if (rc != 0) {
		changeset_free(&out->cs);
		world_free(&out->world);
		free(out->remote);
		memset(out, 0, sizeof(*out));
	}
	return rc;
}
// Uninstall policy in the caller: drop the constraints, solve, show the
// table and let the CLI ask; the daemon only removes the returned rows.
int ostree_plan_remove(const char** refs, size_t nrefs, bool rdepends,
		       fpkg_plan_t* out) {
	assert(refs != NULL);
	assert(nrefs > 0);
	assert(out != NULL);
	memset(out, 0, sizeof(*out));
	if (world_load(&out->world) != 0) return -1;

	char** noops = NULL;
	size_t nnoops = 0, capnoops = 0;
	bool any_installed = false;
	int rc = -1;

	for (size_t k = 0; k < nrefs; k++) {
		fpkg_ref_t r;
		if (ref_parse(refs[k], &r) != 0) {
			fpkg_errorf("invalid ref '%s'", refs[k]);
			goto out;
		}
		const char* type = NULL;
		if (deploy_exists("app", r.id)) type = "app";
		else if (deploy_exists("runtime", r.id))
			type = "runtime";
		else if (deploy_exists("extension", r.id))
			type = "extension";
		if (type == NULL) {
			if (world_dep_has(&out->world, r.id)) {
				char wfull[512];
				world_dep_del(&out->world, r.id);
				ref_full(&r, "app", wfull, sizeof(wfull));
				world_dep_del(&out->world, wfull);
				fpkg_okf("%s removed from the world (it was not "
					 "installed)",
					 r.id);
				ref_free(&r);
				continue;
			}
			ref_free(&r);
			if (nnoops == capnoops) {
				capnoops = capnoops ? capnoops * 2 : 8;
				noops = realloc(noops,
						capnoops * sizeof(*noops));
				if (noops == NULL) {
					fpkg_errorf("out of memory");
					exit(1);
				}
			}
			fpkg_infof("%s is already absent; nothing to do",
				   refs[k]);
			char nref[512];
			snprintf(nref, sizeof(nref), "app/%s/x86_64/stable",
				 refs[k]);
			noops[nnoops++] = xstrdup(nref);
			out->absent = true;
			continue;
		}
		any_installed = true;
		world_dep_del(&out->world, r.id);
		char full[512];
		ref_full(&r, "app", full, sizeof(full));
		world_dep_del(&out->world, full);
		ref_free(&r);
	}
	if (rdepends) {
		for (size_t k = 0; k < nrefs; k++) {
			fpkg_ref_t r;
			if (ref_parse(refs[k], &r) != 0) continue;
			flatpkg_dep_array_t extras = {0};
			uninstall_collect_reverse_deps(r.id, &out->world,
						       &extras);
			for (size_t e = 0; e < extras.count; e++)
				world_dep_del(&out->world,
					      extras.items[e].name);
			world_free(&extras);
			ref_free(&r);
		}
	}
	if (any_installed) {
		solver_incoming_t incoming[1] = {{0}};
		if (solver_solve(&out->world, 0, &out->cs, incoming, 0) != 0) {
			fpkg_errorf("solver failed");
			goto out;
		}
	}
	out->had_work = changeset_is_actionable(&out->cs);
	for (size_t i = 0; i < nnoops; i++)
		changeset_add_noop(&out->cs, noops[i]);
	changeset_show_full(&out->cs, NULL);
	rc = 0;
out:
	for (size_t i = 0; i < nnoops; i++) free(noops[i]);
	free(noops);
	if (rc != 0) {
		changeset_free(&out->cs);
		world_free(&out->world);
		memset(out, 0, sizeof(*out));
	}
	return rc;
}
// Every installed app/runtime whose metadata names ref_id as a dependency,
// recursing into new dependents and deduplicating against 'seen', so
// --rdepends can drop the whole subtree in one solve.
static void uninstall_collect_reverse_deps(const char* ref_id,
					   flatpkg_dep_array_t* world,
					   flatpkg_dep_array_t* extras) {
	assert(ref_id != NULL);
	assert(world != NULL);
	assert(extras != NULL);

	store_list_entry_t** entries = NULL;
	size_t n = 0;
	if (store_list_all(&entries, &n) != 0) return;

	for (size_t i = 0; i < n; i++) {
		const char* dep_id = entries[i]->id;
		const char* kind = entries[i]->kind;
		if (!dep_id || !*dep_id || !kind || !*kind) continue;
		// Skip the ref we are removing: it cannot be its own reverse dep.
		if (strcmp(dep_id, ref_id) == 0) continue;
		// Deduplicate: the same package may be reached through multiple
		// dependency chains; only record it once.
		bool already = false;
		for (size_t e = 0; e < extras->count; e++) {
			if (strcmp(extras->items[e].name, dep_id) == 0) {
				already = true;
				break;
			}
		}
		if (already) continue;

		// store_list_all returns bare ids, so parse the path from the store
		// layout instead of sscanf'ing the name.
		char active_path[PATH_MAX];
		if (find_active_dir(store_root(), kind, dep_id, active_path,
				    sizeof(active_path)) != 0)
			continue;

		char mpath[PATH_MAX];
		snprintf(mpath, sizeof(mpath), "%s/metadata", active_path);
		fp_metadata_t meta;
		memset(&meta, 0, sizeof(meta));
		if (fp_metadata_parse_any(mpath, &meta) != 0) continue;

		bool depends = false;
		if (meta.runtime_ref && *meta.runtime_ref) {
			fpkg_ref_t rt;
			memset(&rt, 0, sizeof(rt));
			if (ref_parse(meta.runtime_ref, &rt) == 0 &&
			    strcmp(rt.id, ref_id) == 0)
				depends = true;
		}
		if (!depends) {
			for (size_t g = 0; g < meta.next_groups; g++) {
				const fp_ext_group_t* grp = &meta.ext_groups[g];
				if (!grp->id || !*grp->id) continue;
				if (strcmp(grp->id, ref_id) == 0) {
					depends = true;
					break;
				}
			}
		}
		fp_metadata_free(&meta);

		if (depends) {
			flatpkg_dep_t dep = {0};
			dep.name = xstrdup(dep_id);
			if (dep.name) {
				if (extras->count == extras->cap) {
					size_t ncap = extras->cap
							      ? extras->cap * 2
							      : 8;
					flatpkg_dep_t* ni =
						realloc(extras->items,
							ncap * sizeof(*ni));
					if (!ni) {
						free(dep.name);
						continue;
					}
					extras->items = ni;
					extras->cap = ncap;
				}
				extras->items[extras->count++] = dep;
			}
			// Recurse: the dependent might itself have dependents.
			uninstall_collect_reverse_deps(dep_id, world, extras);
		}
	}

	store_list_all_free(entries, n);
}
static int update_one(const char* type, const char* branch_dir) {
	assert(type != NULL);
	assert(branch_dir != NULL);
	fpkg_ref_t r;
	memset(&r, 0, sizeof(r));
	r.arch = xstrdup(host_arch());

	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s", branch_dir);
	char* p = strrchr(tmp, '/');
	if (!p) {
		ref_free(&r);
		return -1;
	}
	r.branch = xstrdup(p + 1);
	*p = '\0';
	p = strrchr(tmp, '/');
	if (!p) {
		ref_free(&r);
		return -1;
	}
	free(r.arch);
	r.arch = xstrdup(p + 1);
	*p = '\0';
	p = strrchr(tmp, '/');
	if (!p) {
		ref_free(&r);
		return -1;
	}
	r.id = xstrdup(p + 1);

	char origin[PATH_MAX];
	snprintf(origin, sizeof(origin), "%s/.origin", branch_dir);
	char* remote = slurp_file(origin);
	if (!remote || !*remote) {
		free(remote);
		remote = first_remote();
	}
	if (!remote) {
		fpkg_errorf("no remote for %s", r.id);
		ref_free(&r);
		return -1;
	}
	trim(remote);

	char refstr[512];
	ref_full(&r, type, refstr, sizeof(refstr));
	if (tool_pull(remote, refstr) != 0) {
		free(remote);
		ref_free(&r);
		return -1;
	}

	tool_ctx_t tc;
	tool_ctx_init(&tc);
	char commit[128];
	if (tool_rev_parse(&tc, refstr, commit, sizeof(commit)) != 0) {
		free(remote);
		ref_free(&r);
		return -1;
	}

	char active[PATH_MAX];
	snprintf(active, sizeof(active), "%s/active", branch_dir);
	char cur[256] = "";
	{
		char lbuf[PATH_MAX];
		ssize_t n = readlink(active, lbuf, sizeof(lbuf) - 1);
		if (n > 0) {
			lbuf[n] = '\0';
			snprintf(cur, sizeof(cur), "%s", lbuf);
		}
	}
	if (strcmp(cur, commit) == 0) {
		fpkg_infof("%s is already up to date (%s)", r.id, commit);
		free(remote);
		ref_free(&r);
		return 0;
	}

	char deploy[PATH_MAX];
	if (deploy_checkout(&tc, type, &r, commit, deploy, sizeof(deploy)) !=
	    0) {
		free(remote);
		ref_free(&r);
		return -1;
	}
	if (make_active(deploy) != 0) {
		free(remote);
		ref_free(&r);
		return -1;
	}
	write_origin(deploy, remote);

	{
		commit_dir_t* entries = NULL;
		int nentries = collect_commit_dirs(branch_dir, false, &entries);
		if (nentries < 0) {
			fpkg_warnf("cannot scan %s: %s", branch_dir,
				   strerror(errno));
			nentries = 0;
		}
		{
			size_t keep = FLATPKG_KEEP_DEPLOYS;
			if (keep > (size_t)nentries) keep = (size_t)nentries;
			for (size_t i = 0; i + keep < (size_t)nentries; i++) {
				if (strcmp(entries[i].name, commit) == 0)
					continue;
				if (strcmp(entries[i].name, cur) == 0) continue;
				char old[PATH_MAX];
				snprintf(old, sizeof(old), "%s/%s", branch_dir,
					 entries[i].name);
				rm_rf(old);
			}
			free(entries);
		}
	}
	char repo_arg[PATH_MAX + 32];
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);
	char* prune_argv[] = {"ostree", "prune", "--refs-only", repo_arg, NULL};
	tool_res_t pr;
	{
		fp_spin_t sp = fp_spin_init(FPKG_MODULE, "",
					    "pruning unreferenced objects");
		if (ostree_run_cb(prune_argv, &pr, NULL, NULL, spinner_tick_cb,
				  &sp) == 0) {
			fp_spin_finishf(&sp, pr.rc == 0 ? 1 : 0,
					pr.rc == 0
						? "pruned unreferenced objects"
						: "prune failed");
			if (pr.rc != 0) log_tool_stderr(&pr);
		} else {
			fp_spin_finishf(&sp, 0, "prune could not start");
		}
	}
	tool_res_free(&pr);

	fpkg_okf("updated %s to %s", r.id, commit);
	free(remote);
	ref_free(&r);
	return 0;
}
// Update one installed ref to the latest commit its remote offers: the
// executor a confirmed plan row reaches.
int update_deploy(const char* ref) {
	assert(ref != NULL);
	fpkg_ref_t r;
	if (ref_parse(ref, &r) != 0) {
		fpkg_errorf("invalid ref '%s'", ref);
		return 1;
	}
	const char* type = NULL;
	if (deploy_exists("app", r.id)) type = "app";
	else if (deploy_exists("runtime", r.id))
		type = "runtime";
	if (!type) {
		fpkg_errorf("'%s' is not installed", r.id);
		ref_free(&r);
		return 1;
	}
	char branch_dir[PATH_MAX];
	if (find_deploy_dir(type, &r, branch_dir, sizeof(branch_dir)) != 0) {
		fpkg_errorf("no deploy found for '%s'", r.id);
		ref_free(&r);
		return 1;
	}
	ref_free(&r);
	return update_one(type, branch_dir) == 0 ? 0 : 1;
}
