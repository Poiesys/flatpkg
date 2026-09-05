FreeBSD has no Linux capability bounding set: <linux/capability.h> and
<sys/prctl.h> do not exist, and PR_CAPBSET_READ(CAP_SYS_ADMIN) has no
equivalent.  On FreeBSD euid 0 is simply privileged, so the prctl
refinement is compiled out.

--- src/libotutil/ot-unix-utils.c.orig
+++ src/libotutil/ot-unix-utils.c
@@ -29,10 +29,15 @@
 
 #include <errno.h>
 #include <fcntl.h>
+/* FreeBSD has no <linux/capability.h> or <sys/prctl.h>. */
+#ifndef __FreeBSD__
 #include <linux/capability.h>
+#endif
 #include <stdlib.h>
 #include <string.h>
+#ifndef __FreeBSD__
 #include <sys/prctl.h>
+#endif
 #include <sys/types.h>
 #include <unistd.h>
 
@@ -112,9 +117,14 @@
   if (geteuid () != 0)
     return FALSE;
 
+/* FreeBSD has no capability bounding set; euid 0 is privileged. */
+#ifdef __FreeBSD__
+  return TRUE;
+#else
   // https://github.com/containers/bootc/blob/c88fcfd6e145863408bde7d4706937dd323f64e2/lib/src/cli.rs#L621
   if (prctl (PR_CAPBSET_READ, CAP_SYS_ADMIN) != 1)
     return FALSE;
 
   return TRUE;
+#endif
 }
