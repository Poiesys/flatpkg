#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/endian.h>
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

// The frame is assembled in the nvlist, so a refused field cannot leave a
// half frame behind: the caller gets nothing on failure.
static int finish_build(nvlist_t** out, nvlist_t* nvl) {
	int nverr = nvlist_error(nvl);

	if (nverr != 0) {
		nvlist_destroy(nvl);
		return nverr == ENOMEM ? FP_P_ERR_NOMEM : FP_P_ERR_MALFORMED;
	}
	if (nvlist_size(nvl) > FP_PROTO_MAX_PAYLOAD) {
		nvlist_destroy(nvl);
		return FP_P_ERR_TOO_BIG;
	}
	*out = nvl;
	return FP_P_OK;
}

// The envelope every frame carries: what it is, which message kind it is and
// which request/sequence number it answers.
static int add_frame_fields(nvlist_t* nvl, uint8_t kind, uint32_t type,
			    uint32_t id) {
	int e = add_num(nvl, "kind", kind);

	if (e == FP_P_OK) e = add_num(nvl, "type", type);
	if (e == FP_P_OK) e = add_num(nvl, "id", id);
	return e;
}

int fp_req_encode(nvlist_t** out, uint32_t id, const fp_req_t* r) {
	nvlist_t* nvl = nvlist_create(0);
	int e;

	*out = NULL;
	if (nvl == NULL) return FP_P_ERR_NOMEM;
	e = add_frame_fields(nvl, FP_FRAME_REQUEST, r->type, id);
	if (e != FP_P_OK) {
		nvlist_destroy(nvl);
		return e;
	}
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
	return finish_build(out, nvl);
}

int fp_ack_encode(nvlist_t** out, uint32_t id, const fp_ack_t* a) {
	nvlist_t* nvl = nvlist_create(0);
	int e;

	*out = NULL;
	if (nvl == NULL) return FP_P_ERR_NOMEM;
	e = add_frame_fields(nvl, FP_FRAME_ACK, a->type, id);
	if (e != FP_P_OK) {
		nvlist_destroy(nvl);
		return e;
	}
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
	return finish_build(out, nvl);
}

