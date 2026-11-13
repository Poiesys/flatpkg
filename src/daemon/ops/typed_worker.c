#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "flatpkg.h"
#define FPKG_MODULE "daemon"
#include "fpkglog.h"
#include "cli.h"
#include "evui.h"
#include "protocol.h"
#include "typed_worker.h"
#include "../bus.h"

typedef struct {
	int conn;
	uid_t uid;
	gid_t gid;
	uint32_t seq;	 // per-connection event sequence
	uint64_t op_id;	 // the running operation, 0 when none
	bool hello;
	char client[65]; // the caller's declared state namespace
	char* client_files[16]; // files this connection declared
	size_t nclient_files;
	bool dead; // the client went away; keep going, stop writing
	bool bus_lost;	  // the broker went away; the operation still answers
	bool subscribed;  // this connection follows at least one topic
	int nsubs;
	time_t last_client; // when this connection last asked for something
	int stdio[FP_STDIO_FDS]; // the caller's stdin, stdout, stderr
	bool have_stdio;
	uint32_t pending_sub;	  // request waiting for the broker's ack
	uint32_t pending_cancel;
	size_t bad_frames; // malformed requests seen on this connection
} wctx_t;

// libnv's unpack leaks what it had built when a blob is malformed inside a
// pair or a nested list (base-system bug on this branch).  The worker is
// per-connection, so closing after a few bad frames bounds what one client
// can leave behind in root's address space.
#define FP_MAX_BAD_FRAMES 8

// After HELLO a connection may idle, but not forever in the worker pool: a
// connection following events is exempt, because waiting is its purpose.
#define FP_IDLE_TIMEOUT 900

// The op id is what a cancel names and what a subscription asks to watch, and
// it is broadcast on the firehose: a predictable one names another user's
// operation.  Random, and never zero (zero means "no operation").
static uint64_t fresh_op_id(void) {
	uint64_t id;

	do {
		arc4random_buf(&id, sizeof(id));
	} while (id == 0);
	return id;
}

// The worker runs as root, so a caller-sent name is one root's tools read:
// no PATH, LD_PRELOAD, HOME or FLATPKG_STORE crosses.
static const char* const env_allow[] = {"XDG_RUNTIME_DIR",
					"XDG_CURRENT_DESKTOP",
					"DISPLAY",
					"WAYLAND_DISPLAY",
					"DBUS_SESSION_BUS_ADDRESS",
					"DBUS_SYSTEM_BUS_ADDRESS",
					"AT_SPI_BUS_ADDRESS",
					"LC_ALL",
					"LC_MESSAGES",
					"LANG",
					// A TUI wants the caller's terminal and colour
					// capability, not a terminfo-less default.
					"TERM",
					"COLORTERM",
					"SSH_AUTH_SOCK",
					"PULSE_SERVER",
					"PULSE_RUNTIME_PATH",
					"PULSE_CLIENTCONFIG",
					"CUPS_SERVER",
					"PCSCLITE_CSOCK_NAME",
					"FLATPKG_DEBUG_KEEP",
					"FLATPKG_NO_SHIMS",
					"FLATPKG_OSTREE_VERBOSE",
					"FLATPKG_FILECHOOSER_TIMEOUT",
					"FLATPAK_GL_DRIVERS",
					NULL};

static bool env_value_ok(const char* v, uid_t uid, const char* name) {
	size_t n = strlen(v);
	if (n == 0 || n > 4096) return false;
	for (size_t i = 0; i < n; i++)
		if ((unsigned char)v[i] < 0x20 || v[i] == 0x7f) return false;
	// The runtime dir is where sockets are bound from: it has to be the
	// caller's own.
	if (strcmp(name, "XDG_RUNTIME_DIR") == 0) {
		struct stat st;
		if (v[0] != '/' || lstat(v, &st) != 0 || !S_ISDIR(st.st_mode) ||
		    st.st_uid != uid)
			return false;
	}
	return true;
}

static void worker_env_apply(char* const* vars, size_t n, uid_t uid) {
	for (size_t i = 0; i < n; i++) {
		const char* eq = strchr(vars[i], '=');
		if (eq == NULL || eq == vars[i]) continue;
		size_t nl = (size_t)(eq - vars[i]);
		for (const char* const* a = env_allow; *a != NULL; a++) {
			if (strlen(*a) != nl || strncmp(*a, vars[i], nl) != 0)
				continue;
			if (env_value_ok(eq + 1, uid, *a) &&
			    setenv(*a, eq + 1, 1) != 0)
				fpkg_warnf("cannot set %s: %s", *a,
					   strerror(errno));
			break;
		}
	}
}

// Broker cancel: the flag is polled by the long operations.
static void on_cancel(int sig) {
	fpkg_cancel_flag = 1;
}

static void on_client_gone(int sig) {
	fpkg_client_mark_gone();
}

