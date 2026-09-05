#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>

// Linux TMPFS_MAGIC; memfd files live on an internal tmpfs there.
#ifndef TMPFS_MAGIC
#define TMPFS_MAGIC 0x01021994
#endif

#define FAKE_BLOCKS (1u << 22)

typedef int (*fstatfs_fn)(int, void*);

static const char* trace_env = "FLATPKG_MEMFD_SHIM_TRACE";

static fstatfs_fn real_fstatfs(void) {
	static fstatfs_fn fn;
	static int looked_up;

	if (!looked_up) {
		looked_up = 1;
		fn = (fstatfs_fn)dlsym(RTLD_NEXT, "fstatfs");
	}
	return fn;
}

static int synthesise(int fd, void* buf, size_t bufsize) {
	struct statfs tmp;
	struct stat st;

	// Stand in only for a valid descriptor: an invalid fd keeps its EBADF.
	if (fstat(fd, &st) != 0) return -1;

	memset(&tmp, 0, sizeof(tmp));
	tmp.f_type = TMPFS_MAGIC;
	tmp.f_bsize = 4096;
	tmp.f_frsize = 4096;
	tmp.f_blocks = FAKE_BLOCKS;
	tmp.f_bfree = FAKE_BLOCKS;
	tmp.f_bavail = FAKE_BLOCKS;
	tmp.f_files = FAKE_BLOCKS;
	tmp.f_ffree = FAKE_BLOCKS;
	tmp.f_namelen = 255;
	tmp.f_flags = 0;

	memcpy(buf, &tmp, bufsize < sizeof(tmp) ? bufsize : sizeof(tmp));

	if (getenv(trace_env) != NULL)
		fprintf(stderr, "memfd-shim: fstatfs(%d) -> tmpfs\n", fd);

	return 0;
}

static int interpose(int fd, void* buf, size_t bufsize) {
	fstatfs_fn fn = real_fstatfs();
	int saved;

	if (fn != NULL && fn(fd, buf) == 0) return 0;

	saved = errno;
	if (saved != EINVAL && saved != ENOSYS && saved != ENOTSUP) {
		errno = saved;
		return -1;
	}

	if (synthesise(fd, buf, bufsize) == 0) return 0;

	errno = saved;
	return -1;
}

int fstatfs(int fd, struct statfs* buf) {
	return interpose(fd, buf, sizeof(*buf));
}

int fstatfs64(int fd, struct statfs64* buf) {
	return interpose(fd, buf, sizeof(*buf));
}
