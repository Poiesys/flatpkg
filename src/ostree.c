#include <assert.h>
#include <dlfcn.h>
#include <ftw.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <utime.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <zlib.h>

#include "flatpkg.h"
#define FPKG_MODULE "ostree"
#include "fpkglog.h"

static fpkg_error_t write_file(const char* path, const char* content,
			       mode_t mode) {
	assert(path != NULL);
	assert(content != NULL);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0) return fpkg_error_from_errno(errno);
	size_t len = strlen(content);
	ssize_t w = write(fd, content, len);
	int e = errno;
	close(fd);
	if (w != (ssize_t)len) return e ? fpkg_error_from_errno(e) : FPKG_EIO;
	return FPKG_OK;
}

static int rm_cb(const char* path, const struct stat* st, int type,
		 struct FTW* ftw) {
	assert(path != NULL);
	(void)st;
	(void)ftw;
	if (type == FTW_DP || type == FTW_D) rmdir(path);
	else
		unlink(path);
	return 0;
}

static int rm_rf(const char* path) {
	assert(path != NULL);
	// FTW_MOUNT: FTW_PHYS alone only skips symlinks.
	return nftw(path, rm_cb, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
}

static char* slurp_file(const char* path) {
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

static void trim(char* s) {
	assert(s != NULL);
	char* p = s;
	while (*p && isspace((unsigned char)*p)) p++;
	if (p != s) memmove(s, p, strlen(p) + 1);
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static void store_path(char* buf, size_t len, const char* fmt, ...) {
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

static const char* host_arch(void) {
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

static int atomic_symlink(const char* target, const char* link_path) {
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
		out->remote = strndup(s, colon - s);
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

	char id[256] = "", arch[64] = "", branch[64] = "";
	const char* p1 = strchr(rest, '/');
	if (!p1) {
		snprintf(id, sizeof(id), "%s", rest);
	} else {
		snprintf(id, sizeof(id), "%.*s", (int)(p1 - rest), rest);
		const char* p2 = strchr(p1 + 1, '/');
		if (!p2) {
			snprintf(arch, sizeof(arch), "%s", p1 + 1);
		} else {
			if (p2 > p1 + 1)
				snprintf(arch, sizeof(arch), "%.*s",
					 (int)(p2 - p1 - 1), p1 + 1);
			snprintf(branch, sizeof(branch), "%s", p2 + 1);
		}
	}

	if (!id[0] || ref_kind_word(id)) {
		ref_free(out);
		return -1;
	}
	for (char* c = id; *c; c++) {
		if (isspace((unsigned char)*c) || *c == '/' || *c == ':') {
			ref_free(out);
			return -1;
		}
	}
	out->id = xstrdup(id);
	if (arch[0]) free(out->arch), out->arch = xstrdup(arch);
	if (branch[0]) out->branch = xstrdup(branch);
	return 0;
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
static void ref_full(const fpkg_ref_t* r, const char* type, char* out,
		     size_t len) {
	assert(r != NULL);
	assert(type != NULL);
	assert(out != NULL);
	assert(len > 0);
	snprintf(out, len, "%s/%s/%s/%s", type, r->id, r->arch,
		 r->branch ? r->branch : "");
}

static char* remote_setting(const char* remote, const char* key) {
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

static char** list_remotes(size_t* n_out) {
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
		names[n++] = strndup(line + 9, end - (line + 9));
	}
	fclose(f);
	*n_out = n;
	return names;
}

static char* first_remote(void) {
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

typedef struct {
	char repo[PATH_MAX];
	char keys[PATH_MAX];
	char store[PATH_MAX];
} tool_ctx_t;

typedef struct {
	char* out;
	char* err;
	int rc;
} tool_res_t;

static void tool_res_init(tool_res_t* res) {
	assert(res != NULL);
	memset(res, 0, sizeof(*res));
	res->rc = -1;
}

static void tool_res_free(tool_res_t* res) {
	assert(res != NULL);
	free(res->out);
	free(res->err);
	memset(res, 0, sizeof(*res));
}

static void tool_ctx_init(tool_ctx_t* tc) {
	assert(tc != NULL);
	memset(tc, 0, sizeof(*tc));
	snprintf(tc->repo, sizeof(tc->repo), "/ostree/repo");
	snprintf(tc->keys, sizeof(tc->keys), "/ostree/keys");
	snprintf(tc->store, sizeof(tc->store), "/ostree");
}

static size_t buf_append(char** buf, size_t* cap, const char* data,
			 size_t len) {
	assert(buf != NULL);
	assert(cap != NULL);
	assert(data != NULL);
	size_t cur = *buf ? strlen(*buf) : 0;
	if (cur + len + 1 > *cap) {
		size_t nc = *cap ? *cap : 8192;
		while (cur + len + 1 > nc) nc *= 2;
		char* nb = realloc(*buf, nc);
		if (!nb) {
			fpkg_errorf("out of memory");
			exit(1);
		}
		*buf = nb;
		*cap = nc;
	}
	memcpy(*buf + cur, data, len);
	(*buf)[cur + len] = '\0';
	return cur + len;
}

typedef void (*tool_line_cb)(const char* line, void* arg);

// Splits a stream on '\n', '\r' and ESC[8 (libostree redraws its
// progress line with ESC[8) and hands each segment to cb.
typedef struct {
	char buf[4096];
	size_t len;
} line_buf_t;

static void line_feed(line_buf_t* lb, const char* data, size_t n,
		      tool_line_cb cb, void* arg) {
	assert(lb != NULL);
	assert(data != NULL);
	for (size_t k = 0; k < n; k++) {
		char c = data[k];
		if (c == '8' && k > 0 && data[k - 1] == '\x1b') {
			// ESC[8 frame separator: drop the ESC too.
			if (lb->len > 0 && lb->buf[lb->len - 1] == '\x1b')
				lb->len--;
			if (lb->len) {
				lb->buf[lb->len] = '\0';
				cb(lb->buf, arg);
				lb->len = 0;
			}
			continue;
		}
		if (c == '\n' || c == '\r') {
			if (lb->len) {
				lb->buf[lb->len] = '\0';
				cb(lb->buf, arg);
				lb->len = 0;
			}
		} else if (lb->len < sizeof(lb->buf) - 1) {
			lb->buf[lb->len++] = c;
		}
	}
}

static int capture_child(pid_t pid, int outfd, int errfd, tool_res_t* res,
			 tool_line_cb cb, void* arg, bool cb_from_out,
			 void (*tick)(void* tick_arg), void* tick_arg) {
	struct pollfd fds[2] = {
		{outfd, POLLIN, 0},
		{errfd, POLLIN, 0},
	};
	bool done[2] = {false, false};
	size_t ocap = 0, ecap = 0;
	int reaped = 0;
	int grace = 0;
	line_buf_t lbo = {.len = 0}, lbe = {.len = 0};

	while (1) {
		int n = poll(fds, 2, 200);
		if (n > 0) {
			for (int i = 0; i < 2; i++) {
				if (done[i] ||
				    !(fds[i].revents & (POLLIN | POLLHUP)))
					continue;
				char chunk[4096];
				ssize_t r =
					read(fds[i].fd, chunk, sizeof(chunk));
				if (r > 0) {
					if (i == 0) {
						if (cb && cb_from_out)
							line_feed(&lbo, chunk,
								  (size_t)r, cb,
								  arg);
						else
							buf_append(&res->out,
								   &ocap, chunk,
								   (size_t)r);
					} else {
						buf_append(&res->err, &ecap,
							   chunk, (size_t)r);
						if (cb)
							line_feed(&lbe, chunk,
								  (size_t)r, cb,
								  arg);
					}
				} else if (r == 0) {
					done[i] = true;
				} else if (errno != EINTR && errno != EAGAIN) {
					done[i] = true;
				}
			}
		} else if (n < 0 && errno != EINTR) {
			int status = 0;
			if (!reaped) {
				waitpid(pid, &status, 0);
				res->rc = WIFEXITED(status)
						  ? WEXITSTATUS(status)
						  : 128 + WTERMSIG(status);
			}
			break;
		} else if (n == 0 && tick) {
			tick(tick_arg);
		}

		if (!reaped) {
			int status = 0;
			pid_t w = waitpid(pid, &status, WNOHANG);
			if (w == pid) {
				reaped = 1;
				res->rc = WIFEXITED(status)
						  ? WEXITSTATUS(status)
						  : 128 + WTERMSIG(status);
			} else if (w < 0) {
				reaped = 1;
				res->rc = 127;
			} else if (done[0] && done[1]) {
				// Both stdio streams closed while the child still runs: the tool is
				// working, so keep waiting - breaking out leaves rc at -1 and
				// reports a failure for a pull that then succeeds.
				w = waitpid(pid, &status, 0);
				if (w == pid) {
					reaped = 1;
					res->rc =
						WIFEXITED(status)
							? WEXITSTATUS(status)
							: 128 + WTERMSIG(
									status);
				} else {
					reaped = 1;
					res->rc = 127;
				}
			}
		} else {
			if (done[0] && done[1]) break;
			if (++grace > 10) break;
		}

		if (reaped && done[0] && done[1]) break;
		fds[0].revents = 0;
		fds[1].revents = 0;
	}
	close(outfd);
	close(errfd);
	if (cb && cb_from_out && lbo.len > 0) {
		lbo.buf[lbo.len] = '\0';
		cb(lbo.buf, arg);
	}
	if (cb && lbe.len > 0) {
		lbe.buf[lbe.len] = '\0';
		cb(lbe.buf, arg);
	}
	if (!res->out) res->out = xstrdup("");
	if (!res->err) res->err = xstrdup("");
	return 0;
}

static int tool_binary(char* out, size_t len) {
	assert(out != NULL);
	assert(len > 0);
	// Native ostree (ports/sysutils/libostree) is the only path.
	snprintf(out, len, "/usr/local/bin/ostree");
	return 0;
}

static int run_direct_cb(char* const argv[], tool_res_t* res, tool_line_cb cb,
			 void* arg, void (*tick)(void* tick_arg),
			 void* tick_arg) {
	char tool[PATH_MAX];
	if (tool_binary(tool, sizeof(tool)) != 0) {
		res->rc = 1;
		return -1;
	}

	size_t nargv = 0;
	while (argv[nargv]) nargv++;
	char** argv2 = calloc(nargv + 1, sizeof(char*));
	if (!argv2) {
		fpkg_errorf("out of memory");
		res->rc = 1;
		return -1;
	}
	for (size_t i = 0; i < nargv; i++) {
		char* p = strstr(argv[i], "/ostree/");
		if (p) {
			char tmp[PATH_MAX];
			snprintf(tmp, sizeof(tmp), "%.*s%s/%s",
				 (int)(p - argv[i]), argv[i], store_root(),
				 p + 8);
			argv2[i] = xstrdup(tmp);
		} else {
			argv2[i] = argv[i];
		}
	}

	int outpipe[2], errpipe[2];
	int pty_master = -1, pty_slave = -1;
	if (pipe(errpipe) != 0) {
		fpkg_errorf("pipe: %s", strerror(errno));
		res->rc = 1;
		goto fail_argv;
	}
	if (cb) {
		pty_master = posix_openpt(O_RDWR | O_NOCTTY);
		if (pty_master >= 0 && grantpt(pty_master) == 0 &&
		    unlockpt(pty_master) == 0) {
			char* sname = ptsname(pty_master);
			if (sname) pty_slave = open(sname, O_RDWR | O_NOCTTY);
			if (pty_slave >= 0) {
				struct winsize ws = {24, 80, 0, 0};
				ioctl(pty_slave, TIOCSWINSZ, &ws);
			} else {
				close(pty_master);
				pty_master = -1;
			}
		} else {
			if (pty_master >= 0) close(pty_master);
			pty_master = -1;
		}
	}
	if (pty_master < 0 && pipe(outpipe) != 0) {
		fpkg_errorf("pipe: %s", strerror(errno));
		res->rc = 1;
		close(errpipe[0]);
		close(errpipe[1]);
		goto fail_argv;
	}
	pid_t pid = fork();
	if (pid < 0) {
		fpkg_errorf("fork: %s", strerror(errno));
		res->rc = 1;
		close(errpipe[0]);
		close(errpipe[1]);
		if (pty_master >= 0) {
			close(pty_master);
			close(pty_slave);
		} else {
			close(outpipe[0]);
			close(outpipe[1]);
		}
		goto fail_argv;
	}
	if (pid == 0) {
		if (pty_slave >= 0) {
			dup2(pty_slave, 1);
			close(pty_slave);
			close(pty_master);
		} else {
			dup2(outpipe[1], 1);
			close(outpipe[0]);
			close(outpipe[1]);
		}
		dup2(errpipe[1], 2);
		close(errpipe[0]);
		close(errpipe[1]);
		umask(0);
		execv(tool, argv2);
		dprintf(2, "flatpkg: exec %s: %s\n", tool, strerror(errno));
		_exit(127);
	}
	close(errpipe[1]);
	if (pty_master >= 0) {
		close(pty_slave);
		capture_child(pid, pty_master, errpipe[0], res, cb, arg, true,
			      tick, tick_arg);
	} else {
		close(outpipe[1]);
		capture_child(pid, outpipe[0], errpipe[0], res, cb, arg, false,
			      tick, tick_arg);
	}
	for (size_t i = 0; i < nargv; i++)
		if (argv2[i] != argv[i]) free(argv2[i]);
	free(argv2);
	return 0;

fail_argv:
	for (size_t i = 0; i < nargv; i++)
		if (argv2[i] != argv[i]) free(argv2[i]);
	free(argv2);
	return -1;
}

// Run the ostree CLI with argv. res->rc is the tool's exit status when
// it ran (any rc), -1 when it could not be started.
static int ostree_run_cb(char* const argv[], tool_res_t* res, tool_line_cb cb,
			 void* arg, void (*tick)(void* tick_arg),
			 void* tick_arg) {
	tool_res_init(res);

	// FLATPKG_OSTREE_VERBOSE=1 turns on ostree's own -v chatter.
	char* vargv[24];
	const char* ov = cli_getenv("FLATPKG_OSTREE_VERBOSE");
	if (ov != NULL && (*ov == '1' || *ov == 't') && argv[0] != NULL) {
		size_t vn = 0, maxn = sizeof(vargv) / sizeof(vargv[0]);
		vargv[vn++] = argv[0];
		if (vn < maxn) vargv[vn++] = (char*)"-v";
		for (size_t i = 1; argv[i] != NULL && vn + 1 < maxn; i++)
			vargv[vn++] = argv[i];
		vargv[vn] = NULL;
		argv = vargv;
	}

	return run_direct_cb(argv, res, cb, arg, tick, tick_arg);
}

static int ostree_run(char* const argv[], tool_res_t* res) {
	assert(argv != NULL);
	assert(res != NULL);
	return ostree_run_cb(argv, res, NULL, NULL, NULL, NULL);
}

static void log_tool_stderr(const tool_res_t* res) {
	assert(res != NULL);
	if (!res->err || !*res->err) return;
	char* copy = xstrdup(res->err);
	char* save = NULL;
	for (char* line = strtok_r(copy, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save))
		fpkg_warnf("ostree: %s", line);
	free(copy);
}

typedef struct {
	fp_bar_t* pb;
	uint64_t target;
} pull_bar_t;

static void pull_bar_tick(void* arg) {
	pull_bar_t* s = arg;
	uint64_t cur = s->pb->current;
	uint64_t next = cur + (s->target - cur) / 4 + 1;
	if (next > s->target) next = s->target;
	if (next != cur) fp_bar_update(s->pb, next);
}

static void pull_bar_target(pull_bar_t* s, uint64_t pct) {
	assert(s != NULL);
	if (pct > s->target) s->target = pct;
}

static double size_bytes(double val, const char* unit) {
	assert(unit != NULL);
	if (strcmp(unit, "bytes") == 0 || strcmp(unit, "B") == 0) return val;
	if (strcmp(unit, "KB") == 0 || strcmp(unit, "kB") == 0)
		return val * 1000.0;
	if (strcmp(unit, "MB") == 0) return val * 1000000.0;
	if (strcmp(unit, "GB") == 0) return val * 1000000000.0;
	if (strcmp(unit, "KiB") == 0) return val * 1024.0;
	if (strcmp(unit, "MiB") == 0) return val * 1048576.0;
	if (strcmp(unit, "GiB") == 0) return val * 1073741824.0;
	return -1.0;
}

static int64_t mono_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

static __thread int64_t g_delta_start_ns;
static __thread double g_delta_total_bytes;

static void transfer_progress_cb(const char* line, void* arg) {
	assert(line != NULL);
	pull_bar_t* s = arg;
	// g_format_size separates value and unit with a non-breaking space;
	// normalize so the delta sscanf parses.
	for (char* q = (char*)line; q[0]; q++) {
		if ((unsigned char)q[0] == 0xC2 &&
		    (unsigned char)q[1] == 0xA0) {
			q[0] = ' ';
			q[1] = ' ';
			q++;
		}
	}
	static const char objs_marker[] = "Receiving objects:";
	static const char check_marker[] = "Checking out files:";
	static const char delta_marker[] = "Receiving delta parts:";
	const char* p = strstr(line, objs_marker);
	if (p) {
		int pct = atoi(p + sizeof(objs_marker) - 1);
		if (pct >= 0 && pct <= 100) pull_bar_target(s, (uint64_t)pct);
		return;
	}
	p = strstr(line, check_marker);
	if (p) {
		int pct = atoi(p + sizeof(check_marker) - 1);
		if (pct >= 0 && pct <= 100) pull_bar_target(s, (uint64_t)pct);
		return;
	}
	p = strstr(line, delta_marker);
	if (p) {
		unsigned a = 0, b = 0;
		double fetched = 0.0, total = 0.0, rate = 0.0;
		char fu[8] = "", tu[8] = "", ru[8] = "";
		int nf = sscanf(p + sizeof(delta_marker) - 1,
				"%u/%u %lf %7[^/]/%lf %7s %lf %7s", &a, &b,
				&fetched, fu, &total, tu, &rate, ru);
		if (nf >= 6) {
			double fb = size_bytes(fetched, fu);
			double tb = size_bytes(total, tu);
			if (tb > 0 && fb >= 0) {
				if (fb > 0) {
					double pct = fb * 100.0 / tb;
					if (pct > 100.0) pct = 100.0;
					pull_bar_target(s, (uint64_t)pct);
					return;
				}
				if (nf >= 8) {
					double rb;
					char ru2[8];
					size_t rulen = strlen(ru);
					if (rulen > 2 &&
					    strcmp(ru + rulen - 2, "/s") == 0) {
						memcpy(ru2, ru, rulen - 2);
						ru2[rulen - 2] = '\0';
						rb = size_bytes(rate, ru2);
					} else {
						rb = size_bytes(rate, ru);
					}
					if (rb > 0) {
						if (g_delta_start_ns == 0) {
							g_delta_start_ns =
								mono_ns();
							g_delta_total_bytes =
								tb;
						}
						double elapsed =
							(mono_ns() -
							 g_delta_start_ns) /
							1e9;
						double done = rb * elapsed;
						double pct = done * 100.0 / tb;
						if (pct > 99.0) pct = 99.0;
						pull_bar_target(s,
								(uint64_t)pct);
						return;
					}
				}
			}
		}
		if (b > 0) pull_bar_target(s, (uint64_t)(a * 100ull / b));
	}
}

// "pulling X" -> "pulled X": what a bar says when it is done.
static void pull_done_msg(const char* label, char* out, size_t cap) {
	if (strncmp(label, "Pulling ", 8) == 0)
		snprintf(out, cap, "Pulled %s", label + 8);
	else if (strncmp(label, "Fetching ", 9) == 0)
		snprintf(out, cap, "Fetched %s", label + 9);
	else
		snprintf(out, cap, "Finished %s", label);
}

// quiet_ok: routine work (a metadata fetch) leaves no line when it goes fine.
static int tool_ok_bar_q(char* const argv[], const char* what,
			 const char* label, bool quiet_ok) {
	tool_res_t res;
	fp_bar_t pb = fp_bar_init(FPKG_MODULE, "", label, 100);
	pull_bar_t s = {&pb, 0};
	fp_bar_update(&pb, 0);
	if (!fp_bar_pinned(&pb)) fpkg_infof("Started %s", label);
	if (ostree_run_cb(argv, &res, transfer_progress_cb, &s, pull_bar_tick,
			  &s) != 0) {
		fp_bar_finishf(&pb, 0, "failed");
		fpkg_errorf("%s: could not run the ostree tool", what);
		tool_res_free(&res);
		return -1;
	}
	if (res.rc != 0) {
		fp_bar_finishf(&pb, 0, "failed");
		log_tool_stderr(&res);
		char* err = res.err;
		if (err && *err) {
			trim(err);
			char* last = strrchr(err, '\n');
			fpkg_errorf("%s failed: %s", what,
				    last ? last + 1 : err);
		} else {
			fpkg_errorf("%s failed (rc %d)", what, res.rc);
		}
		tool_res_free(&res);
		return -1;
	}
	if (quiet_ok) fp_bar_dismiss(&pb);
	else {
		char done[700];
		pull_done_msg(label, done, sizeof(done));
		fp_bar_finishf(&pb, 1, "%s", done);
	}
	tool_res_free(&res);
	return 0;
}

static int tool_ok_bar(char* const argv[], const char* what,
		       const char* label) {
	return tool_ok_bar_q(argv, what, label, false);
}

static void spinner_tick_cb(void* arg) { fp_spin_tick((fp_spin_t*)arg); }

static int tool_ok_spin(char* const argv[], const char* what,
			const char* label) {
	tool_res_t res;
	fp_spin_t sp = fp_spin_init(FPKG_MODULE, "", label);
	// The spinner line is replaced by its result, so on a terminal there is
	// nothing for a "started" line to add; a log still wants it.
	if (!fp_is_tty()) fpkg_infof("Started %s", label);
	if (ostree_run_cb(argv, &res, NULL, NULL, spinner_tick_cb, &sp) != 0) {
		fp_spin_finishf(&sp, 0, "failed");
		fpkg_errorf("%s: could not run the ostree tool", what);
		tool_res_free(&res);
		return -1;
	}
	if (res.rc != 0) {
		fp_spin_finishf(&sp, 0, "failed");
		log_tool_stderr(&res);
		char* err = res.err;
		if (err && *err) {
			trim(err);
			char* last = strrchr(err, '\n');
			fpkg_errorf("%s failed: %s", what,
				    last ? last + 1 : err);
		} else {
			fpkg_errorf("%s failed (rc %d)", what, res.rc);
		}
		tool_res_free(&res);
		return -1;
	}
	// Inside a transaction the operation's own result line says it all.
	if (flatpkg_txn_active()) fp_spin_dismiss(&sp);
	else
		fp_spin_finishf(&sp, 1, "Finished %s", label);
	tool_res_free(&res);
	return 0;
}

static int tool_ok(char* const argv[], const char* what) {
	assert(argv != NULL);
	assert(what != NULL);
	tool_res_t res;
	if (ostree_run(argv, &res) != 0) {
		fpkg_errorf("%s: could not run the ostree tool", what);
		tool_res_free(&res);
		return -1;
	}
	if (res.rc != 0) {
		log_tool_stderr(&res);
		char* err = res.err;
		if (err && *err) {
			trim(err);
			char* last = strrchr(err, '\n');
			fpkg_errorf("%s failed: %s", what,
				    last ? last + 1 : err);
		} else {
			fpkg_errorf("%s failed (rc %d)", what, res.rc);
		}
		tool_res_free(&res);
		return -1;
	}
	tool_res_free(&res);
	return 0;
}

static int ensure_repo(void) {
	char repo[PATH_MAX];
	store_path(repo, sizeof(repo), "repo");
	if (mkdir_p(repo, 0755) != 0) {
		fpkg_errorf("mkdir %s: %s", repo, strerror(errno));
		return -1;
	}
	char config[PATH_MAX];
	snprintf(config, sizeof(config), "%s/config", repo);
	if (access(config, F_OK) == 0) return 0;

	tool_ctx_t tc;
	tool_ctx_init(&tc);
	char repo_arg[PATH_MAX + 32];
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);
	char* argv[] = {"ostree", "init", "--mode=bare-user-only", repo_arg,
			NULL};
	return tool_ok(argv, "ostree init");
}

#include <pthread.h>

static void flatpak_ref_header(const char* refstr, char* out, size_t outlen);

typedef struct {
	char* remote;
	char* refstr;
	char* label;
	int rc;
} pull_job_t;

static void* pull_thread(void* arg) {
	pull_job_t* job = arg;
	tool_ctx_t tc;
	tool_ctx_init(&tc);
	char repo_arg[PATH_MAX + 32];
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);
	char refhdr[600];
	flatpak_ref_header(job->refstr, refhdr, sizeof(refhdr));
	char* argv[] = {"ostree",
			"pull",
			"--disable-fsync",
			(char*)job->remote,
			(char*)job->refstr,
			repo_arg,
			refhdr,
			NULL};

	tool_res_t res;
	fp_bar_t pb = fp_bar_init(FPKG_MODULE, "", job->label, 100);
	pull_bar_t s = {&pb, 0};
	fp_bar_update(&pb, 0);
	if (!fp_bar_pinned(&pb)) fpkg_infof("Started %s", job->label);
	if (ostree_run_cb(argv, &res, transfer_progress_cb, &s, pull_bar_tick,
			  &s) != 0) {
		fp_bar_finishf(&pb, 0, "failed");
		fpkg_errorf("%s: could not run the ostree tool", job->label);
		job->rc = -1;
		return NULL;
	}
	if (res.rc != 0) {
		fp_bar_finishf(&pb, 0, "failed");
		log_tool_stderr(&res);
		char* err = res.err;
		if (err && *err) {
			trim(err);
			char* last = strrchr(err, '\n');
			fpkg_errorf("%s failed: %s", job->label,
				    last ? last + 1 : err);
		} else {
			fpkg_errorf("%s failed (rc %d)", job->label, res.rc);
		}
		job->rc = -1;
	} else {
		char done[700];
		pull_done_msg(job->label, done, sizeof(done));
		fp_bar_finishf(&pb, 1, "%s", done);
		job->rc = 0;
	}
	tool_res_free(&res);
	return NULL;
}

#define PULL_WORKERS 6

typedef struct {
	pull_job_t* jobs;
	size_t n;
	size_t next;
	pthread_mutex_t mu;
	// Overall-bar tick: called once per finished job (with the pool's
	// lock held) as "pulled: k/n". NULL when no transaction drives it.
	flatpkg_txn_t* txn;
	size_t pulled;
} pull_pool_t;

// Jobs are handed out one at a time, so at most PULL_WORKERS pulls (and bars)
// run at once however many refs a plan has.
static void* pull_worker(void* arg) {
	pull_pool_t* pool = arg;

	for (;;) {
		pthread_mutex_lock(&pool->mu);
		size_t i = pool->next < pool->n ? pool->next++ : pool->n;
		pthread_mutex_unlock(&pool->mu);
		if (i >= pool->n) break;
		pull_thread(&pool->jobs[i]);
		// Tick the transaction's overall bar once per ref that has
		// finished pulling. Nothing else advances it: the checkout
		// loop only starts after every pull is done.
		if (pool->txn != NULL) {
			pthread_mutex_lock(&pool->mu);
			flatpkg_txn_pull_progress(pool->txn, ++pool->pulled);
			pthread_mutex_unlock(&pool->mu);
		}
	}
	return NULL;
}

static int parallel_pull(pull_job_t* jobs, size_t n, flatpkg_txn_t* txn) {
	assert(jobs != NULL || n == 0);
	if (n == 0) return 0;
	if (txn != NULL) flatpkg_txn_pull_begin(txn, n);
	if (n == 1) {
		pull_thread(&jobs[0]);
		if (txn != NULL) flatpkg_txn_pull_progress(txn, 1);
		goto done;
	}
	pull_pool_t pool = {jobs, n, 0, PTHREAD_MUTEX_INITIALIZER, txn, 0};
	pthread_t tids[PULL_WORKERS];
	size_t nworkers = n < PULL_WORKERS ? n : PULL_WORKERS, started = 0;
	for (size_t i = 0; i < nworkers; i++) {
		if (pthread_create(&tids[i], NULL, pull_worker, &pool) != 0) {
			fpkg_warnf("pthread_create failed; pulling the rest "
				   "one at a time");
			break;
		}
		started++;
	}
	// Whatever no worker could take is pulled here.
	if (started < nworkers) (void)pull_worker(&pool);
	for (size_t i = 0; i < started; i++) pthread_join(tids[i], NULL);
	{
		int rc = 0;
		for (size_t i = 0; i < n; i++)
			if (jobs[i].rc != 0) rc = -1;
		if (txn != NULL) flatpkg_txn_pull_end(txn);
		return rc;
	}
done:
	if (txn != NULL) flatpkg_txn_pull_end(txn);
	return jobs[0].rc;
}

// "app/org.x.App/x86_64/stable" -> "org.x.App:stable".
static const char* pull_name(const char* refstr) {
	static __thread char out[320];
	char type[32], id[256], arch[64], branch[64];

	if (sscanf(refstr, "%31[^/]/%255[^/]/%63[^/]/%63s", type, id, arch,
		   branch) != 4)
		return refstr;
	snprintf(out, sizeof(out), "%s:%s", id, branch);
	return out;
}

static void pull_job_init(pull_job_t* job, const char* remote,
			  const char* refstr) {
	assert(job != NULL);
	assert(remote != NULL);
	job->remote = xstrdup(remote);
	job->refstr = xstrdup(refstr);
	char lbl[640];
	snprintf(lbl, sizeof(lbl), "Pulling %s", pull_name(refstr));
	job->label = xstrdup(lbl);
	job->rc = -1;
}

static void pull_jobs_free(pull_job_t* jobs, size_t n) {
	assert(jobs != NULL || n == 0);
	for (size_t i = 0; i < n; i++) {
		free(jobs[i].remote);
		free(jobs[i].refstr);
		free(jobs[i].label);
		jobs[i].remote = jobs[i].refstr = jobs[i].label = NULL;
	}
}

// Flathub's CDN refuses user agents containing "libostree/" on cache misses
// unless the request carries flatpak's own header.
static void flatpak_ref_header(const char* refstr, char* out, size_t outlen) {
	assert(refstr != NULL);
	assert(out != NULL);
	assert(outlen > 0);
	snprintf(out, outlen, "--http-header=Flatpak-Ref=%s", refstr);
}

// Refs a parallel prefetch already pulled: the sequential install that
// follows must not spawn a second pull for each of them.
static char** g_prefetched;
static size_t g_nprefetched;

static void prefetched_add(const char* refstr) {
	char** grown = realloc(g_prefetched,
			       (g_nprefetched + 1) * sizeof(*g_prefetched));
	if (!grown) return;
	g_prefetched = grown;
	g_prefetched[g_nprefetched++] = xstrdup(refstr);
}

static bool prefetched_take(const char* refstr) {
	for (size_t i = 0; i < g_nprefetched; i++) {
		if (strcmp(g_prefetched[i], refstr) != 0) continue;
		free(g_prefetched[i]);
		g_prefetched[i] = g_prefetched[--g_nprefetched];
		return true;
	}
	return false;
}

static void prefetched_clear(void) {
	for (size_t i = 0; i < g_nprefetched; i++) free(g_prefetched[i]);
	free(g_prefetched);
	g_prefetched = NULL;
	g_nprefetched = 0;
}

static int tool_pull(const char* remote, const char* refstr) {
	assert(remote != NULL);
	assert(refstr != NULL);
	if (prefetched_take(refstr)) return 0;
	tool_ctx_t tc;
	tool_ctx_init(&tc);
	char repo_arg[PATH_MAX + 32];
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);
	char refhdr[600];
	flatpak_ref_header(refstr, refhdr, sizeof(refhdr));
	char* argv[] = {"ostree",      "pull",	      "--disable-fsync",
			(char*)remote, (char*)refstr, repo_arg,
			refhdr,	       NULL};
	char label[640];
	snprintf(label, sizeof(label), "Pulling %s", pull_name(refstr));
	return tool_ok_bar(argv, "ostree pull", label);
}

// --subpath only descends into directories: given the "metadata" file it
// pulls the whole runtime object by object, with no static delta.  The
// commit object already carries the keyfile (xa.metadata), so stop there.
static int tool_pull_metadata(const char* remote, const char* refstr) {
	tool_ctx_t tc;
	tool_ctx_init(&tc);
	char repo_arg[PATH_MAX + 32];
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);
	char refhdr[600];
	flatpak_ref_header(refstr, refhdr, sizeof(refhdr));
	char* argv[] = {
		"ostree",	   "pull",	  "--commit-metadata-only",
		"--disable-fsync", (char*)remote, (char*)refstr,
		repo_arg,	   refhdr,	  NULL};
	char label[640];
	snprintf(label, sizeof(label), "fetching %s metadata", refstr);
	return tool_ok_bar_q(argv, "ostree pull", label, true);
}

// A ref the repo does not have yet is an ordinary answer for a probe (the
// caller pulls it next), so only the noisy form reports a miss.
static int tool_rev_parse_impl(const tool_ctx_t* tc, const char* refstr,
			       char* commit_out, size_t len, bool quiet) {
	char repo_arg[PATH_MAX + 32];
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc->repo);
	char* argv[] = {"ostree", "rev-parse", repo_arg, (char*)refstr, NULL};
	tool_res_t res;
	if (ostree_run(argv, &res) != 0) {
		tool_res_free(&res);
		return -1;
	}
	if (res.rc != 0) {
		if (!quiet) {
			log_tool_stderr(&res);
			fpkg_errorf("ostree rev-parse %s failed", refstr);
		}
		tool_res_free(&res);
		return -1;
	}
	if (!res.out || !*res.out) {
		fpkg_errorf("ostree rev-parse %s: no output", refstr);
		tool_res_free(&res);
		return -1;
	}
	trim(res.out);
	snprintf(commit_out, len, "%s", res.out);
	tool_res_free(&res);
	return 0;
}

static int tool_rev_parse(const tool_ctx_t* tc, const char* refstr,
			  char* commit_out, size_t len) {
	return tool_rev_parse_impl(tc, refstr, commit_out, len, false);
}

static int tool_rev_probe(const tool_ctx_t* tc, const char* refstr,
			  char* commit_out, size_t len) {
	return tool_rev_parse_impl(tc, refstr, commit_out, len, true);
}

static int deploy_checkout(const tool_ctx_t* tc, const char* type,
			   const fpkg_ref_t* ref, const char* commit,
			   char* deploy_out, size_t outlen) {
	char host_deploy[PATH_MAX];
	snprintf(host_deploy, sizeof(host_deploy), "%s/%s/%s/%s/%s/%s",
		 store_root(), type, ref->id, ref->arch,
		 ref->branch ? ref->branch : "", commit);

	char parent[PATH_MAX];
	snprintf(parent, sizeof(parent), "%s", host_deploy);
	char* slash = strrchr(parent, '/');
	if (slash) *slash = '\0';
	if (mkdir_p(parent, 0755) != 0) {
		fpkg_errorf("mkdir %s: %s", parent, strerror(errno));
		return -1;
	}
	rm_rf(host_deploy);

	char repo_arg[PATH_MAX + 32];
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc->repo);
	char jail_deploy[PATH_MAX];
	snprintf(jail_deploy, sizeof(jail_deploy), "/ostree/%s/%s/%s/%s/%s",
		 type, ref->id, ref->arch, ref->branch ? ref->branch : "",
		 commit);
	char* argv[] = {"ostree",      "checkout",     "-U",
			"-M",	       "--fsync=none", repo_arg,
			(char*)commit, jail_deploy,    NULL};
	// -U = OSTREE_REPO_CHECKOUT_MODE_USER: skip the chown/xattr init
	// that, as root, would make the deploy unreadable by the app.
	char label[320];
	snprintf(label, sizeof(label), "Checking out %s", ref->id);
	if (tool_ok_spin(argv, "ostree checkout", label) != 0) return -1;

	if (deploy_out) snprintf(deploy_out, outlen, "%s", host_deploy);
	return 0;
}

static int make_active(const char* deploy_host_path) {
	char branch_dir[PATH_MAX];
	snprintf(branch_dir, sizeof(branch_dir), "%s", deploy_host_path);
	char* slash = strrchr(branch_dir, '/');
	if (!slash) return -1;
	*slash = '\0';
	char commit[256];
	snprintf(commit, sizeof(commit), "%s", slash + 1);

	char active_link[PATH_MAX];
	snprintf(active_link, sizeof(active_link), "%s/active", branch_dir);
	if (atomic_symlink(commit, active_link) != 0) {
		fpkg_errorf("symlink %s -> %s: %s", active_link, commit,
			    strerror(errno));
		return -1;
	}

	{
		char arch_dir[PATH_MAX];
		snprintf(arch_dir, sizeof(arch_dir), "%s", branch_dir);
		slash = strrchr(arch_dir, '/');
		if (slash) {
			*slash = '\0';
			const char* branch = strrchr(branch_dir, '/') + 1;
			char* arch = strrchr(arch_dir, '/') + 1;
			*strrchr(arch_dir, '/') = '\0';
			char current_link[PATH_MAX];
			snprintf(current_link, sizeof(current_link),
				 "%s/current", arch_dir);
			char target[256];
			snprintf(target, sizeof(target), "%s/%s", arch, branch);
			unlink(current_link);
			if (symlink(target, current_link) != 0)
				fpkg_warnf("symlink %s -> %s: %s", current_link,
					   target, strerror(errno));
		}
	}

	// <deploy>/files/.ref: flatpak's marker that this deploy came from a
	// ref. It is empty by design; only its presence matters.
	{
		char ref_marker[PATH_MAX];
		snprintf(ref_marker, sizeof(ref_marker), "%s/files/.ref",
			 deploy_host_path);
		(void)write_file(ref_marker, "", 0644);
	}
	return 0;
}

static void write_origin(const char* deploy_host_path, const char* remote) {
	char origin[PATH_MAX];
	snprintf(origin, sizeof(origin), "%s", deploy_host_path);
	char* slash = strrchr(origin, '/');
	if (!slash) return;
	*slash = '\0';
	{
		char with_origin[PATH_MAX];
		snprintf(with_origin, sizeof(with_origin), "%s/.origin",
			 origin);
		write_file(with_origin, remote, 0644);
	}
}

static bool deploy_exists(const char* type, const char* id) {
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s/%s", store_root(), type, id);
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int find_deploy_dir(const char* type, const fpkg_ref_t* r, char* out,
			   size_t len);

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

static void write_deploy_data(const char* branch_dir, const char* commit,
			      const char* origin, char** subpaths,
			      size_t nsubpaths);

static bool deploy_branch_dir(const char* deploy, char* out, size_t outlen) {
	char tmp[PATH_MAX];

	snprintf(tmp, sizeof(tmp), "%s", deploy);
	char* slash = strrchr(tmp, '/');
	if (slash == NULL) return false;
	*slash = '\0';
	snprintf(out, outlen, "%s", tmp);
	return true;
}

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

static int install_one(const fpkg_ref_t* ref, const char* pull_type,
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

int confirm_proceed(int assume_yes) {
	if (assume_yes) return 0;
	// FLATPKG_INTERACTIVE says a terminal exists at the far end, but the
	// prompt reads a key from a tty on our own stdin and otherwise falls
	// back to its default, which is "yes": never answer for the user.
	if (!fpkg_interactive() || !isatty(STDIN_FILENO)) {
		fpkg_errorf("no terminal attached; pass -y to proceed");
		return 1;
	}
	return fp_prompt_confirm_default(
		       "Proceed with these changes to the system installation?",
		       true)
		       ? 0
		       : 1;
}

static bool g_fetch_quiet;

// Writes the local filesystem path of a file:// remote into out.
static bool remote_local_path(const char* remote, char* out, size_t len) {
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

static char** fetch_remote_refs(const char* remote, size_t* n_out,
				fp_spin_t* sp) {
	assert(remote != NULL);
	assert(n_out != NULL);
	*n_out = 0;

	char local_repo[PATH_MAX];
	bool is_local =
		remote_local_path(remote, local_repo, sizeof(local_repo));

	tool_ctx_t tc;
	tool_ctx_init(&tc);
	char repo_arg[PATH_MAX + 32];
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);
	char* argv[8];
	int nargv = 0;
	if (is_local) {
		// A local repo has no summary file, so "ostree remote refs"
		// cannot list it; read the refs out of the repo itself.
		argv[nargv++] = "ostree";
		argv[nargv++] = "refs";
		argv[nargv++] = "--repo";
		argv[nargv++] = local_repo;
	} else {
		argv[nargv++] = "ostree";
		argv[nargv++] = "remote";
		argv[nargv++] = "refs";
		argv[nargv++] = repo_arg;
		argv[nargv++] = (char*)remote;
	}
	argv[nargv] = NULL;

	tool_res_t res;
	if (ostree_run_cb(argv, &res, NULL, NULL, sp ? spinner_tick_cb : NULL,
			  sp ? (void*)sp : NULL) != 0 ||
	    res.rc != 0 || !res.out) {
		if (sp) fp_spin_finishf(sp, 0, "fetch failed");
		if (!g_fetch_quiet)
			fpkg_errorf("cannot fetch refs for remote '%s'",
				    remote);
		tool_res_free(&res);
		return NULL;
	}
	if (sp) fp_spin_finishf(sp, 1, "fetched refs");

	char** refs = NULL;
	size_t n = 0, cap = 0;

#define PUSH_REF(s)                                                            \
	do {                                                                   \
		if (n == cap) {                                                \
			cap = cap ? cap * 2 : 32;                              \
			char** new_refs = realloc(refs, cap * sizeof(char*));  \
			if (!new_refs) {                                       \
				fpkg_errorf("out of memory");                  \
				exit(1);                                       \
			}                                                      \
			refs = new_refs;                                       \
		}                                                              \
		refs[n++] = xstrdup(s);                                        \
	} while (0)

	char* save = NULL;
	for (char* line = strtok_r(res.out, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		// "ostree remote refs" prints "flathub:app/name/arch/branch"; a local
		// repo's own refs carry no prefix, and stripping one there would eat
		// the part of the name before the first slash.
		if (!is_local) {
			char* colon = strchr(line, ':');
			if (colon) {
				char* slash = strchr(line, '/');
				if (!slash || colon < slash) line = colon + 1;
			}
		}
		PUSH_REF(line);
	}
#undef PUSH_REF

	tool_res_free(&res);
	*n_out = n;
	return refs;
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

typedef struct {
	char* remote;
	char** refs;
	size_t n;
	bool failed;
	bool warned;
	time_t at;
} remote_refs_t;

static remote_refs_t* g_rrefs;
static size_t g_nrrefs;

static void remote_refs_drop(remote_refs_t* e) {
	for (size_t k = 0; k < e->n; k++) free(e->refs[k]);
	free(e->refs);
	e->refs = NULL;
	e->n = 0;
}

static remote_refs_t* remote_refs_cached(const char* remote,
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

typedef struct {
	char* ref;
	char* id;
	char* arch;
	char* branch;
	bool runtime;
} install_match_t;

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
		if (runtime_only && !is_runtime) continue;

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

// Join a strlist into a heap-allocated ", "-separated string; NULL if empty.
static char* perms_join(const strlist_t* l) {
	if (!l || !l->count) return NULL;
	size_t sz = 1;
	for (size_t i = 0; i < l->count; i++) sz += strlen(l->items[i]) + 2;
	char* s = malloc(sz);
	if (!s) return NULL;
	size_t off = 0;
	for (size_t i = 0; i < l->count; i++) {
		if (i) off += (size_t)snprintf(s + off, sz - off, ", ");
		off += (size_t)snprintf(s + off, sz - off, "%s", l->items[i]);
	}
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
		if (!first) off += (size_t)snprintf(s + off, sz - off, ", ");
		off += (size_t)snprintf(s + off, sz - off, "%s", a->items[i]);
		first = false;
	}
	for (size_t i = 0; i < nb; i++) {
		if (!first) off += (size_t)snprintf(s + off, sz - off, ", ");
		off += (size_t)snprintf(s + off, sz - off, "%s", b->items[i]);
		first = false;
	}
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
	char* kvals[8];
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
	char*** rows = calloc(np, sizeof(char**));
	char** cells = calloc(np * 2, sizeof(char*));
	if (!rows || !cells) {
		for (size_t i = 0; i < np; i++) free(kvals[i]);
		free(rows);
		free(cells);
		return;
	}
	for (size_t i = 0; i < np; i++) {
		rows[i] = &cells[i * 2];
		cells[i * 2 + 0] = xstrdup(klabels[i]);
		cells[i * 2 + 1] = kvals[i];
	}
	fp_print_table(headers, 2, (const char* const* const*)rows, np,
		       FP_TABLE_OUTPUT);
	for (size_t i = 0; i < np * 2; i++) free(cells[i]);
	free(rows);
	free(cells);
}

void changeset_show_full(const flatpkg_changeset_t* cs, const char* remote) {
	assert(cs != NULL);

	const char* headers[] = {"ID", "Arch", "Branch", "Operation", "Remote"};
	char*** rows = NULL;
	char** cells = NULL;
	size_t nrows = cs->count;
	size_t nr = 0;

	rows = calloc(nrows, sizeof(char**));
	cells = calloc(nrows * 5, sizeof(char*));
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

		rows[nr] = &cells[nr * 5];
		cells[nr * 5 + 0] = xstrdup(r.id);
		cells[nr * 5 + 1] = xstrdup(r.arch ? r.arch : "");
		cells[nr * 5 + 2] = xstrdup(r.branch ? r.branch : "");
		cells[nr * 5 + 3] = xstrdup(op);
		cells[nr * 5 + 4] = xstrdup(remote ? remote : "-");
		nr++;
		ref_free(&r);
	}

	if (nr > 0) {
		fp_print_table(headers, 5, (const char* const* const*)rows, nr,
			       FP_TABLE_OUTPUT);
	}

	for (size_t i = 0; i < nr * 5; i++) free(cells[i]);
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
// refs), so this is asked per candidate.  Looked up once, and via sysctl(3)
// because a popen(sysctl) per candidate cost more than the whole download.
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

// Once per process, like the GL driver: every Gtk3theme candidate asks.
static const char* active_gtk_theme(void) {
	static char theme[128];
	static bool done;
	if (done) return theme;
	done = true;
	FILE* p = popen("/usr/local/bin/dconf read "
			"/org/gnome/desktop/interface/gtk-theme 2>/dev/null",
			"r");
	if (!p) return theme;
	char raw[128] = "";
	if (fgets(raw, sizeof(raw), p)) {
		raw[strcspn(raw, "\n")] = 0;
		char* q = strchr(raw, '\'');
		if (q++) {
			char* e = strchr(q, '\'');
			if (e) *e = '\0';
			snprintf(theme, sizeof(theme), "%s", q);
		}
	}
	pclose(p);
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

static void related_refs_for_metadata(const fp_metadata_t* meta,
				      const char* parent_branch,
				      const char* arch, char** srefs,
				      size_t nsrefs, fpkg_ref_t** out,
				      size_t* nout);

static void install_extensions(const char* remote, const fpkg_ref_t* rt,
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

// The keyfile a flatpak commit carries as xa.metadata.  "ostree show" prints
// the value as GVariant text.
static char* commit_xa_metadata(const char* commit) {
	tool_ctx_t tc;
	tool_ctx_init(&tc);
	char repo_arg[PATH_MAX + 32];
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);
	char* argv[] = {
		"ostree", "show",	 "--print-metadata-key=xa.metadata",
		repo_arg, (char*)commit, NULL};
	tool_res_t res;
	char* body = NULL;
	if (ostree_run(argv, &res) == 0 && res.rc == 0 && res.out && *res.out) {
		GVariant* v = g_variant_parse(G_VARIANT_TYPE_STRING, res.out,
					      NULL, NULL, NULL);
		if (v) {
			body = xstrdup(g_variant_get_string(v, NULL));
			g_variant_unref(v);
		}
	}
	tool_res_free(&res);
	return body;
}

// Commits without xa.metadata (hand-made repos) still hold the file in the
// tree, which is only readable once the whole commit is pulled.
static char* cat_commit_metadata(const char* commit) {
	tool_ctx_t tc;
	tool_ctx_init(&tc);
	char repo_arg[PATH_MAX + 32];
	snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", tc.repo);
	char* argv[] = {"ostree",      "cat",	   repo_arg,
			(char*)commit, "metadata", NULL};
	tool_res_t res;
	if (ostree_run(argv, &res) != 0) {
		tool_res_free(&res);
		return NULL;
	}
	if (res.rc != 0) {
		log_tool_stderr(&res);
		fpkg_errorf("ostree cat %s metadata failed", commit);
		tool_res_free(&res);
		return NULL;
	}
	if (!res.out || !*res.out) {
		fpkg_errorf("ostree cat %s metadata: no output", commit);
		tool_res_free(&res);
		return NULL;
	}
	char* body = xstrdup(res.out);
	tool_res_free(&res);
	return body;
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
		unlink(tmpl);
		fpkg_errorf("write %s: %s", tmpl, strerror(errno));
		return -1;
	}
	// Runtimes use [Runtime] (no name=/command=), so parse them leniently
	// or their metadata is rejected and the deploy skipped.
	int rc = fp_metadata_parse_any(tmpl, out);
	unlink(tmpl);
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

static char* summary_ref_metadata_text(const char* remote,
				       const char* refstr);

// Fetch the metadata text and commit for one app ref for a caller that
// plans outside the daemon: this runs as root and only reads back
// what it fetched, since the caller cannot write the repo.
int ostree_fetch_ref_metadata(const char* remote, const char* request,
			      char** commit_out, char** meta_out) {
	assert(request != NULL);
	assert(commit_out != NULL);
	assert(meta_out != NULL);
	*commit_out = NULL;
	*meta_out = NULL;

	fpkg_ref_t r;
	if (ref_parse(request, &r) != 0) return -1;
	const char* kind = strncmp(request, "runtime/", 8) == 0 ? "runtime"
			   : strncmp(request, "extension/", 10) == 0
			       ? "extension"
			       : "app";
	char refstr[512];
	ref_full(&r, kind, refstr, sizeof(refstr));

	char* rem = NULL;
	if (remote != NULL && *remote != '\0') {
		rem = xstrdup(remote);
	} else if (r.remote != NULL) {
		rem = xstrdup(r.remote);
	} else {
		rem = first_remote();
	}
	if (rem == NULL) {
		fpkg_errorf("no remotes configured");
		ref_free(&r);
		return -1;
	}

	char* body = NULL;
	char mcommit[256] = "";
	if (strcmp(kind, "app") == 0 || strcmp(kind, "runtime") == 0)
		body = summary_ref_metadata_text(rem, refstr);
	tool_ctx_t tc;
	tool_ctx_init(&tc);
	if (tool_rev_parse(&tc, refstr, mcommit, sizeof(mcommit)) != 0) {
		// The summary has what a plan needs; a commit is optional.  If
		// there is no summary entry either, pull it as root.
		if (body == NULL) {
			char repo_arg[PATH_MAX + 32];
			snprintf(repo_arg, sizeof(repo_arg), "--repo=%s",
				 tc.repo);
			char refhdr[600];
			flatpak_ref_header(refstr, refhdr, sizeof(refhdr));
			char* argv[] = {"ostree", "pull", "--disable-fsync", rem,
					refstr, repo_arg, refhdr, NULL};
			tool_res_t res;
			if (ostree_run(argv, &res) != 0 || res.rc != 0) {
				tool_res_free(&res);
				free(rem);
				ref_free(&r);
				return -1;
			}
			tool_res_free(&res);
			(void)tool_rev_parse(&tc, refstr, mcommit,
					     sizeof(mcommit));
		}
	}
	if (body == NULL && mcommit[0] != '\0')
		body = commit_xa_metadata(mcommit);
	if (body == NULL && mcommit[0] != '\0')
		body = cat_commit_metadata(mcommit);
	if (body == NULL && strcmp(kind, "extension") != 0) {
		free(rem);
		ref_free(&r);
		return -1;
	}
	*commit_out = xstrdup(mcommit);
	*meta_out = body != NULL ? body : xstrdup("");
	free(rem);
	ref_free(&r);
	return 0;
}

// Decompress a zlib-format buffer (with 2-byte header and Adler-32 trailer)
// into a NUL-terminated string. Returns NULL on error; caller frees the result.
static char* zlib_decompress_str(const guint8* src, gsize srclen) {
	if (srclen == 0) return NULL;
	// Start with a 4x estimate; grow if needed (up to 8 doublings).
	uLongf dstlen = (uLongf)(srclen * 4 + 128);
	for (int tries = 0; tries < 8; tries++) {
		char* buf = malloc(dstlen + 1);
		if (!buf) return NULL;
		uLongf got = dstlen;
		int rc = uncompress((Bytef*)buf, &got, src, (uLong)srclen);
		if (rc == Z_OK) {
			buf[got] = '\0';
			return buf;
		}
		free(buf);
		if (rc != Z_BUF_ERROR) return NULL; // real error
		dstlen *= 4;
	}
	return NULL;
}

// The summary's xa.cache maps each ref to its metadata keyfile, so a plan
// needs no commit fetch, and libostree checks it against the remote's
// signature: v1 a{s(tts)}, v2 a{s(ttay)} (deflated), told apart by type.
static char* summary_xa_metadata(const char* remote, const char* refstr) {
	if (!remote_name_valid(remote)) return NULL;
	char path[PATH_MAX];
	store_path(path, sizeof(path), "repo/tmp/cache/summaries/%s", remote);
	GMappedFile* mf = g_mapped_file_new(path, FALSE, NULL);
	if (!mf) return NULL;
	GBytes* bytes = g_mapped_file_get_bytes(mf);
	g_mapped_file_unref(mf);
	// Untrusted: the cache directory is group-writable.
	GVariant* sum = g_variant_new_from_bytes(
		G_VARIANT_TYPE("(a(s(taya{sv}))a{sv})"), bytes, FALSE);
	g_bytes_unref(bytes);
	GVariant* meta = g_variant_get_child_value(sum, 1);
	// The typed lookup does not unbox a{sv} values here, so do it by hand.
	GVariant* boxed = g_variant_lookup_value(meta, "xa.cache", NULL);
	GVariant* cache =
		boxed && g_variant_is_of_type(boxed, G_VARIANT_TYPE_VARIANT)
			? g_variant_get_variant(boxed)
			: NULL;
	if (boxed) g_variant_unref(boxed);

	char* body = NULL;

	if (cache && g_variant_is_of_type(cache, G_VARIANT_TYPE("a{s(tts)}"))) {
		GVariant* ent = g_variant_lookup_value(cache, refstr,
						       G_VARIANT_TYPE("(tts)"));
		if (ent) {
			const char* m = NULL;
			g_variant_get_child(ent, 2, "&s", &m);
			if (m && *m) body = xstrdup(m);
			g_variant_unref(ent);
		}
	} else if (cache &&
		   g_variant_is_of_type(cache, G_VARIANT_TYPE("a{s(ttay)}"))) {
		GVariant* ent = g_variant_lookup_value(
			cache, refstr, G_VARIANT_TYPE("(ttay)"));
		if (ent) {
			GVariant* cv = g_variant_get_child_value(ent, 2);
			gsize clen = 0;
			const guint8* cdata =
				g_variant_get_fixed_array(cv, &clen, 1);
			if (cdata && clen > 0)
				body = zlib_decompress_str(cdata, clen);
			g_variant_unref(cv);
			g_variant_unref(ent);
		}
	}

	if (cache) g_variant_unref(cache);
	g_variant_unref(meta);
	g_variant_unref(sum);
	return body;
}

// Try the local summary cache first (no network) and refresh the
// summary, which may hit the network, only on a miss.
static char* summary_ref_metadata_text(const char* remote,
				       const char* refstr) {
	char* body = summary_xa_metadata(remote, refstr);
	if (body) return body;
	const remote_refs_t* rr =
		remote_refs_cached(remote, "Looking up metadata");
	if (!rr || rr->failed) return NULL;
	return summary_xa_metadata(remote, refstr);
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

static void uninstall_deploy(const char* deploy_type, const char* ref_kind,
			     const char* id);

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

// Pull every ref the plan installs side by side, one bar each. The
// installs that follow find them in the repo; whatever failed here is
// retried there.
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
		if (sscanf(c->ref, "%63[^/]/%255s", ctype, cid) != 2) continue;
		fpkg_ref_t ir;
		if (ref_parse(c->ref, &ir) != 0) continue;
		if (!ir.arch) ir.arch = xstrdup(host_arch());
		if (!ir.branch) ir.branch = xstrdup("stable");
		char refstr[512];
		ref_full(&ir, strcmp(ctype, "app") == 0 ? "app" : "runtime",
			 refstr, sizeof(refstr));
		pull_job_init(&jobs[n++], ir.remote ? ir.remote : remote,
			      refstr);
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
	(void)rt;
	for (size_t i = 0; i < cs->count; i++)
		if (cs->items[i].type == FP_CHANGE_INSTALL &&
		    cs->items[i].ref &&
		    strncmp(cs->items[i].ref, "extension/", 10) == 0)
			return true;
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
static size_t expand_runtime_extensions(flatpkg_changeset_t* cs,
					const char* remote) {
	assert(cs != NULL);
	assert(remote != NULL);
	size_t added = 0;
	size_t orig = cs->count;
	for (size_t i = 0; i < orig; i++) {
		flatpkg_change_t* c = &cs->items[i];
		if (c->type != FP_CHANGE_INSTALL) continue;
		char ctype[64], cid[256];
		if (sscanf(c->ref, "%63[^/]/%255s", ctype, cid) != 2) continue;
		if (strcmp(ctype, "runtime") != 0) continue;
		fpkg_ref_t ir;
		if (ref_parse(c->ref, &ir) != 0) continue;
		if (!ir.arch) {
			free(ir.arch);
			ir.arch = xstrdup(host_arch());
		}
		if (!ir.branch) ir.branch = xstrdup("stable");
		const char* use_remote = ir.remote ? ir.remote : remote;
		char refstr[512];
		ref_full(&ir, "runtime", refstr, sizeof(refstr));
		fp_metadata_t rmeta;
		if (remote_ref_metadata(use_remote, refstr, &rmeta) != 0) {
			ref_free(&ir);
			continue;
		}
		remote_refs_t* rr =
			remote_refs_cached(use_remote, "Looking up extensions");
		if (!rr || rr->failed) {
			fp_metadata_free(&rmeta);
			ref_free(&ir);
			continue;
		}
		fpkg_ref_t* refs = NULL;
		size_t nrefs = 0;
		related_refs_for_metadata(&rmeta, ir.branch, ir.arch, rr->refs,
					  rr->n, &refs, &nrefs);
		fp_metadata_free(&rmeta);
		for (size_t k = 0; k < nrefs; k++) {
			char eref[512];
			snprintf(eref, sizeof(eref), "extension/%s/%s/%s",
				 refs[k].id, refs[k].arch,
				 refs[k].branch ? refs[k].branch : "");
			size_t before = cs->count;
			cs_add_install_once(cs, eref, refs[k].branch);
			if (cs->count != before) added++;
			ref_free(&refs[k]);
		}
		free(refs);
		ref_free(&ir);
	}
	return added;
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
		if (sscanf(c->ref, "%63[^/]/%255s", ctype, cid) != 2) {
			fpkg_warnf("cannot parse ref '%s'", c->ref);
			rc = -1;
			continue;
		}
		if (strcmp(ctype, "extension") == 0) {
			if (ostree_install_extension_ref(c->ref) != 0) {
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
				  : (use_remote ? xstrdup(use_remote) : NULL);
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

// One install request, resolved: the ref, its remote, and the metadata the
// solver needs for its runtime and extensions. A batch resolves every
// request first and solves once, so the table shows the whole change.
typedef struct {
	fpkg_ref_t ref;
	char* remote;
	flatpkg_dep_kind_t kind;
	const char* dep_type;
	bool is_runtime;
	bool is_ext;
	fp_metadata_t meta;
} install_req_t;

static void install_req_free(install_req_t* q) {
	assert(q != NULL);
	fp_metadata_free(&q->meta);
	free(q->remote);
	ref_free(&q->ref);
	memset(q, 0, sizeof(*q));
}

static const char* plan_meta_text(const char* ref);

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

typedef struct {
	char* ref;
	char* text;
} plan_meta_t;

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

void fpkg_plan_free(fpkg_plan_t* plan) {
	assert(plan != NULL);
	changeset_free(&plan->cs);
	world_free(&plan->world);
	free(plan->remote);
	memset(plan, 0, sizeof(*plan));
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
	for (size_t i = 0; i < n; i++)
		expand_runtime_extensions(&out->cs, reqs[i].remote);
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

// ref_kind is the ostree ref prefix ("app"/"runtime"), not the deploy type.
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

void idset_add(idset_t* s, const char* id) {
	assert(s != NULL);
	assert(id != NULL);
	for (size_t i = 0; i < s->n; i++)
		if (strcmp(s->v[i], id) == 0) return;
	if (s->n == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 16;
		s->v = realloc(s->v, s->cap * sizeof(char*));
		if (!s->v) {
			fpkg_errorf("out of memory");
			exit(1);
		}
	}
	s->v[s->n++] = xstrdup(id);
}

bool idset_has(const idset_t* s, const char* id) {
	assert(s != NULL);
	assert(id != NULL);
	for (size_t i = 0; i < s->n; i++)
		if (strcmp(s->v[i], id) == 0) return true;
	return false;
}

// Extensions are declared by prefix (org.freedesktop.Platform.GL covers
// the .default deploy), so membership is exact or a dot-boundary
// prefix.
static bool idset_matches(const idset_t* s, const char* id) {
	assert(s != NULL);
	assert(id != NULL);
	for (size_t i = 0; i < s->n; i++) {
		size_t el = strlen(s->v[i]);
		if (strncmp(s->v[i], id, el) != 0) continue;
		if (id[el] == '\0' || id[el] == '.') return true;
	}
	return false;
}

static void idset_free(idset_t* s) {
	for (size_t i = 0; i < s->n; i++) free(s->v[i]);
	free(s->v);
	memset(s, 0, sizeof(*s));
}

// <store>/<kind>/<id>/<arch>/<branch>/active/metadata, for whichever deploy
// of <id> is active.
static bool deploy_meta_path(const char* kind, const char* id, char* out,
			     size_t len) {
	assert(kind != NULL);
	assert(id != NULL);
	assert(out != NULL);
	char root[PATH_MAX];
	snprintf(root, sizeof(root), "%s/%s/%s", store_root(), kind, id);
	DIR* d2 = opendir(root);
	if (!d2) return false;
	struct dirent* aent;
	bool found = false;
	while (!found && (aent = readdir(d2)) != NULL) {
		if (aent->d_name[0] == '.') continue;
		char archdir[PATH_MAX];
		snprintf(archdir, sizeof(archdir), "%s/%s", root, aent->d_name);
		DIR* d3 = opendir(archdir);
		if (!d3) continue;
		struct dirent* bent;
		while (!found && (bent = readdir(d3)) != NULL) {
			if (bent->d_name[0] == '.') continue;
			char meta[PATH_MAX];
			snprintf(meta, sizeof(meta), "%s/%s/active/metadata",
				 archdir, bent->d_name);
			if (access(meta, F_OK) == 0) {
				snprintf(out, len, "%s", meta);
				found = true;
			}
		}
		closedir(d3);
	}
	closedir(d2);
	return found;
}

void closure_follow(idset_t* s, const char* id) {
	assert(s != NULL);
	assert(id != NULL);
	static const char* kinds[] = {"app", "runtime", NULL};
	for (int k = 0; kinds[k]; k++) {
		char meta[PATH_MAX];
		if (!deploy_meta_path(kinds[k], id, meta, sizeof(meta)))
			continue;
		fp_metadata_t m;
		if (fp_metadata_parse_any(meta, &m) != 0) continue;
		const char* refs[] = {m.runtime_ref, m.sdk_ref};
		for (int j = 0; j < 2; j++) {
			if (!refs[j] || !*refs[j]) continue;
			char rid[256];
			if (sscanf(refs[j], "%255[^/]", rid) != 1) continue;
			// org.freedesktop.Sdk's [Runtime] names itself as sdk.
			if (strcmp(rid, id) == 0) continue;
			if (!idset_matches(s, rid)) {
				idset_add(s, rid);
				closure_follow(s, rid);
			}
		}
		for (size_t g = 0; g < m.next_groups; g++) {
			const char* gid = m.ext_groups[g].id;
			if (!gid || !*gid) continue;
			if (!idset_matches(s, gid)) idset_add(s, gid);
		}
		fp_metadata_free(&m);
		return;
	}
}

idset_t g_closure;
static bool g_closure_ready = false;

void ostree_closure_reset(void) {
	idset_free(&g_closure);
	g_closure_ready = false;
}

void ostree_closure_mark_ready(void) { g_closure_ready = true; }

static void closure_build(void) {
	if (g_closure_ready) return;
	flatpkg_dep_array_t world;
	if (world_load(&world) != 0) {
		g_closure_ready = true;
		return;
	}
	for (size_t i = 0; i < world.count; i++) {
		const char* name = world.items[i].name;
		char id[256];
		// A world entry is a bare app id, or a full ref for something
		// installed explicitly (a runtime, a branch pin).
		if (sscanf(name, "%*[^/]/%255[^/]", id) != 1)
			snprintf(id, sizeof(id), "%s", name);
		if (idset_matches(&g_closure, id)) continue;
		idset_add(&g_closure, id);
		closure_follow(&g_closure, id);
	}
	world_free(&world);
	g_closure_ready = true;
}

bool ostree_deploy_referenced(const char* ref) {
	assert(ref != NULL);
	char type[64], id[256];
	if (sscanf(ref, "%63[^/]/%255[^/]", type, id) != 2)
		return true; // unknown shape: keep
	closure_build();
	if (strcmp(type, "app") == 0) return world_has(id) == 1;
	return idset_matches(&g_closure, id);
}

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

// One remove executor for changeset_apply().  The plan owns the world and
// the table; this does only the store work after confirmation.
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

static void uninstall_collect_reverse_deps(const char* ref_id,
					   flatpkg_dep_array_t* world,
					   flatpkg_dep_array_t* extras);

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

static int find_deploy_dir(const char* type, const fpkg_ref_t* r, char* out,
			   size_t len) {
	assert(type != NULL);
	assert(r != NULL);
	assert(out != NULL);
	assert(len > 0);
	if (r->branch && *r->branch) {
		snprintf(out, len, "%s/%s/%s/%s/%s", store_root(), type, r->id,
			 r->arch, r->branch);
		struct stat st;
		if (stat(out, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
		return -1;
	}
	char root[PATH_MAX];
	snprintf(root, sizeof(root), "%s/%s/%s", store_root(), type, r->id);
	DIR* d1 = opendir(root);
	if (!d1) return -1;
	struct dirent* arch_ent;
	while ((arch_ent = readdir(d1))) {
		if (arch_ent->d_name[0] == '.') continue;
		char archdir[PATH_MAX];
		snprintf(archdir, sizeof(archdir), "%s/%s", root,
			 arch_ent->d_name);
		DIR* d2 = opendir(archdir);
		if (!d2) continue;
		struct dirent* branch_ent;
		while ((branch_ent = readdir(d2))) {
			if (branch_ent->d_name[0] == '.') continue;
			char active[PATH_MAX];
			snprintf(active, sizeof(active), "%s/%s/active",
				 archdir, branch_ent->d_name);
			struct stat st;
			if (stat(active, &st) == 0) {
				snprintf(out, len, "%s/%s", archdir,
					 branch_ent->d_name);
				closedir(d2);
				closedir(d1);
				return 0;
			}
		}
		closedir(d2);
	}
	closedir(d1);
	return -1;
}

bool remote_name_valid(const char* name) {
	assert(name != NULL);
	if (name == NULL || *name == '\0' || name[0] == '.') return false;
	for (const char* c = name; *c; c++) {
		bool alnum = (*c >= '0' && *c <= '9') ||
			     (*c >= 'a' && *c <= 'z') ||
			     (*c >= 'A' && *c <= 'Z');
		if (!alnum && *c != '-' && *c != '_' && *c != '.') return false;
	}
	return true;
}

typedef struct {
	char name[256];
	time_t mtime;
} commit_dir_t;

// Is this name a commit directory? Commit hashes are 64-char SHA-256 hex.
static bool is_commit_dir_name(const char* name) {
	assert(name != NULL);
	size_t l = strlen(name);
	if (l < 32 || l >= 256) return false;
	for (size_t i = 0; i < l; i++) {
		char c = name[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	}
	return true;
}

static int collect_commit_dirs(const char* branch_dir, bool newest_first,
			       commit_dir_t** out) {
	assert(branch_dir != NULL);
	*out = NULL;
	DIR* bd = opendir(branch_dir);
	if (bd == NULL) return -1;

	commit_dir_t* entries = NULL;
	size_t nentries = 0, cap = 16;
	entries = calloc(cap, sizeof(*entries));
	if (entries == NULL) {
		closedir(bd);
		return -1;
	}

	struct dirent* de;
	while ((de = readdir(bd)) != NULL) {
		if (de->d_name[0] == '.') continue;
		if (strcmp(de->d_name, "active") == 0 ||
		    strcmp(de->d_name, ".origin") == 0 ||
		    strcmp(de->d_name, "current") == 0)
			continue;
		if (!is_commit_dir_name(de->d_name)) continue;
		char p[PATH_MAX];
		snprintf(p, sizeof(p), "%s/%s", branch_dir, de->d_name);
		struct stat st;
		if (stat(p, &st) != 0) continue;

		if (nentries == cap) {
			commit_dir_t* nb =
				realloc(entries, cap * 2 * sizeof(*entries));
			if (nb == NULL) break;
			entries = nb;
			cap *= 2;
		}
		snprintf(entries[nentries].name, sizeof(entries[0].name), "%s",
			 de->d_name);
		entries[nentries].mtime = st.st_mtime;
		nentries++;
	}
	closedir(bd);

	for (size_t i = 0; i + 1 < nentries; i++) {
		for (size_t j = i + 1; j < nentries; j++) {
			bool swap =
				newest_first
					? entries[j].mtime > entries[i].mtime
					: entries[j].mtime < entries[i].mtime;
			if (swap) {
				commit_dir_t t = entries[i];
				entries[i] = entries[j];
				entries[j] = t;
			}
		}
	}
	*out = entries;
	return (int)nentries;
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
	assert(text != NULL);
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
		char xml[PATH_MAX], gz[PATH_MAX], cmd[PATH_MAX * 2 + 32];
		snprintf(xml, sizeof(xml), "%s/appstream.xml", tmpdir);
		snprintf(gz, sizeof(gz), "%s/appstream.xml.gz", tmpdir);
		if (access(gz, F_OK) != 0 && access(xml, F_OK) == 0) {
			snprintf(cmd, sizeof(cmd),
				 "/usr/bin/gzip -c '%s' > '%s'", xml, gz);
			if (system(cmd) != 0)
				fpkg_warnf("could not compress %s", xml);
		}
	}

	if (legacy) {
		char gz[PATH_MAX], xml[PATH_MAX];
		snprintf(gz, sizeof(gz), "%s/appstream.xml.gz", tmpdir);
		snprintf(xml, sizeof(xml), "%s/appstream.xml", tmpdir);
		if (access(xml, F_OK) != 0 && access(gz, F_OK) == 0) {
			char cmd[PATH_MAX * 2 + 32];
			snprintf(cmd, sizeof(cmd),
				 "/usr/bin/gzip -dc '%s' > '%s'", gz, xml);
			if (system(cmd) != 0)
				fpkg_warnf("could not decompress %s", gz);
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

// Every libflatpak-based tool reads <commit>/deploy and skips a deploy dir with
// none.
static void write_deploy_data(const char* branch_dir, const char* commit,
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
