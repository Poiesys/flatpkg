#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

// Linux's RLIM_NLIMITS is 16; only 13..15 are synthesised.
#define NRES 16

static struct rlimit synth[NRES];
static int synth_ready;

static const char* trace_env = "FLATPKG_RLIMIT_SHIM_TRACE";

static void synth_init(void) {
	if (synth_ready) return;
	synth_ready = 1;
	synth[RLIMIT_NICE].rlim_cur = 0;
	synth[RLIMIT_NICE].rlim_max = 0;
	synth[RLIMIT_RTPRIO].rlim_cur = 0;
	synth[RLIMIT_RTPRIO].rlim_max = 0;
	synth[RLIMIT_RTTIME].rlim_cur = RLIM_INFINITY;
	synth[RLIMIT_RTTIME].rlim_max = RLIM_INFINITY;
}

static int is_synth(__rlimit_resource_t resource) {
	return resource == RLIMIT_NICE || resource == RLIMIT_RTPRIO ||
	       resource == RLIMIT_RTTIME;
}

static void trace(const char* what, int resource, unsigned long long cur,
		  unsigned long long max) {
	if (getenv(trace_env) != NULL)
		fprintf(stderr, "rlimit-shim: %s(%d) -> %llu/%llu\n", what,
			resource, cur, max);
}

int getrlimit(__rlimit_resource_t resource, struct rlimit* rlim) {
	static int (*real)(__rlimit_resource_t, struct rlimit*);

	if (is_synth(resource)) {
		synth_init();
		*rlim = synth[resource];
		trace("getrlimit", resource,
		      (unsigned long long)synth[resource].rlim_cur,
		      (unsigned long long)synth[resource].rlim_max);
		return 0;
	}
	if (real == NULL)
		real = (int (*)(__rlimit_resource_t, struct rlimit*))dlsym(
			RTLD_NEXT, "getrlimit");
	if (real == NULL) {
		errno = ENOSYS;
		return -1;
	}
	return real(resource, rlim);
}

int setrlimit(__rlimit_resource_t resource, const struct rlimit* rlim) {
	static int (*real)(__rlimit_resource_t, const struct rlimit*);

	if (is_synth(resource)) {
		synth_init();
		// Record rather than reject: the host cannot enforce a Linux RT
		// priority through these limits, and refusing would fail probes.
		synth[resource] = *rlim;
		trace("setrlimit", resource,
		      (unsigned long long)synth[resource].rlim_cur,
		      (unsigned long long)synth[resource].rlim_max);
		return 0;
	}
	if (real == NULL)
		real = (int (*)(__rlimit_resource_t,
				const struct rlimit*))dlsym(RTLD_NEXT,
							    "setrlimit");
	if (real == NULL) {
		errno = ENOSYS;
		return -1;
	}
	return real(resource, rlim);
}

static int is_self(__pid_t pid) { return pid == 0 || pid == getpid(); }

int prlimit(__pid_t pid, __rlimit_resource_t resource,
	    const struct rlimit* newlim, struct rlimit* oldlim) {
	static int (*real)(__pid_t, __rlimit_resource_t, const struct rlimit*,
			   struct rlimit*);

	if (is_synth(resource) && is_self(pid)) {
		synth_init();
		if (oldlim != NULL) *oldlim = synth[resource];
		if (newlim != NULL) synth[resource] = *newlim;
		trace("prlimit", resource,
		      (unsigned long long)synth[resource].rlim_cur,
		      (unsigned long long)synth[resource].rlim_max);
		return 0;
	}
	if (real == NULL)
		real = (int (*)(__pid_t, __rlimit_resource_t,
				const struct rlimit*,
				struct rlimit*))dlsym(RTLD_NEXT, "prlimit");
	if (real == NULL) {
		errno = ENOSYS;
		return -1;
	}
	return real(pid, resource, newlim, oldlim);
}

int prlimit64(__pid_t pid, __rlimit_resource_t resource,
	      const struct rlimit64* newlim, struct rlimit64* oldlim) {
	static int (*real)(__pid_t, __rlimit_resource_t, const struct rlimit64*,
			   struct rlimit64*);

	if (is_synth(resource) && is_self(pid)) {
		synth_init();
		if (oldlim != NULL) {
			oldlim->rlim_cur = synth[resource].rlim_cur;
			oldlim->rlim_max = synth[resource].rlim_max;
		}
		if (newlim != NULL) {
			synth[resource].rlim_cur = newlim->rlim_cur;
			synth[resource].rlim_max = newlim->rlim_max;
		}
		trace("prlimit64", resource,
		      (unsigned long long)synth[resource].rlim_cur,
		      (unsigned long long)synth[resource].rlim_max);
		return 0;
	}
	if (real == NULL)
		real = (int (*)(__pid_t, __rlimit_resource_t,
				const struct rlimit64*,
				struct rlimit64*))dlsym(RTLD_NEXT, "prlimit64");
	if (real == NULL) {
		errno = ENOSYS;
		return -1;
	}
	return real(pid, resource, newlim, oldlim);
}
