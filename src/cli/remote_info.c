#include "../ostree_internal.h"

#include <string.h>

// A `key=` line of a commit's metadata, read without the full parser: a
// report needs two of them.
static void meta_line(const char* body, const char* key, char* out,
		      size_t outlen) {
	assert(key != NULL);
	size_t klen = strlen(key);

	out[0] = '\0';
	if (body == NULL) return;
	for (const char* p = body; *p != '\0';) {
		const char* eol = strchr(p, '\n');
		size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);

		if (len > klen && strncmp(p, key, klen) == 0 && p[klen] == '=') {
			snprintf(out, outlen, "%.*s", (int)(len - klen - 1),
				 p + klen + 1);
			return;
		}
		p = eol != NULL ? eol + 1 : p + len;
	}
}

int cmd_remote_info_print(const char* ref, const char* commit,
			  const char* body, bool show_ref, bool show_commit,
			  bool show_metadata, bool show_parent) {
	if (show_ref) fp_outl("%s", ref);
	if (show_commit) {
		if (commit == NULL) {
			fpkg_errorf("no commit for %s", ref);
			return 1;
		}
		fp_outl("%s", commit);
	}
	if (show_metadata) {
		// A remote's commit metadata, printed to a terminal.
		if (body != NULL) {
			fpkg_text_clean((char*)body);
			fp_out_raw(body, strlen(body));
		}
	}
	if (show_ref || show_commit || show_metadata) return 0;

	fpkg_ref_t r;
	char version[128] = "", runtime_id[256] = "";
	if (ref_parse(ref, &r) != 0) {
		fpkg_errorf("the remote returned an unreadable ref '%s'", ref);
		return 1;
	}
	if (body != NULL) {
		meta_line(body, "version", version, sizeof(version));
		meta_line(body, "runtime", runtime_id, sizeof(runtime_id));
	}
	fp_outl("Ref: %s", ref);
	fp_outl("ID: %s", r.id);
	fp_outl("Arch: %s", r.arch != NULL ? r.arch : "-");
	fp_outl("Branch: %s", r.branch != NULL ? r.branch : "-");
	if (version[0] != '\0') fp_outl("Version: %s", version);
	fp_outl("Commit: %s", commit != NULL ? commit : "-");
	if (show_parent) fp_outl("Parent: %s", "-");
	if (runtime_id[0] != '\0') fp_outl("Runtime: %s", runtime_id);
	ref_free(&r);
	return 0;
}

int cmd_remote_info(const char* remote, const char* request, const char* arch,
		    const char* commit, bool runtime, bool show_ref,
		    bool show_commit, bool show_metadata, bool show_parent) {
	if (!remote_name_valid(remote) || !ostree_remote_exists(remote)) {
		fpkg_errorf("no remote named '%s'", remote);
		return 1;
	}

	char req[600], ref[512];
	snprintf(req, sizeof(req), "%s%s%s", request,
		 arch != NULL && *arch != '\0' ? "/" : "",
		 arch != NULL ? arch : "");
	if (remote_info_ref(remote, req, runtime, ref, sizeof(ref)) != 0) {
		fpkg_errorf("nothing matches %s in remote %s", request,
			    remote);
		return 1;
	}

	char* csum = commit != NULL && *commit != '\0'
			     ? xstrdup(commit)
			     : summary_ref_commit(remote, ref);
	char* body = summary_ref_metadata_text(remote, ref);
	int rc = cmd_remote_info_print(ref, csum, body, show_ref, show_commit,
				       show_metadata, show_parent);
	free(body);
	free(csum);
	return rc;
}

