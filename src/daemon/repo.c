#include "flatpkg.h"
#define FPKG_MODULE "repo"
#include "fpkglog.h"
#include "repo.h"

#include <gio/gio.h>
#include <ostree.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>

const char* fp_repo_version(void) { return OSTREE_VERSION_S; }

static void set_error(char** out_error, const char* what, const GError* err) {
	if (out_error == NULL) return;
	if (err != NULL)
		*out_error = xasprintf("%s: %s", what, err->message);
	else
		*out_error = xstrdup(what);
}

static bool ref_matches(const char* ref, const char* want, bool runtime) {
	const char* type_end = strchr(ref, '/');
	const char* id, *id_end, *arch, *arch_end, *branch;
	const char* w;
	bool is_rt, w_rt;
	size_t n;

	if (type_end == NULL) return false;
	is_rt = (type_end - ref == 7 && strncmp(ref, "runtime", 7) == 0);
	if (!is_rt && (type_end - ref != 3 || strncmp(ref, "app", 3) != 0))
		return false;
	id = type_end + 1;
	id_end = strchr(id, '/');
	if (id_end == NULL) return false;
	arch = id_end + 1;
	arch_end = strchr(arch, '/');
	if (arch_end == NULL) return false;
	branch = arch_end + 1;
	if (*branch == '\0' || strchr(branch, '/') != NULL) return false;

	w = want;
	w_rt = runtime;
	if (strncmp(w, "app/", 4) == 0) {
		w_rt = false;
		w += 4;
	} else if (strncmp(w, "runtime/", 8) == 0) {
		w_rt = true;
		w += 8;
	}
	if (is_rt != w_rt) return false;
	const char* w_slash = strchr(w, '/');
	n = w_slash != NULL ? (size_t)(w_slash - w) : strlen(w);
	if (n == 0 || n != (size_t)(id_end - id) || strncmp(id, w, n) != 0)
		return false;
	if (w_slash == NULL) return true;
	const char* w_arch = w_slash + 1;
	const char* w_arch_end = strchr(w_arch, '/');
	n = w_arch_end != NULL ? (size_t)(w_arch_end - w_arch) : strlen(w_arch);
	if (n == 0) return false;
	if (n != (size_t)(arch_end - arch) || strncmp(arch, w_arch, n) != 0)
		return false;
	if (w_arch_end == NULL) return true;
	return strcmp(branch, w_arch_end + 1) == 0;
}

// Does the commit carry a detached signature the configured remote's keyring
// accepts?  A bundle is untrusted input, so nothing may be written into a
// remote's refs before this answers yes.
int fp_repo_verify_commit(const char* checksum, const char* remote,
			  char** out_error) {
	GError* err = NULL;
	OstreeRepo* repo = NULL;
	g_autoptr(OstreeGpgVerifyResult) result = NULL;
	g_autofree char* repo_path = NULL;
	int rc = -1;

	*out_error = NULL;
	repo_path = g_strdup_printf("%s/repo", store_root());
	repo = ostree_repo_new(g_file_new_for_path(repo_path));
	if (!ostree_repo_open(repo, NULL, &err)) {
		*out_error = g_strdup(err != NULL ? err->message : "cannot open");
		g_clear_error(&err);
		goto out;
	}
	// Verify against the keyring of the remote the bundle claims to come
	// from.  The repo-wide keyring is not a substitute: it holds every
	// remote's keys, so one remote's signature would vouch for another's.
	if (remote != NULL && *remote != '\0')
		result = ostree_repo_verify_commit_for_remote(repo, checksum,
							      remote, NULL,
							      &err);
	else
		result = ostree_repo_verify_commit_ext(repo, checksum, NULL,
						       NULL, NULL, &err);
	if (result == NULL) {
		*out_error = g_strdup(err != NULL ? err->message
						  : "no valid signature");
		g_clear_error(&err);
		goto out;
	}
	rc = 0;
out:
	g_object_unref(repo);
	return rc;
}

