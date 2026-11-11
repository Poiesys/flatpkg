#include "flatpkg.h"
#include "cli.h"
#define FPKG_MODULE "client"
#include "fpkglog.h"
#include "protocol.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void flatpkg_host_modules_emit(const char* msg) {
	char line[512];

	while (msg != NULL && *msg != '\0') {
		const char* nl = strchr(msg, '\n');
		size_t n = nl ? (size_t)(nl - msg) : strlen(msg);
		if (n >= sizeof(line)) n = sizeof(line) - 1;
		memcpy(line, msg, n);
		line[n] = '\0';
		if (n > 0) fpkg_errorf("%s", line);
		msg = nl ? nl + 1 : NULL;
	}
}

// A ref the daemon can take as written: a complete id (the same test
// install_req_prepare() uses to decide a lookup is needed) and a branch.
// Anything else is a choice, and the choice is this side's.
static bool ref_is_pinned(const char* s) {
	fpkg_ref_t r;

	if (s == NULL || ref_parse(s, &r) != 0) return false;
	bool complete = strchr(r.id, '.') != NULL || strchr(r.id, '/') != NULL;
	bool pinned = complete && r.branch != NULL;
	ref_free(&r);
	return pinned;
}

// FLATPKG_FORWARD_FDS names descriptors to carry into the sandbox on those same
// numbers. Mismatched numbers are refused, not dropped: silently losing one fails later.
static int forward_targets(int* fds, size_t* nfds, char* list, size_t listsz) {
	const char* s = cli_getenv("FLATPKG_FORWARD_FDS");
	const char* bad_at = NULL;
	size_t n = 0, off = 0;

	list[0] = '\0';
	*nfds = 0;
	if (s == NULL || *s == '\0') return 0;
	while (*s) {
		char* end;
		long v = strtol(s, &end, 10);
		if (end == s) {
			bad_at = s;
			goto bad;
		}
		if (v < 3 || v >= FP_MAX_FDS || fcntl((int)v, F_GETFD) < 0) {
			bad_at = s;
			goto bad;
		}
		for (size_t i = 0; i < n; i++)
			if (fds[i] == (int)v) {
				bad_at = s;
				goto bad;
			}
		if (n == FP_MAX_FDS) {
			bad_at = s;
			goto bad;
		}
		fds[n++] = (int)v;
		if (*end == '\0') break;
		if (*end != ',') {
			bad_at = end;
			goto bad;
		}
		s = end + 1;
		if (*s == '\0') {
			bad_at = end;
			goto bad;
		}
	}
	for (size_t i = 0; i < n; i++) {
		int k = snprintf(list + off, listsz - off, "%s%d", i ? "," : "",
				 fds[i]);
		if (k < 0 || (size_t)k >= listsz - off) {
			fpkg_errorf("FLATPKG_FORWARD_FDS is too long");
			return -1;
		}
		off += (size_t)k;
	}
	*nfds = n;
	return 0;
bad:
	fpkg_errorf("FLATPKG_FORWARD_FDS: %s is not an open descriptor between "
		    "3 and %d",
		    bad_at, FP_MAX_FDS - 1);
	return -1;
}

// remote-modify's settings as `key=value` lines, one per line: the daemon
// parses what the CLI parsed, so neither side keeps a second vocabulary.
static char* remote_modify_text(const cli_req_t* req) {
	struct {
		const char* key;
		const char* val;
	} rows[] = {
		{"url", req->url},
		{"branch", req->branch_name},
		{"title", req->title},
		{"comment", req->comment},
		{"description", req->description},
		{"homepage", req->homepage},
		{"icon", req->icon},
	};
	char buf[8192] = "";
	size_t off = 0;

	for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
		if (rows[i].val == NULL) continue;
		if (strchr(rows[i].val, '\n') != NULL) {
			fpkg_errorf("--%s cannot hold a newline", rows[i].key);
			return NULL;
		}
		if (!fpkg_buf_append(buf, sizeof(buf), &off, "%s=%s\n",
				     rows[i].key, rows[i].val)) {
			fpkg_errorf("too many remote changes");
			return NULL;
		}
	}
	if (req->gpg_verify >= 0 &&
	    !fpkg_buf_append(buf, sizeof(buf), &off, "gpg-verify=%d\n",
			     req->gpg_verify)) {
		fpkg_errorf("too many remote changes");
		return NULL;
	}
	return xstrdup(buf);
}

