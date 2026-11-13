/* protocol-test.c - harness for src/shared/protocol.c, driven by tests/protocol.sh
 * with the sanitizers when the compiler has them.  A frame is one nvlist sent
 * with nvlist_send(), and the decoder runs as root on bytes an unprivileged
 * process chose. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/nv.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../include/protocol.h"

static int failures;
#define CHECK(cond, ...)                                                      \
	do {                                                                  \
		if (!(cond)) {                                                \
			failures++;                                           \
			printf("FAIL line %d: %s - ", __LINE__, #cond);       \
			printf(__VA_ARGS__);                                  \
			printf("\n");                                         \
		}                                                             \
	} while (0)

typedef struct {
	nvlist_t* nvl;
	uint32_t kind;
	uint32_t type;
} frame_t;

#define MAXFR 24
static frame_t corpus[MAXFR];
static int ncorpus;

static void add(nvlist_t* nvl, uint32_t kind, uint32_t type) {
	CHECK(nvl != NULL, "encode failed");
	if (nvl == NULL) return;
	CHECK(ncorpus < MAXFR, "corpus full");
	corpus[ncorpus].nvl = nvl;
	corpus[ncorpus].kind = kind;
	corpus[ncorpus].type = type;
	ncorpus++;
}

static void add_req(uint32_t id, const fp_req_t* r) {
	nvlist_t* nvl = NULL;

	CHECK(fp_req_encode(&nvl, id, r) == FP_P_OK, "request %u encodes",
	      r->type);
	add(nvl, FP_FRAME_REQUEST, r->type);
}

static void add_ack(uint32_t id, const fp_ack_t* a) {
	nvlist_t* nvl = NULL;

	CHECK(fp_ack_encode(&nvl, id, a) == FP_P_OK, "ack %u encodes", a->type);
	add(nvl, FP_FRAME_ACK, a->type);
}

static void add_event(uint32_t seq, const fp_event_t* ev) {
	nvlist_t* nvl = NULL;

	CHECK(fp_event_encode(&nvl, seq, ev) == FP_P_OK, "event %u encodes",
	      ev->type);
	add(nvl, FP_FRAME_EVENT, ev->type);
}

static void build_corpus(void) {
	char* refs[] = {"app/org.example.App/x86_64/stable",
			"runtime/org.example.Platform/x86_64/26.08"};
	char* args[] = {"--flag", "file name"};
	char* socks[] = {"x11", "!wayland"};
	char* shares[] = {"!network"};
	char* fsadd[] = {"home:ro"};
	char* fsrm[] = {"host"};
	char* envs[] = {"WAYLAND_DISPLAY=wayland-0"};
	fp_change_row_t rows[] = {
	    {FP_APPLY_INSTALL, refs[0], "1.0", "1.1"},
	    {FP_APPLY_REMOVE, refs[1], "2.0", NULL},
	};
	char* prefetch_refs[] = {refs[0]};
	char* decl[] = {"world"};
	fp_req_t rq[] = {
	    {.type = FP_REQ_HELLO, .u.hello = {FP_PROTO_VERSION, "test-client"}},
	    {.type = FP_REQ_SUBSCRIBE, .u.sub = {"op/42"}},
	    {.type = FP_REQ_UNSUBSCRIBE, .u.sub = {"store"}},
	    {.type = FP_REQ_CANCEL, .u.cancel = {0x1122334455667788ull}},
	    {.type = FP_REQ_RUN,
	     .u.run = {"org.example.App", args, 2, "app-cmd", "stable", "x86_64",
		       FP_RUN_DEVEL | FP_RUN_DIE_WITH_PARENT, socks, 2, NULL, 0,
		       shares, 1, fsadd, 1, fsrm, 1, envs, 1, "3,9"}},
	    {.type = FP_REQ_ENV, .u.env = {envs, 1}},
	    {.type = FP_REQ_STDIO, .u.stdio = {FP_STDIO_FDS}},
	    {.type = FP_REQ_INSTALL_BUNDLE, .u.bundle = {0}},
	    {.type = FP_REQ_APPLY, .u.apply = {0, "flathub", rows, 2}},
	    {.type = FP_REQ_PREFETCH,
	     .u.prefetch = {"flathub", prefetch_refs, 1}},
	    {.type = FP_REQ_CLIENT_STATE,
	     .u.client_state = {FP_CLIENT_STATE_WRITE, "world",
				"app/org.example.App/x86_64/stable\n"}},
	    {.type = FP_REQ_CLIENT_DECLARE, .u.client_declare = {decl, 1}},
	};
	fp_ack_t ak[] = {
	    {.type = FP_ACK_OK, .op_id = 7},
	    {.type = FP_ACK_ERR, .code = FP_E_DENIED, .message = "not in group"},
	};
	char* tbl_hdr[] = {"ID", "Operation", NULL};
	char* tbl_cells[] = {"app.A", "Install", "app.B", "Remove", NULL};
	fp_event_t ev[] = {
	    {.type = FP_EV_OP_STARTED, .topic = "op/7", .op_id = 7,
	     .u.started = {3, "install org.example.App"}},
	    {.type = FP_EV_OP_PROGRESS, .topic = "op/7", .op_id = 7,
	     .u.progress = {"fetch", 1024, 1u << 20}},
	    {.type = FP_EV_OP_STATE, .topic = "op/7", .op_id = 7,
	     .u.state = {"deploying"}},
	    {.type = FP_EV_OP_LOG, .topic = "op/7", .op_id = 7,
	     .u.log = {2, "ostree", "pulled 12 objects"}},
	    {.type = FP_EV_OP_FINISHED, .topic = "op/7", .op_id = 7,
	     .u.finished = {0, ""}},
	    {.type = FP_EV_OP_TABLE, .topic = "op/7", .op_id = 7,
	     .u.table = {2, 2, tbl_hdr, tbl_cells}},
	    {.type = FP_EV_META, .topic = "op/7", .op_id = 7,
	     .u.meta = {refs[0], "0123456789abcdef", "[Application]\n"}},
	    {.type = FP_EV_STORE_CHANGED, .topic = "store", .op_id = 0,
	     .u.store = {"install", refs[0]}},
	    {.type = FP_EV_REMOTE_CHANGED, .topic = "remote", .op_id = 0,
	     .u.remote = {"flathub"}},
	    {.type = FP_EV_INSTANCE_STARTED, .topic = "instance/app", .op_id = 0,
	     .u.instance = {"org.example.App"}},
	    {.type = FP_EV_INSTANCE_EXITED, .topic = "instance/app", .op_id = 0,
	     .u.instance = {"org.example.App"}},
	    {.type = FP_EV_CLIENT_STATE, .topic = "op/1", .op_id = 1,
	     .u.client_state = {"world", "app/org.example.App/x86_64/stable\n"}},
	};
	uint32_t id = 100;

	for (size_t i = 0; i < sizeof(rq) / sizeof(rq[0]); i++)
		add_req(id++, &rq[i]);
	for (size_t i = 0; i < sizeof(ak) / sizeof(ak[0]); i++)
		add_ack(id++, &ak[i]);
	for (size_t i = 0; i < sizeof(ev) / sizeof(ev[0]) && ncorpus < MAXFR; i++)
		add_event((uint32_t)i, &ev[i]);
}

static frame_t* corpus_of(uint32_t kind, uint32_t type) {
	for (int i = 0; i < ncorpus; i++)
		if (corpus[i].kind == kind && corpus[i].type == type)
			return &corpus[i];
	return NULL;
}

static int decode_any(nvlist_t* nvl) {
	uint32_t kind = 0;
	int e = FP_P_OK;

	if (nvl == NULL) return FP_P_ERR_MALFORMED;
	if (!nvlist_exists_number(nvl, "kind")) return FP_P_ERR_MALFORMED;
	kind = (uint32_t)nvlist_get_number(nvl, "kind");
	if (kind == FP_FRAME_REQUEST) {
		fp_req_t r;
		e = fp_req_decode(nvl, NULL, &r);
		if (e == FP_P_OK) fp_req_free(&r);
	} else if (kind == FP_FRAME_ACK) {
		fp_ack_t a;
		e = fp_ack_decode(nvl, NULL, &a);
		if (e == FP_P_OK) fp_ack_free(&a);
	} else if (kind == FP_FRAME_EVENT) {
		fp_event_t v;
		e = fp_event_decode(nvl, NULL, &v);
		if (e == FP_P_OK) fp_event_free(&v);
	} else {
		e = FP_P_ERR_MALFORMED;
	}
	return e;
}

static void test_roundtrip(void) {
	for (int i = 0; i < ncorpus; i++) {
		uint32_t kind = 0, type = 0;
		nvlist_t* copy;
		void* blob;
		size_t len = 0;

		CHECK(nvlist_exists_number(corpus[i].nvl, "kind") &&
			  nvlist_get_number(corpus[i].nvl, "kind") ==
			      corpus[i].kind,
		      "frame %d kind", i);
		kind = (uint32_t)nvlist_get_number(corpus[i].nvl, "kind");
		type = (uint32_t)nvlist_get_number(corpus[i].nvl, "type");
		CHECK(kind == corpus[i].kind && type == corpus[i].type,
		      "frame %d envelope", i);
		CHECK(decode_any(corpus[i].nvl) == FP_P_OK, "frame %d decodes", i);
		/* The wire is the packed nvlist: pack, unpack, decode. */
		blob = nvlist_pack(corpus[i].nvl, &len);
		CHECK(blob != NULL, "frame %d packs", i);
		if (blob == NULL) continue;
		copy = nvlist_unpack(blob, len, 0);
		free(blob);
		CHECK(copy != NULL, "frame %d unpacks", i);
		if (copy != NULL) {
			CHECK(decode_any(copy) == FP_P_OK, "frame %d re-decodes",
			      i);
			nvlist_destroy(copy);
		}
	}
	/* spot-check values survive the trip */
	frame_t* f;
	fp_req_t r;
	f = corpus_of(FP_FRAME_REQUEST, FP_REQ_CANCEL);
	CHECK(f != NULL && fp_req_decode(f->nvl, NULL, &r) == FP_P_OK &&
		  r.u.cancel.op_id == 0x1122334455667788ull,
	      "u64 survives");
	if (f != NULL) fp_req_free(&r);
	f = corpus_of(FP_FRAME_REQUEST, FP_REQ_RUN);
	CHECK(f != NULL && fp_req_decode(f->nvl, NULL, &r) == FP_P_OK &&
		  strcmp(r.u.run.app_id, "org.example.App") == 0 &&
		  r.u.run.nargs == 2 &&
		  strcmp(r.u.run.args[1], "file name") == 0 &&
		  r.u.run.flags == (FP_RUN_DEVEL | FP_RUN_DIE_WITH_PARENT) &&
		  r.u.run.nsockets == 2 &&
		  strcmp(r.u.run.sockets[1], "!wayland") == 0 &&
		  r.u.run.ndevices == 0 && r.u.run.nshares == 1 &&
		  r.u.run.nfs_add == 1 && r.u.run.nfs_remove == 1 &&
		  r.u.run.nenvs == 1 &&
		  strcmp(r.u.run.forward, "3,9") == 0,
	      "RUN fields");
	if (f != NULL) fp_req_free(&r);
	f = corpus_of(FP_FRAME_REQUEST, FP_REQ_APPLY);
	CHECK(f != NULL && fp_req_decode(f->nvl, NULL, &r) == FP_P_OK &&
		  r.u.apply.nrows == 2 &&
		  strcmp(r.u.apply.rows[0].ref, "app/org.example.App/x86_64/stable") ==
		      0 &&
		  strcmp(r.u.apply.rows[0].old_version, "1.0") == 0 &&
		  strcmp(r.u.apply.rows[1].new_version, "") == 0,
	      "APPLY rows");
	if (f != NULL) fp_req_free(&r);
	fp_event_t ev;
	f = corpus_of(FP_FRAME_EVENT, FP_EV_OP_PROGRESS);
	CHECK(f != NULL && fp_event_decode(f->nvl, NULL, &ev) == FP_P_OK &&
		  ev.u.progress.current == 1024 &&
		  ev.u.progress.total == (1u << 20) &&
		  strcmp(ev.topic, "op/7") == 0,
	      "progress values");
	if (f != NULL) fp_event_free(&ev);
}

