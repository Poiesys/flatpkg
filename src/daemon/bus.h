#ifndef FPKG_BUS_H
#define FPKG_BUS_H

#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "protocol.h"

#define BUS_MAX_MSG 65536
#define BUS_MAX_PEERS 64
// One user's share: a group member holding every connection would starve the
// others, and an idle authenticated client is not a reason to serve it first.
#define BUS_MAX_PEERS_PER_UID 16
// A forwarding send that cannot be made now is retried for this many frames
// before the listener is dropped: a worker in the middle of an operation is
// not reading its bus for a while.
#define BUS_SLOW_GRACE 8
#define BUS_MAX_TOPICS 8
#define BUS_TOPIC_LEN 128
#define BUS_MAX_SNAP 4

enum {
	BUS_SUBSCRIBE = 1,
	BUS_UNSUBSCRIBE,
	BUS_PUBLISH,
	BUS_REGISTER,
	BUS_DONE,
	BUS_CANCEL,
	BUS_FORWARD,
	BUS_SUBSCRIBED,
	BUS_CANCELED,
	// Authorization answers: the topic is someone else's operation, or the
	// cancel names one this peer did not register.
	BUS_SUB_DENIED,
	BUS_CANCEL_DENIED,
};

typedef struct {
	uint32_t type;
	uint32_t len;
} bus_hdr_t;

int broker_add(int fd, pid_t pid, uid_t uid);
// How many connections the broker is serving: the daemon refuses a new one
// before it forks a worker for it.
int broker_peer_count(void);
int broker_peer_count_uid(uid_t uid);
void broker_handle(int fd);
int broker_pollfds(struct pollfd* out, int max);

int bus_worker_init(int fd);
int bus_worker_fd(void);
int bus_publish(const char* topic, const void* frame, size_t len);
int bus_register(uint64_t op_id);
int bus_done(uint64_t op_id);
int bus_subscribe(const char* topic);
int bus_unsubscribe(const char* topic);
int bus_cancel(uint64_t op_id);
// 0 = one message; `*type` says which.  BUS_FORWARD copies its frame into
// `buf`.  -1 = nothing now, -2 = the broker went away.
int bus_next(void* buf, size_t cap, size_t* len, uint32_t* type);
int bus_emit_event(const char* topic, const fp_event_t* ev);

#endif