// A word naming a file on the caller's side.
static bool caller_repo_path(const char* s) {
	return s != NULL && (strncmp(s, "file://", 7) == 0 || s[0] == '/');
}

// Read a repo file here, where the caller's privileges are.  The bytes are all
// the daemon sees; it never opens a path the caller named.  A repo file is a
// few KB of text, so the cap is the point as much as the grammar.
static char* read_repo_file(const char* word) {
	const char* path = strncmp(word, "file://", 7) == 0 ? word + 7 : word;
	FILE* f = fopen(path, "r");
	char* buf;
	size_t n;
	bool err;

	if (f == NULL) {
		fpkg_errorf("cannot read %s: %s", path, strerror(errno));
		return NULL;
	}
	buf = malloc(REPO_FILE_MAX + 1);
	if (buf == NULL) {
		fclose(f);
		fpkg_errorf("out of memory");
		return NULL;
	}
	n = fread(buf, 1, REPO_FILE_MAX + 1, f);
	err = ferror(f) != 0;
	fclose(f);
	if (err) {
		fpkg_errorf("cannot read %s", path);
		free(buf);
		return NULL;
	}
	if (n > REPO_FILE_MAX) {
		fpkg_errorf("%s is too large for a repo file", path);
		free(buf);
		return NULL;
	}
	buf[n] = '\0';
	if (strlen(buf) != n) {
		fpkg_errorf("%s is not a text repo file", path);
		free(buf);
		return NULL;
	}
	return buf;
}

// Read the caller's key file here, where the caller's privileges are: the
// daemon must never open a path the caller named as root (that turned group
// membership into a read of any root-owned file).  Only an armored key is
// accepted, and only up to the size the protocol carries.
#define KEY_FILE_MAX (64 * 1024)
static char* read_key_file(const char* path) {
	FILE* f = fopen(path, "r");
	if (f == NULL) {
		fpkg_errorf("cannot read %s: %s", path, strerror(errno));
		return NULL;
	}
	char* buf = malloc(KEY_FILE_MAX + 1);
	if (buf == NULL) {
		fclose(f);
		fpkg_errorf("out of memory");
		return NULL;
	}
	size_t n = fread(buf, 1, KEY_FILE_MAX + 1, f);
	bool err = ferror(f) != 0;
	fclose(f);
	if (err) {
		fpkg_errorf("cannot read %s", path);
		free(buf);
		return NULL;
	}
	if (n > KEY_FILE_MAX) {
		fpkg_errorf("%s is too large for a key file", path);
		free(buf);
		return NULL;
	}
	buf[n] = '\0';
	if (strlen(buf) != n) {
		fpkg_errorf("%s is not a text key file", path);
		free(buf);
		return NULL;
	}
	if (strncmp(buf, "-----BEGIN PGP", 14) != 0) {
		fpkg_errorf("%s is not an armored PGP key", path);
		free(buf);
		return NULL;
	}
	return buf;
}

static void free_refs(char** v, size_t n) {
	for (size_t i = 0; i < n; i++) free(v[i]);
}