static int write_all(int fd, const void* buf, size_t n) {
	const char* p = buf;
	while (n > 0) {
		ssize_t w = write(fd, p, n);
		if (w < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

static void send_ack(wctx_t* c, uint32_t id, const fp_ack_t* a) {
	nvlist_t* nvl = NULL;
	int e;

	if (fp_ack_encode(&nvl, id, a) != FP_P_OK) return;
	e = fp_frame_send(c->conn, nvl);
	nvlist_destroy(nvl);
	if (e != FP_P_OK) c->dead = true;
}

static void ack_ok(wctx_t* c, uint32_t id, uint64_t op_id) {
	fp_ack_t a;
	memset(&a, 0, sizeof(a));
	a.type = FP_ACK_OK;
	a.op_id = op_id;
	send_ack(c, id, &a);
}

static void ack_err(wctx_t* c, uint32_t id, uint32_t code, const char* msg) {
	fp_ack_t a;
	memset(&a, 0, sizeof(a));
	a.type = FP_ACK_ERR;
	a.code = code;
	a.message = (char*)msg; // encoded, never freed here
	send_ack(c, id, &a);
}

// Refuse before HELLO (caller outside the flatpkg group) with a decodable
// error frame, not a raw string the client cannot parse.
void typed_refuse(int conn, const char* msg) {
	fp_ack_t a;
	nvlist_t* nvl = NULL;

	memset(&a, 0, sizeof(a));
	a.type = FP_ACK_ERR;
	a.code = FP_E_DENIED;
	a.message = (char*)msg;
	if (fp_ack_encode(&nvl, 0, &a) != FP_P_OK) return;
	if (fp_frame_send(conn, nvl) != FP_P_OK)
		fpkg_debugf("cannot send the refusal: %s", strerror(errno));
	nvlist_destroy(nvl);
}

static void send_event(wctx_t* c, fp_event_t* ev) {
	char topic[FP_PROTO_MAX_TOPIC + 1];
	nvlist_t* nvl = NULL;

	snprintf(topic, sizeof(topic), "op/%llu", (unsigned long long)c->op_id);
	ev->topic = topic;
	ev->op_id = c->op_id;
	if (fp_event_encode(&nvl, c->seq++, ev) != FP_P_OK) return;
	if (!c->dead && fp_frame_send(c->conn, nvl) != FP_P_OK)
		c->dead = true;
	{
		size_t len = 0;
		void* blob = nvlist_pack(nvl, &len);

		if (blob != NULL) {
			(void)bus_publish(topic, blob, len);
			// `op` is the firehose: a watcher that does not yet know
			// an op id can follow every operation.
			if (strcmp(topic, "op") != 0)
				(void)bus_publish("op", blob, len);
			free(blob);
		}
	}
	nvlist_destroy(nvl);
}

static void sink_log(void* ctx, int level, const char* scope,
		     const char* text) {
	fp_event_t ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = FP_EV_OP_LOG;
	ev.u.log.level = (uint32_t)level;
	ev.u.log.scope = (char*)scope;
	ev.u.log.text = (char*)text;
	send_event(ctx, &ev);
}

static void sink_progress(void* ctx, const char* stage, uint64_t cur,
			  uint64_t total) {
	fp_event_t ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = FP_EV_OP_PROGRESS;
	ev.u.progress.stage = (char*)(stage ? stage : "");
	ev.u.progress.current = cur;
	ev.u.progress.total = total;
	send_event(ctx, &ev);
}

static bool sink_table(void* ctx, const char* const* headers, size_t ncols,
		       const char* const* const* rows, size_t nrows) {
	if (ncols == 0 || ncols > FP_PROTO_MAX_COLS ||
	    nrows > FP_PROTO_MAX_LIST || ncols * nrows > FP_PROTO_MAX_LIST)
		return false;
	char** cells = calloc(ncols * nrows + 1, sizeof(char*));
	char** heads =
		headers != NULL ? calloc(ncols + 1, sizeof(char*)) : NULL;
	if (cells == NULL || (headers != NULL && heads == NULL)) {
		free(cells);
		free(heads);
		return false;
	}
	for (size_t r = 0; r < nrows; r++)
		for (size_t k = 0; k < ncols; k++)
			cells[r * ncols + k] =
				(char*)(rows[r][k] ? rows[r][k] : "");
	for (size_t k = 0; headers != NULL && k < ncols; k++)
		heads[k] = (char*)(headers[k] ? headers[k] : "");
	fp_event_t ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = FP_EV_OP_TABLE;
	ev.u.table.ncols = (uint32_t)ncols;
	ev.u.table.nrows = (uint32_t)nrows;
	ev.u.table.headers = heads;
	ev.u.table.cells = cells;
	send_event(ctx, &ev);
	free(cells);
	free(heads);
	return true;
}

static void op_start(wctx_t* c, uint32_t req_id, uint32_t kind,
		     const char* label) {
	c->op_id = fresh_op_id();
	(void)bus_register(c->op_id);
	ack_ok(c, req_id, c->op_id);
	fp_event_t ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = FP_EV_OP_STARTED;
	ev.u.started.kind = kind;
	ev.u.started.label = (char*)label;
	send_event(c, &ev);
	evui_sink_t sink = {.log = sink_log,
			    .progress = sink_progress,
			    .table = sink_table,
			    .ctx = c};
	evui_set_sink(&sink);
}

static void op_finish(wctx_t* c, int rc, const char* error) {
	(void)bus_done(c->op_id);
	evui_set_sink(NULL);
	fp_event_t ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = FP_EV_OP_FINISHED;
	ev.u.finished.status = (uint32_t)(rc < 0 ? 1 : rc);
	ev.u.finished.error = (char*)(rc != 0 ? error : "");
	send_event(c, &ev);
	c->op_id = 0;
}

static const char* nz(const char* s) {
	return (s != NULL && *s != '\0') ? s : NULL;
}

// The bundle is copied from the descriptor into a root-only file and installed
// from there: what is installed is what was handed over, and no caller name is opened.

#define BUNDLE_MAX_BYTES ((off_t)16 << 30)

static int install_bundle_fd(int fd) {
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		fpkg_errorf("the bundle is not a regular file");
		return 1;
	}
	if (st.st_size <= 0 || st.st_size > BUNDLE_MAX_BYTES) {
		fpkg_errorf("the bundle is empty or larger than %lld bytes",
			    (long long)BUNDLE_MAX_BYTES);
		return 1;
	}
	char tmpl[] = "/var/tmp/fpkg-bundle.XXXXXX";
	int out = mkstemp(tmpl);
	if (out < 0) {
		fpkg_errorf("cannot create a temporary file: %s",
			    strerror(errno));
		return 1;
	}
	int rc = 1;
	char buf[1 << 16];
	off_t off = 0;
	for (;;) {
		ssize_t n = pread(fd, buf, sizeof(buf), off);
		if (n < 0 && errno == EINTR) continue;
		if (n < 0) {
			fpkg_errorf("reading the bundle: %s", strerror(errno));
			goto done;
		}
		if (n == 0) break;
		if (off + n > BUNDLE_MAX_BYTES) {
			fpkg_errorf("the bundle grew past the size limit");
			goto done;
		}
		for (ssize_t w = 0; w < n;) {
			ssize_t k = write(out, buf + w, (size_t)(n - w));
			if (k < 0 && errno == EINTR) continue;
			if (k <= 0) {
				fpkg_errorf("copying the bundle: %s",
					    strerror(errno));
				goto done;
			}
			w += k;
		}
		off += n;
	}
	char* ref = NULL;
	if (ostree_install_bundle(tmpl, NULL, 0, &ref) != 0) goto done;
	fpkg_okf("installed %s from the bundle", ref != NULL ? ref : "it");
	free(ref);
	rc = 0;
done:
	close(out);
	(void)unlink(tmpl);
	return rc;
}

// run: the app's stdio are the caller's descriptors (FP_REQ_STDIO, made 0-2),
// so its output never goes through the daemon; forwarded fds keep their numbers.

// The app must not outlive the request: this watcher sends the worker SIGHUP
// when the client's end closes, and cmd_run turns that into termination.
static pid_t watch_client(int conn) {
	const pid_t wpid = getpid();
	pid_t mon = fork();
	if (mon != 0) return mon;
	// Only the connection is watched: an inherited terminal or pty end is another
	// holder when the app exits, and FreeBSD then loses queued output.
	for (int fd = 0; fd < conn; fd++) close(fd);
	closefrom(conn + 1);
	for (;;) {
		struct pollfd pf = {conn, POLLIN, 0};
		if (getppid() != wpid) _exit(0);
		if (poll(&pf, 1, 500) <= 0) continue;
		char ch;
		ssize_t n = recv(conn, &ch, 1, MSG_PEEK | MSG_DONTWAIT);
		if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) {
			(void)kill(wpid, SIGHUP);
			_exit(0);
		}
		if (n > 0) usleep(200000);
	}
}

// The forward list the worker accepted, as targets.  The client validates it
// too; this is the root side, so it does not trust that.
static bool forward_parse(const char* s, int* targets, size_t* n) {
	*n = 0;
	if (s == NULL || *s == '\0') return true;
	while (*s) {
		char* end;
		long v = strtol(s, &end, 10);
		if (end == s || v < 3 || v >= FP_MAX_FDS) return false;
		for (size_t i = 0; i < *n; i++)
			if (targets[i] == (int)v) return false;
		if (*n == FP_MAX_FDS) return false;
		targets[(*n)++] = (int)v;
		if (*end == '\0') break;
		if (*end != ',' || end[1] == '\0') return false;
		s = end + 1;
	}
	return true;
}

// Which of the caller's stdio descriptors are terminals (bit i for descriptor i),
// decided on the descriptors: a client cannot hand over a tty it denies having.
static unsigned stdio_tty_mask(const wctx_t* c) {
	unsigned m = 0;
	for (int i = 0; i < FP_STDIO_FDS; i++)
		if (isatty(c->stdio[i])) m |= 1u << i;
	return m;
}

// The app's terminal when the caller has one: a pty of ours (FLATPKG_APP_TTY /
// FLATPKG_APP_MASTER), so the app cannot push into the user's with TIOCSTI.
static int open_app_pty(int* master, int* slave, int size_from) {
	int m = posix_openpt(O_RDWR | O_NOCTTY);
	if (m < 0) return -1;
	if (grantpt(m) != 0 || unlockpt(m) != 0) {
		close(m);
		return -1;
	}
	const char* name = ptsname(m);
	int s = name != NULL ? open(name, O_RDWR | O_NOCTTY) : -1;
	if (s < 0) {
		close(m);
		return -1;
	}
	struct winsize ws;
	if (ioctl(size_from, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0 ||
	    ws.ws_col == 0) {
		memset(&ws, 0, sizeof(ws));
		ws.ws_row = 24;
		ws.ws_col = 80;
	}
	(void)ioctl(m, TIOCSWINSZ, &ws);
	int hm = fcntl(m, F_DUPFD_CLOEXEC, FP_MAX_FDS + FP_STDIO_FDS);
	int hs = fcntl(s, F_DUPFD_CLOEXEC, FP_MAX_FDS + FP_STDIO_FDS);
	close(m);
	close(s);
	if (hm < 0 || hs < 0) {
		if (hm >= 0) close(hm);
		if (hs >= 0) close(hs);
		return -1;
	}
	*master = hm;
	*slave = hs;
	return 0;
}

// Put the caller's stdio on the worker's 0-2 for one request. The worker's own
// are saved above every target, so restoring cannot pick up an app descriptor.
static int stdio_adopt(wctx_t* c, int saved[FP_STDIO_FDS]) {
	for (int i = 0; i < FP_STDIO_FDS; i++) {
		saved[i] = fcntl(i, F_DUPFD_CLOEXEC, FP_MAX_FDS);
		if (saved[i] < 0 && errno != EBADF) {
			fpkg_errorf("cannot save descriptor %d: %s", i,
				    strerror(errno));
			return -1;
		}
		if (c->stdio[i] >= 0 && dup2(c->stdio[i], i) < 0) {
			fpkg_errorf("cannot adopt descriptor %d: %s", i,
				    strerror(errno));
			return -1;
		}
	}
	return 0;
}

static void stdio_release(wctx_t* c, int saved[FP_STDIO_FDS]) {
	for (int i = 0; i < FP_STDIO_FDS; i++) {
		if (c->stdio[i] >= 0) close(c->stdio[i]);
		c->stdio[i] = -1;
		if (saved[i] >= 0) {
			if (dup2(saved[i], i) < 0)
				fpkg_warnf("cannot restore descriptor %d: %s",
					   i, strerror(errno));
			close(saved[i]);
		} else {
			// It was closed before the request; leave it that way.
			close(i);
		}
	}
	c->have_stdio = false;
}

// The terminal of one request whose caller has one: a pty of ours, with the mask
// after the colon saying which app descriptors are it; a mixture keeps its file elsewhere.
static int terminal_open(int* master, int* slave, unsigned mask) {
	int size_fd = -1;
	char fds[8];
	size_t k = 0;
	for (int i = 0; i < FP_STDIO_FDS; i++)
		if (mask & (1u << i)) {
			if (size_fd < 0) size_fd = i;
			if (k > 0) fds[k++] = ',';
			fds[k++] = (char)('0' + i);
		}
	fds[k] = '\0';
	char num[32];
	if (open_app_pty(master, slave, size_fd) != 0) {
		fpkg_errorf("cannot make a terminal for the app: %s",
			    strerror(errno));
		return -1;
	}
	snprintf(num, sizeof(num), "%d:%s", *slave, fds);
	if (setenv("FLATPKG_APP_TTY", num, 1) != 0) {
		fpkg_errorf("cannot name the app terminal: %s",
			    strerror(errno));
		close(*slave);
		*slave = -1;
		return -1;
	}
	snprintf(num, sizeof(num), "%d", *master);
	if (setenv("FLATPKG_APP_MASTER", num, 1) != 0) {
		fpkg_errorf("cannot name the app terminal: %s",
			    strerror(errno));
		return -1;
	}
	return 0;
}

// The next request on this connection must not find this one's terminal.
static void terminal_close(int master, int slave) {
	// sandbox_release_app_tty() closes the slave once the app has its own copy and
	// unsets the variable, so it is ours only when the launch never forked.
	const char* unreleased = cli_getenv("FLATPKG_APP_TTY");
	bool slave_is_ours = unreleased != NULL && *unreleased != '\0';
	(void)unsetenv("FLATPKG_APP_TTY");
	(void)unsetenv("FLATPKG_APP_MASTER");
	if (master >= 0) close(master);
	if (slave >= 0 && slave_is_ours) close(slave);
}

static void watcher_stop(pid_t mon) {
	if (mon <= 0) return;
	// ESRCH: the watcher already saw the client go and exited.
	if (kill(mon, SIGTERM) != 0 && errno != ESRCH)
		fpkg_warnf("cannot stop the client watcher: %s",
			   strerror(errno));
	(void)waitpid(mon, NULL, 0);
}

// run_app takes ownership of fds[] (the descriptors received on the RUN frame)
// and leaves none open on any path.
static void emit_instance(bool started, const char* app_id);

static int run_app(wctx_t* c, const fp_req_t* r, const int* targets,
		   size_t ntargets, int* fds) {
	char** argv = NULL;
	int saved[FP_STDIO_FDS] = {-1, -1, -1};
	int pty_master = -1, pty_slave = -1;
	const unsigned tty_mask = stdio_tty_mask(c);
	int rc = 1;

	argv = calloc(r->u.run.nargs + 1, sizeof(char*));
	if (argv == NULL) goto out;
	for (size_t i = 0; i < r->u.run.nargs; i++) argv[i] = r->u.run.args[i];

	run_opts_t ro;
	memset(&ro, 0, sizeof(ro));
	ro.sockets = (strlist_t){r->u.run.sockets, r->u.run.nsockets};
	ro.devices = (strlist_t){r->u.run.devices, r->u.run.ndevices};
	ro.shares = (strlist_t){r->u.run.shares, r->u.run.nshares};
	ro.fs_add = (strlist_t){r->u.run.fs_add, r->u.run.nfs_add};
	ro.fs_remove = (strlist_t){r->u.run.fs_remove, r->u.run.nfs_remove};
	ro.envs = (strlist_t){r->u.run.envs, r->u.run.nenvs};
	ro.command = (char*)nz(r->u.run.command);
	ro.branch = (char*)nz(r->u.run.branch);
	ro.arch = (char*)nz(r->u.run.arch);
	ro.devel = (r->u.run.flags & FP_RUN_DEVEL) != 0;
	ro.clearenv = (r->u.run.flags & FP_RUN_CLEARENV) != 0;
	ro.die_with_parent = (r->u.run.flags & FP_RUN_DIE_WITH_PARENT) != 0;
	ro.file_forwarding = (r->u.run.flags & FP_RUN_FILE_FORWARDING) != 0;

	if (stdio_adopt(c, saved) != 0) goto out;

	// Move every source above the saved stdio before applying any target, or one
	// target's dup2 could close a source another target still needs.
	for (size_t i = 0; i < ntargets; i++) {
		if (fds[i] == targets[i]) continue;
		int h = fcntl(fds[i], F_DUPFD_CLOEXEC,
			      FP_MAX_FDS + FP_STDIO_FDS);
		if (h < 0) {
			fpkg_errorf("cannot move descriptor %d: %s", fds[i],
				    strerror(errno));
			goto out;
		}
		close(fds[i]);
		fds[i] = h;
	}
	for (size_t i = 0; i < ntargets; i++) {
		if (fds[i] == targets[i]) continue;
		if (dup2(fds[i], targets[i]) < 0) {
			fpkg_errorf("cannot put descriptor %d on %d: %s",
				    fds[i], targets[i], strerror(errno));
			goto out;
		}
		close(fds[i]);
		fds[i] = targets[i];
	}

	if (ntargets > 0) {
		if (setenv("FLATPKG_FORWARD_FDS", r->u.run.forward, 1) != 0) {
			fpkg_errorf("cannot set the forwarded descriptors: %s",
				    strerror(errno));
			goto out;
		}
	} else {
		(void)unsetenv("FLATPKG_FORWARD_FDS");
	}

	// Started before the pty exists so it cannot inherit the slave: a copy held there
	// is another holder when the session leader exits and FreeBSD drops queued output.
	pid_t mon = watch_client(c->conn);
	if (tty_mask != 0 &&
	    terminal_open(&pty_master, &pty_slave, tty_mask) != 0) {
		watcher_stop(mon);
		goto out;
	}
	emit_instance(true, r->u.run.app_id);
	rc = cmd_run(r->u.run.app_id, argv, &ro);
	emit_instance(false, r->u.run.app_id);
	watcher_stop(mon);
out:
	fflush(stdout);
	fflush(stderr);
	(void)unsetenv("FLATPKG_FORWARD_FDS");
	terminal_close(pty_master, pty_slave);
	// The app has its own copy across the exec; these are ours. A target may use a
	// stdio number, so retire that slot first and never close a number twice.
	for (size_t i = 0; i < ntargets; i++) {
		if (fds[i] < 0) continue;
		for (int k = 0; k < FP_STDIO_FDS; k++)
			if (c->stdio[k] == fds[i]) c->stdio[k] = -1;
		close(fds[i]);
		fds[i] = -1;
	}
	stdio_release(c, saved);
	free(argv);
	return rc;
}

static int enter_app(wctx_t* c, const fp_req_t* r) {
	int saved[FP_STDIO_FDS] = {-1, -1, -1};
	int pty_master = -1, pty_slave = -1;
	const unsigned tty_mask = stdio_tty_mask(c);
	int rc = 1;

	// cmd_enter takes [command, args...]; nothing at all means a shell.
	char** argv = calloc(r->u.enter.ncommand + 1, sizeof(char*));
	if (argv == NULL) goto out;
	for (size_t i = 0; i < r->u.enter.ncommand; i++)
		argv[i] = r->u.enter.command[i];

	if (stdio_adopt(c, saved) != 0) goto out;
	pid_t mon = watch_client(c->conn);
	if (tty_mask != 0 &&
	    terminal_open(&pty_master, &pty_slave, tty_mask) != 0) {
		watcher_stop(mon);
		goto out;
	}
	rc = cmd_enter(r->u.enter.instance, argv);
	watcher_stop(mon);
out:
	fflush(stdout);
	fflush(stderr);
	terminal_close(pty_master, pty_slave);
	stdio_release(c, saved);
	free(argv);
	return rc;
}

static void enter_request(wctx_t* c, uint32_t id, const fp_req_t* r) {
	if (!c->have_stdio) {
		ack_err(c, id, FP_E_INVALID,
			"enter needs the caller's stdio first (STDIO)");
		return;
	}
	if (r->u.enter.instance == NULL || r->u.enter.instance[0] == '\0') {
		ack_err(c, id, FP_E_INVALID, "enter needs an instance");
		return;
	}
	op_start(c, id, 13, "enter");
	int rc = enter_app(c, r);
	op_finish(c, rc, "");
}

// RUN carries its forwarded descriptors, so it is dispatched where they are
// still in hand; handle() below serves the requests that carry none.
static void run_request(wctx_t* c, uint32_t id, fp_req_t* r, int* fds,
			size_t nfds) {
	int targets[FP_MAX_FDS];
	size_t ntargets = 0;

	if (!c->have_stdio) {
		for (size_t i = 0; i < nfds; i++) close(fds[i]);
		ack_err(c, id, FP_E_INVALID,
			"run needs the caller's stdio first (STDIO)");
		return;
	}
	// A terminal gets a pty of ours (the caller's own is relayed, never handed over);
	// a file or pipe is handed over as it is.
	if (!forward_parse(r->u.run.forward, targets, &ntargets) ||
	    ntargets != nfds) {
		for (size_t i = 0; i < nfds; i++) close(fds[i]);
		ack_err(c, id, FP_E_INVALID,
			"the forwarded descriptors do not match the list");
		return;
	}
	op_start(c, id, 11, "run");
	int rc = run_app(c, r, targets, ntargets, fds);
	op_finish(c, rc, "");
}

// A ref, remote name or app id is rendered by clients this worker does not
// own: a control byte there is a new log line or a terminal escape sequence,
// not part of a name.
static bool event_text_safe(const char* s) {
	if (s == NULL) return true;
	for (const unsigned char* c = (const unsigned char*)s; *c; c++)
		if (*c < 0x20 || *c == 0x7f) return false;
	return true;
}

// A changeset ref is a ref: `remote:type/id/arch/branch` or a bare id, each
// component through the same grammar a path or an argv word goes through.
static bool event_ref_ok(const char* s) {
	char buf[512];
	char* colon;

	if (s == NULL || *s == '\0' || strlen(s) >= sizeof(buf)) return false;
	snprintf(buf, sizeof(buf), "%s", s);
	colon = strchr(buf, ':');
	char* p;

	if (colon != NULL) {
		*colon = '\0';
		if (!remote_name_valid(buf)) return false;
		p = colon + 1;
	} else {
		p = buf;
	}
	for (;;) {
		char* slash = strchr(p, '/');

		if (slash != NULL) *slash = '\0';
		if (!fpkg_component_ok(p)) return false;
		if (slash == NULL) break;
		p = slash + 1;
	}
	return true;
}

static void emit_store(const char* action, const char* ref) {
	fp_event_t ev;

	if (!event_text_safe(action) || !event_text_safe(ref)) {
		fpkg_warnf("not announcing a store change with a control byte");
		return;
	}
	memset(&ev, 0, sizeof(ev));
	ev.type = FP_EV_STORE_CHANGED;
	ev.u.store.action = (char*)action;
	ev.u.store.ref = (char*)ref;
	(void)bus_emit_event("store", &ev);
}

static void emit_remote(const char* name) {
	fp_event_t ev;

	if (!event_text_safe(name)) return;
	memset(&ev, 0, sizeof(ev));
	ev.type = FP_EV_REMOTE_CHANGED;
	ev.u.remote.name = (char*)name;
	(void)bus_emit_event("remote", &ev);
}

static void emit_instance(bool started, const char* app_id) {
	char topic[FP_PROTO_MAX_TOPIC + 1];
	char lower[128];
	fp_event_t ev;
	if (app_id == NULL || *app_id == '\0' || !event_text_safe(app_id))
		return;
	// Topics are lowercase by contract; an app id is not.
	snprintf(lower, sizeof(lower), "%s", app_id);
	for (char* p = lower; *p; p++)
		*p = (char)(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p);
	snprintf(topic, sizeof(topic), "instance/%s", lower);
	memset(&ev, 0, sizeof(ev));
	ev.type = started ? FP_EV_INSTANCE_STARTED : FP_EV_INSTANCE_EXITED;
	ev.u.instance.app_id = (char*)app_id;
	(void)bus_emit_event(topic, &ev);
}

static void hello_request(wctx_t* c, uint32_t id, const fp_req_t* r) {
	if (r->u.hello.version != FP_PROTO_VERSION) {
		ack_err(c, id, FP_E_UNSUPPORTED,
			"unsupported protocol version");
		return;
	}
	if (r->u.hello.client == NULL ||
	    !flatpkg_client_name_ok(r->u.hello.client)) {
		ack_err(c, id, FP_E_INVALID, "invalid client name");
		return;
	}
	snprintf(c->client, sizeof(c->client), "%s", r->u.hello.client);
	c->hello = true;
	c->last_client = time(NULL);
	{
		// Past HELLO the connection is an authenticated client that may
		// wait for events; the pre-HELLO timeout has done its work.
		struct timeval tv = {0, 0};

		(void)setsockopt(c->conn, SOL_SOCKET, SO_RCVTIMEO, &tv,
				 sizeof(tv));
	}
	ack_ok(c, id, 0);
}

static void env_request(wctx_t* c, uint32_t id, const fp_req_t* r) {
	worker_env_apply(r->u.env.vars, r->u.env.nvars, c->uid);
	ack_ok(c, id, 0);
}

static void subscribe_request(wctx_t* c, uint32_t id, const fp_req_t* r) {
	if (!fp_topic_valid(r->u.sub.topic) ||
	    bus_subscribe(r->u.sub.topic) != 0)
		ack_err(c, id, FP_E_INVALID, "cannot follow that topic");
	else
		c->pending_sub = id;
}

static void unsubscribe_request(wctx_t* c, uint32_t id, const fp_req_t* r) {
	if (!fp_topic_valid(r->u.sub.topic) ||
	    bus_unsubscribe(r->u.sub.topic) != 0)
		ack_err(c, id, FP_E_INVALID, "cannot drop that topic");
	else {
		ack_ok(c, id, 0);
		if (c->nsubs > 0) c->nsubs--;
	}
}

static void cancel_request(wctx_t* c, uint32_t id, const fp_req_t* r) {
	if (bus_cancel(r->u.cancel.op_id) != 0)
		ack_err(c, id, FP_E_INTERNAL, "cannot reach the broker");
	else
		c->pending_cancel = id;
}

static void changeset_build(const fp_req_t* r, flatpkg_changeset_t* cs) {
	memset(cs, 0, sizeof(*cs));
	if (r->u.apply.nrows == 0) return;
	cs->items = calloc(r->u.apply.nrows, sizeof(*cs->items));
	if (cs->items == NULL) return;
	cs->count = r->u.apply.nrows;
	for (size_t k = 0; k < cs->count; k++) {
		const fp_change_row_t* row = &r->u.apply.rows[k];
		cs->items[k].type = (flatpkg_change_type_t)row->type;
		cs->items[k].ref = strdup(row->ref);
		cs->items[k].old_version = row->old_version != NULL
						   ? strdup(row->old_version)
						   : NULL;
		cs->items[k].new_version = row->new_version != NULL
						   ? strdup(row->new_version)
						   : NULL;
	}
}

static void changeset_rows_free(flatpkg_changeset_t* cs) {
	if (cs->items == NULL) return;
	for (size_t k = 0; k < cs->count; k++) {
		free(cs->items[k].ref);
		free(cs->items[k].old_version);
		free(cs->items[k].new_version);
	}
	free(cs->items);
}

static void changeset_announce(const flatpkg_changeset_t* cs) {
	for (size_t k = 0; k < cs->count; k++) {
		const flatpkg_change_t* row = &cs->items[k];
		const char* action = NULL;
		switch (row->type) {
		case FP_CHANGE_INSTALL: action = "install"; break;
		case FP_CHANGE_UPGRADE: action = "upgrade"; break;
		case FP_CHANGE_DOWNGRADE: action = "downgrade"; break;
		case FP_CHANGE_REMOVE: action = "remove"; break;
		case FP_CHANGE_REINSTALL: action = "reinstall"; break;
		default: break;
		}
		if (action != NULL && event_ref_ok(row->ref))
			emit_store(action, row->ref);
	}
}

static void apply_request(wctx_t* c, uint32_t id, const fp_req_t* r) {
	flatpkg_changeset_t cs;
	int rc = 1;

	op_start(c, id, 14, "apply");
	if (r->u.apply.remote != NULL && r->u.apply.remote[0] != '\0' &&
	    !remote_name_valid(r->u.apply.remote)) {
		op_finish(c, 1, "invalid remote name");
		return;
	}
	changeset_build(r, &cs);
	if (r->u.apply.nrows > 0 && cs.items == NULL) {
		fpkg_errorf("out of memory");
		goto out;
	}
	flatpkg_lock();
	if (changeset_apply(&cs, nz(r->u.apply.remote)) != 0) {
		flatpkg_unlock();
		goto out;
	}
	changeset_announce(&cs);
	flatpkg_unlock();
	rc = 0;
out:
	changeset_rows_free(&cs);
	op_finish(c, rc, "apply failed");
}

static void prefetch_request(wctx_t* c, uint32_t id, const fp_req_t* r) {
	int rc = 0;

	op_start(c, id, 15, "prefetch");
	if (r->u.prefetch.remote != NULL && r->u.prefetch.remote[0] != '\0' &&
	    !remote_name_valid(r->u.prefetch.remote)) {
		op_finish(c, 1, "invalid remote name");
		return;
	}
	for (size_t k = 0; k < r->u.prefetch.nrefs; k++) {
		char* commit = NULL;
		char* meta = NULL;
		if (ostree_fetch_ref_metadata(nz(r->u.prefetch.remote),
					      r->u.prefetch.refs[k], &commit,
					      &meta) != 0) {
			rc = 1;
			continue;
		}
		fp_event_t ev;
		memset(&ev, 0, sizeof(ev));
		ev.type = FP_EV_META;
		ev.u.meta.ref = r->u.prefetch.refs[k];
		ev.u.meta.commit = commit;
		ev.u.meta.metadata = meta;
		send_event(c, &ev);
		free(commit);
		free(meta);
	}
	op_finish(c, rc, "prefetch failed");
}

// A client name belongs to the uid that first declared it: the namespace is
// the caller's own string, and another group member's state is not theirs to
// read or to overwrite.  flatpkg's own name is the installation's, shared by
// the group like the world it writes.
#define FP_CLI_CLIENT "flatpkg"

static bool client_owner_ok(const wctx_t* c) {
	char dir[PATH_MAX], path[PATH_MAX], buf[32], want[32];
	int fd;

	if (strcmp(c->client, FP_CLI_CLIENT) == 0) return true;
	snprintf(dir, sizeof(dir), "%s/clients/.owners", store_root());
	if (mkdir(dir, 0755) != 0 && errno != EEXIST) return false;
	snprintf(path, sizeof(path), "%s/%s", dir, c->client);
	snprintf(want, sizeof(want), "%u\n", (unsigned)c->uid);
	// Trying the create may lose a race with another connection of the same
	// uid: EEXIST is then the file to read, not a refusal.
	for (int attempt = 0; attempt < 2; attempt++) {
		fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
		if (fd >= 0) {
			ssize_t n = read(fd, buf, sizeof(buf) - 1);

			close(fd);
			if (n <= 0) return false;
			buf[n] = '\0';
			return strcmp(buf, want) == 0;
		}
		if (errno != ENOENT) return false;
		fd = open(path,
			  O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
			  0644);
		if (fd >= 0) {
			ssize_t n = write(fd, want, strlen(want));

			close(fd);
			return n == (ssize_t)strlen(want);
		}
		if (errno != EEXIST) return false;
	}
	return false;
}

// The client's state namespace is created on first use, never named by the
// caller.
static bool client_state_dir(const wctx_t* c) {
	char dir[PATH_MAX];

	snprintf(dir, sizeof(dir), "%s/clients", store_root());
	if (mkdir(dir, 0755) != 0 && errno != EEXIST) return false;
	if (!client_owner_ok(c)) return false;
	snprintf(dir, sizeof(dir), "%s/clients/%s", store_root(), c->client);
	return mkdir(dir, 0755) == 0 || errno == EEXIST;
}

static void client_declare_request(wctx_t* c, uint32_t id, const fp_req_t* r) {
	if (!c->hello) {
		ack_err(c, id, FP_E_INVALID, "HELLO first");
		return;
	}
	for (size_t k = 0; k < c->nclient_files; k++)
		free(c->client_files[k]);
	c->nclient_files = 0;
	for (size_t k = 0; k < r->u.client_declare.nnames; k++) {
		const char* name = r->u.client_declare.names[k];
		char path[PATH_MAX];
		if (flatpkg_client_path(c->client, name, path,
					sizeof(path)) != 0) {
			ack_err(c, id, FP_E_INVALID, "invalid state name");
			return;
		}
		c->client_files[c->nclient_files++] = strdup(name);
	}
	if (!client_state_dir(c)) {
		ack_err(c, id, FP_E_INTERNAL,
			"cannot create the client state");
		return;
	}
	ack_ok(c, id, 0);
}

static bool client_file_declared(const wctx_t* c, const char* name) {
	for (size_t k = 0; k < c->nclient_files; k++)
		if (strcmp(c->client_files[k], name) == 0) return true;
	return false;
}

static void client_state_write(wctx_t* c, uint32_t id, const fp_req_t* r,
			       const char* path) {
	if (!client_state_dir(c)) {
		ack_err(c, id, FP_E_INTERNAL, "cannot create the client state");
		return;
	}
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof(tmp), "%s.tmp.%u", path, (unsigned)getpid());
	FILE* f = fopen(tmp, "w");
	if (f == NULL) {
		ack_err(c, id, FP_E_INTERNAL, "cannot write the client state");
		return;
	}
	const char* data = r->u.client_state.data;
	size_t len = strlen(data);
	bool ok = fwrite(data, 1, len, f) == len;
	ok = fflush(f) == 0 && ok;
	ok = fsync(fileno(f)) == 0 && ok;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path) != 0) {
		(void)unlink(tmp);
		ack_err(c, id, FP_E_INTERNAL, "cannot store the client state");
		return;
	}
	ack_ok(c, id, 0);
}

