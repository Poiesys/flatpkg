/* subextension-ref.c - harness for ext_id_matches_declared() and
 * group_covers_branch(), which decide whether `upgrade --prune` may delete a
 * deployed extension: true prunes a mounted driver, false prunes nothing. */
#include <sys/param.h>
#include <sys/types.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stand-ins for the two types the extracted functions touch, named exactly as
 * include/flatpkg.h names them so the fragment compiles unmodified. */
typedef struct {
        char **items;
        size_t count;
} strlist_t;

typedef struct {
        char *id;
        char *version;
        strlist_t versions;
} fp_ext_group_t;

#include <assert.h>

#include "subext.inc"

static int failures;

static void
check(const char *label, bool got, bool want)
{
        printf("%-58s %s\n", label, got == want ? "ok" : "FAIL");
        if (got != want)
                failures++;
}

static void
set_versions(fp_ext_group_t *g, const char *semicolon_list)
{
        g->versions.count = 0;
        if (semicolon_list == NULL)
                return;
        char buf[256];
        snprintf(buf, sizeof(buf), "%s", semicolon_list);
        for (char *p = buf; p != NULL && *p != '\0'; ) {
                char *semi = strchr(p, ';');
                if (semi != NULL)
                        *semi = '\0';
                g->versions.items = realloc(g->versions.items,
                    (g->versions.count + 1) * sizeof(char *));
                g->versions.items[g->versions.count++] = strdup(p);
                p = (semi != NULL) ? semi + 1 : NULL;
        }
}

int
main(void)
{
        /* The real org.freedesktop.Platform.GL group from 25.08's metadata:
         *   [Extension org.freedesktop.Platform.GL]
         *   versions = 25.08;25.08-extra;1.4 */
        fp_ext_group_t gl = { .id = (char *)"org.freedesktop.Platform.GL",
            .version = (char *)"1.4" };
        set_versions(&gl, "25.08;25.08-extra;1.4");

        /* 1. an exact match is always a reference, whatever the branch. */
        check("exact id matches",
            ext_id_matches_declared("org.freedesktop.Platform.GL", "99.99", &gl),
            true);

        /* 2. the bug: a subdirectory sub-extension of a declared parent, on a
         * branch the parent declares, IS referenced - the mounted GL driver. */
        check("GL.default on 25.08 (declared) is referenced",
            ext_id_matches_declared("org.freedesktop.Platform.GL.default",
                "25.08", &gl), true);
        check("GL.default on 25.08-extra (declared) is referenced",
            ext_id_matches_declared("org.freedesktop.Platform.GL.default",
                "25.08-extra", &gl), true);
        check("GL.default on 1.4 (declared version) is referenced",
            ext_id_matches_declared("org.freedesktop.Platform.GL.default",
                "1.4", &gl), true);

        /* 3. the over-correction: a sub-extension on a branch the parent does
         * NOT declare is not referenced, so the 26.08 copies stay prunable. */
        check("GL.default on 26.08 (undeclared) is not referenced",
            ext_id_matches_declared("org.freedesktop.Platform.GL.default",
                "26.08", &gl), false);
        check("GL.default on 26.08-extra (undeclared) is not referenced",
            ext_id_matches_declared("org.freedesktop.Platform.GL.default",
                "26.08-extra", &gl), false);

        /* 4. an unrelated id is not a sub-extension of anything. */
        check("unrelated id does not match",
            ext_id_matches_declared("org.gnome.Platform.Locale", "25.08", &gl),
            false);
        /* 5. a shared prefix is not a parent: "...GLX" must not match
         * "...Platform.GL". */
        check("shared prefix is not a parent",
            ext_id_matches_declared("org.freedesktop.Platform.GLX.default",
                "25.08", &gl), false);

        /* 6. a group with no versions= at all falls back to version=. */
        fp_ext_group_t simple = { .id = (char *)"org.example.Ext",
            .version = (char *)"stable" };
        set_versions(&simple, NULL);
        check("version= fallback matches its branch",
            ext_id_matches_declared("org.example.Ext.sub", "stable", &simple),
            true);
        check("version= fallback rejects another branch",
            ext_id_matches_declared("org.example.Ext.sub", "beta", &simple),
            false);

        /* 7. no branch at all: the callers (reap_orphans and plan_removal walk
         * <store>/extension, whose entries are bare ids) cannot tell which branch
         * is live, so a sub-extension has to be accepted. */
        check("no branch: a sub-extension is accepted",
            ext_id_matches_declared("org.freedesktop.Platform.GL.default",
                NULL, &gl), true);
        check("no branch: an empty branch is accepted too",
            ext_id_matches_declared("org.freedesktop.Platform.GL.default",
                "", &gl), true);
        check("no branch: an unrelated id still does not match",
            ext_id_matches_declared("org.gnome.Platform.Locale", NULL, &gl),
            false);

        /* 8. a NULL group or id must not crash. */
        check("NULL group is rejected",
            ext_id_matches_declared("org.example.Ext", "stable", NULL),
            false);
        check("NULL id is rejected",
            ext_id_matches_declared(NULL, "stable", &gl), false);

        /* 9. group_covers_branch on its own. */
        check("group_covers_branch finds a listed version",
            group_covers_branch(&gl, "25.08"), true);
        check("group_covers_branch rejects an unlisted one",
            group_covers_branch(&gl, "26.08"), false);
        check("group_covers_branch with an empty list is false",
            group_covers_branch(&simple, "25.08"), false);

        if (failures) {
                printf("\n%d check(s) FAILED\n", failures);
                return 1;
        }
        printf("\nALL OK\n");
        return 0;
}
