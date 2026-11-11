#include <assert.h>
#include <dirent.h>
#include <jail.h>
#include <libutil.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/jail.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/procctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <termios.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <libgen.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

// The ABI baseline the Flatpak runtimes are built against; there is no
// per-runtime kernel version to read, so the Linuxulator is told this.
#define FLATPKG_LINUX_OSRELEASE "5.15.0"

#include "flatpkg.h"
#define FPKG_MODULE "sandbox"
#include "sandbox_internal.h"

#include "fpkglog.h"

static int m_devfs(sandbox_t* sb, const char* to, int ruleset) {
	assert(sb != NULL);
	assert(to != NULL);
	char opts[32] = "";
	if (ruleset > 0) snprintf(opts, sizeof(opts), "ruleset=%d", ruleset);
	if (do_nmount("devfs", "devfs", to, opts[0] ? opts : NULL, false) ==
	    0) {
		sb_track(sb, "devfs", NULL, to);
		return 0;
	}
	if (ruleset > 0) {
		char rs[16];
		snprintf(rs, sizeof(rs), "ruleset=%d", ruleset);
		char* args[] = {"-t",	 "devfs",   "-o", rs,
				"devfs", (char*)to, NULL};
		return mount_tool(sb, "/sbin/mount", args, "devfs", NULL, to);
	}
	char* args[] = {"-t", "devfs", "devfs", (char*)to, NULL};
	return mount_tool(sb, "/sbin/mount", args, "devfs", NULL, to);
}

static int devfs_rule(int ruleset, int out_fd, int err_fd, const char* w1,
		      const char* w2, const char* w3, const char* w4) {
	char num[16];
	const char* words[4] = {w1, w2, w3, w4};
	char* argv[9];
	int n = 0;

	snprintf(num, sizeof(num), "%d", ruleset);
	argv[n++] = (char*)"/sbin/devfs";
	argv[n++] = (char*)"rule";
	argv[n++] = (char*)"-s";
	argv[n++] = num;
	for (int i = 0; i < 4 && words[i] != NULL; i++)
		argv[n++] = (char*)words[i];
	argv[n] = NULL;
	return run_program(argv, -1, out_fd, err_fd);
}

static void devfs_install_set(int set, bool include_base, bool hide,
			      const char* const* paths) {
	char base[16];
	int devnull = open("/dev/null", O_WRONLY);

	snprintf(base, sizeof(base), "%d", FLATPKG_RULESET_BASE);
	// The delset is expected to fail when the set does not exist.
	(void)devfs_rule(set, -1, devnull, "delset", NULL, NULL, NULL);
	if (include_base &&
	    devfs_rule(set, -1, devnull, "add", "include", base, NULL) != 0)
		fpkg_warnf("devfs ruleset %d: cannot include base ruleset", set);
	if (hide && devfs_rule(set, -1, devnull, "add", "hide", NULL, NULL) != 0)
		fpkg_warnf("devfs ruleset %d: cannot add hide rule", set);
	for (size_t i = 0; paths[i] != NULL; i++)
		if (devfs_rule(set, -1, devnull, "add", "path", paths[i],
			       "unhide") != 0)
			fpkg_warnf("devfs ruleset %d: cannot unhide '%s'", set,
				   paths[i]);
	if (devnull >= 0) close(devnull);
}

static const char* const devfs_base_paths[] = {
	"null", "zero", "random", "urandom", "shm/*", "pts/*", "ptmx",
	"tty", "fd", "fd/*", "stdin", "stdout", "stderr", NULL};
static const char* const devfs_dri_paths[] = {
	"dri", "dri/*", "drm", "drm/*", NULL};
static const char* const devfs_vmm_paths[] = {"vmm", "vmm/*", NULL};
static const char* const devfs_input_paths[] = {"input", "input/*", NULL};