static void client_state_read(wctx_t* c, uint32_t id, const fp_req_t* r,
			      const char* path) {
	op_start(c, id, 16, "state");
	int rc = 0;
	char* data = NULL;
	FILE* f = fopen(path, "r");
	if (f != NULL) {
		if (fseek(f, 0, SEEK_END) == 0) {
			long n = ftell(f);
			if (n >= 0 && n <= 1024 * 1024 &&
			    fseek(f, 0, SEEK_SET) == 0) {
				data = calloc((size_t)n + 1, 1);
				if (data != NULL &&
				    fread(data, 1, (size_t)n, f) !=
					    (size_t)n) {
					free(data);
					data = NULL;
				}
			}
		}
		fclose(f);
	}
	if (data == NULL && errno != ENOENT) rc = 1;
	fp_event_t ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = FP_EV_CLIENT_STATE;
	ev.u.client_state.name = r->u.client_state.name;
	ev.u.client_state.data = data != NULL ? data : (char*)"";
	send_event(c, &ev);
	free(data);
	op_finish(c, rc, "state read failed");
}

static void client_state_request(wctx_t* c, uint32_t id, const fp_req_t* r) {
	char path[PATH_MAX];

	if (!c->hello ||
	    flatpkg_client_path(c->client, r->u.client_state.name, path,
				sizeof(path)) != 0) {
		ack_err(c, id, FP_E_INVALID, "invalid state name");
		return;
	}
	if (!client_file_declared(c, r->u.client_state.name)) {
		ack_err(c, id, FP_E_INVALID, "state file was not declared");
		return;
	}
	if (r->u.client_state.flags & FP_CLIENT_STATE_WRITE)
		client_state_write(c, id, r, path);
	else
		client_state_read(c, id, r, path);
}