// Returns false when the request cannot be framed.  `reported` says the builder
// already named the reason, so the caller must not add a vaguer one.
static bool typed_request(const cli_req_t* req, fp_req_t* out, char** owned,
			  bool* reported) {
	const char* const* op = req->ops;
	size_t n = req->nops;

	memset(out, 0, sizeof(*out));
	*reported = false;
	switch (cli_req_cmd(req)) {
	case CMD_REMOTE_ADD: {
		const char* from = req->from;
		const char* url = n > 1 ? op[1] : "";

		out->type = FP_REQ_REMOTE_ADD;
		out->u.remote_add.name = (char*)(n > 0 ? op[0] : "");
		// A repo file is the caller's to read: it is the key's source,
		// and root opening a caller-named path is exactly the primitive
		// the key import no longer has.
		if (from != NULL && caller_repo_path(from)) {
			*owned = read_repo_file(from);
			if (*owned == NULL) {
				*reported = true;
				return false;
			}
			out->u.remote_add.data = *owned;
			from = "";
		} else if (from == NULL && caller_repo_path(url) &&
			   strstr(url, ".flatpakrepo") != NULL) {
			*owned = read_repo_file(url);
			if (*owned == NULL) {
				*reported = true;
				return false;
			}
			out->u.remote_add.data = *owned;
			url = "";
		}
		out->u.remote_add.url = (char*)url;
		out->u.remote_add.from = (char*)(from != NULL ? from : "");
		return n > 0;
	}
	case CMD_REMOTE_DELETE:
		out->type = FP_REQ_REMOTE_DELETE;
		out->u.remote_delete.name = (char*)(n > 0 ? op[0] : "");
		return n > 0;
	case CMD_REMOTE_MODIFY:
		out->type = FP_REQ_REMOTE_MODIFY;
		out->u.remote_modify.name = (char*)(n > 0 ? op[0] : "");
		*owned = remote_modify_text(req);
		if (*owned == NULL) {
			*reported = true;
			return false;
		}
		out->u.remote_modify.changes = *owned;
		if (req->gpg_import != NULL) {
			out->u.remote_modify.gpg_data =
				read_key_file(req->gpg_import);
			if (out->u.remote_modify.gpg_data == NULL) {
				*reported = true;
				return false;
			}
		}
		return n > 0;
	case CMD_OVERRIDE:
		// Only the system scope is a request (cli_req_is_local() keeps the rest in the
		// caller's process). The text is allocated here and owned by the caller.
		out->type = FP_REQ_OVERRIDE;
		out->u.override.app_id = (char*)(n > 0 ? op[0] : "");
		out->u.override.flags = req->over_reset ? FP_OVERRIDE_RESET : 0;
		*owned = overrides_to_text(&req->over);
		if (*owned == NULL) return false;
		out->u.override.changes = *owned;
		return true;
	case CMD_KILL:
		out->type = FP_REQ_KILL;
		out->u.kill.instance = (char*)(n > 0 ? op[0] : "");
		return n > 0;
	case CMD_IMPORT:
		out->type = FP_REQ_IMPORT;
		out->u.import.path = (char*)(n > 0 ? op[0] : "");
		return n > 0;
	case CMD_ROLLBACK:
		out->type = FP_REQ_ROLLBACK;
		out->u.rollback.ref = (char*)(n > 0 ? op[0] : "");
		out->u.rollback.commit = (char*)(n > 1 ? op[1] : "");
		return n > 0;
	case CMD_INSTALL:
		if (req->from == NULL) return false;
		out->type = FP_REQ_INSTALL_BUNDLE;
		return true;
	case CMD_RUN: {
		// A pipe or file is handed over as it is; a terminal becomes a
		// pty of the daemon's own, so the app never holds the user's.
		static char* noargs[1] = {NULL};
		size_t nrest = 0;
		if (n == 0) return false;
		while (req->rest != NULL && req->rest[nrest] != NULL) nrest++;
		out->type = FP_REQ_RUN;
		out->u.run.app_id = (char*)op[0];
		out->u.run.args =
			req->rest != NULL ? (char**)req->rest : noargs;
		out->u.run.nargs = nrest;
		out->u.run.command =
			req->run.command ? req->run.command : (char*)"";
		out->u.run.branch =
			req->run.branch ? req->run.branch : (char*)"";
		out->u.run.arch = req->run.arch ? req->run.arch : (char*)"";
		out->u.run.flags =
			(req->run.devel ? FP_RUN_DEVEL : 0) |
			(req->run.clearenv ? FP_RUN_CLEARENV : 0) |
			(req->run.die_with_parent ? FP_RUN_DIE_WITH_PARENT
						  : 0) |
			(req->run.file_forwarding ? FP_RUN_FILE_FORWARDING : 0);
		out->u.run.sockets = req->run.sockets.items;
		out->u.run.nsockets = req->run.sockets.count;
		out->u.run.devices = req->run.devices.items;
		out->u.run.ndevices = req->run.devices.count;
		out->u.run.shares = req->run.shares.items;
		out->u.run.nshares = req->run.shares.count;
		out->u.run.fs_add = req->run.fs_add.items;
		out->u.run.nfs_add = req->run.fs_add.count;
		out->u.run.fs_remove = req->run.fs_remove.items;
		out->u.run.nfs_remove = req->run.fs_remove.count;
		out->u.run.envs = req->run.envs.items;
		out->u.run.nenvs = req->run.envs.count;
		return true;
	}
	case CMD_ENTER: {
		static char* nocommand[1] = {NULL};
		size_t ncmd = 0;
		if (n == 0) return false;
		while (req->rest != NULL && req->rest[ncmd] != NULL) ncmd++;
		out->type = FP_REQ_ENTER;
		out->u.enter.instance = (char*)op[0];
		out->u.enter.command =
			req->rest != NULL ? (char**)req->rest : nocommand;
		out->u.enter.ncommand = ncmd;
		return true;
	}
	case CMD_EXPORTS:
		out->type = FP_REQ_EXPORTS;
		return true;
	default:
		return false;
	}
}

