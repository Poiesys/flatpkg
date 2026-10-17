#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "protocol.h"

void fp_w_init(fp_wbuf_t* w) { memset(w, 0, sizeof(*w)); }

void fp_w_free(fp_wbuf_t* w) {
	free(w->p);
	memset(w, 0, sizeof(*w));
}

static bool w_reserve(fp_wbuf_t* w, size_t n) {
	if (w->failed) return false;
	if (n > SIZE_MAX - w->len) {
		w->failed = true;
		return false;
	}
	if (w->len + n <= w->cap) return true;
	size_t cap = w->cap ? w->cap : 256;
	while (cap < w->len + n) {
		if (cap > SIZE_MAX / 2) {
			w->failed = true;
			return false;
		}
		cap *= 2;
	}
	uint8_t* p = realloc(w->p, cap);
	if (p == NULL) {
		w->failed = true;
		return false;
	}
	w->p = p;
	w->cap = cap;
	return true;
}

static void w_put(fp_wbuf_t* w, const void* src, size_t n) {
	if (n == 0 || !w_reserve(w, n)) return;
	memcpy(w->p + w->len, src, n);
	w->len += n;
}

void fp_w_u32(fp_wbuf_t* w, uint32_t v) {
	uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16),
			(uint8_t)(v >> 8), (uint8_t)v};
	w_put(w, b, sizeof(b));
}

void fp_w_u64(fp_wbuf_t* w, uint64_t v) {
	fp_w_u32(w, (uint32_t)(v >> 32));
	fp_w_u32(w, (uint32_t)v);
}

void fp_w_str(fp_wbuf_t* w, const char* s) {
	size_t n = s ? strlen(s) : 0;
	if (n > FP_PROTO_MAX_STR) {
		w->failed = true;
		return;
	}
	fp_w_u32(w, (uint32_t)n);
	w_put(w, s, n);
}

void fp_w_strlist(fp_wbuf_t* w, char* const* v, size_t n) {
	if (n > FP_PROTO_MAX_LIST || (n > 0 && v == NULL)) {
		w->failed = true;
		return;
	}
	fp_w_u32(w, (uint32_t)n);
	for (size_t i = 0; i < n; i++) fp_w_str(w, v[i]);
}

void fp_r_init(fp_rbuf_t* r, const uint8_t* p, size_t len) {
	r->p = p;
	r->len = len;
	r->pos = 0;
	r->err = FP_P_OK;
}

static bool r_need(fp_rbuf_t* r, size_t n) {
	if (r->err != FP_P_OK) return false;
	if (n > r->len - r->pos) {
		r->err = FP_P_ERR_TRUNCATED;
		return false;
	}
	return true;
}

uint32_t fp_r_u32(fp_rbuf_t* r) {
	if (!r_need(r, 4)) return 0;
	const uint8_t* b = r->p + r->pos;
	r->pos += 4;
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
	       ((uint32_t)b[2] << 8) | (uint32_t)b[3];
}

uint64_t fp_r_u64(fp_rbuf_t* r) {
	uint64_t hi = fp_r_u32(r);
	uint64_t lo = fp_r_u32(r);
	return (hi << 32) | lo;
}

char* fp_r_str(fp_rbuf_t* r) {
	uint32_t n = fp_r_u32(r);
	if (r->err != FP_P_OK) return NULL;
	if (n > FP_PROTO_MAX_STR) {
		r->err = FP_P_ERR_TOO_BIG;
		return NULL;
	}
	if (!r_need(r, n)) return NULL;
	if (n > 0 && memchr(r->p + r->pos, '\0', n) != NULL) {
		// A NUL inside a string would end it early in every C consumer
		// and let the bytes after it ride past a check on the front.
		r->err = FP_P_ERR_MALFORMED;
		return NULL;
	}
	char* s = malloc((size_t)n + 1);
	if (s == NULL) {
		r->err = FP_P_ERR_NOMEM;
		return NULL;
	}
	if (n > 0) memcpy(s, r->p + r->pos, n);
	s[n] = '\0';
	r->pos += n;
	return s;
}

void fp_strlist_free(char** v, size_t n) {
	if (v == NULL) return;
	for (size_t i = 0; i < n; i++) free(v[i]);
	free(v);
}

char** fp_r_strlist(fp_rbuf_t* r, size_t* n_out) {
	*n_out = 0;
	uint32_t n = fp_r_u32(r);
	if (r->err != FP_P_OK) return NULL;
	if (n > FP_PROTO_MAX_LIST) {
		r->err = FP_P_ERR_TOO_BIG;
		return NULL;
	}
	// Every element costs at least its 4-byte length, so a count the rest
	// of the buffer cannot hold is refused before anything is allocated.
	if ((size_t)n > (r->len - r->pos) / 4) {
		r->err = FP_P_ERR_TRUNCATED;
		return NULL;
	}
	char** v = calloc((size_t)n + 1, sizeof(char*));
	if (v == NULL) {
		r->err = FP_P_ERR_NOMEM;
		return NULL;
	}
	for (size_t i = 0; i < n; i++) {
		v[i] = fp_r_str(r);
		if (v[i] == NULL) {
			fp_strlist_free(v, i);
			return NULL;
		}
	}
	*n_out = n;
	return v;
}

bool fp_topic_valid(const char* s) {
	if (s == NULL) return false;
	size_t n = strlen(s);
	if (n == 0 || n > FP_PROTO_MAX_TOPIC) return false;
	if (s[0] == '/' || s[n - 1] == '/') return false;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			  c == '/' || c == '.' || c == '_' || c == '-';
		if (!ok) return false;
		if (c == '/' && s[i + 1] == '/') return false;
		if (c == '.' && s[i + 1] == '.') return false;
	}
	return true;
}

