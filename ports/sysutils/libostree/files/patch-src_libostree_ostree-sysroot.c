ostree-sysroot FreeBSD guards

--- src/libostree/ostree-sysroot.c.orig
+++ src/libostree/ostree-sysroot.c
@@ -22,10 +22,16 @@
 
 #include "otutil.h"
 #include <err.h>
+/* FreeBSD has no <linux/magic.h> or <sys/vfs.h>; struct statfs and
+ * fstatfs(2) come from <sys/mount.h>. */
+#ifndef __FreeBSD__
 #include <linux/magic.h>
+#endif
 #include <sys/file.h>
 #include <sys/mount.h>
+#ifndef __FreeBSD__
 #include <sys/vfs.h>
+#endif
 #include <sys/wait.h>
 
 #include "ostree-bootloader-aboot.h"
@@ -306,8 +312,12 @@
   // If the mount namespaces are the same, we need to unshare().
   if (strcmp (mntns_pid1, mntns_self) == 0)
     {
+#ifdef __FreeBSD__
+      return glnx_throw (error, "mount namespaces are not supported on FreeBSD");
+#else
       if (unshare (CLONE_NEWNS) < 0)
         return glnx_throw_errno_prefix (error, "Failed to invoke unshare(CLONE_NEWNS)");
+#endif
     }
 
   ostree_sysroot_set_mount_namespace_in_use (self);
@@ -347,7 +357,12 @@
   struct statfs stbuf;
   if (fstatfs (fd, &stbuf) < 0)
     return glnx_throw_errno_prefix (error, "fstatfs(boot)");
+/* FreeBSD's struct statfs has no f_type; match the fs type name. */
+#ifdef __FreeBSD__
+  self->boot_is_vfat = (strcmp (stbuf.f_fstypename, "msdosfs") == 0);
+#else
   self->boot_is_vfat = (stbuf.f_type == MSDOS_SUPER_MAGIC);
+#endif
   if (self->boot_is_vfat)
     return glnx_throw (error, "/boot cannot currently be a vfat filesystem");
   return TRUE;
@@ -417,10 +432,14 @@
   if ((stvfsbuf.f_flag & ST_RDONLY) != 0)
     {
       /* OK, let's remount writable. */
+#ifdef __FreeBSD__
+      return glnx_throw (error, "remounting %s read-write is not supported on FreeBSD", path);
+#else
       if (mount (path, path, NULL, MS_REMOUNT | MS_RELATIME, "") < 0)
         return glnx_throw_errno_prefix (error, "Remounting %s read-write", path);
       *did_remount = TRUE;
       g_debug ("remounted %s writable", path);
+#endif
     }
 
   return TRUE;
@@ -2401,6 +2420,11 @@
       return glnx_throw_errno_prefix (error, "fork");
     else if (mount_child == 0)
       {
+/* FreeBSD has no overlayfs and no Linux mount flags; unlocking a
+ * deployment is a Linux-only operation. */
+#ifdef __FreeBSD__
+        errx (1, "unlocking deployments is not supported on FreeBSD");
+#else
         int mountflags = 0;
         if (unlocked_state == OSTREE_DEPLOYMENT_UNLOCKED_TRANSIENT)
           mountflags |= MS_RDONLY;
@@ -2414,6 +2438,7 @@
         if (mount ("overlay", "/usr", "overlay", mountflags, ovl_options) < 0)
           err (1, "mount");
         exit (EXIT_SUCCESS);
+#endif
       }
     else
       {
