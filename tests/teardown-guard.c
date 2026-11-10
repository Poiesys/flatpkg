// teardown-guard.c - unit harness for sandbox_remove_jail_root().
//
// Built together with src/daemon/sandbox.c compiled with -DFLATPKG_TESTING, so
// the mount table and unmount are the fakes below.  That is the only way to
// exercise "a mount will not come down": on this platform MNT_FORCE takes
// down a busy tmpfs, nullfs and unionfs, so a real fixture cannot arrange it.
#include <sys/param.h>
#include <sys/mount.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include "flatpkg.h"

int sandbox_remove_jail_root(const char* root);
bool root_is_held(const char* dir);

static char g_root[PATH_MAX];
static bool g_refuse;

int fpkg_test_getmntinfo(struct statfs** out, int flags) {
	static struct statfs mnts[1];

	(void)flags;
	memset(mnts, 0, sizeof(mnts));
	*out = mnts;
	if (!g_refuse) return 0;
	snprintf(mnts[0].f_mntonname, sizeof(mnts[0].f_mntonname), "%s/mnt",
		 g_root);
	return 1;
}

int fpkg_test_unmount(const char* dir, int flags) {
	(void)dir;
	(void)flags;
	errno = EBUSY;
	return -1;
}

int main(void) {
	char tmpl[PATH_MAX];
	int rc = 0;

	snprintf(tmpl, sizeof(tmpl), "/var/run/flatpkg/tg.XXXXXX");
	if (mkdtemp(tmpl) == NULL) {
		perror("mkdtemp");
		return 2;
	}
	snprintf(g_root, sizeof(g_root), "%s", tmpl);

	// A mount that refuses to unmount: the root and everything in it stay.
	g_refuse = true;
	if (sandbox_remove_jail_root(g_root) == 0) {
		fprintf(stderr, "FAIL: removed a root with a mount still under it\n");
		rc = 1;
	}
	if (access(g_root, F_OK) != 0) {
		fprintf(stderr, "FAIL: the root was removed anyway\n");
		rc = 1;
	}

	// Once the mount is gone, the guard lets the removal through.
	g_refuse = false;
	if (sandbox_remove_jail_root(g_root) != 0) {
		fprintf(stderr, "FAIL: did not remove the root once unmounted\n");
		rc = 1;
	}
	if (access(g_root, F_OK) == 0) {
		fprintf(stderr, "FAIL: the root survived removal\n");
		rc = 1;
	}

	// One component under the run dir: a deeper path is refused even though
	// it exists and sits below /var/run/flatpkg.
	{
		char sub[PATH_MAX];

		snprintf(tmpl, sizeof(tmpl), "/var/run/flatpkg/tg.XXXXXX");
		if (mkdtemp(tmpl) == NULL) {
			perror("mkdtemp");
			return 2;
		}
		snprintf(sub, sizeof(sub), "%s/sub", tmpl);
		if (mkdir(sub, 0755) != 0) {
			perror("mkdir");
			return 2;
		}
		snprintf(g_root, sizeof(g_root), "%s", tmpl);
		if (sandbox_remove_jail_root(sub) == 0) {
			fprintf(stderr, "FAIL: removed a path two components "
					"under the run dir\n");
			rc = 1;
		}
		if (access(sub, F_OK) != 0) {
			fprintf(stderr, "FAIL: the subdirectory was removed\n");
			rc = 1;
		}
		if (sandbox_remove_jail_root(tmpl) != 0) {
			fprintf(stderr, "FAIL: could not clean up the subdir "
					"case\n");
			rc = 1;
		}
	}
	// The lock probe judges the file it opened.  A lock that is not a regular
	// file - a dangling symlink, a fifo, a directory - is one the sweep cannot
	// read, so its root counts as held and is left alone; a symlink followed by
	// the open would read as free and lose the root.
	{
		char lock[PATH_MAX];
		int fd;

		snprintf(tmpl, sizeof(tmpl), "/var/run/flatpkg/tg.XXXXXX");
		if (mkdtemp(tmpl) == NULL) {
			perror("mkdtemp");
			return 2;
		}
		snprintf(g_root, sizeof(g_root), "%s", tmpl);
		snprintf(lock, sizeof(lock), "%s/.lock", tmpl);

		if (root_is_held(g_root)) {
			fprintf(stderr, "FAIL: a root with no lock counts as held\n");
			rc = 1;
		}
		fd = open(lock, O_CREAT | O_RDWR, 0600);
		if (fd < 0) {
			perror("open");
			return 2;
		}
		close(fd);
		if (root_is_held(g_root)) {
			fprintf(stderr, "FAIL: an unlocked lock counts as held\n");
			rc = 1;
		}
		fd = open(lock, O_RDWR);
		if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
			perror("flock");
			return 2;
		}
		if (!root_is_held(g_root)) {
			fprintf(stderr, "FAIL: a lock another process holds "
					"counts as free\n");
			rc = 1;
		}
		close(fd);

		unlink(lock);
		if (symlink("/nonexistent-fpkg", lock) != 0) {
			perror("symlink");
			return 2;
		}
		if (!root_is_held(g_root)) {
			fprintf(stderr, "FAIL: a symlinked lock counts as free\n");
			rc = 1;
		}
		unlink(lock);
		if (mkfifo(lock, 0600) != 0) {
			perror("mkfifo");
			return 2;
		}
		if (!root_is_held(g_root)) {
			fprintf(stderr, "FAIL: a fifo lock counts as free\n");
			rc = 1;
		}
		unlink(lock);
		if (mkdir(lock, 0755) != 0) {
			perror("mkdir");
			return 2;
		}
		if (!root_is_held(g_root)) {
			fprintf(stderr, "FAIL: a directory named .lock counts as "
					"free\n");
			rc = 1;
		}
		rmdir(lock);
		if (sandbox_remove_jail_root(g_root) != 0) {
			fprintf(stderr, "FAIL: could not clean up the lock cases\n");
			rc = 1;
		}
	}

	if (rc == 0)
		printf("PASS: refusal, removal, the path shape and the lock probe\n");
	return rc;
}