/* A frame built by hand, as a peer would: envelope fields plus whatever the
 * test wants to put under them. */
static nvlist_t* frame(uint32_t kind, uint32_t type) {
	nvlist_t* nvl = nvlist_create(0);

	if (nvl == NULL) return NULL;
	nvlist_add_number(nvl, "kind", kind);
	nvlist_add_number(nvl, "type", type);
	nvlist_add_number(nvl, "id", 1);
	return nvl;
}

static void test_table_event(void) {
	frame_t* f = corpus_of(FP_FRAME_EVENT, FP_EV_OP_TABLE);
	fp_event_t ev;

	CHECK(f != NULL, "the corpus has a table frame");
	if (f == NULL) return;
	CHECK(fp_event_decode(f->nvl, NULL, &ev) == FP_P_OK, "table decodes");
	CHECK(ev.u.table.ncols == 2 && ev.u.table.nrows == 2, "table shape");
	CHECK(ev.u.table.headers != NULL &&
		  strcmp(ev.u.table.headers[1], "Operation") == 0,
	      "header");
	CHECK(strcmp(ev.u.table.cells[2], "app.B") == 0 &&
		  strcmp(ev.u.table.cells[3], "Remove") == 0,
	      "cells, row by row");
	fp_event_free(&ev);

	/* A table without headers, and one with no rows. */
	nvlist_t* nvl = frame(FP_FRAME_EVENT, FP_EV_OP_TABLE);
	char* cells[] = {"a", "b", "c", "d"};
	nvlist_add_string(nvl, "topic", "op/7");
	nvlist_add_number(nvl, "op_id", 7);
	nvlist_add_number(nvl, "ncols", 2);
	nvlist_add_number(nvl, "nrows", 2);
	nvlist_add_string_array(nvl, "cells", (const char* const*)cells, 4);
	CHECK(fp_event_decode(nvl, NULL, &ev) == FP_P_OK,
	      "table without headers");
	fp_event_free(&ev);
	nvlist_destroy(nvl);
	nvl = frame(FP_FRAME_EVENT, FP_EV_OP_TABLE);
	nvlist_add_string(nvl, "topic", "op/7");
	nvlist_add_number(nvl, "op_id", 7);
	nvlist_add_number(nvl, "ncols", 3);
	nvlist_add_number(nvl, "nrows", 0);
	CHECK(fp_event_decode(nvl, NULL, &ev) == FP_P_OK,
	      "a table of no rows");
	fp_event_free(&ev);
	nvlist_destroy(nvl);

	/* Hostile shapes, each refused whole. */
	struct {
		uint64_t nc, nr;
		const char* what;
	} bad[] = {
	    {0, 1, "no columns"},
	    {FP_PROTO_MAX_COLS + 1, 1, "too many columns"},
	    {2, FP_PROTO_MAX_LIST + 1, "too many rows"},
	    {16, 300, "more cells than the protocol allows"},
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		nvl = frame(FP_FRAME_EVENT, FP_EV_OP_TABLE);
		nvlist_add_string(nvl, "topic", "op/7");
		nvlist_add_number(nvl, "op_id", 7);
		nvlist_add_number(nvl, "ncols", bad[i].nc);
		nvlist_add_number(nvl, "nrows", bad[i].nr);
		CHECK(fp_event_decode(nvl, NULL, &ev) != FP_P_OK, "%s",
		      bad[i].what);
		if (nvl != NULL) nvlist_destroy(nvl);
	}

	/* Headers and cells must match the declared shape. */
	nvl = frame(FP_FRAME_EVENT, FP_EV_OP_TABLE);
	char* two[] = {"H1", "H2"};
	nvlist_add_string(nvl, "topic", "op/7");
	nvlist_add_number(nvl, "op_id", 7);
	nvlist_add_number(nvl, "ncols", 2);
	nvlist_add_number(nvl, "nrows", 2);
	nvlist_add_string_array(nvl, "headers", (const char* const*)two, 2);
	nvlist_add_string_array(nvl, "cells", (const char* const*)two, 2);
	CHECK(fp_event_decode(nvl, NULL, &ev) != FP_P_OK,
	      "fewer cells than the shape");
	nvlist_destroy(nvl);
	nvl = frame(FP_FRAME_EVENT, FP_EV_OP_TABLE);
	nvlist_add_string(nvl, "topic", "op/7");
	nvlist_add_number(nvl, "op_id", 7);
	nvlist_add_number(nvl, "ncols", 2);
	nvlist_add_number(nvl, "nrows", 2);
	nvlist_add_string(nvl, "headers", "not an array");
	nvlist_add_number(nvl, "cells", 1);
	CHECK(fp_event_decode(nvl, NULL, &ev) != FP_P_OK,
	      "headers and cells of the wrong type");
	nvlist_destroy(nvl);

	/* And the encoder refuses to send what the decoder would refuse. */
	char* hdr[] = {"a", NULL};
	char* one[] = {"x", NULL};
	fp_event_t badv = {.type = FP_EV_OP_TABLE, .topic = "op/7", .op_id = 7,
			   .u.table = {0, 1, hdr, one}};
	nvlist_t* out = NULL;
	CHECK(fp_event_encode(&out, 1, &badv) != FP_P_OK,
	      "encode refuses no columns");
	CHECK(out == NULL, "a refused event leaves no frame");
	badv.u.table.ncols = FP_PROTO_MAX_COLS + 1;
	CHECK(fp_event_encode(&out, 1, &badv) != FP_P_OK,
	      "encode refuses too many columns");
	badv.u.table.ncols = 1;
	badv.u.table.nrows = 1;
	badv.u.table.cells = NULL;
	CHECK(fp_event_encode(&out, 1, &badv) != FP_P_OK,
	      "encode refuses rows without cells");
}