const char* fp_proto_strerror(int err) {
	switch (err) {
	case FP_P_OK:
		return "ok";
	case FP_P_ERR_MAGIC:
		return "not a flatpkg frame";
	case FP_P_ERR_VERSION:
		return "unsupported protocol version";
	case FP_P_ERR_KIND:
		return "unexpected frame kind";
	case FP_P_ERR_TOO_BIG:
		return "field or payload too large";
	case FP_P_ERR_TRUNCATED:
		return "frame ends inside a field";
	case FP_P_ERR_MALFORMED:
		return "malformed frame";
	case FP_P_ERR_NOMEM:
		return "out of memory";
	case FP_P_ERR_IO:
		return "i/o error";
	case FP_P_EOF:
		return "connection closed";
	}
	return "unknown error";
}

void fp_frame_hdr_encode(uint8_t out[FP_PROTO_HDR_LEN],
			 const fp_frame_hdr_t* h) {
	out[0] = (uint8_t)(FP_PROTO_MAGIC >> 8);
	out[1] = (uint8_t)FP_PROTO_MAGIC;
	out[2] = (uint8_t)FP_PROTO_VERSION;
	out[3] = h->kind;
	for (int i = 0; i < 4; i++) {
		out[4 + i] = (uint8_t)(h->type >> (24 - 8 * i));
		out[8 + i] = (uint8_t)(h->id >> (24 - 8 * i));
		out[12 + i] = (uint8_t)(h->len >> (24 - 8 * i));
	}
}

int fp_frame_hdr_decode(const uint8_t in[FP_PROTO_HDR_LEN], fp_frame_hdr_t* h) {
	memset(h, 0, sizeof(*h));
	if ((((unsigned)in[0] << 8) | in[1]) != FP_PROTO_MAGIC)
		return FP_P_ERR_MAGIC;
	if (in[2] != FP_PROTO_VERSION) return FP_P_ERR_VERSION;
	if (in[3] < FP_FRAME_REQUEST || in[3] > FP_FRAME_EVENT)
		return FP_P_ERR_KIND;
	fp_rbuf_t r;
	fp_r_init(&r, in + 4, FP_PROTO_HDR_LEN - 4);
	h->kind = in[3];
	h->type = fp_r_u32(&r);
	h->id = fp_r_u32(&r);
	h->len = fp_r_u32(&r);
	if (h->len > FP_PROTO_MAX_PAYLOAD) {
		memset(h, 0, sizeof(*h));
		return FP_P_ERR_TOO_BIG;
	}
	return FP_P_OK;
}

// A frame is built in place: a placeholder header, the payload, then the
// length patched in.  On any failure the buffer goes back to what it was.
typedef struct {
	size_t start;
	bool was_failed;
} frame_t;

static int frame_begin(fp_wbuf_t* w, frame_t* f, uint8_t kind, uint32_t type,
		       uint32_t id) {
	f->start = w->len;
	f->was_failed = w->failed;
	if (w->failed) return FP_P_ERR_NOMEM;
	fp_frame_hdr_t h = {.kind = kind, .type = type, .id = id, .len = 0};
	uint8_t hdr[FP_PROTO_HDR_LEN];
	fp_frame_hdr_encode(hdr, &h);
	w_put(w, hdr, sizeof(hdr));
	return w->failed ? FP_P_ERR_NOMEM : FP_P_OK;
}

static int frame_abort(fp_wbuf_t* w, const frame_t* f, int err) {
	w->len = f->start;
	w->failed = f->was_failed;
	return err;
}

static int frame_end(fp_wbuf_t* w, const frame_t* f) {
	if (w->failed) return frame_abort(w, f, FP_P_ERR_NOMEM);
	size_t payload = w->len - f->start - FP_PROTO_HDR_LEN;
	if (payload > FP_PROTO_MAX_PAYLOAD)
		return frame_abort(w, f, FP_P_ERR_TOO_BIG);
	uint8_t* len = w->p + f->start + 12;
	for (int i = 0; i < 4; i++)
		len[i] = (uint8_t)((uint32_t)payload >> (24 - 8 * i));
	return FP_P_OK;
}

