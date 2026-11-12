/* protocol-test.c - harness for src/shared/protocol.c, driven by tests/protocol.sh
 * with the sanitizers when the compiler has them.  The payloads are nvlists
 * packed by libnv, and the decoder runs as root on bytes an unprivileged
 * process chose. */
#include <errno.h>
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

/* Decode whatever frame sits at the start of buf, by kind. */
static int decode_any(const uint8_t* buf, size_t len) {
	fp_frame_hdr_t h;
	if (len < FP_PROTO_HDR_LEN) return FP_P_ERR_TRUNCATED;
	int e = fp_frame_hdr_decode(buf, &h);
	if (e != FP_P_OK) return e;
	if (len - FP_PROTO_HDR_LEN < h.len) return FP_P_ERR_TRUNCATED;
	const uint8_t* p = buf + FP_PROTO_HDR_LEN;
	if (h.kind == FP_FRAME_REQUEST) {
		fp_req_t r;
		e = fp_req_decode(&h, p, &r);
		if (e == FP_P_OK) fp_req_free(&r);
	} else if (h.kind == FP_FRAME_ACK) {
		fp_ack_t a;
		e = fp_ack_decode(&h, p, &a);
		if (e == FP_P_OK) fp_ack_free(&a);
	} else {
		fp_event_t v;
		e = fp_event_decode(&h, p, &v);
		if (e == FP_P_OK) fp_event_free(&v);
	}
	return e;
}

/* A corpus of valid frames, one of every message. */
#define MAXFR 24
#define FP_MAX_FDS_TEST 8
static fp_wbuf_t corpus[MAXFR];
static int ncorpus;

static void add(int e) {
	CHECK(e == FP_P_OK, "encode failed: %s", fp_proto_strerror(e));
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
	    {.type = FP_REQ_APPLY,
	     .u.apply = {0, "flathub", rows, 2}},
	    {.type = FP_REQ_PREFETCH,
	     .u.prefetch = {"flathub", prefetch_refs, 1}},
	    {.type = FP_REQ_CLIENT_STATE,
	     .u.client_state = {FP_CLIENT_STATE_WRITE, "world",
				"app/org.example.App/x86_64/stable\n"}},
	    {.type = FP_REQ_CLIENT_DECLARE,
	     .u.client_declare = {decl, 1}},
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
	for (size_t i = 0; i < sizeof(rq) / sizeof(rq[0]); i++) {
		fp_w_init(&corpus[ncorpus]);
		add(fp_req_encode(&corpus[ncorpus++], id++, &rq[i]));
	}
	for (size_t i = 0; i < sizeof(ak) / sizeof(ak[0]); i++) {
		fp_w_init(&corpus[ncorpus]);
		add(fp_ack_encode(&corpus[ncorpus++], id++, &ak[i]));
	}
	for (size_t i = 0; i < sizeof(ev) / sizeof(ev[0]) && ncorpus < MAXFR; i++) {
		fp_w_init(&corpus[ncorpus]);
		add(fp_event_encode(&corpus[ncorpus++], (uint32_t)i, &ev[i]));
	}
}

static fp_wbuf_t* corpus_of(uint32_t kind, uint32_t type) {
	for (int i = 0; i < ncorpus; i++) {
		fp_frame_hdr_t h;
		fp_frame_hdr_decode(corpus[i].p, &h);
		if (h.kind == kind && h.type == type) return &corpus[i];
	}
	return NULL;
}