void install_devfs_base_rulesets(void) {
	devfs_install_set(FLATPKG_RULESET_BASE, false, true, devfs_base_paths);
	devfs_install_set(FLATPKG_RULESET_BASE + 1, true, false,
			  devfs_dri_paths);
	devfs_install_set(FLATPKG_RULESET_BASE + 2, true, false,
			  devfs_vmm_paths);
	devfs_install_set(FLATPKG_RULESET_BASE + 3, true, false,
			  devfs_input_paths);
}

static int alloc_devfs_ruleset(const sandbox_plan_t* plan) {
	assert(plan != NULL);
	if (plan->dev_all) return 0;

	int ruleset = -1;
	int devnull = open("/dev/null", O_WRONLY);
	char base[16];

	snprintf(base, sizeof(base), "%d", FLATPKG_RULESET_BASE);
	for (int attempt = 0; attempt < 2 && ruleset < 0; attempt++) {
		int lockfd = open("/var/run/flatpkg/rulesets.lock",
				  O_CREAT | O_RDWR, 0600);
		if (lockfd >= 0) flock(lockfd, LOCK_EX);

		for (int n = FLATPKG_RULESET_MIN; n <= FLATPKG_RULESET_MAX;
		     n++) {
			if (devfs_rule(n, devnull, devnull, "show", NULL, NULL,
				       NULL) != 0)
				continue;
			// A stale set from a crashed launch is reused.
			(void)devfs_rule(n, -1, devnull, "delset", NULL, NULL,
					 NULL);
			if (devfs_rule(n, -1, devnull, "add", "include", base,
				       NULL) == 0) {
				ruleset = n;
				break;
			}
		}
		close(lockfd);

		if (ruleset < 0 && attempt == 0) {
			fpkg_warnf("base devfs ruleset %d missing - "
				   "re-installing (reboot wiped in-kernel "
				   "rulesets?)",
				   FLATPKG_RULESET_BASE);
			install_devfs_base_rulesets();
		}
	}

	if (ruleset < 0) {
		fpkg_errorf("no free devfs ruleset number (min %d max %d)",
			    FLATPKG_RULESET_MIN, FLATPKG_RULESET_MAX);
		if (devnull >= 0) close(devnull);
		return -1;
	}

	bool on_dri = plan->dev_dri, on_kvm = plan->dev_kvm,
	     on_input = plan->dev_input, on_usb = plan->dev_usb,
	     on_shm = plan->dev_shm,
	     on_snd = plan->dev_snd || plan->sock_pulseaudio;
	const struct {
		bool on;
		const char* path;
	} rules[] = {
		{true, "shm"},	     {on_dri, "dri"},	    {on_dri, "dri/*"},
		{on_dri, "drm"},     {on_dri, "drm/*"},	    {on_dri, "udmabuf"},
		{on_dri, "kfd"},     {on_kvm, "vmm"},	    {on_kvm, "vmm/*"},
		{on_input, "input"}, {on_input, "input/*"}, {on_usb, "usb"},
		{on_usb, "usb/*"},   {on_usb, "ugen*"},	    {on_shm, "shm"},
		{on_shm, "shm/*"},   {on_snd, "snd"},	    {on_snd, "snd/*"},
		{on_snd, "dsp*"},    {on_snd, "mixer*"},    {on_snd, "midi*"},
	};

	for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
		if (!rules[i].on) continue;
		if (devfs_rule(ruleset, -1, devnull, "add", "path",
			       rules[i].path, "unhide") != 0)
			fpkg_warnf("devfs ruleset %d: cannot unhide '%s'",
				   ruleset, rules[i].path);
	}
	if (devnull >= 0) close(devnull);
	return ruleset;
}