int fp_req_encode(fp_wbuf_t* w, uint32_t id, const fp_req_t* r) {
	frame_t f;
	int e = frame_begin(w, &f, FP_FRAME_REQUEST, r->type, id);
	if (e != FP_P_OK) return e;
	switch (r->type) {
	case FP_REQ_HELLO:
		fp_w_u32(w, r->u.hello.version);
		fp_w_str(w, r->u.hello.client);
		break;
	case FP_REQ_SUBSCRIBE:
	case FP_REQ_UNSUBSCRIBE:
		if (!fp_topic_valid(r->u.sub.topic))
			return frame_abort(w, &f, FP_P_ERR_MALFORMED);
		fp_w_str(w, r->u.sub.topic);
		break;
	case FP_REQ_CANCEL:
		fp_w_u64(w, r->u.cancel.op_id);
		break;
	case FP_REQ_ROLLBACK:
		fp_w_str(w, r->u.rollback.ref);
		fp_w_str(w, r->u.rollback.commit);
		break;
	case FP_REQ_RUN:
		fp_w_str(w, r->u.run.app_id);
		fp_w_strlist(w, r->u.run.args, r->u.run.nargs);
		fp_w_str(w, r->u.run.command);
		fp_w_str(w, r->u.run.branch);
		fp_w_str(w, r->u.run.arch);
		fp_w_u32(w, r->u.run.flags);
		fp_w_strlist(w, r->u.run.sockets, r->u.run.nsockets);
		fp_w_strlist(w, r->u.run.devices, r->u.run.ndevices);
		fp_w_strlist(w, r->u.run.shares, r->u.run.nshares);
		fp_w_strlist(w, r->u.run.fs_add, r->u.run.nfs_add);
		fp_w_strlist(w, r->u.run.fs_remove, r->u.run.nfs_remove);
		fp_w_strlist(w, r->u.run.envs, r->u.run.nenvs);
		fp_w_str(w, r->u.run.forward);
		break;
	case FP_REQ_ENV:
		fp_w_strlist(w, r->u.env.vars, r->u.env.nvars);
		break;
	case FP_REQ_STDIO:
		fp_w_u32(w, r->u.stdio.count);
		break;
	case FP_REQ_INSTALL_BUNDLE:
		fp_w_u32(w, r->u.bundle.flags);
		break;
	case FP_REQ_EXPORTS:
		break;
	case FP_REQ_APPLY:
		if (r->u.apply.nrows > FP_APPLY_MAX)
			return frame_abort(w, &f, FP_P_ERR_MALFORMED);
		fp_w_u32(w, r->u.apply.flags);
		fp_w_str(w, r->u.apply.remote);
		fp_w_u32(w, (uint32_t)r->u.apply.nrows);
		for (size_t i = 0; i < r->u.apply.nrows; i++) {
			const fp_change_row_t* row = &r->u.apply.rows[i];
			if (row->type > FP_APPLY_NOOP || row->ref == NULL)
				return frame_abort(w, &f, FP_P_ERR_MALFORMED);
			fp_w_u32(w, row->type);
			fp_w_str(w, row->ref);
			fp_w_str(w, row->old_version);
			fp_w_str(w, row->new_version);
		}
		break;
	case FP_REQ_CLIENT_DECLARE:
		if (r->u.client_declare.nnames == 0 ||
		    r->u.client_declare.nnames > 16)
			return frame_abort(w, &f, FP_P_ERR_MALFORMED);
		fp_w_strlist(w, r->u.client_declare.names,
			     r->u.client_declare.nnames);
		break;
	case FP_REQ_CLIENT_STATE:
		if (r->u.client_state.name == NULL)
			return frame_abort(w, &f, FP_P_ERR_MALFORMED);
		fp_w_u32(w, r->u.client_state.flags);
		fp_w_str(w, r->u.client_state.name);
		fp_w_str(w, r->u.client_state.data);
		break;
	case FP_REQ_PREFETCH:
		if (r->u.prefetch.nrefs == 0 ||
		    r->u.prefetch.nrefs > FP_APPLY_MAX)
			return frame_abort(w, &f, FP_P_ERR_MALFORMED);
		fp_w_str(w, r->u.prefetch.remote);
		fp_w_strlist(w, r->u.prefetch.refs, r->u.prefetch.nrefs);
		break;
	case FP_REQ_ENTER:
		fp_w_str(w, r->u.enter.instance);
		fp_w_strlist(w, r->u.enter.command, r->u.enter.ncommand);
		break;
	case FP_REQ_KILL:
		fp_w_str(w, r->u.kill.instance);
		break;
	case FP_REQ_IMPORT:
		fp_w_str(w, r->u.import.path);
		break;
	case FP_REQ_REMOTE_ADD:
		fp_w_str(w, r->u.remote_add.name);
		fp_w_str(w, r->u.remote_add.url);
		fp_w_str(w, r->u.remote_add.from);
		break;
	case FP_REQ_REMOTE_DELETE:
		fp_w_str(w, r->u.remote_delete.name);
		break;
	case FP_REQ_APPSTREAM:
		fp_w_str(w, r->u.appstream.remote);
		fp_w_str(w, r->u.appstream.arch);
		break;
	case FP_REQ_OVERRIDE:
		fp_w_str(w, r->u.override.app_id);
		fp_w_u32(w, r->u.override.flags);
		fp_w_str(w, r->u.override.changes);
		break;
	default:
		return frame_abort(w, &f, FP_P_ERR_MALFORMED);
	}
	return frame_end(w, &f);
}

int fp_ack_encode(fp_wbuf_t* w, uint32_t id, const fp_ack_t* a) {
	frame_t f;
	int e = frame_begin(w, &f, FP_FRAME_ACK, a->type, id);
	if (e != FP_P_OK) return e;
	switch (a->type) {
	case FP_ACK_OK:
		fp_w_u64(w, a->op_id);
		break;
	case FP_ACK_ERR:
		fp_w_u32(w, a->code);
		fp_w_str(w, a->message);
		break;
	default:
		return frame_abort(w, &f, FP_P_ERR_MALFORMED);
	}
	return frame_end(w, &f);
}

