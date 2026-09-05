ostree-linuxfsutil FreeBSD reimplementation

--- src/libostree/ostree-linuxfsutil.c.orig
+++ src/libostree/ostree-linuxfsutil.c
@@ -24,6 +24,70 @@
 
 #include <fcntl.h>
 #include <sys/ioctl.h>
+
+#ifdef __FreeBSD__
+/* FreeBSD has neither the Linux EXT2_IOC_*FLAGS ioctls nor the
+ * FIFREEZE/FITHAW filesystem-freeze ioctls.  The immutable flag maps
+ * onto the BSD UF_IMMUTABLE file flag, manipulated with fchflags(2);
+ * filesystem freeze/thaw has no equivalent, so those calls report
+ * EOPNOTSUPP.  See the Linux implementation below for the documented
+ * _ostree_linuxfs_fd_alter_immutable_flag contract. */
+#include <sys/stat.h>
+#include <unistd.h>
+
+gboolean
+_ostree_linuxfs_fd_alter_immutable_flag (int fd, gboolean new_immutable_state,
+                                         GCancellable *cancellable, GError **error)
+{
+  static gint no_alter_immutable = 0;
+  struct stat stbuf;
+
+  if (g_atomic_int_get (&no_alter_immutable))
+    return TRUE;
+
+  if (fstat (fd, &stbuf) < 0)
+    return glnx_throw_errno_prefix (error, "fstat");
+
+  const gboolean prev_immutable_state = (stbuf.st_flags & (UF_IMMUTABLE | SF_IMMUTABLE)) != 0;
+  if (prev_immutable_state == new_immutable_state)
+    return TRUE; /* Nothing to do */
+
+  unsigned long flags = stbuf.st_flags;
+  if (new_immutable_state)
+    flags |= UF_IMMUTABLE;
+  else
+    flags &= ~(UF_IMMUTABLE | SF_IMMUTABLE);
+
+  if (fchflags (fd, flags) < 0)
+    {
+      if (errno == EPERM || errno == EOPNOTSUPP || errno == ENOTSUP)
+        g_atomic_int_set (&no_alter_immutable, 1);
+      else
+        return glnx_throw_errno_prefix (error, "fchflags");
+    }
+
+  return TRUE;
+}
+
+/* FreeBSD has no filesystem freeze/thaw ioctls. */
+int
+_ostree_linuxfs_filesystem_freeze (int fd)
+{
+  (void)fd;
+  errno = EOPNOTSUPP;
+  return -1;
+}
+
+int
+_ostree_linuxfs_filesystem_thaw (int fd)
+{
+  (void)fd;
+  errno = EOPNOTSUPP;
+  return -1;
+}
+
+#else /* !__FreeBSD__ */
+
 // This should be the only file including linux/fs.h; see
 // https://sourceware.org/glibc/wiki/Release/2.36#Usage_of_.3Clinux.2Fmount.h.3E_and_.3Csys.2Fmount.h.3E
 // https://github.com/ostreedev/ostree/issues/2685
@@ -106,3 +170,5 @@
 {
   return TEMP_FAILURE_RETRY (ioctl (fd, FITHAW, 0));
 }
+
+#endif /* !__FreeBSD__ */
