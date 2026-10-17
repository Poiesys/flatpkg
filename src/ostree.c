#include "ostree_internal.h"

fpkg_error_t write_file(const char* path, const char* content,
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
	if (type == FTW_DP || type == FTW_D) rmdir(path);
	else
		unlink(path);
	return 0;
}
int rm_rf(const char* path) {
	assert(path != NULL);
	// FTW_MOUNT: FTW_PHYS alone only skips symlinks.
	return nftw(path, rm_cb, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
}
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
void trim(char* s) {
	assert(s != NULL);
	char* p = s;
	while (*p && isspace((unsigned char)*p)) p++;
	if (p != s) memmove(s, p, strlen(p) + 1);
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
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
		names[n++] = strndup(line + 9, end - (line + 9));
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
void tool_res_init(tool_res_t* res) {
	assert(res != NULL);
	memset(res, 0, sizeof(*res));
	res->rc = -1;
}
void tool_res_free(tool_res_t* res) {
	assert(res != NULL);
	free(res->out);
	free(res->err);
	memset(res, 0, sizeof(*res));
}
void tool_ctx_init(tool_ctx_t* tc) {
	assert(tc != NULL);
	memset(tc, 0, sizeof(*tc));
	snprintf(tc->repo, sizeof(tc->repo), "/ostree/repo");
	snprintf(tc->keys, sizeof(tc->keys), "/ostree/keys");
	snprintf(tc->store, sizeof(tc->store), "/ostree");
}
size_t buf_append(char** buf, size_t* cap, const char* data,
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
int ostree_run_cb(char* const argv[], tool_res_t* res, tool_line_cb cb,
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
int ostree_run(char* const argv[], tool_res_t* res) {
	assert(argv != NULL);
	assert(res != NULL);
	return ostree_run_cb(argv, res, NULL, NULL, NULL, NULL);
}
void log_tool_stderr(const tool_res_t* res) {
	assert(res != NULL);
	if (!res->err || !*res->err) return;
	char* copy = xstrdup(res->err);
	char* save = NULL;
	for (char* line = strtok_r(copy, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save))
		fpkg_warnf("ostree: %s", line);
	free(copy);
}
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
void spinner_tick_cb(void* arg) { fp_spin_tick((fp_spin_t*)arg); }
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
int ensure_repo(void) {
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
int parallel_pull(pull_job_t* jobs, size_t n, flatpkg_txn_t* txn) {
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
void pull_job_init(pull_job_t* job, const char* remote,
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
void pull_jobs_free(pull_job_t* jobs, size_t n) {
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
void flatpak_ref_header(const char* refstr, char* out, size_t outlen) {
	assert(refstr != NULL);
	assert(out != NULL);
	assert(outlen > 0);
	snprintf(out, outlen, "--http-header=Flatpak-Ref=%s", refstr);
}
// Refs a parallel prefetch already pulled: the sequential install that
// follows must not spawn a second pull for each of them.
static char** g_prefetched;
static size_t g_nprefetched;
void prefetched_add(const char* refstr) {
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
int tool_pull(const char* remote, const char* refstr) {
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
int tool_pull_metadata(const char* remote, const char* refstr) {
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
int tool_rev_parse(const tool_ctx_t* tc, const char* refstr,
			  char* commit_out, size_t len) {
	return tool_rev_parse_impl(tc, refstr, commit_out, len, false);
}
int tool_rev_probe(const tool_ctx_t* tc, const char* refstr,
			  char* commit_out, size_t len) {
	return tool_rev_parse_impl(tc, refstr, commit_out, len, true);
}
int deploy_checkout(const tool_ctx_t* tc, const char* type,
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
int make_active(const char* deploy_host_path) {
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
void write_origin(const char* deploy_host_path, const char* remote) {
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
bool deploy_exists(const char* type, const char* id) {
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/%s/%s", store_root(), type, id);
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
bool deploy_branch_dir(const char* deploy, char* out, size_t outlen) {
	char tmp[PATH_MAX];

	snprintf(tmp, sizeof(tmp), "%s", deploy);
	char* slash = strrchr(tmp, '/');
	if (slash == NULL) return false;
	*slash = '\0';
	snprintf(out, outlen, "%s", tmp);
	return true;
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
bool g_fetch_quiet;
char** fetch_remote_refs(const char* remote, size_t* n_out,
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
// The keyfile a flatpak commit carries as xa.metadata.  "ostree show" prints
// the value as GVariant text.
char* commit_xa_metadata(const char* commit) {
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
char* cat_commit_metadata(const char* commit) {
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
char* summary_ref_metadata_text(const char* remote,
				       const char* refstr) {
	char* body = summary_xa_metadata(remote, refstr);
	if (body) return body;
	const remote_refs_t* rr =
		remote_refs_cached(remote, "Looking up metadata");
	if (!rr || rr->failed) return NULL;
	return summary_xa_metadata(remote, refstr);
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
void fpkg_plan_free(fpkg_plan_t* plan) {
	assert(plan != NULL);
	changeset_free(&plan->cs);
	world_free(&plan->world);
	free(plan->remote);
	memset(plan, 0, sizeof(*plan));
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
int find_deploy_dir(const char* type, const fpkg_ref_t* r, char* out,
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
	if (name == NULL || *name == '\0' || name[0] == '.') return false;
	for (const char* c = name; *c; c++) {
		bool alnum = (*c >= '0' && *c <= '9') ||
			     (*c >= 'a' && *c <= 'z') ||
			     (*c >= 'A' && *c <= 'Z');
		if (!alnum && *c != '-' && *c != '_' && *c != '.') return false;
	}
	return true;
}
// Is this name a commit directory? Commit hashes are 64-char SHA-256 hex.
bool is_commit_dir_name(const char* name) {
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
int collect_commit_dirs(const char* branch_dir, bool newest_first,
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
// Every libflatpak-based tool reads <commit>/deploy and skips a deploy dir with
// none.
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