static void test_hostile_fields(void) {
	nvlist_t* nvl;
	fp_req_t r;
	fp_ack_t a;
	fp_event_t ev;

	/* A frame of the wrong kind is refused by the decoder it was handed to. */
	nvl = frame(FP_FRAME_EVENT, FP_EV_OP_STATE);
	CHECK(fp_req_decode(nvl, NULL, &r) == FP_P_ERR_MALFORMED,
	      "an event decoded as a request");
	nvlist_destroy(nvl);

	/* Missing and mistyped fields. */
	nvl = frame(FP_FRAME_REQUEST, FP_REQ_HELLO);
	nvlist_add_number(nvl, "version", FP_PROTO_VERSION);
	CHECK(fp_req_decode(nvl, NULL, &r) == FP_P_ERR_MALFORMED,
	      "missing client");
	nvlist_destroy(nvl);
	nvl = frame(FP_FRAME_REQUEST, FP_REQ_HELLO);
	nvlist_add_number(nvl, "version", FP_PROTO_VERSION);
	nvlist_add_number(nvl, "client", 1);
	CHECK(fp_req_decode(nvl, NULL, &r) == FP_P_ERR_MALFORMED,
	      "client of the wrong type");
	nvlist_destroy(nvl);

	/* A string longer than the cap is refused even though libnv carried it
	 * whole. */
	size_t big = FP_PROTO_MAX_STR + 1;
	char* huge = calloc(1, big + 1);
	memset(huge, 'x', big);
	nvl = frame(FP_FRAME_REQUEST, FP_REQ_HELLO);
	nvlist_add_number(nvl, "version", FP_PROTO_VERSION);
	nvlist_add_string(nvl, "client", huge);
	CHECK(fp_req_decode(nvl, NULL, &r) == FP_P_ERR_TOO_BIG,
	      "string over the cap");
	nvlist_destroy(nvl);
	free(huge);

	/* A list over the cap is refused. */
	size_t n = FP_PROTO_MAX_LIST + 1;
	char** many = calloc(n, sizeof(char*));
	for (size_t i = 0; i < n; i++) many[i] = "x";
	nvl = frame(FP_FRAME_REQUEST, FP_REQ_PREFETCH);
	nvlist_add_string(nvl, "remote", "r");
	nvlist_add_string_array(nvl, "refs", (const char* const*)many, n);
	CHECK(fp_req_decode(nvl, NULL, &r) == FP_P_ERR_TOO_BIG,
	      "list over the cap");
	nvlist_destroy(nvl);
	free(many);

	/* An empty ref list is not a request. */
	nvl = frame(FP_FRAME_REQUEST, FP_REQ_PREFETCH);
	nvlist_add_string(nvl, "remote", "r");
	CHECK(fp_req_decode(nvl, NULL, &r) == FP_P_ERR_MALFORMED,
	      "remove nothing");
	nvlist_destroy(nvl);

	/* A 32-bit field cannot be widened by the wire. */
	nvl = frame(FP_FRAME_REQUEST, FP_REQ_STDIO);
	nvlist_add_number(nvl, "count", 0x100000000ull);
	CHECK(fp_req_decode(nvl, NULL, &r) == FP_P_ERR_MALFORMED,
	      "count over 32 bits");
	nvlist_destroy(nvl);

	/* Topics are validated on the way in. */
	nvl = frame(FP_FRAME_REQUEST, FP_REQ_SUBSCRIBE);
	nvlist_add_string(nvl, "topic", "../x");
	CHECK(fp_req_decode(nvl, NULL, &r) == FP_P_ERR_MALFORMED,
	      "subscribe to ../x");
	nvlist_destroy(nvl);
	const char* badtopics[] = {"", "../x", "a/../b", "A", "a b",
				  "/a", "a/", "a//b", "op/4\x01"};
	for (size_t i = 0; i < sizeof(badtopics) / sizeof(badtopics[0]); i++)
		CHECK(!fp_topic_valid(badtopics[i]), "topic '%s' accepted",
		      badtopics[i]);
	CHECK(fp_topic_valid("op/42") && fp_topic_valid("instance/app.x-y_z") &&
		  fp_topic_valid("store"),
	      "good topics");

	/* Apply rows are parallel arrays: they must agree in length. */
	{
		uint64_t types[2] = {FP_APPLY_INSTALL, FP_APPLY_REMOVE};
		const char* refs[1] = {"org.example.App"};
		const char* olds[2] = {"", ""};
		const char* news[2] = {"", ""};

		nvl = frame(FP_FRAME_REQUEST, FP_REQ_APPLY);
		nvlist_add_number(nvl, "flags", 0);
		nvlist_add_string(nvl, "remote", "flathub");
		nvlist_add_number_array(nvl, "row_types", types, 2);
		nvlist_add_string_array(nvl, "row_refs", refs, 1);
		nvlist_add_string_array(nvl, "row_old", olds, 2);
		nvlist_add_string_array(nvl, "row_new", news, 2);
		CHECK(fp_req_decode(nvl, NULL, &r) == FP_P_ERR_MALFORMED,
		      "rows of unequal length");
		nvlist_destroy(nvl);
	}

	/* The envelope is required, and a bad one is not a frame. */
	nvl = nvlist_create(0);
	nvlist_add_number(nvl, "type", FP_REQ_HELLO);
	nvlist_add_number(nvl, "id", 1);
	CHECK(fp_req_decode(nvl, NULL, &r) == FP_P_ERR_MALFORMED,
	      "missing kind");
	nvlist_destroy(nvl);
	nvl = frame(FP_FRAME_REQUEST, 999);
	CHECK(fp_req_decode(nvl, NULL, &r) == FP_P_ERR_MALFORMED,
	      "unknown request type");
	nvlist_destroy(nvl);
	nvl = frame(FP_FRAME_ACK, 999);
	CHECK(fp_ack_decode(nvl, NULL, &a) == FP_P_ERR_MALFORMED,
	      "unknown ack type");
	nvlist_destroy(nvl);
	nvl = frame(FP_FRAME_EVENT, 999);
	CHECK(fp_event_decode(nvl, NULL, &ev) == FP_P_ERR_MALFORMED,
	      "unknown event type");
	nvlist_destroy(nvl);

	/* The id is the caller's, and comes back even when a field is bad. */
	{
		uint32_t got_id = 0;

		nvl = frame(FP_FRAME_REQUEST, FP_REQ_CANCEL); // id 1
		CHECK(fp_req_decode(nvl, &got_id, &r) == FP_P_ERR_MALFORMED &&
			  got_id == 1,
		      "id survives a bad field");
		nvlist_destroy(nvl);
	}
}

