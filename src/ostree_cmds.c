#include "ostree_internal.h"

static void* fetch_hdl;
static FILE* (*fn_fetch_get_url)(const char*, const char*);
static int* fn_fetch_last_err_code;
static char* fn_fetch_last_err_string;
static int libfetch_init(void) {
	if (fetch_hdl) return 0;
	static const char* names[] = {"libfetch.so",
				      "libfetch.so.6",
				      "libfetch.so.7",
				      "libfetch.so.8",
				      "libfetch.so.9",
				      "libfetch.so.10",
				      NULL};
	for (int i = 0; names[i]; i++) {
		fetch_hdl = dlopen(names[i], RTLD_LAZY);
		if (fetch_hdl) break;
	}
	if (!fetch_hdl) {
		fpkg_errorf("cannot dlopen libfetch: %s", dlerror());
		return -1;
	}
	fn_fetch_get_url = (FILE * (*)(const char*, const char*))
		dlsym(fetch_hdl, "fetchGetURL");
	fn_fetch_last_err_code = (int*)dlsym(fetch_hdl, "fetchLastErrCode");
	fn_fetch_last_err_string =
		(char*)dlsym(fetch_hdl, "fetchLastErrString");
	if (!fn_fetch_get_url) {
		fpkg_errorf("libfetch symbol fetchGetURL missing");
		return -1;
	}
	return 0;
}
static char* fetch_url(const char* url, size_t* out_len, fp_spin_t* sp) {
	if (libfetch_init() != 0) return NULL;
	FILE* f = fn_fetch_get_url(url, "");
	if (!f) {
		if (fn_fetch_last_err_string && *fn_fetch_last_err_string)
			fpkg_errorf("fetch %s: %s (err %d)", url,
				    fn_fetch_last_err_string,
				    fn_fetch_last_err_code
					    ? *fn_fetch_last_err_code
					    : -1);
		else
			fpkg_errorf("fetch %s failed", url);
		return NULL;
	}
	char* buf = NULL;
	size_t len = 0, cap = 0;
	char chunk[4096];
	size_t r;
	for (;;) {
		r = fread(chunk, 1, sizeof(chunk), f);
		if (r == 0) break;
		if (len + r + 1 > cap) {
			cap = cap ? cap * 2 : 16384;
			while (len + r + 1 > cap) cap *= 2;
			char* nb = realloc(buf, cap);
			if (!nb) {
				free(buf);
				fclose(f);
				fpkg_errorf("out of memory");
				return NULL;
			}
			buf = nb;
		}
		memcpy(buf + len, chunk, r);
		len += r;
	}
	fclose(f);
	if (!buf) return NULL;
	buf[len] = '\0';
	if (out_len) *out_len = len;
	return buf;
}
// The GPGKey= value is bare base64 and --gpg-import needs armor, so
// re-armor it (already-armored keys pass through).
static int b64_val(int c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}
static unsigned char* b64_decode(const char* in, size_t inlen, size_t* outlen) {
	unsigned char* out = malloc(inlen / 4 * 3 + 4);
	if (!out) {
		fpkg_errorf("out of memory");
		exit(1);
	}
	size_t o = 0;
	int acc = 0, nbits = 0;
	for (size_t i = 0; i < inlen; i++) {
		int v = b64_val((unsigned char)in[i]);
		if (v < 0) continue;
		acc = (acc << 6) | v;
		nbits += 6;
		if (nbits >= 8) {
			nbits -= 8;
			out[o++] = (unsigned char)((acc >> nbits) & 0xff);
		}
	}
	*outlen = o;
	return out;
}
// OpenPGP CRC24 (RFC 4880).
static unsigned int crc24(const unsigned char* data, size_t len) {
	unsigned int crc = 0xB704CE;
	for (size_t i = 0; i < len; i++) {
		crc ^= (unsigned int)data[i] << 16;
		for (int j = 0; j < 8; j++) {
			crc <<= 1;
			if (crc & 0x1000000) crc ^= 0x1864CFB;
		}
	}
	return crc & 0xFFFFFF;
}
static const char b64_alphabet[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char* armor_gpg_key(const char* key) {
	assert(key != NULL);
	if (strncmp(key, "-----BEGIN PGP", 14) == 0) return xstrdup(key);

	size_t klen = strlen(key);
	size_t binlen = 0;
	unsigned char* bin = b64_decode(key, klen, &binlen);
	if (binlen == 0) {
		free(bin);
		return NULL;
	}

	unsigned int crc = crc24(bin, binlen);
	size_t b64len = ((binlen + 2) / 3) * 4;
	char* b64 = malloc(b64len + 1);
	if (!b64) {
		free(bin);
		fpkg_errorf("out of memory");
		exit(1);
	}
	size_t o = 0;
	for (size_t i = 0; i + 2 < binlen || i < binlen; i += 3) {
		unsigned int v = (unsigned int)bin[i] << 16;
		if (i + 1 < binlen) v |= (unsigned int)bin[i + 1] << 8;
		if (i + 2 < binlen) v |= (unsigned int)bin[i + 2];
		b64[o++] = b64_alphabet[(v >> 18) & 63];
		b64[o++] = b64_alphabet[(v >> 12) & 63];
		b64[o++] = (i + 1 < binlen) ? b64_alphabet[(v >> 6) & 63] : '=';
		b64[o++] = (i + 2 < binlen) ? b64_alphabet[v & 63] : '=';
	}
	b64[o] = '\0';
	free(bin);

	char crc_b64[5];
	crc_b64[0] = b64_alphabet[(crc >> 18) & 63];
	crc_b64[1] = b64_alphabet[(crc >> 12) & 63];
	crc_b64[2] = b64_alphabet[(crc >> 6) & 63];
	crc_b64[3] = b64_alphabet[crc & 63];
	crc_b64[4] = '\0';

	// Fold at 64 columns, like gpg.
	size_t outlen = 41 + b64len + b64len / 64 + 8;
	char* out = malloc(outlen);
	if (!out) {
		free(b64);
		fpkg_errorf("out of memory");
		exit(1);
	}
	size_t n = 0;
	n += (size_t)snprintf(out + n, outlen - n,
			      "-----BEGIN PGP PUBLIC KEY BLOCK-----\n\n");
	for (size_t i = 0; i < b64len; i += 64) {
		n += (size_t)snprintf(
			out + n, outlen - n, "%.*s\n",
			(int)((b64len - i < 64) ? b64len - i : 64), b64 + i);
	}
	snprintf(out + n, outlen - n,
		 "=%s\n-----END PGP PUBLIC KEY BLOCK-----\n", crc_b64);
	free(b64);
	return out;
}
static int flatpakrepo_parse(const char* buf, char** url_out, char** key_out,
			     char** branch_out) {
	assert(buf != NULL);
	assert(url_out != NULL);
	assert(key_out != NULL);
	if (url_out) *url_out = NULL;
	if (key_out) *key_out = NULL;
	if (branch_out) *branch_out = NULL;

	char* line = NULL;
	const char* p = buf;
	bool in_repo = false;
	bool in_key = false;

	if (!buf) return -1;
	while (*p) {
		const char* nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		char* next = strndup(p, len);
		if (!next) {
			free(line);
			return -1;
		}
		free(line);
		line = next;
		trim(line);
		if (nl) p = nl + 1;
		else
			p += len;

		if (!*line) continue;

		if (line[0] == '[') {
			in_repo = (strcmp(line, "[Flatpak Repo]") == 0);
			in_key = false;
			continue;
		}
		if (!in_repo) continue;

		if (in_key && line[0] == ' ') {
			char* kp = line;
			while (*kp == ' ') kp++;
			if (key_out && *key_out) {
				size_t old = strlen(*key_out);
				char* new_key =
					realloc(*key_out, old + strlen(kp) + 2);
				if (!new_key) {
					fpkg_errorf("out of memory");
					exit(1);
				}
				*key_out = new_key;
				strlcat(*key_out, "\n", old + strlen(kp) + 2);
				strlcat(*key_out, kp, old + strlen(kp) + 2);
			}
			continue;
		}
		in_key = false;

		char* eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';
		char* val = eq + 1;
		if (strcmp(line, "Url") == 0 && url_out)
			*url_out = xstrdup(val);
		else if (strcmp(line, "DefaultBranch") == 0 && branch_out)
			*branch_out = xstrdup(val);
		else if (strcmp(line, "GPGKey") == 0 && key_out) {
			// Repeated GPGKey= keys (GKeyFile list style).
			if (*key_out) {
				size_t old = strlen(*key_out);
				*key_out = realloc(*key_out,
						   old + strlen(val) + 2);
				if (!*key_out) {
					fpkg_errorf("out of memory");
					exit(1);
				}
				strlcat(*key_out, "\n", old + strlen(val) + 2);
				strlcat(*key_out, val, old + strlen(val) + 2);
			} else {
				*key_out = xstrdup(val);
			}
			in_key = true;
		}
	}
	free(line);
	return 0;
}
int cmd_remote_add(const char* name, const char* url, const char* from) {
	if (!name || !*name) {
		fpkg_errorf("usage: remote-add [--from=URL] NAME [URL]");
		return 1;
	}
	if (!remote_name_valid(name)) {
		fpkg_errorf("invalid remote name '%s' (letters, digits, "
			    "'.', '-' and '_' only)",
			    name);
		return 1;
	}

	char* repo_url = NULL;
	char* gpgkey = NULL;
	char* default_branch = NULL;
	char* repo_buf = NULL;
	int rc = 1;
	if (from) {
		char* buf = NULL;
		if (strncmp(from, "file://", 7) == 0)
			buf = slurp_file(from + 7);
		else if (from[0] == '/')
			buf = slurp_file(from);
		else
			buf = fetch_url(from, NULL, NULL);
		if (!buf) {
			// Say why: a bare rc=1 with no output reads as a hung pull.
			fpkg_errorf("cannot read flatpak repo file %s", from);
			goto fail;
		}
		repo_buf = buf;
		if (flatpakrepo_parse(buf, &repo_url, &gpgkey,
				      &default_branch) != 0) {
			fpkg_errorf("cannot parse flatpak repo file %s", from);
			goto fail;
		}
		if (!repo_url) {
			fpkg_errorf("no Url= in %s", from);
			goto fail;
		}
	} else {
		if (!url || !*url) {
			fpkg_errorf(
				"usage: remote-add [--from=URL] NAME [URL]");
			return 1;
		}
		if (strstr(url, ".flatpakrepo") != NULL) {
			char* buf = NULL;
			if (strncmp(url, "file://", 7) == 0)
				buf = slurp_file(url + 7);
			else if (url[0] == '/')
				buf = slurp_file(url);
			else
				buf = fetch_url(url, NULL, NULL);
			if (!buf) {
				fpkg_errorf("cannot read flatpak repo file "
					    "%s",
					    url);
				goto fail;
			}
			repo_buf = buf;
			if (flatpakrepo_parse(buf, &repo_url, &gpgkey,
					      &default_branch) != 0) {
				fpkg_errorf("cannot parse flatpak repo file %s",
					    url);
				goto fail;
			}
			if (!repo_url) {
				fpkg_errorf("no Url= in %s", url);
				goto fail;
			}
		} else {
			repo_url = xstrdup(url);
		}
	}

	if (ensure_repo() != 0) goto fail;

	// The flatpakrepo GPGKey is bare base64, so re-armor it or
	// --gpg-import will not parse it.
	char key_host[PATH_MAX] = "", key_arg[PATH_MAX] = "";
	if (gpgkey && *gpgkey) {
		char* armored = armor_gpg_key(gpgkey);
		if (!armored) {
			fpkg_errorf("cannot decode the GPGKey from %s",
				    from ? from : "the repo file");
			goto fail;
		}
		store_path(key_host, sizeof(key_host), "keys/%s.gpg", name);
		char keysdir[PATH_MAX];
		snprintf(keysdir, sizeof(keysdir), "%s", key_host);
		char* slash = strrchr(keysdir, '/');
		if (slash) *slash = '\0';
		mkdir_p(keysdir, 0755);
		if (write_file(key_host, armored, 0644) != 0) {
			fpkg_errorf("write %s: %s", key_host, strerror(errno));
			free(armored);
			goto fail;
		}
		free(armored);
	}

	tool_ctx_t tc;
	tool_ctx_init(&tc);
	char repo_arg[PATH_MAX + 32];
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);

	char branch_arg[PATH_MAX + 64];
	char* argv[16];
	int n = 0;
	argv[n++] = "ostree";
	argv[n++] = "remote";
	argv[n++] = "add";
	argv[n++] = "--if-not-exists";
	if (gpgkey && *gpgkey) {
		snprintf(key_arg, sizeof(key_arg),
			 "--gpg-import=%s/keys/%s.gpg", tc.store, name);
		argv[n++] = key_arg;
	} else {
		fpkg_warnf("no GPGKey in the repo file; disabling GPG "
			   "verification for remote '%s'",
			   name);
		argv[n++] = "--no-gpg-verify";
	}
	// flatpak writes both booleans explicitly: a key sets gpg-verify and
	// gpg-verify-summary; --no-gpg-verify clears both (missing = false).
	if (gpgkey && *gpgkey) {
		argv[n++] = "--set=gpg-verify=true";
		argv[n++] = "--set=gpg-verify-summary=true";
	} else {
		argv[n++] = "--set=gpg-verify=false";
		argv[n++] = "--set=gpg-verify-summary=false";
	}
	if (default_branch && *default_branch) {
		snprintf(branch_arg, sizeof(branch_arg),
			 "--set=flatpkg.branch=%s", default_branch);
		argv[n++] = branch_arg;
	}
	argv[n++] = (char*)name;
	argv[n++] = repo_url;
	argv[n++] = repo_arg;
	argv[n] = NULL;

	tool_res_t res;
	if (ostree_run(argv, &res) == 0 && res.rc == 0) {
		fpkg_okf("added remote %s (%s)", name, repo_url);
		if (default_branch && *default_branch)
			fpkg_okf("  default branch: %s", default_branch);
		rc = 0;
	} else {
		log_tool_stderr(&res);
		fpkg_errorf("remote add '%s' failed", name);
	}
	if (res.err && *res.err) fpkg_warnf("ostree stderr: %s", res.err);
	tool_res_free(&res);
	free(repo_buf);
	free(repo_url);
	free(gpgkey);
	free(default_branch);
	return rc;

fail:
	free(repo_buf);
	free(repo_url);
	free(gpgkey);
	free(default_branch);
	return 1;
}
int cmd_remote_list(void) {
	size_t n = 0;
	char** remotes = list_remotes(&n);
	if (n == 0) {
		fpkg_infof("no remotes configured (flatpkg remote-add "
			   "[--from=URL] NAME [URL])");
		free(remotes);
		return 0;
	}
	const char* headers[] = {"Name", "URL", "GPG verify", "GPG summary",
				 "Default branch"};
	enum { REMOTE_ROWS_MAX = 256 };
	char* cells[REMOTE_ROWS_MAX][5];
	const char** rows[REMOTE_ROWS_MAX];
	size_t m = 0;
	bool truncated = false;
	for (size_t i = 0; i < n; i++) {
		if (m == REMOTE_ROWS_MAX) {
			truncated = true;
			break;
		}
		char* url = remote_setting(remotes[i], "url");
		// Report what the store actually says - a missing key reads as false -
		// not a default: that is what makes flatpak refuse the remote's
		// appstream data.
		char* verify = remote_setting(remotes[i], "gpg-verify");
		char* vsum = remote_setting(remotes[i], "gpg-verify-summary");
		char* branch = remote_setting(remotes[i], "flatpkg.branch");
		cells[m][0] = strdup(remotes[i]);
		cells[m][1] = strdup(url ? url : "(none)");
		cells[m][2] = strdup(verify ? verify : "-");
		cells[m][3] = strdup(vsum ? vsum : "-");
		cells[m][4] = strdup(branch ? branch : "-");
		rows[m] = (const char**)cells[m];
		m++;
		free(url);
		free(verify);
		free(vsum);
		free(branch);
		free(remotes[i]);
	}
	free(remotes);
	if (m > 0)
		fp_print_table(headers, 5, (const char* const* const*)rows, m,
			       FP_TABLE_OUTPUT);
	if (truncated)
		fpkg_warnf("more than %d remotes; not all are listed",
			   REMOTE_ROWS_MAX);
	for (size_t i = 0; i < m; i++)
		for (int c = 0; c < 5; c++) free(cells[i][c]);
	return 0;
}
int cmd_remote_delete(const char* name) {
	if (!name || !*name) {
		fpkg_errorf("usage: remote-delete NAME");
		return 1;
	}
	if (!remote_name_valid(name)) {
		fpkg_errorf("invalid remote name '%s' (letters, digits, "
			    "'.', '-' and '_' only)",
			    name);
		return 1;
	}
	if (ensure_repo() != 0) return 1;

	tool_ctx_t tc;
	tool_ctx_init(&tc);
	char repo_arg[PATH_MAX + 32];
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);
	char* argv[] = {"ostree",    "remote", "delete",
			(char*)name, repo_arg, NULL};
	tool_res_t res;
	int rc = 1;
	if (ostree_run(argv, &res) == 0 && res.rc == 0) {
		fpkg_okf("deleted remote %s", name);
		rc = 0;
	} else {
		log_tool_stderr(&res);
		fpkg_errorf("remote delete '%s' failed", name);
	}
	tool_res_free(&res);

	char key_host[PATH_MAX];
	store_path(key_host, sizeof(key_host), "keys/%s.gpg", name);
	unlink(key_host);
	return rc;
}
static int list_commits_one(const char* type, const char* branch_dir) {
	assert(type != NULL);
	assert(branch_dir != NULL);
	char active[PATH_MAX];
	snprintf(active, sizeof(active), "%s/active", branch_dir);
	char cur[256] = "";
	{
		char lbuf[PATH_MAX];
		ssize_t n = readlink(active, lbuf, sizeof(lbuf) - 1);
		if (n > 0) {
			lbuf[n] = '\0';
			char* b = strrchr(lbuf, '/');
			snprintf(cur, sizeof(cur), "%s", b ? b + 1 : lbuf);
		}
	}
	commit_dir_t* entries = NULL;
	int nentries = collect_commit_dirs(branch_dir, true, &entries);
	if (nentries < 0) {
		fpkg_errorf("cannot open %s: %s", branch_dir, strerror(errno));
		return -1;
	}
	for (size_t i = 0; i < (size_t)nentries; i++) {
		char ts[32];
		time_t t = entries[i].mtime;
		struct tm tm;
		localtime_r(&t, &tm);
		strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &tm);
		fpkg_infof("  %c %s  %s  (%s)",
			   strcmp(entries[i].name, cur) == 0 ? '*' : ' ',
			   entries[i].name, ts, i == 0 ? "newest" : "rollback");
	}
	free(entries);
	return 0;
}
int cmd_list_commits(const char* ref) {
	assert(ref != NULL);
	if (!ref) {
		fpkg_errorf("usage: flatpkg list-commits <ref>");
		return 1;
	}
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
	fpkg_infof("commits for %s (active marked *):", branch_dir);
	return list_commits_one(type, branch_dir) == 0 ? 0 : 1;
}
// Re-point <branch_dir>/active to a previous commit dir. Mirrors `flatpak
// update --commit=<hash>`.
int cmd_rollback(const char* ref, const char* commit) {
	assert(ref != NULL);
	assert(commit != NULL);
	if (!ref || !commit) {
		fpkg_errorf("usage: flatpkg rollback <ref> <commit>");
		return 1;
	}
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

	char target[PATH_MAX];
	if (!is_commit_dir_name(commit)) {
		fpkg_errorf("'%s' is not a commit id (expected the hex "
			    "checksum from `flatpkg list-commits %s`)",
			    commit, ref);
		return 1;
	}
	snprintf(target, sizeof(target), "%s/%s", branch_dir, commit);
	struct stat st;
	if (stat(target, &st) != 0 || !S_ISDIR(st.st_mode)) {
		fpkg_errorf("commit '%s' not deployed under %s", commit,
			    branch_dir);
		return 1;
	}
	if (make_active(target) != 0) {
		fpkg_errorf("failed to roll back to %s", commit);
		return 1;
	}
	fpkg_okf("rolled back %s to %s", branch_dir, commit);
	return 0;
}
int cmd_search(const char* text) {
	if (!text || !*text) {
		fpkg_errorf("usage: search TEXT");
		return 1;
	}

	char** refs = NULL;
	size_t nrefs = 0, cap = 0;
#define PUSH_REF(s)                                                            \
	do {                                                                   \
		if (nrefs == cap) {                                            \
			cap = cap ? cap * 2 : 32;                              \
			char** new_refs = realloc(refs, cap * sizeof(char*));  \
			if (!new_refs) {                                       \
				fpkg_errorf("out of memory");                  \
				exit(1);                                       \
			}                                                      \
			refs = new_refs;                                       \
		}                                                              \
		refs[nrefs++] = xstrdup(s);                                    \
	} while (0)

	char repo[PATH_MAX];
	store_path(repo, sizeof(repo), "repo");
	if (access(repo, F_OK) == 0) {
		tool_ctx_t tc;
		tool_ctx_init(&tc);
		char repo_arg[PATH_MAX + 32];
		snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);
		char* argv[] = {"ostree", "refs", repo_arg, NULL};
		tool_res_t res;
		if (ostree_run(argv, &res) == 0 && res.rc == 0 && res.out) {
			char* save = NULL;
			for (char* line = strtok_r(res.out, "\n", &save); line;
			     line = strtok_r(NULL, "\n", &save)) {
				char* colon = strchr(line, ':');
				char* slash = strchr(line, '/');
				if (colon && (!slash || colon < slash))
					line = colon + 1;
				PUSH_REF(line);
			}
		}
		tool_res_free(&res);
	}

	size_t nremotes = 0;
	char** remotes = list_remotes(&nremotes);
	for (size_t i = 0; i < nremotes; i++) {
		size_t nsrefs = 0;
		char** srefs = fetch_remote_refs(remotes[i], &nsrefs, NULL);
		if (!srefs) continue;
		if (nsrefs == 0)
			fpkg_warnf("no refs found for remote '%s'", remotes[i]);
		for (size_t k = 0; k < nsrefs; k++) {
			bool seen = false;
			for (size_t m = 0; m < nrefs; m++) {
				if (strcmp(refs[m], srefs[k]) == 0) {
					seen = true;
					break;
				}
			}
			if (!seen) PUSH_REF(srefs[k]);
			free(srefs[k]);
		}
		free(srefs);
	}
	for (size_t i = 0; i < nremotes; i++) free(remotes[i]);
	free(remotes);

	char needle[256];
	snprintf(needle, sizeof(needle), "%s", text);
	for (size_t i = 0; needle[i]; i++)
		needle[i] = (char)tolower((unsigned char)needle[i]);

	const char* headers[] = {"Kind", "ID", "Arch", "Branch"};
	enum { SEARCH_ROWS_MAX = 512 };
	char* cells[SEARCH_ROWS_MAX][4];
	const char** rows[SEARCH_ROWS_MAX];
	size_t m = 0;
	bool truncated = false;
	for (size_t i = 0; i < nrefs; i++) {
		char low[512];
		snprintf(low, sizeof(low), "%s", refs[i]);
		for (char* c = low; *c; c++)
			*c = (char)tolower((unsigned char)*c);
		if (!strstr(low, needle)) {
			free(refs[i]);
			continue;
		}
		if (m == SEARCH_ROWS_MAX) {
			truncated = true;
			free(refs[i]);
			continue;
		}
		{
			const char* slash = strchr(refs[i], '/');
			size_t klen = slash ? (size_t)(slash - refs[i])
					    : strlen(refs[i]);
			const char* rest = slash ? slash + 1 : "";
			char* id = xstrdup(rest);
			char *arch = (char*)"-", *branch = (char*)"-";
			char* s1 = strchr(id, '/');
			if (s1) {
				*s1 = '\0';
				arch = s1 + 1;
				char* s2 = strchr(arch, '/');
				if (s2) {
					*s2 = '\0';
					branch = s2 + 1;
				}
			}
			char kind[8];
			snprintf(kind, sizeof(kind), "%.*s", (int)klen,
				 refs[i]);
			cells[m][0] = xstrdup(kind);
			cells[m][1] = xstrdup(id);
			cells[m][2] = xstrdup(arch);
			cells[m][3] = xstrdup(branch);
			rows[m] = (const char**)cells[m];
			m++;
			free(id);
		}
		free(refs[i]);
	}
	free(refs);

	if (m == 0) {
		fpkg_errorf("no matches for '%s'", text);
		return 1;
	}
	fp_print_table(headers, 4, (const char* const* const*)rows, m,
		       FP_TABLE_OUTPUT);
	if (truncated)
		fpkg_warnf("more than %d matches; refine the search term",
			   SEARCH_ROWS_MAX);
	for (size_t i = 0; i < m; i++)
		for (int c = 0; c < 4; c++) free(cells[i][c]);
	return 0;
