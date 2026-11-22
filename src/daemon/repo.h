#ifndef FPKG_REPO_H
#define FPKG_REPO_H

#include <stddef.h>

// The daemon's libostree boundary: the only header a caller includes without
// GLib or ostree types.  Implemented in repo.c, the only TU that includes
// <ostree.h>; linked into flatpkgd only.
int fp_repo_verify_commit(const char* checksum, const char* remote,
			  char** out_error);
int fp_repo_local_ref_info(const char* store, const char* remote,
			   const char* ref, const char* commit, bool runtime,
			   char out_ref[512], char out_commit[65],
			   char** out_metadata, char** out_error);
// The libostree version the daemon was linked against, for the startup line
// that makes the daemon-only link observable.
const char* fp_repo_version(void);

#endif
