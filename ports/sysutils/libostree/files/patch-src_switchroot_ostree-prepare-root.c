ostree-prepare-root FreeBSD stub

--- src/switchroot/ostree-prepare-root.c.orig
+++ src/switchroot/ostree-prepare-root.c
@@ -51,6 +51,24 @@
  */
 
 #include "config.h"
+
+#ifdef __FreeBSD__
+/* ostree-prepare-root is a Linux initramfs helper: it reparents the root
+ * filesystem using Linux mount namespaces, pivot_root, btrfs ioctls and
+ * MS_* mount flags, none of which exist on FreeBSD.  FreeBSD does not boot
+ * this way, so build a stub that reports the operation is unsupported
+ * instead of pulling the whole Linux-specific program into the port. */
+#include <err.h>
+#include <stdlib.h>
+
+int
+main (int argc, char *argv[])
+{
+  (void)argc;
+  (void)argv;
+  errx (EXIT_FAILURE, "ostree-prepare-root is not supported on FreeBSD");
+}
+#else /* !__FreeBSD__ */
 
 #include <ctype.h>
 #include <err.h>
@@ -512,3 +530,5 @@
 
   exit (EXIT_SUCCESS);
 }
+
+#endif /* !__FreeBSD__ */