static void test_roundtrip(void) {
	for (int i = 0; i < ncorpus; i++) {
		fp_frame_hdr_t h;
		CHECK(fp_frame_hdr_decode(corpus[i].p, &h) == FP_P_OK,
		      "frame %d header", i);
		CHECK(h.len + FP_PROTO_HDR_LEN == corpus[i].len,
		      "frame %d length", i);
		CHECK(decode_any(corpus[i].p, corpus[i].len) == FP_P_OK,
		      "frame %d decodes", i);
	}
	/* spot-check values survive the trip */
	fp_frame_hdr_t h;
	fp_req_t r;
	fp_frame_hdr_decode(corpus[3].p, &h);
	CHECK(fp_req_decode(&h, corpus[3].p + FP_PROTO_HDR_LEN, &r) == FP_P_OK,
	      "cancel");
	CHECK(r.u.cancel.op_id == 0x1122334455667788ull, "u64 survives");
	fp_req_free(&r);
	fp_frame_hdr_decode(corpus[4].p, &h);
	CHECK(fp_req_decode(&h, corpus[4].p + FP_PROTO_HDR_LEN, &r) == FP_P_OK,
	      "run");
	CHECK(strcmp(r.u.run.app_id, "org.example.App") == 0, "run app id");
	CHECK(r.u.run.nargs == 2 && strcmp(r.u.run.args[1], "file name") == 0,
	      "run args");
	CHECK(strcmp(r.u.run.command, "app-cmd") == 0 &&
		  strcmp(r.u.run.arch, "x86_64") == 0,
	      "run strings");
	CHECK(r.u.run.flags == (FP_RUN_DEVEL | FP_RUN_DIE_WITH_PARENT),
	      "run flags");
	CHECK(r.u.run.nsockets == 2 &&
		  strcmp(r.u.run.sockets[1], "!wayland") == 0,
	      "run sockets");
	CHECK(r.u.run.ndevices == 0 && r.u.run.nshares == 1 &&
		  r.u.run.nfs_add == 1 && r.u.run.nfs_remove == 1 &&
		  r.u.run.nenvs == 1,
	      "run list counts");
	CHECK(strcmp(r.u.run.forward, "3,9") == 0, "run forwarded targets");
	fp_req_free(&r);
	fp_frame_hdr_decode(corpus[8].p, &h);
	CHECK(fp_req_decode(&h, corpus[8].p + FP_PROTO_HDR_LEN, &r) == FP_P_OK,
	      "apply");
	CHECK(r.u.apply.nrows == 2 &&
		  strcmp(r.u.apply.rows[0].ref, "app/org.example.App/x86_64/stable") ==
		      0 &&
		  strcmp(r.u.apply.rows[0].old_version, "1.0") == 0 &&
		  strcmp(r.u.apply.rows[1].new_version, "") == 0,
	      "apply rows");
	fp_req_free(&r);
	fp_event_t ev;
	fp_frame_hdr_decode(corpus[15].p, &h);
	CHECK(fp_event_decode(&h, corpus[15].p + FP_PROTO_HDR_LEN, &ev) ==
		  FP_P_OK,
	      "progress");
	CHECK(ev.u.progress.current == 1024 &&
		  ev.u.progress.total == (1u << 20),
	      "progress values");
	CHECK(strcmp(ev.topic, "op/7") == 0, "topic");
	fp_event_free(&ev);
}

/* Build a payload from an nvlist the way a peer would: pack it and decode it
 * as the given frame type.  The caller owns `nvl` and it is consumed. */
static int decode_nvl(uint8_t kind, uint32_t type, nvlist_t* nvl) {
	void* blob;
	size_t len = 0;
	fp_frame_hdr_t h = {
	    .kind = kind, .type = type, .id = 1, .len = 0};
	int e;

	if (nvl == NULL) return FP_P_ERR_NOMEM;
	blob = nvlist_pack(nvl, &len);
	nvlist_destroy(nvl);
	if (blob == NULL) return FP_P_ERR_NOMEM;
	h.len = (uint32_t)len;
	if (kind == FP_FRAME_REQUEST) {
		fp_req_t r;
		e = fp_req_decode(&h, blob, &r);
		if (e == FP_P_OK) fp_req_free(&r);
	} else if (kind == FP_FRAME_ACK) {
		fp_ack_t a;
		e = fp_ack_decode(&h, blob, &a);
		if (e == FP_P_OK) fp_ack_free(&a);
	} else {
		fp_event_t v;
		e = fp_event_decode(&h, blob, &v);
		if (e == FP_P_OK) fp_event_free(&v);
	}
	free(blob);
	return e;
}

