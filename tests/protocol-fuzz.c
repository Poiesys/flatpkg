// protocol-fuzz.c - libFuzzer entry point for the frame decoder.  A frame is a
// packed nvlist; the decoder runs as root on bytes an unprivileged client
// chose.  Built by tests/protocol-fuzz.sh with -fsanitize=fuzzer,address.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/nv.h>

#include "../include/protocol.h"

static int decode_any(const uint8_t* buf, size_t len) {
	nvlist_t* nvl;
	uint32_t kind = 0;
	int e;

	nvl = nvlist_unpack(buf, len, 0);
	if (nvl == NULL) return FP_P_ERR_MALFORMED;
	if (nvlist_error(nvl) != 0 || !nvlist_exists_number(nvl, "kind")) {
		nvlist_destroy(nvl);
		return FP_P_ERR_MALFORMED;
	}
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
	nvlist_destroy(nvl);
	return e;
}

#ifdef FPKG_FUZZ_SEEDS
static int write_frame(const char* dir, const char* name, nvlist_t* nvl) {
	char path[1024];
	size_t len = 0;
	void* blob;
	FILE* f;

	if (nvl == NULL) return 1;
	blob = nvlist_pack(nvl, &len);
	nvlist_destroy(nvl);
	if (blob == NULL) return 1;
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "wb");
	if (f == NULL) {
		free(blob);
		return 1;
	}
	int rc = fwrite(blob, 1, len, f) == len ? 0 : 1;

	fclose(f);
	free(blob);
	return rc;
}

// Emit a small valid corpus: one request of each shape, one ack, one event.
int main(int argc, char** argv) {
	fp_req_t r;
	fp_ack_t a;
	fp_event_t ev;
	nvlist_t* nvl = NULL;

	if (argc != 2) return 2;

	memset(&r, 0, sizeof(r));
	r.type = FP_REQ_HELLO;
	r.u.hello.version = FP_PROTO_VERSION;
	r.u.hello.client = (char*)"fuzz";
	if (fp_req_encode(&nvl, 1, &r) != FP_P_OK) return 1;
	if (write_frame(argv[1], "hello", nvl)) return 1;

	memset(&r, 0, sizeof(r));
	r.type = FP_REQ_CANCEL;
	r.u.cancel.op_id = 1;
	if (fp_req_encode(&nvl, 2, &r) != FP_P_OK) return 1;
	if (write_frame(argv[1], "cancel", nvl)) return 1;

	memset(&a, 0, sizeof(a));
	a.type = FP_ACK_OK;
	a.op_id = 1;
	if (fp_ack_encode(&nvl, 3, &a) != FP_P_OK) return 1;
	if (write_frame(argv[1], "ack", nvl)) return 1;

	memset(&ev, 0, sizeof(ev));
	ev.type = FP_EV_OP_PROGRESS;
	ev.topic = "op/1";
	ev.op_id = 1;
	ev.u.progress.stage = "fetch";
	ev.u.progress.current = 1;
	ev.u.progress.total = 2;
	if (fp_event_encode(&nvl, 1, &ev) != FP_P_OK) return 1;
	if (write_frame(argv[1], "progress", nvl)) return 1;

	return 0;
}
#else
int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	decode_any(data, size);
	return 0;
}
#endif
