#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/nv.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "protocol.h"

// The 16-byte frame header stays a fixed envelope; the payload is an nvlist
// packed by libnv.  Every field is then a named, typed value, so a decoder
// checks a name and a type instead of a position, and the host's endianness
// and the length arithmetic are the library's.  Descriptors still travel in
// SCM_RIGHTS beside the frame.

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

static uint32_t be32(const uint8_t* b) {
	return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
	       ((uint32_t)b[2] << 8) | (uint32_t)b[3];
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
	h->kind = in[3];
	h->type = be32(in + 4);
	h->id = be32(in + 8);
	h->len = be32(in + 12);
	if (h->len > FP_PROTO_MAX_PAYLOAD) {
		memset(h, 0, sizeof(*h));
		return FP_P_ERR_TOO_BIG;
	}
	return FP_P_OK;
}

// --- encoding -------------------------------------------------------------
//
// An nvlist that libnv set an error on is failed as a whole; a name can be
// added twice only by a caller mistake here, never by a peer's bytes (the
// decoder rejects the duplicates libnv's unpack would otherwise adopt).

static bool nvl_ok(const nvlist_t* nvl) {
	return nvl != NULL && nvlist_error(nvl) == 0;
}

static int add_str(nvlist_t* nvl, const char* name, const char* s) {
	if (s == NULL) s = "";
	if (strlen(s) > FP_PROTO_MAX_STR) return FP_P_ERR_TOO_BIG;
	nvlist_add_string(nvl, name, s);
	return nvl_ok(nvl) ? FP_P_OK : FP_P_ERR_NOMEM;
}

static int add_num(nvlist_t* nvl, const char* name, uint64_t v) {
	nvlist_add_number(nvl, name, v);
	return nvl_ok(nvl) ? FP_P_OK : FP_P_ERR_NOMEM;
}

static int add_strlist(nvlist_t* nvl, const char* name, char* const* v,
		       size_t n) {
	if (n == 0) return FP_P_OK;
	if (n > FP_PROTO_MAX_LIST) return FP_P_ERR_TOO_BIG;
	if (v == NULL) return FP_P_ERR_MALFORMED;
	for (size_t i = 0; i < n; i++) {
		if (v[i] == NULL) return FP_P_ERR_MALFORMED;
		if (strlen(v[i]) > FP_PROTO_MAX_STR) return FP_P_ERR_TOO_BIG;
	}
	nvlist_add_string_array(nvl, name, (const char* const*)v, n);
	return nvl_ok(nvl) ? FP_P_OK : FP_P_ERR_NOMEM;
}

static int add_rows(nvlist_t* nvl, const fp_change_row_t* rows, size_t n) {
	const char** refs;
	const char** olds;
	const char** news;
	uint64_t* types;
	int e = FP_P_OK;

	if (n == 0) return FP_P_OK;
	if (n > FP_APPLY_MAX) return FP_P_ERR_MALFORMED;
	if (rows == NULL) return FP_P_ERR_MALFORMED;
	types = calloc(n, sizeof(*types));
	refs = calloc(n, sizeof(*refs));
	olds = calloc(n, sizeof(*olds));
	news = calloc(n, sizeof(*news));
	if (types == NULL || refs == NULL || olds == NULL || news == NULL) {
		e = FP_P_ERR_NOMEM;
		goto out;
	}
	for (size_t i = 0; i < n; i++) {
		if (rows[i].type > FP_APPLY_NOOP || rows[i].ref == NULL ||
		    strlen(rows[i].ref) > FP_PROTO_MAX_STR ||
		    (rows[i].old_version != NULL &&
		     strlen(rows[i].old_version) > FP_PROTO_MAX_STR) ||
		    (rows[i].new_version != NULL &&
		     strlen(rows[i].new_version) > FP_PROTO_MAX_STR)) {
			e = FP_P_ERR_MALFORMED;
			goto out;
		}
		types[i] = rows[i].type;
		refs[i] = rows[i].ref;
		olds[i] = rows[i].old_version != NULL ? rows[i].old_version : "";
		news[i] = rows[i].new_version != NULL ? rows[i].new_version : "";
	}
	nvlist_add_number_array(nvl, "row_types", types, n);
	nvlist_add_string_array(nvl, "row_refs", refs, n);
	nvlist_add_string_array(nvl, "row_old", olds, n);
	nvlist_add_string_array(nvl, "row_new", news, n);
	if (!nvl_ok(nvl)) e = FP_P_ERR_NOMEM;
out:
	free(types);
	free(refs);
	free(olds);
	free(news);
	return e;
}

