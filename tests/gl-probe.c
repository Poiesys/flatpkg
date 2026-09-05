/*
 * gl-probe.c - what does EGL actually give us inside the sandbox?
 *
 * Prints, in one run:
 *   - whether the DRM dev_t shim is mapped into this process
 *   - each /dev/dri node's st_rdev (the Linux value this client sees;
 *     the compositor advertises the host's value instead)
 *   - EGL vendor/renderer/version for the surfaceless platform (does the
 *     hardware driver load at all?)
 *   - the same for the Wayland platform (the dmabuf-feedback path the
 *     shim exists for) and for X11 (the escape hatch, which matches
 *     devices by fd instead of dev_t)
 *
 * Everything is dlopen'd, so this links against nothing but libc and can
 * be cross-compiled with `zig cc --target=x86_64-linux-gnu`, then run
 * inside a sandbox via
 *   flatpak run --command=<path-inside-the-sandbox> APP
 *
 * Run it with WAYLAND_DMABUF_DEVT_MAP= to compare with the shim inert.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

typedef void *EGLDisplay;
typedef void *EGLSurface;
typedef void *EGLContext;
typedef void *EGLConfig;
typedef unsigned int EGLint;
typedef unsigned int EGLenum;
typedef unsigned int EGLBoolean;
typedef unsigned int GLenum;
typedef unsigned char GLubyte;

#define EGL_DEFAULT_DISPLAY 0
#define EGL_NO_DISPLAY NULL
#define EGL_NO_CONTEXT NULL
#define EGL_NO_SURFACE NULL
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_NONE 0x3038
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define EGL_CLIENT_APIS 0x308D
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_PLATFORM_WAYLAND_KHR 0x31D8
#define EGL_PLATFORM_X11_KHR 0x31D5
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_TEXTURE_2D 0x0DE1
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define EGL_GL_TEXTURE_2D_KHR 0x30B1
#define EGL_IMAGE_PRESERVED_KHR 0x30D2
#define EGL_TRUE 1
#define EGL_FALSE 0

typedef EGLDisplay (*fn_eglGetDisplay)(void *);
typedef EGLDisplay (*fn_eglGetPlatformDisplay)(EGLenum, void *, const EGLint *);
typedef EGLBoolean (*fn_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
typedef const char *(*fn_eglQueryString)(EGLDisplay, EGLint);
typedef EGLBoolean (*fn_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *,
    EGLint, EGLint *);
typedef EGLContext (*fn_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext,
    const EGLint *);
typedef EGLBoolean (*fn_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface,
    EGLContext);
typedef void *(*fn_eglGetProcAddress)(const char *);
typedef EGLBoolean (*fn_eglBindAPI)(EGLenum);
typedef const GLubyte *(*fn_glGetString)(GLenum);
typedef unsigned int GLuint;
typedef void (*fn_glGenTextures)(int, GLuint *);
typedef void (*fn_glBindTexture)(GLenum, GLuint);
typedef void (*fn_glTexImage2D)(GLenum, int, int, int, int, int, GLenum,
    GLenum, const void *);
typedef void (*fn_glDeleteTextures)(int, const GLuint *);
typedef void *(*fn_eglCreateImageKHR)(EGLDisplay, EGLContext, EGLenum,
    void *, const EGLint *);
typedef EGLBoolean (*fn_eglExportDMABUFImageQueryMESA)(EGLDisplay, void *,
    int *, int *, unsigned long long *);
typedef EGLBoolean (*fn_eglExportDMABUFImageMESA)(EGLDisplay, void *, int *,
    int *, int *);

static int
shim_mapped(void)
{
        FILE *fp = fopen("/proc/self/maps", "r");
        char line[1024];
        int found = 0;

        if (fp == NULL)
                return -1;
        while (fgets(line, sizeof(line), fp)) {
                if (strstr(line, "dmabuf_devt") != NULL) {
                        found = 1;
                        break;
                }
        }
        fclose(fp);
        return found;
}

static void
show_drm_nodes(void)
{
        DIR *d = opendir("/dev/dri");
        struct dirent *ent;

        if (d == NULL) {
                printf("drm nodes: /dev/dri missing\n");
                return;
        }
        while ((ent = readdir(d)) != NULL) {
                char path[256];
                struct stat st;

                if (ent->d_name[0] == '.')
                        continue;
                snprintf(path, sizeof(path), "/dev/dri/%s", ent->d_name);
                if (stat(path, &st) != 0)
                        continue;
                printf("drm node: %s st_rdev=%llu (0x%llx)\n", path,
                    (unsigned long long)st.st_rdev,
                    (unsigned long long)st.st_rdev);
        }
        closedir(d);
}

/* GTK4 (Bazaar, Fractal) picks its renderer with GSK: Vulkan when the
 * loader can bring up a device, GL otherwise.  Enumerating devices says
 * which driver is really behind a window. */