static int send_req(int fd, uint32_t id, const fp_req_t* r) {
	fp_wbuf_t w;
	fp_w_init(&w);
	if (fp_req_encode(&w, id, r) != 0) {
		fp_w_free(&w);
		return EIO;
	}
	int e = fp_frame_write(fd, w.p, w.len);
	fp_w_free(&w);
	return e == FP_P_OK ? 0 : (errno ? errno : EIO);
}

static int read_ack(int fd, uint64_t* op_id) {
	fp_frame_hdr_t hdr;
	uint8_t* payload = NULL;
	fp_ack_t ack;

	if (fp_frame_read(fd, &hdr, &payload) != FP_P_OK) {
		free(payload);
		return -1;
	}
	int e = fp_ack_decode(&hdr, payload, &ack);
	free(payload);
	if (e != 0) return -1;
	int rc = 0;
	if (ack.type == FP_ACK_ERR) {
		fpkg_errorf("%s", ack.message ? ack.message : "refused");
		rc = 1;
	} else if (op_id != NULL) {
		*op_id = ack.op_id;
	}
	fp_ack_free(&ack);
	return rc;
}

static void send_env(int fd, uint32_t id) {
	char** env = cli_environ();
	size_t n = 0;
	while (env != NULL && env[n] != NULL) n++;
	if (n > 512) n = 512;
	fp_req_t r;
	memset(&r, 0, sizeof(r));
	r.type = FP_REQ_ENV;
	r.u.env.vars = env;
	r.u.env.nvars = n;
	if (send_req(fd, id, &r) == 0) (void)read_ack(fd, NULL);
}

static void render_log(const fp_event_t* ev) {
	fp_logf(ev->u.log.scope && *ev->u.log.scope ? ev->u.log.scope
						    : "daemon",
		"", (int)ev->u.log.level, "%s",
		ev->u.log.text ? ev->u.log.text : "");
}

static int prefetch_meta(int fd, const char* const* refs, size_t n) {
	fp_req_t r;
	fp_wbuf_t w;
	int e;

	if (n == 0) return 0;
	memset(&r, 0, sizeof(r));
	r.type = FP_REQ_PREFETCH;
	r.u.prefetch.remote = "";
	r.u.prefetch.refs = (char**)refs;
	r.u.prefetch.nrefs = n;
	fp_w_init(&w);
	e = fp_req_encode(&w, 3, &r);
	if (e == FP_P_OK) e = fp_frame_write(fd, w.p, w.len);
	fp_w_free(&w);
	if (e != FP_P_OK || read_ack(fd, NULL) != 0) return 1;

	for (;;) {
		fp_frame_hdr_t h;
		uint8_t* payload = NULL;
		fp_event_t ev;
		if (fp_frame_read(fd, &h, &payload) != FP_P_OK) {
			free(payload);
			return 1;
		}
		e = fp_event_decode(&h, payload, &ev);
		free(payload);
		if (e != FP_P_OK) return 1;
		if (ev.type == FP_EV_META) {
			if (ev.u.meta.ref == NULL || ev.u.meta.metadata == NULL ||
			    ostree_plan_meta_add(ev.u.meta.ref,
						 ev.u.meta.metadata) != 0) {
				fp_event_free(&ev);
				return 1;
			}
		} else if (ev.type == FP_EV_OP_LOG) {
			render_log(&ev);
		} else if (ev.type == FP_EV_OP_FINISHED) {
			int rc = (int)ev.u.finished.status;
			fp_event_free(&ev);
			return rc == 0 ? 0 : 1;
		}
		fp_event_free(&ev);
	}
}

// A file in this client's own state directory.  It is not package data:
// the daemon only maps it to clients/<hello client>/<name>.
static int state_declare(int fd, uint32_t id, const char* name) {
	fp_req_t r;
	fp_wbuf_t w;
	char* names[1] = {(char*)name};
	int e;
	memset(&r, 0, sizeof(r));
	r.type = FP_REQ_CLIENT_DECLARE;
	r.u.client_declare.names = names;
	r.u.client_declare.nnames = 1;
	fp_w_init(&w);
	e = fp_req_encode(&w, id, &r);
	if (e == FP_P_OK) e = fp_frame_write(fd, w.p, w.len);
	fp_w_free(&w);
	if (e != FP_P_OK) return -1;
	return read_ack(fd, NULL) == 0 ? 0 : -1;
}