static void test_encode_rules(void) {
	nvlist_t* out = NULL;
	fp_ack_t ok = {.type = FP_ACK_OK, .op_id = 1};

	CHECK(fp_ack_encode(&out, 1, &ok) == FP_P_OK && out != NULL,
	      "first frame");
	nvlist_destroy(out);
	out = NULL;

	/* A failed encode leaves nothing behind. */
	fp_req_t badtopic = {.type = FP_REQ_SUBSCRIBE, .u.sub = {"../etc"}};
	CHECK(fp_req_encode(&out, 2, &badtopic) == FP_P_ERR_MALFORMED &&
		  out == NULL,
	      "invalid topic refused");

	char* r1[] = {"a"};
	fp_req_t mism = {.type = FP_REQ_PREFETCH,
			 .u.prefetch = {"r", r1, FP_APPLY_MAX + 1}};
	CHECK(fp_req_encode(&out, 3, &mism) == FP_P_ERR_MALFORMED &&
		  out == NULL,
	      "too many refs");

	fp_req_t unk = {.type = 999};
	CHECK(fp_req_encode(&out, 4, &unk) == FP_P_ERR_MALFORMED &&
		  out == NULL,
	      "unknown type refused on encode");

	/* A frame past the cap is refused, not truncated. */
	size_t n = (FP_PROTO_MAX_PAYLOAD / FP_PROTO_MAX_STR) + 2;
	char* s = malloc(FP_PROTO_MAX_STR + 1);
	memset(s, 'z', FP_PROTO_MAX_STR);
	s[FP_PROTO_MAX_STR] = 0;
	char** many = calloc(n, sizeof(char*));
	for (size_t i = 0; i < n; i++) many[i] = s;
	fp_req_t huge = {.type = FP_REQ_PREFETCH, .u.prefetch = {"r", many, n}};
	CHECK(fp_req_encode(&out, 5, &huge) == FP_P_ERR_TOO_BIG && out == NULL,
	      "frame over the cap");
	free(many);
	free(s);
}

