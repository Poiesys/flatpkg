// The host kernel-module check, shared by both binaries: the client refuses
// before the daemon and the daemon refuses again.  It needs no privilege.
#include "flatpkg.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool flatpkg_kernel_module_loaded(const char* name) {
	char path[PATH_MAX];

	if (name == NULL || *name == '\0') return false;
	for (const char* p = name; *p != '\0'; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '_' || *p == '-'))
			return false;
	}

	snprintf(path, sizeof(path), "/sys/module/%s", name);
	if (access(path, F_OK) == 0) return true;

	int null = open("/dev/null", O_WRONLY);
	char* argv[] = {"/sbin/kldstat", "-q", "-n", (char*)name, NULL};
	bool loaded = run_program(argv, -1, -1, null) == 0;
	if (null >= 0) close(null);
	if (loaded) return true;

	snprintf(path, sizeof(path), "/boot/kernel/%s.ko", name);
	return access(path, F_OK) == 0;
}

const char* const flatpkg_host_modules[] = {"linux64",	"nullfs",  "linprocfs",
					    "linsysfs", "unionfs", "fdescfs",
					    NULL};

bool flatpkg_host_modules_message(char* out, size_t n) {
	char missing[192];
	size_t used = 0;

	if (out == NULL || n == 0) return false;
	missing[0] = '\0';
	for (const char* const* m = flatpkg_host_modules; *m != NULL; m++) {
		if (flatpkg_kernel_module_loaded(*m)) continue;
		int k = snprintf(missing + used, sizeof(missing) - used, "%s%s",
				 used > 0 ? "," : "", *m);
		if (k < 0 || (size_t)k >= sizeof(missing) - used) break;
		used += (size_t)k;
	}
	if (used == 0) return false;

	used = (size_t)snprintf(out, n, "this host is missing kernel modules:");
	for (const char* const* m = flatpkg_host_modules; *m != NULL; m++) {
		if (flatpkg_kernel_module_loaded(*m)) continue;
		if (used < n)
			used += (size_t)snprintf(out + used, n - used,
						 "\n    %s", *m);
	}
	if (used < n)
		snprintf(out + used, n - used,
			 "\nSet <name>_load=\"YES\" in /etc/rc.conf,"
			 "\nthen: service flatpkgd restart"
			 "\nOr now: kldload <name>");
	return true;
}