int fp_event_encode(fp_wbuf_t* w, uint32_t seq, const fp_event_t* ev) {
	frame_t f;
	int e = frame_begin(w, &f, FP_FRAME_EVENT, ev->type, seq);
	if (e != FP_P_OK) return e;
	if (!fp_topic_valid(ev->topic))
		return frame_abort(w, &f, FP_P_ERR_MALFORMED);
	fp_w_str(w, ev->topic);
	fp_w_u64(w, ev->op_id);
	switch (ev->type) {
	case FP_EV_OP_STARTED:
		fp_w_u32(w, ev->u.started.kind);
		fp_w_str(w, ev->u.started.label);
		break;
	case FP_EV_OP_PROGRESS:
		fp_w_str(w, ev->u.progress.stage);
		fp_w_u64(w, ev->u.progress.current);
		fp_w_u64(w, ev->u.progress.total);
		break;
	case FP_EV_OP_STATE:
		fp_w_str(w, ev->u.state.state);
		break;
	case FP_EV_OP_LOG:
		fp_w_u32(w, ev->u.log.level);
		fp_w_str(w, ev->u.log.scope);
		fp_w_str(w, ev->u.log.text);
		break;
	case FP_EV_OP_FINISHED:
		fp_w_u32(w, ev->u.finished.status);
		fp_w_str(w, ev->u.finished.error);
		break;
	case FP_EV_OP_TABLE: {
		const uint32_t nc = ev->u.table.ncols, nr = ev->u.table.nrows;
		if (nc == 0 || nc > FP_PROTO_MAX_COLS ||
		    nr > FP_PROTO_MAX_LIST ||
		    (uint64_t)nc * nr > FP_PROTO_MAX_LIST ||
		    (nr > 0 && ev->u.table.cells == NULL))
			return frame_abort(w, &f, FP_P_ERR_MALFORMED);
		fp_w_u32(w, nc);
		fp_w_u32(w, nr);
		fp_w_u32(w, ev->u.table.headers != NULL ? 1u : 0u);
		if (ev->u.table.headers != NULL)
			fp_w_strlist(w, ev->u.table.headers, nc);
		fp_w_strlist(w, ev->u.table.cells, (size_t)nc * nr);
		break;
	}
	case FP_EV_META:
		if (ev->u.meta.ref == NULL || ev->u.meta.metadata == NULL)
			return frame_abort(w, &f, FP_P_ERR_MALFORMED);
		fp_w_str(w, ev->u.meta.ref);
		fp_w_str(w, ev->u.meta.commit);
		fp_w_str(w, ev->u.meta.metadata);
		break;
	case FP_EV_STORE_CHANGED:
		if (ev->u.store.action == NULL || ev->u.store.ref == NULL)
			return frame_abort(w, &f, FP_P_ERR_MALFORMED);
		fp_w_str(w, ev->u.store.action);
		fp_w_str(w, ev->u.store.ref);
		break;
	case FP_EV_REMOTE_CHANGED:
		if (ev->u.remote.name == NULL)
			return frame_abort(w, &f, FP_P_ERR_MALFORMED);
		fp_w_str(w, ev->u.remote.name);
		break;
	case FP_EV_INSTANCE_STARTED:
	case FP_EV_INSTANCE_EXITED:
		if (ev->u.instance.app_id == NULL)
			return frame_abort(w, &f, FP_P_ERR_MALFORMED);
		fp_w_str(w, ev->u.instance.app_id);
		break;
	case FP_EV_CLIENT_STATE:
		if (ev->u.client_state.name == NULL ||
		    ev->u.client_state.data == NULL)
			return frame_abort(w, &f, FP_P_ERR_MALFORMED);
		fp_w_str(w, ev->u.client_state.name);
		fp_w_str(w, ev->u.client_state.data);
		break;
	default:
		return frame_abort(w, &f, FP_P_ERR_MALFORMED);
	}
	return frame_end(w, &f);
}

void fp_req_free(fp_req_t* r) {
	switch (r->type) {
	case FP_REQ_HELLO:
		free(r->u.hello.client);
		break;
	case FP_REQ_SUBSCRIBE:
	case FP_REQ_UNSUBSCRIBE:
		free(r->u.sub.topic);
		break;
	case FP_REQ_ROLLBACK:
		free(r->u.rollback.ref);
		free(r->u.rollback.commit);
		break;
	case FP_REQ_RUN:
		free(r->u.run.app_id);
		fp_strlist_free(r->u.run.args, r->u.run.nargs);
		free(r->u.run.command);
		free(r->u.run.branch);
		free(r->u.run.arch);
		fp_strlist_free(r->u.run.sockets, r->u.run.nsockets);
		fp_strlist_free(r->u.run.devices, r->u.run.ndevices);
		fp_strlist_free(r->u.run.shares, r->u.run.nshares);
		fp_strlist_free(r->u.run.fs_add, r->u.run.nfs_add);
		fp_strlist_free(r->u.run.fs_remove, r->u.run.nfs_remove);
		fp_strlist_free(r->u.run.envs, r->u.run.nenvs);
		free(r->u.run.forward);
		break;
	case FP_REQ_ENV:
		fp_strlist_free(r->u.env.vars, r->u.env.nvars);
		break;
	case FP_REQ_ENTER:
		free(r->u.enter.instance);
		fp_strlist_free(r->u.enter.command, r->u.enter.ncommand);
		break;
	case FP_REQ_KILL:
		free(r->u.kill.instance);
		break;
	case FP_REQ_IMPORT:
		free(r->u.import.path);
		break;
	case FP_REQ_REMOTE_ADD:
		free(r->u.remote_add.name);
		free(r->u.remote_add.url);
		free(r->u.remote_add.from);
		break;
	case FP_REQ_REMOTE_DELETE:
		free(r->u.remote_delete.name);
		break;
	case FP_REQ_APPSTREAM:
		free(r->u.appstream.remote);
		free(r->u.appstream.arch);
		break;
	case FP_REQ_OVERRIDE:
		free(r->u.override.app_id);
		free(r->u.override.changes);
		break;
	case FP_REQ_PREFETCH:
		free(r->u.prefetch.remote);
		fp_strlist_free(r->u.prefetch.refs, r->u.prefetch.nrefs);
		break;
	case FP_REQ_APPLY:
		free(r->u.apply.remote);
		for (size_t i = 0; i < r->u.apply.nrows; i++) {
			free(r->u.apply.rows[i].ref);
			free(r->u.apply.rows[i].old_version);
			free(r->u.apply.rows[i].new_version);
		}
		free(r->u.apply.rows);
		break;
	case FP_REQ_CLIENT_STATE:
		free(r->u.client_state.name);
		free(r->u.client_state.data);
		break;
	case FP_REQ_CLIENT_DECLARE:
		fp_strlist_free(r->u.client_declare.names,
				r->u.client_declare.nnames);
		break;
	default:
		break;
	}
	memset(r, 0, sizeof(*r));
}

