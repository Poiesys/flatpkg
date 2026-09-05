ostree-repo-commit FreeBSD include guard

--- src/libostree/ostree-repo-commit.c.orig
+++ src/libostree/ostree-repo-commit.c
@@ -22,7 +22,11 @@
 
 #include "config.h"
 
+/* FreeBSD has no <ext2fs/ext2_fs.h> (the e2fsprogs header, which the
+ * Linux build uses only for EXT2_IOC_*FLAGS); nothing here uses it. */
+#ifndef __FreeBSD__
 #include <ext2fs/ext2_fs.h>
+#endif
 #include <gio/gfiledescriptorbased.h>
 #include <gio/gunixinputstream.h>
 #include <gio/gunixoutputstream.h>
