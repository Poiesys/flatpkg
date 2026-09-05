ostree-repo FreeBSD portability fixes

--- src/libostree/ostree-repo.c.orig
+++ src/libostree/ostree-repo.c
@@ -31,7 +31,11 @@
 #include <gio/gunixoutputstream.h>
 #include <glib-unix.h>
 #include <glnx-console.h>
+/* FreeBSD has no <linux/magic.h>; the only symbol used from it,
+ * FUSE_SUPER_MAGIC, has a local fallback definition below. */
+#ifndef __FreeBSD__
 #include <linux/magic.h>
+#endif
 
 #include "ostree-autocleanups.h"
 #include "ostree-core-private.h"
@@ -48,7 +52,16 @@
 #include <glib/gstdio.h>
 #include <locale.h>
 #include <sys/file.h>
+/* FreeBSD has no <sys/statfs.h>; struct statfs and fstatfs(2) live in
+ * <sys/mount.h>. */
+#ifdef __FreeBSD__
+#include <sys/mount.h>
+#include <sys/sysctl.h>
+#include <sys/time.h>
+#include <sys/types.h>
+#else
 #include <sys/statfs.h>
+#endif
 #include <sys/statvfs.h>
 
 #define REPO_LOCK_DISABLED (-2)
@@ -1532,9 +1545,14 @@
   if (is_system)
     ot_journal_print (LOG_INFO, "Starting syncfs for system repo");
   guint64 start_msec = g_get_monotonic_time () / 1000;
+/* FreeBSD has no syncfs(2); sync(2) flushes all mounted filesystems. */
+#ifdef __FreeBSD__
+  sync ();
+#else
   int repo_dfd = ostree_repo_get_dfd (self);
   if (syncfs (repo_dfd) != 0)
     return glnx_throw_errno_prefix (error, "syncfs(repo)");
+#endif
   guint64 end_msec = g_get_monotonic_time () / 1000;
   if (is_system)
     ot_journal_print (LOG_INFO, "Completed syncfs() for system repo in %" G_GUINT64_FORMAT " ms",
@@ -3480,9 +3498,20 @@
       boot_id = g_strdup (env_bootid);
     else
       {
+/* FreeBSD has no /proc/sys/kernel/random/boot_id; derive a stable
+ * per-boot identifier from the kern.boottime sysctl instead. */
+#ifdef __FreeBSD__
+        struct timeval boottime;
+        size_t boottime_len = sizeof (boottime);
+        int boottime_mib[2] = { CTL_KERN, KERN_BOOTTIME };
+        if (sysctl (boottime_mib, 2, &boottime, &boottime_len, NULL, 0) < 0)
+          return glnx_throw_errno_prefix (error, "sysctl(kern.boottime)");
+        boot_id = g_strdup_printf ("%" G_GINT64_FORMAT, (gint64)boottime.tv_sec);
+#else
         if (!g_file_get_contents ("/proc/sys/kernel/random/boot_id", &boot_id, NULL, error))
           return FALSE;
         g_strdelimit (boot_id, "\n", '\0');
+#endif
       }
 
     self->stagedir_prefix = g_strconcat (OSTREE_REPO_TMPDIR_STAGING, boot_id, "-", NULL);
@@ -3519,7 +3548,13 @@
 #ifndef FUSE_SUPER_MAGIC
 #define FUSE_SUPER_MAGIC 0x65735546
 #endif
+/* FreeBSD's struct statfs has no f_type; identify FUSE by the
+ * filesystem type name instead. */
+#ifdef __FreeBSD__
+    self->is_on_fuse = (strcmp (fsstbuf.f_fstypename, "fusefs") == 0);
+#else
     self->is_on_fuse = (fsstbuf.f_type == FUSE_SUPER_MAGIC);
+#endif
     g_debug ("using fuse: %d", self->is_on_fuse);
   }
 