void fp_ack_free(fp_ack_t* a) {
	free(a->message);
	memset(a, 0, sizeof(*a));
}

void fp_event_free(fp_event_t* e) {
	free(e->topic);
	switch (e->type) {
	case FP_EV_OP_STARTED:
		free(e->u.started.label);
		break;
	case FP_EV_OP_PROGRESS:
		free(e->u.progress.stage);
		break;
	case FP_EV_OP_STATE:
		free(e->u.state.state);
		break;
	case FP_EV_OP_LOG:
		free(e->u.log.scope);
		free(e->u.log.text);
		break;
	case FP_EV_OP_FINISHED:
		free(e->u.finished.error);
		break;
	case FP_EV_META:
		free(e->u.meta.ref);
		free(e->u.meta.commit);
		free(e->u.meta.metadata);
		break;
	case FP_EV_OP_TABLE:
		// Only lists that decoded in full, with the counts the header
		// promised, are ever stored, so these counts are the lists'.
		fp_strlist_free(e->u.table.headers, e->u.table.ncols);
		fp_strlist_free(e->u.table.cells,
				(size_t)e->u.table.ncols * e->u.table.nrows);
		break;
	case FP_EV_STORE_CHANGED:
		free(e->u.store.action);
		free(e->u.store.ref);
		break;
	case FP_EV_REMOTE_CHANGED:
		free(e->u.remote.name);
		break;
	case FP_EV_INSTANCE_STARTED:
	case FP_EV_INSTANCE_EXITED:
		free(e->u.instance.app_id);
		break;
	case FP_EV_CLIENT_STATE:
		free(e->u.client_state.name);
		free(e->u.client_state.data);
		break;
	default:
		break;
	}
	memset(e, 0, sizeof(*e));
}

static int hdr_is(const fp_frame_hdr_t* h, uint8_t kind) {
	if (h->kind != kind) return FP_P_ERR_KIND;
	if (h->len > FP_PROTO_MAX_PAYLOAD) return FP_P_ERR_TOO_BIG;
	return FP_P_OK;
}

static int finish(const fp_rbuf_t* r) {
	if (r->err != FP_P_OK) return r->err;
	if (r->pos != r->len) return FP_P_ERR_MALFORMED;
	return FP_P_OK;
}

