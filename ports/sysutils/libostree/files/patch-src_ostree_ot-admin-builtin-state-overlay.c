FreeBSD has no overlayfs, so the `ostree admin state-overlay` mount cannot
succeed.  Refuse loudly with a clear error instead of failing inside
mount(2) with an unknown filesystem type.

--- src/ostree/ot-admin-builtin-state-overlay.c.orig
+++ src/ostree/ot-admin-builtin-state-overlay.c
@@ -241,10 +241,17 @@
       = g_build_filename (OSTREE_STATEOVERLAYS_DIR, name, OSTREE_STATEOVERLAY_WORK_DIR, NULL);
   g_autofree char *ovl_options
       = g_strdup_printf ("lowerdir=%s,upperdir=%s,workdir=%s", mountpath, upperdir, workdir);
+/* FreeBSD has no overlayfs; refuse loudly rather than fail inside mount(2). */
+#ifdef __FreeBSD__
+  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
+               "state overlays require overlayfs, which FreeBSD does not have");
+  return FALSE;
+#else
   if (mount ("overlay", mountpath, "overlay", MS_SILENT, ovl_options) < 0)
     return glnx_throw_errno_prefix (error, "mount(%s)", mountpath);
 
   return TRUE;
+#endif
 }
 
 static gboolean