static void rollback_request(wctx_t* c, uint32_t id, const fp_req_t* r) {
	op_start(c, id, 5, "rollback");
	int rc = cmd_rollback(r->u.rollback.ref, nz(r->u.rollback.commit));
	if (rc == 0) emit_store("rollback", r->u.rollback.ref);
	op_finish(c, rc, "rollback failed");
}

static void kill_request(wctx_t* c, uint32_t id, const fp_req_t* r) {
	op_start(c, id, 7, "kill");
	int rc = cmd_kill(r->u.kill.instance);
	if (rc == 0) emit_instance(false, r->u.kill.instance);
	op_finish(c, rc, "kill failed");
}

static void import_request(wctx_t* c, uint32_t id, const fp_req_t* r) {
	op_start(c, id, 8, "import");
	int rc = cmd_import(r->u.import.path);
	if (rc == 0) emit_store("import", r->u.import.path);
	op_finish(c, rc, "import failed");
}

static void remote_add_request(wctx_t* c, uint32_t id, const fp_req_t* r) {
	op_start(c, id, 9, "remote-add");
	int rc = cmd_remote_add(r->u.remote_add.name,
				nz(r->u.remote_add.url),
				nz(r->u.remote_add.from),
				nz(r->u.remote_add.data));
	if (rc == 0) emit_remote(r->u.remote_add.name);
	op_finish(c, rc, "remote-add failed");
}