int fp_req_decode(const fp_frame_hdr_t* h, const uint8_t* payload,
		  fp_req_t* out) {
	memset(out, 0, sizeof(*out));
	int e = hdr_is(h, FP_FRAME_REQUEST);
	if (e != FP_P_OK) return e;
	fp_rbuf_t r;
	fp_r_init(&r, payload, h->len);
	out->type = h->type;
	switch (h->type) {
	case FP_REQ_HELLO:
		out->u.hello.version = fp_r_u32(&r);
		out->u.hello.client = fp_r_str(&r);
		break;
	case FP_REQ_SUBSCRIBE:
	case FP_REQ_UNSUBSCRIBE:
		out->u.sub.topic = fp_r_str(&r);
		if (r.err == FP_P_OK && !fp_topic_valid(out->u.sub.topic))
			r.err = FP_P_ERR_MALFORMED;
		break;
	case FP_REQ_CANCEL:
		out->u.cancel.op_id = fp_r_u64(&r);
		break;
	case FP_REQ_ROLLBACK:
		out->u.rollback.ref = fp_r_str(&r);
		out->u.rollback.commit = fp_r_str(&r);
		break;
	case FP_REQ_RUN:
		out->u.run.app_id = fp_r_str(&r);
		out->u.run.args = fp_r_strlist(&r, &out->u.run.nargs);
		out->u.run.command = fp_r_str(&r);
		out->u.run.branch = fp_r_str(&r);
		out->u.run.arch = fp_r_str(&r);
		out->u.run.flags = fp_r_u32(&r);
		out->u.run.sockets = fp_r_strlist(&r, &out->u.run.nsockets);
		out->u.run.devices = fp_r_strlist(&r, &out->u.run.ndevices);
		out->u.run.shares = fp_r_strlist(&r, &out->u.run.nshares);
		out->u.run.fs_add = fp_r_strlist(&r, &out->u.run.nfs_add);
		out->u.run.fs_remove = fp_r_strlist(&r, &out->u.run.nfs_remove);
		out->u.run.envs = fp_r_strlist(&r, &out->u.run.nenvs);
		out->u.run.forward = fp_r_str(&r);
		break;
	case FP_REQ_ENV:
		out->u.env.vars = fp_r_strlist(&r, &out->u.env.nvars);
		break;
	case FP_REQ_INSTALL_BUNDLE:
		out->u.bundle.flags = fp_r_u32(&r);
		if (out->u.bundle.flags != 0) r.err = FP_P_ERR_MALFORMED;
		break;
	case FP_REQ_EXPORTS:
		break;
	case FP_REQ_APPLY: {
		out->u.apply.flags = fp_r_u32(&r);
		out->u.apply.remote = fp_r_str(&r);
		uint32_t nrows = fp_r_u32(&r);
		if (nrows > FP_APPLY_MAX) {
			r.err = FP_P_ERR_MALFORMED;
			break;
		}
		out->u.apply.rows = calloc(nrows ? nrows : 1,
					   sizeof(fp_change_row_t));
		if (out->u.apply.rows == NULL) {
			r.err = FP_P_ERR_NOMEM;
			break;
		}
		for (uint32_t i = 0; i < nrows; i++) {
			fp_change_row_t* row = &out->u.apply.rows[i];
			row->type = fp_r_u32(&r);
			row->ref = fp_r_str(&r);
			row->old_version = fp_r_str(&r);
			row->new_version = fp_r_str(&r);
			if (row->type > FP_APPLY_NOOP || row->ref == NULL ||
			    *row->ref == '\0')
				r.err = FP_P_ERR_MALFORMED;
			out->u.apply.nrows = i + 1;
			if (r.err != FP_P_OK) break;
		}
		break;
	}
	case FP_REQ_CLIENT_DECLARE: {
		out->u.client_declare.names =
			fp_r_strlist(&r, &out->u.client_declare.nnames);
		if (out->u.client_declare.names == NULL ||
		    out->u.client_declare.nnames == 0 ||
		    out->u.client_declare.nnames > 16)
			r.err = FP_P_ERR_MALFORMED;
		break;
	}
	case FP_REQ_CLIENT_STATE:
		out->u.client_state.flags = fp_r_u32(&r);
		out->u.client_state.name = fp_r_str(&r);
		out->u.client_state.data = fp_r_str(&r);
		if (out->u.client_state.name == NULL ||
		    out->u.client_state.data == NULL)
			r.err = FP_P_ERR_MALFORMED;
		break;
	case FP_REQ_PREFETCH:
		out->u.prefetch.remote = fp_r_str(&r);
		out->u.prefetch.refs =
			fp_r_strlist(&r, &out->u.prefetch.nrefs);
		if (r.err == FP_P_OK &&
		    (out->u.prefetch.nrefs == 0 ||
		     out->u.prefetch.nrefs > FP_APPLY_MAX))
			r.err = FP_P_ERR_MALFORMED;
		break;
	case FP_REQ_STDIO:
		out->u.stdio.count = fp_r_u32(&r);
		if (out->u.stdio.count != FP_STDIO_FDS)
			r.err = FP_P_ERR_MALFORMED;
		break;
	case FP_REQ_ENTER:
		out->u.enter.instance = fp_r_str(&r);
		out->u.enter.command = fp_r_strlist(&r, &out->u.enter.ncommand);
		break;
	case FP_REQ_KILL:
		out->u.kill.instance = fp_r_str(&r);
		break;
	case FP_REQ_IMPORT:
		out->u.import.path = fp_r_str(&r);
		break;
	case FP_REQ_REMOTE_ADD:
		out->u.remote_add.name = fp_r_str(&r);
		out->u.remote_add.url = fp_r_str(&r);
		out->u.remote_add.from = fp_r_str(&r);
		break;
	case FP_REQ_REMOTE_DELETE:
		out->u.remote_delete.name = fp_r_str(&r);
		break;
	case FP_REQ_APPSTREAM:
		out->u.appstream.remote = fp_r_str(&r);
		out->u.appstream.arch = fp_r_str(&r);
		break;
	case FP_REQ_OVERRIDE:
		out->u.override.app_id = fp_r_str(&r);
		out->u.override.flags = fp_r_u32(&r);
		out->u.override.changes = fp_r_str(&r);
		break;
	default:
		r.err = FP_P_ERR_MALFORMED;
		break;
	}
	e = finish(&r);
	if (e != FP_P_OK) {
		fp_req_free(out);
		return e;
	}
	return FP_P_OK;
}

int fp_ack_decode(const fp_frame_hdr_t* h, const uint8_t* payload,
		  fp_ack_t* out) {
	memset(out, 0, sizeof(*out));
	int e = hdr_is(h, FP_FRAME_ACK);
	if (e != FP_P_OK) return e;
	fp_rbuf_t r;
	fp_r_init(&r, payload, h->len);
	out->type = h->type;
	switch (h->type) {
	case FP_ACK_OK:
		out->op_id = fp_r_u64(&r);
		break;
	case FP_ACK_ERR:
		out->code = fp_r_u32(&r);
		out->message = fp_r_str(&r);
		break;
	default:
		r.err = FP_P_ERR_MALFORMED;
		break;
	}
	e = finish(&r);
	if (e != FP_P_OK) {
		fp_ack_free(out);
		return e;
	}
	return FP_P_OK;
}

