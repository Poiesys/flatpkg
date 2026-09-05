/*
 * mk-summary.c - write a classic OSTree summary with chosen metadata.
 *
 * UpdateRemote only does something when the summary a client hands over
 * carries configuration the local remote does not have yet, and the two
 * interesting shapes cannot be produced with the ostree CLI:
 *
 *   - xa.gpg-keys is a *bytestring* (armored keys), and `ostree summary -m`
 *     cannot express one;
 *   - the metadata has to differ from what the remote already recorded,
 *     which means crafting a summary that was never published.
 *
 * So this is a test fixture: build, then
 *
 *   mk-summary out.summary [KEY=VALUE ...] [--gpg-keys=FILE]
 *
 * KEY=VALUE adds a string key (xa.title, xa.comment, xa.homepage, ...);
 * --gpg-keys adds xa.gpg-keys from an armored export.  Feed the result to
 * org.freedesktop.Flatpak.SystemHelper.UpdateRemote together with any
 * non-empty signature path for a remote that is not GPG-verified:
 *
 *   gdbus call --system --dest org.freedesktop.Flatpak.SystemHelper \
 *     --object-path /org/freedesktop/Flatpak/SystemHelper \
 *     --method org.freedesktop.Flatpak.SystemHelper.UpdateRemote \
 *     0 REMOTE "" "$(ay out.summary)" "$(ay dummy.sig)"
 *
 * (see CHANGES.md round 20; $(ay) is a bytestring literal, GVariant text
 * for the ay arguments.)
 *
 * Build: zig cc -O2 $(pkg-config --cflags glib-2.0) -o build/mk-summary \
 *            tests/mk-summary.c $(pkg-config --libs glib-2.0)
 */
#include <glib.h>
#include <stdio.h>
#include <string.h>

int
main(int argc, char **argv)
{
        GVariantBuilder meta;
        GVariant *refs, *summary;
        const char *out;
        int i;

        if (argc < 2) {
                fprintf(stderr, "usage: %s out.summary [KEY=VALUE ...] "
                    "[--gpg-keys=FILE]\n", argv[0]);
                return 2;
        }
        out = argv[1];

        g_variant_builder_init(&meta, G_VARIANT_TYPE("a{sv}"));
        for (i = 2; i < argc; i++) {
                char *eq, *key, *val;

                if (g_str_has_prefix(argv[i], "--gpg-keys=")) {
                        gchar *data = NULL;
                        gsize len = 0;

                        if (!g_file_get_contents(argv[i] + 11, &data, &len,
                            NULL) || len == 0) {
                                fprintf(stderr, "cannot read %s\n",
                                    argv[i] + 11);
                                return 1;
                        }
                        /* Armored text: no interior NUL, so a bytestring
                         * (ay + NUL) is exactly how flatpak stores it
                         * (flatpak_repo_set_gpg_keys). */
                        g_variant_builder_add(&meta, "{sv}", "xa.gpg-keys",
                            g_variant_new_bytestring(data));
                        g_free(data);
                        continue;
                }
                eq = strchr(argv[i], '=');
                if (eq == NULL) {
                        fprintf(stderr, "not KEY=VALUE: %s\n", argv[i]);
                        return 2;
                }
                key = g_strndup(argv[i], (gsize)(eq - argv[i]));
                val = eq + 1;
                g_variant_builder_add(&meta, "{sv}", key,
                    g_variant_new_string(val));
                g_free(key);
        }

        /* The classic summary: (refs, metadata).  An empty refs array is
         * valid - UpdateRemote only looks at child 1. */
        refs = g_variant_new_array(G_VARIANT_TYPE("(s(taya{sv}))"), NULL, 0);
        summary = g_variant_new("(@a(s(taya{sv}))@a{sv})", refs,
            g_variant_builder_end(&meta));
        if (!g_variant_is_normal_form(summary)) {
                fprintf(stderr, "summary is not normal form\n");
                return 1;
        }
        if (!g_file_set_contents(out, g_variant_get_data(summary),
            (gssize)g_variant_get_size(summary), NULL)) {
                fprintf(stderr, "cannot write %s\n", out);
                return 1;
        }
        printf("wrote %s (%zu bytes)\n", out,
            (size_t)g_variant_get_size(summary));
        return 0;
}