static void remote_delete_request(wctx_t* c, uint32_t id,
				  const fp_req_t* r) {
	op_start(c, id, 10, "remote-delete");
	int rc = cmd_remote_delete(r->u.remote_delete.name);
	if (rc == 0) emit_remote(r->u.remote_delete.name);
	op_finish(c, rc, "remote-delete failed");
}

static void remote_modify_request(wctx_t* c, uint32_t id,
				  const fp_req_t* r) {
	op_start(c, id, 11, "remote-modify");
	int rc = cmd_remote_modify(r->u.remote_modify.name,
				   nz(r->u.remote_modify.changes),
				   nz(r->u.remote_modify.gpg_data));
	if (rc == 0) emit_remote(r->u.remote_modify.name);
	op_finish(c, rc, "remote-modify failed");
}

static void override_request(wctx_t* c, uint32_t id, const fp_req_t* r) {
	op_start(c, id, 10, "override");
	int rc = cmd_override_system(r->u.override.app_id,
				     r->u.override.flags,
				     r->u.override.changes);
	op_finish(c, rc, "override failed");
}

static void exports_request(wctx_t* c, uint32_t id, const fp_req_t* r) {
	op_start(c, id, 13, "exports");
	int rc = cmd_exports();
	op_finish(c, rc, "exports failed");
}