typedef struct {
        unsigned sType;
        const void *pNext;
        const char *pApplicationName;
        unsigned applicationVersion;
        const char *pEngineName;
        unsigned engineVersion;
        unsigned apiVersion;
} vk_app_info_t;

typedef struct {
        unsigned sType;
        const void *pNext;
        unsigned flags;
        const vk_app_info_t *pApplicationInfo;
        unsigned enabledLayerCount;
        const char *const *ppEnabledLayerNames;
        unsigned enabledExtensionCount;
        const char *const *ppEnabledExtensionNames;
} vk_instance_create_info_t;

typedef struct {
        unsigned apiVersion;
        unsigned driverVersion;
        unsigned vendorID;
        unsigned deviceID;
        unsigned deviceType;
        char deviceName[256];
} vk_phys_props_t;

static void
try_vulkan(void)
{
        typedef void *(*fn_get_instance_proc_addr)(void *, const char *);
        vk_app_info_t app_info;
        vk_instance_create_info_t create_info;
        vk_phys_props_t props;
        fn_get_instance_proc_addr get_proc;
        void *vk = dlopen("libvulkan.so.1", RTLD_NOW);
        void *inst = NULL;
        void *devs[8];
        int (*create_instance)(const vk_instance_create_info_t *, const void *,
            void **);
        int (*enumerate_devices)(void *, unsigned *, void **);
        void (*get_props)(void *, vk_phys_props_t *);
        unsigned n = 0, i;
        int rc;

        if (vk == NULL) {
                printf("vulkan: libvulkan.so.1: %s\n", dlerror());
                return;
        }
        get_proc = (fn_get_instance_proc_addr)dlsym(vk,
            "vkGetInstanceProcAddr");
        if (get_proc == NULL) {
                printf("vulkan: no vkGetInstanceProcAddr\n");
                return;
        }
        create_instance = (int (*)(const vk_instance_create_info_t *,
            const void *, void **))get_proc(NULL, "vkCreateInstance");
        if (create_instance == NULL) {
                printf("vulkan: loader has no vkCreateInstance\n");
                return;
        }

        memset(&app_info, 0, sizeof(app_info));
        app_info.sType = 0;                     /* APPLICATION_INFO */
        app_info.apiVersion = (1u << 22) | (1u << 12);  /* 1.1 */
        memset(&create_info, 0, sizeof(create_info));
        create_info.sType = 1;                  /* INSTANCE_CREATE_INFO */
        create_info.pApplicationInfo = &app_info;

        rc = create_instance(&create_info, NULL, &inst);
        if (rc != 0 || inst == NULL) {
                printf("vulkan: vkCreateInstance failed (VkResult %d)\n", rc);
                return;
        }
        /* Device enumeration is instance-level: it only resolves once the
         * instance exists. */
        enumerate_devices = (int (*)(void *, unsigned *, void **))
            get_proc(inst, "vkEnumeratePhysicalDevices");
        get_props = (void (*)(void *, vk_phys_props_t *))
            get_proc(inst, "vkGetPhysicalDeviceProperties");
        if (enumerate_devices == NULL || get_props == NULL) {
                printf("vulkan: no enumeration entry points\n");
                return;
        }
        if (enumerate_devices(inst, &n, NULL) != 0 || n == 0) {
                printf("vulkan: no physical devices\n");
                return;
        }
        if (n > 8)
                n = 8;
        if (enumerate_devices(inst, &n, devs) != 0) {
                printf("vulkan: enumerate failed\n");
                return;
        }
        for (i = 0; i < n; i++) {
                memset(&props, 0, sizeof(props));
                get_props(devs[i], &props);
                printf("vulkan: device[%u] name=%s vendor=0x%x "
                    "driverVersion=0x%x type=%u (1=integrated,2=discrete,"
                    "4=cpu)\n", i, props.deviceName, props.vendorID,
                    props.driverVersion, props.deviceType);
        }
}