// Append one frame to `w`, or leave it exactly as it was.
static int pack_frame(fp_wbuf_t* w, uint8_t kind, uint32_t type, uint32_t id,
		      nvlist_t* nvl) {
	uint8_t hdr[FP_PROTO_HDR_LEN];
	fp_frame_hdr_t h = {.kind = kind, .type = type, .id = id, .len = 0};
	size_t start = w->len;
	bool was_failed = w->failed;
	void* blob;
	size_t len = 0;
	int e = FP_P_OK;
	int nverr;

	if (nvl == NULL) return FP_P_ERR_NOMEM;
	nverr = nvlist_error(nvl);
	if (nverr != 0) {
		nvlist_destroy(nvl);
		return nverr == ENOMEM ? FP_P_ERR_NOMEM : FP_P_ERR_MALFORMED;
	}
	blob = nvlist_pack(nvl, &len);
	nvlist_destroy(nvl);
	if (blob == NULL)
		return errno == ENOMEM ? FP_P_ERR_NOMEM : FP_P_ERR_MALFORMED;
	if (len > FP_PROTO_MAX_PAYLOAD) {
		free(blob);
		return FP_P_ERR_TOO_BIG;
	}
	h.len = (uint32_t)len;
	fp_frame_hdr_encode(hdr, &h);
	w_put(w, hdr, sizeof(hdr));
	w_put(w, blob, len);
	free(blob);
	if (w->failed) {
		w->len = start;
		w->failed = was_failed;
		e = FP_P_ERR_NOMEM;
	}
	return e;
}

int fp_req_encode(fp_wbuf_t* w, uint32_t id, const fp_req_t* r) {
	nvlist_t* nvl = nvlist_create(0);
	int e = FP_P_OK;

	if (nvl == NULL) return FP_P_ERR_NOMEM;
	switch (r->type) {
	case FP_REQ_HELLO:
		e = add_num(nvl, "version", r->u.hello.version);
		if (e == FP_P_OK)
			e = add_str(nvl, "client", r->u.hello.client);
		break;
	case FP_REQ_SUBSCRIBE:
	case FP_REQ_UNSUBSCRIBE:
		if (!fp_topic_valid(r->u.sub.topic)) {
			e = FP_P_ERR_MALFORMED;
			break;
		}
		e = add_str(nvl, "topic", r->u.sub.topic);
		break;
	case FP_REQ_CANCEL:
		e = add_num(nvl, "op_id", r->u.cancel.op_id);
		break;
	case FP_REQ_ROLLBACK:
		e = add_str(nvl, "ref", r->u.rollback.ref);
		if (e == FP_P_OK)
			e = add_str(nvl, "commit", r->u.rollback.commit);
		break;
	case FP_REQ_RUN:
		e = add_str(nvl, "app_id", r->u.run.app_id);
		if (e == FP_P_OK)
			e = add_strlist(nvl, "args", r->u.run.args,
					r->u.run.nargs);
		if (e == FP_P_OK)
			e = add_str(nvl, "command", r->u.run.command);
		if (e == FP_P_OK) e = add_str(nvl, "branch", r->u.run.branch);
		if (e == FP_P_OK) e = add_str(nvl, "arch", r->u.run.arch);
		if (e == FP_P_OK) e = add_num(nvl, "flags", r->u.run.flags);
		if (e == FP_P_OK)
			e = add_strlist(nvl, "sockets", r->u.run.sockets,
					r->u.run.nsockets);
		if (e == FP_P_OK)
			e = add_strlist(nvl, "devices", r->u.run.devices,
					r->u.run.ndevices);
		if (e == FP_P_OK)
			e = add_strlist(nvl, "shares", r->u.run.shares,
					r->u.run.nshares);
		if (e == FP_P_OK)
			e = add_strlist(nvl, "fs_add", r->u.run.fs_add,
					r->u.run.nfs_add);
		if (e == FP_P_OK)
			e = add_strlist(nvl, "fs_remove", r->u.run.fs_remove,
					r->u.run.nfs_remove);
		if (e == FP_P_OK)
			e = add_strlist(nvl, "envs", r->u.run.envs,
					r->u.run.nenvs);
		if (e == FP_P_OK)
			e = add_str(nvl, "forward", r->u.run.forward);
		break;
	case FP_REQ_ENV:
		e = add_strlist(nvl, "vars", r->u.env.vars, r->u.env.nvars);
		break;
	case FP_REQ_STDIO:
		e = add_num(nvl, "count", r->u.stdio.count);
		break;
	case FP_REQ_INSTALL_BUNDLE:
		e = add_num(nvl, "flags", r->u.bundle.flags);
		break;
	case FP_REQ_EXPORTS:
		break;
	case FP_REQ_APPLY:
		if (r->u.apply.nrows > FP_APPLY_MAX) {
			e = FP_P_ERR_MALFORMED;
			break;
		}
		e = add_num(nvl, "flags", r->u.apply.flags);
		if (e == FP_P_OK)
			e = add_str(nvl, "remote", r->u.apply.remote);
		if (e == FP_P_OK)
			e = add_rows(nvl, r->u.apply.rows, r->u.apply.nrows);
		break;
	case FP_REQ_CLIENT_DECLARE:
		if (r->u.client_declare.nnames == 0 ||
		    r->u.client_declare.nnames > 16) {
			e = FP_P_ERR_MALFORMED;
			break;
		}
		e = add_strlist(nvl, "names", r->u.client_declare.names,
				r->u.client_declare.nnames);
		break;
	case FP_REQ_CLIENT_STATE:
		if (r->u.client_state.name == NULL) {
			e = FP_P_ERR_MALFORMED;
			break;
		}
		e = add_num(nvl, "flags", r->u.client_state.flags);
		if (e == FP_P_OK)
			e = add_str(nvl, "name", r->u.client_state.name);
		if (e == FP_P_OK)
			e = add_str(nvl, "data", r->u.client_state.data);
		break;
	case FP_REQ_PREFETCH:
		if (r->u.prefetch.nrefs == 0 ||
		    r->u.prefetch.nrefs > FP_APPLY_MAX) {
			e = FP_P_ERR_MALFORMED;
			break;
		}
		e = add_str(nvl, "remote", r->u.prefetch.remote);
		if (e == FP_P_OK)
			e = add_strlist(nvl, "refs", r->u.prefetch.refs,
					r->u.prefetch.nrefs);
		break;
	case FP_REQ_ENTER:
		e = add_str(nvl, "instance", r->u.enter.instance);
		if (e == FP_P_OK)
			e = add_strlist(nvl, "command", r->u.enter.command,
					r->u.enter.ncommand);
		break;
	case FP_REQ_KILL:
		e = add_str(nvl, "instance", r->u.kill.instance);
		break;
	case FP_REQ_IMPORT:
		e = add_str(nvl, "path", r->u.import.path);
		break;
	case FP_REQ_REMOTE_ADD:
		e = add_str(nvl, "name", r->u.remote_add.name);
		if (e == FP_P_OK)
			e = add_str(nvl, "url", r->u.remote_add.url);
		if (e == FP_P_OK)
			e = add_str(nvl, "from", r->u.remote_add.from);
		if (e == FP_P_OK)
			e = add_str(nvl, "data", r->u.remote_add.data);
		break;
	case FP_REQ_REMOTE_DELETE:
		e = add_str(nvl, "name", r->u.remote_delete.name);
		break;
	case FP_REQ_REMOTE_MODIFY:
		e = add_str(nvl, "name", r->u.remote_modify.name);
		if (e == FP_P_OK)
			e = add_str(nvl, "changes", r->u.remote_modify.changes);
		if (e == FP_P_OK)
			e = add_str(nvl, "gpg_data",
				    r->u.remote_modify.gpg_data);
		break;
	case FP_REQ_APPSTREAM:
		e = add_str(nvl, "remote", r->u.appstream.remote);
		if (e == FP_P_OK)
			e = add_str(nvl, "arch", r->u.appstream.arch);
		break;
	case FP_REQ_OVERRIDE:
		e = add_str(nvl, "app_id", r->u.override.app_id);
		if (e == FP_P_OK) e = add_num(nvl, "flags", r->u.override.flags);
		if (e == FP_P_OK)
			e = add_str(nvl, "changes", r->u.override.changes);
		break;
	default:
		e = FP_P_ERR_MALFORMED;
		break;
	}
	if (e != FP_P_OK) {
		nvlist_destroy(nvl);
		return e;
	}
	return pack_frame(w, FP_FRAME_REQUEST, r->type, id, nvl);
}