static void appstream_request(wctx_t* c, uint32_t id, const fp_req_t* r) {
	const char* arch = nz(r->u.appstream.arch);
	const char* remote = nz(r->u.appstream.remote);
	char** remotes = NULL;
	size_t nrem = 0;

	if (arch == NULL) arch = "x86_64";
	if (remote != NULL) {
		remotes = malloc(sizeof(*remotes));
		if (remotes == NULL) {
			ack_err(c, id, FP_E_INTERNAL, "out of memory");
			return;
		}
		remotes[0] = strdup(remote);
		nrem = 1;
	} else {
		remotes = ostree_remote_names(&nrem);
	}
	op_start(c, id, 4, "appstream");
	int rc = 0;
	for (size_t i = 0; i < nrem; i++) {
		if (ostree_appstream_deploy(remotes[i], arch) != 0 ||
		    ostree_appstream_extract(remotes[i], arch) != 0) {
			rc = -1;
			break;
		}
	}
	for (size_t i = 0; i < nrem; i++) free(remotes[i]);
	free(remotes);
	op_finish(c, rc, "appstream failed");
}

static void handle(wctx_t* c, uint32_t id, fp_req_t* r) {
	if (!c->hello && r->type != FP_REQ_HELLO) {
		ack_err(c, id, FP_E_INVALID, "the first request must be HELLO");
		return;
	}
	switch (r->type) {
	case FP_REQ_HELLO:
		hello_request(c, id, r);
		return;
	case FP_REQ_ENV:
		env_request(c, id, r);
		return;
	case FP_REQ_SUBSCRIBE:
		subscribe_request(c, id, r);
		return;
	case FP_REQ_UNSUBSCRIBE:
		unsubscribe_request(c, id, r);
		return;
	case FP_REQ_CANCEL:
		cancel_request(c, id, r);
		return;
	case FP_REQ_APPLY:
		apply_request(c, id, r);
		return;
	case FP_REQ_PREFETCH:
		prefetch_request(c, id, r);
		return;
	case FP_REQ_CLIENT_DECLARE:
		client_declare_request(c, id, r);
		return;
	case FP_REQ_CLIENT_STATE:
		client_state_request(c, id, r);
		return;
	case FP_REQ_ROLLBACK:
		rollback_request(c, id, r);
		return;
	case FP_REQ_KILL:
		kill_request(c, id, r);
		return;
	case FP_REQ_IMPORT:
		import_request(c, id, r);
		return;
	case FP_REQ_REMOTE_ADD:
		remote_add_request(c, id, r);
		return;
	case FP_REQ_REMOTE_DELETE:
		remote_delete_request(c, id, r);
		return;
	case FP_REQ_REMOTE_MODIFY:
		remote_modify_request(c, id, r);
		return;
	case FP_REQ_OVERRIDE:
		override_request(c, id, r);
		return;
	case FP_REQ_EXPORTS:
		exports_request(c, id, r);
		return;
	case FP_REQ_APPSTREAM:
		appstream_request(c, id, r);
		return;
	case FP_REQ_INSTALL_BUNDLE:
	case FP_REQ_STDIO:
		ack_err(c, id, FP_E_INVALID, "no descriptors came with it");
		return;
	case FP_REQ_RUN:
		ack_err(c, id, FP_E_INTERNAL,
			"run reached the wrong dispatcher");
		return;
	case FP_REQ_ENTER:
		enter_request(c, id, r);
		return;
	default:
		ack_err(c, id, FP_E_UNSUPPORTED, "unsupported operation");
		return;
	}
}

