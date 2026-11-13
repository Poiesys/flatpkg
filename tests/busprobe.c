// busprobe.c - a second client for the event-bus guard: it speaks the framed
// protocol to follow another connection's operation and to cancel it, plus an
// install to produce one worth following.  Built by tests/typed-daemon.sh.
#include "protocol.h"

#include <sys/socket.h>
#include <sys/un.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int connect_socket(const char* path) {
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un sa;

	if (fd < 0) return -1;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(sa.sun_path) ||
	    strlcpy(sa.sun_path, path, sizeof(sa.sun_path)) >=
		    sizeof(sa.sun_path)) {
		close(fd);
		return -1;
	}
	if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int send_req(int fd, uint32_t id, const fp_req_t* r) {
	nvlist_t* nvl = NULL;
	int e;

	if (fp_req_encode(&nvl, id, r) != FP_P_OK) return -1;
	e = fp_frame_send(fd, nvl);
	nvlist_destroy(nvl);
	return e == FP_P_OK ? 0 : -1;
}

static int read_ack(int fd, uint64_t* op_id) {
	nvlist_t* nvl = NULL;
	fp_ack_t ack;
	int e;

	if (fp_frame_recv(fd, &nvl) != FP_P_OK) return -1;
	e = fp_ack_decode(nvl, NULL, &ack);
	nvlist_destroy(nvl);
	if (e != FP_P_OK) return -1;
	if (ack.type == FP_ACK_ERR) {
		printf("ack=error: %s\n", ack.message ? ack.message : "?");
		fp_ack_free(&ack);
		return -1;
	}
	if (op_id != NULL) *op_id = ack.op_id;
	fp_ack_free(&ack);
	return 0;
}

static int hello(int fd) {
	fp_req_t r;

	memset(&r, 0, sizeof(r));
	r.type = FP_REQ_HELLO;
	r.u.hello.version = FP_PROTO_VERSION;
	r.u.hello.client = "busprobe";
	if (send_req(fd, 1, &r) != 0 || read_ack(fd, NULL) != 0) return -1;
	memset(&r, 0, sizeof(r));
	r.type = FP_REQ_ENV;
	r.u.env.vars = NULL;
	r.u.env.nvars = 0;
	return (send_req(fd, 2, &r) == 0 && read_ack(fd, NULL) == 0) ? 0 : -1;
}

static int read_events(int fd, int timeout_ms, int require_finished) {
	int finished = 0;
	int seen = 0;
	int64_t deadline = 0; // 0: first call, use timeout_ms

	(void)deadline;
	for (;;) {
		struct pollfd p = {fd, POLLIN, 0};
		int pr = poll(&p, 1, timeout_ms);
		if (pr <= 0) return (finished || seen) ? 0 : -1;
		nvlist_t* nvl = NULL;
		fp_event_t ev;
		int e;

		if (fp_frame_recv(fd, &nvl) != FP_P_OK) return -1;
		e = fp_event_decode(nvl, NULL, &ev);
		nvlist_destroy(nvl);
		if (e != FP_P_OK) return -1;
		switch (ev.type) {
		case FP_EV_OP_STARTED:
			printf("event=started op=%llu\n",
			       (unsigned long long)ev.op_id);
			break;
		case FP_EV_OP_PROGRESS:
			printf("event=progress\n");
			break;
		case FP_EV_OP_TABLE:
			printf("event=table\n");
			break;
		case FP_EV_OP_LOG:
			printf("event=log\n");
			break;
		case FP_EV_META:
			printf("event=meta ref=%s commit=%.12s\n",
			       ev.u.meta.ref ? ev.u.meta.ref : "?",
			       ev.u.meta.commit ? ev.u.meta.commit : "");
			break;
		case FP_EV_STORE_CHANGED:
			printf("event=store action=%s ref=%s\n",
			       ev.u.store.action ? ev.u.store.action : "?",
			       ev.u.store.ref ? ev.u.store.ref : "?");
			break;
		case FP_EV_REMOTE_CHANGED:
			printf("event=remote name=%s\n",
			       ev.u.remote.name ? ev.u.remote.name : "?");
			break;
		case FP_EV_INSTANCE_STARTED:
			printf("event=instance-started app=%s\n",
			       ev.u.instance.app_id ? ev.u.instance.app_id : "?");
			break;
		case FP_EV_CLIENT_STATE:
			printf("event=client-state name=%s data=%s\n",
			       ev.u.client_state.name ? ev.u.client_state.name
						      : "?",
			       ev.u.client_state.data ? ev.u.client_state.data
						      : "?");
			break;
		case FP_EV_INSTANCE_EXITED:
			printf("event=instance-exited app=%s\n",
			       ev.u.instance.app_id ? ev.u.instance.app_id : "?");
			break;
		case FP_EV_OP_FINISHED:
			printf("event=finished status=%u\n",
			       ev.u.finished.status);
			finished = 1;
			break;
		default:
			break;
		}
		fp_event_free(&ev);
		seen = 1;
		if (finished) return 0;
		if (!require_finished) {
			timeout_ms = 15000;
			continue;
		}
		timeout_ms = 30000;
	}
}

