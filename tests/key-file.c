// key-file.c - the writers that go through a directory descriptor.  A name in
// a directory the caller can write may be a symlink standing in for the file,
// and a key is not just any file: it is the trust anchor for every pull, so it
// is created 0600 and replaced rather than truncated in place.
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include "flatpkg.h"

// Answered here because this harness links only the writers and the logging
// they warn through: these are cli.c's and the translation layer's.
const char* get_string(const char* msgid) { return msgid; }
const char* cli_getenv(const char* name) {
	(void)name;
	return NULL;
}

static int bad;

static void note(const char* what) {
	printf("FAIL: %s\n", what);
	bad = 1;
}

static mode_t mode_of(int dfd, const char* name) {
	struct stat st;

	if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) return 0;
	return st.st_mode & 07777;
}

static char* slurp(const char* path) {
	static char buf[64];
	FILE* f = fopen(path, "r");
	size_t n;

	if (f == NULL) return NULL;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	return buf;
}

int main(void) {
	char tmpl[] = "/tmp/.fpkg-keyfile.XXXXXX";
	char* dir = mkdtemp(tmpl);
	char outside[PATH_MAX];
	int dfd, ofd;

	assert(dir != NULL);
	snprintf(outside, sizeof(outside), "%s/outside", dir);
	ofd = open(outside, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	assert(ofd >= 0);
	if (write(ofd, "keep", 4) != 4) note("cannot seed the outside file");
	close(ofd);

	dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	assert(dfd >= 0);

	// An ordinary write, and its mode is the one asked for.
	if (write_file_at(dfd, "plain", "hello", 0644) != FPKG_OK)
		note("write_file_at() failed");
	if (mode_of(dfd, "plain") != 0644) note("a written file kept another mode");
	if (strcmp(slurp(outside), "keep") != 0)
		note("the outside file changed during a plain write");

	// A symlink is not followed: the target keeps its content and the call
	// fails rather than succeeding through the link.
	if (symlinkat(outside, dfd, "link") != 0) note("cannot plant the symlink");
	if (write_file_at(dfd, "link", "clobber", 0644) == FPKG_OK)
		note("write_file_at() followed a symlink");
	if (strcmp(slurp(outside), "keep") != 0)
		note("write_file_at() wrote through a symlink");

	// A secret file is 0600 from its first byte.
	if (write_secret_file_at(dfd, "key", "s3cret") != FPKG_OK)
		note("write_secret_file_at() failed");
	if (mode_of(dfd, "key") != 0600) note("a key file is not 0600");
	if (strcmp(slurp(outside), "keep") != 0)
		note("the outside file changed during a secret write");

	// Replacing an existing key changes its mode to 0600 as well.
	if (write_file_at(dfd, "old", "old", 0644) != FPKG_OK)
		note("cannot seed an old key");
	if (write_secret_file_at(dfd, "old", "new") != FPKG_OK)
		note("replacing a key failed");
	if (mode_of(dfd, "old") != 0600) note("a replaced key is not 0600");

	// A symlink standing in for a key is removed, not followed.
	if (symlinkat(outside, dfd, "klink") != 0)
		note("cannot plant the key symlink");
	if (write_secret_file_at(dfd, "klink", "fresh") != FPKG_OK)
		note("a key symlink was not replaced");
	if (mode_of(dfd, "klink") != 0600) note("the replacement key is not 0600");
	if (strcmp(slurp(outside), "keep") != 0)
		note("a key write followed a symlink");

	// A directory name is only that: a file in its place is not it.
	{
		char p[PATH_MAX];

		snprintf(p, sizeof(p), "%s/plain", dir);
		if (mkdir_p(p, 0755) == FPKG_OK)
			note("mkdir_p() accepted a regular file as a directory");
	}

	unlinkat(dfd, "plain", 0);
	unlinkat(dfd, "link", 0);
	unlinkat(dfd, "key", 0);
	unlinkat(dfd, "old", 0);
	unlinkat(dfd, "klink", 0);
	close(dfd);
	unlink(outside);
	rmdir(dir);

	if (bad) return 1;
	printf("ok   file writes are descriptor-relative, and a key is 0600\n");
	return 0;
}