#undef PUSH_REF
}
// The ref name a caller's child repo used for `branch`: ask the repo rather
// than guessing. Returns a malloc'd name or NULL.
static char* child_repo_ref(const char* child_repo, const char* branch) {
	assert(child_repo != NULL);
	assert(branch != NULL);
	tool_res_t res;
	char* argv[5];
	char* found = NULL;

	argv[0] = "ostree";
	argv[1] = "refs";
	argv[2] = NULL;
	char repo_arg[PATH_MAX + 32];
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", child_repo);
	argv[2] = repo_arg;
	argv[3] = NULL;

	tool_res_init(&res);
	if (ostree_run(argv, &res) == 0 && res.rc == 0 && res.out) {
		char* save = NULL;
		for (char* line = strtok_r(res.out, "\n", &save); line;
		     line = strtok_r(NULL, "\n", &save)) {
			size_t ll = strlen(line), bl = strlen(branch);
			if (ll >= bl && strcmp(line + ll - bl, branch) == 0) {
				found = xstrdup(line);
				break;
			}
		}
	}
	tool_res_free(&res);
	return found;
}
static void normalize_symlink_modes(const char* repo) {
	assert(repo != NULL);
	char objdir[PATH_MAX];
	DIR* d;
	struct dirent* e;

	snprintf(objdir, sizeof(objdir), "%s/objects", repo);
	d = opendir(objdir);
	if (d == NULL) return;
	while ((e = readdir(d)) != NULL) {
		char sub[PATH_MAX];
		DIR* d2;
		struct dirent* e2;

		if (e->d_name[0] == '.') continue;
		snprintf(sub, sizeof(sub), "%s/%s", objdir, e->d_name);
		d2 = opendir(sub);
		if (d2 == NULL) continue;
		while ((e2 = readdir(d2)) != NULL) {
			char path[PATH_MAX];
			struct stat st;

			if (e2->d_name[0] == '.') continue;
			if (e2->d_type != DT_UNKNOWN && e2->d_type != DT_LNK)
				continue;
			snprintf(path, sizeof(path), "%s/%s", sub, e2->d_name);
			if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode) &&
			    (st.st_mode & 07777) != 0777)
				(void)fchmodat(AT_FDCWD, path, 0777,
					       AT_SYMLINK_NOFOLLOW);
		}
		closedir(d2);
	}
	closedir(d);
}
static int pull_remote_ref(tool_ctx_t* tc, const char* origin,
			   const char* ref) {
	char repo_arg[PATH_MAX + 32], hdr[600];
	tool_res_t res;
	int failed;

	if (origin == NULL || *origin == '\0') return -1;

	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc->repo);
	snprintf(hdr, sizeof(hdr), "--http-header=Flatpak-Ref=%s", ref);
	char* argv[] = {"ostree",
			"pull",
			(char*)origin,
			(char*)ref,
			"--http-header=User-Agent=flatpak/1.17.6",
			hdr,
			repo_arg,
			NULL};

	tool_res_init(&res);
	failed = (ostree_run(argv, &res) != 0 || res.rc != 0);
	if (failed) log_tool_stderr(&res);
	tool_res_free(&res);
	if (!failed) {
		store_mark_changed();
		fpkg_infof("pulled %s from '%s' (the caller's repo could not "
			   "supply it)",
			   ref, origin);
	}
	return failed ? -1 : 0;
}
static int pull_child_repo(tool_ctx_t* tc, const char* child_repo,
			   const char* origin, const char* commit) {
	char repo_arg[PATH_MAX + 32], url[PATH_MAX + 16];
	tool_res_t res;
	int failed;

	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc->repo);
	snprintf(url, sizeof(url), "--url=file://%s", child_repo);
	char* argv[] = {
		"ostree", "pull",   "--untrusted", "--bareuseronly-files",
		url,	  repo_arg, (char*)origin, (char*)commit,
		NULL};

	tool_res_init(&res);
	failed = (ostree_run(argv, &res) != 0 || res.rc != 0);
	if (failed) {
		log_tool_stderr(&res);
		fpkg_warnf("cannot pull %s from the caller's repo", commit);
	}
	tool_res_free(&res);
	return failed ? -1 : 0;
}
// A caller inside a sandbox sees the store at /var/lib/flatpak (sandbox.c
// binds it there), so libflatpak writes that path into parent=; here it is
// store_root(), which ostree will not resolve: rewrite the recorded parent.
static void child_repo_fix_parent(const char* child_repo) {
	assert(child_repo != NULL);
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/config", child_repo);

	char* src = slurp_file(path);
	if (src == NULL) return;

	const char* p = src;
	bool changed = false;
	char* out = NULL;
	size_t ocap = 0;
	while (*p != '\0') {
		const char* eol = strchr(p, '\n');
		size_t len = eol ? (size_t)(eol - p) : strlen(p);
		char* line = malloc(len + 1);
		if (line == NULL) break;
		memcpy(line, p, len);
		line[len] = '\0';

		char* trimmed = line;
		while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
		if (strncmp(trimmed, "parent=", 7) == 0) {
			char* val = trimmed + 7;
			while (*val == ' ') val++;
			size_t pfx = 0;
			if (strncmp(val, "/var/lib/flatpkg/", 18) == 0)
				pfx = 18; // strlen("/var/lib/flatpkg/")
			else if (strcmp(val, "/var/lib/flatpkg") == 0)
				pfx = 17;
			// Compatibility for metadata written before the
			// canonical path moved; never emitted ourselves.
			else if (strncmp(val, "/var/lib/flatpak/", 17) == 0)
				pfx = 17;
			else if (strcmp(val, "/var/lib/flatpak") == 0)
				pfx = 16;
			if (pfx > 0 && access(val, F_OK) != 0) {
				const char* rest = val + pfx;
				buf_append(&out, &ocap, line,
					   (size_t)(val - line));
				buf_append(&out, &ocap, store_root(),
					   strlen(store_root()));
				buf_append(&out, &ocap, rest, strlen(rest));
				if (eol) buf_append(&out, &ocap, "\n", 1);
				changed = true;
				free(line);
				if (!eol) break;
				p = eol + 1;
				continue;
			}
		}
		buf_append(&out, &ocap, line, len);
		if (eol) buf_append(&out, &ocap, "\n", 1);
		free(line);
		if (!eol) break;
		p = eol + 1;
	}

	if (changed && out != NULL) (void)write_file(path, out, 0644);
	free(src);
	free(out);
}
static int child_repo_import(tool_ctx_t* tc, const char* child_repo,
			     const char* ref, const char* origin,
			     size_t nsubpaths, char* commit, size_t commitlen) {
	char *cref, *argv[8], repo_arg[PATH_MAX + 32], newref[768];
	char create[800];
	tool_res_t res;
	int n, failed;

	child_repo_fix_parent(child_repo);

	if (nsubpaths > 0 && origin != NULL && *origin != '\0' &&
	    pull_remote_ref(tc, origin, ref) == 0) {
		snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc->repo);
		snprintf(newref, sizeof(newref), "%s:%s", origin, ref);
		char* rv[] = {"ostree", "rev-parse", repo_arg, newref, NULL};

		tool_res_init(&res);
		if (ostree_run(rv, &res) != 0 || res.rc != 0 || !res.out ||
		    !*res.out) {
			log_tool_stderr(&res);
			tool_res_free(&res);
			fpkg_errorf("cannot resolve '%s' in the store", newref);
			return -1;
		}
		trim(res.out);
		snprintf(commit, commitlen, "%s", res.out);
		tool_res_free(&res);
		goto have_commit;
	}

	cref = child_repo_ref(child_repo, ref);
	if (cref == NULL) {
		fpkg_errorf("the caller's repo has no '%s' ref", ref);
		return -1;
	}

	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", child_repo);
	{
		char* rv[] = {"ostree", "rev-parse", repo_arg, cref, NULL};

		tool_res_init(&res);
		if (ostree_run(rv, &res) != 0 || res.rc != 0 || !res.out ||
		    !*res.out) {
			log_tool_stderr(&res);
			tool_res_free(&res);
			fpkg_errorf("cannot resolve '%s' in the caller's repo",
				    cref);
			free(cref);
			return -1;
		}
		trim(res.out);
		snprintf(commit, commitlen, "%s", res.out);
		tool_res_free(&res);
	}

	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc->repo);

	n = 0;
	argv[n++] = "ostree";
	argv[n++] = "pull-local";
	argv[n++] = "--untrusted";
	argv[n++] = "--bareuseronly-files";
	argv[n++] = (char*)child_repo;
	argv[n++] = commit;
	argv[n++] = repo_arg;
	argv[n] = NULL;

	normalize_symlink_modes(child_repo);

	tool_res_init(&res);
	failed = (ostree_run(argv, &res) != 0 || res.rc != 0);
	if (failed) log_tool_stderr(&res);
	tool_res_free(&res);
	free(cref);
	if (failed) {
		if (pull_child_repo(tc, child_repo, origin, commit) != 0 &&
		    !pull_remote_ref(tc, origin, ref)) {
			fpkg_errorf("cannot import %s from the caller's repo "
				    "or from '%s'",
				    ref, origin ? origin : "-");
			return -1;
		}
	}