int fp_event_decode(const fp_frame_hdr_t* h, const uint8_t* payload,
		    fp_event_t* out) {
	memset(out, 0, sizeof(*out));
	int e = hdr_is(h, FP_FRAME_EVENT);
	if (e != FP_P_OK) return e;
	// Kept in function scope so the cleanup below can free them: the
	// analyzer cannot follow ownership through the event union.
	char** table_headers = NULL;
	char** table_cells = NULL;
	size_t table_nc = 0, table_nr = 0;
	fp_rbuf_t r;
	fp_r_init(&r, payload, h->len);
	out->type = h->type;
	out->topic = fp_r_str(&r);
	if (r.err == FP_P_OK && !fp_topic_valid(out->topic))
		r.err = FP_P_ERR_MALFORMED;
	out->op_id = fp_r_u64(&r);
	switch (h->type) {
	case FP_EV_OP_STARTED:
		out->u.started.kind = fp_r_u32(&r);
		out->u.started.label = fp_r_str(&r);
		break;
	case FP_EV_OP_PROGRESS:
		out->u.progress.stage = fp_r_str(&r);
		out->u.progress.current = fp_r_u64(&r);
		out->u.progress.total = fp_r_u64(&r);
		break;
	case FP_EV_OP_STATE:
		out->u.state.state = fp_r_str(&r);
		break;
	case FP_EV_OP_LOG:
		out->u.log.level = fp_r_u32(&r);
		out->u.log.scope = fp_r_str(&r);
		out->u.log.text = fp_r_str(&r);
		break;
	case FP_EV_OP_FINISHED:
		out->u.finished.status = fp_r_u32(&r);
		out->u.finished.error = fp_r_str(&r);
		break;
	case FP_EV_OP_TABLE: {
		const uint32_t nc = fp_r_u32(&r);
		const uint32_t nr = fp_r_u32(&r);
		const uint32_t has_headers = fp_r_u32(&r);
		if (r.err != FP_P_OK) break;
		if (nc == 0 || nc > FP_PROTO_MAX_COLS ||
		    nr > FP_PROTO_MAX_LIST ||
		    (uint64_t)nc * nr > FP_PROTO_MAX_LIST || has_headers > 1) {
			r.err = FP_P_ERR_MALFORMED;
			break;
		}
		size_t n = 0;
		char** headers = NULL;
		if (has_headers) {
			headers = fp_r_strlist(&r, &n);
			if (r.err == FP_P_OK && n != nc) {
				// Not the column count: free by what it
				// holds, never by what the header promised.
				fp_strlist_free(headers, n);
				headers = NULL;
				r.err = FP_P_ERR_MALFORMED;
			}
			if (r.err != FP_P_OK) {
				fp_strlist_free(headers, n);
				headers = NULL;
				break;
			}
		}
		char** cells = fp_r_strlist(&r, &n);
		if (r.err == FP_P_OK && n != (size_t)nc * nr) {
			fp_strlist_free(cells, n);
			cells = NULL;
			r.err = FP_P_ERR_MALFORMED;
		}
		if (r.err != FP_P_OK) {
			fp_strlist_free(headers, nc);
			fp_strlist_free(cells, n);
			break;
		}
		out->u.table.ncols = nc;
		out->u.table.nrows = nr;
		out->u.table.headers = headers;
		out->u.table.cells = cells;
		table_headers = headers;
		table_cells = cells;
		table_nc = nc;
		table_nr = nr;
		// The event owns both lists now; the locals must not look
		// owned here, or a failed finish() reads as a leak.
		headers = NULL;
		cells = NULL;
		break;
	}
	case FP_EV_META:
		out->u.meta.ref = fp_r_str(&r);
		out->u.meta.commit = fp_r_str(&r);
		out->u.meta.metadata = fp_r_str(&r);
		if (out->u.meta.ref == NULL || out->u.meta.metadata == NULL)
			r.err = FP_P_ERR_MALFORMED;
		break;
	case FP_EV_STORE_CHANGED:
		out->u.store.action = fp_r_str(&r);
		out->u.store.ref = fp_r_str(&r);
		if (out->u.store.action == NULL || out->u.store.ref == NULL)
			r.err = FP_P_ERR_MALFORMED;
		break;
	case FP_EV_REMOTE_CHANGED:
		out->u.remote.name = fp_r_str(&r);
		if (out->u.remote.name == NULL)
			r.err = FP_P_ERR_MALFORMED;
		break;
	case FP_EV_INSTANCE_STARTED:
	case FP_EV_INSTANCE_EXITED:
		out->u.instance.app_id = fp_r_str(&r);
		if (out->u.instance.app_id == NULL)
			r.err = FP_P_ERR_MALFORMED;
		break;
	case FP_EV_CLIENT_STATE:
		out->u.client_state.name = fp_r_str(&r);
		out->u.client_state.data = fp_r_str(&r);
		if (out->u.client_state.name == NULL ||
		    out->u.client_state.data == NULL)
			r.err = FP_P_ERR_MALFORMED;
		break;
	default:
		r.err = FP_P_ERR_MALFORMED;
		break;
	}
	e = finish(&r);
	if (e != FP_P_OK) {
		fp_strlist_free(table_headers, table_nc);
		fp_strlist_free(table_cells, table_nc * table_nr);
		if (out->type == FP_EV_OP_TABLE) {
			out->u.table.headers = NULL;
			out->u.table.cells = NULL;
		}
		fp_event_free(out);
		return e;
	}
	return FP_P_OK;
}

