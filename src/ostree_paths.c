#include <sys/param.h>
#include <sys/stat.h>

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ostree_internal.h"

// Every file this reads is small (an .origin, a repo config): the ceiling is
// what keeps a caller-named device or a runaway file from being an allocation
// the daemon pays for.
#define SLURP_MAX (1u << 20)

char* slurp_file(const char* path) {
	assert(path != NULL);
	FILE* f = fopen(path, "r");
	if (!f) return NULL;
	clearerr(f);
	char* buf = NULL;
	size_t len = 0, cap = 0;
	char chunk[4096];
	// Stop at the first short read: fread() has already set EOF or the
	// error, and reading again after it has no effect.
	for (;;) {
		size_t r = fread(chunk, 1, sizeof(chunk), f);
		if (r > 0) {
			if (len + r > SLURP_MAX) {
				free(buf);
				fclose(f);
				return NULL;
			}
			if (len + r + 1 > cap) {
				cap = cap ? cap * 2 : 8192;
				while (len + r + 1 > cap) cap *= 2;
				char* nb = realloc(buf, cap);
				if (!nb) {
					free(buf);
					fclose(f);
					return NULL;
				}
				buf = nb;
			}
			memcpy(buf + len, chunk, r);
			len += r;
		}
		if (r < sizeof(chunk)) break;
	}
	if (ferror(f)) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	if (!buf) return NULL;
	buf[len] = '\0';
	return buf;
}
void store_path(char* buf, size_t len, const char* fmt, ...) {
	assert(buf != NULL);
	assert(len > 0);
	assert(fmt != NULL);
	va_list ap;
	va_start(ap, fmt);
	char tail[PATH_MAX];
	vsnprintf(tail, sizeof(tail), fmt, ap);
	va_end(ap);
	snprintf(buf, len, "%s/%s", store_root(), tail);
}
const char* host_arch(void) {
	struct utsname u;
	if (uname(&u) == 0) {
		if (strcmp(u.machine, "amd64") == 0) return "x86_64";
		if (strcmp(u.machine, "aarch64") == 0) return "aarch64";
		if (strcmp(u.machine, "i386") == 0) return "i386";
		static char other[64];
		snprintf(other, sizeof(other), "%s", u.machine);
		return other;
	}
	return "x86_64";
}
int atomic_symlink(const char* target, const char* link_path) {
	assert(target != NULL);
	assert(link_path != NULL);
	char tmp[PATH_MAX];
	int n = snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", link_path,
			 (long)getpid());
	if (n < 0 || (size_t)n >= sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	(void)unlink(tmp);
	if (symlink(target, tmp) != 0) {
		int e = errno;
		(void)unlink(tmp);
		errno = e;
		return -1;
	}
	if (rename(tmp, link_path) != 0) {
		int e = errno;
		(void)unlink(tmp);
		errno = e;
		return -1;
	}
	return 0;
}
// A bare kind word is a missing id, not an id: "extension" names a
// type, never a package.
static bool ref_kind_word(const char* s) {
	assert(s != NULL);
	return strcmp(s, "app") == 0 || strcmp(s, "runtime") == 0 ||
	       strcmp(s, "extension") == 0;
}
int ref_parse(const char* s, fpkg_ref_t* out) {
	assert(s != NULL);
	assert(out != NULL);
	memset(out, 0, sizeof(*out));
	out->arch = xstrdup(host_arch());

	const char* rest = s;
	const char* colon = strchr(s, ':');
	const char* slash = strchr(s, '/');
	if (colon && (!slash || colon < slash)) {
		// An empty prefix names no remote, and a prefix is an argv
		// word for root's ostree as well as a store path: same grammar.
		if (colon == s) {
			ref_free(out);
			return -1;
		}
		out->remote = strndup(s, colon - s);
		if (out->remote == NULL || !remote_name_valid(out->remote)) {
			ref_free(out);
			return -1;
		}
		rest = colon + 1;
	}
	if (!*rest) {
		ref_free(out);
		return -1;
	}

	if (strncmp(rest, "app/", 4) == 0) rest += 4;
	else if (strncmp(rest, "runtime/", 8) == 0)
		rest += 8;
	else if (strncmp(rest, "extension/", 10) == 0) {
		rest += 10;
		if (!*rest) {
			ref_free(out);
			return -1;
		}
	}
	if (!*rest) {
		ref_free(out);
		return -1;
	}

	// An over-long component is refused, not silently cut to the first
	// 255 bytes and then judged: the two spellings would be one ref.
	char id[256] = "", arch[64] = "", branch[64] = "";
	const char* p1 = strchr(rest, '/');
	size_t idlen = p1 != NULL ? (size_t)(p1 - rest) : strlen(rest);
	if (idlen >= sizeof(id)) {
		ref_free(out);
		return -1;
	}
	memcpy(id, rest, idlen);
	id[idlen] = '\0';
	if (p1 != NULL) {
		const char* a = p1 + 1;
		const char* p2 = strchr(a, '/');
		size_t alen = p2 != NULL ? (size_t)(p2 - a) : strlen(a);
		if (alen >= sizeof(arch)) {
			ref_free(out);
			return -1;
		}
		memcpy(arch, a, alen);
		arch[alen] = '\0';
		if (p2 != NULL) {
			size_t blen = strlen(p2 + 1);
			if (blen >= sizeof(branch)) {
				ref_free(out);
				return -1;
			}
			memcpy(branch, p2 + 1, blen + 1);
		}
	}

	if (!id[0] || ref_kind_word(id) || !fpkg_component_ok(id)) {
		ref_free(out);
		return -1;
	}
	// An arch or branch becomes a path component too: same grammar, so a
	// crafted ref cannot walk out of the store.
	if (arch[0] && !fpkg_component_ok(arch)) {
		ref_free(out);
		return -1;
	}
	if (branch[0] && !fpkg_component_ok(branch)) {
		ref_free(out);
		return -1;
	}
	out->id = xstrdup(id);
	if (arch[0]) free(out->arch), out->arch = xstrdup(arch);
	if (branch[0]) out->branch = xstrdup(branch);
	return 0;
}
// True when `path` is the store root or under it once canonicalized.  A
// destructive store operation calls this before it touches anything: a
// component that still escapes cannot reach a file outside the store even if
// the grammar above is ever loosened.
bool store_path_contained(const char* path) {
	assert(path != NULL);
	char root[PATH_MAX], real[PATH_MAX];

	if (realpath(store_root(), root) == NULL) return false;
	size_t rl = strlen(root);
	if (realpath(path, real) != NULL)
		return strncmp(real, root, rl) == 0 &&
		       (real[rl] == '/' || real[rl] == '\0');
	// The leaf may not exist yet (a checkout still to come): judge the
	// directory it will be created in.
	char parent[PATH_MAX];
	snprintf(parent, sizeof(parent), "%s", path);
	char* slash = strrchr(parent, '/');
	if (slash == NULL || slash == parent) return false;
	*slash = '\0';
	if (realpath(parent, real) == NULL) return false;
	return strncmp(real, root, rl) == 0 &&
	       (real[rl] == '/' || real[rl] == '\0');
}