struct test_nv_hdr {
	uint8_t magic;
	uint8_t version;
	uint8_t flags;
	uint64_t descriptors;
	uint64_t size;
} __packed;

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

static int mkpair(int sv[2]) {
	return socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
}

static void try_header(int sv[2], uint8_t magic, uint8_t version,
		       uint8_t flags, uint64_t descs, uint64_t size,
		       int want) {
	struct test_nv_hdr h;
	nvlist_t* got = NULL;
	int e;

	memset(&h, 0, sizeof(h));
	h.magic = magic;
	h.version = version;
	h.flags = flags;
	h.descriptors = descs;
	h.size = size;
	CHECK(write_all(sv[1], &h, sizeof(h)) == 0, "write header");
	e = fp_frame_recv(sv[0], &got);
	CHECK(e == want, "header (magic %u flags %u descs %llu size %llu): %s",
	      magic, flags, (unsigned long long)descs,
	      (unsigned long long)size, fp_proto_strerror(e));
	nvlist_destroy(got);
}

static void test_frame_bounds(void) {
	int sv[2];

	/* A body past the cap, and more descriptors than the protocol carries,
	 * are refused from the header alone, before nvlist_recv allocates. */
	CHECK(mkpair(sv) == 0, "bounds pair 1");
	try_header(sv, 0x6c, 0, 0, 0, FP_PROTO_MAX_PAYLOAD, FP_P_ERR_TOO_BIG);
	close(sv[0]);
	close(sv[1]);
	CHECK(mkpair(sv) == 0, "bounds pair 2");
	try_header(sv, 0x6c, 0, 0, FP_MAX_FDS + 1, 0, FP_P_ERR_TOO_BIG);
	close(sv[0]);
	close(sv[1]);
	CHECK(mkpair(sv) == 0, "bounds pair 3");
	try_header(sv, 0x6c, 0, 0x02, 0, 0, FP_P_ERR_MALFORMED);
	close(sv[0]);
	close(sv[1]);
	CHECK(mkpair(sv) == 0, "bounds pair 4");
	try_header(sv, 0x41, 0, 0, 0, 0, FP_P_ERR_MAGIC);
	close(sv[0]);
	close(sv[1]);
	CHECK(mkpair(sv) == 0, "bounds pair 5");
	try_header(sv, 0x6c, 9, 0, 0, 0, FP_P_ERR_MAGIC);
	close(sv[0]);
	close(sv[1]);

	/* A close between frames, and one inside the header. */
	CHECK(mkpair(sv) == 0, "bounds pair 6");
	close(sv[1]);
	{
		nvlist_t* got = NULL;
		CHECK(fp_frame_recv(sv[0], &got) == FP_P_EOF,
		      "EOF between frames");
	}
	close(sv[0]);
	CHECK(mkpair(sv) == 0, "bounds pair 7");
	CHECK(write_all(sv[1], "01234", 5) == 0, "partial header");
	close(sv[1]);
	{
		nvlist_t* got = NULL;
		CHECK(fp_frame_recv(sv[0], &got) == FP_P_ERR_TRUNCATED,
		      "close inside the header");
	}
	close(sv[0]);

	/* A close inside the body is a malformed frame, not a valid one. */
	CHECK(mkpair(sv) == 0, "bounds pair 8");
	{
		nvlist_t* nvl = NULL;
		void* blob;
		size_t len = 0;

		fp_req_t hello = {.type = FP_REQ_HELLO,
				  .u.hello = {FP_PROTO_VERSION, "t"}};
		CHECK(fp_req_encode(&nvl, 1, &hello) == FP_P_OK, "hello");
		blob = nvlist_pack(nvl, &len);
		nvlist_destroy(nvl);
		CHECK(blob != NULL && len > 4, "packs");
		if (blob != NULL) {
			CHECK(write_all(sv[1], blob, len - 3) == 0,
			      "partial body");
			free(blob);
		}
		close(sv[1]);
		nvlist_t* got = NULL;
		CHECK(fp_frame_recv(sv[0], &got) != FP_P_OK,
		      "close inside the body");
		nvlist_destroy(got);
	}
	close(sv[0]);
}