static void test_table_event(void) {
	fp_wbuf_t* t = corpus_of(FP_FRAME_EVENT, FP_EV_OP_TABLE);
	fp_event_t ev;
	fp_frame_hdr_t h;

	CHECK(t != NULL, "the corpus has a table frame");
	if (t == NULL) return;
	fp_frame_hdr_decode(t->p, &h);
	CHECK(fp_event_decode(&h, t->p + FP_PROTO_HDR_LEN, &ev) == FP_P_OK,
	      "table decodes");
	CHECK(ev.u.table.ncols == 2 && ev.u.table.nrows == 2, "table shape");
	CHECK(ev.u.table.headers != NULL &&
		  strcmp(ev.u.table.headers[1], "Operation") == 0,
	      "header");
	CHECK(strcmp(ev.u.table.cells[2], "app.B") == 0 &&
		  strcmp(ev.u.table.cells[3], "Remove") == 0,
	      "cells, row by row");
	fp_event_free(&ev);

	/* A table without headers, and one with no rows. */
	nvlist_t* nvl = nvlist_create(0);
	char* cells[] = {"a", "b", "c", "d"};
	nvlist_add_string(nvl, "topic", "op/7");
	nvlist_add_number(nvl, "op_id", 7);
	nvlist_add_number(nvl, "ncols", 2);
	nvlist_add_number(nvl, "nrows", 2);
	nvlist_add_string_array(nvl, "cells", (const char* const*)cells, 4);
	CHECK(decode_nvl(FP_FRAME_EVENT, FP_EV_OP_TABLE, nvl) == FP_P_OK,
	      "table without headers");
	nvl = nvlist_create(0);
	nvlist_add_string(nvl, "topic", "op/7");
	nvlist_add_number(nvl, "op_id", 7);
	nvlist_add_number(nvl, "ncols", 3);
	nvlist_add_number(nvl, "nrows", 0);
	CHECK(decode_nvl(FP_FRAME_EVENT, FP_EV_OP_TABLE, nvl) == FP_P_OK,
	      "a table of no rows");

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
		nvl = nvlist_create(0);
		nvlist_add_string(nvl, "topic", "op/7");
		nvlist_add_number(nvl, "op_id", 7);
		nvlist_add_number(nvl, "ncols", bad[i].nc);
		nvlist_add_number(nvl, "nrows", bad[i].nr);
		CHECK(decode_nvl(FP_FRAME_EVENT, FP_EV_OP_TABLE, nvl) != FP_P_OK,
		      "%s", bad[i].what);
	}

	/* Headers and cells must match the declared shape. */
	nvl = nvlist_create(0);
	char* two[] = {"H1", "H2"};
	nvlist_add_string(nvl, "topic", "op/7");
	nvlist_add_number(nvl, "op_id", 7);
	nvlist_add_number(nvl, "ncols", 2);
	nvlist_add_number(nvl, "nrows", 2);
	nvlist_add_string_array(nvl, "headers", (const char* const*)two, 2);
	nvlist_add_string_array(nvl, "cells", (const char* const*)two, 2);
	CHECK(decode_nvl(FP_FRAME_EVENT, FP_EV_OP_TABLE, nvl) != FP_P_OK,
	      "fewer cells than the shape");
	nvl = nvlist_create(0);
	nvlist_add_string(nvl, "topic", "op/7");
	nvlist_add_number(nvl, "op_id", 7);
	nvlist_add_number(nvl, "ncols", 2);
	nvlist_add_number(nvl, "nrows", 2);
	nvlist_add_string(nvl, "headers", "not an array");
	nvlist_add_number(nvl, "cells", 1);
	CHECK(decode_nvl(FP_FRAME_EVENT, FP_EV_OP_TABLE, nvl) != FP_P_OK,
	      "headers and cells of the wrong type");

	/* And the encoder refuses to send what the decoder would refuse. */
	char* hdr[] = {"a", NULL};
	char* one[] = {"x", NULL};
	fp_event_t badv = {.type = FP_EV_OP_TABLE, .topic = "op/7", .op_id = 7,
			   .u.table = {0, 1, hdr, one}};
	fp_wbuf_t w;
	fp_w_init(&w);
	CHECK(fp_event_encode(&w, 1, &badv) != FP_P_OK, "encode refuses no columns");
	CHECK(w.len == 0, "a refused event leaves the buffer alone");
	badv.u.table.ncols = FP_PROTO_MAX_COLS + 1;
	CHECK(fp_event_encode(&w, 1, &badv) != FP_P_OK,
	      "encode refuses too many columns");
	badv.u.table.ncols = 1;
	badv.u.table.nrows = 1;
	badv.u.table.cells = NULL;
	CHECK(fp_event_encode(&w, 1, &badv) != FP_P_OK,
	      "encode refuses rows without cells");
	fp_w_free(&w);
}