int fp_ack_encode(fp_wbuf_t* w, uint32_t id, const fp_ack_t* a) {
	nvlist_t* nvl = nvlist_create(0);
	int e = FP_P_OK;

	if (nvl == NULL) return FP_P_ERR_NOMEM;
	switch (a->type) {
	case FP_ACK_OK:
		e = add_num(nvl, "op_id", a->op_id);
		break;
	case FP_ACK_ERR:
		e = add_num(nvl, "code", a->code);
		if (e == FP_P_OK)
			e = add_str(nvl, "message", a->message);
		break;
	default:
		e = FP_P_ERR_MALFORMED;
		break;
	}
	if (e != FP_P_OK) {
		nvlist_destroy(nvl);
		return e;
	}
	return pack_frame(w, FP_FRAME_ACK, a->type, id, nvl);
}

int fp_event_encode(fp_wbuf_t* w, uint32_t seq, const fp_event_t* ev) {
	nvlist_t* nvl = nvlist_create(0);
	int e = FP_P_OK;

	if (nvl == NULL) return FP_P_ERR_NOMEM;
	if (!fp_topic_valid(ev->topic)) {
		nvlist_destroy(nvl);
		return FP_P_ERR_MALFORMED;
	}
	e = add_str(nvl, "topic", ev->topic);
	if (e == FP_P_OK) e = add_num(nvl, "op_id", ev->op_id);
	switch (ev->type) {
	case FP_EV_OP_STARTED:
		if (e == FP_P_OK) e = add_num(nvl, "kind", ev->u.started.kind);
		if (e == FP_P_OK)
			e = add_str(nvl, "label", ev->u.started.label);
		break;
	case FP_EV_OP_PROGRESS:
		if (e == FP_P_OK)
			e = add_str(nvl, "stage", ev->u.progress.stage);
		if (e == FP_P_OK)
			e = add_num(nvl, "current", ev->u.progress.current);
		if (e == FP_P_OK)
			e = add_num(nvl, "total", ev->u.progress.total);
		break;
	case FP_EV_OP_STATE:
		if (e == FP_P_OK)
			e = add_str(nvl, "state", ev->u.state.state);
		break;
	case FP_EV_OP_LOG:
		if (e == FP_P_OK) e = add_num(nvl, "level", ev->u.log.level);
		if (e == FP_P_OK)
			e = add_str(nvl, "scope", ev->u.log.scope);
		if (e == FP_P_OK) e = add_str(nvl, "text", ev->u.log.text);
		break;
	case FP_EV_OP_FINISHED:
		if (e == FP_P_OK)
			e = add_num(nvl, "status", ev->u.finished.status);
		if (e == FP_P_OK)
			e = add_str(nvl, "error", ev->u.finished.error);
		break;
	case FP_EV_OP_TABLE: {
		const uint32_t nc = ev->u.table.ncols, nr = ev->u.table.nrows;

		if (nc == 0 || nc > FP_PROTO_MAX_COLS ||
		    nr > FP_PROTO_MAX_LIST ||
		    (uint64_t)nc * nr > FP_PROTO_MAX_LIST ||
		    (nr > 0 && ev->u.table.cells == NULL)) {
			e = FP_P_ERR_MALFORMED;
			break;
		}
		if (e == FP_P_OK) e = add_num(nvl, "ncols", nc);
		if (e == FP_P_OK) e = add_num(nvl, "nrows", nr);
		if (e == FP_P_OK && ev->u.table.headers != NULL)
			e = add_strlist(nvl, "headers", ev->u.table.headers,
					nc);
		if (e == FP_P_OK && nc * nr > 0)
			e = add_strlist(nvl, "cells", ev->u.table.cells,
					(size_t)nc * nr);
		break;
	}
	case FP_EV_META:
		if (ev->u.meta.ref == NULL || ev->u.meta.metadata == NULL) {
			e = FP_P_ERR_MALFORMED;
			break;
		}
		if (e == FP_P_OK) e = add_str(nvl, "ref", ev->u.meta.ref);
		if (e == FP_P_OK)
			e = add_str(nvl, "commit", ev->u.meta.commit);
		if (e == FP_P_OK)
			e = add_str(nvl, "metadata", ev->u.meta.metadata);
		break;
	case FP_EV_STORE_CHANGED:
		if (ev->u.store.action == NULL || ev->u.store.ref == NULL) {
			e = FP_P_ERR_MALFORMED;
			break;
		}
		if (e == FP_P_OK)
			e = add_str(nvl, "action", ev->u.store.action);
		if (e == FP_P_OK) e = add_str(nvl, "ref", ev->u.store.ref);
		break;
	case FP_EV_REMOTE_CHANGED:
		if (ev->u.remote.name == NULL) {
			e = FP_P_ERR_MALFORMED;
			break;
		}
		e = add_str(nvl, "name", ev->u.remote.name);
		break;
	case FP_EV_INSTANCE_STARTED:
	case FP_EV_INSTANCE_EXITED:
		if (ev->u.instance.app_id == NULL) {
			e = FP_P_ERR_MALFORMED;
			break;
		}
		e = add_str(nvl, "app_id", ev->u.instance.app_id);
		break;
	case FP_EV_CLIENT_STATE:
		if (ev->u.client_state.name == NULL ||
		    ev->u.client_state.data == NULL) {
			e = FP_P_ERR_MALFORMED;
			break;
		}
		if (e == FP_P_OK)
			e = add_str(nvl, "name", ev->u.client_state.name);
		if (e == FP_P_OK)
			e = add_str(nvl, "data", ev->u.client_state.data);
		break;
	default:
		e = FP_P_ERR_MALFORMED;
		break;
	}
	if (e != FP_P_OK) {
		nvlist_destroy(nvl);
		return e;
	}
	return pack_frame(w, FP_FRAME_EVENT, ev->type, seq, nvl);
}

