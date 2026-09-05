#define _GNU_SOURCE

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAP_ENV "WAYLAND_DMABUF_DEVT_MAP"
#define TRACE_ENV "FLATPKG_DEVT_SHIM_TRACE"
#define MAX_ENTRIES 8

struct devt_entry {
	unsigned long long host;   // FreeBSD dev_t, as advertised
	unsigned long long client; // what the client's stat() sees
};

static struct devt_entry entries[MAX_ENTRIES];
static int n_entries = -1; // -1: not read yet

static void trace_translation(const char* what, unsigned long long from,
			      unsigned long long to) {
	if (getenv(TRACE_ENV) == NULL) return;
	fprintf(stderr, "devt-shim: %s(0x%llx) -> 0x%llx\n", what, from, to);
}

static pthread_once_t map_once = PTHREAD_ONCE_INIT;

static void load_map(void) {
	const char* v = getenv(MAP_ENV);
	const char* p;

	n_entries = 0;
	if (v == NULL) return;

	for (p = v; *p != '\0' && n_entries < MAX_ENTRIES;) {
		const char* sep = strchr(p, ';');
		size_t len = sep != NULL ? (size_t)(sep - p) : strlen(p);
		const char* colon = NULL;
		size_t i;

		for (i = 0; i < len; i++) {
			if (p[i] == ':') colon = p + i;
		}
		if (colon != NULL && colon != p) {
			char path[256];
			size_t plen = (size_t)(colon - p);
			struct stat st;

			if (plen < sizeof(path)) {
				memcpy(path, p, plen);
				path[plen] = '\0';
				if (stat(path, &st) == 0) {
					entries[n_entries].host =
						strtoull(colon + 1, NULL, 10);
					entries[n_entries].client =
						(unsigned long long)st.st_rdev;
					n_entries++;
				}
			}
		}
		if (sep == NULL) break;
		p = sep + 1;
	}
}

static unsigned long long translate(unsigned long long dev) {
	int i;

	pthread_once(&map_once, load_map);
	for (i = 0; i < n_entries; i++) {
		if (entries[i].host == dev &&
		    entries[i].host != entries[i].client) {
			trace_translation("dev_t", dev, entries[i].client);
			return entries[i].client;
		}
	}
	return dev;
}

// The intercepted entry points. The signatures are libdrm's, and dev_t is
// unsigned long on x86-64 glibc, so these are ABI-compatible without
// <xf86drm.h>.

typedef int (*get_device_from_devid_fn)(unsigned long, unsigned int, void**);
typedef int (*get_node_type_from_devid_fn)(unsigned long);

static void* real_symbol(const char* name) {
	static void* handle;
	void* p;

	p = dlsym(RTLD_NEXT, name);
	if (p != NULL) return p;
	if (handle == NULL)
		handle = dlopen("libdrm.so.2", RTLD_NOW | RTLD_NOLOAD);
	if (handle == NULL)
		handle = dlopen("libdrm.so.2", RTLD_NOW | RTLD_LOCAL);
	if (handle == NULL) return NULL;
	return dlsym(handle, name);
}

int drmGetDeviceFromDevId(unsigned long dev_id, unsigned int flags,
			  void** device) {
	static get_device_from_devid_fn real;
	get_device_from_devid_fn fn;

	if (real == NULL)
		real = (get_device_from_devid_fn)real_symbol(
			"drmGetDeviceFromDevId");
	fn = real;
	if (fn == NULL) return -38;
	return fn((unsigned long)translate(dev_id), flags, device);
}

int drmGetNodeTypeFromDevId(unsigned long dev_id) {
	static get_node_type_from_devid_fn real;
	get_node_type_from_devid_fn fn;

	if (real == NULL)
		real = (get_node_type_from_devid_fn)real_symbol(
			"drmGetNodeTypeFromDevId");
	fn = real;
	if (fn == NULL) return -38;
	return fn((unsigned long)translate(dev_id));
}