static void test_header_rejects(void) {
	uint8_t hdr[FP_PROTO_HDR_LEN];
	fp_frame_hdr_t h;
	memcpy(hdr, corpus[0].p, sizeof(hdr));
	hdr[0] ^= 0xff;
	CHECK(fp_frame_hdr_decode(hdr, &h) == FP_P_ERR_MAGIC, "bad magic");
	memcpy(hdr, corpus[0].p, sizeof(hdr));
	hdr[2] = FP_PROTO_VERSION + 1;
	CHECK(fp_frame_hdr_decode(hdr, &h) == FP_P_ERR_VERSION, "bad version");
	memcpy(hdr, corpus[0].p, sizeof(hdr));
	hdr[3] = 0;
	CHECK(fp_frame_hdr_decode(hdr, &h) == FP_P_ERR_KIND, "kind 0");
	hdr[3] = 4;
	CHECK(fp_frame_hdr_decode(hdr, &h) == FP_P_ERR_KIND, "kind 4");
	memcpy(hdr, corpus[0].p, sizeof(hdr));
	hdr[12] = 0xff;
	CHECK(fp_frame_hdr_decode(hdr, &h) == FP_P_ERR_TOO_BIG,
	      "len over the cap");
	CHECK(h.len == 0 && h.kind == 0,
	      "a rejected header leaves nothing behind");

	/* a request frame handed to the event decoder is the wrong kind */
	fp_frame_hdr_decode(corpus[0].p, &h);
	fp_event_t ev;
	CHECK(fp_event_decode(&h, corpus[0].p + FP_PROTO_HDR_LEN, &ev) ==
		  FP_P_ERR_KIND,
	      "kind mismatch");
}

static void test_every_truncation(void) {
	for (int i = 0; i < ncorpus; i++) {
		fp_frame_hdr_t h;
		fp_frame_hdr_decode(corpus[i].p, &h);
		for (uint32_t k = 0; k < h.len; k++) {
			fp_frame_hdr_t t = h;
			t.len = k; /* the payload ends after k bytes */
			int e;
			if (h.kind == FP_FRAME_REQUEST) {
				fp_req_t r;
				e = fp_req_decode(&t, corpus[i].p +
							 FP_PROTO_HDR_LEN,
						  &r);
				if (e == FP_P_OK) fp_req_free(&r);
			} else if (h.kind == FP_FRAME_ACK) {
				fp_ack_t a;
				e = fp_ack_decode(&t, corpus[i].p +
							 FP_PROTO_HDR_LEN,
						  &a);
				if (e == FP_P_OK) fp_ack_free(&a);
			} else {
				fp_event_t v;
				e = fp_event_decode(&t, corpus[i].p +
							   FP_PROTO_HDR_LEN,
						    &v);
				if (e == FP_P_OK) fp_event_free(&v);
			}
			CHECK(e != FP_P_OK,
			      "frame %d truncated to %u bytes decoded", i, k);
		}
		/* one trailing byte is also a rejection: the nvlist carries
		 * its own size and unpack refuses a buffer that is not it */
		uint8_t* big = malloc(h.len + 1);
		memcpy(big, corpus[i].p + FP_PROTO_HDR_LEN, h.len);
		big[h.len] = 0;
		fp_frame_hdr_t t = h;
		t.len = h.len + 1;
		int e;
		if (h.kind == FP_FRAME_REQUEST) {
			fp_req_t r;
			e = fp_req_decode(&t, big, &r);
			if (e == FP_P_OK) fp_req_free(&r);
		} else if (h.kind == FP_FRAME_ACK) {
			fp_ack_t a;
			e = fp_ack_decode(&t, big, &a);
			if (e == FP_P_OK) fp_ack_free(&a);
		} else {
			fp_event_t v;
			e = fp_event_decode(&t, big, &v);
			if (e == FP_P_OK) fp_event_free(&v);
		}
		CHECK(e != FP_P_OK, "frame %d with a trailing byte: %s", i,
		      fp_proto_strerror(e));
		free(big);
	}
}

