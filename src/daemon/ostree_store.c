#include "../ostree_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int uninstall_target_type(const char* id, const char** type_out) {
	assert(id != NULL);
	assert(type_out != NULL);
	if (deploy_exists("app", id)) *type_out = "app";
	else if (deploy_exists("runtime", id))
		*type_out = "runtime";
	else if (deploy_exists("extension", id))
		*type_out = "extension";
	else
		*type_out = NULL;
	return *type_out != NULL ? 0 : -1;
}

static void uninstall_deploy(const char* deploy_type, const char* ref_kind,
			     const char* id) {
	assert(deploy_type != NULL);
	assert(ref_kind != NULL);
	fpkg_ref_t r;
	memset(&r, 0, sizeof(r));
	r.id = (char*)id;
	char branch_dir[PATH_MAX];
	char* remote = NULL;
	if (find_deploy_dir(deploy_type, &r, branch_dir, sizeof(branch_dir)) ==
	    0) {
		char origin[PATH_MAX];
		snprintf(origin, sizeof(origin), "%s/.origin", branch_dir);
		remote = slurp_file(origin);
		if (remote) trim(remote);
	}
	if (!remote) remote = xstrdup("");

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s/%s", store_root(), deploy_type, id);
	// The grammar above should make this impossible; prove it before a
	// recursive delete, so a looser parser cannot reach outside the store.
	if (!store_path_contained(path)) {
		fpkg_errorf("refusing to remove %s: not under the store", path);
		return;
	}
	// Mark the deploy as being removed BEFORE deleting it
	{
		char marker[PATH_MAX];
		snprintf(marker, sizeof(marker), "%s/.removed", path);
		(void)write_file(marker, "", 0644);
	}
	{
		char label[288], done[288];
		snprintf(label, sizeof(label), "uninstalling %s %s",
			 deploy_type, id);
		snprintf(done, sizeof(done), "uninstalled %s %s", deploy_type,
			 id);
		if (store_remove_tree(path, label, done) != 0)
			fpkg_warnf("remove %s: %s", path, strerror(errno));
	}

	{
		char repo_dir[PATH_MAX];
		snprintf(repo_dir, sizeof(repo_dir), "%s/repo", store_root());
		struct stat rst;
		if (stat(repo_dir, &rst) == 0 && S_ISDIR(rst.st_mode)) {
			tool_ctx_t tc;
			tool_ctx_init(&tc);
			char repo_arg[PATH_MAX + 32];
			snprintf(repo_arg, sizeof(repo_arg), "--repo=%s",
				 tc.repo);

			char* prefixes[4];
			size_t npre = 0;
			{
				char bare[512];
				snprintf(bare, sizeof(bare), "%s/%s", ref_kind,
					 id);
				prefixes[npre++] = xstrdup(bare);
				char dep[512];
				snprintf(dep, sizeof(dep), "deploy/%s/%s",
					 ref_kind, id);
				prefixes[npre++] = xstrdup(dep);
				if (remote && *remote) {
					char withremote[640];
					snprintf(withremote, sizeof(withremote),
						 "%s:%s/%s", remote, ref_kind,
						 id);
					prefixes[npre++] = xstrdup(withremote);
				}
			}
			for (size_t i = 0; i < npre; i++) {
				char* argv[] = {"ostree",   "refs",
						"--delete", prefixes[i],
						repo_arg,   NULL};
				tool_res_t res;
				if (ostree_run(argv, &res) == 0 && res.rc != 0)
					log_tool_stderr(&res);
				tool_res_free(&res);
				free(prefixes[i]);
			}
		}
	}
	free(remote);
}
static void sweep_deploy_refs(void) {
	tool_ctx_t tc;
	tool_ctx_init(&tc);
	char repo_arg[PATH_MAX + 32];
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);

	char* list_argv[] = {"ostree", "refs", "--list", repo_arg, NULL};
	tool_res_t res;
	if (ostree_run(list_argv, &res) != 0 || res.rc != 0 || !res.out) {
		tool_res_free(&res);
		return;
	}
	char* save = NULL;
	for (char* line = strtok_r(res.out, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		char* ref = xstrdup(line);
		char* save2 = NULL;
		char* t0 = strtok_r(line, "/", &save2);
		char* t1 = t0 ? strtok_r(NULL, "/", &save2) : NULL;
		char* id = t1 ? strtok_r(NULL, "/", &save2) : NULL;
		if (!id || strcmp(t0, "deploy") != 0 ||
		    deploy_exists("app", id) || deploy_exists("runtime", id) ||
		    deploy_exists("extension", id)) {
			free(ref);
			continue;
		}
		fpkg_infof("dropping stale deploy ref %s", ref);
		char* del_argv[] = {"ostree", "refs",	"--delete",
				    ref,      repo_arg, NULL};
		tool_res_t dres;
		if (ostree_run(del_argv, &dres) == 0 && dres.rc != 0)
			log_tool_stderr(&dres);
		tool_res_free(&dres);
		free(ref);
	}
	tool_res_free(&res);
}
static void prune_repo(void) {
	{
		char repo_dir[PATH_MAX];
		snprintf(repo_dir, sizeof(repo_dir), "%s/repo", store_root());
		struct stat rst;
		if (stat(repo_dir, &rst) != 0 || !S_ISDIR(rst.st_mode)) return;
	}
	sweep_deploy_refs();
	pid_t pid = fork();
	if (pid != 0) return;
	setsid();
	int nul = open("/dev/null", O_RDWR);
	if (nul >= 0) {
		dup2(nul, 0);
		dup2(nul, 1);
		dup2(nul, 2);
		if (nul > 2) close(nul);
	}
	for (int fd = 3; fd < 256; fd++) close(fd);

	tool_ctx_t tc;
	tool_ctx_init(&tc);
	char repo_arg[PATH_MAX + 32];
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);
	char* prune_argv[] = {"ostree",	   "prune",  "--refs-only",
			      "--depth=0", repo_arg, NULL};
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
	_exit(0);
}

