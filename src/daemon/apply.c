#include "flatpkg.h"
#include "../ostree_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

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

static void prefetch_installs(const flatpkg_changeset_t* cs, const char* remote,
			      flatpkg_txn_t* txn) {
	assert(cs != NULL);
	assert(remote != NULL);
	pull_job_t* jobs = calloc(cs->count ? cs->count : 1, sizeof(*jobs));
	if (!jobs) return;
	size_t n = 0;
	for (size_t i = 0; i < cs->count; i++) {
		const flatpkg_change_t* c = &cs->items[i];
		if (c->type != FP_CHANGE_INSTALL) continue;
		char ctype[64], cid[256];
		if (sscanf(ref_body(c->ref), "%63[^/]/%255s", ctype, cid) != 2)
			continue;
		fpkg_ref_t ir;
		if (ref_parse(c->ref, &ir) != 0) continue;
		if (!ir.arch) ir.arch = xstrdup(host_arch());
		if (!ir.branch) ir.branch = xstrdup("stable");
		char refstr[512];
		ref_full(&ir, strcmp(ctype, "app") == 0 ? "app" : "runtime",
			 refstr, sizeof(refstr));
		char* rem = ir.remote ? xstrdup(ir.remote)
				      : remote_for_ref(remote, c->ref);
		pull_job_init(&jobs[n++], rem != NULL ? rem : remote, refstr);
		free(rem);
		ref_free(&ir);
	}
	if (n > 1 || (txn != NULL && n > 0)) {
		(void)parallel_pull(jobs, n, txn);
		for (size_t i = 0; i < n; i++)
			if (jobs[i].rc == 0) prefetched_add(jobs[i].refstr);
	} else if (n == 0 && txn != NULL) {
		flatpkg_txn_pull_begin(txn, 0);
		flatpkg_txn_pull_end(txn);
	}
	pull_jobs_free(jobs, n);
	free(jobs);
}
// True when the plan already carries extension install rows: the
// executor must then skip the post-install sweep, which would pull
// everything a second time.
static bool cs_ext_install_planned(const flatpkg_changeset_t* cs,
				   const fpkg_ref_t* rt) {
	assert(cs != NULL);
	for (size_t i = 0; i < cs->count; i++)
		if (cs->items[i].type == FP_CHANGE_INSTALL &&
		    cs->items[i].ref &&
		    strncmp(cs->items[i].ref, "extension/", 10) == 0)
			return true;
	return false;
}
// Apply a confirmed changeset: pull in parallel, then one transaction over
// every row.  The install plan and the solver's commit both come through
// here, so an install and an upgrade reach the store the same way.
int changeset_apply(flatpkg_changeset_t* cs, const char* remote) {
	assert(cs != NULL);

	bool installs = false;
	size_t nwork = 0;
	for (size_t i = 0; i < cs->count; i++) {
		if (cs->items[i].type == FP_CHANGE_INSTALL) installs = true;
		if (cs->items[i].type != FP_CHANGE_KEEP &&
		    cs->items[i].type != FP_CHANGE_NOOP)
			nwork++;
	}
	char* fallback = (remote == NULL && installs) ? first_remote() : NULL;
	const char* use_remote = remote != NULL ? remote : fallback;

	flatpkg_txn_t txn;
	flatpkg_txn_begin(&txn, nwork);
	if (!txn.nested && use_remote != NULL)
		prefetch_installs(cs, use_remote, &txn);

	int rc = 0;
	fpkg_span_t phase_apply = fpkg_span_begin("apply");
	for (size_t i = 0; i < cs->count; i++) {
		if (fpkg_cancel_requested()) {
			fpkg_warnf("cancelled");
			rc = -1;
			break;
		}
		flatpkg_change_t* c = &cs->items[i];
		if (c->type == FP_CHANGE_KEEP || c->type == FP_CHANGE_NOOP)
			continue;
		const char* verb = "installing";
		if (c->type == FP_CHANGE_UPGRADE) verb = "upgrading";
		else if (c->type == FP_CHANGE_DOWNGRADE)
			verb = "downgrading";
		else if (c->type == FP_CHANGE_REMOVE)
			verb = "removing";
		else if (c->type == FP_CHANGE_REINSTALL)
			verb = "reinstalling";
		flatpkg_txn_op(&txn, verb, c->ref);

		if (c->type == FP_CHANGE_REMOVE) {
			if (remove_deploy_ref(c->ref) != 0) {
				fpkg_errorf("remove %s failed", c->ref);
				rc = -1;
			}
			continue;
		}
		if (c->type == FP_CHANGE_UPGRADE ||
		    c->type == FP_CHANGE_DOWNGRADE ||
		    c->type == FP_CHANGE_REINSTALL) {
			if (update_deploy(c->ref) != 0) {
				fpkg_errorf("update %s failed", c->ref);
				rc = -1;
			} else {
				const char* action =
					c->type == FP_CHANGE_UPGRADE ? "upgrade"
					: c->type == FP_CHANGE_DOWNGRADE
						? "downgrade"
						: "reinstall";
				history_append(action, c->ref, c->old_version,
					       c->new_version);
			}
			continue;
		}
		char ctype[64], cid[256];
		if (sscanf(ref_body(c->ref), "%63[^/]/%255s", ctype, cid) != 2) {
			fpkg_warnf("cannot parse ref '%s'", c->ref);
			rc = -1;
			continue;
		}
		if (strcmp(ctype, "extension") == 0) {
			// The extension installer takes the remote from the ref, so
			// the resolved one is spelled on when the row has none.
			char eref[768];
			char* erem = NULL;
			if (ref_body(c->ref) == c->ref) {
				erem = remote_for_ref(use_remote, c->ref);
				snprintf(eref, sizeof(eref), "%s:%s",
					 erem != NULL ? erem : "",
					 c->ref);
			} else {
				snprintf(eref, sizeof(eref), "%s", c->ref);
			}
			int erc = ostree_install_extension_ref(eref);
			free(erem);
			if (erc != 0) {
				fpkg_errorf("install %s failed", c->ref);
				rc = -1;
			} else {
				history_append("install", c->ref, NULL,
					       c->new_version);
			}
			continue;
		}
		int runtime_only = strcmp(ctype, "runtime") == 0;
		fpkg_ref_t ir;
		if (ref_parse(c->ref, &ir) != 0) {
			fpkg_warnf("cannot parse ref '%s'", c->ref);
			rc = -1;
			continue;
		}
		if (!ir.arch) {
			free(ir.arch);
			ir.arch = xstrdup(host_arch());
		}
		if (!ir.branch) ir.branch = xstrdup("stable");
		char* iremote =
			ir.remote ? xstrdup(ir.remote)
				  : remote_for_ref(use_remote, c->ref);
		if (iremote == NULL) {
			fpkg_errorf("no remote for %s", c->ref);
			rc = -1;
			ref_free(&ir);
			continue;
		}
		install_one(&ir, runtime_only ? "runtime" : "app",
			    runtime_only ? "runtime" : "app", iremote);
		if (deploy_exists(runtime_only ? "runtime" : "app", ir.id)) {
			fpkg_okf("installed %s %s", ctype, ir.id);
			history_append("install", c->ref, NULL, c->new_version);
			if (runtime_only && !cs_ext_install_planned(cs, &ir)) {
				// Expansion turned the runtime's extensions into rows; fall back to
				// the sweep only when it found none.
				remote_refs_t* rr = remote_refs_cached(
					iremote, "Looking up extensions");
				if (rr && !rr->failed)
					install_extensions(iremote, &ir,
							   rr->refs, rr->n);
				else
					fpkg_warnf(
						"cannot fetch refs for "
						"'%s': extensions of %s were "
						"not installed",
						iremote, ir.id);
			}
		} else {
			fpkg_errorf("install %s failed", c->ref);
			rc = -1;
		}
		free(iremote);
		ref_free(&ir);
	}
	fpkg_span_end(&phase_apply);
	flatpkg_txn_end(&txn, rc);
	prefetched_clear();
	free(fallback);
	return rc == 0 ? 0 : 1;
}