static void test_hostile_fields(void) {
	/* a missing field, and a field of the wrong type */
	nvlist_t* nvl = nvlist_create(0);
	nvlist_add_number(nvl, "version", FP_PROTO_VERSION);
	CHECK(decode_nvl(FP_FRAME_REQUEST, FP_REQ_HELLO, nvl) ==
		  FP_P_ERR_MALFORMED,
	      "missing client");
	nvl = nvlist_create(0);
	nvlist_add_number(nvl, "version", FP_PROTO_VERSION);
	nvlist_add_number(nvl, "client", 1);
	CHECK(decode_nvl(FP_FRAME_REQUEST, FP_REQ_HELLO, nvl) ==
		  FP_P_ERR_MALFORMED,
	      "client of the wrong type");

	/* a string longer than the cap is refused even though libnv
	 * carried it whole */
	size_t big = FP_PROTO_MAX_STR + 1;
	char* huge = calloc(1, big + 1);
	memset(huge, 'x', big);
	nvl = nvlist_create(0);
	nvlist_add_number(nvl, "version", FP_PROTO_VERSION);
	nvlist_add_string(nvl, "client", huge);
	CHECK(decode_nvl(FP_FRAME_REQUEST, FP_REQ_HELLO, nvl) ==
		  FP_P_ERR_TOO_BIG,
	      "string over the cap");
	free(huge);

	/* a list over the cap is refused before anything is typed into it */
	size_t n = FP_PROTO_MAX_LIST + 1;
	char** many = calloc(n, sizeof(char*));
	for (size_t i = 0; i < n; i++) many[i] = "x";
	nvl = nvlist_create(0);
	nvlist_add_string(nvl, "remote", "r");
	nvlist_add_string_array(nvl, "refs", (const char* const*)many, n);
	CHECK(decode_nvl(FP_FRAME_REQUEST, FP_REQ_PREFETCH, nvl) ==
		  FP_P_ERR_TOO_BIG,
	      "list over the cap");
	free(many);

	/* an empty ref list is not a request, and an absent one is empty */
	nvl = nvlist_create(0);
	nvlist_add_string(nvl, "remote", "r");
	CHECK(decode_nvl(FP_FRAME_REQUEST, FP_REQ_PREFETCH, nvl) ==
		  FP_P_ERR_MALFORMED,
	      "remove nothing");

	/* a 32-bit field cannot be widened by the wire */
	nvl = nvlist_create(0);
	nvlist_add_number(nvl, "count", 0x100000000ull);
	CHECK(decode_nvl(FP_FRAME_REQUEST, FP_REQ_STDIO, nvl) ==
		  FP_P_ERR_MALFORMED,
	      "count over 32 bits");

	/* topics are validated on the way in */
	const char* badtopics[] = {"", "../x", "a/../b", "A", "a b",
				  "/a", "a/", "a//b", "op/4\x01"};
	for (size_t i = 0; i < sizeof(badtopics) / sizeof(badtopics[0]); i++)
		CHECK(!fp_topic_valid(badtopics[i]), "topic '%s' accepted",
		      badtopics[i]);
	CHECK(fp_topic_valid("op/42") && fp_topic_valid("instance/app.x-y_z") &&
		  fp_topic_valid("store"),
	      "good topics");
	char longt[FP_PROTO_MAX_TOPIC + 2];
	memset(longt, 'a', sizeof(longt) - 1);
	longt[sizeof(longt) - 1] = 0;
	CHECK(!fp_topic_valid(longt), "topic over the cap");
	nvl = nvlist_create(0);
	nvlist_add_string(nvl, "topic", "../x");
	CHECK(decode_nvl(FP_FRAME_REQUEST, FP_REQ_SUBSCRIBE, nvl) ==
		  FP_P_ERR_MALFORMED,
	      "subscribe to ../x");

	/* duplicate names are ambiguity, not a feature: libnv's unpack
	 * adopts the flags it was given, so a blob packed with NO_UNIQUE is
	 * refused before a name is read twice */
	nvl = nvlist_create(NV_FLAG_NO_UNIQUE);
	nvlist_add_number(nvl, "version", FP_PROTO_VERSION);
	nvlist_add_string(nvl, "client", "one");
	nvlist_add_string(nvl, "client", "two");
	CHECK(decode_nvl(FP_FRAME_REQUEST, FP_REQ_HELLO, nvl) != FP_P_OK,
	      "duplicate names accepted");

	/* Apply rows are parallel arrays: they must agree in length, or the
	 * frame is refused whole. */
	{
		uint64_t types[2] = {FP_APPLY_INSTALL, FP_APPLY_REMOVE};
		const char* refs[1] = {"org.example.App"};
		const char* olds[2] = {"", ""};
		const char* news[2] = {"", ""};

		nvl = nvlist_create(0);
		nvlist_add_number(nvl, "flags", 0);
		nvlist_add_string(nvl, "remote", "flathub");
		nvlist_add_number_array(nvl, "row_types", types, 2);
		nvlist_add_string_array(nvl, "row_refs", refs, 1);
		nvlist_add_string_array(nvl, "row_old", olds, 2);
		nvlist_add_string_array(nvl, "row_new", news, 2);
		CHECK(decode_nvl(FP_FRAME_REQUEST, FP_REQ_APPLY, nvl) ==
			  FP_P_ERR_MALFORMED,
		      "rows of unequal length");
	}
}

