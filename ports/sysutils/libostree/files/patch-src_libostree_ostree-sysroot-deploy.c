ostree-sysroot-deploy FreeBSD guards

--- src/libostree/ostree-sysroot-deploy.c.orig
+++ src/libostree/ostree-sysroot-deploy.c
@@ -24,7 +24,11 @@
 #include <gio/gunixoutputstream.h>
 #include <glib-unix.h>
 #include <inttypes.h>
+/* FreeBSD has no <linux/kexec.h>; the kexec support below is gated on
+ * SYS_kexec_file_load, which FreeBSD does not define. */
+#ifndef __FreeBSD__
 #include <linux/kexec.h>
+#endif
 #include <stdbool.h>
 #include <stdint.h>
 #include <sys/ioctl.h>
@@ -1604,8 +1608,13 @@
               /* Warn if the filesystem was already frozen */
               if (errno == EBUSY)
                 g_debug ("Filesystem already frozen, falling back to syncfs");
+/* FreeBSD has no syncfs(2); sync(2) flushes all mounted filesystems. */
+#ifdef __FreeBSD__
+              sync ();
+#else
               if (TEMP_FAILURE_RETRY (syncfs (rootfs_dfd)) != 0)
                 return glnx_throw_errno_prefix (error, "syncfs");
+#endif
               /* Write the completion, and return */
               if (write (sock_parent, &c, sizeof (c)) != sizeof (c))
                 return glnx_throw_errno_prefix (error, "write(watchdog syncfs complete)");
@@ -4026,10 +4035,15 @@
 prepare_root_child_setup (gpointer data)
 {
   struct PrepareRootChildSetupContext *ctx = data;
+  int rc;
+/* FreeBSD has no mount namespaces, so there is no root namespace to
+ * enter; just chdir into the deployment. */
+#ifndef __FreeBSD__
   // Enter the root namespace first to escape the overlayfs context
-  int rc = setns (ctx->rootns_fd, CLONE_NEWNS);
+  rc = setns (ctx->rootns_fd, CLONE_NEWNS);
   if (rc < 0)
     err (1, "setns");
+#endif
   // Then change to the deployment directory in the root namespace
   rc = chdir (ctx->deployment_path);
   if (rc < 0)