static int pump_bus(wctx_t* c) {
	char frame[BUS_MAX_MSG];
	size_t len;
	uint32_t type;

	if (c->bus_lost) return 0;
	for (;;) {
		int rc = bus_next(frame, sizeof(frame), &len, &type);
		if (rc == -2) {
			// The broker is gone.  The operation's own events go to
			// the caller over its own connection, so losing the bus
			// mid-operation is not a reason to abandon it.
			c->bus_lost = true;
			if (c->pending_sub != 0) {
				ack_err(c, c->pending_sub, FP_E_INTERNAL,
					"the event bus went away");
				c->pending_sub = 0;
			}
			if (c->pending_cancel != 0) {
				ack_err(c, c->pending_cancel, FP_E_INTERNAL,
					"the event bus went away");
				c->pending_cancel = 0;
			}
			return 0;
		}
		if (rc != 0) return 0;
		if (type == BUS_SUBSCRIBED) {
			c->subscribed = true;
			c->nsubs++;
			if (c->pending_sub != 0) {
				ack_ok(c, c->pending_sub, 0);
				c->pending_sub = 0;
			}
			continue;
		}
		if (type == BUS_SUB_DENIED) {
			if (c->pending_sub != 0)
				ack_err(c, c->pending_sub, FP_E_DENIED,
					"cannot follow that topic");
			c->pending_sub = 0;
			continue;
		}
		if (type == BUS_CANCELED) {
			if (c->pending_cancel != 0) {
				ack_ok(c, c->pending_cancel, 0);
				c->pending_cancel = 0;
			}
			continue;
		}
		if (type == BUS_CANCEL_DENIED) {
			if (c->pending_cancel != 0)
				ack_err(c, c->pending_cancel, FP_E_DENIED,
					"cannot cancel that operation");
			c->pending_cancel = 0;
			continue;
		}
		if (type != BUS_FORWARD) continue;
		if (write_all(c->conn, frame, len) != 0) return -1;
	}
}