static void test_encode_rules(void) {
	fp_wbuf_t w;
	fp_w_init(&w);
	fp_ack_t ok = {.type = FP_ACK_OK, .op_id = 1};
	CHECK(fp_ack_encode(&w, 1, &ok) == FP_P_OK, "first frame");
	size_t before = w.len;

	/* a failed encode leaves the buffer as it was, so a half frame can
	 * never be queued behind a good one */
	fp_req_t badtopic = {.type = FP_REQ_SUBSCRIBE, .u.sub = {"../etc"}};
	CHECK(fp_req_encode(&w, 2, &badtopic) == FP_P_ERR_MALFORMED,
	      "invalid topic refused");
	CHECK(w.len == before && !w.failed,
	      "buffer unchanged after a refused encode");

	char* r1[] = {"a"};
	fp_req_t mism = {.type = FP_REQ_PREFETCH,
			 .u.prefetch = {"r", r1, FP_APPLY_MAX + 1}};
	CHECK(fp_req_encode(&w, 3, &mism) == FP_P_ERR_MALFORMED,
	      "too many refs");
	CHECK(w.len == before, "unchanged after an oversized list");

	fp_req_t unk = {.type = 999};
	CHECK(fp_req_encode(&w, 4, &unk) == FP_P_ERR_MALFORMED,
	      "unknown type refused on encode");
	CHECK(w.len == before, "unchanged after an unknown type");

	/* a payload past the cap is refused, not truncated */
	size_t n = (FP_PROTO_MAX_PAYLOAD / FP_PROTO_MAX_STR) + 2;
	char* s = malloc(FP_PROTO_MAX_STR + 1);
	memset(s, 'z', FP_PROTO_MAX_STR);
	s[FP_PROTO_MAX_STR] = 0;
	char** many = calloc(n, sizeof(char*));
	for (size_t i = 0; i < n; i++) many[i] = s;
	fp_req_t huge = {.type = FP_REQ_PREFETCH, .u.prefetch = {"r", many, n}};
	CHECK(fp_req_encode(&w, 5, &huge) == FP_P_ERR_TOO_BIG,
	      "payload over the cap");
	CHECK(w.len == before && !w.failed,
	      "unchanged after an oversized frame");
	free(many);
	free(s);

	/* and the buffer still works afterwards */
	CHECK(fp_ack_encode(&w, 6, &ok) == FP_P_OK && w.len == 2 * before,
	      "still usable");
	fp_w_free(&w);
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) {
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (uint32_t)(rng >> 16);
}