void fp_strlist_free(char** v, size_t n) {
	if (v == NULL) return;
	for (size_t i = 0; i < n; i++) free(v[i]);
	free(v);
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
		free(r->u.remote_add.data);
		break;
	case FP_REQ_REMOTE_DELETE:
		free(r->u.remote_delete.name);
		break;
	case FP_REQ_REMOTE_MODIFY:
		free(r->u.remote_modify.name);
		free(r->u.remote_modify.changes);
		free(r->u.remote_modify.gpg_data);
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
		// Only lists that decoded in full, with the counts the frame
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

// --- decoding -------------------------------------------------------------
//
// nvlist_get_*() aborts the process when a name is missing, and the names come
// from a caller-chosen frame: every read below asks nvlist_exists_*() first and
// a missing or mistyped field is FP_P_ERR_MALFORMED, never an exit in root's
// worker.

static nvlist_t* unpack_frame(const fp_frame_hdr_t* h, const uint8_t* payload,
			      int* err) {
	nvlist_t* nvl;

	*err = FP_P_OK;
	if (h->len == 0 || payload == NULL) {
		*err = FP_P_ERR_MALFORMED;
		return NULL;
	}
	errno = 0;
	nvl = nvlist_unpack(payload, h->len, 0);
	if (nvl == NULL) {
		*err = errno == ENOMEM ? FP_P_ERR_NOMEM : FP_P_ERR_MALFORMED;
		return NULL;
	}
	if (nvlist_error(nvl) != 0) {
		int e = nvlist_error(nvl);

		nvlist_destroy(nvl);
		*err = e == ENOMEM ? FP_P_ERR_NOMEM : FP_P_ERR_MALFORMED;
		return NULL;
	}
	return nvl;
}

static const char* need_str(const nvlist_t* nvl, const char* name, int* err) {
	const char* s;

	if (*err != FP_P_OK) return NULL;
	if (!nvlist_exists_string(nvl, name)) {
		*err = FP_P_ERR_MALFORMED;
		return NULL;
	}
	s = nvlist_get_string(nvl, name);
	if (strlen(s) > FP_PROTO_MAX_STR) {
		*err = FP_P_ERR_TOO_BIG;
		return NULL;
	}
	return s;
}

static uint64_t need_num(const nvlist_t* nvl, const char* name, int* err) {
	if (*err != FP_P_OK) return 0;
	if (!nvlist_exists_number(nvl, name)) {
		*err = FP_P_ERR_MALFORMED;
		return 0;
	}
	return nvlist_get_number(nvl, name);
}

static uint32_t need_u32(const nvlist_t* nvl, const char* name, int* err) {
	uint64_t v = need_num(nvl, name, err);

	if (*err != FP_P_OK) return 0;
	if (v > UINT32_MAX) {
		*err = FP_P_ERR_MALFORMED;
		return 0;
	}
	return (uint32_t)v;
}

static char* dup_str(const nvlist_t* nvl, const char* name, int* err) {
	const char* s = need_str(nvl, name, err);
	char* out;
	size_t n;

	if (s == NULL) return NULL;
	n = strlen(s);
	out = malloc(n + 1);
	if (out == NULL) {
		*err = FP_P_ERR_NOMEM;
		return NULL;
	}
	memcpy(out, s, n + 1);
	return out;
}

// A string array: absent is an empty list, present with any other type is a
// malformed frame (never silently read as empty).
static char** dup_strlist(const nvlist_t* nvl, const char* name, size_t* n_out,
			  int* err) {
	const char* const* v;
	size_t n, i;
	char** out;

	*n_out = 0;
	if (*err != FP_P_OK) return NULL;
	if (!nvlist_exists(nvl, name)) return NULL;
	if (!nvlist_exists_string_array(nvl, name)) {
		*err = FP_P_ERR_MALFORMED;
		return NULL;
	}
	v = nvlist_get_string_array(nvl, name, &n);
	if (n > FP_PROTO_MAX_LIST) {
		*err = FP_P_ERR_TOO_BIG;
		return NULL;
	}
	out = calloc(n + 1, sizeof(char*));
	if (out == NULL) {
		*err = FP_P_ERR_NOMEM;
		return NULL;
	}
	for (i = 0; i < n; i++) {
		size_t len;

		if (v[i] == NULL) {
			*err = FP_P_ERR_MALFORMED;
			break;
		}
		len = strlen(v[i]);
		if (len > FP_PROTO_MAX_STR) {
			*err = FP_P_ERR_TOO_BIG;
			break;
		}
		out[i] = malloc(len + 1);
		if (out[i] == NULL) {
			*err = FP_P_ERR_NOMEM;
			break;
		}
		memcpy(out[i], v[i], len + 1);
	}
	if (*err != FP_P_OK) {
		fp_strlist_free(out, i);
		return NULL;
	}
	*n_out = n;
	return out;
}

static fp_change_row_t* dup_rows(const nvlist_t* nvl, size_t* n_out,
				 int* err) {
	const uint64_t* types;
	const char* const* refs;
	const char* const* olds;
	const char* const* news;
	fp_change_row_t* rows;
	size_t n = 0, nr = 0, no = 0, nn = 0, i;

	*n_out = 0;
	if (*err != FP_P_OK) return NULL;
	// Parallel arrays, not a nested nvlist array: libnv's unpack of a
	// nested array that is cut short leaks what it had built, and this
	// decoder runs on hostile input in root's worker.
	if (!nvlist_exists(nvl, "row_types") && !nvlist_exists(nvl, "row_refs") &&
	    !nvlist_exists(nvl, "row_old") && !nvlist_exists(nvl, "row_new"))
		return NULL;
	if (!nvlist_exists_number_array(nvl, "row_types") ||
	    !nvlist_exists_string_array(nvl, "row_refs") ||
	    !nvlist_exists_string_array(nvl, "row_old") ||
	    !nvlist_exists_string_array(nvl, "row_new")) {
		*err = FP_P_ERR_MALFORMED;
		return NULL;
	}
	types = nvlist_get_number_array(nvl, "row_types", &n);
	refs = nvlist_get_string_array(nvl, "row_refs", &nr);
	olds = nvlist_get_string_array(nvl, "row_old", &no);
	news = nvlist_get_string_array(nvl, "row_new", &nn);
	if (n > FP_APPLY_MAX) {
		*err = FP_P_ERR_TOO_BIG;
		return NULL;
	}
	if (nr != n || no != n || nn != n) {
		*err = FP_P_ERR_MALFORMED;
		return NULL;
	}
	rows = calloc(n ? n : 1, sizeof(*rows));
	if (rows == NULL) {
		*err = FP_P_ERR_NOMEM;
		return NULL;
	}
	for (i = 0; i < n; i++) {
		fp_change_row_t* row = &rows[i];

		if (types[i] > FP_APPLY_NOOP || refs[i] == NULL ||
		    *refs[i] == '\0' || olds[i] == NULL || news[i] == NULL) {
			*err = FP_P_ERR_MALFORMED;
			break;
		}
		if (strlen(refs[i]) > FP_PROTO_MAX_STR ||
		    strlen(olds[i]) > FP_PROTO_MAX_STR ||
		    strlen(news[i]) > FP_PROTO_MAX_STR) {
			*err = FP_P_ERR_TOO_BIG;
			break;
		}
		row->type = (uint32_t)types[i];
		row->ref = strdup(refs[i]);
		row->old_version = strdup(olds[i]);
		row->new_version = strdup(news[i]);
		if (row->ref == NULL || row->old_version == NULL ||
		    row->new_version == NULL) {
			*err = FP_P_ERR_NOMEM;
			break;
		}
	}
	if (*err != FP_P_OK) {
		for (size_t k = 0; k <= i && k < n; k++) {
			free(rows[k].ref);
			free(rows[k].old_version);
			free(rows[k].new_version);
		}
		free(rows);
		return NULL;
	}
	*n_out = n;
	return rows;
}

static int hdr_is(const fp_frame_hdr_t* h, uint8_t kind) {
	if (h->kind != kind) return FP_P_ERR_KIND;
	if (h->len > FP_PROTO_MAX_PAYLOAD) return FP_P_ERR_TOO_BIG;
	return FP_P_OK;
}

int fp_req_decode(const fp_frame_hdr_t* h, const uint8_t* payload,
		  fp_req_t* out) {
	int err = FP_P_OK;
	nvlist_t* nvl;
	int e = hdr_is(h, FP_FRAME_REQUEST);

	memset(out, 0, sizeof(*out));
	if (e != FP_P_OK) return e;
	nvl = unpack_frame(h, payload, &err);
	if (nvl == NULL) return err;
	out->type = h->type;
	switch (h->type) {
	case FP_REQ_HELLO:
		out->u.hello.version = need_u32(nvl, "version", &err);
		out->u.hello.client = dup_str(nvl, "client", &err);
		break;
	case FP_REQ_SUBSCRIBE:
	case FP_REQ_UNSUBSCRIBE:
		out->u.sub.topic = dup_str(nvl, "topic", &err);
		if (err == FP_P_OK && !fp_topic_valid(out->u.sub.topic))
			err = FP_P_ERR_MALFORMED;
		break;
	case FP_REQ_CANCEL:
		out->u.cancel.op_id = need_num(nvl, "op_id", &err);
		break;
	case FP_REQ_ROLLBACK:
		out->u.rollback.ref = dup_str(nvl, "ref", &err);
		out->u.rollback.commit = dup_str(nvl, "commit", &err);
		break;
	case FP_REQ_RUN:
		out->u.run.app_id = dup_str(nvl, "app_id", &err);
		out->u.run.args =
			dup_strlist(nvl, "args", &out->u.run.nargs, &err);
		out->u.run.command = dup_str(nvl, "command", &err);
		out->u.run.branch = dup_str(nvl, "branch", &err);
		out->u.run.arch = dup_str(nvl, "arch", &err);
		out->u.run.flags = need_u32(nvl, "flags", &err);
		out->u.run.sockets = dup_strlist(nvl, "sockets",
						 &out->u.run.nsockets, &err);
		out->u.run.devices = dup_strlist(nvl, "devices",
						 &out->u.run.ndevices, &err);
		out->u.run.shares = dup_strlist(nvl, "shares",
						&out->u.run.nshares, &err);
		out->u.run.fs_add = dup_strlist(nvl, "fs_add",
						&out->u.run.nfs_add, &err);
		out->u.run.fs_remove = dup_strlist(nvl, "fs_remove",
						   &out->u.run.nfs_remove, &err);
		out->u.run.envs = dup_strlist(nvl, "envs", &out->u.run.nenvs,
					      &err);
		out->u.run.forward = dup_str(nvl, "forward", &err);
		break;
	case FP_REQ_ENV:
		out->u.env.vars =
			dup_strlist(nvl, "vars", &out->u.env.nvars, &err);
		break;
	case FP_REQ_STDIO:
		out->u.stdio.count = need_u32(nvl, "count", &err);
		if (err == FP_P_OK && out->u.stdio.count != FP_STDIO_FDS)
			err = FP_P_ERR_MALFORMED;
		break;
	case FP_REQ_INSTALL_BUNDLE:
		out->u.bundle.flags = need_u32(nvl, "flags", &err);
		if (err == FP_P_OK && out->u.bundle.flags != 0)
			err = FP_P_ERR_MALFORMED;
		break;
	case FP_REQ_EXPORTS:
		break;
	case FP_REQ_APPLY:
		out->u.apply.flags = need_u32(nvl, "flags", &err);
		out->u.apply.remote = dup_str(nvl, "remote", &err);
		out->u.apply.rows = dup_rows(nvl, &out->u.apply.nrows, &err);
		if (err == FP_P_OK && out->u.apply.nrows > FP_APPLY_MAX)
			err = FP_P_ERR_TOO_BIG;
		break;
	case FP_REQ_CLIENT_DECLARE:
		out->u.client_declare.names =
			dup_strlist(nvl, "names",
				    &out->u.client_declare.nnames, &err);
		if (err == FP_P_OK &&
		    (out->u.client_declare.nnames == 0 ||
		     out->u.client_declare.nnames > 16))
			err = FP_P_ERR_MALFORMED;
		break;
	case FP_REQ_CLIENT_STATE:
		out->u.client_state.flags = need_u32(nvl, "flags", &err);
		out->u.client_state.name = dup_str(nvl, "name", &err);
		out->u.client_state.data = dup_str(nvl, "data", &err);
		break;
	case FP_REQ_PREFETCH:
		out->u.prefetch.remote = dup_str(nvl, "remote", &err);
		out->u.prefetch.refs =
			dup_strlist(nvl, "refs", &out->u.prefetch.nrefs, &err);
		if (err == FP_P_OK &&
		    (out->u.prefetch.nrefs == 0 ||
		     out->u.prefetch.nrefs > FP_APPLY_MAX))
			err = FP_P_ERR_MALFORMED;
		break;
	case FP_REQ_ENTER:
		out->u.enter.instance = dup_str(nvl, "instance", &err);
		out->u.enter.command =
			dup_strlist(nvl, "command", &out->u.enter.ncommand,
				    &err);
		break;
	case FP_REQ_KILL:
		out->u.kill.instance = dup_str(nvl, "instance", &err);
		break;
	case FP_REQ_IMPORT:
		out->u.import.path = dup_str(nvl, "path", &err);
		break;
	case FP_REQ_REMOTE_ADD:
		out->u.remote_add.name = dup_str(nvl, "name", &err);
		out->u.remote_add.url = dup_str(nvl, "url", &err);
		out->u.remote_add.from = dup_str(nvl, "from", &err);
		out->u.remote_add.data = dup_str(nvl, "data", &err);
		break;
	case FP_REQ_REMOTE_DELETE:
		out->u.remote_delete.name = dup_str(nvl, "name", &err);
		break;
	case FP_REQ_REMOTE_MODIFY:
		out->u.remote_modify.name = dup_str(nvl, "name", &err);
		out->u.remote_modify.changes = dup_str(nvl, "changes", &err);
		out->u.remote_modify.gpg_data = dup_str(nvl, "gpg_data", &err);
		break;
	case FP_REQ_APPSTREAM:
		out->u.appstream.remote = dup_str(nvl, "remote", &err);
		out->u.appstream.arch = dup_str(nvl, "arch", &err);
		break;
	case FP_REQ_OVERRIDE:
		out->u.override.app_id = dup_str(nvl, "app_id", &err);
		out->u.override.flags = need_u32(nvl, "flags", &err);
		out->u.override.changes = dup_str(nvl, "changes", &err);
		break;
	default:
		err = FP_P_ERR_MALFORMED;
		break;
	}
	nvlist_destroy(nvl);
	if (err != FP_P_OK) {
		fp_req_free(out);
		return err;
	}
	return FP_P_OK;
}

int fp_ack_decode(const fp_frame_hdr_t* h, const uint8_t* payload,
		  fp_ack_t* out) {
	int err = FP_P_OK;
	nvlist_t* nvl;
	int e = hdr_is(h, FP_FRAME_ACK);

	memset(out, 0, sizeof(*out));
	if (e != FP_P_OK) return e;
	nvl = unpack_frame(h, payload, &err);
	if (nvl == NULL) return err;
	out->type = h->type;
	switch (h->type) {
	case FP_ACK_OK:
		out->op_id = need_num(nvl, "op_id", &err);
		break;
	case FP_ACK_ERR:
		out->code = need_u32(nvl, "code", &err);
		out->message = dup_str(nvl, "message", &err);
		break;
	default:
		err = FP_P_ERR_MALFORMED;
		break;
	}
	nvlist_destroy(nvl);
	if (err != FP_P_OK) {
		fp_ack_free(out);
		return err;
	}
	return FP_P_OK;
}

int fp_event_decode(const fp_frame_hdr_t* h, const uint8_t* payload,
		    fp_event_t* out) {
	int err = FP_P_OK;
	nvlist_t* nvl;
	char** table_headers = NULL;
	char** table_cells = NULL;
	size_t table_hn = 0, table_cn = 0;
	int e = hdr_is(h, FP_FRAME_EVENT);

	memset(out, 0, sizeof(*out));
	if (e != FP_P_OK) return e;
	nvl = unpack_frame(h, payload, &err);
	if (nvl == NULL) return err;
	out->type = h->type;
	out->topic = dup_str(nvl, "topic", &err);
	if (err == FP_P_OK && !fp_topic_valid(out->topic))
		err = FP_P_ERR_MALFORMED;
	out->op_id = need_num(nvl, "op_id", &err);
	switch (h->type) {
	case FP_EV_OP_STARTED:
		out->u.started.kind = need_u32(nvl, "kind", &err);
		out->u.started.label = dup_str(nvl, "label", &err);
		break;
	case FP_EV_OP_PROGRESS:
		out->u.progress.stage = dup_str(nvl, "stage", &err);
		out->u.progress.current =
			need_num(nvl, "current", &err);
		out->u.progress.total = need_num(nvl, "total", &err);
		break;
	case FP_EV_OP_STATE:
		out->u.state.state = dup_str(nvl, "state", &err);
		break;
	case FP_EV_OP_LOG:
		out->u.log.level = need_u32(nvl, "level", &err);
		out->u.log.scope = dup_str(nvl, "scope", &err);
		out->u.log.text = dup_str(nvl, "text", &err);
		break;
	case FP_EV_OP_FINISHED:
		out->u.finished.status = need_u32(nvl, "status", &err);
		out->u.finished.error = dup_str(nvl, "error", &err);
		break;
	case FP_EV_OP_TABLE: {
		uint32_t nc = need_u32(nvl, "ncols", &err);
		uint32_t nr = need_u32(nvl, "nrows", &err);
		size_t hn = 0, cn = 0;

		if (err != FP_P_OK) break;
		if (nc == 0 || nc > FP_PROTO_MAX_COLS) {
			err = FP_P_ERR_MALFORMED;
			break;
		}
		if (nr > FP_PROTO_MAX_LIST ||
		    (uint64_t)nc * nr > FP_PROTO_MAX_LIST) {
			err = FP_P_ERR_TOO_BIG;
			break;
		}
		if (nvlist_exists(nvl, "headers")) {
			table_headers = dup_strlist(nvl, "headers", &hn, &err);
			table_hn = hn;
			if (err == FP_P_OK && hn != nc)
				err = FP_P_ERR_MALFORMED;
		}
		table_cells = dup_strlist(nvl, "cells", &cn, &err);
		table_cn = cn;
		if (err == FP_P_OK && cn != (size_t)nc * nr)
			err = FP_P_ERR_MALFORMED;
		if (err != FP_P_OK) break;
		out->u.table.ncols = nc;
		out->u.table.nrows = nr;
		out->u.table.headers = table_headers;
		out->u.table.cells = table_cells;
		// The event owns both lists now.
		table_headers = NULL;
		table_cells = NULL;
		break;
	}
	case FP_EV_META:
		out->u.meta.ref = dup_str(nvl, "ref", &err);
		out->u.meta.commit = dup_str(nvl, "commit", &err);
		out->u.meta.metadata = dup_str(nvl, "metadata", &err);
		if (err == FP_P_OK && *out->u.meta.ref == '\0')
			err = FP_P_ERR_MALFORMED;
		break;
	case FP_EV_STORE_CHANGED:
		out->u.store.action = dup_str(nvl, "action", &err);
		out->u.store.ref = dup_str(nvl, "ref", &err);
		if (err == FP_P_OK &&
		    (*out->u.store.action == '\0' || *out->u.store.ref == '\0'))
			err = FP_P_ERR_MALFORMED;
		break;
	case FP_EV_REMOTE_CHANGED:
		out->u.remote.name = dup_str(nvl, "name", &err);
		if (err == FP_P_OK && *out->u.remote.name == '\0')
			err = FP_P_ERR_MALFORMED;
		break;
	case FP_EV_INSTANCE_STARTED:
	case FP_EV_INSTANCE_EXITED:
		out->u.instance.app_id = dup_str(nvl, "app_id", &err);
		if (err == FP_P_OK && *out->u.instance.app_id == '\0')
			err = FP_P_ERR_MALFORMED;
		break;
	case FP_EV_CLIENT_STATE:
		out->u.client_state.name = dup_str(nvl, "name", &err);
		out->u.client_state.data = dup_str(nvl, "data", &err);
		break;
	default:
		err = FP_P_ERR_MALFORMED;
		break;
	}
	nvlist_destroy(nvl);
	if (err != FP_P_OK) {
		fp_strlist_free(table_headers, table_hn);
		fp_strlist_free(table_cells, table_cn);
		fp_event_free(out);
		return err;
	}
	return FP_P_OK;
}

// --- stream transport -----------------------------------------------------

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
		ssize_t k = recvmsg(fd, &m, MSG_CMSG_CLOEXEC);
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
		// More descriptors than the control buffer holds: the kernel
		// dropped the rest and this is not a frame the protocol
		// carries.  Refuse it; the caller closes what did fit.
		if ((m.msg_flags & MSG_CTRUNC) != 0) return -1;
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