static void chown_dev_nodes(sandbox_t* sb, const sandbox_plan_t* plan) {
	assert(sb != NULL);
	assert(plan != NULL);
	static const char* const subdirs[] = {"dri", "drm", "snd", "input",
					      NULL};
	for (size_t i = 0; subdirs[i] != NULL; i++) {
		char dir[PATH_MAX];
		snprintf(dir, sizeof(dir), "%s/dev/%s", sb->jailroot,
			 subdirs[i]);
		DIR* d = opendir(dir);
		if (d == NULL) continue;
		int dfd = dirfd(d);
		if (dfd < 0) {
			closedir(d);
			continue;
		}
		struct dirent* de;
		while ((de = readdir(d)) != NULL) {
			if (de->d_name[0] == '.') continue;
			char node[PATH_MAX];
			snprintf(node, sizeof(node), "%s/%s", dir, de->d_name);
			struct stat st;
			// Both operations are relative to the open directory and
			// neither follows a symlink, so a swapped path cannot
			// redirect the chown.
			if (fstatat(dfd, de->d_name, &st,
				    AT_SYMLINK_NOFOLLOW) != 0 ||
			    !(S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)))
				continue;
			if (fchownat(dfd, de->d_name, (uid_t)-1, plan->gid,
				     AT_SYMLINK_NOFOLLOW) != 0)
				fpkg_warnf("chown %s: %s", node,
					   strerror(errno));
		}
		closedir(d);
	}
}

// The per-launch tree is the app's: everything flatpkg created goes to the
// caller, who is who the app runs as.  A bind-mounted path keeps the source's
// owner.  The walk holds the directory open, so the entry that is chowned is
// the entry that is descended into and there is no path to swap underneath.
static void own_tree_dir_at(sandbox_t* sb, uid_t uid, gid_t gid, int fd,
			    const char* dir) {
	assert(sb != NULL);
	assert(dir != NULL);
	DIR* d = fdopendir(fd);
	if (d == NULL) {
		close(fd);
		return;
	}
	int dfd = dirfd(d);
	if (dfd < 0) {
		closedir(d);
		return;
	}
	struct dirent* de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.' &&
		    (de->d_name[1] == '\0' ||
		     (de->d_name[1] == '.' && de->d_name[2] == '\0')))
			continue;
		char child[PATH_MAX];
		snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
		bool bound = false;
		for (size_t i = 0; i < sb->nmounts; i++) {
			if (sb->mounts[i].to == NULL ||
			    strcmp(sb->mounts[i].to, child) != 0)
				continue;
			bound = strcmp(sb->mounts[i].fstype, "tmpfs") != 0;
		}
		if (bound) continue;
		int cfd = openat(dfd, de->d_name,
				 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (cfd < 0) continue;
		if (fchown(cfd, uid, gid) != 0) {
			fpkg_debugf("chown %s: %s", child, strerror(errno));
			close(cfd);
			continue;
		}
		own_tree_dir_at(sb, uid, gid, cfd, child);
	}
	closedir(d);
}

static void own_tree_dir(sandbox_t* sb, uid_t uid, gid_t gid, const char* dir) {
	assert(sb != NULL);
	assert(dir != NULL);
	int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd >= 0) own_tree_dir_at(sb, uid, gid, fd, dir);
}

// The one handover point, not part of jail creation: the proxies' mount
// points and the instance files are root writes that come after that, and
// every one of them has to precede this.  Nothing root writes by path may
// follow it; the pid file holds the instance directory open for that reason.
void sandbox_own_tree(sandbox_t* sb, const sandbox_plan_t* plan) {
	assert(sb != NULL);
	assert(plan != NULL);
	if (sb->jailroot == NULL) return;
	own_tree_dir(sb, plan->uid, plan->gid, sb->jailroot);
}

int drm_devt_map(char* out, size_t outlen) {
	assert(out != NULL);
	assert(outlen > 0);
	static const char* names[] = {"card0", "card1", "renderD128",
				      "renderD129"};
	size_t off = 0;

	out[0] = '\0';
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char path[PATH_MAX];
		struct stat st;

		snprintf(path, sizeof(path), "/dev/dri/%s", names[i]);
		if (stat(path, &st) != 0) continue;
		// snprintf's return is what it would have written, so adding it to
		// an offset underflows on a short buffer; refuse the entry whole.
		if (!fpkg_buf_append(out, outlen, &off, "%s%s:%llu",
				     off > 0 ? ";" : "", path,
				     (unsigned long long)st.st_rdev)) {
			out[off] = '\0';
			break;
		}
	}
	return 0;
}