static int state_write(int fd, uint32_t id, const char* name,
		       const char* data) {
	fp_req_t r;
	fp_wbuf_t w;
	int e;
	memset(&r, 0, sizeof(r));
	r.type = FP_REQ_CLIENT_STATE;
	r.u.client_state.flags = FP_CLIENT_STATE_WRITE;
	r.u.client_state.name = (char*)name;
	r.u.client_state.data = (char*)data;
	fp_w_init(&w);
	e = fp_req_encode(&w, id, &r);
	if (e == FP_P_OK) e = fp_frame_write(fd, w.p, w.len);
	fp_w_free(&w);
	if (e != FP_P_OK) return -1;
	return read_ack(fd, NULL) == 0 ? 0 : -1;
}

typedef struct {
	cli_req_t req;
	fp_req_t typed;
	char* owned;
	int fd;
	bool plan_install;
	bool plan_remove;
	bool plan_upgrade;
	bool have_plan;
	uint32_t req_id;
	int fwd[FP_MAX_FDS];
	size_t nfwd;
	char fwlist[512];
	char* ref_store[64];
	const char* refs[64];
	size_t nresolved;
	fpkg_plan_t plan;
	fp_change_row_t plan_rows[FP_APPLY_MAX];
	char* plan_world;
} typed_ctx_t;

static void typed_ctx_free(typed_ctx_t* c) {
	if (c->fd >= 0) close(c->fd);
	if (c->have_plan) fpkg_plan_free(&c->plan);
	free(c->plan_world);
	free_refs(c->ref_store, c->nresolved);
	cli_req_free(&c->req);
	free(c->owned);
}

static int typed_connect(typed_ctx_t* c) {
	struct sockaddr_un sa;

	ignore_sigpipe();
	c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (c->fd < 0) return daemon_unreachable(errno);
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (strlcpy(sa.sun_path, FPKG_SOCKET, sizeof(sa.sun_path)) >=
	    sizeof(sa.sun_path))
		return daemon_unreachable(ENAMETOOLONG);
	if (connect(c->fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
		int err = errno;
		return daemon_unreachable(err);
	}
	return 0;
}

static int typed_handshake(typed_ctx_t* c) {
	fp_req_t hello;
	char why[768];
	int e;

	// Refuse here, before anything runs: the missing modules are a fact
	// about this host and the user is the one who can load them.
	if (flatpkg_host_modules_message(why, sizeof(why))) {
		flatpkg_host_modules_emit(why);
		return 1;
	}
	memset(&hello, 0, sizeof(hello));
	hello.type = FP_REQ_HELLO;
	hello.u.hello.version = FP_PROTO_VERSION;
	hello.u.hello.client = "flatpkg";
	e = send_req(c->fd, 1, &hello);
	if (e != 0 || read_ack(c->fd, NULL) != 0)
		return daemon_unreachable(e ? e : EIO);
	send_env(c->fd, 2);
	if ((c->plan_install || c->plan_remove || c->plan_upgrade) &&
	    state_declare(c->fd, 8, "world") != 0) {
		fpkg_errorf("flatpkgd refused the client state");
		return 1;
	}
	return 0;
}

static int typed_send_stdio(typed_ctx_t* c) {
	fp_req_t sio;
	fp_wbuf_t w;
	int fds[FP_STDIO_FDS] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};

	// The daemon relays this stdin to the app's pty, so it must arrive unedited and
	// unechoed; the pty does both. Only stdin: stdout and stderr may be files.
	if (isatty(STDIN_FILENO)) client_enter_raw_stdin();
	// The caller's own stdio goes with the run, not through us.
	memset(&sio, 0, sizeof(sio));
	sio.type = FP_REQ_STDIO;
	sio.u.stdio.count = FP_STDIO_FDS;
	fp_w_init(&w);
	if (fp_req_encode(&w, 4, &sio) != 0 ||
	    fp_frame_write_fds(c->fd, w.p, w.len, fds, FP_STDIO_FDS) !=
		    FP_P_OK ||
	    read_ack(c->fd, NULL) != 0) {
		fp_w_free(&w);
		return daemon_unreachable(EIO);
	}
	fp_w_free(&w);
	return 0;
}

// A partial name or a missing branch is the caller's choice: the daemon
// takes exact refs only, and the menu needs the terminal this process has.
static int typed_resolve_refs(typed_ctx_t* c) {
	size_t n = c->req.nops;

	for (size_t k = 0; k < n; k++) {
		if (ref_is_pinned(c->req.ops[k])) {
			c->refs[k] = c->req.ops[k];
			continue;
		}
		c->ref_store[k] = ostree_resolve_install_request(
			c->req.ops[k], c->req.runtime);
		if (c->ref_store[k] == NULL) return 1;
		c->refs[k] = c->ref_store[k];
		c->nresolved = k + 1;
	}
	return 0;
}