// Read exactly n bytes.  Returns the number read, which is short only at
// end of stream; -1 on error.
static ssize_t read_full(int fd, uint8_t* buf, size_t n) {
	size_t got = 0;
	while (got < n) {
		ssize_t k = read(fd, buf + got, n - got);
		if (k == 0) break;
		if (k < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		got += (size_t)k;
	}
	return (ssize_t)got;
}

int fp_frame_read(int fd, fp_frame_hdr_t* h, uint8_t** payload) {
	uint8_t hdr[FP_PROTO_HDR_LEN];
	*payload = NULL;
	ssize_t got = read_full(fd, hdr, sizeof(hdr));
	if (got < 0) return FP_P_ERR_IO;
	if (got == 0) return FP_P_EOF;
	if ((size_t)got < sizeof(hdr)) return FP_P_ERR_TRUNCATED;
	int e = fp_frame_hdr_decode(hdr, h);
	if (e != FP_P_OK) return e;
	if (h->len == 0) return FP_P_OK;
	uint8_t* buf = malloc(h->len);
	if (buf == NULL) return FP_P_ERR_NOMEM;
	got = read_full(fd, buf, h->len);
	if (got < 0 || (size_t)got < h->len) {
		free(buf);
		return got < 0 ? FP_P_ERR_IO : FP_P_ERR_TRUNCATED;
	}
	*payload = buf;
	return FP_P_OK;
}

// MSG_NOSIGNAL: a client that vanished mid-write must not kill the daemon
// with SIGPIPE.  This is why the helper is for sockets only.
int fp_frame_write(int fd, const uint8_t* buf, size_t len) {
	size_t sent = 0;
	while (sent < len) {
		ssize_t k = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
		if (k < 0) {
			if (errno == EINTR) continue;
			return FP_P_ERR_IO;
		}
		sent += (size_t)k;
	}
	return FP_P_OK;
}

#include <sys/socket.h>
#include <sys/uio.h>

#define FP_MAX_CTL_FDS FP_MAX_FDS

int fp_frame_write_fds(int fd, const uint8_t* buf, size_t len, const int* fds,
		       size_t nfds) {
	if (nfds == 0) return fp_frame_write(fd, buf, len);
	if (nfds > FP_MAX_CTL_FDS || len == 0) return FP_P_ERR_MALFORMED;

	union {
		struct cmsghdr h;
		char b[CMSG_SPACE(sizeof(int) * FP_MAX_CTL_FDS)];
	} ctl;
	memset(&ctl, 0, sizeof(ctl));
	struct iovec iov = {(void*)buf, len};
	struct msghdr m;
	memset(&m, 0, sizeof(m));
	m.msg_iov = &iov;
	m.msg_iovlen = 1;
	m.msg_control = ctl.b;
	m.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
	struct cmsghdr* c = CMSG_FIRSTHDR(&m);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
	memcpy(CMSG_DATA(c), fds, sizeof(int) * nfds);

	ssize_t k;
	do {
		k = sendmsg(fd, &m, MSG_NOSIGNAL);
	} while (k < 0 && errno == EINTR);
	if (k < 0) return FP_P_ERR_IO;
	if ((size_t)k == len) return FP_P_OK;
	return fp_frame_write(fd, buf + k, len - (size_t)k);
}

// Header bytes with recvmsg, so descriptors riding on them are seen.
static ssize_t read_hdr_fds(int fd, uint8_t* buf, size_t n, int* fds,
			    size_t max, size_t* nfds) {
	size_t got = 0;
	while (got < n) {
		union {
			struct cmsghdr h;
			char b[CMSG_SPACE(sizeof(int) * FP_MAX_CTL_FDS)];
		} ctl;
		struct iovec iov = {buf + got, n - got};
		struct msghdr m;
		memset(&m, 0, sizeof(m));
		m.msg_iov = &iov;
		m.msg_iovlen = 1;
		m.msg_control = ctl.b;
		m.msg_controllen = sizeof(ctl.b);
		ssize_t k = recvmsg(fd, &m, 0);
		if (k == 0) break;
		if (k < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		for (struct cmsghdr* c = CMSG_FIRSTHDR(&m); c != NULL;
		     c = CMSG_NXTHDR(&m, c)) {
			if (c->cmsg_level != SOL_SOCKET ||
			    c->cmsg_type != SCM_RIGHTS)
				continue;
			size_t cnt = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			int tmp[FP_MAX_CTL_FDS];
			if (cnt > FP_MAX_CTL_FDS) cnt = FP_MAX_CTL_FDS;
			memcpy(tmp, CMSG_DATA(c), cnt * sizeof(int));
			for (size_t i = 0; i < cnt; i++) {
				if (*nfds < max) fds[(*nfds)++] = tmp[i];
				else
					close(tmp[i]);
			}
		}
		got += (size_t)k;
	}
	return (ssize_t)got;
}

int fp_frame_read_fds(int fd, fp_frame_hdr_t* h, uint8_t** payload, int* fds,
		      size_t max, size_t* nfds) {
	uint8_t hdr[FP_PROTO_HDR_LEN];
	*payload = NULL;
	*nfds = 0;
	ssize_t got = read_hdr_fds(fd, hdr, sizeof(hdr), fds, max, nfds);
	if (got < 0) goto io;
	if (got == 0) return FP_P_EOF;
	if ((size_t)got < sizeof(hdr)) goto trunc;
	int e = fp_frame_hdr_decode(hdr, h);
	if (e != FP_P_OK) goto bad;
	if (h->len == 0) return FP_P_OK;
	uint8_t* buf = malloc(h->len);
	if (buf == NULL) {
		e = FP_P_ERR_NOMEM;
		goto bad;
	}
	got = read_full(fd, buf, h->len);
	if (got < 0 || (size_t)got < h->len) {
		free(buf);
		e = got < 0 ? FP_P_ERR_IO : FP_P_ERR_TRUNCATED;
		goto bad;
	}
	*payload = buf;
	return FP_P_OK;
io:
	e = FP_P_ERR_IO;
	goto bad;
trunc:
	e = FP_P_ERR_TRUNCATED;
bad:
	for (size_t i = 0; i < *nfds; i++) close(fds[i]);
	*nfds = 0;
	return e;
}