int fp_event_encode(nvlist_t** out, uint32_t seq, const fp_event_t* ev) {
	nvlist_t* nvl = nvlist_create(0);
	int e;

	*out = NULL;
	if (nvl == NULL) return FP_P_ERR_NOMEM;
	if (!fp_topic_valid(ev->topic)) {
		nvlist_destroy(nvl);
		return FP_P_ERR_MALFORMED;
	}
	e = add_frame_fields(nvl, FP_FRAME_EVENT, ev->type, seq);
	if (e != FP_P_OK) {
		nvlist_destroy(nvl);
		return e;
	}
	e = add_str(nvl, "topic", ev->topic);
	if (e == FP_P_OK) e = add_num(nvl, "op_id", ev->op_id);
	switch (ev->type) {
	case FP_EV_OP_STARTED:
		if (e == FP_P_OK)
			e = add_num(nvl, "op_kind", ev->u.started.kind);
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
	return finish_build(out, nvl);
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

// Every frame carries its kind, its type and the request or sequence number
// it answers.  A frame handed to the wrong decoder is malformed, not
// reinterpreted.
static bool frame_fields(const nvlist_t* nvl, uint8_t kind, uint32_t* type_out,
			 uint32_t* id_out, int* err) {
	uint32_t kindv = need_u32(nvl, "kind", err);
	uint32_t type = need_u32(nvl, "type", err);
	uint32_t id = need_u32(nvl, "id", err);

	if (*err != FP_P_OK) return false;
	if (kindv != kind) {
		*err = FP_P_ERR_MALFORMED;
		return false;
	}
	*type_out = type;
	*id_out = id;
	return true;
}

int fp_req_decode(const nvlist_t* nvl, uint32_t* id_out, fp_req_t* out) {
	if (nvl == NULL || nvlist_error(nvl) != 0)
		return FP_P_ERR_MALFORMED;
	uint32_t type = 0, id = 0;
	int err = FP_P_OK;

	memset(out, 0, sizeof(*out));
	if (id_out != NULL) *id_out = 0;
	if (!frame_fields(nvl, FP_FRAME_REQUEST, &type, &id, &err)) return err;
	if (id_out != NULL) *id_out = id;
	out->type = type;
	switch (type) {
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
	if (err != FP_P_OK) {
		fp_req_free(out);
		return err;
	}
	return FP_P_OK;
}

int fp_ack_decode(const nvlist_t* nvl, uint32_t* id_out, fp_ack_t* out) {
	if (nvl == NULL || nvlist_error(nvl) != 0)
		return FP_P_ERR_MALFORMED;
	uint32_t type = 0, id = 0;
	int err = FP_P_OK;

	memset(out, 0, sizeof(*out));
	if (id_out != NULL) *id_out = 0;
	if (!frame_fields(nvl, FP_FRAME_ACK, &type, &id, &err)) return err;
	if (id_out != NULL) *id_out = id;
	out->type = type;
	switch (type) {
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
	if (err != FP_P_OK) {
		fp_ack_free(out);
		return err;
	}
	return FP_P_OK;
}

int fp_event_decode(const nvlist_t* nvl, uint32_t* id_out, fp_event_t* out) {
	if (nvl == NULL || nvlist_error(nvl) != 0)
		return FP_P_ERR_MALFORMED;
	uint32_t type = 0, id = 0;
	int err = FP_P_OK;
	char** table_headers = NULL;
	char** table_cells = NULL;
	size_t table_hn = 0, table_cn = 0;

	memset(out, 0, sizeof(*out));
	if (id_out != NULL) *id_out = 0;
	if (!frame_fields(nvl, FP_FRAME_EVENT, &type, &id, &err)) return err;
	if (id_out != NULL) *id_out = id;
	out->type = type;
	out->topic = dup_str(nvl, "topic", &err);
	if (err == FP_P_OK && !fp_topic_valid(out->topic))
		err = FP_P_ERR_MALFORMED;
	out->op_id = need_num(nvl, "op_id", &err);
	switch (type) {
	case FP_EV_OP_STARTED:
		out->u.started.kind = need_u32(nvl, "op_kind", &err);
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
	if (err != FP_P_OK) {
		fp_strlist_free(table_headers, table_hn);
		fp_strlist_free(table_cells, table_cn);
		fp_event_free(out);
		return err;
	}
	return FP_P_OK;
}

// --- stream transport -----------------------------------------------------

// libnv's wire header, which nvlist_send(2) writes in front of the packed
// list.  It is read with MSG_PEEK before nvlist_recv() so a frame over the
// payload cap, or one carrying more descriptors than the protocol allows, is
// refused before the library allocates anything for it.
struct fp_nv_header {
	uint8_t magic;
	uint8_t version;
	uint8_t flags;
	uint64_t descriptors;
	uint64_t size;
} __packed;

#define FP_NV_MAGIC 0x6c
#define FP_NV_VERSION 0x00
#define FP_NV_FLAG_BIG_ENDIAN 0x080u

static uint64_t nv_hdr64(const uint8_t* p, bool be) {
	uint64_t v;

	memcpy(&v, p, sizeof(v));
	return be ? be64toh(v) : le64toh(v);
}

// The header is 19 bytes: magic, version, flags, then the descriptor count and
// the body size in the sender's byte order.  The public flags are ours to
// refuse: this protocol never uses case-folding or duplicate names.
static int nv_header_check(const struct fp_nv_header* h, size_t* bodyp) {
	bool be;
	uint64_t size, descs;

	if (h->magic != FP_NV_MAGIC || h->version != FP_NV_VERSION)
		return FP_P_ERR_MAGIC;
	if ((h->flags & ~FP_NV_FLAG_BIG_ENDIAN) != 0)
		return FP_P_ERR_MALFORMED;
	be = (h->flags & FP_NV_FLAG_BIG_ENDIAN) != 0;
	size = nv_hdr64((const uint8_t*)&h->size, be);
	descs = nv_hdr64((const uint8_t*)&h->descriptors, be);
	if (size > FP_PROTO_MAX_PAYLOAD - sizeof(*h))
		return FP_P_ERR_TOO_BIG;
	if (descs > FP_MAX_FDS) return FP_P_ERR_TOO_BIG;
	*bodyp = (size_t)size;
	return FP_P_OK;
}

int fp_frame_recv(int fd, nvlist_t** out) {
	struct fp_nv_header h;
	ssize_t n;
	size_t body = 0;
	int e;

	if (out == NULL) return FP_P_ERR_MALFORMED;
	*out = NULL;
	// Wait for the header only.  A peer that sends half of it and stops is
	// a truncated frame, not a frame whose body is read.  MSG_WAITALL is
	// not honoured together with MSG_PEEK on this kernel, so peek what is
	// there and wait for the rest without consuming it.
	for (;;) {
		n = recv(fd, &h, sizeof(h), MSG_PEEK);
		if (n < 0) {
			if (errno == EINTR) continue;
			return FP_P_ERR_IO;
		}
		if ((size_t)n >= sizeof(h)) break;
		if (n == 0) return FP_P_EOF;
		{
			struct pollfd p = {fd, POLLIN, 0};
			int pr;

			do pr = poll(&p, 1, -1);
			while (pr < 0 && errno == EINTR);
			if (pr < 0) return FP_P_ERR_IO;
			// Data left over after the peer closed cannot become a
			// whole header: that is a truncated frame.
			if ((p.revents & (POLLHUP | POLLERR)) != 0)
				return FP_P_ERR_TRUNCATED;
		}
	}
	e = nv_header_check(&h, &body);
	if (e != FP_P_OK) return e;
	(void)body;
	errno = 0;
	*out = nvlist_recv(fd, 0);
	if (*out == NULL) {
		int err = errno;

		return err == ENOMEM ? FP_P_ERR_NOMEM : FP_P_ERR_MALFORMED;
	}
	return FP_P_OK;
}

size_t fp_nvl_fd_count(const nvlist_t* nvl) {
	void* cookie = NULL;
	size_t n = 0;
	const char* name;
	int type;

	if (nvl == NULL) return 0;
	while ((name = nvlist_next(nvl, &type, &cookie)) != NULL) {
		if (type == NV_TYPE_DESCRIPTOR) {
			n++;
		} else if (type == NV_TYPE_DESCRIPTOR_ARRAY) {
			size_t k = 0;

			(void)nvlist_get_descriptor_array(nvl, name, &k);
			n += k;
		}
	}
	return n;
}

int fp_frame_send(int fd, const nvlist_t* nvl) {
	if (nvl == NULL || nvlist_error(nvl) != 0)
		return FP_P_ERR_MALFORMED;
	if (nvlist_size(nvl) > FP_PROTO_MAX_PAYLOAD)
		return FP_P_ERR_TOO_BIG;
	if (fp_nvl_fd_count(nvl) > FP_MAX_FDS) return FP_P_ERR_TOO_BIG;
	{
		// nvlist_send(2) uses send(2)/sendmsg(2) without MSG_NOSIGNAL: a
		// peer that vanished mid-frame would otherwise kill the caller
		// with SIGPIPE.
		int one = 1;

		(void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one,
				 sizeof(one));
	}
	if (nvlist_send(fd, nvl) != 0) return FP_P_ERR_IO;
	return FP_P_OK;
}