int remove_deploy_ref(const char* ref) {
	assert(ref != NULL);
	fpkg_ref_t r;
	if (ref_parse(ref, &r) != 0) {
		fpkg_errorf("invalid ref '%s'", ref);
		return 1;
	}

	const char* type = NULL;
	if (uninstall_target_type(r.id, &type) != 0) {
		// Removing a deploy takes every branch of it with it, so a later row
		// of the same plan can find it gone: that is not news.
		fpkg_debugf("%s is already gone", ref);
		ref_free(&r);
		return 0;
	}

	if (store_deploy_in_use(type, r.id)) {
		fpkg_errorf("%s %s is in use by a running app; stop it "
			    "first (flatpkg ps)",
			    type, r.id);
		ref_free(&r);
		return 1;
	}

	// Extensions are store-only: they have no world entry.
	if (strcmp(type, "extension") == 0) {
		uninstall_deploy("extension", "runtime", r.id);
		prune_repo();
		ref_free(&r);
		return 0;
	}

	{
		char logged[512];
		char branch_dir[PATH_MAX];
		const char* store = store_root();
		size_t slen = strlen(store);
		logged[0] = '\0';
		if (find_deploy_dir(type, &r, branch_dir, sizeof(branch_dir)) ==
			    0 &&
		    strncmp(branch_dir, store, slen) == 0 &&
		    branch_dir[slen] == '/')
			snprintf(logged, sizeof(logged), "%s",
				 branch_dir + slen + 1);
		if (logged[0] == '\0')
			snprintf(logged, sizeof(logged), "%s/%s", type, r.id);
		uninstall_deploy(type, type, r.id);
		history_append("remove", logged, NULL, NULL);
	}
	// The plan that got us here already lists the orphans this removal
	// leaves behind, so sweeping them again would remove them twice.
	prune_repo();
	if (strcmp(type, "app") == 0) (void)store_update_exports();
	ref_free(&r);
	return 0;
}

void write_deploy_data(const char* branch_dir, const char* commit,
			      const char* origin, char** subpaths,
			      size_t nsubpaths) {
	char path[PATH_MAX], meta_path[PATH_MAX];
	const char* empty[] = {NULL};
	GVariantBuilder meta;
	GVariant* v;
	gchar* data = NULL;
	gsize len = 0;
	GKeyFile* kf;

	snprintf(path, sizeof(path), "%s/%s/deploy", branch_dir, commit);
	snprintf(meta_path, sizeof(meta_path), "%s/%s/metadata", branch_dir,
		 commit);

	g_variant_builder_init(&meta, G_VARIANT_TYPE("a{sv}"));
	// 4 = FLATPAK_DEPLOY_VERSION_CURRENT.
	g_variant_builder_add(&meta, "{sv}", "deploy-version",
			      g_variant_new_int32(4));
	g_variant_builder_add(&meta, "{sv}", "timestamp",
			      g_variant_new_uint64((guint64)time(NULL)));

	kf = g_key_file_new();
	if (g_key_file_load_from_file(kf, meta_path, G_KEY_FILE_NONE, NULL)) {
		gchar* rt = g_key_file_get_string(kf, "Application", "runtime",
						  NULL);

		if (rt != NULL && *rt != '\0')
			g_variant_builder_add(&meta, "{sv}", "runtime",
					      g_variant_new_string(rt));
		g_free(rt);
	}
	g_key_file_free(kf);

	v = g_variant_ref_sink(g_variant_new(
		"(ss^ast@a{sv})", origin != NULL ? origin : "", commit,
		nsubpaths > 0 && subpaths != NULL ? (const char* const*)subpaths
						  : empty,
		(guint64)0, g_variant_builder_end(&meta)));
	data = (gchar*)g_variant_get_data(v);
	len = g_variant_get_size(v);
	GError* werr = NULL;

	if (g_file_set_contents(path, data, (gssize)len, &werr)) {
		chmod(path, 0644);
	} else {
		fpkg_warnf("cannot write the deploy data %s: %s", path,
			   werr != NULL ? werr->message : "unknown");
		g_clear_error(&werr);
	}
	g_variant_unref(v);
}