static void test_stream(void) {
	int sv[2];
	nvlist_t* nvl = NULL;
	void* blob;
	size_t len = 0;
	frame_t* f = corpus_of(FP_FRAME_REQUEST, FP_REQ_RUN);

	CHECK(f != NULL, "the corpus has a RUN frame");
	if (f == NULL) return;
	blob = nvlist_pack(f->nvl, &len);
	CHECK(blob != NULL, "RUN packs");
	if (blob == NULL) return;

	/* A writer that sends one byte at a time: the reader reassembles. */
	CHECK(mkpair(sv) == 0, "stream pair 1");
	pid_t pid = fork();
	if (pid == 0) {
		close(sv[0]);
		for (size_t k = 0; k < len; k++)
			if (write_all(sv[1], (const char*)blob + k, 1) != 0)
				_exit(2);
		_exit(0);
	}
	close(sv[1]);
	{
		nvlist_t* got = NULL;
		int e = fp_frame_recv(sv[0], &got);

		CHECK(e == FP_P_OK, "byte-at-a-time frame: %s",
		      fp_proto_strerror(e));
		if (e == FP_P_OK) {
			fp_req_t r;
			CHECK(fp_req_decode(got, NULL, &r) == FP_P_OK,
			      "decodes after reassembly");
			fp_req_free(&r);
		}
		nvlist_destroy(got);
	}
	{
		nvlist_t* got = NULL;
		CHECK(fp_frame_recv(sv[0], &got) == FP_P_EOF,
		      "EOF between frames");
	}
	close(sv[0]);
	int st;
	waitpid(pid, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "writer exited cleanly");

	/* Junk is refused at the header, before any body is read. */
	CHECK(mkpair(sv) == 0, "stream pair 2");
	{
		uint8_t junk[19];
		nvlist_t* got = NULL;

		memset(junk, 0x41, sizeof(junk));
		CHECK(write_all(sv[1], junk, sizeof(junk)) == 0, "write junk");
		CHECK(fp_frame_recv(sv[0], &got) == FP_P_ERR_MAGIC,
		      "junk header");
		CHECK(got == NULL, "no frame allocated for junk");
	}
	close(sv[1]);
	close(sv[0]);

	/* Writing to a peer that is gone is an error, not SIGPIPE. */
	CHECK(mkpair(sv) == 0, "stream pair 3");
	CHECK(fp_req_encode(&nvl, 1, &(fp_req_t){.type = FP_REQ_EXPORTS}) ==
		  FP_P_OK,
	      "exports frame");
	if (nvl != NULL) {
		close(sv[0]);
		CHECK(fp_frame_send(sv[1], nvl) == FP_P_ERR_IO,
		      "send to a closed peer");
		nvlist_destroy(nvl);
	}
	close(sv[1]);
	free(blob);
}