static void test_fuzz(void) {
	/* Mutate valid frames (flip bytes, cut short) and decode.  The
	 * result does not matter; surviving it, leak-free, does. */
	int accepted = 0, rejected = 0;
	for (int iter = 0; iter < 20000; iter++) {
		int i = (int)(rnd() % (uint32_t)ncorpus);
		size_t len = corpus[i].len;
		uint8_t buf[4096];
		if (len > sizeof(buf)) continue;
		memcpy(buf, corpus[i].p, len);
		int flips = 1 + (int)(rnd() % 3);
		for (int f = 0; f < flips; f++)
			buf[rnd() % len] ^= (uint8_t)(1u << (rnd() % 8));
		if (rnd() % 4 == 0) len = rnd() % (len + 1);
		if (decode_any(buf, len) == FP_P_OK)
			accepted++;
		else
			rejected++;
	}
	CHECK(rejected > 0, "the fuzz rejected nothing");
	printf("     fuzz: %d accepted, %d rejected\n", accepted, rejected);
}

static void test_stream(void) {
	int sv[2];
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair");

	/* a writer that sends one byte at a time: the reader must reassemble */
	pid_t pid = fork();
	if (pid == 0) {
		close(sv[0]);
		for (size_t k = 0; k < corpus[4].len; k++)
			if (fp_frame_write(sv[1], corpus[4].p + k, 1) !=
			    FP_P_OK)
				_exit(2);
		_exit(0);
	}
	close(sv[1]);
	fp_frame_hdr_t h;
	uint8_t* payload = NULL;
	int e = fp_frame_read(sv[0], &h, &payload);
	CHECK(e == FP_P_OK, "byte-at-a-time frame: %s", fp_proto_strerror(e));
	if (e == FP_P_OK) {
		fp_req_t r;
		CHECK(fp_req_decode(&h, payload, &r) == FP_P_OK,
		      "decodes after reassembly");
		fp_req_free(&r);
	}
	free(payload);
	/* the writer closes: a clean end between frames */
	CHECK(fp_frame_read(sv[0], &h, &payload) == FP_P_EOF,
	      "EOF between frames");
	int st;
	waitpid(pid, &st, 0);
	CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "writer exited cleanly");
	close(sv[0]);

	/* a close in the middle of a frame is truncation, not EOF */
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair 2");
	CHECK(fp_frame_write(sv[1], corpus[4].p, corpus[4].len - 3) == FP_P_OK,
	      "partial write");
	close(sv[1]);
	CHECK(fp_frame_read(sv[0], &h, &payload) == FP_P_ERR_TRUNCATED,
	      "close inside a frame");
	close(sv[0]);

	/* a close in the middle of a header */
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair 3");
	CHECK(fp_frame_write(sv[1], corpus[4].p, 5) == FP_P_OK,
	      "partial header");
	close(sv[1]);
	CHECK(fp_frame_read(sv[0], &h, &payload) == FP_P_ERR_TRUNCATED,
	      "close inside a header");
	close(sv[0]);

	/* garbage is refused at the header, before any payload is read */
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair 4");
	uint8_t junk[FP_PROTO_HDR_LEN];
	memset(junk, 0x41, sizeof(junk));
	CHECK(fp_frame_write(sv[1], junk, sizeof(junk)) == FP_P_OK,
	      "write junk");
	CHECK(fp_frame_read(sv[0], &h, &payload) == FP_P_ERR_MAGIC,
	      "junk header");
	CHECK(payload == NULL, "no payload allocated for junk");
	close(sv[0]);

	/* writing to a peer that is gone is an error, not SIGPIPE */
	close(sv[1]);
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "socketpair 5");
	close(sv[0]);
	CHECK(fp_frame_write(sv[1], junk, sizeof(junk)) == FP_P_ERR_IO,
	      "write to a closed peer");
	close(sv[1]);
}