int sandbox_create_jail(sandbox_t* sb, const fp_metadata_t* meta,
			const sandbox_plan_t* plan) {
	assert(sb != NULL);
	assert(meta != NULL);
	sb->devfs_ruleset = alloc_devfs_ruleset(plan);
	if (sb->devfs_ruleset < 0) return -1;

	struct jailparam params[24];
	int nparams = 0;
#define ADDP(name, val)                                                        \
	do {                                                                   \
		jailparam_init(&params[nparams], name);                        \
		if (jailparam_import(&params[nparams], val) != 0) {            \
			fpkg_errorf("jailparam %s: %s", name, jail_errmsg);    \
			goto fail;                                             \
		}                                                              \
		nparams++;                                                     \
	} while (0)

	ADDP("path", sb->jailroot);

	char jname[256];
	snprintf(jname, sizeof(jname), FLATPKG_JAIL_NAME_PREFIX "%s-%ld",
		 sb->app_id, (long)getpid());
	for (char* c = jname; *c; c++)
		if (*c == '.') *c = '_';
	ADDP("name", jname);

	ADDP("host.hostname", sb->app_id);

	ADDP("persist", "1");

	ADDP("allow.mount", "1");
	ADDP("allow.mount.devfs", "1");
	ADDP("allow.mount.procfs", "1");
	ADDP("allow.mount.fdescfs", "1");

	{
		int present;
		size_t len = sizeof(present);

		if (sysctlbyname("security.jail.param.allow.mlock", &present,
				 &len, NULL, 0) == 0)
			ADDP("allow.mlock", "1");
	}

	// Who the jail belongs to, held by the kernel with the jail and so not
	// something a user can edit as they could their own runtime dir.  enter
	// and kill act only for this uid, or root; no owner means root only.
	{
		// Only whether the parameter exists: meta is a string, and
		// reading it into an int fails with ENOMEM, which is not
		// "absent".
		size_t len = 0;

		if (sysctlbyname("security.jail.param.meta", NULL, &len, NULL,
				 0) == 0) {
			char owner[64];
			snprintf(owner, sizeof(owner),
				 FLATPKG_JAIL_OWNER_KEY "=%lu",
				 (unsigned long)plan->uid);
			ADDP("meta", owner);
		}
	}

	ADDP("enforce_statfs", "1");

	ADDP("sysvmsg", plan->want_ipc ? "inherit" : "new");
	ADDP("sysvsem", plan->want_ipc ? "inherit" : "new");
	ADDP("sysvshm", plan->want_ipc ? "inherit" : "new");

	if (plan->want_network) {
		ADDP("ip4", "inherit");
		ADDP("ip6", "inherit");
	} else {
		ADDP("vnet", "new");
	}

	ADDP("linux", "new");
	ADDP("linux.osname", "Linux");
	ADDP("linux.osrelease", FLATPKG_LINUX_OSRELEASE);

	ADDP("children.max", "1024");

	if (sb->devfs_ruleset > 0) {
		char rs[16];
		snprintf(rs, sizeof(rs), "%d", sb->devfs_ruleset);
		ADDP("devfs_ruleset", rs);
	}

	sb->jid = jailparam_set(params, nparams, JAIL_CREATE);
	for (int i = 0; i < nparams; i++) jailparam_free(&params[i], 1);

	if (sb->jid < 0) {
		fpkg_errorf("jailparam_set: %s", jail_errmsg);
		return -1;
	}

	{
		char devdir[PATH_MAX];
		snprintf(devdir, sizeof(devdir), "%s/dev", sb->jailroot);
		if (m_devfs(sb, devdir, sb->devfs_ruleset) != 0) {
			fpkg_warnf("devfs mount failed; /dev will be empty");
		} else {
			char fddir[PATH_MAX];
			snprintf(fddir, sizeof(fddir), "%s/dev/fd",
				 sb->jailroot);
			if (m_special_opts(sb, "fdescfs", fddir, "linrdlnk") !=
			    0)
				m_special(sb, "fdescfs", fddir);
			chown_dev_nodes(sb, plan);
		}
	}

	stage_dev_shm(sb, plan);

	return 0;

fail:
	for (int i = 0; i < nparams; i++) jailparam_free(&params[i], 1);
	return -1;
#undef ADDP
}