have_commit:
	if (origin != NULL && *origin != '\0')
		snprintf(newref, sizeof(newref), "%s:%s", origin, ref);
	else
		snprintf(newref, sizeof(newref), "%s", ref);
	snprintf(create, sizeof(create), "--create=%s", newref);

	n = 0;
	argv[n++] = "ostree";
	argv[n++] = "refs";
	argv[n++] = create;
	argv[n++] = "--force";
	argv[n++] = repo_arg;
	argv[n++] = commit;
	argv[n] = NULL;

	tool_res_init(&res);
	if (ostree_run(argv, &res) != 0 || res.rc != 0) {
		log_tool_stderr(&res);
		tool_res_free(&res);
		fpkg_errorf("cannot record '%s' in the store", newref);
		return -1;
	}
	tool_res_free(&res);
	return 0;
}
// An arch becomes a path and a gzip argument, so keep it to name characters.
static bool appstream_arch_valid(const char* arch) {
	assert(arch != NULL);
	if (*arch == '\0' || arch[0] == '.' || arch[0] == '-') return false;
	for (const char* p = arch; *p; p++) {
		bool alnum = (*p >= '0' && *p <= '9') ||
			     (*p >= 'a' && *p <= 'z') ||
			     (*p >= 'A' && *p <= 'Z');
		if (!alnum && *p != '_' && *p != '-') return false;
	}
	return true;
}
int ostree_appstream_deploy(const char* remote, const char* arch) {
	assert(remote != NULL);
	assert(arch != NULL);
	// appstream2 first, then the legacy appstream branch.
	static const char* branch_fmts[] = {"appstream2/%s", "appstream/%s"};
	tool_ctx_t tc;
	char repo_arg[PATH_MAX + 32];
	int deployed = 0;

	tool_ctx_init(&tc);
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);

	for (size_t b = 0; b < sizeof(branch_fmts) / sizeof(branch_fmts[0]);
	     b++) {
		char branch[64], hdr[160];
		char* argv[8];
		int n = 0;
		tool_res_t res;

		snprintf(branch, sizeof(branch), branch_fmts[b], arch);
		snprintf(hdr, sizeof(hdr), "--http-header=Flatpak-Ref=%s",
			 branch);
		argv[n++] = "ostree";
		argv[n++] = "pull";
		argv[n++] = "--depth=0";
		argv[n++] = hdr;
		argv[n++] = (char*)remote;
		argv[n++] = branch;
		argv[n++] = repo_arg;
		argv[n] = NULL;

		tool_res_init(&res);
		if (ostree_run(argv, &res) == 0 && res.rc == 0) {
			fpkg_infof("deployed %s from remote '%s'", branch,
				   remote);
			deployed++;
		} else if (res.err && *res.err) {
			char* nl = strchr(res.err, '\n');
			if (nl) *nl = '\0';
			fpkg_infof("no %s to deploy: %s", branch, res.err);
		}
		tool_res_free(&res);
	}

	return deployed > 0 ? 0 : -1;
}
int ostree_appstream_extract(const char* remote, const char* arch) {
	assert(remote != NULL);
	assert(arch != NULL);
	tool_ctx_t tc;
	char repo_arg[PATH_MAX + 32], arch_dir[PATH_MAX], active[PATH_MAX];
	char commit[128] = "";
	bool legacy = false;

	if (!remote || !*remote || !arch || !*arch) return -1;
	if (!remote_name_valid(remote) || !appstream_arch_valid(arch)) {
		fpkg_errorf("invalid appstream remote/arch '%s'/'%s'", remote,
			    arch);
		return -1;
	}

	tool_ctx_init(&tc);
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);
	store_path(arch_dir, sizeof(arch_dir), "appstream/%s/%s", remote, arch);
	snprintf(active, sizeof(active), "%s/active", arch_dir);

	for (int b = 0; b < 2 && commit[0] == '\0'; b++) {
		char refname[160], *argv[6];
		tool_res_t res;

		snprintf(refname, sizeof(refname), "%s:appstream%s/%s", remote,
			 b == 0 ? "2" : "", arch);
		argv[0] = "ostree";
		argv[1] = "rev-parse";
		argv[2] = refname;
		argv[3] = repo_arg;
		argv[4] = NULL;

		tool_res_init(&res);
		if (ostree_run(argv, &res) == 0 && res.rc == 0 && res.out &&
		    res.out[0]) {
			char* nl = strchr(res.out, '\n');
			if (nl) *nl = '\0';
			snprintf(commit, sizeof(commit), "%s", res.out);
			legacy = (b == 1);
		}
		tool_res_free(&res);
	}
	if (commit[0] == '\0') return -1;

	{
		char cur[PATH_MAX], dir[PATH_MAX];
		ssize_t cl = readlink(active, cur, sizeof(cur) - 1);
		if (cl > 0) {
			struct stat st;
			cur[cl] = '\0';
			snprintf(dir, sizeof(dir), "%s/%s", arch_dir, cur);
			if (strcmp(cur, commit) == 0 && stat(dir, &st) == 0 &&
			    S_ISDIR(st.st_mode)) {
				char ts[PATH_MAX];
				snprintf(ts, sizeof(ts), "%s/.timestamp",
					 arch_dir);
				(void)write_file(ts, "", 0644);
				return 0;
			}
		}
	}

	if (mkdir_p(arch_dir, 0755) != 0) return -1;

	// ostree checkout requires the target not to exist.
	char tmpdir[PATH_MAX], final[PATH_MAX];
	snprintf(tmpdir, sizeof(tmpdir), "%s/.%s.%ld", arch_dir, commit,
		 (long)getpid());
	rm_rf(tmpdir);
	snprintf(final, sizeof(final), "%s/%s", arch_dir, commit);

	{
		char* argv[] = {"ostree",
				"checkout",
				"--user-mode",
				"--bareuseronly-dirs",
				"--fsync=none",
				commit,
				tmpdir,
				repo_arg,
				NULL};
		tool_res_t res;
		tool_res_init(&res);
		if (ostree_run(argv, &res) != 0 || res.rc != 0) {
			log_tool_stderr(&res);
			tool_res_free(&res);
			fpkg_errorf("ostree checkout %s failed", commit);
			rm_rf(tmpdir);
			return -1;
		}
		tool_res_free(&res);
	}

	{
		char xml[PATH_MAX], gz[PATH_MAX];
		snprintf(xml, sizeof(xml), "%s/appstream.xml", tmpdir);
		snprintf(gz, sizeof(gz), "%s/appstream.xml.gz", tmpdir);
		if (access(gz, F_OK) != 0 && access(xml, F_OK) == 0) {
			int out = open(gz, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			char* argv[] = {"/usr/bin/gzip", "-c", xml, NULL};
			int rc = out >= 0 ? run_program(argv, -1, out, -1) : -1;
			if (out >= 0) close(out);
			if (rc != 0) fpkg_warnf("could not compress %s", xml);
		}
	}

	if (legacy) {
		char gz[PATH_MAX], xml[PATH_MAX];
		snprintf(gz, sizeof(gz), "%s/appstream.xml.gz", tmpdir);
		snprintf(xml, sizeof(xml), "%s/appstream.xml", tmpdir);
		if (access(xml, F_OK) != 0 && access(gz, F_OK) == 0) {
			int in = open(gz, O_RDONLY);
			int out = open(xml, O_WRONLY | O_CREAT | O_TRUNC, 0644);
			char* argv[] = {"/usr/bin/gzip", "-dc", NULL};
			int rc = (in >= 0 && out >= 0)
					 ? run_program(argv, in, out, -1)
					 : -1;
			if (in >= 0) close(in);
			if (out >= 0) close(out);
			if (rc != 0) fpkg_warnf("could not decompress %s", gz);
		}
	}

	if (rename(tmpdir, final) != 0) {
		struct stat st;
		if (stat(final, &st) == 0 && S_ISDIR(st.st_mode)) {
			rm_rf(tmpdir);
		} else {
			fpkg_errorf("rename %s -> %s: %s", tmpdir, final,
				    strerror(errno));
			rm_rf(tmpdir);
			return -1;
		}
	}

	if (atomic_symlink(commit, active) != 0) {
		fpkg_errorf("symlink %s -> %s: %s", active, commit,
			    strerror(errno));
		return -1;
	}
	char ts[PATH_MAX];
	snprintf(ts, sizeof(ts), "%s/.timestamp", arch_dir);
	(void)write_file(ts, "", 0644);

	fpkg_infof("appstream %s/%s deployed (%s)", remote, arch,
		   legacy ? "legacy branch" : "appstream2");
	return 0;
}
static int ostree_config_get(const char* key, char* out, size_t outlen) {
	assert(key != NULL);
	// ostree_config_has() asks for the answer without a buffer.
	assert(out != NULL || outlen == 0);
	tool_ctx_t tc;
	char repo_arg[PATH_MAX + 32];
	tool_res_t res;

	tool_ctx_init(&tc);
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);

	char* argv[] = {"ostree", "config", repo_arg, "get", (char*)key, NULL};
	tool_res_init(&res);
	if (ostree_run(argv, &res) != 0 || res.rc != 0) {
		tool_res_free(&res);
		return -1;
	}
	if (out && outlen > 0) {
		if (res.out) trim(res.out);
		snprintf(out, outlen, "%s", res.out ? res.out : "");
	}
	tool_res_free(&res);
	return 0;
}
bool ostree_remote_exists(const char* name) {
	char key[512];

	snprintf(key, sizeof(key), "remote \"%s\".url", name);
	return ostree_config_get(key, NULL, 0) == 0;
}
int ostree_remote_add(const char* name, const char* url, bool gpg_verify,
		      bool gpg_verify_summary) {
	assert(name != NULL);
	assert(url != NULL);
	tool_ctx_t tc;
	char repo_arg[PATH_MAX + 32];
	char gv[64], gvs[64];
	tool_res_t res;

	if (ostree_remote_exists(name)) return 0;

	tool_ctx_init(&tc);
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);
	snprintf(gv, sizeof(gv), "--set=gpg-verify=%s",
		 gpg_verify ? "true" : "false");
	snprintf(gvs, sizeof(gvs), "--set=gpg-verify-summary=%s",
		 gpg_verify_summary ? "true" : "false");

	char* argv[] = {"ostree",   "remote", "add",	"--if-not-exists",
			gv,	    gvs,      repo_arg, (char*)name,
			(char*)url, NULL};
	tool_res_init(&res);
	if (ostree_run(argv, &res) != 0 || res.rc != 0) {
		log_tool_stderr(&res);
		tool_res_free(&res);
		fpkg_errorf("cannot add remote '%s'", name);
		return -1;
	}
	tool_res_free(&res);
	fpkg_infof("added remote '%s' (%s)", name, url);
	return 0;
}
// A bundle is an OSTree static delta superblock whose metadata carries
// the ref it installs and the origin URL it was built from.
#define BUNDLE_SUPERBLOCK_FORMAT                                               \
	"(a{sv}tayay(a{sv}aya(say)sstayay)aya(uayttay)a(yaytt))"
