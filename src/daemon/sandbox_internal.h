#ifndef FPKG_SANDBOX_INTERNAL_H
#define FPKG_SANDBOX_INTERNAL_H

// Helpers shared by the sandbox translation units, not by the rest of the tree.
void close_inherited_fds(void);
int drm_devt_map(char* out, size_t outlen);
int jail_process_count(int jid);
// True when a binary in the launch tree is the store's to execute as root.
bool ldconfig_trusted(const char* path);

void sb_track(sandbox_t* sb, const char* fstype, const char* from,
	      const char* to);
int do_nmount(const char* fstype, const char* from, const char* to, char* opts,
	      bool ro);
int mount_tool(sandbox_t* sb, const char* tool, char* const args[],
	       const char* fstype, const char* from, const char* to);
int m_special(sandbox_t* sb, const char* fstype, const char* to);
int m_special_opts(sandbox_t* sb, const char* fstype, const char* to,
		   const char* opts);
void stage_dev_shm(sandbox_t* sb, const sandbox_plan_t* plan);

struct drm_version_local {
	int version_major, version_minor, version_patchlevel;
	unsigned long name_len;
	char* name;
	unsigned long date_len;
	char* date;
	unsigned long desc_len;
	char* desc;
};
#define DRM_IOCTL_VERSION_LOCAL _IOWR('d', 0x00, struct drm_version_local)


#define LD_SO_CACHE_KEEP 4

typedef struct {
	char path[PATH_MAX];
	time_t mtime;
} ld_cache_file_t;

// How long a launch waits for the runtime's ldconfig before it gives up.
#ifndef FLATPKG_LDCONFIG_TIMEOUT_S
#define FLATPKG_LDCONFIG_TIMEOUT_S 120
#endif

#endif
