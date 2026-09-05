zipl bootloader FreeBSD guard

--- src/libostree/ostree-bootloader-zipl.c.orig
+++ src/libostree/ostree-bootloader-zipl.c
@@ -84,20 +84,30 @@
 static gboolean
 _ostree_secure_execution_mount (GError **error)
 {
+/* FreeBSD has no 5-argument Linux mount(2) and no umount(2); s390x
+ * secure execution is Linux-only anyway. */
+#ifdef __FreeBSD__
+  return glnx_throw (error, "s390x secure execution is not supported on FreeBSD");
+#else
   const char *device = realpath (SECURE_EXECUTION_PARTITION, NULL);
   if (device == NULL)
     return glnx_throw_errno_prefix (error, "s390x SE: resolving %s", SECURE_EXECUTION_PARTITION);
   if (mount (device, SECURE_EXECUTION_MOUNTPOINT, "ext4", 0, NULL) < 0)
     return glnx_throw_errno_prefix (error, "s390x SE: Mounting %s", device);
   return TRUE;
+#endif
 }
 
 static gboolean
 _ostree_secure_execution_umount (GError **error)
 {
+#ifdef __FreeBSD__
+  return glnx_throw (error, "s390x secure execution is not supported on FreeBSD");
+#else
   if (umount (SECURE_EXECUTION_MOUNTPOINT) < 0)
     return glnx_throw_errno_prefix (error, "s390x SE: Unmounting %s", SECURE_EXECUTION_MOUNTPOINT);
   return TRUE;
+#endif
 }
 
 static gboolean