/* One EGL attempt; returns 0 if a context came up and the GL strings were
 * read.  platform_name is only for the output. */
static void check_dmabuf_export(void *lib, EGLDisplay dpy, EGLContext ctx,
    fn_eglGetProcAddress get_proc, const char *platform_name);

static int
try_platform(void *lib, const char *platform_name, EGLenum platform,
    void *native_display)
{
        fn_eglGetPlatformDisplay get_platform;
        fn_eglGetDisplay get_display;
        fn_eglInitialize initialize;
        fn_eglQueryString query_string;
        fn_eglChooseConfig choose_config;
        fn_eglCreateContext create_context;
        fn_eglMakeCurrent make_current;
        fn_eglGetProcAddress get_proc;
        fn_eglBindAPI bind_api;
        fn_glGetString gl_get_string;
        EGLDisplay dpy;
        EGLint major = 0, minor = 0, nconfigs = 0;
        EGLConfig config = NULL;
        EGLContext ctx;
        const EGLint cfg_attrs[] = {
                EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                EGL_NONE
        };
        const EGLint ctx_attrs[] = {
                EGL_CONTEXT_CLIENT_VERSION, 2,
                EGL_NONE
        };

        get_platform = (fn_eglGetPlatformDisplay)dlsym(lib,
            "eglGetPlatformDisplay");
        get_display = (fn_eglGetDisplay)dlsym(lib, "eglGetDisplay");
        initialize = (fn_eglInitialize)dlsym(lib, "eglInitialize");
        query_string = (fn_eglQueryString)dlsym(lib, "eglQueryString");
        choose_config = (fn_eglChooseConfig)dlsym(lib, "eglChooseConfig");
        create_context = (fn_eglCreateContext)dlsym(lib, "eglCreateContext");
        make_current = (fn_eglMakeCurrent)dlsym(lib, "eglMakeCurrent");
        get_proc = (fn_eglGetProcAddress)dlsym(lib, "eglGetProcAddress");
        bind_api = (fn_eglBindAPI)dlsym(lib, "eglBindAPI");

        if (initialize == NULL || query_string == NULL) {
                printf("%s: libEGL has no eglInitialize\n", platform_name);
                return -1;
        }

        if (platform != 0 && get_platform != NULL)
                dpy = get_platform(platform, native_display, NULL);
        else
                dpy = get_display != NULL ? get_display(native_display) : NULL;
        if (dpy == EGL_NO_DISPLAY) {
                printf("%s: no EGL display\n", platform_name);
                return -1;
        }
        if (!initialize(dpy, &major, &minor)) {
                printf("%s: eglInitialize failed\n", platform_name);
                return -1;
        }
        printf("%s: EGL %d.%d vendor=%s version=%s\n", platform_name, major,
            minor, query_string(dpy, EGL_VENDOR),
            query_string(dpy, EGL_VERSION));

        if (bind_api != NULL)
                bind_api(EGL_OPENGL_ES_API);
        if (choose_config != NULL &&
            choose_config(dpy, cfg_attrs, &config, 1, &nconfigs) &&
            nconfigs > 0 && create_context != NULL) {
                ctx = create_context(dpy, config, EGL_NO_CONTEXT, ctx_attrs);
                if (ctx != EGL_NO_CONTEXT && make_current != NULL &&
                    make_current(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
                        gl_get_string = (fn_glGetString)get_proc("glGetString");
                        if (gl_get_string != NULL) {
                                printf("%s: GL_VENDOR=%s\n", platform_name,
                                    (const char *)gl_get_string(GL_VENDOR));
                                printf("%s: GL_RENDERER=%s\n", platform_name,
                                    (const char *)gl_get_string(GL_RENDERER));
                                printf("%s: GL_VERSION=%s\n", platform_name,
                                    (const char *)gl_get_string(GL_VERSION));
                        }
                        check_dmabuf_export(lib, dpy, ctx, get_proc,
                            platform_name);
                } else {
                        printf("%s: no GL context\n", platform_name);
                }
        } else {
                printf("%s: no EGL config\n", platform_name);
        }
        return 0;
}

/* A renderer string is not proof of zero-copy.  This is: allocate a GL
 * texture, export it as a dma-buf, and report the fd the driver handed
 * back.  That fd is what the compositor imports, and it is exactly what a
 * software rasterizer cannot produce. */
static void
check_dmabuf_export(void *lib, EGLDisplay dpy, EGLContext ctx,
    fn_eglGetProcAddress get_proc, const char *platform_name)
{
        static const EGLint img_attrs[] = {
                EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
                EGL_NONE
        };
        fn_eglCreateImageKHR create_image;
        fn_eglExportDMABUFImageQueryMESA query_export;
        fn_eglExportDMABUFImageMESA export_image;
        fn_glGenTextures gen_textures;
        fn_glBindTexture bind_texture;
        fn_glTexImage2D tex_image;
        fn_glDeleteTextures delete_textures;
        GLuint tex = 0;
        void *img;
        int fourcc = 0, nplanes = 0, fd = -1, stride = 0, offset = 0;
        unsigned long long modifier = 0;

        create_image = (fn_eglCreateImageKHR)dlsym(lib, "eglCreateImageKHR");
        if (create_image == NULL)
                create_image = (fn_eglCreateImageKHR)get_proc(
                    "eglCreateImageKHR");
        query_export = (fn_eglExportDMABUFImageQueryMESA)get_proc(
            "eglExportDMABUFImageQueryMESA");
        export_image = (fn_eglExportDMABUFImageMESA)get_proc(
            "eglExportDMABUFImageMESA");
        gen_textures = (fn_glGenTextures)get_proc("glGenTextures");
        bind_texture = (fn_glBindTexture)get_proc("glBindTexture");
        tex_image = (fn_glTexImage2D)get_proc("glTexImage2D");
        delete_textures = (fn_glDeleteTextures)get_proc("glDeleteTextures");

        if (create_image == NULL || query_export == NULL ||
            export_image == NULL || gen_textures == NULL ||
            bind_texture == NULL || tex_image == NULL) {
                printf("%s: dmabuf: no export extension\n", platform_name);
                return;
        }

        gen_textures(1, &tex);
        bind_texture(GL_TEXTURE_2D, tex);
        tex_image(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA,
            GL_UNSIGNED_BYTE, NULL);

        img = create_image(dpy, ctx, EGL_GL_TEXTURE_2D_KHR,
            (void *)(unsigned long)tex, img_attrs);
        if (img == NULL) {
                printf("%s: dmabuf: eglCreateImageKHR failed\n",
                    platform_name);
                return;
        }
        if (!query_export(dpy, img, &fourcc, &nplanes, &modifier) ||
            !export_image(dpy, img, &fd, &stride, &offset)) {
                printf("%s: dmabuf: export failed\n", platform_name);
                return;
        }
        printf("%s: dmabuf export fd=%d fourcc=0x%x planes=%d stride=%d "
            "modifier=0x%llx\n", platform_name, fd, fourcc, nplanes, stride,
            modifier);
        if (fd >= 0)
                close(fd);
        if (delete_textures != NULL)
                delete_textures(1, &tex);
}

/* Call libdrm directly, so the shim can be tested without Mesa in the
 * way: can it enumerate, and does drmGetDeviceFromDevId() accept the
 * FreeBSD dev_t (256) and the Linux one (57984 = 0xe280)? */
static void
try_libdrm(void)
{
        void *drm = dlopen("libdrm.so.2", RTLD_NOW);
        int (*get_devices2)(unsigned, void **, int);
        int (*get_from_devid)(dev_t, unsigned, void **);
        int (*get_node_type_from_devid)(dev_t) = NULL;
        int n;
        void *dev = NULL;
        int rc;
        const char *devid_scope = "none";

        if (drm == NULL) {
                printf("libdrm: %s\n", dlerror());
                return;
        }
        get_devices2 = (int (*)(unsigned, void **, int))
            dlsym(drm, "drmGetDevices2");
        /* Resolve through the global scope first: a preloaded shim
         * interposes this symbol for libEGL as well, so this is the call
         * Mesa makes.  A handle-specific dlsym would bypass the shim. */
        get_from_devid = (int (*)(dev_t, unsigned, void **))
            dlsym(RTLD_DEFAULT, "drmGetDeviceFromDevId");
        devid_scope = "via-preload";
        if (get_from_devid == NULL) {
                get_from_devid = (int (*)(dev_t, unsigned, void **))
                    dlsym(drm, "drmGetDeviceFromDevId");
                devid_scope = "via-dlopen";
        }
        if (get_devices2 == NULL || get_from_devid == NULL) {
                printf("libdrm: no enumeration symbols\n");
                return;
        }
        n = get_devices2(0, NULL, 0);
        printf("libdrm: drmGetDevices2 count=%d\n", n);

        /* The other entry point on this path (Mesa's
         * loader_get_driver_for_fd -> drmGetDevice2(fd)). */
        {
                int (*get_device2)(int, unsigned, void **) =
                    (int (*)(int, unsigned, void **))dlsym(drm,
                    "drmGetDevice2");
                char real[512];
                int fd;

                if (realpath("/sys/dev/char/226:128/device", real) == NULL)
                        snprintf(real, sizeof(real), "(realpath failed)");
                printf("libdrm: realpath(device)=%s\n", real);
                {
                        char up[600];
                        FILE *fp;

                        snprintf(up, sizeof(up), "%s/uevent", real);
                        fp = fopen(up, "r");
                        if (fp != NULL) {
                                char line[256];

                                printf("libdrm: device/uevent:");
                                while (fgets(line, sizeof(line), fp) != NULL) {
                                        char *nl = strchr(line, '\n');
                                        if (nl != NULL)
                                                *nl = '\0';
                                        printf(" [%s]", line);
                                }
                                printf("\n");
                                fclose(fp);
                        } else {
                                printf("libdrm: device/uevent: %s\n",
                                    strerror(errno));
                        }
                }
                {
                        int (*node_type_from_fd)(int) =
                            (int (*)(int))dlsym(drm, "drmGetNodeTypeFromFd");
                        DIR *dd = opendir("/dev/dri");
                        struct dirent *de;

                        printf("libdrm: readdir(/dev/dri):");
                        if (dd != NULL) {
                                while ((de = readdir(dd)) != NULL)
                                        printf(" [%s]", de->d_name);
                                closedir(dd);
                        }
                        printf("\n");
                        if (node_type_from_fd != NULL) {
                                int tfd = open("/dev/dri/renderD128",
                                    O_RDWR | O_CLOEXEC);

                                if (tfd >= 0) {
                                        printf("libdrm: "
                                            "drmGetNodeTypeFromFd(renderD128)"
                                            "=%d (2=render)\n",
                                            node_type_from_fd(tfd));
                                        close(tfd);
                                }
                        }
                }
                fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
                if (fd < 0) {
                        printf("libdrm: open renderD128: %s\n",
                            strerror(errno));
                } else if (get_device2 != NULL) {
                        void *d2 = NULL;

                        printf("libdrm: drmGetDevice2(fd) rc=%d\n",
                            get_device2(fd, 0, &d2));
                }
                if (fd >= 0)
                        close(fd);
        }

        rc = get_from_devid((dev_t)256, 0, &dev);
        printf("libdrm: drmGetDeviceFromDevId(256/0x100, host) rc=%d (%s)\n",
            rc, devid_scope);
        dev = NULL;
        rc = get_from_devid((dev_t)57984, 0, &dev);
        printf("libdrm: drmGetDeviceFromDevId(57984/0xe280, client) rc=%d\n",
            rc);
        /* Same lookup through the preload scope, which is what libEGL's
         * copy of libdrm sees: this is the call Mesa makes. */
        get_node_type_from_devid = (int (*)(dev_t))dlsym(RTLD_DEFAULT,
            "drmGetNodeTypeFromDevId");
        if (get_node_type_from_devid != NULL) {
                printf("libdrm: drmGetNodeTypeFromDevId(256/0x100) rc=%d "
                    "(2=render)\n", get_node_type_from_devid((dev_t)256));
                printf("libdrm: drmGetNodeTypeFromDevId(57984/0xe280) rc=%d\n",
                    get_node_type_from_devid((dev_t)57984));
        }
        printf("libdrm: trace env %s\n",
            getenv("FLATPKG_DEVT_SHIM_TRACE") != NULL ? "set" : "unset");
}

/* Walk the checks libdrm's process_device() makes for one node, so the
 * step that fails enumeration is visible instead of inferred. */
static void
check_sysfs(const char *node_name, unsigned maj, unsigned min)
{
        char node[256], path[512], buf[256];
        struct stat st;
        ssize_t n;

        printf("check %s (%u:%u):", node_name, maj, min);
        snprintf(node, sizeof(node), "/dev/dri/%s", node_name);
        if (stat(node, &st) != 0) {
                printf(" stat=%s", strerror(errno));
        } else {
                printf(" mode=%s rdev=%u:%u", S_ISCHR(st.st_mode) ? "chr" :
                    "other", (unsigned)((st.st_rdev >> 8) & 0xfff),
                    (unsigned)(st.st_rdev & 0xff));
        }
        snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device/subsystem",
            maj, min);
        n = readlink(path, buf, sizeof(buf) - 1);
        if (n < 0)
                printf(" subsystem=ENOENT");
        else {
                buf[n] = '\0';
                printf(" subsystem->%s", strrchr(buf, '/') != NULL ?
                    strrchr(buf, '/') + 1 : buf);
        }
        static const char *keys[] = { "vendor", "device", "subsystem_vendor",
            "subsystem_device", "uevent" };
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
                FILE *fp;

                snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/device/%s",
                    maj, min, keys[i]);
                fp = fopen(path, "r");
                if (fp == NULL) {
                        printf(" %s=ENOENT", keys[i]);
                        continue;
                }
                buf[0] = '\0';
                if (fgets(buf, sizeof(buf), fp) != NULL) {
                        char *nl = strchr(buf, '\n');
                        if (nl != NULL)
                                *nl = '\0';
                        printf(" %s=%s", keys[i], buf);
                }
                fclose(fp);
        }
        printf("\n");
}

