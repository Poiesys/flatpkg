/*
 * deploy-commit.c - harness for deploy_commit()'s two commit helpers,
 * #included from a fragment tests/deploy-commit.sh carves out of
 * src/daemon/ops/instance_ctl.c, so the shipped code is what runs.
 */
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <assert.h>

#include "deploy_commit.inc"

static int failures;

static void
check(const char *label, bool ok)
{
        printf("%-46s %s\n", label, ok ? "ok" : "FAIL");
        if (!ok)
                failures++;
}

static const char HEX64[] =
    "be6bc4ab095c275998efad5b3538907f957b2b3833e6c3735d7351f69b010bf2";

/* <tree>/<name>/active -> 64-hex, plus the files dir the caller passes. */
static void
make_symlink_case(const char *tree, const char *name, const char *target)
{
        char b[PATH_MAX];
        snprintf(b, sizeof(b), "%s/%s", tree, name);
        mkdir(b, 0755);
        char active[PATH_MAX], files[PATH_MAX];
        snprintf(active, sizeof(active), "%s/active", b);
        snprintf(files, sizeof(files), "%s/active/files", b);
        if (symlink(target, active) != 0) {
                perror("symlink");
                exit(1);
        }
        mkdir(files, 0755);
}

/* <tree>/<name>/deploy holding "<origin>\0<commit>\0<rest>", no symlink. */
static void
make_deploy_file_case(const char *tree, const char *name, const char *origin,
    const char *commit, const char *rest)
{
        char b[PATH_MAX];
        snprintf(b, sizeof(b), "%s/%s", tree, name);
        mkdir(b, 0755);
        char dp[PATH_MAX], files[PATH_MAX];
        snprintf(dp, sizeof(dp), "%s/deploy", b);
        snprintf(files, sizeof(files), "%s/files", b);
        mkdir(files, 0755);

        int fd = open(dp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
                perror("open deploy");
                exit(1);
        }
        /* The origin and the commit are the first two NUL-terminated
         * strings in the serialised (a{sv}, t, a(yay), y). */
        (void)!write(fd, origin, strlen(origin) + 1);
        (void)!write(fd, commit, strlen(commit) + 1);
        if (rest != NULL)
                (void)!write(fd, rest, strlen(rest) + 1);
        close(fd);
}

/* <tree>/<name>/active/ is a real directory holding `deploy` and
 * `files/` - what the branch looks like once the symlink has been
 * resolved in place rather than followed. */
static void
make_active_dir_deploy_case(const char *tree, const char *name)
{
        char b[PATH_MAX], active[PATH_MAX];
        snprintf(b, sizeof(b), "%s/%s", tree, name);
        mkdir(b, 0755);
        snprintf(active, sizeof(active), "%s/active", b);
        if (mkdir(active, 0755) != 0) {
                perror("mkdir active");
                exit(1);
        }
        char files[PATH_MAX];
        snprintf(files, sizeof(files), "%s/files", active);
        mkdir(files, 0755);

        char dp[PATH_MAX];
        snprintf(dp, sizeof(dp), "%s/deploy", active);
        int fd = open(dp, O_CREAT | O_WRONLY | O_TRUNC, 0644);
        if (fd < 0) {
                perror("open deploy");
                exit(1);
        }
        /* OSTree serialises (a{sv}, t, a(yay), y); the origin and the
         * commit are the first two NUL-terminated strings. */
        (void)!write(fd, "flathub", 8);
        (void)!write(fd, HEX64, 65);
        (void)!write(fd, "\1\0", 2);
        close(fd);
}

