// ldconfig-trust.c - the predicate that decides whether a binary inside a
// launch tree may be executed as root.
//
// An imported runtime is the caller's own tree, so its usr/bin/ldconfig is
// whatever the caller put there; running it as root is root code execution.
// Only a regular file the store owns - root's, not writable by group or
// other - may be executed.
#include <sys/param.h>
#include <sys/stat.h>

#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

bool ldconfig_trusted(const char* path);

int main(int argc, char** argv) {
	int failures = 0;

	(void)argc;
	if (argc != 2) {
		fprintf(stderr, "usage: ldconfig-trust DIR\n");
		return 2;
	}
	// The guard lays out DIR/root, DIR/user, DIR/writable, DIR/dir and
	// DIR/link; this only asks the predicate.
	struct {
		const char* name;
		bool want;
		const char* why;
	} cases[] = {
		{"root", true, "a root-owned regular file is the store's"},
		{"user", false, "a caller-owned file is not the store's"},
		{"writable", false, "a root file anybody may write is not trusted"},
		{"dir", false, "a directory is not a program"},
		{"link", false, "a symlink is not judged by its name"},
		{"missing", false, "a missing file is not trusted"},
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char path[PATH_MAX];
		bool got;

		snprintf(path, sizeof(path), "%s/%s", argv[1], cases[i].name);
		got = ldconfig_trusted(path);
		if (got == cases[i].want) continue;
		fprintf(stderr, "FAIL: %s: %s\n", cases[i].name, cases[i].why);
		failures++;
	}
	if (failures == 0)
		printf("PASS: only a root-owned, non-writable regular file is "
		       "trusted for execution\n");
	return failures == 0 ? 0 : 1;
}