static bool typed_apply_from_plan(typed_ctx_t* c, const char* what) {
	c->plan_world = world_to_text(&c->plan.world);
	if (c->plan_world == NULL || c->plan.cs.count > FP_APPLY_MAX) {
		fpkg_errorf("cannot encode the %s plan", what);
		free(c->plan_world);
		c->plan_world = NULL;
		return false;
	}
	for (size_t k = 0; k < c->plan.cs.count; k++) {
		c->plan_rows[k].type = (uint32_t)c->plan.cs.items[k].type;
		c->plan_rows[k].ref = c->plan.cs.items[k].ref;
		c->plan_rows[k].old_version =
			c->plan.cs.items[k].old_version;
		c->plan_rows[k].new_version =
			c->plan.cs.items[k].new_version;
	}
	memset(&c->typed, 0, sizeof(c->typed));
	c->typed.type = FP_REQ_APPLY;
	c->typed.u.apply.flags = 0;
	c->typed.u.apply.remote =
		c->plan.remote != NULL ? c->plan.remote : "";
	c->typed.u.apply.rows = c->plan_rows;
	c->typed.u.apply.nrows = c->plan.cs.count;
	return true;
}

static bool typed_plan_install(typed_ctx_t* c, int* status) {
	flatpkg_dep_kind_t kinds[FP_APPLY_MAX];
	const char* commits[FP_APPLY_MAX];
	const char* prefetch_refs[FP_APPLY_MAX];
	size_t nprefetch = 0;
	size_t n = c->req.nops;

	for (size_t k = 0; k < n; k++) {
		kinds[k] = c->req.runtime ? FLATPKG_DEP_KIND_RUNTIME
					  : FLATPKG_DEP_KIND_APP;
		commits[k] = c->req.commit;
		if (kinds[k] == FLATPKG_DEP_KIND_APP)
			prefetch_refs[nprefetch++] = c->refs[k];
	}
	if (prefetch_meta(c->fd, prefetch_refs, nprefetch) != 0 ||
	    ostree_plan_install(c->refs, commits, kinds, n, c->req.reinstall,
				&c->plan) != 0) {
		ostree_plan_meta_reset();
		*status = 1;
		return false;
	}
	c->have_plan = true;
	if (c->req.g.simulate) {
		fpkg_infof("(simulate: no changes made)");
		ostree_plan_meta_reset();
		*status = 0;
		return false;
	}
	if (!c->req.assume_yes && confirm_proceed(0) != 0) {
		fpkg_errorf("aborted");
		ostree_plan_meta_reset();
		*status = 1;
		return false;
	}
	if (!typed_apply_from_plan(c, "install")) {
		ostree_plan_meta_reset();
		*status = 1;
		return false;
	}
	ostree_plan_meta_reset();
	return true;
}

static bool typed_plan_remove(typed_ctx_t* c, int* status) {
	if (ostree_plan_remove((const char**)c->req.ops, c->req.nops,
			       c->req.rdepends, &c->plan) != 0) {
		*status = 1;
		return false;
	}
	c->have_plan = true;
	if (c->req.g.simulate) {
		fpkg_infof("(simulate: no changes made)");
		*status = c->plan.absent && !c->plan.had_work ? 1 : 0;
		return false;
	}
	if (!c->req.assume_yes && confirm_proceed(0) != 0) {
		fpkg_errorf("aborted");
		*status = 1;
		return false;
	}
	if (!typed_apply_from_plan(c, "removal")) {
		*status = 1;
		return false;
	}
	return true;
}

static bool typed_plan_upgrade(typed_ctx_t* c, int* status) {
	unsigned uf = FLATPKG_SOLVERF_UPGRADE;

	if (c->req.prune) uf |= FLATPKG_SOLVERF_PRUNE;
	if (c->req.available) uf |= FLATPKG_SOLVERF_AVAILABLE;
	if (c->req.g.simulate) uf |= FLATPKG_SOLVERF_SIMULATE;
	if (ostree_plan_upgrade(uf, (const char* const*)c->req.ops,
				c->req.nops, &c->plan) != 0) {
		*status = 1;
		return false;
	}
	c->have_plan = true;
	if (!c->plan.had_work) {
		*status = 0;
		return false;
	}
	if (c->req.g.simulate) {
		fpkg_infof("(simulate: no changes made)");
		*status = 0;
		return false;
	}
	if (!c->req.assume_yes && confirm_proceed(0) != 0) {
		fpkg_errorf("aborted");
		*status = 1;
		return false;
	}
	if (!typed_apply_from_plan(c, "upgrade")) {
		*status = 1;
		return false;
	}
	return true;
}

