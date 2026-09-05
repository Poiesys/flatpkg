#ifndef FPKG_TYPED_WORKER_H
#define FPKG_TYPED_WORKER_H

#include <sys/types.h>
#include "protocol.h"

void typed_worker(int conn, int bus_fd, uid_t peer_uid, gid_t peer_gid);
// Refuse the connection before HELLO (a caller outside the group).
void typed_refuse(int conn, const char* msg);

#endif