static void
run(const char *tree)
{
        char out[80];
        char files[PATH_MAX];

        /* 1. the ordinary store layout: `active` is a symlink to the
         * commit.  This is the path every real launch takes. */
        make_symlink_case(tree, "symlink", HEX64);
        snprintf(files, sizeof(files), "%s/symlink/active/files", tree);
        out[0] = 'X';
        deploy_commit(files, out, sizeof(out));
        check("symlinked active -> that checksum", strcmp(out, HEX64) == 0);

        /* 2. a deploy dir that has already been resolved: its own name is
         * the commit, and no symlink or deploy file is consulted. */
        {
                char resolved[PATH_MAX];
                snprintf(resolved, sizeof(resolved), "%s/resolved/%s",
                    tree, HEX64);
                mkdir(resolved, 0755);
                char f[PATH_MAX];
                snprintf(f, sizeof(f), "%s/files", resolved);
                mkdir(f, 0755);
                out[0] = 'X';
                deploy_commit(f, out, sizeof(out));
                check("resolved <commit>/files -> that checksum",
                    strcmp(out, HEX64) == 0);
        }

        /* 3. a tree whose `active` is a real directory: the deploy file
         * inside it is the only place the commit survives. */
        make_active_dir_deploy_case(tree, "deployfile");
        snprintf(files, sizeof(files), "%s/deployfile/active/files", tree);
        out[0] = 'X';
        deploy_commit(files, out, sizeof(out));
        check("active/ is a real dir -> its deploy file",
            strcmp(out, HEX64) == 0);

        /* 4. a symlink to something that is not a checksum. */
        make_symlink_case(tree, "notlink", "current");
        snprintf(files, sizeof(files), "%s/notlink/active/files", tree);
        out[0] = 'X';
        deploy_commit(files, out, sizeof(out));
        check("symlink to a non-checksum -> empty", out[0] == '\0');

        /* 5. one character short. */
        make_symlink_case(tree, "short", "be6bc4ab095c275998efad5b3538907f"
            "957b2b3833e6c3735d7351f69b010bf");
        snprintf(files, sizeof(files), "%s/short/active/files", tree);
        out[0] = 'X';
        deploy_commit(files, out, sizeof(out));
        check("63-char symlink target -> empty", out[0] == '\0');

        /* 6. upper case: OSTree checksums are lower case hex. */
        {
                char upper[65];
                snprintf(upper, sizeof(upper), "%s", HEX64);
                for (char *p = upper; *p; p++)
                        if (*p >= 'a' && *p <= 'f')
                                *p = (char)(*p - 'a' + 'A');
                make_symlink_case(tree, "upper", upper);
                snprintf(files, sizeof(files), "%s/upper/active/files",
                    tree);
                out[0] = 'X';
                deploy_commit(files, out, sizeof(out));
                check("upper-case hex -> empty", out[0] == '\0');
        }

        /* 7. a deploy file whose second field is not a checksum. */
        make_deploy_file_case(tree, "baddeploy", "flathub", "deadbeef",
            "\1\0");
        snprintf(files, sizeof(files), "%s/baddeploy/files", tree);
        out[0] = 'X';
        deploy_commit(files, out, sizeof(out));
        check("non-hex second field -> empty", out[0] == '\0');

        /* 8. a truncated caller buffer must yield nothing, not half a
         * checksum.  This is the one case a correct-looking result would
         * still be wrong. */
        make_symlink_case(tree, "trunc", HEX64);
        snprintf(files, sizeof(files), "%s/trunc/active/files", tree);
        out[0] = 'X';
        deploy_commit(files, out, 8);
        check("out buffer too small -> empty", out[0] == '\0');

        /* 9. a path that does not exist at all. */
        snprintf(files, sizeof(files), "%s/nope/active/files", tree);
        out[0] = 'X';
        deploy_commit(files, out, sizeof(out));
        check("missing tree -> empty", out[0] == '\0');

        /* 10. NULL and empty inputs must not be dereferenced. */
        out[0] = 'X';
        deploy_commit(NULL, out, sizeof(out));
        check("NULL files_dir -> empty", out[0] == '\0');
        out[0] = 'X';
        deploy_commit("", out, sizeof(out));
        check("empty files_dir -> empty", out[0] == '\0');

        /* 11. a relative path with no '/' at all. */
        out[0] = 'X';
        deploy_commit("files", out, sizeof(out));
        check("no slash in files_dir -> empty", out[0] == '\0');
}

int
main(int argc, char **argv)
{
        if (argc != 2) {
                fprintf(stderr, "usage: %s <tree>\n", argv[0]);
                return 2;
        }
        run(argv[1]);
        if (failures) {
                printf("\n%d check(s) FAILED\n", failures);
                return 1;
        }
        printf("\nALL OK\n");
        return 0;
}