int fp_repo_local_ref_info(const char* store, const char* remote,
			   const char* ref, const char* commit, bool runtime,
			   char out_ref[512], char out_commit[65],
			   char** out_metadata, char** out_error) {
	GError* err = NULL;
	OstreeRepo* repo = NULL;
	OstreeRepo* local = NULL;
	GHashTable* refs = NULL;
	g_autofree char* repo_path = NULL;
	g_autofree char* url = NULL;
	g_autofree char* resolved = NULL;
	g_autoptr(GFile) root = NULL;
	g_autoptr(GFile) metadata_file = NULL;
	g_autofree char* data = NULL;
	gsize len = 0;
	const char* path;
	const char* match = NULL;
	int rc = -1;

	if (out_metadata != NULL) *out_metadata = NULL;
	if (out_error != NULL) *out_error = NULL;
	out_ref[0] = '\0';
	out_commit[0] = '\0';
	if (store == NULL || remote == NULL || ref == NULL || *store == '\0' ||
	    *remote == '\0' || *ref == '\0') {
		if (out_error != NULL)
			*out_error = xstrdup("local ref lookup: missing name");
		return -1;
	}
	repo_path = g_strdup_printf("%s/repo", store);
	repo = ostree_repo_new(g_file_new_for_path(repo_path));
	if (!ostree_repo_open(repo, NULL, &err)) {
		set_error(out_error, "cannot open the store repo", err);
		goto out;
	}
	if (!ostree_repo_remote_get_url(repo, remote, &url, &err)) {
		set_error(out_error, "cannot read the remote URL", err);
		goto out;
	}
	if (g_str_has_prefix(url, "file://"))
		path = url + strlen("file://");
	else if (url[0] == '/')
		path = url;
	else {
		if (out_error != NULL)
			*out_error = xasprintf("remote '%s' is not local", remote);
		goto out;
	}
	local = ostree_repo_new(g_file_new_for_path(path));
	if (!ostree_repo_open(local, NULL, &err)) {
		set_error(out_error, "cannot open the remote repo", err);
		goto out;
	}
	if (!ostree_repo_list_refs(local, NULL, &refs, NULL, &err)) {
		set_error(out_error, "cannot list the remote refs", err);
		goto out;
	}
	GHashTableIter it;
	gpointer k, v;
	g_hash_table_iter_init(&it, refs);
	while (g_hash_table_iter_next(&it, &k, &v))
		if (ref_matches((const char*)k, ref, runtime)) {
			match = k;
			break;
		}
	if (match == NULL) {
		if (out_error != NULL)
			*out_error = xasprintf("nothing matches %s in remote %s",
					       ref, remote);
		goto out;
	}
	g_strlcpy(out_ref, match, 512);
	if (commit != NULL && *commit != '\0') {
		g_strlcpy(out_commit, commit, 65);
	} else {
		if (!ostree_repo_resolve_rev(local, out_ref, FALSE, &resolved,
					     &err)) {
			set_error(out_error, "cannot resolve the ref", err);
			goto out;
		}
		g_strlcpy(out_commit, resolved, 65);
	}
	if (!ostree_repo_read_commit(local, out_commit, &root, NULL, NULL,
				     &err)) {
		set_error(out_error, "cannot read the commit", err);
		goto out;
	}
	metadata_file = g_file_get_child(root, "metadata");
	if (!g_file_load_contents(metadata_file, NULL, &data, &len, NULL,
				   &err)) {
		set_error(out_error, "cannot read the metadata", err);
		goto out;
	}
	if (out_metadata != NULL) *out_metadata = g_strdup(data);
	rc = 0;
out:
	if (refs != NULL) g_hash_table_unref(refs);
	if (err != NULL) g_error_free(err);
	if (local != NULL) g_object_unref(local);
	if (repo != NULL) g_object_unref(repo);
	return rc;
}
