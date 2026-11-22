#ifndef PORTAL_IDENTITY_H
#define PORTAL_IDENTITY_H

#include <sys/types.h>

#include <dbus/dbus.h>

// Who is calling the portal: the app id behind a bus name, and the instance
// records the Spawn and FileChooser paths read.  The portal is the caller's
// own process, so this is a lookup, not an authorization boundary.

void instance_base(char* out, size_t n);
char** snapshot_instances(void);
bool in_snapshot(char** snap, const char* id);
void free_snapshot(char** snap);
int read_instance_info(const char* path, char* app_out, size_t app_n, pid_t* pid_out);
char* resolve_app_id(DBusConnection* conn, const char* sender);

#endif