static void test_fds(void) {
	int sv[2], p1[2], p2[2], p3[2];
	nvlist_t* nvl = NULL;
	fp_req_t sio = {.type = FP_REQ_STDIO, .u.stdio = {FP_STDIO_FDS}};
	int fds[3];

	CHECK(mkpair(sv) == 0, "fds socketpair");
	CHECK(pipe(p1) == 0 && pipe(p2) == 0 && pipe(p3) == 0, "fds pipes");
	fds[0] = p1[0];
	fds[1] = p2[1];
	fds[2] = p3[1];
	CHECK(fp_req_encode(&nvl, 6, &sio) == FP_P_OK, "STDIO frame");
	nvlist_add_descriptor(nvl, FP_FD_STDIN, fds[0]);
	nvlist_add_descriptor(nvl, FP_FD_STDOUT, fds[1]);
	nvlist_add_descriptor(nvl, FP_FD_STDERR, fds[2]);
	CHECK(nvlist_error(nvl) == 0, "descriptors added");
	CHECK(fp_frame_send(sv[1], nvl) == FP_P_OK, "send with descriptors");
	nvlist_destroy(nvl);

	{
		nvlist_t* got = NULL;
		fp_req_t r;
		int in, out, errfd;

		CHECK(fp_frame_recv(sv[0], &got) == FP_P_OK,
		      "receive with descriptors");
		CHECK(fp_nvl_fd_count(got) == FP_STDIO_FDS,
		      "three descriptors arrive");
		CHECK(fp_req_decode(got, NULL, &r) == FP_P_OK &&
			  r.u.stdio.count == FP_STDIO_FDS,
		      "the frame decodes");
		fp_req_free(&r);
		in = nvlist_take_descriptor(got, FP_FD_STDIN);
		out = nvlist_take_descriptor(got, FP_FD_STDOUT);
		errfd = nvlist_take_descriptor(got, FP_FD_STDERR);
		CHECK(in >= 0 && out >= 0 && errfd >= 0, "named descriptors");
		if (out >= 0) {
			char c = 0;

			CHECK(write(out, "x", 1) == 1,
			      "received descriptor is writable");
			CHECK(read(p2[0], &c, 1) == 1 && c == 'x',
			      "it is the sender's pipe");
		}
		close(in);
		close(out);
		close(errfd);
		nvlist_destroy(got);
	}

	/* A plain frame carries none, and the counter says so. */
	CHECK(fp_req_encode(&nvl, 7, &(fp_req_t){.type = FP_REQ_EXPORTS}) ==
		  FP_P_OK,
	      "plain frame");
	CHECK(fp_nvl_fd_count(nvl) == 0, "no descriptors counted");
	CHECK(fp_frame_send(sv[1], nvl) == FP_P_OK, "send plain frame");
	nvlist_destroy(nvl);
	{
		nvlist_t* got = NULL;

		CHECK(fp_frame_recv(sv[0], &got) == FP_P_OK &&
			  fp_nvl_fd_count(got) == 0,
		      "a plain frame carries no descriptors");
		nvlist_destroy(got);
	}

	/* More than FP_MAX_FDS is refused before it reaches the wire. */
	{
		int lots[FP_MAX_FDS + 1];
		nvlist_t* big = NULL;

		for (size_t i = 0; i < FP_MAX_FDS + 1; i++)
			lots[i] = open("/dev/null", O_RDONLY);
		CHECK(fp_req_encode(&big, 8, &(fp_req_t){.type = FP_REQ_EXPORTS}) ==
			  FP_P_OK,
		      "big frame");
		for (size_t i = 0; i < FP_MAX_FDS + 1; i++)
			if (lots[i] >= 0) {
				char name[32];

				snprintf(name, sizeof(name), FP_FD_PREFIX "%zu",
					 i);
				nvlist_add_descriptor(big, name, lots[i]);
			}
		CHECK(fp_frame_send(sv[1], big) == FP_P_ERR_TOO_BIG,
		      "too many descriptors refused");
		nvlist_destroy(big);
		for (size_t i = 0; i < FP_MAX_FDS + 1; i++)
			if (lots[i] >= 0) close(lots[i]);
	}

	close(p1[0]);
	close(p1[1]);
	close(p2[0]);
	close(p2[1]);
	close(p3[0]);
	close(p3[1]);
	close(sv[0]);
	close(sv[1]);
}