static int bundle_load(const char* path, char** ref_out, char** commit_out,
		       char** origin_out) {
	GError* err = NULL;
	GMappedFile* mf;
	GBytes* bytes;
	GVariant *delta, *meta, *to;
	gsize n = 0;
	const guint8* raw;

	*ref_out = NULL;
	*commit_out = NULL;
	*origin_out = NULL;

	mf = g_mapped_file_new(path, FALSE, &err);
	if (mf == NULL) {
		fpkg_errorf("%s: %s", path, err->message);
		g_error_free(err);
		return -1;
	}
	bytes = g_mapped_file_get_bytes(mf);
	delta = g_variant_new_from_bytes(
		G_VARIANT_TYPE(BUNDLE_SUPERBLOCK_FORMAT), bytes, FALSE);
	if (delta == NULL || !g_variant_is_normal_form(delta)) {
		fpkg_errorf("%s is not a flatpak bundle", path);
		if (delta != NULL) g_variant_unref(delta);
		g_bytes_unref(bytes);
		g_mapped_file_unref(mf);
		return -1;
	}
	meta = g_variant_get_child_value(delta, 0);
	to = g_variant_get_child_value(delta, 3);

	if (!g_variant_lookup(meta, "ref", "s", ref_out) || *ref_out == NULL ||
	    **ref_out == '\0') {
		fpkg_errorf("%s carries no ref", path);
		goto fail;
	}
	raw = g_variant_get_fixed_array(to, &n, 1);
	if (raw == NULL || n != 32) {
		fpkg_errorf("%s carries no commit", path);
		goto fail;
	}
	char* hex = malloc(n * 2 + 1);
	if (hex == NULL) {
		fpkg_errorf("out of memory");
		goto fail;
	}
	for (gsize i = 0; i < n; i++) snprintf(hex + 2 * i, 3, "%02x", raw[i]);
	*commit_out = hex;
	(void)g_variant_lookup(meta, "origin", "s", origin_out);

	g_variant_unref(meta);
	g_variant_unref(to);
	g_variant_unref(delta);
	g_bytes_unref(bytes);
	g_mapped_file_unref(mf);
	return 0;

fail:
	g_variant_unref(meta);
	g_variant_unref(to);
	g_variant_unref(delta);
	g_bytes_unref(bytes);
	g_mapped_file_unref(mf);
	free(*ref_out);
	*ref_out = NULL;
	return -1;
}
static int bundle_find_remote(const char* origin, char* out, size_t outlen) {
	tool_ctx_t tc;
	char repo_arg[PATH_MAX + 32];
	tool_res_t res;

	out[0] = '\0';
	if (origin == NULL || *origin == '\0') return -1;

	tool_ctx_init(&tc);
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);
	char* argv[] = {"ostree", "remote", "list", "-u", repo_arg, NULL};
	tool_res_init(&res);
	if (ostree_run(argv, &res) != 0 || res.rc != 0 || res.out == NULL) {
		tool_res_free(&res);
		return -1;
	}
	char* save = NULL;
	for (char* line = strtok_r(res.out, "\n", &save); line != NULL;
	     line = strtok_r(NULL, "\n", &save)) {
		char* ws = strpbrk(line, " \t");
		char *url, *end;

		if (ws == NULL) continue;
		// Skip the column padding between name and URL, then terminate the URL
		// (nothing may overwrite the name's terminator before it is compared).
		url = ws;
		while (*url == ' ' || *url == '\t') url++;
		end = url;
		while (*end != '\0' && *end != ' ' && *end != '\t') end++;
		*end = '\0';
		if (strcmp(url, origin) != 0) continue;
		*ws = '\0';
		snprintf(out, outlen, "%s", line);
		break;
	}
	tool_res_free(&res);
	return out[0] != '\0' ? 0 : -1;
}
// Read the ref a bundle installs without applying or deploying it.
int ostree_bundle_ref(const char* bundle_path, char** out_ref) {
	char *ref = NULL, *commit = NULL, *origin = NULL;

	*out_ref = NULL;
	if (bundle_load(bundle_path, &ref, &commit, &origin) != 0) return -1;
	free(commit);
	free(origin);
	*out_ref = ref;
	return 0;
}
int ostree_install_bundle(const char* bundle_path, const char* remote,
			  unsigned flags, char** out_ref) {
	assert(bundle_path != NULL);
	char *ref = NULL, *commit = NULL, *origin = NULL;
	char useremote[256], full[1024], create[1100];

	*out_ref = NULL;

	if (bundle_load(bundle_path, &ref, &commit, &origin) != 0) return -1;
	fpkg_infof("bundle %s: ref=%s commit=%s origin=%s", bundle_path, ref,
		   commit, origin != NULL ? origin : "(none)");

	{
		tool_ctx_t tc;
		char repo_arg[PATH_MAX + 32];
		tool_res_t res;

		tool_ctx_init(&tc);
		snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);
		char* argv[] = {"ostree", "static-delta",     "apply-offline",
				repo_arg, (char*)bundle_path, NULL};
		tool_res_init(&res);
		if (ostree_run(argv, &res) != 0 || res.rc != 0) {
			log_tool_stderr(&res);
			tool_res_free(&res);
			fpkg_errorf("cannot apply the bundle %s", bundle_path);
			goto fail;
		}
		tool_res_free(&res);
	}

	if (remote != NULL && *remote != '\0') {
		snprintf(useremote, sizeof(useremote), "%s", remote);
	} else if (bundle_find_remote(origin, useremote, sizeof(useremote)) !=
		   0) {
		const char* base = strrchr(bundle_path, '/');
		base = base != NULL ? base + 1 : bundle_path;
		size_t n = 0;
		for (; base[n] != '\0' && n + 1 < sizeof(useremote); n++) {
			char c = base[n];
			useremote[n] = (isalnum((unsigned char)c) || c == '.' ||
					c == '_' || c == '-')
					       ? c
					       : '_';
		}
		useremote[n] = '\0';
		fpkg_warnf("no remote matches '%s'; creating '%s' for it "
			   "(add its GPG key to enable updates)",
			   origin != NULL ? origin : "?", useremote);
		if (origin != NULL && *origin != '\0')
			(void)ostree_remote_add(useremote, origin, false,
						false);
	}

	snprintf(full, sizeof(full), "%s:%s", useremote, ref);
	snprintf(create, sizeof(create), "--create=%s", full);
	{
		tool_ctx_t tc;
		char repo_arg[PATH_MAX + 32];
		tool_res_t res;

		tool_ctx_init(&tc);
		snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);
		char* argv[] = {"ostree", "refs", create, "--force",
				repo_arg, commit, NULL};
		tool_res_init(&res);
		if (ostree_run(argv, &res) != 0 || res.rc != 0) {
			log_tool_stderr(&res);
			tool_res_free(&res);
			fpkg_errorf("cannot record %s in the store", full);
			goto fail;
		}
		tool_res_free(&res);
	}

	if (ostree_deploy_from_repo(NULL, ref, useremote, NULL, 0, flags) != 0)
		goto fail;

	*out_ref = ref;
	free(commit);
	free(origin);
	return 0;

