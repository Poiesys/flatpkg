#ifndef FPKG_TYPED_WORKER_H
#define FPKG_TYPED_WORKER_H

#include <sys/types.h>
#include "protocol.h"

void typed_worker(int conn, int bus_fd, uid_t peer_uid, gid_t peer_gid);
// Refuse the connection before HELLO (a caller outside the group).
void typed_refuse(int conn, const char* msg);

#endif
// The daemon side of the worker: a user installation runs the same binary as
// the caller, so its worker must keep the user store after clearenv() and
// must refuse the operations that need root.
bool daemon_is_user(void);
const char* daemon_store(void);