#ifdef FLATPKG_TESTING
// A test build can fake the mount table and force unmount failures; the
// daemon never defines this.  Not an environment switch on purpose.
extern int fpkg_test_getmntinfo(struct statfs** mntbuf, int flags);
extern int fpkg_test_unmount(const char* dir, int flags);
#define getmntinfo fpkg_test_getmntinfo
#define unmount fpkg_test_unmount
#endif

typedef struct {
	char path[PATH_MAX];
	// unmount(2) takes an fsid in decimal: "FSID:<val0>:<val1>".  The id
	// names the mounted filesystem itself, so a path the app can swap cannot
	// redirect the unmount, and a mount point that has been unlinked still has
	// a handle.
	char fsid[64];
} mount_ent_t;

typedef struct {
	mount_ent_t* ent;
	size_t n;
} mount_list_t;

static void mount_list_free(mount_list_t* l) {
	free(l->ent);
	l->ent = NULL;
	l->n = 0;
}

static void fsid_of(const struct statfs* m, char* out, size_t len) {
	assert(m != NULL);
	assert(out != NULL);
	snprintf(out, len, "FSID:%d:%d", (int)m->f_fsid.val[0],
		 (int)m->f_fsid.val[1]);
}

// Mount points at or under `root`.  The list is sized from the mount table,
// not a fixed cap: a heavy app can stage more mounts than any guess.
static int mounts_under(const char* root, mount_list_t* l) {
	struct statfs* mnts = NULL;
	int total = getmntinfo(&mnts, MNT_NOWAIT);
	size_t rl = strlen(root);

	// A previous scan's list is dead here; overwriting it would leak it.
	mount_list_free(l);
	if (total < 0) return -1;
	if (total == 0) return 0;
	l->ent = calloc((size_t)total, sizeof(l->ent[0]));
	if (l->ent == NULL) return -1;
	for (int i = 0; i < total; i++) {
		const char* at = mnts[i].f_mntonname;

		if (strncmp(at, root, rl) != 0) continue;
		if (at[rl] != '\0' && at[rl] != '/') continue;
		snprintf(l->ent[l->n].path, PATH_MAX, "%s", at);
		fsid_of(&mnts[i], l->ent[l->n].fsid, sizeof(l->ent[l->n].fsid));
		l->n++;
	}
	return 0;
}

// Deepest first, so a filesystem mounted on top of another is gone before the
// one below it is asked to go.
static void sort_deepest_first(mount_list_t* l) {
	for (size_t i = 1; i < l->n; i++) {
		mount_ent_t key = l->ent[i];
		size_t j = i;

		while (j > 0 && strlen(l->ent[j - 1].path) < strlen(key.path)) {
			l->ent[j] = l->ent[j - 1];
			j--;
		}
		l->ent[j] = key;
	}
}

static bool unmount_entry(const mount_ent_t* e) {
	assert(e != NULL);
	if (unmount(e->fsid, MNT_BYFSID) == 0) return true;
	if (unmount(e->fsid, MNT_BYFSID | MNT_FORCE) == 0) return true;
	return false;
}

// Unmount whatever is at `path`, looked up by name and taken down by id.  A
// path with nothing mounted at it is not a failure: the launch may have taken
// it down itself.
static bool unmount_at(const char* path) {
	struct statfs* mnts = NULL;
	int total = getmntinfo(&mnts, MNT_NOWAIT);

	if (total <= 0) return false;
	for (int i = 0; i < total; i++) {
		mount_ent_t e;

		if (strcmp(mnts[i].f_mntonname, path) != 0) continue;
		snprintf(e.path, sizeof(e.path), "%s", path);
		fsid_of(&mnts[i], e.fsid, sizeof(e.fsid));
		return unmount_entry(&e);
	}
	return true;
}