fail:
	free(ref);
	free(commit);
	free(origin);
	return -1;
}
static const char* ref_split_type(const char* ref, char* type, size_t typelen) {
	const char* slash = strchr(ref, '/');

	if (slash == NULL || slash == ref) return NULL;
	snprintf(type, typelen, "%.*s", (int)(slash - ref), ref);
	return slash + 1;
}
static void write_deploy_ref(tool_ctx_t* tc, const char* ref,
			     const char* commit) {
	char repo_arg[PATH_MAX + 32], create[600];
	tool_res_t res;

	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc->repo);
	snprintf(create, sizeof(create), "--create=deploy/%s", ref);
	char* argv[] = {"ostree", "refs",	 repo_arg,
			create,	  (char*)commit, NULL};
	tool_res_init(&res);
	if (ostree_run(argv, &res) != 0 || res.rc != 0) {
		log_tool_stderr(&res);
		fpkg_warnf("cannot create the deploy ref for %s", ref);
	}
	tool_res_free(&res);
}
void store_mark_changed(void) {
	char path[PATH_MAX];
	int fd;

	store_path(path, sizeof(path), ".changed");
	if (utime(path, NULL) == 0) return;
	if (errno != ENOENT) return;
	fd = open(path, O_WRONLY | O_CREAT, 0644);
	if (fd >= 0) close(fd);
}
int ostree_backfill_deploy_data(void) {
	static const char* const kinds[] = {"app", "runtime", "extension",
					    NULL};
	tool_ctx_t tc;
	size_t fixed = 0;

	tool_ctx_init(&tc);
	for (size_t k = 0; kinds[k] != NULL; k++) {
		char kroot[PATH_MAX];
		DIR* kd;

		snprintf(kroot, sizeof(kroot), "%s/%s", store_root(), kinds[k]);
		kd = opendir(kroot);
		if (kd == NULL) continue;
		struct dirent* ide;
		while ((ide = readdir(kd)) != NULL) {
			char idroot[PATH_MAX];
			DIR* idd;

			if (ide->d_name[0] == '.') continue;
			snprintf(idroot, sizeof(idroot), "%s/%s", kroot,
				 ide->d_name);
			idd = opendir(idroot);
			if (idd == NULL) continue;
			struct dirent* ade;
			while ((ade = readdir(idd)) != NULL) {
				char archroot[PATH_MAX];
				DIR* ad;

				if (ade->d_name[0] == '.') continue;
				snprintf(archroot, sizeof(archroot), "%s/%s",
					 idroot, ade->d_name);
				ad = opendir(archroot);
				if (ad == NULL) continue;
				struct dirent* be;
				while ((be = readdir(ad)) != NULL) {
					char branch[PATH_MAX], active[PATH_MAX],
						origin[PATH_MAX], ref[512];
					char commit[128];
					ssize_t n;
					FILE* f;

					if (be->d_name[0] == '.') continue;
					snprintf(branch, sizeof(branch),
						 "%s/%s", archroot, be->d_name);
					snprintf(active, sizeof(active),
						 "%s/active", branch);
					n = readlink(active, commit,
						     sizeof(commit) - 1);
					if (n <= 0) continue;
					commit[n] = '\0';

					char probe[PATH_MAX];
					snprintf(probe, sizeof(probe),
						 "%s/%s/deploy", branch,
						 commit);
					if (access(probe, F_OK) == 0) {
						char ref_marker[PATH_MAX];
						snprintf(ref_marker,
							 sizeof(ref_marker),
							 "%s/%s/files/.ref",
							 branch, commit);
						if (access(ref_marker, F_OK) !=
						    0) {
							(void)write_file(
								ref_marker, "",
								0644);
							fixed++;
						}
						continue;
					}

					origin[0] = '\0';
					snprintf(probe, sizeof(probe),
						 "%s/.origin", branch);
					f = fopen(probe, "r");
					if (f != NULL) {
						if (fgets(origin,
							  sizeof(origin),
							  f) != NULL) {
							char* nl = strchr(
								origin, '\n');
							if (nl != NULL)
								*nl = '\0';
						}
						fclose(f);
					}
					snprintf(ref, sizeof(ref),
						 "%s/%s/%s/%s",
						 strcmp(kinds[k],
							"extension") == 0
							 ? "runtime"
							 : kinds[k],
						 ide->d_name, ade->d_name,
						 be->d_name);
					write_deploy_data(branch, commit,
							  origin, NULL, 0);
					write_deploy_ref(&tc, ref, commit);
					{
						char ref_marker[PATH_MAX];
						snprintf(ref_marker,
							 sizeof(ref_marker),
							 "%s/%s/files/.ref",
							 branch, commit);
						(void)write_file(ref_marker, "",
								 0644);
					}
					fixed++;
				}
				closedir(ad);
			}
			closedir(idd);
		}
		closedir(kd);
	}
	if (fixed > 0) {
		store_mark_changed();
		fpkg_infof("wrote flatpak deploy data for %zu deploy(s)",
			   fixed);
	}
	return 0;
}
int ostree_deploy_from_repo(const char* child_repo, const char* ref,
			    const char* origin, char** subpaths,
			    size_t nsubpaths, unsigned flags) {
	// NULL child_repo means the commit is already in the store repo;
	// ostree_install_bundle() takes that path.
	assert(ref != NULL);
	char type[32], full[512];
	const char* rest;
	fpkg_ref_t r;
	tool_ctx_t tc;
	char commit[128], deploy[PATH_MAX];

	rest = ref_split_type(ref, type, sizeof(type));
	if (rest == NULL) {
		fpkg_errorf("bad ref '%s'", ref);
		return -1;
	}
	if (strcmp(type, "app") != 0 && strcmp(type, "runtime") != 0) {
		fpkg_errorf("cannot deploy '%s': flatpkg has app/ and "
			    "runtime/ deploys",
			    ref);
		return -1;
	}
	if (ref_parse(rest, &r) != 0) {
		fpkg_errorf("bad ref '%s'", ref);
		return -1;
	}
	if (nsubpaths > 0)
		fpkg_infof("%zu subpath(s) requested for %s; deploying the "
			   "whole ref",
			   nsubpaths, ref);

	tool_ctx_init(&tc);

	if (child_repo && *child_repo) {
		char ccommit[128];

		if (child_repo_import(&tc, child_repo, ref, origin, nsubpaths,
				      ccommit, sizeof(ccommit)) != 0) {
			ref_free(&r);
			return -1;
		}
	}

	snprintf(full, sizeof(full), "%s:%s", origin ? origin : "", ref);
	if (tool_rev_probe(&tc, full, commit, sizeof(commit)) != 0 &&
	    tool_rev_parse(&tc, ref, commit, sizeof(commit)) != 0) {
		fpkg_errorf("cannot resolve %s after importing it", ref);
		ref_free(&r);
		return -1;
	}

	char branch_dir[PATH_MAX];
	if (!(flags & FLATPAK_HELPER_DEPLOY_FLAGS_REINSTALL) &&
	    find_deploy_dir(type, &r, branch_dir, sizeof(branch_dir)) == 0) {
		char active[PATH_MAX], lbuf[PATH_MAX];
		snprintf(active, sizeof(active), "%s/active", branch_dir);
		ssize_t n = readlink(active, lbuf, sizeof(lbuf) - 1);
		if (n > 0) {
			lbuf[n] = '\0';
			if (strcmp(lbuf, commit) == 0) {
				fpkg_infof("%s is already installed at %s", ref,
					   commit);
				(void)ostree_backfill_deploy_data();
				if (strcmp(type, "app") == 0) {
					world_add(r.id);
					(void)store_update_exports();
				}
				ref_free(&r);
				return 0;
			}
		}
	}

	// --no-deploy: the caller only wanted the objects and the ref in place.
	if (flags & FLATPAK_HELPER_DEPLOY_FLAGS_NO_DEPLOY) {
		fpkg_infof("%s imported without a deploy (--no-deploy)", ref);
		ref_free(&r);
		return 0;
	}

	if (deploy_checkout(&tc, type, &r, commit, deploy, sizeof(deploy)) !=
	    0) {
		ref_free(&r);
		return -1;
	}
	if (make_active(deploy) != 0) {
		ref_free(&r);
		return -1;
	}
	write_origin(deploy, origin ? origin : "");
	// write_deploy_data() takes the BRANCH dir and appends the commit
	// itself, but `deploy` here IS the commit dir, so trim it the way
	// write_origin() does.
	{
		char bdir[PATH_MAX];
		if (!deploy_branch_dir(deploy, bdir, sizeof(bdir)))
			fpkg_warnf("deploy data: no '/' in %s, "
				   "skipping deploy data",
				   deploy);
		else
			write_deploy_data(bdir, commit, origin, subpaths,
					  nsubpaths);
	}
	write_deploy_ref(&tc, ref, commit);

	if (strcmp(type, "app") == 0) {
		world_add(r.id);
		(void)store_update_exports();
	}
	ref_free(&r);
	store_mark_changed();
	fpkg_okf("deployed %s (%s)", ref, commit);
	return 0;
}
