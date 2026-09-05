grub2 bootloader FreeBSD guard

--- src/libostree/ostree-bootloader-grub2.c.orig
+++ src/libostree/ostree-bootloader-grub2.c
@@ -316,6 +316,10 @@
       _exit (1);
     }
 
+/* FreeBSD has no mount namespaces: unshare(2), MS_REC/MS_PRIVATE/
+ * MS_BIND/MS_MOVE are Linux-only.  The chroot below is enough on
+ * FreeBSD; this code path only runs for grub2 config generation. */
+#ifndef __FreeBSD__
   if (unshare (CLONE_NEWNS) != 0)
     {
       perror ("CLONE_NEWNS");
@@ -339,6 +343,7 @@
       perror ("failed to MS_MOVE to /");
       _exit (1);
     }
+#endif
 
   if (chroot (".") != 0)
     {
