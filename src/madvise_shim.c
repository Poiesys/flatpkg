#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static const char* trace_env = "FLATPKG_MADVISE_SHIM_TRACE";

static int advisory_advice(int advice) {
	switch (advice) {
	case 8:
	case 10:
	case 16:
	case 18:
	case 22:
	case 23:
	case 100:
		return 1;
	default:
		return 0;
	}
}

static int (*real_madvise)(void*, size_t, int);

static int resolve(void) {
	if (real_madvise == NULL)
		real_madvise = (int (*)(void*, size_t, int))dlsym(RTLD_NEXT,
								  "madvise");
	return real_madvise != NULL;
}

int madvise(void* addr, size_t len, int advice) {
	if (resolve()) {
		int rc = real_madvise(addr, len, advice);
		if (rc == 0 || !advisory_advice(advice)) return rc;
		if (getenv(trace_env) != NULL)
			fprintf(stderr,
				"madvise-shim: advice %d -> ENOSYS, "
				"answered 0\n",
				advice);
	} else {
		if (!advisory_advice(advice)) {
			errno = ENOSYS;
			return -1;
		}
		if (getenv(trace_env) != NULL)
			fprintf(stderr,
				"madvise-shim: advice %d -> no real "
				"madvise, answered 0\n",
				advice);
	}
	return 0;
}
