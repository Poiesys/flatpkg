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

int fp_repo_local_ref_info(const char* store, const char* remote,
			   const char* ref, char out_commit[65],
			   char** out_metadata, char** out_error) {
	GError* err = NULL;
	OstreeRepo* repo = NULL;
	OstreeRepo* local = NULL;
	g_autofree char* repo_path = NULL;
	g_autofree char* url = NULL;
	g_autofree char* commit = NULL;
	g_autoptr(GFile) root = NULL;
	g_autoptr(GFile) metadata_file = NULL;
	g_autofree char* data = NULL;
	gsize len = 0;
	const char* path;
	int rc = -1;

	if (out_metadata != NULL) *out_metadata = NULL;
	if (out_error != NULL) *out_error = NULL;
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
	if (!ostree_repo_resolve_rev(local, ref, FALSE, &commit, &err)) {
		set_error(out_error, "cannot resolve the ref", err);
		goto out;
	}
	if (!ostree_repo_read_commit(local, commit, &root, NULL, NULL, &err)) {
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
	snprintf(out_commit, 65, "%s", commit);
	rc = 0;
out:
	if (err != NULL) g_error_free(err);
	if (local != NULL) g_object_unref(local);
	if (repo != NULL) g_object_unref(repo);
	return rc;
}
