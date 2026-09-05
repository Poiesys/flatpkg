/* nullfs-duplicate.c - compare nullfs_already_mounted() with the kernel.
 * The kernel answers EDEADLK on a nullfs stacked over the same lower vnode, so
 * every prediction is checked against a real mount_nullfs under argv[1]. */
#include <sys/param.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <assert.h>

#include "nlfn.inc"

static char A[PATH_MAX], B[PATH_MAX], T1[PATH_MAX], NOENT[PATH_MAX];
static int fails;

static int
run_tool(const char *tool, char *const argv[])
{
	pid_t pid = fork();
	int status = 0;

	if (pid == 0) {
		execv(tool, argv);
		_exit(127);
	}
	if (pid < 0 || waitpid(pid, &status, 0) < 0)
		return -1;
	if (!WIFEXITED(status))
		return -1;
	return WEXITSTATUS(status);
}

static int
try_mount(const char *from, const char *to)
{
	char *argv[] = { (char *)"/sbin/mount_nullfs", (char *)from,
	    (char *)to, NULL };

	return run_tool("/sbin/mount_nullfs", argv);
}

static void
umount_all(const char *to)
{
	char *argv[] = { (char *)"/sbin/umount", (char *)to, NULL };

	while (run_tool("/sbin/umount", argv) == 0)
		;
}

static void
check(const char *what, bool got, bool want)
{
	printf("%-56s %s\n", what, got == want ? "ok" : "FAIL");
	if (got != want)
		fails++;
}

/* The kernel must agree with the prediction: refused when the guard says
 * "already mounted there", mounted otherwise. */
static void
check_kernel(const char *what, const char *from, const char *to, bool mounted)
{
	int rc = try_mount(from, to);

	check(what, rc != 0, mounted);
}

int
main(int argc, char **argv)
{
	const char *root = argc > 1 ? argv[1] : "/tmp/flatpkg-nullfs-test";
	char cmd[PATH_MAX * 2], p[PATH_MAX];

	snprintf(cmd, sizeof(cmd), "rm -rf '%s' && mkdir -p '%s/A' '%s/B' "
	    "'%s/T1'", root, root, root, root);
	if (system(cmd) != 0) {
		fprintf(stderr, "cannot prepare %s\n", root);
		return 2;
	}
	snprintf(A, sizeof(A), "%s/A", root);
	snprintf(B, sizeof(B), "%s/B", root);
	snprintf(T1, sizeof(T1), "%s/T1", root);
	snprintf(NOENT, sizeof(NOENT), "%s/nope", root);

	umount_all(T1);

	/* Empty target. */
	check("empty target -> false", nullfs_already_mounted(A, T1), false);
	check_kernel("kernel mounts A over the empty target", A, T1, false);

	/* T1 is now a nullfs of A: the next A mount is the EDEADLK case. */
	check("A at T1 -> true", nullfs_already_mounted(A, T1), true);
	check_kernel("kernel refuses A again (EDEADLK)", A, T1, true);
	check("other source B at T1 -> false", nullfs_already_mounted(B, T1),
	    false);
	check_kernel("kernel accepts B over A", B, T1, false);

	/* Topmost is B; A is the covered lower and may be mounted again. */
	check("topmost B at T1 -> true", nullfs_already_mounted(B, T1), true);
	check("covered A at T1 -> false", nullfs_already_mounted(A, T1), false);
	check_kernel("kernel refuses B again (EDEADLK)", B, T1, true);
	check_kernel("kernel accepts the covered A again", A, T1, false);

	check("topmost A at T1 -> true", nullfs_already_mounted(A, T1), true);
	check_kernel("kernel refuses A again (EDEADLK)", A, T1, true);

	/* Degenerate and non-existent paths. */
	check("source == target -> true", nullfs_already_mounted(T1, T1), true);
	check("missing target -> false", nullfs_already_mounted(A, NOENT),
	    false);
	snprintf(p, sizeof(p), "%s/./A/", root);
	check("non-canonical source -> true", nullfs_already_mounted(p, T1),
	    true);

	umount_all(T1);
	printf("%s\n", fails == 0 ? "ALL OK" : "FAILURES");
	return fails != 0;
}