// Remove a jail root, and only when it really is one: canonicalized under the
// run dir, with every filesystem mounted at or under it gone first.  A tree
// that still has a mount is left in place - rm_rf's FTW_MOUNT skips a mount
// point, it does not keep the walk from reaching one through a swapped path.
#ifdef FLATPKG_TESTING
int sandbox_remove_jail_root(const char* root) {
#else
static int sandbox_remove_jail_root(const char* root) {
#endif
	char rroot[PATH_MAX], rrun[PATH_MAX];
	mount_list_t l = {0};
	size_t rl;

	if (realpath(root, rroot) == NULL) {
		if (errno == ENOENT) return 0;
		fpkg_errorf("cannot resolve %s: %s", root, strerror(errno));
		return -1;
	}
	if (realpath(FLATPKG_RUN_DIR, rrun) == NULL) {
		fpkg_errorf("cannot resolve %s: %s", FLATPKG_RUN_DIR,
			    strerror(errno));
		return -1;
	}
	rl = strlen(rrun);
	if (strncmp(rroot, rrun, rl) != 0 || rroot[rl] != '/' ||
	    rroot[rl + 1] == '\0' || strchr(rroot + rl + 1, '/') != NULL) {
		fpkg_errorf("not removing %s: it is not one directory under %s",
			    rroot, rrun);
		return -1;
	}
	for (int pass = 0; pass < 3; pass++) {
		if (mounts_under(rroot, &l) != 0) {
			mount_list_free(&l);
			fpkg_errorf("cannot read the mount table");
			return -1;
		}
		if (l.n == 0) break;
		sort_deepest_first(&l);
		for (size_t i = 0; i < l.n; i++) {
			if (unmount_entry(&l.ent[i])) continue;
			fpkg_warnf("not unmounting %s", l.ent[i].path);
		}
		mount_list_free(&l);
	}
	if (mounts_under(rroot, &l) != 0 || l.n > 0) {
		fpkg_warnf("not removing %s: %zu filesystem(s) still mounted "
			   "under it",
			   rroot, l.n);
		mount_list_free(&l);
		return -1;
	}
	mount_list_free(&l);
	if (rm_rf(rroot) != 0) {
		fpkg_warnf("cannot remove %s: %s", rroot, strerror(errno));
		return -1;
	}
	return 0;
}

// The jail rooted at `path`, if any.  `ours` says flatpkg made it: its name
// carries the prefix, so a third party's jail of the same shape is not a
// launch of ours to clean up.
static int jail_for_path(const char* path, bool* ours) {
	char jidstr[16] = "0";
	char jpath[PATH_MAX], jname[MAXHOSTNAMELEN];

	*ours = false;
	for (;;) {
		int jid = jail_getv(0, "lastjid", jidstr, NULL);

		if (jid <= 0) return 0;
		snprintf(jidstr, sizeof(jidstr), "%d", jid);
		jpath[0] = jname[0] = '\0';
		if (jail_getv(0, "jid", jidstr, "path", jpath, "name", jname,
			      NULL) != jid)
			continue;
		{
			char rpath[PATH_MAX];

			if (realpath(jpath, rpath) == NULL ||
			    strcmp(rpath, path) != 0)
				continue;
		}
		*ours = strncmp(jname, FLATPKG_JAIL_NAME_PREFIX,
				sizeof(FLATPKG_JAIL_NAME_PREFIX) - 1) == 0;
		return jid;
	}
}

// A jail root's name is <app id>.<six random characters> from mkdtemp().
static bool jail_root_name(const char* name) {
	const char* dot = strrchr(name, '.');

	if (dot == NULL || dot == name || strlen(dot + 1) != 6) return false;
	for (const char* p = dot + 1; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9')))
			return false;
	return true;
}

// The worker holds this from mkdtemp() until teardown; a root still locked is
// a launch in progress, not a leak.  An unreadable lock is skipped, not
// deleted: the sweep only removes what it can prove is unheld.
#ifdef FLATPKG_TESTING
bool root_is_held(const char* dir) {
#else
static bool root_is_held(const char* dir) {
#endif
	char path[PATH_MAX];
	struct stat st;
	int fd, e;

	snprintf(path, sizeof(path), "%s/.lock", dir);
	// Judge the opened file, not the name: a symlink swapped in between an
	// lstat and the open would be followed.  O_NOFOLLOW makes that ELOOP,
	// and O_NONBLOCK keeps a fifo from blocking the open before fstat.
	fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) return errno != ENOENT;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return true;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
		close(fd);
		return false;
	}
	e = errno;
	close(fd);
	return e == EWOULDBLOCK;
}

// Jail roots left behind by a worker that died between mkdtemp and teardown.
// Only trees no worker still holds are touched; a jail still serving them is
// left alone, and one of ours with nothing in it is dropped so its tree can
// go.  The removal goes through the same guard as a normal teardown.
void sandbox_gc_leaks(void) {
	DIR* d = opendir(FLATPKG_RUN_DIR);

	if (d == NULL) return;
	struct dirent* e;
	while ((e = readdir(d)) != NULL) {
		char path[PATH_MAX], rpath[PATH_MAX];
		struct stat st;
		bool ours;
		int jid;

		if (!jail_root_name(e->d_name)) continue;
		snprintf(path, sizeof(path), "%s/%s", FLATPKG_RUN_DIR,
			 e->d_name);
		if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
		if (realpath(path, rpath) == NULL) continue;
		if (root_is_held(rpath)) continue;
		jid = jail_for_path(rpath, &ours);
		if (jid > 0) {
			if (!ours || jail_process_count(jid) > 0) continue;
			if (jail_remove(jid) != 0) continue;
			fpkg_infof("removed abandoned jail %d", jid);
		}
		if (sandbox_remove_jail_root(rpath) == 0)
			fpkg_infof("removed leaked sandbox %s", rpath);
		else
			fpkg_warnf("leaked sandbox left at %s", rpath);
	}
	closedir(d);
}

void sandbox_teardown(sandbox_t* sb) {
	assert(sb != NULL);
	if (cli_getenv("FLATPKG_DEBUG_KEEP") != NULL) {
		fpkg_infof("DEBUG_KEEP: jail id %d root %s", sb->jid,
			   sb->jailroot ? sb->jailroot : "?");
		return;
	}

	dbus_stop_proxies(sb);

	if (sb->jid > 0) {
		jail_remove(sb->jid);
		sb->jid = 0;
	}

	// A tracked mount outside the jail root is not in the scan's reach.
	if (sb->jailroot) {
		size_t rl = strlen(sb->jailroot);

		for (size_t i = 0; i < sb->nmounts; i++) {
			sb_mount_t* m = &sb->mounts[i];

			if (m->to == NULL) continue;
			if (strncmp(m->to, sb->jailroot, rl) == 0 &&
			    (m->to[rl] == '\0' || m->to[rl] == '/'))
				continue;
			if (!unmount_at(m->to))
				fpkg_warnf("still mounted after teardown: %s",
					   m->to);
		}
		if (sandbox_remove_jail_root(sb->jailroot) != 0)
			fpkg_warnf("sandbox root left at %s", sb->jailroot);
	}
	if (sb->root_lock_fd >= 0) {
		close(sb->root_lock_fd);
		sb->root_lock_fd = -1;
	}

	if (sb->devfs_ruleset > 0) {
		int devnull = open("/dev/null", O_WRONLY);

		(void)devfs_rule(sb->devfs_ruleset, -1, devnull, "delset",
				 NULL, NULL, NULL);
		if (devnull >= 0) close(devnull);
		sb->devfs_ruleset = 0;
	}

	for (size_t i = 0; i < sb->nmounts; i++) {
		free(sb->mounts[i].fstype);
		free(sb->mounts[i].from);
		free(sb->mounts[i].to);
	}
	free(sb->mounts);
	sb->mounts = NULL;
	sb->nmounts = 0;

	fp_extensions_free(sb);

	free(sb->p11kit_socket_name);
	sb->p11kit_socket_name = NULL;
	free(sb->jailroot);
	sb->jailroot = NULL;
}
