otcore prepare-root FreeBSD guard

--- src/libotcore/otcore-prepare-root.c.orig
+++ src/libotcore/otcore-prepare-root.c
@@ -451,6 +451,11 @@
 gboolean
 otcore_mount_boot (const char *physical_root, const char *deployment, GError **error)
 {
+/* FreeBSD has no Linux MS_BIND/MS_SILENT mount flags; prepare-root is a
+ * Linux boot-time helper. */
+#ifdef __FreeBSD__
+  return glnx_throw (error, "prepare-root mount operations are not supported on FreeBSD");
+#else
   g_autofree char *boot_loader = g_build_filename (physical_root, "boot/loader", NULL);
   struct stat stbuf;
 
@@ -469,6 +474,7 @@
     return glnx_throw (error, "failed to bind mount /boot");
 
   return TRUE;
+#endif
 }
 
 /**
@@ -480,6 +486,11 @@
 otcore_mount_etc (GKeyFile *config, GVariantBuilder *metadata_builder, const char *mount_target,
                   GError **error)
 {
+/* FreeBSD has no overlayfs and no Linux MS_BIND/MS_REMOUNT/MS_SILENT
+ * mount flags; prepare-root is a Linux boot-time helper. */
+#ifdef __FreeBSD__
+  return glnx_throw (error, "prepare-root mount operations are not supported on FreeBSD");
+#else
   gboolean etc_transient = FALSE;
   if (!ot_keyfile_get_boolean_with_default (config, ETC_KEY, OTCORE_PREPARE_ROOT_TRANSIENT_KEY,
                                             FALSE, &etc_transient, error))
@@ -531,6 +542,7 @@
                                         target_etc);
     }
   return TRUE;
+#endif
 }
 
 gboolean