/* A peer that repeats a name is refused: libnv's unpack adopts the flags it
 * was given, so a blob packed as NV_FLAG_NO_UNIQUE is not our frame. */
static void test_duplicate_names(void) {
	int sv[2];
	nvlist_t* nvl = nvlist_create(NV_FLAG_NO_UNIQUE);
	void* blob;
	size_t len = 0;

	CHECK(nvl != NULL, "dup frame");
	nvlist_add_number(nvl, "kind", FP_FRAME_REQUEST);
	nvlist_add_number(nvl, "type", FP_REQ_HELLO);
	nvlist_add_number(nvl, "id", 1);
	nvlist_add_string(nvl, "client", "one");
	nvlist_add_string(nvl, "client", "two");
	blob = nvlist_pack(nvl, &len);
	nvlist_destroy(nvl);
	CHECK(blob != NULL, "dup packs");
	if (blob == NULL) return;
	CHECK(mkpair(sv) == 0, "dup pair");
	CHECK(write_all(sv[1], blob, len) == 0, "dup write");
	free(blob);
	{
		nvlist_t* got = NULL;

		CHECK(fp_frame_recv(sv[0], &got) == FP_P_ERR_MALFORMED,
		      "a duplicate name is not a frame");
		nvlist_destroy(got);
	}
	close(sv[0]);
	close(sv[1]);
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) {
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (uint32_t)(rng >> 16);
}

static void test_fuzz(void) {
	/* Mutate packed frames (flip bytes, cut short) and decode.  The result
	 * does not matter; surviving it, leak-free, does. */
	int accepted = 0, rejected = 0;

	for (int iter = 0; iter < 20000; iter++) {
		int i = (int)(rnd() % (uint32_t)ncorpus);
		size_t len = 0;
		void* blob = nvlist_pack(corpus[i].nvl, &len);
		uint8_t buf[4096];

		if (blob == NULL) continue;
		if (len > sizeof(buf)) {
			free(blob);
			continue;
		}
		memcpy(buf, blob, len);
		free(blob);
		int flips = 1 + (int)(rnd() % 3);
		for (int f = 0; f < flips; f++)
			buf[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8));
		if (rnd() % 4 == 0) len = rnd() % (len + 1);
		{
			nvlist_t* nvl = nvlist_unpack(buf, len, 0);
			int e = nvl != NULL ? decode_any(nvl)
					    : FP_P_ERR_MALFORMED;

			nvlist_destroy(nvl);
			if (e == FP_P_OK)
				accepted++;
			else
				rejected++;
		}
	}
	CHECK(rejected > 0, "the fuzz rejected nothing");
	printf("     fuzz: %d accepted, %d rejected\n", accepted, rejected);
}

int main(void) {
	build_corpus();
	test_roundtrip();
	test_table_event();
	test_hostile_fields();
	test_encode_rules();
	test_frame_bounds();
	test_stream();
	test_fds();
	test_duplicate_names();
	test_fuzz();
	for (int i = 0; i < ncorpus; i++) nvlist_destroy(corpus[i].nvl);
	if (failures) {
		printf("FAILED: %d check(s)\n", failures);
		return 1;
	}
	printf("PASS: %d nvlist frames round-trip and pack/unpack; the frame\n"
	       "      header refuses an over-sized body or too many descriptors\n"
	       "      before allocation; missing and mistyped fields, oversized\n"
	       "      strings and lists, duplicate names and mismatched row\n"
	       "      arrays are refused; a failed encode leaves no frame; the\n"
	       "      stream helpers reassemble, report EOF vs truncation, never\n"
	       "      raise SIGPIPE, and descriptors arrive by name.\n",
	       ncorpus);
	return 0;
}