int main(int argc, char** argv) {
	if (argc < 3) return 2;
	const char* mode = argv[1];
	int fd = connect_socket(argv[2]);

	if (fd < 0) {
		perror("busprobe: connect");
		return 1;
	}
	// Before HELLO, on purpose: the fd-carrying requests are dispatched
	// ahead of the gate in handle(), and a RUN without a HELLO must still
	// be refused.
	if (strcmp(mode, "run-nohello") == 0) {
		fp_req_t nr;

		if (argc < 4) return 2;
		memset(&nr, 0, sizeof(nr));
		nr.type = FP_REQ_RUN;
		nr.u.run.app_id = argv[3];
		if (send_req(fd, 3, &nr) != 0) return 1;
		if (read_ack(fd, NULL) == 0) {
			printf("gate=accepted\n");
			return 1;
		}
		printf("gate=denied\n");
		return 0;
	}

	if (hello(fd) != 0) return 1;

	fp_req_t r;
	memset(&r, 0, sizeof(r));
	if (strcmp(mode, "prefetch") == 0) {
		if (argc < 4) return 2;
		r.type = FP_REQ_PREFETCH;
		r.u.prefetch.remote = "";
		r.u.prefetch.refs = (char**)&argv[3];
		r.u.prefetch.nrefs = (size_t)(argc - 3);
		if (send_req(fd, 3, &r) != 0) return 1;
		uint64_t op = 0;
		if (read_ack(fd, &op) != 0) return 1;
		return read_events(fd, 30000, 1) == 0 ? 0 : 1;
	}
	if ((strcmp(mode, "state-write") == 0 ||
	     strcmp(mode, "state-read") == 0) && argc < 4)
		return 2;
	if (strcmp(mode, "state-write") == 0 ||
	    strcmp(mode, "state-read") == 0) {
		fp_req_t d;
		char* names[1] = {argv[3]};
		memset(&d, 0, sizeof(d));
		d.type = FP_REQ_CLIENT_DECLARE;
		d.u.client_declare.names = names;
		d.u.client_declare.nnames = 1;
		if (send_req(fd, 9, &d) != 0 ||
		    read_ack(fd, NULL) != 0)
			return 1;
	}
	if (strcmp(mode, "state-write") == 0) {
		if (argc < 5) return 2;
		r.type = FP_REQ_CLIENT_STATE;
		r.u.client_state.flags = FP_CLIENT_STATE_WRITE;
		r.u.client_state.name = argv[3];
		r.u.client_state.data = argv[4];
		if (send_req(fd, 3, &r) != 0 || read_ack(fd, NULL) != 0) return 1;
		printf("state=ok\n");
		return 0;
	}
	if (strcmp(mode, "state-undeclared") == 0) {
		if (argc < 5) return 2;
		r.type = FP_REQ_CLIENT_STATE;
		r.u.client_state.flags = FP_CLIENT_STATE_WRITE;
		r.u.client_state.name = argv[3];
		r.u.client_state.data = argv[4];
		if (send_req(fd, 3, &r) != 0) return 1;
		if (read_ack(fd, NULL) == 0) {
			printf("state=accepted\n");
			return 1;
		}
		printf("state=denied\n");
		return 0;
	}
	if (strcmp(mode, "state-read") == 0) {
		if (argc < 4) return 2;
		r.type = FP_REQ_CLIENT_STATE;
		r.u.client_state.flags = 0;
		r.u.client_state.name = argv[3];
		r.u.client_state.data = "";
		if (send_req(fd, 3, &r) != 0) return 1;
		uint64_t op = 0;
		if (read_ack(fd, &op) != 0) return 1;
		return read_events(fd, 30000, 1) == 0 ? 0 : 1;
	}
	if (strcmp(mode, "apply-noop") == 0) {
		if (argc < 4) return 2;
		fp_change_row_t row = {FP_APPLY_NOOP, argv[3], NULL, NULL};
		r.type = FP_REQ_APPLY;
		r.u.apply.flags = 0;
		r.u.apply.remote = "";
		r.u.apply.rows = &row;
		r.u.apply.nrows = 1;
		if (send_req(fd, 3, &r) != 0) return 1;
		uint64_t op = 0;
		if (read_ack(fd, &op) != 0) return 1;
		return read_events(fd, 30000, 1) == 0 ? 0 : 1;
	}
	if (strcmp(mode, "watch-topic-any") == 0) {
		if (argc < 4) return 2;
		r.type = FP_REQ_SUBSCRIBE;
		r.u.sub.topic = argv[3];
		if (send_req(fd, 3, &r) != 0 || read_ack(fd, NULL) != 0) return 1;
		printf("sub=ok\n");
		fflush(stdout);
		return read_events(fd, 10000, 0) == 0 ? 0 : 1;
	}
	if (strcmp(mode, "watch-topic") == 0) {
		if (argc < 4) return 2;
		r.type = FP_REQ_SUBSCRIBE;
		r.u.sub.topic = argv[3];
		if (send_req(fd, 3, &r) != 0 || read_ack(fd, NULL) != 0) return 1;
		printf("sub=ok\n");
		fflush(stdout);
		return read_events(fd, 60000, 1) == 0 ? 0 : 1;
	}
	if (strcmp(mode, "subscribe") == 0) {
		if (argc < 4) return 2;
		r.type = FP_REQ_SUBSCRIBE;
		r.u.sub.topic = argv[3];
		if (send_req(fd, 3, &r) != 0 || read_ack(fd, NULL) != 0) return 1;
		printf("sub=ok\n");
		return 0;
	}
	if (strcmp(mode, "cancel") == 0) {
		if (argc < 4) return 2;
		r.type = FP_REQ_CANCEL;
		r.u.cancel.op_id = strtoull(argv[3], NULL, 10);
		if (send_req(fd, 3, &r) != 0 || read_ack(fd, NULL) != 0) return 1;
		printf("ack=ok\n");
		return 0;
	}
	return 2;
}
