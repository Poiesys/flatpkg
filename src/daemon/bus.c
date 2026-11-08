#include "bus.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "protocol.h"

typedef struct {
	int fd;
	pid_t pid;
	uid_t uid;
	char topics[BUS_MAX_TOPICS][BUS_TOPIC_LEN];
	int ntopics;
} peer_t;

typedef struct {
	uint64_t op;
	int fd;
	pid_t pid;
} op_t;

typedef struct {
	char topic[BUS_TOPIC_LEN];
	char* frame[BUS_MAX_SNAP];
	size_t len[BUS_MAX_SNAP];
	int head;
	int n;
} snap_t;

static peer_t peers[BUS_MAX_PEERS];
static op_t ops[128];
static snap_t snaps[BUS_MAX_TOPICS * 8];

static int send_all(int fd, const void* buf, size_t n) {
	const char* p = buf;
	while (n > 0) {
		ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
		if (w < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

static int send_msg(int fd, uint32_t type, const void* a, size_t alen,
		    const void* b, size_t blen) {
	bus_hdr_t h;
	size_t n = sizeof(h) + alen + blen;
	char* buf;
	int rc;

	if (n > BUS_MAX_MSG || (buf = malloc(n)) == NULL) return -1;
	h.type = type;
	h.len = (uint32_t)(alen + blen);
	memcpy(buf, &h, sizeof(h));
	if (alen > 0) memcpy(buf + sizeof(h), a, alen);
	if (blen > 0) memcpy(buf + sizeof(h) + alen, b, blen);
	rc = send_all(fd, buf, n);
	free(buf);
	return rc;
}

static snap_t* snap_find(const char* topic) {
	for (size_t i = 0; i < sizeof(snaps) / sizeof(snaps[0]); i++)
		if (snaps[i].n > 0 && strcmp(snaps[i].topic, topic) == 0)
			return &snaps[i];
	return NULL;
}

static snap_t* snap_new(const char* topic) {
	for (size_t i = 0; i < sizeof(snaps) / sizeof(snaps[0]); i++)
		if (snaps[i].n == 0) {
			snprintf(snaps[i].topic, sizeof(snaps[i].topic), "%s",
				 topic);
			return &snaps[i];
		}
	for (size_t i = 0; i < sizeof(snaps) / sizeof(snaps[0]); i++) {
		for (int k = 0; k < snaps[i].n; k++)
			free(snaps[i].frame[k]);
		memset(&snaps[i], 0, sizeof(snaps[i]));
	}
	snprintf(snaps[0].topic, sizeof(snaps[0].topic), "%s", topic);
	return &snaps[0];
}

static void snap_add(const char* topic, const void* frame, size_t len) {
	snap_t* s = snap_find(topic);
	int slot;

	if (len > BUS_MAX_MSG) return;
	if (s == NULL) s = snap_new(topic);
	slot = (s->head + s->n) % BUS_MAX_SNAP;
	if (s->n == BUS_MAX_SNAP) {
		slot = s->head;
		s->head = (s->head + 1) % BUS_MAX_SNAP;
		free(s->frame[slot]);
	} else {
		s->n++;
	}
	s->frame[slot] = malloc(len);
	if (s->frame[slot] == NULL) {
		s->n--;
		return;
	}
	memcpy(s->frame[slot], frame, len);
	s->len[slot] = len;
}

static void snap_send(int fd, const char* topic) {
	snap_t* s = snap_find(topic);

	if (s == NULL) return;
	for (int i = 0; i < s->n; i++) {
		int k = (s->head + i) % BUS_MAX_SNAP;
		if (send_msg(fd, BUS_FORWARD, s->frame[k], s->len[k], NULL,
			     0) != 0)
			return;
	}
}

static peer_t* peer_for(int fd) {
	for (size_t i = 0; i < sizeof(peers) / sizeof(peers[0]); i++)
		if (peers[i].fd == fd) return &peers[i];
	return NULL;
}

static int peer_subscribed(const peer_t* p, const char* topic) {
	for (int i = 0; i < p->ntopics; i++)
		if (p->topics[i][0] != '\0' &&
		    strcmp(p->topics[i], topic) == 0)
			return 1;
	return 0;
}

static void op_forget(int fd) {
	for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
		if (ops[i].fd == fd) memset(&ops[i], 0, sizeof(ops[i]));
}

static void peer_remove(int fd) {
	peer_t* p = peer_for(fd);

	if (p == NULL) return;
	op_forget(fd);
	close(fd);
	memset(p, 0, sizeof(*p));
}

int broker_add(int fd, pid_t pid, uid_t uid) {
	for (size_t i = 0; i < sizeof(peers) / sizeof(peers[0]); i++)
		if (peers[i].fd == 0) {
			peers[i].fd = fd;
			peers[i].pid = pid;
			peers[i].uid = uid;
			return 0;
		}
	return -1;
}

int broker_pollfds(struct pollfd* out, int max) {
	int n = 0;
	for (size_t i = 0; i < sizeof(peers) / sizeof(peers[0]) && n < max; i++)
		if (peers[i].fd != 0) {
			out[n].fd = peers[i].fd;
			out[n].events = POLLIN;
			out[n].revents = 0;
			n++;
		}
	return n;
}

static void route(const char* topic, const void* frame, size_t len,
		  int from_fd) {
	int drop[BUS_MAX_PEERS];
	int ndrop = 0;

	for (size_t i = 0; i < sizeof(peers) / sizeof(peers[0]); i++) {
		peer_t* p = &peers[i];
		if (p->fd == 0 || p->fd == from_fd) continue;
		if (!peer_subscribed(p, topic)) continue;
		if (send_msg(p->fd, BUS_FORWARD, frame, len, NULL, 0) != 0)
			drop[ndrop++] = p->fd;
	}
	for (int i = 0; i < ndrop; i++) peer_remove(drop[i]);
}

void broker_handle(int fd) {
	char buf[BUS_MAX_MSG];
	bus_hdr_t h;
	ssize_t n;
	peer_t* p = peer_for(fd);

	if (p == NULL) return;
	n = recv(fd, buf, sizeof(buf), 0);
	if (n <= 0) {
		peer_remove(fd);
		return;
	}
	if ((size_t)n < sizeof(h)) return;
	memcpy(&h, buf, sizeof(h));
	if (h.len != (uint32_t)n - sizeof(h) || h.len > BUS_MAX_MSG)
		return;
	const char* payload = buf + sizeof(h);

	switch (h.type) {
	case BUS_SUBSCRIBE: {
		char topic[BUS_TOPIC_LEN];
		if (h.len == 0 || h.len >= BUS_TOPIC_LEN) return;
		memcpy(topic, payload, h.len);
		topic[h.len] = '\0';
		if (peer_subscribed(p, topic)) return;
		if (p->ntopics >= BUS_MAX_TOPICS) return;
		snprintf(p->topics[p->ntopics++], BUS_TOPIC_LEN, "%s", topic);
		// The ack first: the worker answers the client before the
		// snapshot frames, which the client reads as events.
		(void)send_msg(fd, BUS_SUBSCRIBED, NULL, 0, NULL, 0);
		snap_send(fd, topic);
		return;
	}
	case BUS_UNSUBSCRIBE: {
		char topic[BUS_TOPIC_LEN];
		if (h.len == 0 || h.len >= BUS_TOPIC_LEN) return;
		memcpy(topic, payload, h.len);
		topic[h.len] = '\0';
		for (int i = 0; i < p->ntopics; i++)
			if (strcmp(p->topics[i], topic) == 0)
				p->topics[i][0] = '\0';
		return;
	}
	case BUS_PUBLISH: {
		uint32_t tlen;
		char topic[BUS_TOPIC_LEN];

		if (h.len < 4) return;
		memcpy(&tlen, payload, 4);
		if (tlen == 0 || tlen >= BUS_TOPIC_LEN || 4 + tlen > h.len)
			return;
		memcpy(topic, payload + 4, tlen);
		topic[tlen] = '\0';
		const char* frame = payload + 4 + tlen;
		size_t flen = h.len - 4 - tlen;
		// `op` is a live stream, not one operation: replaying whatever ran before would
		// tell a new watcher nothing true about the operation it waits for.
		if (strcmp(topic, "op") != 0) snap_add(topic, frame, flen);
		route(topic, frame, flen, fd);
		return;
	}
	case BUS_REGISTER: {
		uint64_t op;
		if (h.len != sizeof(op)) return;
		memcpy(&op, payload, sizeof(op));
		for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
			if (ops[i].op == 0) {
				ops[i].op = op;
				ops[i].fd = fd;
				ops[i].pid = p->pid;
				return;
			}
		return;
	}
	case BUS_DONE: {
		uint64_t op;
		if (h.len != sizeof(op)) return;
		memcpy(&op, payload, sizeof(op));
		for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
			if (ops[i].op == op && ops[i].fd == fd)
				memset(&ops[i], 0, sizeof(ops[i]));
		return;
	}
	case BUS_CANCEL: {
		uint64_t op;
		if (h.len != sizeof(op)) return;
		memcpy(&op, payload, sizeof(op));
		for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
			if (ops[i].op == op && ops[i].pid > 0) {
				(void)kill(ops[i].pid, SIGUSR1);
				break;
			}
		(void)send_msg(fd, BUS_CANCELED, NULL, 0, NULL, 0);
		return;
	}
	default:
		return;
	}
}

static int worker_fd = -1;

int bus_worker_init(int fd) {
	worker_fd = fd;
	// A tool the worker starts must not hold the bus open after it exits.
	(void)fcntl(fd, F_SETFD, FD_CLOEXEC);
	return 0;
}

int bus_worker_fd(void) { return worker_fd; }

int bus_publish(const char* topic, const void* frame, size_t len) {
	uint32_t tlen;
	char* buf;
	int rc;
	size_t n;

	if (worker_fd < 0 || topic == NULL || *topic == '\0') return -1;
	tlen = (uint32_t)strlen(topic);
	if (tlen >= BUS_TOPIC_LEN || 4 + tlen + len > BUS_MAX_MSG) return -1;
	n = 4 + tlen + len;
	buf = malloc(n);
	if (buf == NULL) return -1;
	memcpy(buf, &tlen, 4);
	memcpy(buf + 4, topic, tlen);
	memcpy(buf + 4 + tlen, frame, len);
	rc = send_msg(worker_fd, BUS_PUBLISH, buf, n, NULL, 0);
	free(buf);
	return rc;
}

static int send_op(uint32_t type, uint64_t op) {
	return send_msg(worker_fd, type, &op, sizeof(op), NULL, 0);
}

int bus_register(uint64_t op_id) { return send_op(BUS_REGISTER, op_id); }
int bus_done(uint64_t op_id) { return send_op(BUS_DONE, op_id); }
int bus_cancel(uint64_t op_id) { return send_op(BUS_CANCEL, op_id); }

int bus_subscribe(const char* topic) {
	if (worker_fd < 0 || topic == NULL) return -1;
	return send_msg(worker_fd, BUS_SUBSCRIBE, topic, strlen(topic), NULL,
			0);
}

int bus_unsubscribe(const char* topic) {
	if (worker_fd < 0 || topic == NULL) return -1;
	return send_msg(worker_fd, BUS_UNSUBSCRIBE, topic, strlen(topic), NULL,
			0);
}

int bus_next(void* buf, size_t cap, size_t* len, uint32_t* type) {
	char* msg;
	bus_hdr_t h;
	ssize_t n;
	int rc = -1;

	*len = 0;
	*type = 0;
	if (worker_fd < 0) return -1;
	msg = malloc(BUS_MAX_MSG);
	if (msg == NULL) return -1;
	n = recv(worker_fd, msg, BUS_MAX_MSG, MSG_DONTWAIT);
	if (n < 0) {
		rc = (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
		goto out;
	}
	if (n == 0) {
		rc = -2;
		goto out;
	}
	if ((size_t)n < sizeof(h)) goto out;
	memcpy(&h, msg, sizeof(h));
	if (h.len > (uint32_t)n - sizeof(h)) goto out;
	*type = h.type;
	if (h.type == BUS_FORWARD) {
		if (h.len > cap) goto out;
		memcpy(buf, msg + sizeof(h), h.len);
		*len = h.len;
	}
	rc = 0;
out:
	free(msg);
	return rc;
}

int bus_emit_event(const char* topic, const fp_event_t* ev) {
	static uint32_t seq;
	fp_event_t e;
	fp_wbuf_t w;
	int rc;

	if (worker_fd < 0 || topic == NULL || !fp_topic_valid(topic))
		return -1;
	e = *ev;
	e.topic = (char*)topic;
	e.op_id = 0;
	fp_w_init(&w);
	rc = fp_event_encode(&w, seq++, &e);
	if (rc == FP_P_OK) rc = bus_publish(topic, w.p, w.len);
	fp_w_free(&w);
	return rc == FP_P_OK ? 0 : -1;
}
