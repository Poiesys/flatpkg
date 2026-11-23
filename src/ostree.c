#include <signal.h>

#include "ostree_internal.h"

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
// Same buffer, but the length is carried and the total is bounded: a tool's
// output is remote-controlled when it prints commit metadata.
size_t buf_append_capped(char** buf, size_t* cap, size_t* len, bool* stopped,
			 const char* data, size_t n) {
	assert(buf != NULL);
	assert(cap != NULL);
	assert(len != NULL);
	assert(stopped != NULL);
	assert(data != NULL);

	if (*stopped) return *len;
	if (!*buf) *len = 0;
	if (*len + n + 1 > FPKG_TOOL_CAPTURE_MAX) {
		// Keep what arrived, stop copying, and let the consumer refuse.
		*stopped = true;
		return *len;
	}
	if (*len + n + 1 > *cap) {
		size_t nc = *cap ? *cap : 8192;

		while (*len + n + 1 > nc) nc *= 2;
		char* nb = realloc(*buf, nc);

		if (!nb) {
			fpkg_errorf("out of memory");
			exit(1);
		}
		*buf = nb;
		*cap = nc;
	}
	memcpy(*buf + *len, data, n);
	*len += n;
	(*buf)[*len] = '\0';
	return *len;
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
	size_t ocap = 0, ecap = 0, olen = 0, elen = 0;
	bool ostopped = false, estopped = false;
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
							buf_append_capped(
								&res->out, &ocap,
								&olen, &ostopped,
								chunk, (size_t)r);
					} else {
						buf_append_capped(
							&res->err, &ecap,
							&elen, &estopped,
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
			// Quiet is not finished: ostree writes a remote's config after
			// its last line, so returning here let a caller act on a
			// half-run command (a --gpg-import raced with the config write
			// and the import won).  Keep waiting - but a tool that neither
			// prints nor exits is killed rather than hanging the caller.
			if (++grace > 600) {
				int status = 0;

				(void)kill(pid, SIGKILL);
				(void)waitpid(pid, &status, 0);
				fpkg_errorf("a tool ran without output for two "
					    "minutes; killed it");
				res->rc = 124;
				break;
			}
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
	if (ostopped) res->out_too_big = true;
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

	int rc = run_direct_cb(argv, res, cb, arg, tick, tick_arg);

	// A pull refreshes repo/tmp/cache/summaries as whoever ran it, and the
	// planner trusts what it reads there: take it back after any tool run
	// that succeeded.  Cheap, idempotent, and a no-op when the caller
	// is not root.
	if (rc == 0) ostree_summary_cache_own();
	return rc;
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
								fpkg_mono_ns();
							g_delta_total_bytes =
								tb;
						}
						double elapsed =
							(fpkg_mono_ns() -
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
	if (!remote_name_valid(job->remote)) {
		fpkg_errorf("refusing an invalid remote name '%s'",
			    job->remote);
		job->rc = -1;
		return NULL;
	}
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
void prefetched_clear(void) {
	for (size_t i = 0; i < g_nprefetched; i++) free(g_prefetched[i]);
	free(g_prefetched);
	g_prefetched = NULL;
	g_nprefetched = 0;
}
int tool_pull(const char* remote, const char* refstr) {
	assert(remote != NULL);
	assert(refstr != NULL);
	if (!remote_name_valid(remote)) {
		fpkg_errorf("refusing an invalid remote name '%s'", remote);
		return -1;
	}
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
	if (!remote_name_valid(remote)) return -1;
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
// True when the nearest ancestor of `path` that exists is under the store.
// mkdir_p follows a symlink that is already there, so the question has to be
// answered before it creates anything, not after.
static bool store_parent_contained(const char* path) {
	char root[PATH_MAX], probe[PATH_MAX], real[PATH_MAX];

	if (realpath(store_root(), root) == NULL) return false;
	snprintf(probe, sizeof(probe), "%s", path);
	for (;;) {
		size_t rl;
		char* slash;

		if (realpath(probe, real) != NULL) {
			rl = strlen(root);
			return strncmp(real, root, rl) == 0 &&
			       (real[rl] == '/' || real[rl] == '\0');
		}
		slash = strrchr(probe, '/');
		if (slash == NULL || slash == probe) return false;
		*slash = '\0';
	}
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
	if (!store_parent_contained(parent)) {
		fpkg_errorf("refusing to deploy outside the store: %s", parent);
		return -1;
	}
	if (mkdir_p(parent, 0755) != 0) {
		fpkg_errorf("mkdir %s: %s", parent, strerror(errno));
		return -1;
	}
	if (!store_path_contained(parent)) {
		fpkg_errorf("refusing to deploy outside the store: %s", parent);
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
	// A bar, not a spinner: the worker's spinner never reaches the caller,
	// and the bar at least names the phase.  This ostree prints no
	// "Checking out files:" percentage, so it reads 0% until the
	// transaction's own result line.
	char label[320];
	snprintf(label, sizeof(label), "Installing %s", ref->id);
	if (tool_ok_bar_q(argv, "ostree checkout", label,
			  flatpkg_txn_active()) != 0)
		return -1;

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
			(void)unlink(current_link);
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
	if (!remote_name_valid(remote)) return;
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
	if (!remote_name_valid(remote)) return NULL;

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

	strlist_t refs = {0};

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
		strlist_push(&refs, line);
	}

	tool_res_free(&res);
	*n_out = refs.count;
	return refs.items;
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
	if (ostree_run(argv, &res) == 0 && res.out_too_big) {
		// Half a metadata document reads as a complete one with the keys
		// after the cut missing.
		fpkg_errorf("commit metadata is larger than %u bytes",
			    (unsigned)FPKG_TOOL_CAPTURE_MAX);
		tool_res_free(&res);
		return NULL;
	}
	if (res.rc == 0 && res.out && *res.out) {
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
	if (res.out_too_big) {
		fpkg_errorf("%s's metadata is larger than %u bytes", commit,
			    (unsigned)FPKG_TOOL_CAPTURE_MAX);
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
	if (!remote_name_valid(rem)) {
		fpkg_errorf("refusing an invalid remote name '%s'", rem);
		free(rem);
		ref_free(&r);
		return -1;
	}

	char* body = NULL;
	char mcommit[256] = "";
	if (strcmp(kind, "app") == 0 || strcmp(kind, "runtime") == 0)
		body = summary_ref_metadata_text(rem, refstr);
	tool_ctx_t tc;
	tool_ctx_init(&tc);
	if (tool_rev_probe(&tc, refstr, mcommit, sizeof(mcommit)) != 0) {
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
// A summary's xa.cache entry is a few hundred bytes of keyfile; the bound is
// what keeps a hostile one from asking the daemon for gigabytes.
#define SUMMARY_META_MAX (64u << 20)

static char* zlib_decompress_str(const guint8* src, gsize srclen) {
	if (srclen == 0 || srclen > SUMMARY_META_MAX) return NULL;
	// Start with a 4x estimate; grow if needed, but never past the cap.
	uLongf dstlen = (uLongf)(srclen * 4 + 128);
	for (int tries = 0; tries < 8 && dstlen <= SUMMARY_META_MAX; tries++) {
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
// The planner trusts this cache, and it is written by whoever pulled, so a
// group member could rewrite it.  Root takes it over after a pull, and a reader
// judges the descriptor it opened: this tree is group-writable.
static int summary_cache_open(const char* path) {
	char dir[PATH_MAX];
	const char* base;
	char* slash;
	struct stat st;
	int dfd, fd;

	assert(path != NULL);
	snprintf(dir, sizeof(dir), "%s", path);
	slash = strrchr(dir, '/');
	if (slash == NULL) return -1;
	*slash = '\0';
	base = path + (slash - dir) + 1;
	dfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (dfd < 0) return -1;
	if (fstat(dfd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 ||
	    (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
		close(dfd);
		return -1;
	}
	fd = openat(dfd, base, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	close(dfd);
	if (fd < 0) return -1;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != 0 ||
	    (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

// The bytes of the file the descriptor names: the map is of the object that
// was judged, never of a path a swap could re-point.
static GBytes* summary_cache_bytes(const char* path) {
	int fd = summary_cache_open(path);
	GMappedFile* mf;
	GBytes* bytes;

	if (fd < 0) return NULL;
	mf = g_mapped_file_new_from_fd(fd, FALSE, NULL);
	close(fd);
	if (mf == NULL) return NULL;
	bytes = g_mapped_file_get_bytes(mf);
	g_mapped_file_unref(mf);
	return bytes;
}

// Root takes the cache back through the descriptors it opened: the store's tmp
// tree is group-writable, so a path looked at and then changed by name is a
// path a swap can choose.  A file root did not write is dropped, never adopted:
// taking it over would bless whatever a group member planted before the pull.
void ostree_summary_cache_own(void) {
	char tmpdir[PATH_MAX];
	struct stat st;
	DIR* d;
	int tfd, cd, dfd;

	if (geteuid() != 0) return;
	store_path(tmpdir, sizeof(tmpdir), "repo/tmp");
	tfd = open(tmpdir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (tfd < 0) return;
	cd = openat(tfd, "cache", O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
					 O_CLOEXEC);
	close(tfd);
	if (cd < 0) return;
	if (fstat(cd, &st) != 0 || !S_ISDIR(st.st_mode)) {
		close(cd);
		return;
	}
	(void)fchmod(cd, 0755);
	(void)fchown(cd, 0, 0);
	dfd = openat(cd, "summaries", O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
					   O_CLOEXEC);
	close(cd);
	if (dfd < 0) return;
	if (fstat(dfd, &st) != 0 || !S_ISDIR(st.st_mode)) {
		close(dfd);
		return;
	}
	(void)fchmod(dfd, 0755);
	(void)fchown(dfd, 0, 0);
	d = fdopendir(dfd);
	if (d == NULL) {
		close(dfd);
		return;
	}
	dfd = dirfd(d);
	if (dfd < 0) {
		closedir(d);
		return;
	}
	for (struct dirent* de; (de = readdir(d)) != NULL;) {
		int fd;

		if (de->d_name[0] == '.') continue;
		fd = openat(dfd, de->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		if (fd < 0) {
			// A name root cannot open as itself (a symlink, or
			// something that is not a regular file) was not
			// written by root: drop it.
			if (errno != ENOENT)
				(void)unlinkat(dfd, de->d_name, 0);
			continue;
		}
		if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
		    st.st_uid != 0) {
			close(fd);
			(void)unlinkat(dfd, de->d_name, 0);
			continue;
		}
		(void)fchmod(fd, 0644);
		(void)fchown(fd, 0, 0);
		close(fd);
	}
	closedir(d);
}

// The summary's xa.cache maps each ref to its metadata keyfile, so a plan
// needs no commit fetch, and libostree checks it against the remote's
// signature: v1 a{s(tts)}, v2 a{s(ttay)} (deflated), told apart by type.
static char* summary_xa_metadata(const char* remote, const char* refstr) {
	if (!remote_name_valid(remote)) return NULL;
	char path[PATH_MAX];
	store_path(path, sizeof(path), "repo/tmp/cache/summaries/%s", remote);
	GBytes* bytes = summary_cache_bytes(path);
	if (bytes == NULL) {
		fpkg_debugf("summary cache %s is not root's; ignoring it", path);
		return NULL;
	}
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
// The commit a remote's summary advertises for a ref.  The summary's ref list
// is (name, (timestamp, checksum, metadata)), so the checksum is 32 bytes of
// binary that the rest of the tree spells in hex.
char* summary_ref_commit(const char* remote, const char* refstr) {
	if (!remote_name_valid(remote)) return NULL;
	// A local repository has no summary cache: ask it directly.
	char local[PATH_MAX];
	if (remote_local_path(remote, local, sizeof(local))) {
		char repo_arg[PATH_MAX + 32];
		tool_res_t res;

		snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", local);
		char* argv[] = {"ostree", "rev-parse", repo_arg, (char*)refstr,
				NULL};
		if (ostree_run(argv, &res) != 0 || res.rc != 0 || !res.out ||
		    !*res.out) {
			tool_res_free(&res);
			return NULL;
		}
		res.out[strcspn(res.out, " \t\r\n")] = '\0';
		char* out = xstrdup(res.out);
		tool_res_free(&res);
		return out;
	}
	char path[PATH_MAX];
	store_path(path, sizeof(path), "repo/tmp/cache/summaries/%s", remote);
	GBytes* bytes = summary_cache_bytes(path);
	if (bytes == NULL) {
		fpkg_debugf("summary cache %s is not root's; ignoring it", path);
		return NULL;
	}
	GVariant* sum = g_variant_new_from_bytes(
		G_VARIANT_TYPE("(a(s(taya{sv}))a{sv})"), bytes, FALSE);
	g_bytes_unref(bytes);
	GVariant* refs = g_variant_get_child_value(sum, 0);
	char* out = NULL;

	for (gsize i = 0; i < g_variant_n_children(refs) && out == NULL; i++) {
		GVariant* ent = g_variant_get_child_value(refs, i);
		const char* name = NULL;

		g_variant_get_child(ent, 0, "&s", &name);
		if (name == NULL || strcmp(name, refstr) != 0) {
			g_variant_unref(ent);
			continue;
		}
		GVariant* body = g_variant_get_child_value(ent, 1);
		GVariant* csum = g_variant_get_child_value(body, 1);
		gsize clen = 0;
		const guint8* cdata = g_variant_get_fixed_array(csum, &clen, 1);

		if (cdata != NULL && clen == 32) {
			char hex[65];

			for (gsize k = 0; k < clen; k++)
				snprintf(hex + k * 2, 3, "%02x", cdata[k]);
			out = xstrdup(hex);
		}
		g_variant_unref(csum);
		g_variant_unref(body);
		g_variant_unref(ent);
	}
	g_variant_unref(refs);
	g_variant_unref(sum);
	return out;
}
// Try the local summary cache first (no network) and refresh the
// summary, which may hit the network, only on a miss.
char* summary_ref_metadata_text(const char* remote,
				       const char* refstr) {
	// A local repository has no summary cache; the commit is local too, so
	// its metadata can be read straight out of it.
	char local[PATH_MAX];
	if (remote_local_path(remote, local, sizeof(local))) {
		char* commit = summary_ref_commit(remote, refstr);

		if (commit == NULL) return NULL;
		char repo_arg[PATH_MAX + 32];
		tool_res_t res;

		snprintf(repo_arg, sizeof(repo_arg), "--repo=%s", local);
		char* argv[] = {"ostree", "cat", repo_arg, commit, "metadata",
				NULL};
		int rc = ostree_run(argv, &res);

		// argv kept the pointer: free it only after the child ran.
		free(commit);
		if (rc != 0 || res.rc != 0 || !res.out) {
			tool_res_free(&res);
			return NULL;
		}
		char* out = xstrdup(res.out);
		tool_res_free(&res);
		return out;
	}
	char* body = summary_xa_metadata(remote, refstr);
	if (body) return body;
	const remote_refs_t* rr =
		remote_refs_cached(remote, "Looking up metadata");
	if (!rr || rr->failed) return NULL;
	return summary_xa_metadata(remote, refstr);
}
// A plan row may name its own remote (`flathub:runtime/...`); the kind word is
// whatever follows the colon.  Rows the planner built from metadata carry no
// remote, so the executor asks for the body only.
const char* ref_body(const char* ref) {
	const char* colon = strchr(ref, ':');
	const char* slash = strchr(ref, '/');

	return (colon != NULL && (slash == NULL || colon < slash)) ? colon + 1
								  : ref;
}

// Does this remote's summary carry the ref?  Extensions are published as
// runtime/<id>/<arch>/<branch>, and a row spells them extension/.
static bool remote_has_ref(const char* remote, const char* ref) {
	char want[512];
	remote_refs_t* rr;

	snprintf(want, sizeof(want), "%s",
		 ref_body(ref));
	if (strncmp(want, "extension/", 10) == 0) {
		char tmp[512];
		snprintf(tmp, sizeof(tmp), "runtime/%s", want + 10);
		snprintf(want, sizeof(want), "%s", tmp);
	}
	rr = remote_refs_cached(remote, "Looking up dependencies");
	if (rr == NULL || rr->failed) return false;
	for (size_t i = 0; i < rr->n; i++)
		if (strcmp(rr->refs[i], want) == 0) return true;
	return false;
}

// Which remote has this ref?  A dependency row carries no remote of its own,
// and the app's remote usually does not ship the runtime - NVIDIA's repo has
// com.nvidia.geforcenow, flathub has org.freedesktop.Platform - so the plan's
// remote is a preference, not the answer.  Nothing found keeps the preference.
char* remote_for_ref(const char* prefer, const char* ref) {
	if (prefer != NULL && remote_has_ref(prefer, ref)) return xstrdup(prefer);
	size_t n = 0;
	char** remotes = list_remotes(&n);

	for (size_t i = 0; i < n; i++) {
		if (prefer != NULL && strcmp(remotes[i], prefer) == 0) continue;
		if (!remote_has_ref(remotes[i], ref)) continue;
		char* out = xstrdup(remotes[i]);
		for (size_t k = 0; k < n; k++) free(remotes[k]);
		free(remotes);
		return out;
	}
	for (size_t k = 0; k < n; k++) free(remotes[k]);
	free(remotes);
	return prefer != NULL ? xstrdup(prefer) : NULL;
}

// Pull every ref the plan installs side by side, one bar each. The
// installs that follow find them in the repo; whatever failed here is
// retried there.
void fpkg_plan_free(fpkg_plan_t* plan) {
	assert(plan != NULL);
	changeset_free(&plan->cs);
	world_free(&plan->world);
	free(plan->remote);
	memset(plan, 0, sizeof(*plan));
}
// ref_kind is the ostree ref prefix ("app"/"runtime"), not the deploy type.
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
// One remove executor for changeset_apply().  The plan owns the world and
// the table; this does only the store work after confirmation.
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
	// A remote name is an argv word for root's ostree (and a store path
	// component): no leading dash, or it is parsed as an ostree flag.
	if (name == NULL || *name == '\0' || name[0] == '-') return false;
	return fpkg_component_ok(name);
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