static bool typed_make_plan(typed_ctx_t* c, int* status) {
	if (c->plan_install) return typed_plan_install(c, status);
	if (c->plan_remove) return typed_plan_remove(c, status);
	if (c->plan_upgrade) return typed_plan_upgrade(c, status);
	return true;
}

static int typed_send_request(typed_ctx_t* c) {
	uint64_t op_id = 0;
	int e;

	if (c->typed.type == FP_REQ_INSTALL_BUNDLE) {
		int bfd = open(c->req.from, O_RDONLY | O_CLOEXEC);
		if (bfd < 0) {
			fpkg_errorf("cannot open %s: %s", c->req.from,
				    strerror(errno));
			return 1;
		}
		fp_wbuf_t w;
		fp_w_init(&w);
		e = fp_req_encode(&w, c->req_id, &c->typed);
		if (e == 0)
			e = fp_frame_write_fds(c->fd, w.p, w.len, &bfd,
					       FP_BUNDLE_FDS);
		fp_w_free(&w);
		close(bfd);
	} else if (c->typed.type == FP_REQ_RUN && c->nfwd > 0) {
		// The descriptors go with the request itself: the daemon
		// applies them just before the launch and only for this run.
		fp_wbuf_t w;
		fp_w_init(&w);
		e = fp_req_encode(&w, c->req_id, &c->typed);
		if (e == 0 &&
		    fp_frame_write_fds(c->fd, w.p, w.len, c->fwd,
				       c->nfwd) != FP_P_OK)
			e = EIO;
		fp_w_free(&w);
	} else {
		e = send_req(c->fd, c->req_id, &c->typed);
	}
	if (e != 0) return daemon_unreachable(e);
	int rc = read_ack(c->fd, &op_id);
	if (rc != 0) return rc < 0 ? daemon_unreachable(EIO) : rc;
	return 0;
}

static int typed_render(typed_ctx_t* c) {
	int exit_code = 1;
	bool finished = false;
	bool said_error = false;
	fp_bar_t bar;
	bool have_bar = false;
	int e;

	while (!finished) {
		fp_frame_hdr_t hdr;
		uint8_t* payload = NULL;
		fp_event_t ev;

		if (fp_frame_read(c->fd, &hdr, &payload) != FP_P_OK) {
			free(payload);
			break;
		}
		e = fp_event_decode(&hdr, payload, &ev);
		free(payload);
		if (e != 0) break;
		switch (ev.type) {
		case FP_EV_OP_LOG:
			if (ev.u.log.level >= FP_LEVEL_ERROR) said_error = true;
			render_log(&ev);
			break;
		case FP_EV_OP_TABLE: {
			// The daemon sends the cells; this side draws them as it draws its own table, so
			// a plan reads the same whichever side worked it out.
			const size_t nc = ev.u.table.ncols,
				     nr = ev.u.table.nrows;
			const char* const** rows =
				calloc(nr + 1, sizeof(*rows));
			if (rows != NULL) {
				for (size_t r = 0; r < nr; r++)
					rows[r] = (const char* const*)&ev.u
							  .table.cells[r * nc];
				fp_print_table(
					(const char* const*)ev.u.table.headers,
					nc, (const char* const* const*)rows, nr,
					FP_TABLE_OUTPUT);
				free(rows);
			}
			break;
		}
		case FP_EV_OP_PROGRESS:
			// A bar is for a person at a terminal; into a pipe or file it is one line per
			// update, and the log records already say what finished.
			if (ev.u.progress.total == 0 || !fp_is_tty()) break;
			if (!have_bar ||
			    strcmp(bar.label, ev.u.progress.stage) != 0) {
				if (have_bar) fp_bar_dismiss(&bar);
				bar = fp_bar_init("daemon", "",
						  ev.u.progress.stage,
						  ev.u.progress.total);
				have_bar = true;
			}
			fp_bar_update(&bar, ev.u.progress.current);
			break;
		case FP_EV_OP_FINISHED:
			if (have_bar) fp_bar_dismiss(&bar);
			have_bar = false;
			exit_code = (int)ev.u.finished.status;
			if (exit_code != 0 && !said_error &&
			    ev.u.finished.error && *ev.u.finished.error)
				fpkg_errorf("%s", ev.u.finished.error);
			finished = true;
			break;
		default:
			break;
		}
		fp_event_free(&ev);
	}
	if (c->typed.type == FP_REQ_RUN || c->typed.type == FP_REQ_ENTER)
		client_leave_raw_stdin();
	if (!finished) {
		fpkg_errorf("flatpkgd closed the connection before the "
			    "operation finished");
	}
	return exit_code;
}