/* a frame can carry descriptors; they arrive with it, only where asked for,
 * and are never leaked when nobody asked */
static void test_fds(void) {
	int sv[2], p1[2], p2[2], p3[2];
	fp_frame_hdr_t h;
	uint8_t* payload = NULL;
	int got[FP_MAX_FDS_TEST];
	size_t n = 0;
	fp_wbuf_t* stdio_frame = corpus_of(FP_FRAME_REQUEST, FP_REQ_STDIO);
	fp_req_t r;

	CHECK(stdio_frame != NULL, "the corpus has a STDIO frame");
	if (stdio_frame == NULL) return;
	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0, "fds socketpair");
	CHECK(pipe(p1) == 0 && pipe(p2) == 0 && pipe(p3) == 0, "fds pipes");
	int out[3] = {p1[0], p2[1], p3[1]};
	CHECK(fp_frame_write_fds(sv[1], stdio_frame->p, stdio_frame->len, out,
				 3) == FP_P_OK,
	      "write frame with fds");
	CHECK(fp_frame_write(sv[1], corpus[0].p, corpus[0].len) == FP_P_OK,
	      "write plain frame");
	CHECK(fp_frame_read_fds(sv[0], &h, &payload, got, FP_MAX_FDS_TEST,
				&n) == FP_P_OK,
	      "read frame with fds");
	CHECK(n == 3, "three descriptors arrive");
	CHECK(fp_req_decode(&h, payload, &r) == FP_P_OK &&
		  r.type == FP_REQ_STDIO && r.u.stdio.count == FP_STDIO_FDS,
	      "the frame decodes");
	fp_req_free(&r);
	free(payload);
	/* what arrives is the same open file: write on the received write end */
	if (n == 3) {
		char c = 0;
		CHECK(write(got[1], "x", 1) == 1,
		      "received descriptor is writable");
		CHECK(read(p2[0], &c, 1) == 1 && c == 'x',
		      "it is the sender's pipe");
		for (size_t i = 0; i < n; i++) close(got[i]);
	}
	/* the next frame has none, and the reader is told so */
	CHECK(fp_frame_read_fds(sv[0], &h, &payload, got, FP_MAX_FDS_TEST,
				&n) == FP_P_OK &&
		  n == 0,
	      "a plain frame carries no descriptors");
	free(payload);
	/* a reader that asks for fewer than were sent closes the rest */
	CHECK(fp_frame_write_fds(sv[1], stdio_frame->p, stdio_frame->len, out,
				 3) == FP_P_OK,
	      "write again");
	CHECK(fp_frame_read_fds(sv[0], &h, &payload, got, 1, &n) == FP_P_OK &&
		  n == 1,
	      "only as many as asked for are kept");
	free(payload);
	close(got[0]);
	close(p1[0]);
	close(p1[1]);
	close(p2[0]);
	close(p2[1]);
	close(p3[0]);
	close(p3[1]);
	close(sv[0]);
	close(sv[1]);
}

int main(void) {
	build_corpus();
	test_roundtrip();
	test_table_event();
	test_header_rejects();
	test_every_truncation();
	test_hostile_fields();
	test_encode_rules();
	test_fuzz();
	test_stream();
	test_fds();
	for (int i = 0; i < ncorpus; i++) fp_w_free(&corpus[i]);
	if (failures) {
		printf("FAILED: %d check(s)\n", failures);
		return 1;
	}
	printf("PASS: %d nvlist frames round-trip; truncations, trailing bytes,\n"
	       "      missing and mistyped fields, oversized strings and lists,\n"
	       "      duplicate names and mismatched row arrays are refused\n"
	       "      cleanly; a\n"
	       "      failed encode leaves the buffer alone; the stream helpers\n"
	       "      reassemble, report EOF vs truncation, and never raise\n"
	       "      SIGPIPE.\n",
	       ncorpus);
	return 0;
}
