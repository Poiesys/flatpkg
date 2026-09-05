ostree-soft-reboot FreeBSD include guards

--- src/libostree/ostree-soft-reboot.c.orig
+++ src/libostree/ostree-soft-reboot.c
@@ -12,7 +12,11 @@
 #include <errno.h>
 #include <fcntl.h>
 #include <libglnx.h>
+/* FreeBSD has no <linux/magic.h> or <sys/statfs.h>; the soft-reboot
+ * implementation itself is Linux-only and gated on HAVE_SOFT_REBOOT. */
+#ifndef __FreeBSD__
 #include <linux/magic.h>
+#endif
 #include <stdarg.h>
 #include <stdbool.h>
 #include <stdio.h>
@@ -22,7 +26,9 @@
 #include <sys/mount.h>
 #include <sys/param.h>
 #include <sys/stat.h>
+#ifndef __FreeBSD__
 #include <sys/statfs.h>
+#endif
 #include <sys/syscall.h>
 #include <sys/types.h>
 #include <unistd.h>