// The bare id from any ref spelling the CLI accepts; 0 with the id,
// -1 when the ref does not parse.
int ostree_ref_id(const char* ref, char* out, size_t len) {
	assert(ref != NULL);
	assert(out != NULL);
	assert(len > 0);
	fpkg_ref_t r;
	if (ref_parse(ref, &r) != 0) return -1;
	snprintf(out, len, "%s", r.id);
	ref_free(&r);
	return 0;
}
// "<type>/<id>/<arch>/<branch>" (type = "app" or "runtime").
void ref_full(const fpkg_ref_t* r, const char* type, char* out,
		     size_t len) {
	assert(r != NULL);
	assert(type != NULL);
	assert(out != NULL);
	assert(len > 0);
	snprintf(out, len, "%s/%s/%s/%s", type, r->id, r->arch,
		 r->branch ? r->branch : "");
}
char* remote_setting(const char* remote, const char* key) {
	assert(remote != NULL);
	assert(key != NULL);
	char config[PATH_MAX];
	store_path(config, sizeof(config), "repo/config");
	FILE* f = fopen(config, "r");
	if (!f) return NULL;

	char want[128];
	snprintf(want, sizeof(want), "[remote \"%s\"]", remote);
	char line[1024];
	bool in_remote = false;
	char* val = NULL;
	while (fgets(line, sizeof(line), f)) {
		trim(line);
		if (line[0] == '[') {
			in_remote = (strcmp(line, want) == 0);
			continue;
		}
		if (!in_remote) continue;
		size_t klen = strlen(key);
		if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
			val = xstrdup(line + klen + 1);
			break;
		}
	}
	fclose(f);
	return val;
}
bool ostree_remote_is_local(const char* remote) {
	char* url;
	bool local;

	if (remote == NULL || *remote == '\0') return false;
	url = remote_setting(remote, "url");
	local = url != NULL &&
		(g_str_has_prefix(url, "file://") || url[0] == '/');
	free(url);
	return local;
}

bool ostree_remote_exists(const char* name) {
	char* url;
	bool found;

	if (name == NULL || *name == '\0') return false;
	url = remote_setting(name, "url");
	found = url != NULL;
	free(url);
	return found;
}

char** list_remotes(size_t* n_out) {
	*n_out = 0;
	char config[PATH_MAX];
	store_path(config, sizeof(config), "repo/config");
	FILE* f = fopen(config, "r");
	if (!f) return NULL;

	char** names = NULL;
	size_t n = 0, cap = 0;
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		trim(line);
		if (strncmp(line, "[remote \"", 9) != 0) continue;
		char* end = strchr(line + 9, '"');
		if (!end) continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 8;
			char** new_names = realloc(names, cap * sizeof(char*));
			if (!new_names) {
				fpkg_errorf("out of memory");
				exit(1);
			}
			names = new_names;
		}
		{
			// An allocation failure exits here rather than storing a
			// NULL every caller would strcmp() and free().
			size_t klen = (size_t)(end - (line + 9));
			char* name = malloc(klen + 1);

			if (name == NULL) {
				fpkg_errorf("out of memory");
				exit(1);
			}
			memcpy(name, line + 9, klen);
			name[klen] = '\0';
			names[n++] = name;
		}
	}
	fclose(f);
	*n_out = n;
	return names;
}
char* first_remote(void) {
	size_t n = 0;
	char** remotes = list_remotes(&n);
	if (n == 0) {
		free(remotes);
		return NULL;
	}
	char* r = xstrdup(remotes[0]);
	for (size_t i = 0; i < n; i++) free(remotes[i]);
	free(remotes);
	return r;
}
// Bare remote names, for `flatpkg complete remotes`.
char** ostree_remote_names(size_t* n_out) { return list_remotes(n_out); }
