fastfetch's Packages module implements Flatpak counting only in
packages_linux.c; the FreeBSD backend (packages_bsd.c) counts just pkg
and mport, so Flatpak/Flatpkg apps never appear in the "Packages" line.

Port the Linux directory scan to the FreeBSD backend: under
/var/lib/flatpak an app/<id>/current link counts as one app and each
runtime/<id>/<arch>/<branch> as one runtime (skipping .Locale/.Debug),
with the same scan under ~/.local/share/flatpak for user installs.
flatpkg keeps its store at /var/db/flatpkg, and flatpkg apps are counted as flatpak.

Mirrors getFlatpakPackages() in packages_linux.c.

--- src/detection/packages/packages_bsd.c.orig
+++ src/detection/packages/packages_bsd.c
@@ -2,6 +2,11 @@
 
 #include "common/settings.h"
 
+#include <dirent.h>
+#include <stdlib.h>
+#include <string.h>
+#include <sys/stat.h>
+
 static uint32_t getSQLite3Int(const char* dbPath, const char* query, const char* packageId) {
     FF_STRBUF_AUTO_DESTROY cacheDir = ffStrbufCreate();
     FF_STRBUF_AUTO_DESTROY cacheContent = ffStrbufCreate();
@@ -18,6 +23,94 @@
     return num_elements;
 }
 
+static uint32_t getFlatpakAppPackages(const char* flatpakDir) {
+    char path[1024];
+    snprintf(path, sizeof(path), "%s/app", flatpakDir);
+    DIR* dirp = opendir(path);
+    if (dirp == nullptr) {
+        return 0;
+    }
+
+    uint32_t num_elements = 0;
+    struct dirent* entry;
+    while ((entry = readdir(dirp)) != nullptr) {
+        if (entry->d_name[0] == '.') {
+            continue;
+        }
+        char current[2048];
+        snprintf(current, sizeof(current), "%s/app/%s/current", flatpakDir, entry->d_name);
+        struct stat st;
+        if (stat(current, &st) == 0) { // follows the link: a deleted app's dangling `current` is not counted
+            ++num_elements;
+        }
+    }
+    closedir(dirp);
+    return num_elements;
+}
+
+static uint32_t getFlatpakRuntimeArchPackages(const char* idDir) {
+    DIR* dirp = opendir(idDir);
+    if (dirp == nullptr) {
+        return 0;
+    }
+
+    uint32_t num_elements = 0;
+    struct dirent* entry;
+    while ((entry = readdir(dirp)) != nullptr) {
+        if (entry->d_name[0] == '.' || entry->d_type != DT_DIR) {
+            continue;
+        }
+        char archDir[2048];
+        snprintf(archDir, sizeof(archDir), "%s/%s", idDir, entry->d_name);
+        num_elements += ffPackagesGetNumElements(archDir, true);
+    }
+    closedir(dirp);
+    return num_elements;
+}
+
+static uint32_t getFlatpakRuntimePackages(const char* flatpakDir) {
+    char path[1024];
+    snprintf(path, sizeof(path), "%s/runtime", flatpakDir);
+    DIR* dirp = opendir(path);
+    if (dirp == nullptr) {
+        return 0;
+    }
+
+    uint32_t num_elements = 0;
+    struct dirent* entry;
+    while ((entry = readdir(dirp)) != nullptr) {
+        if (entry->d_name[0] == '.' || entry->d_type != DT_DIR) {
+            continue;
+        }
+        const char* dot = strrchr(entry->d_name, '.');
+        if (dot == nullptr || strcmp(dot + 1, "Locale") == 0 || strcmp(dot + 1, "Debug") == 0) {
+            continue;
+        }
+        char idDir[2048];
+        snprintf(idDir, sizeof(idDir), "%s/%s", path, entry->d_name);
+        num_elements += getFlatpakRuntimeArchPackages(idDir);
+    }
+    closedir(dirp);
+    return num_elements;
+}
+
+static uint32_t getFlatpakPackages(const char* root) {
+    char flatpakDir[1024];
+    snprintf(flatpakDir, sizeof(flatpakDir), "%s/flatpkg", root);
+    return getFlatpakAppPackages(flatpakDir) + getFlatpakRuntimePackages(flatpakDir);
+}
+
 void ffDetectPackagesImpl(FFPackagesResult* result, FFPackagesOptions* options) {
     if (FF_PACKAGES_IS_ENABLED(options, PKG)) {
         result->pkg = getSQLite3Int(FASTFETCH_TARGET_DIR_ROOT "/var/db/pkg/local.sqlite", "SELECT count(*) FROM packages", "pkg");
@@ -25,4 +118,13 @@
     if (FF_PACKAGES_IS_ENABLED(options, MPORT)) {
         result->mport = getSQLite3Int(FASTFETCH_TARGET_DIR_ROOT "/var/db/mport/master.db", "SELECT count(*) FROM packages", "mport");
     }
+    if (FF_PACKAGES_IS_ENABLED(options, FLATPAK)) {
+        result->flatpakSystem += getFlatpakPackages(FASTFETCH_TARGET_DIR_ROOT "/var/lib");
+        const char* home = getenv("HOME");
+        if (home != nullptr && home[0] != '\0') {
+            char share[1024];
+            snprintf(share, sizeof(share), "%s/.local/share", home);
+            result->flatpakUser += getFlatpakPackages(share);
+        }
+    }
 }
