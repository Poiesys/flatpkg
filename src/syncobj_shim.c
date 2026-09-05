#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Linux errno values (this is a Linux ELF). ETIME is 62 on Linux.
#ifndef ETIME
#define ETIME 62
#endif

#define TRACE_ENV "FLATPKG_SYNCOBJ_SHIM_TRACE"

typedef int (*timeline_wait_fn)(int, uint32_t*, uint64_t*, unsigned, int64_t,
				unsigned, uint32_t*);
typedef int (*wait_fn)(int, uint32_t*, unsigned, int64_t, unsigned, uint32_t*);

static void* real_symbol(const char* name) {
	static void* handle;
	void* p = dlsym(RTLD_NEXT, name);

	if (p != NULL) return p;
	if (handle == NULL)
		handle = dlopen("libdrm.so.2", RTLD_NOW | RTLD_NOLOAD);
	if (handle == NULL)
		handle = dlopen("libdrm.so.2", RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL) return NULL;
	return dlsym(handle, name);
}

// libdrm returns -errno on failure; on timeout FreeBSD says ETIMEDOUT where
// Linux says ETIME.
static int normalise(int ret) {
	if (ret == -ETIMEDOUT) {
		if (getenv(TRACE_ENV) != NULL)
			fprintf(stderr, "syncobj-shim: ETIMEDOUT -> ETIME\n");
		errno = ETIME;
		return -ETIME;
	}
	return ret;
}

int drmSyncobjTimelineWait(int fd, uint32_t* handles, uint64_t* points,
			   unsigned num_handles, int64_t timeout_nsec,
			   unsigned flags, uint32_t* first_signaled) {
	static timeline_wait_fn real;
	timeline_wait_fn fn;

	if (real == NULL)
		real = (timeline_wait_fn)real_symbol("drmSyncobjTimelineWait");
	fn = real;
	if (fn == NULL) {
		errno = ENOSYS;
		return -1;
	}
	return normalise(fn(fd, handles, points, num_handles, timeout_nsec,
			    flags, first_signaled));
}

int drmSyncobjWait(int fd, uint32_t* handles, unsigned num_handles,
		   int64_t timeout_nsec, unsigned flags,
		   uint32_t* first_signaled) {
	static wait_fn real;
	wait_fn fn;

	if (real == NULL) real = (wait_fn)real_symbol("drmSyncobjWait");
	fn = real;
	if (fn == NULL) {
		errno = ENOSYS;
		return -1;
	}
	return normalise(fn(fd, handles, num_handles, timeout_nsec, flags,
			    first_signaled));
}