int
main(void)
{
        void *lib, *wl;
        void *wl_display = NULL;
        const char *wd = getenv("WAYLAND_DISPLAY");
        int shim = shim_mapped();

        printf("shim mapped: %s\n", shim == 1 ? "yes" :
            (shim == 0 ? "no" : "unknown"));
        printf("WAYLAND_DMABUF_DEVT_MAP=%s\n",
            getenv("WAYLAND_DMABUF_DEVT_MAP") != NULL ?
            getenv("WAYLAND_DMABUF_DEVT_MAP") : "(unset)");
        show_drm_nodes();

        check_sysfs("renderD128", 226, 128);
        check_sysfs("card0", 226, 0);
        try_libdrm();
        try_vulkan();

        lib = dlopen("libEGL.so.1", RTLD_NOW);
        if (lib == NULL) {
                printf("libEGL.so.1: %s\n", dlerror());
                return 1;
        }

        try_platform(lib, "surfaceless", EGL_PLATFORM_SURFACELESS_MESA, NULL);

        /* The Wayland platform needs a wl_display; libwayland-client is in
         * the runtime, and only its connect symbol is used here. */
        if (wd != NULL && *wd != '\0') {
                wl = dlopen("libwayland-client.so.0", RTLD_NOW);
                if (wl == NULL)
                        printf("wayland: libwayland-client.so.0: %s\n",
                            dlerror());
                else {
                        void *(*connect_dpy)(const char *) =
                            (void *(*)(const char *))dlsym(wl,
                            "wl_display_connect");
                        if (connect_dpy != NULL)
                                wl_display = connect_dpy(NULL);
                        if (wl_display == NULL)
                                printf("wayland: cannot connect to the "
                                    "compositor\n");
                        else
                                try_platform(lib, "wayland",
                                    EGL_PLATFORM_WAYLAND_KHR, wl_display);
                }
        } else {
                printf("wayland: WAYLAND_DISPLAY unset\n");
        }

        /* X11 too: XWayland is the escape hatch apps are told to use, and
         * its EGL path matches devices by fd rather than by dev_t, so it
         * should be hardware whether or not the shim is loaded. */
        {
                void *x11 = dlopen("libX11.so.6", RTLD_NOW);

                if (x11 == NULL) {
                        printf("x11: libX11.so.6: %s\n", dlerror());
                } else {
                        void *(*open_display)(const char *) =
                            (void *(*)(const char *))dlsym(x11,
                            "XOpenDisplay");
                        void *xdpy = NULL;

                        if (open_display != NULL)
                                xdpy = open_display(NULL);
                        if (xdpy == NULL)
                                printf("x11: cannot open DISPLAY=%s\n",
                                    getenv("DISPLAY") != NULL ?
                                    getenv("DISPLAY") : "(unset)");
                        else
                                try_platform(lib, "x11", EGL_PLATFORM_X11_KHR,
                                    xdpy);
                }
        }
        return 0;
}
