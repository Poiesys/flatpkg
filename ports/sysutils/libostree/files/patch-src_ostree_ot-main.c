FreeBSD has no mount namespaces; unshare(2) and MS_REMOUNT/MS_SILENT are
Linux-only.  Both call sites are guarded rather than emulated and fail
loudly if reached.

--- src/ostree/ot-main.c.orig
+++ src/ostree/ot-main.c
@@ -125,11 +125,20 @@
   if (errno == ENOENT)
     return TRUE;
 
+/* FreeBSD has no mount namespaces.  This point is only reached on an
+ * ostree-booted system, which FreeBSD is not; fail loudly rather than
+ * pretending to have a namespace we do not have. */
+#ifdef __FreeBSD__
+  g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
+               "mount namespaces are not supported on FreeBSD");
+  return FALSE;
+#else
   if (unshare (CLONE_NEWNS) < 0)
     return glnx_throw_errno_prefix (error, "setting up mount namespace: unshare(CLONE_NEWNS)");
 
   *out_ns = TRUE;
   return TRUE;
+#endif
 }
 
 static void
@@ -400,8 +409,12 @@
         return FALSE;
       if (setup_ns)
         {
+/* FreeBSD: MS_REMOUNT/MS_SILENT do not exist and this remount is only
+ * reachable on ostree-booted systems (see maybe_setup_mount_namespace). */
+#ifndef __FreeBSD__
           if (mount ("/sysroot", "/sysroot", NULL, MS_REMOUNT | MS_SILENT, NULL) < 0)
             return glnx_null_throw_errno_prefix (error, "Remounting /sysroot read-write");
+#endif
 
           /* Reload the repo so it's actually writable. */
           g_clear_pointer (&repo, g_object_unref);