void typed_worker(int conn, int bus_fd, uid_t peer_uid, gid_t peer_gid) {
	(void)signal(SIGUSR1, on_cancel);
	(void)signal(SIGHUP, on_client_gone);
	bus_worker_init(bus_fd);
	// Move the control socket out of the forwarded-descriptor range (FP_MAX_FDS): a
	// dup2 onto a caller target must not overwrite the connection this worker answers on.
	int high = fcntl(conn, F_DUPFD_CLOEXEC, FP_MAX_FDS);
	if (high < 0) {
		fpkg_errorf("cannot move the control socket: %s",
			    strerror(errno));
		return;
	}
	close(conn);
	conn = high;

	wctx_t c = {.conn = conn,
		    .uid = peer_uid,
		    .gid = peer_gid,
		    .last_client = time(NULL)};
	char uidb[32], gidb[32];

	// A fresh environment: the caller's comes over the wire, allowlisted.
	clearenv();
	setenv("PATH",
	       "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
	       1);
	snprintf(uidb, sizeof(uidb), "%u", (unsigned)peer_uid);
	snprintf(gidb, sizeof(gidb), "%u", (unsigned)peer_gid);
	setenv("FLATPKG_APP_UID", uidb, 1);
	setenv("FLATPKG_APP_GID", gidb, 1);
	struct passwd* pw = getpwuid(peer_uid);
	if (pw != NULL) {
		setenv("FLATPKG_APP_USER", pw->pw_name, 1);
		setenv("FLATPKG_APP_HOME", pw->pw_dir, 1);
		setenv("HOME", pw->pw_dir, 1);
	}
	// No pty here, so no descriptor for the app's terminal: cleared so a
	// value the caller exported cannot survive.
	unsetenv("FLATPKG_APP_TTY");
	unsetenv("FLATPKG_APP_MASTER");
	setenv("FLATPKG_INTERACTIVE", "0", 1);

	// The tools the primitives start must not inherit the socket or read
	// the client's stdin, and must be waitable.
	(void)fcntl(conn, F_SETFD, FD_CLOEXEC);
	int dn = open("/dev/null", O_RDONLY);
	if (dn >= 0) {
		if (dup2(dn, STDIN_FILENO) < 0)
			fpkg_warnf("cannot redirect stdin: %s",
				   strerror(errno));
		if (dn > 2) close(dn);
	}
	(void)signal(SIGCHLD, SIG_DFL);
	(void)signal(SIGPIPE, SIG_IGN);

	for (;;) {
		// Forwarded events first: a follower waiting on this topic must
		// not wait for the next client request.
		if (pump_bus(&c) != 0) break;
		struct pollfd pfd[2] = {{conn, POLLIN, 0}, {bus_fd, POLLIN, 0}};
		int timeout = -1;
		if (c.hello && c.nsubs == 0) {
			time_t idle = time(NULL) - c.last_client;

			if (idle >= FP_IDLE_TIMEOUT) break;
			timeout = (int)(FP_IDLE_TIMEOUT - idle) * 1000;
			if (timeout < 1000) timeout = 1000;
		}
		int pr;
		do pr = poll(pfd, 2, timeout);
		while (pr < 0 && errno == EINTR);
		if (pr == 0) break;
		if (pr < 0) break;
		if ((pfd[1].revents & (POLLIN | POLLHUP)) != 0 &&
		    pump_bus(&c) != 0)
			break;
		if ((pfd[0].revents & POLLIN) == 0) continue;

		c.last_client = time(NULL);
		nvlist_t* nvl = NULL;
		int rc = fp_frame_recv(conn, &nvl);
		if (rc != FP_P_OK) {
			nvlist_destroy(nvl);
			if (rc != FP_P_EOF)
				fpkg_warnf("dropping a connection: %s",
					   fp_proto_strerror(rc));
			break;
		}
		fp_req_t req;
		uint32_t id = 0;
		rc = fp_req_decode(nvl, &id, &req);
		if (rc != FP_P_OK) {
			ack_err(&c, id, FP_E_INVALID, "malformed request");
			nvlist_destroy(nvl);
			if (c.dead) break;
			if (++c.bad_frames >= FP_MAX_BAD_FRAMES) {
				fpkg_warnf("closing a connection after %zu "
					   "malformed requests",
					   c.bad_frames);
				break;
			}
			continue;
		}
		// The three requests that carry descriptors are handled here,
		// before handle()'s gate: a bundle or a terminal without a
		// HELLO is the same untrusted connection as any other.
		if (!c.hello && (req.type == FP_REQ_STDIO ||
				 req.type == FP_REQ_INSTALL_BUNDLE ||
				 req.type == FP_REQ_RUN)) {
			nvlist_destroy(nvl); // closes the frame's descriptors
			ack_err(&c, id, FP_E_INVALID,
				"the first request must be HELLO");
			fp_req_free(&req);
			if (c.dead) break;
			continue;
		}
		if (req.type == FP_REQ_STDIO) {
			static const char* const names[FP_STDIO_FDS] = {
			    FP_FD_STDIN, FP_FD_STDOUT, FP_FD_STDERR};
			int fds[FP_STDIO_FDS] = {-1, -1, -1};
			int nfds = 0;
			bool ok = fp_nvl_fd_count(nvl) == FP_STDIO_FDS;

			for (int i = 0; ok && i < FP_STDIO_FDS; i++) {
				if (!nvlist_exists_descriptor(nvl, names[i])) {
					ok = false;
					break;
				}
				fds[nfds++] =
					nvlist_take_descriptor(nvl, names[i]);
			}
			if (!ok) {
				for (int i = 0; i < nfds; i++) close(fds[i]);
				ack_err(&c, id, FP_E_INVALID,
					"STDIO carries three descriptors");
			} else if (c.have_stdio) {
				for (int i = 0; i < nfds; i++) close(fds[i]);
				ack_err(&c, id, FP_E_INVALID,
					"STDIO was already given");
			} else {
				for (int i = 0; i < nfds; i++) {
					c.stdio[i] = fds[i];
					(void)fcntl(fds[i], F_SETFD,
						    FD_CLOEXEC);
				}
				c.have_stdio = true;
				ack_ok(&c, id, 0);
			}
			nvlist_destroy(nvl);
			fp_req_free(&req);
			if (c.dead) break;
			continue;
		}
		if (req.type == FP_REQ_INSTALL_BUNDLE) {
			int bfd = -1;
			bool ok = fp_nvl_fd_count(nvl) == FP_BUNDLE_FDS &&
				  nvlist_exists_descriptor(nvl,
							   FP_FD_BUNDLE);

			if (ok) bfd = nvlist_take_descriptor(nvl, FP_FD_BUNDLE);
			if (!ok || bfd < 0) {
				if (bfd >= 0) close(bfd);
				ack_err(&c, id, FP_E_INVALID,
					"a bundle comes as exactly one "
					"descriptor");
			} else {
				(void)fcntl(bfd, F_SETFD, FD_CLOEXEC);
				op_start(&c, id, 12, "install-bundle");
				int brc = install_bundle_fd(bfd);
				close(bfd);
				op_finish(&c, brc,
					  "install from the bundle failed");
			}
			nvlist_destroy(nvl);
			fp_req_free(&req);
			if (c.dead) break;
			continue;
		}
		if (req.type == FP_REQ_RUN) {
			int targets[FP_MAX_FDS];
			int fds[FP_MAX_FDS];
			size_t ntargets = 0, nfds = 0;
			bool ok = forward_parse(req.u.run.forward, targets,
						&ntargets) &&
				  fp_nvl_fd_count(nvl) == ntargets;

			for (size_t i = 0; ok && i < ntargets; i++) {
				char name[32];

				snprintf(name, sizeof(name),
					 FP_FD_PREFIX "%d", targets[i]);
				if (!nvlist_exists_descriptor(nvl, name)) {
					ok = false;
					break;
				}
				fds[nfds++] =
					nvlist_take_descriptor(nvl, name);
			}
			if (!ok) {
				for (size_t i = 0; i < nfds; i++)
					close(fds[i]);
				ack_err(&c, id, FP_E_INVALID,
					"the forwarded descriptors do not "
					"match the list");
			} else {
				run_request(&c, id, &req, fds, nfds);
			}
			nvlist_destroy(nvl);
			fp_req_free(&req);
			if (c.dead) break;
			continue;
		}
		// No other request carries a descriptor; one that does is not the
		// frame it claims to be.
		if (fp_nvl_fd_count(nvl) != 0) {
			ack_err(&c, id, FP_E_INVALID,
				"this request carries no descriptors");
			nvlist_destroy(nvl);
			fp_req_free(&req);
			if (c.dead) break;
			continue;
		}
		handle(&c, id, &req);
		nvlist_destroy(nvl);
		fp_req_free(&req);
		if (c.dead) break;
	}
	for (size_t i = 0; i < c.nclient_files; i++)
		free(c.client_files[i]);
}