static int typed_complete(typed_ctx_t* c, int exit_code) {
	// The world is the CLI's file, not the daemon's: after the rows have
	// been applied, the client writes its own state.
	if (exit_code == 0 && c->plan_world != NULL &&
	    state_write(c->fd, c->req_id + 1, "world", c->plan_world) != 0) {
		fpkg_errorf("the store changed, but the client state was not "
			    "written");
		exit_code = 1;
	}
	if (c->have_plan && c->plan_remove && c->plan.absent &&
	    !c->plan.had_work && exit_code == 0)
		exit_code = 1;
	return exit_code;
}

int client_try_typed(int argc, char* argv[]) {
	typed_ctx_t c;
	bool appstream, reported;
	int rc = -1;

	memset(&c, 0, sizeof(c));
	c.fd = -1;

	if (cli_parse(argc, argv, &c.req) != CLI_PROCEED) goto out;
	if (cli_req_is_local(&c.req)) goto out;
	// A bundle simulation changes nothing, so it needs no daemon.
	if (cli_req_cmd(&c.req) == CMD_INSTALL && c.req.from != NULL &&
	    c.req.g.simulate) {
		fpkg_infof("(simulate: would install %s)", c.req.from);
		rc = 0;
		goto out;
	}
	c.plan_install =
		cli_req_cmd(&c.req) == CMD_INSTALL && c.req.from == NULL;
	c.plan_remove = cli_req_cmd(&c.req) == CMD_UNINSTALL;
	appstream = cli_req_cmd(&c.req) == CMD_UPDATE && c.req.appstream;
	c.plan_upgrade = !appstream &&
			 (cli_req_cmd(&c.req) == CMD_UPDATE ||
			  cli_req_cmd(&c.req) == CMD_UPGRADE);
	if (appstream) {
		memset(&c.typed, 0, sizeof(c.typed));
		c.typed.type = FP_REQ_APPSTREAM;
		c.typed.u.appstream.remote =
			(char*)(c.req.remote ? c.req.remote : "");
		c.typed.u.appstream.arch =
			(char*)(c.req.run.arch ? c.req.run.arch : "");
	} else if (c.plan_install || c.plan_remove || c.plan_upgrade) {
		if ((c.plan_install || c.plan_remove) &&
		    (c.req.nops == 0 || c.req.nops > FP_APPLY_MAX)) {
			fpkg_errorf("too many refs for one operation");
			rc = 1;
			goto out;
		}
		memset(&c.typed, 0, sizeof(c.typed));
	} else if (!typed_request(&c.req, &c.typed, &c.owned, &reported)) {
		if (!reported)
			fpkg_errorf("'%s' cannot be sent to flatpkgd",
				    c.req.name);
		rc = 1;
		goto out;
	}

	// A run may carry descriptors the caller wants inherited. The list is decided
	// here, where the caller's table is; the descriptors ride on the RUN frame.
	if (c.typed.type == FP_REQ_RUN) {
		if (forward_targets(c.fwd, &c.nfwd, c.fwlist,
				    sizeof(c.fwlist)) != 0) {
			rc = 1;
			goto out;
		}
		c.typed.u.run.forward = c.fwlist;
	}

	rc = typed_connect(&c);
	if (rc != 0) goto out;
	rc = typed_handshake(&c);
	if (rc != 0) goto out;
	if (c.typed.type == FP_REQ_RUN || c.typed.type == FP_REQ_ENTER) {
		rc = typed_send_stdio(&c);
		if (rc != 0) goto out;
	}
	if (c.plan_install) {
		rc = typed_resolve_refs(&c);
		if (rc != 0) goto out;
	}
	c.req_id = c.plan_install ? 4 : 3;
	if (!typed_make_plan(&c, &rc)) goto out;
	rc = typed_send_request(&c);
	if (rc != 0) goto out;
	rc = typed_complete(&c, typed_render(&c));

out:
	typed_ctx_free(&c);
	return rc;
}
