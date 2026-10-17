// protocol-fuzz.c - libFuzzer entry point for the framed protocol decoder.
// Built by tests/protocol-fuzz.sh with -fsanitize=fuzzer,address; the decoder
// runs as root on bytes an unprivileged client chose.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/protocol.h"

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

#ifdef FPKG_FUZZ_SEEDS
static int write_frame(const char* dir, const char* name, fp_wbuf_t* w) {
	char path[1024];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE* f = fopen(path, "wb");
	if (!f) return 1;
	size_t n = fwrite(w->p, 1, w->len, f);
	fclose(f);
	return n == w->len ? 0 : 1;
}

// Emit a small valid corpus: one request of each shape, one ack, one event.
int main(int argc, char** argv) {
	if (argc != 2) return 2;

	fp_wbuf_t w;
	fp_w_init(&w);
	fp_req_t r;
	memset(&r, 0, sizeof(r));
	r.type = FP_REQ_HELLO;
	r.u.hello.version = FP_PROTO_VERSION;
	r.u.hello.client = (char*)"fuzz";
	if (fp_req_encode(&w, 1, &r) != FP_P_OK) return 1;
	if (write_frame(argv[1], "hello", &w)) return 1;
	fp_w_free(&w);

	fp_w_init(&w);
	memset(&r, 0, sizeof(r));
	r.type = FP_REQ_CANCEL;
	r.u.cancel.op_id = 1;
	if (fp_req_encode(&w, 2, &r) != FP_P_OK) return 1;
	if (write_frame(argv[1], "cancel", &w)) return 1;
	fp_w_free(&w);

	fp_w_init(&w);
	fp_ack_t a;
	memset(&a, 0, sizeof(a));
	a.type = FP_ACK_OK;
	a.op_id = 1;
	if (fp_ack_encode(&w, 3, &a) != FP_P_OK) return 1;
	if (write_frame(argv[1], "ack", &w)) return 1;
	fp_w_free(&w);

	return 0;
}
#else
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	decode_any(data, size);
	return 0;
}
#endif
