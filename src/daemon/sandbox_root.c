#include <assert.h>
#include <dirent.h>
#include <ftw.h>
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

#include "flatpkg.h"
#define FPKG_MODULE "sandbox"
#include "fpkglog.h"
#include "sandbox_internal.h"

static const char* usrmerged_dirs[] = {"bin",  "lib",	"lib64",
				       "sbin", "lib32", NULL};

// The host's Wayland display name: the caller's value when usable, else the
// runtime dir's own wayland-<digits> socket (a leading '/' is a path).
static void host_wayland_display(const sandbox_plan_t* plan, char* out,
				 size_t n) {
	assert(out != NULL);
	assert(n > 0);
	char* nm = fp_wayland_display_name(plan ? plan->xdg_runtime_dir : NULL);
	snprintf(out, n, "%s", nm ? nm : "wayland-0");
	free(nm);
}

// Copy `from` into a new `to` owned by uid:gid.  O_EXCL makes "the caller
// wanted this file created" one decision: an existing file is left alone, and
// the owner is set through the descriptor rather than a second name lookup.
static fpkg_error_t copy_file_owned(const char* from, const char* to,
				    mode_t mode, uid_t uid, gid_t gid) {
	int in = open(from, O_RDONLY);
	if (in < 0) return fpkg_error_from_errno(errno);
	int out = open(to, O_WRONLY | O_CREAT | O_EXCL, mode);
	if (out < 0) {
		int e = errno;
		close(in);
		errno = e;
		return fpkg_error_from_errno(e);
	}
	char buf[16384];
	int e = 0;
	while (e == 0) {
		ssize_t n = read(in, buf, sizeof(buf));
		if (n == 0) break;
		if (n < 0) {
			if (errno == EINTR) continue;
			e = errno;
			break;
		}
		for (ssize_t off = 0; off < n;) {
			ssize_t w = write(out, buf + off, (size_t)(n - off));
			if (w < 0) {
				if (errno == EINTR) continue;
				e = errno;
				break;
			}
			off += w;
		}
	}
	close(in);
	if (e == 0 && fchown(out, uid, gid) != 0) e = errno;
	close(out);
	if (e != 0) {
		(void)unlink(to);
		errno = e;
		return fpkg_error_from_errno(e);
	}
	return FPKG_OK;
}

static int make_symlink(const char* target, const char* linkpath) {
	assert(target != NULL);
	assert(linkpath != NULL);
	(void)unlink(linkpath);
	return symlink(target, linkpath);
}

static int pulse_server_from_config(const char* path, char* out, size_t n) {
	assert(path != NULL);
	assert(out != NULL);
	assert(n > 0);
	FILE* f = fopen(path, "r");
	char line[PATH_MAX];
	int rc = -1;

	if (f == NULL) return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		char* p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '\0' || *p == '\n' || *p == ';' || *p == '#')
			continue;
		if (strncmp(p, ".include ", 9) == 0) {
			char inc[PATH_MAX];
			snprintf(inc, sizeof(inc), "%s", p + 9);
			char* e = inc + strlen(inc);
			while (e > inc && (e[-1] == '\n' || e[-1] == '\r' ||
					   e[-1] == ' ' || e[-1] == '\t'))
				*--e = '\0';
			if (pulse_server_from_config(inc, out, n) == 0) {
				rc = 0;
				break;
			}
			continue;
		}
		if (*p == '[') break;
		char* eq = strchr(p, '=');
		if (eq == NULL) continue;
		size_t kl = (size_t)(eq - p);
		while (kl > 0 && (p[kl - 1] == ' ' || p[kl - 1] == '\t')) kl--;
		if (kl != strlen("default-server") ||
		    strncmp(p, "default-server", kl) != 0)
			continue;
		char* v = eq + 1;
		while (*v == ' ' || *v == '\t') v++;
		char* ve = v + strlen(v);
		while (ve > v && (ve[-1] == '\n' || ve[-1] == '\r' ||
				  ve[-1] == ' ' || ve[-1] == '\t'))
			*--ve = '\0';
		snprintf(out, n, "%s", v);
		rc = 0;
		break;
	}
	fclose(f);
	return rc;
}

static int pulse_parse_server(const char* value, char* out, size_t n,
			      bool* remote) {
	assert(value != NULL);
	assert(out != NULL);
	assert(n > 0);
	char* copy = xstrdup(value);
	int rc = -1;

	for (char *save = NULL, *tok = strtok_r(copy, " ", &save); tok != NULL;
	     tok = strtok_r(NULL, " ", &save)) {
		const char* s = tok;
		if (s[0] == '{') {
			const char* closing = strchr(s, '}');
			if (closing == NULL) continue;
			s = closing + 1;
		}
		if (strncmp(s, "unix:", 5) == 0) {
			snprintf(out, n, "%s", s + 5);
			rc = 0;
			break;
		}
		if (s[0] == '/') {
			snprintf(out, n, "%s", s);
			rc = 0;
			break;
		}
		if (strncmp(s, "tcp:", 4) == 0) {
			*remote = true;
			break;
		}
	}
	free(copy);
	return rc;
}

static int pulse_machine_id(char* out, size_t n) {
	assert(out != NULL);
	assert(n > 0);
	static const char* const ids[] = {"/etc/machine-id",
					  "/var/lib/dbus/machine-id"};

	for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		FILE* f = fopen(ids[i], "r");
		if (f == NULL) continue;
		bool got = fgets(out, (int)n, f) != NULL;
		fclose(f);
		if (!got) continue;
		char* e = out + strlen(out);
		while (e > out && (e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';
		bool hex = out[0] != '\0';
		for (char* c = out; *c; c++)
			if (!isxdigit((unsigned char)*c)) {
				hex = false;
				break;
			}
		if (hex) return 0;
	}
	return gethostname(out, n) == 0 ? 0 : -1;
}

static int pulse_runtime_dir(const sandbox_plan_t* plan, char* out, size_t n) {
	assert(plan != NULL);
	assert(out != NULL);
	assert(n > 0);
	const char* v = cli_getenv("PULSE_RUNTIME_PATH");
	char dir[PATH_MAX], real[PATH_MAX];
	struct stat st;

	if (v && *v && realpath(v, real) != NULL) {
		snprintf(out, n, "%s", real);
		return 0;
	}
	if (plan->xdg_runtime_dir && *plan->xdg_runtime_dir) {
		snprintf(dir, sizeof(dir), "%s/pulse", plan->xdg_runtime_dir);
		if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode) &&
		    realpath(dir, real) != NULL) {
			snprintf(out, n, "%s", real);
			return 0;
		}
	}
	char mid[256];
	if (pulse_machine_id(mid, sizeof(mid)) == 0) {
		static const char* const parents[] = {".pulse",
						      ".config/pulse"};
		for (size_t i = 0; i < sizeof(parents) / sizeof(parents[0]);
		     i++) {
			snprintf(dir, sizeof(dir), "%s/%s/%s-runtime",
				 plan->home_dir, parents[i], mid);
			if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode) &&
			    realpath(dir, real) != NULL) {
				snprintf(out, n, "%s", real);
				return 0;
			}
		}
	}
	return -1;
}

static int pulse_socket_path(const sandbox_plan_t* plan, char* out, size_t n,
			     bool* remote) {
	assert(plan != NULL);
	assert(out != NULL);
	assert(n > 0);
	const char* v = cli_getenv("PULSE_SERVER");
	char conf[PATH_MAX], dir[PATH_MAX], raw[PATH_MAX];
	struct stat st;

	*remote = false;
	if (v && *v) {
		if (pulse_parse_server(v, out, n, remote) == 0) return 0;
		if (*remote) return -1;
	}
	v = cli_getenv("PULSE_CLIENTCONFIG");
	if (v && *v && pulse_server_from_config(v, raw, sizeof(raw)) == 0 &&
	    pulse_parse_server(raw, out, n, remote) == 0)
		return 0;
	snprintf(conf, sizeof(conf), "%s/.config/pulse/client.conf",
		 plan->home_dir);
	if (pulse_server_from_config(conf, raw, sizeof(raw)) == 0 &&
	    pulse_parse_server(raw, out, n, remote) == 0)
		return 0;
	if (pulse_server_from_config("/etc/pulse/client.conf", raw,
				     sizeof(raw)) == 0 &&
	    pulse_parse_server(raw, out, n, remote) == 0)
		return 0;
	if (pulse_runtime_dir(plan, dir, sizeof(dir)) == 0) {
		snprintf(out, n, "%s/native", dir);
		if (stat(out, &st) == 0) return 0;
	}
	snprintf(out, n, "/var/run/pulse/native");
	if (stat(out, &st) == 0) return 0;
	return -1;
}

// A socket is served by mounting its parent directory, so a socket that sits
// in a shared directory would show the jail everything else in it too.
static bool socket_dir_too_broad(const char* dir) {
	assert(dir != NULL);
	static const char* const broad[] = {"/",	  "/tmp",  "/var/tmp",
					    "/var/run", "/run", "/var",
					    "/dev",	  NULL};

	for (size_t i = 0; broad[i] != NULL; i++)
		if (strcmp(dir, broad[i]) == 0) return true;
	return false;
}

// Bind a host UNIX socket by mounting its parent directory read-only: FreeBSD
// nullfs refuses a socket inode outright.
static int bind_host_socket_dir(sandbox_t* sb, const char* hsock,
				const char* jaildir) {
	assert(sb != NULL);
	assert(hsock != NULL);
	char dir[PATH_MAX], to[PATH_MAX];
	char* slash;
	struct stat st;

	if (stat(hsock, &st) != 0) return -1;
	snprintf(dir, sizeof(dir), "%s", hsock);
	slash = strrchr(dir, '/');
	if (slash == NULL || slash == dir) return -1;
	*slash = '\0';
	if (socket_dir_too_broad(dir)) {
		fpkg_warnf("refusing to bind broad socket directory %s "
			   "(socket %s)",
			   dir, hsock);
		return -1;
	}
	snprintf(to, sizeof(to), "%s%s", sb->jailroot, jaildir);
	if (mkdir_p(to, 0755) != 0) return -1;
	return m_nullfs(sb, dir, to, true);
}

// CUPS client.conf `ServerName <path>`; only a local unix socket is accepted
// (absolute, no ':'), else the default socket is used.
static int cups_server_from_config(const char* path, char* out, size_t n) {
	assert(path != NULL);
	assert(out != NULL);
	assert(n > 0);
	FILE* f = fopen(path, "r");
	char line[PATH_MAX];
	int rc = -1;

	if (f == NULL) return -1;
	while (fgets(line, sizeof(line), f) != NULL) {
		char* p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '\0' || *p == '\n' || *p == '#') continue;
		if (strncmp(p, "ServerName", 10) != 0) continue;
		p += 10;
		if (*p != ' ' && *p != '\t') continue;
		while (*p == ' ' || *p == '\t') p++;
		char* e = p + strlen(p);
		while (e > p && (e[-1] == '\n' || e[-1] == '\r' ||
				 e[-1] == ' ' || e[-1] == '\t'))
			*--e = '\0';
		if (p[0] == '/' && strchr(p, ':') == NULL) {
			snprintf(out, n, "%s", p);
			rc = 0;
		}
		break;
	}
	fclose(f);
	return rc;
}

static int cups_socket_path(const sandbox_plan_t* plan, char* out, size_t n) {
	assert(plan != NULL);
	assert(out != NULL);
	assert(n > 0);
	char conf[PATH_MAX], raw[PATH_MAX];
	static const char* const defaults[] = {"/var/run/cups/cups.sock",
					       "/run/cups/cups.sock",
					       "/var/run/cups.sock", NULL};
	const char* v = cli_getenv("CUPS_SERVER");
	struct stat st;

	if (v && *v && v[0] == '/' && strchr(v, ':') == NULL) {
		snprintf(out, n, "%s", v);
		return 0;
	}
	snprintf(conf, sizeof(conf), "%s/.cups/client.conf", plan->home_dir);
	if (cups_server_from_config(conf, raw, sizeof(raw)) == 0) {
		snprintf(out, n, "%s", raw);
		return 0;
	}
	if (cups_server_from_config("/etc/cups/client.conf", raw,
				    sizeof(raw)) == 0) {
		snprintf(out, n, "%s", raw);
		return 0;
	}
	for (size_t i = 0; defaults[i] != NULL; i++) {
		if (stat(defaults[i], &st) == 0) {
			snprintf(out, n, "%s", defaults[i]);
			return 0;
		}
	}
	return -1;
}

static int pcsc_socket_path(char* out, size_t n) {
	assert(out != NULL);
	assert(n > 0);
	static const char* const defaults[] = {"/var/run/pcscd/pcscd.comm",
					       "/run/pcscd/pcscd.comm", NULL};
	const char* v = cli_getenv("PCSCLITE_CSOCK_NAME");
	struct stat st;

	if (v && *v) {
		snprintf(out, n, "%s", v);
		return 0;
	}
	for (size_t i = 0; defaults[i] != NULL; i++) {
		if (stat(defaults[i], &st) == 0) {
			snprintf(out, n, "%s", defaults[i]);
			return 0;
		}
	}
	return -1;
}

static int gpg_agent_socket(const sandbox_plan_t* plan, char* out, size_t n) {
	assert(plan != NULL);
	assert(out != NULL);
	assert(n > 0);
	static const char* const gpgconfs[] = {"/usr/local/bin/gpgconf", NULL};
	struct stat st;

	for (size_t i = 0; gpgconfs[i]; i++) {
		char* const argv[] = {(char*)gpgconfs[i],
				      (char*)"--list-dir",
				      (char*)"agent-socket", NULL};
		int pfd[2];

		if (pipe(pfd) != 0) continue;
		int devnull = open("/dev/null", O_WRONLY);
		// run_program waits, so the first line must fit the pipe.
		(void)run_program(argv, -1, pfd[1], devnull);
		if (devnull >= 0) close(devnull);
		close(pfd[1]);
		ssize_t got = read(pfd[0], out, n - 1);
		close(pfd[0]);
		if (got <= 0) continue;
		out[got] = '\0';
		char* eol = strchr(out, '\n');
		if (eol != NULL) *eol = '\0';
		eol = out + strlen(out);
		while (eol > out && eol[-1] == '\r') *--eol = '\0';
		if (*out && stat(out, &st) == 0) return 0;
	}
	if (plan->xdg_runtime_dir && *plan->xdg_runtime_dir) {
		snprintf(out, n, "%s/gnupg/S.gpg-agent", plan->xdg_runtime_dir);
		if (stat(out, &st) == 0) return 0;
	}
	snprintf(out, n, "%s/.gnupg/S.gpg-agent", plan->home_dir);
	if (stat(out, &st) == 0) return 0;
	return -1;
}

void sb_track(sandbox_t* sb, const char* fstype, const char* from,
		     const char* to) {
	assert(sb != NULL);
	assert(fstype != NULL);
	assert(to != NULL);
	if (sb->nmounts == sb->mounts_cap) {
		sb->mounts_cap = sb->mounts_cap ? sb->mounts_cap * 2 : 32;
		sb_mount_t* new_mounts = realloc(
			sb->mounts, sb->mounts_cap * sizeof(sb_mount_t));
		if (!new_mounts) {
			fpkg_errorf("out of memory");
			exit(1);
		}
		sb->mounts = new_mounts;
	}
	sb_mount_t* m = &sb->mounts[sb->nmounts++];
	memset(m, 0, sizeof(*m));
	m->fstype = xstrdup(fstype);
	m->from = from ? xstrdup(from) : NULL;
	m->to = xstrdup(to);
}

// A mount the launch took down itself is no longer the teardown's to remove;
// leaving the record makes it unmount a path that is already gone and warn.
static void untrack_mount(sandbox_t* sb, const char* to) {
	assert(sb != NULL);
	assert(to != NULL);
	for (size_t i = 0; i < sb->nmounts; i++) {
		sb_mount_t* m = &sb->mounts[i];

		if (m->to == NULL || strcmp(m->to, to) != 0) continue;
		free(m->fstype);
		free(m->from);
		free(m->to);
		memset(m, 0, sizeof(*m));
	}
}

static bool sb_mounted_from(sandbox_t* sb, const char* to, const char* from) {
	assert(sb != NULL);
	assert(to != NULL);
	for (size_t i = 0; i < sb->nmounts; i++) {
		if (sb->mounts[i].from && sb->mounts[i].to &&
		    strcmp(sb->mounts[i].to, to) == 0 &&
		    strcmp(sb->mounts[i].from, from) == 0)
			return true;
	}
	return false;
}

bool sb_is_mounted(sandbox_t* sb, const char* path) {
	assert(sb != NULL);
	assert(path != NULL);
	for (size_t i = 0; i < sb->nmounts; i++) {
		if (sb->mounts[i].to != NULL &&
		    strcmp(sb->mounts[i].to, path) == 0)
			return true;
	}
	return false;
}

static void nmount_opt(struct iovec* iov, int* n, char* opt) {
	assert(iov != NULL);
	assert(n != NULL);
	assert(opt != NULL);
	char* eq = strchr(opt, '=');
	if (eq) {
		*eq = '\0';
		iov[*n].iov_base = opt;
		iov[*n].iov_len = strlen(opt) + 1;
		(*n)++;
		iov[*n].iov_base = eq + 1;
		iov[*n].iov_len = strlen(eq + 1) + 1;
		(*n)++;
	} else {
		iov[*n].iov_base = opt;
		iov[*n].iov_len = strlen(opt) + 1;
		(*n)++;
	}
}

int do_nmount(const char* fstype, const char* from, const char* to,
		     char* opts, bool ro) {
	assert(fstype != NULL);
	assert(to != NULL);
	struct iovec iov[24];
	int n = 0, flags = 0;

	iov[n].iov_base = (void*)"fstype";
	iov[n].iov_len = sizeof("fstype");
	n++;
	iov[n].iov_base = (void*)fstype;
	iov[n].iov_len = strlen(fstype) + 1;
	n++;
	iov[n].iov_base = (void*)"fspath";
	iov[n].iov_len = sizeof("fspath");
	n++;
	iov[n].iov_base = (void*)to;
	iov[n].iov_len = strlen(to) + 1;
	n++;
	if (from && *from) {
		iov[n].iov_base = (void*)"from";
		iov[n].iov_len = sizeof("from");
		n++;
		iov[n].iov_base = (void*)from;
		iov[n].iov_len = strlen(from) + 1;
		n++;
	}
	if (ro) flags |= MNT_RDONLY;
	if (opts) {
		char* save = NULL;
		for (char* o = strtok_r(opts, ";", &save); o;
		     o = strtok_r(NULL, ";", &save)) {
			if (n + 2 >= (int)(sizeof(iov) / sizeof(iov[0]))) break;
			nmount_opt(iov, &n, o);
		}
	}
	return nmount(iov, n, flags);
}

int mount_tool(sandbox_t* sb, const char* tool, char* const args[],
		      const char* fstype, const char* from, const char* to) {
	assert(sb != NULL);
	assert(tool != NULL);
	assert(args != NULL);
	size_t n = 0;
	while (args[n]) n++;
	char** argv = calloc(n + 2, sizeof(char*));
	if (!argv) return -1;
	argv[0] = (char*)tool;
	for (size_t i = 0; i < n; i++) argv[i + 1] = args[i];
	int rc = run_program(argv, -1, -1, -1);
	free(argv);
	if (rc != 0) {
		fpkg_errorf("%s failed (rc=%d): %s%s%s", tool, rc,
			    from ? from : "", from ? " -> " : "", to);
		return -1;
	}
	sb_track(sb, fstype, from, to);
	return 0;
}

// Is `to` already a nullfs mount of `from`? nullfs rejects a vnode mounted
// over itself with EDEADLK; statfs(2) gives the lower vnode via f_mntfromname.
static bool nullfs_already_mounted(const char* from, const char* to) {
	assert(from != NULL);
	assert(to != NULL);
	struct statfs sfs;
	char rfrom[PATH_MAX], rto[PATH_MAX], rsrc[PATH_MAX];
	const char *cf, *ct, *cs;

	if (statfs(to, &sfs) != 0 || strcmp(sfs.f_fstypename, "nullfs") != 0)
		return false;
	cf = realpath(from, rfrom) ? rfrom : from;
	ct = realpath(to, rto) ? rto : to;
	if (strcmp(cf, ct) == 0) return true;
	cs = realpath(sfs.f_mntfromname, rsrc) ? rsrc : sfs.f_mntfromname;
	return strcmp(cs, cf) == 0;
}

// The earlier nullfs mount already making `from` visible at `to` (a target
// that is a proper ancestor, same ancestor on the source side).  Ignoring it
// writes a mount point through into the host as root, and then EDEADLKs.
static const sb_mount_t* mount_covering(const sb_mount_t* ms, size_t n,
					const char* from, const char* to) {
	assert(from != NULL);
	assert(to != NULL);
	for (size_t i = 0; i < n; i++) {
		const sb_mount_t* m = &ms[i];
		if (m->from == NULL || m->to == NULL || m->fstype == NULL ||
		    strcmp(m->fstype, "nullfs") != 0)
			continue;
		size_t tl = strlen(m->to), fl = strlen(m->from);
		if (tl == 0 || fl == 0) continue;
		if (strncmp(to, m->to, tl) != 0 || to[tl] != '/') continue;
		if (strncmp(from, m->from, fl) != 0 || from[fl] != '/')
			continue;
		if (strcmp(to + tl, from + fl) == 0) return m;
	}
	return NULL;
}

int m_nullfs(sandbox_t* sb, const char* from, const char* to, bool ro) {
	assert(sb != NULL);
	assert(from != NULL);
	assert(to != NULL);
	if (do_nmount("nullfs", from, to, NULL, ro) == 0) {
		sb_track(sb, "nullfs", from, to);
		return 0;
	}

	// The kernel cannot stack the same lower vnode twice, so this means the
	// sandbox is already in the requested state: report the no-op. Not
	// keyed on errno.
	if (nullfs_already_mounted(from, to)) {
		struct statfs sfs;
		bool mounted_ro = statfs(to, &sfs) == 0 &&
				  (sfs.f_flags & MNT_RDONLY) != 0;

		if (mounted_ro != ro)
			fpkg_warnf("'%s' is already mounted %s, keeping it "
				   "(wanted %s)",
				   to, mounted_ro ? "read-only" : "read-write",
				   ro ? "read-only" : "read-write");
		else
			fpkg_infof("'%s' is already a nullfs mount of %s, "
				   "skipping duplicate",
				   to, from);
		return 0;
	}

	char* args[] = {"-o", ro ? (char*)"ro" : (char*)"rw", (char*)from,
			(char*)to, NULL};
	return mount_tool(sb, "/sbin/mount_nullfs", args, "nullfs", from, to);
}

static int m_unionfs(sandbox_t* sb, const char* from, const char* to,
		     bool below) {
	assert(sb != NULL);
	assert(from != NULL);
	assert(to != NULL);
	char opts[] = "below";
	if (below && do_nmount("unionfs", from, to, opts, false) == 0) {
		sb_track(sb, "unionfs", from, to);
		return 0;
	}
	char* args[] = {"-o", below ? (char*)"below" : (char*)"rw", (char*)from,
			(char*)to, NULL};
	return mount_tool(sb, "/sbin/mount_unionfs", args, "unionfs", from, to);
}

// A per-launch tmpfs is memory the app can fill, so each one carries a cap:
// without a size the app drives the host into swap from inside the jail.
#define TMPFS_SIZE_DEFAULT (256ull << 20)
#define TMPFS_SIZE_SHM (1ull << 30)

static int m_tmpfs_sized(sandbox_t* sb, const char* to,
			 unsigned long long size) {
	assert(sb != NULL);
	assert(to != NULL);
	char opts[64];
	char sizeopt[40];
	char* args[] = {"-t", "tmpfs", "-o", sizeopt, "tmpfs", (char*)to, NULL};
	int rc;

	snprintf(sizeopt, sizeof(sizeopt), "size=%llu", size);
	snprintf(opts, sizeof(opts), "size=%llu", size);
	if (do_nmount("tmpfs", "tmpfs", to, opts, false) == 0) {
		sb_track(sb, "tmpfs", NULL, to);
		rc = 0;
	} else {
		rc = mount_tool(sb, "/sbin/mount", args, "tmpfs", NULL, to);
	}
	if (rc == 0) {
		// Default tmpfs mode is 0755 root-owned; the app must write it.
		if (chmod(to, 01777) != 0)
			fpkg_warnf("chmod %s: %s", to, strerror(errno));
	}
	return rc;
}

static int m_tmpfs(sandbox_t* sb, const char* to) {
	return m_tmpfs_sized(sb, to, TMPFS_SIZE_DEFAULT);
}

int m_special(sandbox_t* sb, const char* fstype, const char* to) {
	assert(sb != NULL);
	assert(fstype != NULL);
	assert(to != NULL);
	if (do_nmount(fstype, fstype, to, NULL, false) == 0) {
		sb_track(sb, fstype, NULL, to);
		return 0;
	}
	char* args[] = {"-t", (char*)fstype, (char*)fstype, (char*)to, NULL};
	return mount_tool(sb, "/sbin/mount", args, fstype, NULL, to);
}

int m_special_opts(sandbox_t* sb, const char* fstype, const char* to,
			  const char* opts) {
	assert(sb != NULL);
	assert(fstype != NULL);
	assert(to != NULL);
	char buf[256];
	snprintf(buf, sizeof(buf), "%s", opts);
	for (char* c = buf; *c; c++)
		if (*c == ',') *c = ';';
	if (do_nmount(fstype, fstype, to, buf, false) == 0) {
		sb_track(sb, fstype, NULL, to);
		return 0;
	}
	char* args[] = {"-t",	       (char*)fstype, "-o", (char*)opts,
			(char*)fstype, (char*)to,     NULL};
	return mount_tool(sb, "/sbin/mount", args, fstype, NULL, to);
}

static bool p11kit_socket_usable(const char* path, uid_t uid) {
	assert(path != NULL);
	struct stat st;
	if (lstat(path, &st) != 0 || !S_ISSOCK(st.st_mode) || st.st_uid != uid)
		return false;

	struct sockaddr_un sa;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(sa.sun_path)) return false;
	strlcpy(sa.sun_path, path, sizeof(sa.sun_path));

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return false;
	bool live = connect(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0;
	close(fd);
	return live;
}

static bool p11kit_find_server(const char* dir, uid_t uid, char* sock_out,
			       size_t sock_len) {
	assert(dir != NULL);
	assert(sock_out != NULL);
	assert(sock_len > 0);
	DIR* d = opendir(dir);
	if (d == NULL) return false;

	struct dirent* de;
	while ((de = readdir(d)) != NULL) {
		if (strncmp(de->d_name, "pkcs11-", 7) != 0) continue;
		char p[PATH_MAX];
		int n = snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
		if (n < 0 || (size_t)n >= sizeof(p)) continue;
		if (p11kit_socket_usable(p, uid)) {
			n = snprintf(sock_out, sock_len, "%s", p);
			if (n < 0 || (size_t)n >= sock_len) continue;
			closedir(d);
			return true;
		}
	}
	closedir(d);
	return false;
}

// Make the host's PKCS#11 trust store reachable: reuse or start one per-user
// `p11-kit server` and bind its directory into the jail.
static bool p11kit_ensure_server(const sandbox_plan_t* plan, char* sock_out,
				 size_t sock_len) {
	assert(plan != NULL);
	assert(sock_out != NULL);
	assert(sock_len > 0);
	static const char* const servers[] = {"/usr/local/bin/p11-kit",
					      "/usr/bin/p11-kit", NULL};
	static const char* const modules[] = {
		"/usr/local/lib/pkcs11/p11-kit-trust.so",
		"/usr/local/lib/ossl-modules/p11-kit-trust.so", NULL};
	const char *server = NULL, *module = NULL;

	sock_out[0] = '\0';
	if (plan->xdg_runtime_dir == NULL || plan->xdg_runtime_dir[0] == '\0' ||
	    plan->user_name == NULL || plan->user_name[0] == '\0')
		return false;

	char dir[PATH_MAX];
	int n = snprintf(dir, sizeof(dir), "%s/p11-kit", plan->xdg_runtime_dir);
	if (n < 0 || (size_t)n >= sizeof(dir)) return false;

	struct stat st;
	if (lstat(dir, &st) == 0 &&
	    (!S_ISDIR(st.st_mode) || st.st_uid != plan->uid))
		return false;

	if (p11kit_find_server(dir, plan->uid, sock_out, sock_len)) return true;

	for (size_t i = 0; servers[i] != NULL && server == NULL; i++)
		if (access(servers[i], X_OK) == 0) server = servers[i];
	for (size_t i = 0; modules[i] != NULL && module == NULL; i++)
		if (access(modules[i], R_OK) == 0) module = modules[i];
	if (server == NULL || module == NULL) return false;

	char lockp[PATH_MAX];
	n = snprintf(lockp, sizeof(lockp), "%s/.flatpkg-p11kit.lock",
		     plan->xdg_runtime_dir);
	if (n < 0 || (size_t)n >= sizeof(lockp)) return false;
	int lfd = open(lockp, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (lfd < 0) return false;
	if (fchown(lfd, plan->uid, plan->gid) != 0) {
		close(lfd);
		return false;
	}

	if (flock(lfd, LOCK_EX | LOCK_NB) != 0) {
		for (int t = 0; t < 20; t++) {
			usleep(100000);
			if (p11kit_find_server(dir, plan->uid, sock_out,
					       sock_len)) {
				close(lfd);
				return true;
			}
		}
		close(lfd);
		return false;
	}

	if (p11kit_find_server(dir, plan->uid, sock_out, sock_len)) {
		close(lfd);
		return true;
	}

	pid_t pid = fork();
	if (pid == 0) {
		if (setgid(plan->gid) != 0 ||
		    initgroups(plan->user_name, plan->gid) != 0 ||
		    setuid(plan->uid) != 0)
			_exit(127);
		close_inherited_fds();
		if (chdir(plan->home_dir) != 0) _exit(127);

		setenv("HOME", plan->home_dir, 1);
		setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
		setenv("XDG_RUNTIME_DIR", plan->xdg_runtime_dir, 1);
		{
			char v[PATH_MAX];
			snprintf(v, sizeof(v), "%s/.config", plan->home_dir);
			setenv("XDG_CONFIG_HOME", v, 1);
			snprintf(v, sizeof(v), "%s/.local/share",
				 plan->home_dir);
			setenv("XDG_DATA_HOME", v, 1);
		}
		unsetenv("LD_PRELOAD");
		unsetenv("P11_KIT_SERVER_ADDRESS");
		unsetenv("P11_KIT_SERVER_PID");
		setsid();
		{
			int null = open("/dev/null", O_RDWR);
			if (null >= 0) {
				dup2(null, 0);
				dup2(null, 1);
				dup2(null, 2);
				if (null > 2) close(null);
			}
		}
		char* argv[] = {(char*)server, (char*)"server",
				(char*)"--timeout=300",
				(char*)"pkcs11:token=Default%20Trust", NULL};
		execv(server, argv);
		_exit(127);
	}

	bool ready = false;
	if (pid > 0) {
		for (int t = 0; t < 40; t++) {
			usleep(50000);
			if (p11kit_find_server(dir, plan->uid, sock_out,
					       sock_len)) {
				ready = true;
				break;
			}
			(void)waitpid(pid, NULL, WNOHANG);
		}
	}
	close(lfd);
	return ready;
}

static void drm_kernel_driver(char* out, size_t outlen) {
	assert(out != NULL);
	assert(outlen > 0);
	static const char* paths[] = {"/dev/dri/card0", "/dev/dri/renderD128"};

	out[0] = '\0';
	for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		char name[64] = "";
		struct drm_version_local v;

		int fd = open(paths[i], O_RDWR | O_CLOEXEC);
		if (fd < 0) continue;
		memset(&v, 0, sizeof(v));
		v.name = name;
		v.name_len = sizeof(name) - 1;
		if (ioctl(fd, DRM_IOCTL_VERSION_LOCAL, &v) == 0 &&
		    name[0] != '\0') {
			snprintf(out, outlen, "%s", name);
			close(fd);
			return;
		}
		close(fd);
	}
}

static void read_sysfs_value(const char* path, char* out, size_t outlen) {
	assert(path != NULL);
	assert(out != NULL);
	assert(outlen > 0);
	out[0] = '\0';
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return;
	char buf[128];
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0) return;
	buf[n] = '\0';
	char* nl = strchr(buf, '\n');
	if (nl) *nl = '\0';
	snprintf(out, outlen, "%s", buf);
}

static void read_sysfs_key(const char* path, const char* key, char* out,
			   size_t outlen) {
	assert(path != NULL);
	assert(key != NULL);
	assert(out != NULL);
	assert(outlen > 0);
	FILE* fp;
	size_t klen = strlen(key);

	out[0] = '\0';
	fp = fopen(path, "r");
	if (fp == NULL) return;
	char line[256];
	while (fgets(line, sizeof(line), fp)) {
		if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
			char* v = line + klen + 1;
			char* nl = strchr(v, '\n');
			if (nl) *nl = '\0';
			snprintf(out, outlen, "%s", v);
			break;
		}
	}
	fclose(fp);
}

static void write_drm_node(const char* jailroot, const char* rel,
			   unsigned major, unsigned minor,
			   const char* node_name, const char* driver,
			   const char* slot, const char* busname,
			   const char* vendor, const char* devid,
			   const char* subvendor, const char* subdevid) {
	assert(jailroot != NULL);
	assert(rel != NULL);
	char dir[PATH_MAX], file[PATH_MAX], buf[512];

	snprintf(dir, sizeof(dir), "%s/sys/%s", jailroot, rel);
	if (mkdir_p(dir, 0755) != 0) return;

	snprintf(file, sizeof(file), "%s/dev", dir);
	snprintf(buf, sizeof(buf), "%u:%u\n", major, minor);
	write_file(file, buf, 0644);

	snprintf(file, sizeof(file), "%s/uevent", dir);
	snprintf(buf, sizeof(buf),
		 "MAJOR=%u\nMINOR=%u\n"
		 "DEVNAME=dri/%s\nDEVTYPE=drm_minor\n",
		 major, minor, node_name);
	write_file(file, buf, 0644);

	snprintf(file, sizeof(file), "%s/subsystem", dir);
	(void)unlink(file);
	(void)symlink("../../../.sys/class/drm", file);

	snprintf(file, sizeof(file), "%s/device", dir);
	if (mkdir_p(file, 0755) != 0) return;
	char devdir[PATH_MAX];
	snprintf(devdir, sizeof(devdir), "%s", file);

	// libdrm's drmNodeIsDRM() stat()s <device>/drm, so synthesize it or
	// drmGetDevices2() rejects every node.
	snprintf(file, sizeof(file), "%s/drm", devdir);
	if (mkdir_p(file, 0755) != 0) return;

	if (driver != NULL && *driver != '\0') {
		snprintf(file, sizeof(file), "%s/driver", devdir);
		snprintf(buf, sizeof(buf), "../../../../.drmdrv/%s", driver);
		(void)unlink(file);
		(void)symlink(buf, file);
	}

	snprintf(file, sizeof(file), "%s/subsystem", devdir);
	snprintf(buf, sizeof(buf), "../../../../.drmbus/%s",
		 (busname != NULL && *busname != '\0') ? busname : "pci");
	(void)unlink(file);
	(void)symlink(buf, file);

	if (slot != NULL && *slot != '\0') {
		snprintf(file, sizeof(file), "%s/uevent", devdir);
		snprintf(buf, sizeof(buf), "DRIVER=%s\nPCI_SLOT_NAME=%s\n",
			 driver, slot);
		write_file(file, buf, 0644);
	}

	struct {
		const char* name;
		const char* val;
	} ids[] = {
		{"vendor", vendor},
		{"device", devid},
		{"subsystem_vendor", subvendor},
		{"subsystem_device", subdevid},
	};
	for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		if (ids[i].val == NULL || ids[i].val[0] == '\0') continue;
		snprintf(file, sizeof(file), "%s/%s", devdir, ids[i].name);
		snprintf(buf, sizeof(buf), "%s\n", ids[i].val);
		write_file(file, buf, 0644);
	}
}

// UNIX socket grants under the host XDG_RUNTIME_DIR cannot be nullfs-mounted;
// they are bound by symlinking through the /run/host tree instead.
static bool plan_has_xdg_socket_grant(const sandbox_plan_t* plan) {
	assert(plan != NULL);
	size_t len = strlen(plan->xdg_runtime_dir);
	for (size_t i = 0; i < plan->ngrants; i++) {
		const fs_grant_t* g = &plan->grants[i];
		if (g->mode == FS_GRANT_HIDE) continue;
		if (strncmp(g->host_path, plan->xdg_runtime_dir, len) == 0 &&
		    g->host_path[len] == '/') {
			struct stat st;
			if (lstat(g->host_path, &st) == 0 &&
			    S_ISSOCK(st.st_mode))
				return true;
		}
	}
	return false;
}

static int ensure_app_data_dirs(const sandbox_plan_t* plan,
				const char* app_id) {
	assert(plan != NULL);
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		if (initgroups(plan->user_name, plan->gid) != 0 ||
		    setgid(plan->gid) != 0 || setuid(plan->uid) != 0)
			_exit(1);
		static const char* subs[] = {"",
					     "data",
					     "config",
					     "cache",
					     "cache/fontconfig",
					     "cache/tmp",
					     ".local",
					     ".local/state",
					     NULL};
		for (int i = 0; subs[i]; i++) {
			char p[PATH_MAX];
			if (!subs[i][0])
				snprintf(p, sizeof(p), "%s/.var/app/%s",
					 plan->home_dir, app_id);
			else
				snprintf(p, sizeof(p), "%s/.var/app/%s/%s",
					 plan->home_dir, app_id, subs[i]);
			if (mkdir_p(p, 0755) != 0) _exit(2);
		}
		{
			static const char* nested[] = {"",
						       "data",
						       "config",
						       "cache",
						       "cache/fontconfig",
						       "cache/tmp",
						       ".local",
						       ".local/state",
						       NULL};
			for (int i = 0; nested[i]; i++) {
				char p[PATH_MAX];
				snprintf(p, sizeof(p),
					 "%s/.var/app/%s/.var/app/%s%s%s",
					 plan->home_dir, app_id, app_id,
					 nested[i][0] ? "/" : "", nested[i]);
				if (mkdir_p(p, 0755) != 0) _exit(2);
			}
		}
		_exit(0);
	}
	int ws = 0;
	if (waitpid(pid, &ws, 0) < 0 || !WIFEXITED(ws) || WEXITSTATUS(ws) != 0)
		return -1;
	return 0;
}

// $XDG_RUNTIME_DIR belongs to the caller, mode 0700. When missing, root must
// not leave it root-owned: it is then unusable and the session will not replace
// it.
int sandbox_ensure_runtime_dir(const sandbox_plan_t* plan) {
	assert(plan != NULL);
	struct stat st;
	char parent[PATH_MAX];
	char* slash;

	if (stat(plan->xdg_runtime_dir, &st) == 0) {
		if (st.st_uid != plan->uid) {
			fpkg_warnf("refusing to take over %s: it belongs "
				   "to uid %u, not %s (set XDG_RUNTIME_DIR to "
				   "your own runtime directory)",
				   plan->xdg_runtime_dir, (unsigned)st.st_uid,
				   plan->user_name);
			return -1;
		}
		if ((st.st_mode & 0777) != 0700 &&
		    chmod(plan->xdg_runtime_dir, 0700) != 0) {
			fpkg_warnf("cannot tighten %s: %s",
				   plan->xdg_runtime_dir, strerror(errno));
			return -1;
		}
		return 0;
	}

	snprintf(parent, sizeof(parent), "%s", plan->xdg_runtime_dir);
	slash = strrchr(parent, '/');
	if (slash != NULL && slash != parent) {
		*slash = '\0';
		if (mkdir_p(parent, 0711) != 0) return -1;
	}
	if (mkdir(plan->xdg_runtime_dir, 0700) != 0 && errno != EEXIST)
		return -1;
	if (chown_nofollow(plan->xdg_runtime_dir, plan->uid, plan->gid) != 0 ||
	    chmod(plan->xdg_runtime_dir, 0700) != 0) {
		fpkg_warnf("cannot hand %s to %s: %s", plan->xdg_runtime_dir,
			   plan->user_name, strerror(errno));
		return -1;
	}
	return 0;
}

static void host_timezone(char* out, size_t n) {
	assert(out != NULL);
	assert(n > 0);
	const char* tzdir = cli_getenv("TZDIR");
	if (!tzdir || !*tzdir) tzdir = "/usr/share/zoneinfo";
	char link[PATH_MAX];
	ssize_t l = readlink("/etc/localtime", link, sizeof(link) - 1);
	if (l > 0) {
		link[l] = '\0';
		char canon[PATH_MAX];
		if (link[0] == '/') snprintf(canon, sizeof(canon), "%s", link);
		else
			snprintf(canon, sizeof(canon), "/etc/%s", link);
		size_t tlen = strlen(tzdir);
		if (strncmp(canon, tzdir, tlen) == 0 && canon[tlen] == '/') {
			const char* tz = canon + tlen + 1;
			if (*tz && *tz != '/' && strstr(tz, "..") == NULL) {
				snprintf(out, n, "%s", tz);
				return;
			}
		}
	}
	FILE* f = fopen("/var/db/zoneinfo", "r");
	if (f) {
		char tz[128] = "";
		if (fgets(tz, sizeof(tz), f)) {
			tz[strcspn(tz, "\r\n")] = '\0';
			if (tz[0] && tz[0] != '/' && strstr(tz, "..") == NULL) {
				snprintf(out, n, "%s", tz);
				fclose(f);
				return;
			}
		}
		fclose(f);
	}
	snprintf(out, n, "UTC");
}

static int mkdir_app_xdg_dir(const sandbox_plan_t* plan, const char* app_id,
			     const char* sub) {
	assert(plan != NULL);
	assert(app_id != NULL);
	char base[PATH_MAX], appd[PATH_MAX], leaf[PATH_MAX];

	if (sandbox_ensure_runtime_dir(plan) != 0) return -1;
	snprintf(base, sizeof(base), "%s/.flatpak", plan->xdg_runtime_dir);
	snprintf(appd, sizeof(appd), "%s/%s", base, app_id);
	if (sub && sub[0]) snprintf(leaf, sizeof(leaf), "%s/%s", appd, sub);
	else
		snprintf(leaf, sizeof(leaf), "%s", appd);

	if (mkdir_p(base, 0755) != 0) return -1;
	if (mkdir_p(appd, 0755) != 0) return -1;
	if (sub && sub[0] && mkdir_p(leaf, 0700) != 0) return -1;

	// Each name lives under the caller's runtime dir, so it is chowned
	// without following a link the caller could have put there.
	const char* give[] = {base, appd, leaf};
	const mode_t modes[] = {0755, 0755, 0700};
	size_t ngive = (sub && sub[0]) ? 3 : 2;
	for (size_t i = 0; i < ngive; i++) {
		if (chmod(give[i], modes[i]) != 0)
			fpkg_warnf("chmod %s: %s", give[i], strerror(errno));
		if (chown_nofollow(give[i], plan->uid, plan->gid) != 0)
			fpkg_warnf("chown %s: %s", give[i], strerror(errno));
	}
	return 0;
}

// What a /etc/hosts line is, for ordering: 0 comment or blank, 1 an address of
// ::1, 2 anything else.  *l4 is set when the line maps "localhost" to an IPv4
// loopback address.
static int hosts_line_kind(const char* line, bool* l4) {
	assert(line != NULL);
	assert(l4 != NULL);
	const char* p = line + strspn(line, " \t");
	if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') return 0;
	size_t alen = strcspn(p, " \t\r\n#");
	bool v4lo = strncmp(p, "127.", 4) == 0;
	bool v6lo = alen == 3 && strncmp(p, "::1", 3) == 0;
	p += alen;
	while (*p != '\0' && *p != '#') {
		p += strspn(p, " \t\r\n");
		size_t n = strcspn(p, " \t\r\n#");
		if (n == 0) break;
		if (v4lo && n == 9 && strncasecmp(p, "localhost", 9) == 0)
			*l4 = true;
		p += n;
	}
	return v6lo ? 1 : 2;
}

// glibc answers a family-unspecific "localhost" from the first matching
// /etc/hosts line (no host.conf, so `multi` is off), and FreeBSD lists ::1
// first: an app listening on 127.0.0.1 is refused.  Copy Linux's order.
static int stage_hosts(const char* src, const char* etc) {
	assert(src != NULL);
	assert(etc != NULL);
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/hosts", etc);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	FILE* out = fd >= 0 ? fdopen(fd, "w") : NULL;
	if (out == NULL) {
		fpkg_errorf("write %s: %s", path, strerror(errno));
		if (fd >= 0) close(fd);
		return -1;
	}
	FILE* in = fopen(src, "r");
	char* line = NULL;
	size_t cap = 0;
	bool have4 = false;
	while (in != NULL && getline(&line, &cap, in) > 0) {
		bool l4 = false;
		(void)hosts_line_kind(line, &l4);
		have4 = have4 || l4;
	}
	if (!have4) fputs("127.0.0.1\tlocalhost\n", out);
	for (int pass = 0; pass < 2 && in != NULL; pass++) {
		// A failed rewind would make this pass read nothing and the
		// staged file silently lose every line, so it is checked.
		if (fseek(in, 0, SEEK_SET) != 0) {
			fpkg_errorf("read %s: %s", src, strerror(errno));
			free(line);
			fclose(in);
			fclose(out);
			return -1;
		}
		while (getline(&line, &cap, in) > 0) {
			bool l4 = false;
			bool v6 = hosts_line_kind(line, &l4) == 1;
			if (v6 != (pass == 1)) continue;
			fputs(line, out);
			if (strchr(line, '\n') == NULL) fputc('\n', out);
		}
	}
	free(line);
	if (in != NULL) fclose(in);
	// errno is read right where the failure happens: getline() above can
	// leave it set, so a later strerror(errno) could name the wrong error.
	int werr = ferror(out) != 0 ? EIO : 0;
	if (fclose(out) != 0 && werr == 0) werr = errno;
	if (werr != 0) {
		fpkg_errorf("write %s: %s", path, strerror(werr));
		return -1;
	}
	return 0;
}

// Dump the user's dconf database as the app, which is the uid that can read
// it. The dconf binary is local to the host, not the runtime.
static int dconf_dump_as_app(const sandbox_plan_t* plan, int out_fd) {
	pid_t pid = fork();

	if (pid < 0) return -1;
	if (pid == 0) {
		if (initgroups(plan->user_name, plan->gid) != 0 ||
		    setgid(plan->gid) != 0 || setuid(plan->uid) != 0) {
			dprintf(2, "flatpkg: setuid: %s\n", strerror(errno));
			_exit(127);
		}
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			if (dup2(devnull, 2) < 0) _exit(127);
			if (devnull != 2) close(devnull);
		}
		if (out_fd >= 0) {
			if (dup2(out_fd, 1) < 0) _exit(127);
			if (out_fd != 1) close(out_fd);
		}
		char* const argv[] = {(char*)"dconf", (char*)"dump",
				      (char*)"/", NULL};
		execv("/usr/local/bin/dconf", argv);
		execv("/usr/bin/dconf", argv);
		_exit(127);
	}
	int ws = 0;
	while (waitpid(pid, &ws, 0) < 0) {
		if (errno != EINTR) return -1;
	}
	return WIFEXITED(ws) && WEXITSTATUS(ws) == 0 ? 0 : -1;
}

// Seed the app's dconf keyfile from the host DB and its fontconfig cache
// from the user's. Best effort.
static void stage_dconf_migration(sandbox_t* sb, const sandbox_plan_t* plan) {
	char dconf_dir[PATH_MAX];

	snprintf(dconf_dir, sizeof(dconf_dir), "%s/run/flatpak/dconf",
		 sb->jailroot);
	if (mkdir_p(dconf_dir, 0755) != 0) return;

	char dump_path[PATH_MAX];
	snprintf(dump_path, sizeof(dump_path), "%s/user", dconf_dir);
	if (chown_nofollow(dconf_dir, plan->uid, plan->gid) != 0)
		fpkg_warnf("chown %s: %s", dconf_dir, strerror(errno));
	if (write_file(dump_path, "", 0644) != 0)
		fpkg_warnf("write %s: %s", dump_path, strerror(errno));
	if (chown_nofollow(dump_path, plan->uid, plan->gid) != 0)
		fpkg_warnf("chown %s: %s", dump_path, strerror(errno));
	int out_fd = open(dump_path, O_WRONLY | O_TRUNC);
	if (out_fd < 0) {
		fpkg_warnf("open %s: %s", dump_path, strerror(errno));
	} else {
		if (dconf_dump_as_app(plan, out_fd) != 0)
			fpkg_warnf("could not dump the host dconf database");
		close(out_fd);
	}

	char kf_dir[PATH_MAX];
	char kf_path[PATH_MAX * 2];
	snprintf(kf_dir, sizeof(kf_dir),
		 "%s/.var/app/%s/config/glib-2.0/settings", plan->home_dir,
		 sb->app_id);
	snprintf(kf_path, sizeof(kf_path), "%s/keyfile", kf_dir);
	if (mkdir_p(kf_dir, 0755) != 0) {
		fpkg_warnf("mkdir %s: %s", kf_dir, strerror(errno));
	} else {
		if (chown_nofollow(kf_dir, plan->uid, plan->gid) != 0)
			fpkg_warnf("chown %s: %s", kf_dir, strerror(errno));
		if (copy_file_owned(dump_path, kf_path, 0644, plan->uid,
				    plan->gid) != FPKG_OK &&
		    errno != EEXIST)
			fpkg_warnf("copy %s: %s", kf_path, strerror(errno));
	}

	char app_fc[PATH_MAX];
	char host_fc[PATH_MAX];
	const char* home = cli_getenv("HOME");
	struct stat fcst;
	snprintf(app_fc, sizeof(app_fc), "%s/.var/app/%s/cache/fontconfig",
		 plan->home_dir, sb->app_id);
	snprintf(host_fc, sizeof(host_fc), "%s/.cache/fontconfig",
		 home ? home : "/nonexistent");
	if (stat(host_fc, &fcst) == 0 && S_ISDIR(fcst.st_mode) &&
	    access(app_fc, F_OK) != 0) {
		char* const argv[] = {"/bin/cp", "-R", host_fc, app_fc, NULL};
		int devnull = open("/dev/null", O_WRONLY);

		if (run_program(argv, -1, -1, devnull) != 0)
			fpkg_warnf("could not copy the fontconfig cache");
		if (devnull >= 0) close(devnull);
	}
}


static int build_etc(sandbox_t* sb, const sandbox_plan_t* plan) {
	assert(sb != NULL);
	assert(plan != NULL);
	char path[PATH_MAX];
	char etc[PATH_MAX];
	snprintf(etc, sizeof(etc), "%s/run/flatpak/etc", sb->jailroot);
	if (mkdir_p(etc, 0755) != 0) {
		fpkg_errorf("mkdir %s: %s", etc, strerror(errno));
		return -1;
	}

	snprintf(path, sizeof(path), "%s/passwd", etc);
	// This is the jail's /etc/passwd entry: written as a file, never sent.
	char passwd_file[2048];
	snprintf(passwd_file, sizeof(passwd_file),
		 "%s:x:%u:%u:%s:%s:/bin/sh\n"
		 "nfsnobody:x:65534:65534:Unmapped user:/:/sbin/nologin\n",
		 plan->user_name, (unsigned)plan->uid, (unsigned)plan->gid,
		 plan->real_name, plan->home_dir);
	if (write_file(path, passwd_file, 0644) != 0) {
		fpkg_errorf("write %s: %s", path, strerror(errno));
		return -1;
	}
	snprintf(path, sizeof(path), "%s/group", etc);
	char group[1024];
	snprintf(group, sizeof(group), "%s:x:%u:%s\nnfsnobody:x:65534:\n",
		 plan->group_name, (unsigned)plan->gid, plan->user_name);
	if (write_file(path, group, 0644) != 0) {
		fpkg_errorf("write %s: %s", path, strerror(errno));
		return -1;
	}

	// GDBus refuses to autolaunch a bus unless this file holds 32 hex
	// digits, and the host's own id identifies the machine to the app and
	// the app to the host at once: each sandbox gets one of its own, as
	// flatpak's does.  arc4random is the kernel's, with nothing to seed.
	char mid[33];
	{
		unsigned char r[16];

		arc4random_buf(r, sizeof(r));
		for (size_t i = 0; i < sizeof(r); i++)
			snprintf(mid + i * 2, 3, "%02x", r[i]);
	}
	if (mid[0] != '\0') {
		// The canonical file is the id and a newline.
		char line[40];
		snprintf(line, sizeof(line), "%s\n", mid);
		snprintf(path, sizeof(path), "%s/machine-id", etc);
		if (write_file(path, line, 0644) != 0)
			fpkg_warnf("write %s: %s", path, strerror(errno));
		snprintf(path, sizeof(path), "%s/var/lib/dbus/machine-id",
			 sb->jailroot);
		if (write_file(path, line, 0644) != 0)
			fpkg_warnf("write %s: %s", path, strerror(errno));
	}

	if (plan->want_network && stage_hosts("/etc/hosts", etc) != 0)
		return -1;

	// The shims also load from this file, so the debugging escape hatch has
	// to leave it out: an app that clears LD_PRELOAD (GeForce NOW's launcher
	// does) would still get them otherwise.
	if (cli_getenv("FLATPKG_NO_SHIMS") == NULL) {
		snprintf(path, sizeof(path), "%s/ld.so.preload", etc);
		write_file(path,
			   "/.signalfd_shim.so\n/.seccomp_shim.so\n"
			   "/.memfd_shim.so\n/.syncobj_shim.so\n"
			   "/.rlimit_shim.so\n/.madvise_shim.so\n",
			   0644);
	}

	{
		static const char* conf =
			"include /run/flatpak/ld.so.conf.d/app-*.conf\n"
			"include /app/etc/ld.so.conf\n"
			"/app/lib\n"
			"include /run/flatpak/ld.so.conf.d/runtime-*.conf\n";
		snprintf(path, sizeof(path), "%s/ld.so.conf", etc);
		write_file(path, conf, 0644);
	}
	{
		char dir[PATH_MAX], conf_path[PATH_MAX];
		snprintf(dir, sizeof(dir), "%s/run/flatpak/ld.so.conf.d",
			 sb->jailroot);
		mkdir_p(dir, 0755);
		snprintf(conf_path, sizeof(conf_path), "%s/app-001-%s.conf",
			 dir, sb->app_id);
		write_file(conf_path, "/app/lib\n", 0644);
	}

	char tz[128] = "UTC";
	host_timezone(tz, sizeof(tz));
	{
		char tzc[192];
		snprintf(tzc, sizeof(tzc), "%s\n", tz);
		snprintf(path, sizeof(path), "%s/timezone", etc);
		write_file(path, tzc, 0644);
	}
	snprintf(path, sizeof(path), "%s/localtime", etc);
	{
		char zpath[PATH_MAX];
		const char* tzdir = cli_getenv("TZDIR");
		if (!tzdir || !*tzdir) tzdir = "/usr/share/zoneinfo";
		snprintf(zpath, sizeof(zpath), "%s/%s", tzdir, tz);
		if (copy_file(zpath, path, 0644) != FPKG_OK)
			write_file(path, "", 0644);
	}

	stage_dconf_migration(sb, plan);

	return 0;
}

static bool home_is_granted(const sandbox_plan_t* plan) {
	assert(plan != NULL);
	for (size_t i = 0; i < plan->ngrants; i++) {
		if (plan->grants[i].mode != FS_GRANT_HIDE &&
		    strcmp(plan->grants[i].jail_path, plan->home_dir) == 0)
			return true;
	}
	return false;
}

static bool plan_has_host_grant(const sandbox_plan_t* plan) {
	assert(plan != NULL);
	for (size_t i = 0; i < plan->ngrants; i++) {
		if (plan->grants[i].mode != FS_GRANT_HIDE &&
		    strcmp(plan->grants[i].jail_path, "/run/host") == 0)
			return true;
	}
	return false;
}

static void write_font_dirs_xml(sandbox_t* sb, const sandbox_plan_t* plan) {
	char p[PATH_MAX];
	const char* root = sb->jailroot;

	{
		char xml[4096];
		size_t off = 0;
		off += snprintf(xml + off, sizeof(xml) - off,
				"<?xml version=\"1.0\"?>\n"
				"<!DOCTYPE fontconfig SYSTEM "
				"\"urn:fontconfig:fonts.dtd\">\n"
				"<fontconfig>\n");
		struct {
			const char* host;
			const char* jail;
		} fonts[] = {
			{"/usr/share/fonts", "/run/host/fonts"},
			{"/usr/local/share/fonts", "/run/host/local-fonts"},
			{NULL, NULL}};
		for (int i = 0; fonts[i].host && off < sizeof(xml); i++) {
			struct stat st;
			if (stat(fonts[i].host, &st) != 0 ||
			    !S_ISDIR(st.st_mode))
				continue;
			off += snprintf(
				xml + off, sizeof(xml) - off,
				"   <remap-dir as-path=\"%s\">%s</remap-dir>\n",
				fonts[i].host, fonts[i].jail);
		}
		static const char* user_dirs[] = {".local/share/fonts",
						  ".fonts", NULL};
		for (int i = 0; user_dirs[i] && off < sizeof(xml); i++) {
			char ud[PATH_MAX];
			snprintf(ud, sizeof(ud), "%s/%s", plan->home_dir,
				 user_dirs[i]);
			struct stat st;
			if (stat(ud, &st) != 0 || !S_ISDIR(st.st_mode))
				continue;
			off += snprintf(xml + off, sizeof(xml) - off,
					"   <remap-dir as-path=\"%s\">"
					"/run/host/user-fonts</remap-dir>\n",
					ud);
			break;
		}
		if (off < sizeof(xml) - 16)
			snprintf(xml + off, sizeof(xml) - off,
				 "</fontconfig>\n");
		snprintf(p, sizeof(p), "%s/run/host/font-dirs.xml", root);
		write_file(p, xml, 0644);
	}
}

// /tmp: with filesystem=host the jail keeps a private dir, otherwise the
// per-app host dir is bound rw so the app's temp files persist.
static int stage_tmp(sandbox_t* sb, const sandbox_plan_t* plan) {
	char p[PATH_MAX];
	const char* root = sb->jailroot;

	if (plan_has_host_grant(plan)) {
		snprintf(p, sizeof(p), "%s/tmp", root);
		if (m_tmpfs(sb, p) != 0) return -1;
	} else if (sandbox_ensure_runtime_dir(plan) == 0 &&
		   access(plan->xdg_runtime_dir, R_OK) == 0) {
		char tmpdir[PATH_MAX];
		snprintf(tmpdir, sizeof(tmpdir), "%s/.flatpak/%s/tmp",
			 plan->xdg_runtime_dir, sb->app_id);
		// A private tmpfs, like bwrap --tmpfs.
		snprintf(p, sizeof(p), "%s/tmp", root);
		if (m_tmpfs(sb, p) != 0) return -1;
	} else {
		fpkg_warnf("no usable XDG_RUNTIME_DIR (%s), /tmp stays "
			   "private",
			   plan->xdg_runtime_dir);
		snprintf(p, sizeof(p), "%s/tmp", root);
		if (chmod(p, 01777) != 0)
			fpkg_warnf("chmod %s: %s", p, strerror(errno));
	}

	return 0;
}

static int build_skeleton(sandbox_t* sb, const sandbox_plan_t* plan) {
	assert(sb != NULL);
	assert(plan != NULL);
	char p[PATH_MAX];
	const char* root = sb->jailroot;

	static const char* dirs[] = {"usr",
				     "app",
				     "etc",
				     "tmp",
				     "var",
				     "var/cache",
				     "var/data",
				     "var/config",
				     "var/tmp",
				     "var/lib",
				     "var/lib/dbus",
				     "run",
				     "run/host",
				     "run/host/fonts",
				     "run/host/fonts-cache",
				     "run/host/user-fonts-cache",
				     "run/host/share",
				     "run/host/share/icons",
				     "run/host/user-share",
				     "run/host/user-share/icons",
				     "run/host/xdg",
				     "run/flatpak",
				     "run/flatpak/ld.so.conf.d",
				     "run/flatpak/p11-kit",
				     "proc",
				     "sys",
				     "sys/.sys",
				     "sys/class",
				     "sys/class/drm",
				     "sys/dev",
				     "sys/dev/char",
				     "sys/dev/char/226:0",
				     "sys/dev/char/226:0/device",
				     "sys/dev/char/226:128",
				     "sys/dev/char/226:128/device",
				     "dev",
				     "dev/dri",
				     "dev/shm",
				     "home",
				     "root",
				     "opt",
				     "srv",
				     "mnt",
				     "media",
				     NULL};
	for (int i = 0; dirs[i]; i++) {
		snprintf(p, sizeof(p), "%s/%s", root, dirs[i]);
		if (mkdir_p(p, 0755) != 0) {
			fpkg_errorf("mkdir %s: %s", p, strerror(errno));
			return -1;
		}
	}

	snprintf(p, sizeof(p), "%s/run/host/container-manager", root);
	if (write_file(p, "flatpak\n", 0644) != 0) {
		fpkg_errorf("write %s: %s", p, strerror(errno));
		return -1;
	}

	// flatpak always exposes the host's os-release here, and sandboxes read it
	// (GeForce NOW's launcher cats it); a missing file makes them guess.
	snprintf(p, sizeof(p), "%s/run/host/os-release", root);
	if (copy_file("/etc/os-release", p, 0644) != FPKG_OK)
		fpkg_warnf("no host os-release at %s", p);

	write_font_dirs_xml(sb, plan);

	// /run/user/<uid> for XDG_RUNTIME_DIR (0700, user-owned). The /run/user
	// parent must stay traversable, so 0755 there.
	snprintf(p, sizeof(p), "%s/run/user", root);
	if (mkdir_p(p, 0755) != 0) {
		fpkg_errorf("mkdir %s: %s", p, strerror(errno));
		return -1;
	}
	snprintf(p, sizeof(p), "%s/run/user/%u", root, (unsigned)plan->uid);
	if (mkdir_p(p, 0700) != 0) {
		fpkg_errorf("mkdir %s: %s", p, strerror(errno));
		return -1;
	}
	if (chown_nofollow(p, plan->uid, plan->gid) != 0) {
		fpkg_errorf("chown %s: %s", p, strerror(errno));
		return -1;
	}

	if (stage_tmp(sb, plan) != 0) return -1;

	snprintf(p, sizeof(p), "%s/var/run", root);
	make_symlink("../run", p);

	for (int i = 0; usrmerged_dirs[i]; i++) {
		snprintf(p, sizeof(p), "%s/%s", sb->runtime_dir,
			 usrmerged_dirs[i]);
		struct stat st;
		if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
			char link[PATH_MAX];
			snprintf(link, sizeof(link), "%s/%s", root,
				 usrmerged_dirs[i]);
			char target[64];
			snprintf(target, sizeof(target), "usr/%s",
				 usrmerged_dirs[i]);
			make_symlink(target, link);
		}
	}

	{
		pid_t pid = fork();
		if (pid < 0) {
			fpkg_errorf("fork: %s", strerror(errno));
			return -1;
		}
		if (pid == 0) {
			if (initgroups(plan->user_name, plan->gid) != 0 ||
			    setgid(plan->gid) != 0 || setuid(plan->uid) != 0)
				_exit(1);
			char usr[PATH_MAX];
			snprintf(usr, sizeof(usr), "%s/data/flatpak",
				 plan->app_data_dir);
			(void)mkdir_p(usr, 0755);
			_exit(permissions_ensure_data_dirs(
				      plan->app_data_dir) == 0
				      ? 0
				      : 1);
		}
		int wstatus = 0;
		if (waitpid(pid, &wstatus, 0) < 0) {
			fpkg_errorf("waitpid: %s", strerror(errno));
			return -1;
		}
		if (!WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
			fpkg_errorf("cannot create %s as %s",
				    plan->app_data_dir, plan->user_name);
			return -1;
		}
	}

	return build_etc(sb, plan);
}

// Create `path` when it is missing.  `created` is set only when this call
// made the file, so a caller may remove the placeholder without removing a
// file that was already there.
static int ensure_file(const char* path, bool* created) {
	int fd;

	assert(path != NULL);
	if (created != NULL) *created = false;
	fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd >= 0) {
		close(fd);
		if (created != NULL) *created = true;
		return 0;
	}
	if (errno != EEXIST) return -1;
	fd = open(path, O_RDWR);
	if (fd < 0) return -1;
	close(fd);
	return 0;
}

static void detach_bind(sandbox_t* sb, const char* path) {
	assert(sb != NULL);
	assert(path != NULL);
	char* const argv[] = {"/sbin/umount", (char*)path, NULL};
	int devnull = open("/dev/null", O_WRONLY);

	(void)run_program(argv, -1, -1, devnull);
	if (devnull >= 0) close(devnull);
	(void)rmdir(path);
	untrack_mount(sb, path);
}

static void mimic_recreate_one(sandbox_t* sb, const char* src, const char* dst,
			       const struct stat* st) {
	assert(sb != NULL);
	assert(src != NULL);
	assert(dst != NULL);
	if (S_ISDIR(st->st_mode)) {
		if (mkdir_p(dst, st->st_mode & 07777) != 0)
			fpkg_warnf("cannot recreate directory %s", dst);
		else if (m_nullfs(sb, src, dst, true) != 0)
			fpkg_warnf("cannot bind %s", dst);
	} else if (S_ISLNK(st->st_mode)) {
		char target[PATH_MAX];
		ssize_t l = readlink(src, target, sizeof(target) - 1);
		if (l > 0) {
			target[l] = '\0';
			(void)unlink(dst);
			if (symlink(target, dst) != 0)
				fpkg_warnf("cannot recreate symlink %s", dst);
		}
	} else if (S_ISREG(st->st_mode)) {
		bool created = false;

		if (ensure_file(dst, &created) != 0) {
			fpkg_warnf("cannot create %s", dst);
		} else if (m_nullfs(sb, src, dst, true) != 0) {
			fpkg_warnf("cannot bind %s", dst);
			if (created) (void)unlink(dst);
		}
	}
}

static void prune_ld_so_caches(const char* cache_dir) {
	assert(cache_dir != NULL);
	ld_cache_file_t files[64];
	size_t n = 0;

	DIR* d = opendir(cache_dir);
	if (!d) return;
	struct dirent* de;
	while ((de = readdir(d)) != NULL && n < 64) {
		size_t len = strlen(de->d_name);
		if (len != 16 + 4 || strcmp(de->d_name + 16, ".bin") != 0)
			continue;
		snprintf(files[n].path, sizeof(files[n].path), "%s/%s",
			 cache_dir, de->d_name);
		struct stat st;
		if (stat(files[n].path, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		files[n].mtime = st.st_mtime;
		n++;
	}
	closedir(d);

	for (size_t i = 0; i < n; i++)
		for (size_t j = i + 1; j < n; j++)
			if (files[j].mtime > files[i].mtime) {
				ld_cache_file_t tmp = files[i];
				files[i] = files[j];
				files[j] = tmp;
			}
	for (size_t i = LD_SO_CACHE_KEEP; i < n; i++) unlink(files[i].path);
}

// Read a whole Xauthority file; a 1 MiB cap keeps a hostile file bounded.
static bool xauth_read_file(const char* path, unsigned char** out,
			    size_t* outlen) {
	int fd = open(path, O_RDONLY);
	if (fd < 0) return false;

	size_t cap = 65536, len = 0;
	unsigned char* buf = malloc(cap);
	if (buf == NULL) {
		close(fd);
		return false;
	}
	for (;;) {
		if (len == cap) {
			unsigned char* nb = realloc(buf, cap * 2);
			if (nb == NULL) {
				free(buf);
				close(fd);
				return false;
			}
			buf = nb;
			cap *= 2;
		}
		ssize_t n = read(fd, buf + len, cap - len);
		if (n < 0) {
			free(buf);
			close(fd);
			return false;
		}
		if (n == 0) break;
		len += (size_t)n;
		if (len > 1024 * 1024) {
			free(buf);
			close(fd);
			return false;
		}
	}
	close(fd);
	if (len == 0) {
		free(buf);
		return false;
	}
	*out = buf;
	*outlen = len;
	return true;
}

static bool xauth_write_filtered(const char* root, const char* app_id,
				 const char* home, uid_t uid, gid_t gid) {
	assert(root != NULL);
	assert(app_id != NULL);
	if (home == NULL || *home == '\0') return false;
	char src[PATH_MAX];
	snprintf(src, sizeof(src), "%s/.Xauthority", home);

	unsigned char* buf = NULL;
	size_t len = 0;
	if (!xauth_read_file(src, &buf, &len)) return false;

	char hostname[256] = {0};
	if (gethostname(hostname, sizeof(hostname) - 1) != 0)
		hostname[0] = '\0';
	const char* accept[6];
	size_t naccept = 0;
	if (hostname[0] != '\0') accept[naccept++] = hostname;
	if (app_id != NULL && *app_id != '\0') accept[naccept++] = app_id;
	static const char* const local[] = {
		"localhost", "localhost.localdomain", "127.0.0.1", "unix"};
	for (size_t i = 0; i < sizeof(local) / sizeof(local[0]); i++)
		accept[naccept++] = local[i];

	struct xauth_rec {
		size_t start, len;
		bool keep;
	};
	struct xauth_rec* recs = NULL;
	size_t nrecs = 0, cap_recs = 16;
	recs = calloc(cap_recs, sizeof(*recs));
	if (recs == NULL) {
		free(buf);
		free(recs);
		return false;
	}
	size_t off = 0;
	bool malformed = false;
	while (off < len) {
		if (nrecs == cap_recs) {
			struct xauth_rec* nb =
				realloc(recs, cap_recs * 2 * sizeof(*recs));
			if (nb == NULL) {
				malformed = true;
				break;
			}
			recs = nb;
			cap_recs *= 2;
		}
		size_t start = off;
		if (off + 2 > len) {
			malformed = true;
			break;
		}
		off += 2;
		char addr[256] = {0};
		int field;
		for (field = 0; field < 4; field++) {
			if (off + 2 > len) {
				malformed = true;
				break;
			}
			size_t flen = ((size_t)buf[off] << 8) | buf[off + 1];
			off += 2;
			if (flen > len || off + flen > len) {
				malformed = true;
				break;
			}
			if (field == 0) {
				size_t c = flen < sizeof(addr) - 1
						   ? flen
						   : sizeof(addr) - 1;
				memcpy(addr, buf + off, c);
				addr[c] = '\0';
			}
			off += flen;
		}
		if (malformed) break;
		bool keep = (addr[0] == '\0');
		for (size_t i = 0; !keep && i < naccept; i++)
			if (strcmp(addr, accept[i]) == 0) keep = true;
		recs[nrecs].start = start;
		recs[nrecs].len = off - start;
		recs[nrecs].keep = keep;
		nrecs++;
	}
	if (malformed) {
		free(buf);
		free(recs);
		return false;
	}

	char rundir[PATH_MAX], dst[PATH_MAX], tmp[PATH_MAX];
	snprintf(rundir, sizeof(rundir), "%s/run/flatpak", root);
	if (mkdir_p(rundir, 0755) != 0) {
		free(buf);
		free(recs);
		return false;
	}
	snprintf(dst, sizeof(dst), "%s/Xauthority", rundir);
	snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
	int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (out < 0) {
		free(buf);
		free(recs);
		return false;
	}
	bool wrote = false;
	for (size_t i = 0; i < nrecs; i++) {
		if (!recs[i].keep) continue;
		if (write(out, buf + recs[i].start, recs[i].len) !=
		    (ssize_t)recs[i].len) {
			wrote = false;
			break;
		}
		wrote = true;
	}
	free(buf);
	free(recs);
	// The owner is set while the file is still open, before rename(2) gives
	// it its final name.
	if (wrote && fchown(out, uid, gid) != 0) wrote = false;
	close(out);
	if (!wrote) {
		(void)unlink(tmp);
		return false;
	}
	if (rename(tmp, dst) != 0) {
		(void)unlink(tmp);
		return false;
	}
	return true;
}

static bool dir_writable(const char* dir) {
	assert(dir != NULL);
	char probe[PATH_MAX];
	snprintf(probe, sizeof(probe), "%s/.flatpkg-wprobe.%ld", dir,
		 (long)getpid());
	if (mkdir(probe, 0700) == 0) {
		(void)rmdir(probe);
		return true;
	}
	return false;
}

static int writable_mimic(sandbox_t* sb, const char* target_dir) {
	assert(sb != NULL);
	assert(target_dir != NULL);
	if (dir_writable(target_dir)) return 0;

	char tmpl[PATH_MAX];
	snprintf(tmpl, sizeof(tmpl), "/tmp/.flatpkg_mimic.XXXXXX");
	char* safe = mkdtemp(tmpl);
	if (!safe) {
		fpkg_warnf("writable_mimic: mkdtemp failed: %s",
			   strerror(errno));
		return -1;
	}

	if (m_nullfs(sb, target_dir, safe, true) != 0) {
		fpkg_warnf("writable_mimic: bind %s -> %s failed", target_dir,
			   safe);
		(void)rmdir(safe);
		return -1;
	}

	// Capture the original dir's mode/owner before tmpfs covers it: tmpfs
	// defaults to 0755 root-owned, wrong for e.g. /usr/share.
	struct stat orig_st;
	bool have_orig_st = (lstat(safe, &orig_st) == 0);

	if (m_tmpfs(sb, target_dir) != 0) {
		fpkg_warnf("writable_mimic: tmpfs over %s failed", target_dir);
		detach_bind(sb, safe);
		return -1;
	}
	if (have_orig_st) {
		if (chmod(target_dir, orig_st.st_mode & 07777) != 0)
			fpkg_warnf("writable_mimic: chmod %s: %s", target_dir,
				   strerror(errno));
		// Restore the owner too, or a dir that was the user's comes
		// back root-owned and the app loses write access to its tree.
		if (orig_st.st_uid != 0 || orig_st.st_gid != 0) {
			if (chown(target_dir, orig_st.st_uid,
				  orig_st.st_gid) != 0)
				fpkg_warnf("writable_mimic: chown %s: %s",
					   target_dir, strerror(errno));
		}
	}

	DIR* d = opendir(safe);
	if (!d) {
		fpkg_warnf("writable_mimic: opendir %s failed: %s", safe,
			   strerror(errno));
		detach_bind(sb, safe);
		return -1;
	}
	struct dirent* ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.' &&
		    (ent->d_name[1] == '\0' ||
		     (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
			continue;
		char src[PATH_MAX], dst[PATH_MAX];
		snprintf(src, sizeof(src), "%s/%s", safe, ent->d_name);
		snprintf(dst, sizeof(dst), "%s/%s", target_dir, ent->d_name);
		struct stat est;
		if (lstat(src, &est) != 0) continue;
		mimic_recreate_one(sb, src, dst, &est);
	}
	closedir(d);
	detach_bind(sb, safe);
	return 0;
}

static int find_deepest_existing(const char* path, char* existing,
				 size_t outlen) {
	assert(path != NULL);
	assert(existing != NULL);
	assert(outlen > 0);
	char cur[PATH_MAX];
	snprintf(cur, sizeof(cur), "%s", path);
	for (;;) {
		struct stat st;
		if (stat(cur, &st) == 0 && S_ISDIR(st.st_mode)) {
			snprintf(existing, outlen, "%s", cur);
			return 0;
		}
		char* slash = strrchr(cur, '/');
		if (slash == NULL || slash == cur) break;
		*slash = '\0';
	}
	return -1;
}

int sandbox_ensure_writable_dir(sandbox_t* sb, const char* path) {
	assert(sb != NULL);
	assert(path != NULL);
	if (dir_writable(path)) return 0;

	char ancestor[PATH_MAX];
	if (find_deepest_existing(path, ancestor, sizeof(ancestor)) != 0)
		return -1;

	if (!dir_writable(ancestor)) {
		if (writable_mimic(sb, ancestor) != 0) return -1;
	}

	return mkdir_p(path, 0755);
}

static bool is_mount_point_dir(const char* path) {
	assert(path != NULL);
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// Extension mounts: a needs_tmpfs extension gets a writable parent, then its
// files/ tree is ro-bound; shallowest first, since nullfs hides inner mounts.
static int mount_extensions(sandbox_t* sb) {
	assert(sb != NULL);
	size_t n = sb->nextensions;
	if (n == 0) return 0;

	char(*targets)[PATH_MAX] = calloc(n, PATH_MAX);
	size_t* order = calloc(n, sizeof(*order));
	if (!targets || !order) {
		free(targets);
		free(order);
		fpkg_warnf("mount_extensions: out of memory");
		return -1;
	}
	for (size_t i = 0; i < n; i++) {
		fp_extension_t* e = &sb->extensions[i];
		snprintf(targets[i], PATH_MAX, "%s%s/%s", sb->jailroot,
			 e->target, e->directory);
		if (e->subdir_suffix && *e->subdir_suffix) {
			char tmp[PATH_MAX];
			snprintf(tmp, sizeof(tmp), "%s/%s", targets[i],
				 e->subdir_suffix);
			snprintf(targets[i], PATH_MAX, "%s", tmp);
		}
		order[i] = i;
	}
	{
		char(*cand)[PATH_MAX] = calloc(2 * n, PATH_MAX);
		size_t ncand = 0;
		if (!cand) {
			free(targets);
			free(order);
			fpkg_warnf("mount_extensions: out of memory");
			return -1;
		}
		for (size_t i = 0; i < n; i++) {
			char paths[2][PATH_MAX];
			size_t np = 0;
			if (is_mount_point_dir(targets[i])) continue;
			snprintf(paths[np++], PATH_MAX, "%s", targets[i]);
			if (sb->extensions[i].needs_tmpfs) {
				char* copy = xstrdup(targets[i]);
				char* dp = dirname(copy);
				snprintf(paths[np++], PATH_MAX, "%s", dp);
				free(copy);
			}
			for (size_t k = 0; k < np; k++) {
				char deep[PATH_MAX];
				if (find_deepest_existing(paths[k], deep,
							  sizeof(deep)) != 0)
					continue;
				if (dir_writable(deep)) continue;
				bool dup = false;
				for (size_t c = 0; c < ncand; c++) {
					if (strcmp(cand[c], deep) == 0) {
						dup = true;
						break;
					}
				}
				if (!dup && ncand < 2 * n)
					snprintf(cand[ncand++], PATH_MAX, "%s",
						 deep);
			}
		}
		for (size_t a = 1; a < ncand; a++) {
			char key[PATH_MAX];
			snprintf(key, sizeof(key), "%s", cand[a]);
			size_t b = a;
			while (b > 0 && strlen(cand[b - 1]) > strlen(key)) {
				snprintf(cand[b], PATH_MAX, "%s", cand[b - 1]);
				b--;
			}
			snprintf(cand[b], PATH_MAX, "%s", key);
		}
		for (size_t c = 0; c < ncand; c++) {
			if (sandbox_ensure_writable_dir(sb, cand[c]) != 0)
				fpkg_warnf("cannot make %s writable", cand[c]);
		}
		free(cand);
	}

	for (size_t i = 1; i < n; i++) {
		size_t key = order[i];
		size_t j = i;
		while (j > 0 &&
		       strlen(targets[order[j - 1]]) > strlen(targets[key])) {
			order[j] = order[j - 1];
			j--;
		}
		order[j] = key;
	}

	for (size_t k = 0; k < n; k++) {
		size_t i = order[k];
		fp_extension_t* e = &sb->extensions[i];
		bool missing = !is_mount_point_dir(targets[i]);

		if (e->needs_tmpfs && missing) {
			char parent[PATH_MAX];
			char* copy = xstrdup(targets[i]);
			char* dp = dirname(copy);
			snprintf(parent, sizeof(parent), "%s", dp);
			free(copy);
			if (sandbox_ensure_writable_dir(sb, parent) != 0) {
				fpkg_warnf("cannot create %s for "
					   "extension %s, skipping (writable "
					   "mimic failed on read-only runtime)",
					   parent, e->installed_id);
				continue;
			}
			// A subdirectory extension needs a writable parent;
			// upstream tmpfs'es it unconditionally.
			if (!dir_writable(parent) &&
			    writable_mimic(sb, parent) != 0) {
				fpkg_warnf("could not tmpfs %s for "
					   "extension %s, skipping",
					   parent, e->installed_id);
				continue;
			}
		}

		if (missing &&
		    sandbox_ensure_writable_dir(sb, targets[i]) != 0) {
			fpkg_warnf("cannot create mount point %s for extension "
				   "%s, skipping",
				   targets[i], e->installed_id);
			continue;
		}
		if (sb_mounted_from(sb, targets[i], e->files_path)) {
			fpkg_warnf("'%s' already mounted, skipping duplicate "
				   "extension mount",
				   targets[i]);
			continue;
		}
		if (m_nullfs(sb, e->files_path, targets[i], true) != 0)
			fpkg_warnf("failed to mount extension %s, continuing "
				   "without it",
				   e->installed_id);
	}

	free(targets);
	free(order);
	return 0;
}

static char* store_path_fallback(const char* home, const char* host_path) {
	assert(host_path != NULL);
	if (access(host_path, F_OK) == 0) return NULL;
	char upfx[4096];
	snprintf(upfx, sizeof(upfx), "%s/.local/share/flatpak", home);
	const char* prefix = NULL;
	if (strcmp(host_path, "/var/lib/flatpkg") == 0 ||
	    strncmp(host_path, "/var/lib/flatpkg/", 18) == 0)
		prefix = "/var/lib/flatpkg";
	else if (strcmp(host_path, "/var/lib/flatpak") == 0 ||
		 strncmp(host_path, "/var/lib/flatpak/", 17) == 0)
		prefix = "/var/lib/flatpak";
	else if (strcmp(host_path, upfx) == 0 ||
		 (strncmp(host_path, upfx, strlen(upfx)) == 0 &&
		  host_path[strlen(upfx)] == '/'))
		prefix = upfx;
	if (!prefix) return NULL;
	const char* sub = host_path + strlen(prefix);
	char* out = malloc(strlen(store_root()) + strlen(sub) + 1);
	if (!out) return NULL;
	snprintf(out, strlen(store_root()) + strlen(sub) + 1, "%s%s",
		 store_root(), sub);
	if (access(out, F_OK) != 0) {
		free(out);
		return NULL;
	}
	return out;
}

// Copy one preload shim into the jail root.  A short write or a failed
// close is reported like a missing shim: the app would otherwise start
// without the interposer and fail somewhere unrelated.
static void copy_shim(const char* jailroot, const char* name,
		      const char* why) {
	char from[PATH_MAX], to[PATH_MAX];
	snprintf(from, sizeof(from), "/usr/local/lib/flatpkg/%s", name);
	snprintf(to, sizeof(to), "%s/.%s", jailroot, name);
	if (copy_file(from, to, 0644) != FPKG_OK)
		fpkg_warnf("%s: %s; %s", from, strerror(errno), why);
}

static int stage_shims(sandbox_t* sb) {
	static const struct {
		const char* name;
		const char* why;
	} shims[] = {
		{"signalfd_shim.so", "launch-client signal watching will fail"},
		{"seccomp_shim.so", "sandbox will not block mount/unshare/etc."},
		{"memfd_shim.so", "JVM -XX:+UseZGC will fail to start"},
		{"syncobj_shim.so", "Vulkan apps may abort on fence status"},
		{"rlimit_shim.so", "RLIMIT_NICE/RTPRIO/RTTIME reads will fail"},
		{"madvise_shim.so", "Qt apps cannot open a window without it"},
	};
	for (size_t i = 0; i < sizeof(shims) / sizeof(shims[0]); i++)
		copy_shim(sb->jailroot, shims[i].name, shims[i].why);
	return 0;
}

static int stage_runtime(sandbox_t* sb, const sandbox_plan_t* plan) {
	char p[PATH_MAX];

	if (ensure_app_data_dirs(plan, sb->app_id) != 0)
		fpkg_warnf("could not prepare the app data dir for %s",
			   sb->app_id);

	snprintf(p, sizeof(p), "%s/usr", sb->jailroot);
	if (m_nullfs(sb, sb->runtime_dir, p, true) != 0) return -1;

	{
		char sroot[PATH_MAX];
		char sfrom[PATH_MAX];
		snprintf(sroot, sizeof(sroot), "%s/share", sb->jailroot);
		snprintf(sfrom, sizeof(sfrom), "%s/share", sb->runtime_dir);
		if (mkdir_p(sroot, 0755) == 0 &&
		    m_nullfs(sb, sfrom, sroot, true) != 0)
			fpkg_warnf("runtime %s has no top-level /share",
				   sb->runtime_dir);
	}
	return 0;
}

static int stage_spawn_shim(sandbox_t* sb) {
	// The flatpak-spawn shim on the writable /run tmpfs: --host execs
	// the runtime's real binary, otherwise COMMAND runs in-sandbox.
	{
		char up[PATH_MAX];
		snprintf(up, sizeof(up), "%s/run/flatpak", sb->jailroot);
		if (mkdir_p(up, 0755) == 0) {
			snprintf(up, sizeof(up),
				 "%s/run/flatpak/"
				 "flatpak-spawn",
				 sb->jailroot);
			static const char* shim_script =
				"#!/bin/sh\n"
				"# flatpkg-provided flatpak-spawn shim.\n"
				"# --host runs on the HOST: that is the "
				"runtime's\n"
				"#  own flatpak-spawn binary, which speaks\n"
				"#  HostCommand on org.freedesktop.Flatpak\n"
				"#  (flatpak-session-helper, served by "
				"flatpkg's\n"
				"#  portal process) and forwards stdio as "
				"real\n"
				"#  fds.  This shim only substitutes for the\n"
				"#  in-sandbox case, where the runtime's "
				"binary\n"
				"#  would look for a portal that does not "
				"exist.\n"
				"for a in \"$@\"; do\n"
				"  case \"$a\" in\n"
				"    --) break;;\n"
				"    --host) exec /usr/bin/flatpak-spawn "
				"\"$@\";;\n"
				"    -*) ;;\n"
				"    *) break;;\n"
				"  esac\n"
				"done\n"
				"# In-sandbox exec: set the XDG homes to the\n"
				"#  app's per-app dir first.\n"
				"export XDG_CONFIG_HOME=\"$HOME/.var/app/"
				"$FLATPAK_ID/config\"\n"
				"export XDG_DATA_HOME=\"$HOME/.var/app/"
				"$FLATPAK_ID/data\"\n"
				"export XDG_CACHE_HOME=\"$HOME/.var/app/"
				"$FLATPAK_ID/cache\"\n"
				"\n"
				"host=0\n"
				"args=''\n"
				"while [ $# -gt 0 ]; do\n"
				"  case \"$1\" in\n"
				"    --host) host=1; shift;;\n"
				"    --) shift; break;;\n"
				"    -*) shift;;\n"
				"    *) break;;\n"
				"  esac\n"
				"done\n"
				"if [ $host -eq 0 ]; then\n"
				"  exec \"$@\"\n"
				"fi\n"
				"# --host after '--' or behind an odd "
				"ordering:\n"
				"#  hand the whole (already stripped) command "
				"to the\n"
				"#  real binary.\n"
				"exec /usr/bin/flatpak-spawn --host \"$@\"\n";
			if (write_file(up, shim_script, 0755) != 0)
				fpkg_warnf("could not install the "
					   "flatpak-spawn shim");
		}
	}
	return 0;
}

static int stage_app(sandbox_t* sb, const fp_metadata_t* meta) {
	char p[PATH_MAX];
	snprintf(p, sizeof(p), "%s/app", sb->jailroot);
	if (m_nullfs(sb, sb->app_files_dir, p, true) != 0) return -1;

	if (fp_extensions_apply(meta, sb) != 0) return -1;
	if (mount_extensions(sb) != 0) return -1;
	return 0;
}

static int stage_etc(sandbox_t* sb) {
	char p[PATH_MAX];
	snprintf(p, sizeof(p), "%s/usr/etc", sb->jailroot);
	{
		struct stat st;
		if (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) {
			char etc_mnt[PATH_MAX];
			snprintf(etc_mnt, sizeof(etc_mnt), "%s/etc",
				 sb->jailroot);
			if (m_tmpfs(sb, etc_mnt) != 0) return -1;
			// 0755, not m_tmpfs's sticky 1777: /etc is this app's alone.
			if (chmod(etc_mnt, 0755) != 0)
				fpkg_warnf("chmod %s: %s", etc_mnt,
					   strerror(errno));
			char stage[PATH_MAX];
			snprintf(stage, sizeof(stage), "%s/run/flatpak/etc",
				 sb->jailroot);
			char staged_names[16][256];
			size_t nstaged = 0;
			DIR* gd = opendir(stage);
			if (gd) {
				struct dirent* ge;
				while ((ge = readdir(gd)) != NULL) {
					if (ge->d_name[0] == '.') continue;
					if (nstaged < 16) {
						snprintf(
							staged_names[nstaged++],
							sizeof(staged_names[0]),
							"%s", ge->d_name);
					}
				}
				closedir(gd);
			}
			DIR* sd = opendir(p);
			if (sd) {
				struct dirent* se;
				while ((se = readdir(sd)) != NULL) {
					if (se->d_name[0] == '.') continue;
					bool skip = false;
					for (size_t i = 0; i < nstaged; i++) {
						if (strcmp(staged_names[i],
							   se->d_name) == 0) {
							skip = true;
							break;
						}
					}
					if (skip) continue;
					if (strcmp(se->d_name, "pkcs11") == 0)
						continue;
					char sfrom[PATH_MAX], sto[PATH_MAX];
					snprintf(sfrom, sizeof(sfrom), "%s/%s",
						 p, se->d_name);
					snprintf(sto, sizeof(sto), "%s/etc/%s",
						 sb->jailroot, se->d_name);
					struct stat rst;
					if (stat(sfrom, &rst) != 0) continue;
					if (S_ISDIR(rst.st_mode)) {
						if (mkdir_p(sto, 0755) == 0)
							m_nullfs(sb, sfrom, sto,
								 true);
					} else if (S_ISREG(rst.st_mode)) {
						bool created = false;

						if (ensure_file(sto,
								&created) != 0)
							continue;
						if (m_nullfs(sb, sfrom, sto,
							     true) != 0 &&
						    created)
							(void)unlink(sto);
					}
				}
				closedir(sd);
			}
			DIR* sd2 = opendir(stage);
			if (sd2) {
				struct dirent* se;
				while ((se = readdir(sd2)) != NULL) {
					if (se->d_name[0] == '.') continue;
					char sfrom[PATH_MAX], sto[PATH_MAX];
					snprintf(sfrom, sizeof(sfrom), "%s/%s",
						 stage, se->d_name);
					snprintf(sto, sizeof(sto), "%s/etc/%s",
						 sb->jailroot, se->d_name);
					bool created = false;

					if (ensure_file(sto, &created) != 0) {
						fpkg_warnf("cannot create %s",
							   sto);
					} else if (m_nullfs(sb, sfrom, sto, true) !=
						   0) {
						fpkg_warnf("cannot bind %s",
							   sto);
						// An empty placeholder is worse
						// than no file: glib reads it as
						// an invalid machine-id and stops
						// falling back.  Only remove what
						// this call created.
						if (created) (void)unlink(sto);
					}
				}
				closedir(sd2);
			}
		}
	}

	{
		char p11dir[PATH_MAX], p11conf[PATH_MAX];
		snprintf(p11dir, sizeof(p11dir), "%s/etc/pkcs11", sb->jailroot);
		if (mkdir(p11dir, 0755) != 0 && errno != EEXIST)
			fpkg_warnf("mkdir %s: %s", p11dir, strerror(errno));
		else {
			snprintf(p11conf, sizeof(p11conf), "%s/pkcs11.conf",
				 p11dir);
			if (write_file(
				    p11conf,
				    "# Disable user pkcs11 config: the host's\n"
				    "# PKCS#11 modules are FreeBSD binaries "
				    "and\n"
				    "# cannot be loaded by the Linux runtime.\n"
				    "user-config: none\n",
				    0644) != 0)
				fpkg_warnf("write %s: %s", p11conf,
					   strerror(errno));
		}
	}
	return 0;
}

// FNV-1a over the runtime, app and extension paths; a change invalidates the
// cached ld.so.cache.
static void ld_cache_key(sandbox_t* sb, char* out, size_t n) {
	uint64_t hash = 0xcbf29ce484222325ULL;
	const char* sources[] = {sb->runtime_dir, sb->app_files_dir, NULL};

	for (int i = 0; sources[i]; i++) {
		for (const char* q = sources[i]; *q; q++) {
			hash ^= (uint64_t)(unsigned char)*q;
			hash *= 0x100000001b3ULL;
		}
		hash ^= '/';
		hash *= 0x100000001b3ULL;
	}
	for (size_t i = 0; i < sb->nextensions; i++) {
		const fp_extension_t* e = &sb->extensions[i];
		for (const char* q = e->files_path; *q; q++) {
			hash ^= (uint64_t)(unsigned char)*q;
			hash *= 0x100000001b3ULL;
		}
		hash ^= ';';
		hash *= 0x100000001b3ULL;
	}
	snprintf(out, n, "%016llx", (unsigned long long)hash);
}

// Executing a store binary as root is only safe when the store owns it: a
// deploy from a verified remote is root's, an imported one is the caller's own
// tree, so its ldconfig is code the caller chose.  Judge the file itself -
// regular, root's, not writable by anybody else - never the ref that named
// it.
bool ldconfig_trusted(const char* path) {
	assert(path != NULL);
	struct stat st;

	if (lstat(path, &st) != 0) return false;
	if (!S_ISREG(st.st_mode)) return false;
	if (st.st_uid != 0) return false;
	if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) return false;
	return true;
}

// Bounded: a stuck ldconfig once pinned a worker, a jail and its mounts.
static void run_ldconfig(sandbox_t* sb, const char* cache_dest) {
	// Re-judged here, not only where the path was picked: this is the fork
	// that would run it as root.
	if (sb->ldconfig_path == NULL || !ldconfig_trusted(sb->ldconfig_path)) {
		fpkg_warnf("not running %s as root: the store does not own it",
			   sb->ldconfig_path != NULL ? sb->ldconfig_path : "?");
		return;
	}
	pid_t lcp = fork();
	int status = -1;

	if (lcp == 0) {
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, 1);
			dup2(devnull, 2);
			close(devnull);
		}
		unsetenv("LD_DEBUG");
		unsetenv("LD_AUDIT");
		char ldp[PATH_MAX + 256];
		snprintf(ldp, sizeof(ldp),
			 "%s/lib:%s/lib64:%s/usr/lib:"
			 "%s/usr/lib64:%s/usr/lib/"
			 "x86_64-linux-gnu",
			 sb->jailroot, sb->jailroot, sb->jailroot, sb->jailroot,
			 sb->jailroot);
		setenv("LD_LIBRARY_PATH", ldp, 1);
		char* const av[] = {(char*)"ldconfig", (char*)"-r", sb->jailroot,
				    NULL};
		execv(sb->ldconfig_path, av);
		_exit(127);
	}
	if (lcp < 0) return;

	time_t deadline = time(NULL) + FLATPKG_LDCONFIG_TIMEOUT_S;
	bool killed = false;
	for (;;) {
		pid_t w = waitpid(lcp, &status, WNOHANG);
		if (w == lcp) break;
		if (w < 0 && errno != EINTR) break;
		if (time(NULL) >= deadline) {
			if (!killed) {
				fpkg_warnf("ldconfig did not finish in %ds; "
					   "starting without a library cache",
					   FLATPKG_LDCONFIG_TIMEOUT_S);
				(void)kill(lcp, SIGKILL);
				killed = true;
				deadline = time(NULL) + 5;
			} else {
				// It is stuck in the kernel; leave it.
				status = -1;
				break;
			}
		}
		usleep(50000);
	}
	if (killed) unlink(cache_dest);
}

// The cache is only stored if the runtime's ldconfig wrote glibc's format.
static bool ld_cache_valid(const char* cache_dest) {
	static const char want_magic[] = "glibc-ld.so.cache";
	char magic[sizeof(want_magic)] = {0};
	int in_fd = open(cache_dest, O_RDONLY);
	bool ok;

	if (in_fd < 0) return false;
	ssize_t got = read(in_fd, magic, sizeof(want_magic) - 1);
	close(in_fd);
	ok = got == (ssize_t)(sizeof(want_magic) - 1) &&
	     memcmp(magic, want_magic, sizeof(want_magic) - 1) == 0;
	if (!ok) {
		if (got > 0)
			fpkg_warnf("ldconfig produced a non-glibc cache "
				   "(magic \"%.16s\"), not caching",
				   magic);
		(void)unlink(cache_dest);
	}
	return ok;
}

static void store_ld_cache(const char* cache_dest, const char* cache_store,
			   const char* cache_dir, const char* sum) {
	int in_fd = open(cache_dest, O_RDONLY);

	if (in_fd < 0) return;
	struct stat ist;
	if (fstat(in_fd, &ist) == 0 && ist.st_size > 0) {
		int out_fd = open(cache_store,
				  O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out_fd >= 0) {
			char buf[65536];
			ssize_t n = 0;
			bool ok = true;
			while ((n = read(in_fd, buf, sizeof(buf))) > 0)
				if (write(out_fd, buf, (size_t)n) != n) {
					ok = false;
					break;
				}
			close(out_fd);
			if (n < 0) ok = false;
			if (ok) {
				fpkg_infof("ld.so.cache built and cached "
					   "(%ld bytes, %s)",
					   (long)ist.st_size, sum);
				prune_ld_so_caches(cache_dir);
			} else {
				(void)unlink(cache_store);
			}
		}
	}
	close(in_fd);
}

static int stage_ld_cache(sandbox_t* sb) {
	if (!sb->use_ld_so_cache || sb->ldconfig_path == NULL) return 0;

	char sum[32];
	ld_cache_key(sb, sum, sizeof(sum));

	char cache_dir[PATH_MAX];
	snprintf(cache_dir, sizeof(cache_dir), "%s/cache/ld.so",
		 store_root());
	mkdir_p(cache_dir, 0755);

	char cache_store[PATH_MAX];
	snprintf(cache_store, sizeof(cache_store), "%s/%s.bin", cache_dir, sum);
	char cache_dest[PATH_MAX];
	snprintf(cache_dest, sizeof(cache_dest), "%s/etc/ld.so.cache",
		 sb->jailroot);

	struct stat cst;
	bool hit = stat(cache_store, &cst) == 0 && S_ISREG(cst.st_mode) &&
		   cst.st_size > 0;
	if (hit && (ensure_file(cache_dest, NULL) != 0 ||
		    m_nullfs(sb, cache_store, cache_dest, true) != 0))
		hit = false;

	if (!hit) {
		run_ldconfig(sb, cache_dest);
		if (ld_cache_valid(cache_dest))
			store_ld_cache(cache_dest, cache_store, cache_dir, sum);
	}
	return 0;
}

static int stage_time(sandbox_t* sb) {
	{
		const char* tzdir = cli_getenv("TZDIR");
		if (!tzdir || !*tzdir) tzdir = "/usr/share/zoneinfo";
		struct stat zst;
		if (stat(tzdir, &zst) == 0 && S_ISDIR(zst.st_mode)) {
			char zp[PATH_MAX];
			snprintf(zp, sizeof(zp), "%s/usr/share/zoneinfo",
				 sb->jailroot);
			if (mkdir_p(zp, 0755) == 0)
				m_nullfs(sb, tzdir, zp, true);
		} else {
			fpkg_warnf(
				"host tzdir %s not found; named TZ= "
				"lookups fall back to the runtime's zoneinfo",
				tzdir);
		}
		char zdb[PATH_MAX], zdbdir[PATH_MAX];
		snprintf(zdbdir, sizeof(zdbdir), "%s/var/db", sb->jailroot);
		if (mkdir_p(zdbdir, 0755) == 0) {
			snprintf(zdb, sizeof(zdb), "%s/zoneinfo", zdbdir);
			(void)unlink(zdb);
			if (symlink("/etc/timezone", zdb) != 0)
				fpkg_warnf("symlink /var/db/zoneinfo: %s",
					   strerror(errno));
		}
	}
	return 0;
}

static int stage_var(sandbox_t* sb, const sandbox_plan_t* plan) {
	{
		static const struct {
			const char* sub;
			const char* jail;
		} var_mounts[] = {
			{"data", "var/data"},	{"config", "var/config"},
			{"cache", "var/cache"}, {"cache/tmp", "var/tmp"},
			{NULL, NULL},
		};
		for (int i = 0; var_mounts[i].sub; i++) {
			char from[PATH_MAX], to[PATH_MAX];
			snprintf(from, sizeof(from), "%s/%s",
				 plan->app_data_dir, var_mounts[i].sub);
			snprintf(to, sizeof(to), "%s/%s", sb->jailroot,
				 var_mounts[i].jail);
			if (m_nullfs(sb, from, to, false) != 0) return -1;
		}
	}
	return 0;
}

static int stage_p11kit(sandbox_t* sb, const sandbox_plan_t* plan) {
	{
		char p11sock[PATH_MAX];
		if (p11kit_ensure_server(plan, p11sock, sizeof(p11sock))) {
			const char* base = strrchr(p11sock, '/');
			if (base != NULL && base[1] != '\0') {
				char from[PATH_MAX], to[PATH_MAX];
				snprintf(from, sizeof(from), "%s/p11-kit",
					 plan->xdg_runtime_dir);
				snprintf(to, sizeof(to),
					 "%s/run/flatpak/p11-kit",
					 sb->jailroot);
				if (m_nullfs(sb, from, to, true) == 0) {
					char moddir[PATH_MAX],
						modfile[PATH_MAX];
					snprintf(moddir, sizeof(moddir),
						 "%s/etc/pkcs11/modules",
						 sb->jailroot);
					snprintf(modfile, sizeof(modfile),
						 "%s/p11-kit-trust.module",
						 moddir);
					if (mkdir_p(moddir, 0755) == 0 &&
					    write_file(modfile,
						       "# This overrides the "
						       "runtime "
						       "p11-kit-trusted module "
						       "with a "
						       "client one talking to "
						       "the trust "
						       "module on the host\n"
						       "module: "
						       "p11-kit-client.so\n",
						       0644) == 0) {
						free(sb->p11kit_socket_name);
						sb->p11kit_socket_name =
							xstrdup(base + 1);
					}
				}
			}
		}
	}
	return 0;
}

// Create `path` as the caller.  A mount point under $HOME is created on a
// directory the user owns (and, when the app data dir is the home, on the host
// itself), so a root-owned placeholder must not be left behind.
static int mkdir_as_caller(const sandbox_plan_t* plan, const char* path,
			   mode_t mode) {
	pid_t pid = fork();
	int ws = 0;
	int e;

	if (pid < 0) return -1;
	if (pid == 0) {
		if (initgroups(plan->user_name, plan->gid) != 0 ||
		    setgid(plan->gid) != 0 || setuid(plan->uid) != 0)
			_exit(EPERM);
		if (mkdir_p(path, mode) != 0)
			_exit(errno > 0 && errno < 256 ? errno : 1);
		_exit(0);
	}
	while (waitpid(pid, &ws, 0) < 0) {
		if (errno != EINTR) return -1;
	}
	if (!WIFEXITED(ws)) return -1;
	e = WEXITSTATUS(ws);
	if (e != 0) errno = e;
	return e == 0 ? 0 : -1;
}

static bool jail_path_under_home(const sandbox_t* sb,
				 const sandbox_plan_t* plan, const char* path) {
	char home[PATH_MAX];
	int n = snprintf(home, sizeof(home), "%s%s/", sb->jailroot,
			 plan->home_dir);

	return n > 0 && (size_t)n < sizeof(home) &&
	       strncmp(path, home, (size_t)n) == 0;
}

static int stage_home(sandbox_t* sb, const sandbox_plan_t* plan) {
	bool home_granted = home_is_granted(plan);
	char hto[PATH_MAX], appto[PATH_MAX], hide[PATH_MAX];

	snprintf(hto, sizeof(hto), "%s%s", sb->jailroot, plan->home_dir);
	if (mkdir_p(hto, 0755) != 0) {
		fpkg_errorf("mkdir %s: %s", hto, strerror(errno));
		return -1;
	}

	if (home_granted) {
		if (m_nullfs(sb, plan->home_dir, hto, false) != 0) return -1;
		snprintf(hide, sizeof(hide), "%s/.var/app", hto);
		if (mkdir_p(hide, 0755) != 0) {
			fpkg_errorf("mkdir %s: %s", hide, strerror(errno));
			return -1;
		}
		if (m_tmpfs(sb, hide) != 0) return -1;
	} else {
		// The app's home is its private dir, as in flatpak: a write to
		// $HOME (Firefox's ~/.mozilla, an app's own ~/.<name>) must
		// outlive the sandbox.  XDG_*_HOME and the spawn shim spell the
		// same dir as ~/.var/app/<id>/..., so it is mounted there too.
		if (m_nullfs(sb, plan->app_data_dir, hto, false) != 0) return -1;
	}

	snprintf(appto, sizeof(appto), "%s/.var/app/%s", hto, sb->app_id);
	// The mount point is created with the mode it needs: a chmod walk here
	// would go through the home nullfs and change the host directory.
	if ((home_granted ? mkdir_p(appto, 0755)
			  : mkdir_as_caller(plan, appto, 0755)) != 0) {
		fpkg_errorf("mkdir %s: %s", appto, strerror(errno));
		return -1;
	}
	if (m_nullfs(sb, plan->app_data_dir, appto, false) != 0) return -1;
	return 0;
}

// FreeBSD cannot narrow a mount with a later one: nullfs refuses a target that
// already resolves to the same lower vnode, so the narrower grant would be
// dropped and the wider mode would rule the child.  The wider grant is left
// out instead, and the narrower one mounts on the jail's own directory.
static bool grant_narrowed_below(const sandbox_plan_t* plan,
				 const fs_grant_t* g) {
	size_t gl = strlen(g->jail_path);
	bool want_ro = g->mode == FS_GRANT_RO;

	for (size_t i = 0; i < plan->ngrants; i++) {
		const fs_grant_t* o = &plan->grants[i];

		// A persistent grant is the app's own data directory, mounted
		// over the home it already sits in; it is not a request that
		// narrows anything.
		if (o == g || o->mode == FS_GRANT_HIDE || o->persistent) continue;
		if (strncmp(o->jail_path, g->jail_path, gl) != 0) continue;
		if (o->jail_path[gl] != '/') continue;
		if ((o->mode == FS_GRANT_RO) != want_ro) return true;
	}
	return false;
}

// The same rule one level up: a host top-level view a grant would have to
// narrow is not mounted, and the grant mounts on the jail's own directory.
static bool host_mount_narrowed(const sandbox_plan_t* plan, const char* name,
				bool ro, const char** why) {
	char prefix[PATH_MAX];
	size_t pl;

	snprintf(prefix, sizeof(prefix), "/%s/", name);
	pl = strlen(prefix);
	for (size_t i = 0; i < plan->ngrants; i++) {
		const fs_grant_t* g = &plan->grants[i];

		if (g->mode == FS_GRANT_HIDE || g->persistent) continue;
		if (strncmp(g->jail_path, prefix, pl) != 0) continue;
		if ((g->mode == FS_GRANT_RO) != ro) {
			*why = g->jail_path;
			return true;
		}
	}
	return false;
}

// Handle one grant; returns 1 when its host path was missing.
static int apply_grant(sandbox_t* sb, const sandbox_plan_t* plan,
		       fs_grant_t* g, bool home_granted) {
	if (g->mode != FS_GRANT_HIDE && strcmp(g->jail_path, "/run/host") == 0)
		return 0;

	if (g->mode == FS_GRANT_HIDE) {
		char to[PATH_MAX];
		snprintf(to, sizeof(to), "%s%s", sb->jailroot, g->jail_path);
		if ((jail_path_under_home(sb, plan, to)
			 ? mkdir_as_caller(plan, to, 0755)
			 : mkdir_p(to, 0755)) != 0) {
			fpkg_warnf("cannot create hide point %s, skipping",
				   g->jail_path);
			return 0;
		}
		if (m_tmpfs(sb, to) != 0)
			fpkg_warnf("could not hide %s", g->jail_path);
		return 0;
	}

	if (g->persistent && home_granted) return 0;

	if (grant_narrowed_below(plan, g)) {
		fpkg_warnf("not mounting '%s': a narrower grant inside it asks "
			   "for the other mode, which a later mount here cannot "
			   "give it",
			   g->jail_path);
		return 0;
	}

	if (g->mode == FS_GRANT_CREATE) {
		pid_t pid = fork();
		if (pid < 0) {
			fpkg_warnf("fork failed for %s, skipping grant",
				   g->host_path);
			return 0;
		}
		if (pid == 0) {
			if (initgroups(plan->user_name, plan->gid) != 0 ||
			    setgid(plan->gid) != 0 || setuid(plan->uid) != 0)
				_exit(1);
			_exit(mkdir_p(g->host_path, 0755) == 0 ? 0 : 1);
		}
		int wstatus = 0;
		if (waitpid(pid, &wstatus, 0) < 0 || !WIFEXITED(wstatus) ||
		    WEXITSTATUS(wstatus) != 0) {
			fpkg_warnf("cannot create %s for grant, skipping",
				   g->host_path);
			return 0;
		}
	}

	char to[PATH_MAX];
	snprintf(to, sizeof(to), "%s%s", sb->jailroot, g->jail_path);
	{
		// Decided before making the mount point; see mount_covering().
		const sb_mount_t* cover =
			mount_covering(sb->mounts, sb->nmounts, g->host_path, to);
		if (cover != NULL) {
			struct statfs cfs;
			bool cover_ro = statfs(cover->to, &cfs) == 0 &&
					(cfs.f_flags & MNT_RDONLY) != 0;
			bool want_ro = (g->mode == FS_GRANT_RO);
			bool same_source = cover->from != NULL &&
					   strcmp(cover->from, g->host_path) == 0;

			// The same view twice, or the permissions wanted are
			// already the answer: nothing to mount.
			if (same_source || cover_ro == want_ro) {
				fpkg_infof("'%s' is already visible through "
					   "the grant of %s, skipping",
					   g->jail_path, cover->from);
				return 0;
			}
			// A narrower grant over a wider one: mount it, or the
			// wider mount's permissions stay the answer - which is
			// how a read-only child ended up writable through the
			// parent.
			fpkg_debugf("'%s' narrows the grant of %s", g->jail_path,
				    cover->from);
		}
	}
	if ((jail_path_under_home(sb, plan, to) ? mkdir_as_caller(plan, to, 0755)
					       : mkdir_p(to, 0755)) != 0) {
		fpkg_warnf("cannot create mount point for '%s', skipping",
			   g->jail_path);
		return 0;
	}
	if (access(g->host_path, F_OK) != 0) {
		char* mapped =
			store_path_fallback(plan->home_dir, g->host_path);
		if (mapped) {
			free(g->host_path);
			g->host_path = mapped;
		} else {
			fpkg_infof("skipping grant '%s' -> '%s': no such host "
				   "path",
				   g->host_path, g->jail_path);
			return 1;
		}
	}
	{
		struct stat st;
		if (lstat(g->host_path, &st) == 0 && S_ISSOCK(st.st_mode))
			return 0;
	}
	if (sb_mounted_from(sb, to, g->host_path)) {
		fpkg_infof("'%s' already bound from %s, skipping duplicate "
			   "grant",
			   g->jail_path, g->host_path);
		return 0;
	}
	bool ro = (g->mode == FS_GRANT_RO);
	if (m_nullfs(sb, g->host_path, to, ro) != 0)
		fpkg_warnf("failed to bind grant '%s', continuing without it",
			   g->host_path);
	return 0;
}

static size_t grant_depth(const fs_grant_t* g) {
	size_t depth = 0;

	assert(g != NULL);
	for (const char* c = g->jail_path; *c; c++)
		if (*c == '/') depth++;
	return depth;
}

static int stage_grants(sandbox_t* sb, const sandbox_plan_t* plan) {
	bool home_granted = home_is_granted(plan);
	size_t missing_paths = 0;
	size_t* order = NULL;

	// Shallow first: a grant is a view of a host path, so a narrower one
	// has to be mounted on top of the wider one it lives in.  The order is
	// the claim; a metadata file's line order is not.
	if (plan->ngrants > 0) {
		order = calloc(plan->ngrants, sizeof(*order));
		if (order == NULL) return -1;
		for (size_t i = 0; i < plan->ngrants; i++) order[i] = i;
		for (size_t i = 1; i < plan->ngrants; i++) {
			size_t key = order[i];
			size_t j = i;

			while (j > 0 && grant_depth(&plan->grants[order[j - 1]]) >
						grant_depth(&plan->grants[key])) {
				order[j] = order[j - 1];
				j--;
			}
			order[j] = key;
		}
	}
	for (size_t i = 0; i < plan->ngrants; i++) {
		fs_grant_t* g = &plan->grants[order[i]];

		if (apply_grant(sb, plan, g, home_granted) > 0) missing_paths++;
	}
	free(order);
	if (missing_paths > 0)
		fpkg_infof("skipped %zu missing host-path grant(s)",
			   missing_paths);
	return 0;
}

static bool tmp_is_exported(const sandbox_plan_t* plan) {
	for (size_t i = 0; i < plan->ngrants; i++)
		if (strcmp(plan->grants[i].jail_path, "/tmp") == 0) return true;
	return false;
}

static int stage_x11(sandbox_t* sb, const sandbox_plan_t* plan) {
	char x11dir[PATH_MAX];
	snprintf(x11dir, sizeof(x11dir), "%s/tmp/.X11-unix", sb->jailroot);
	if (mkdir_p(x11dir, 0755) != 0) {
		fpkg_errorf("mkdir %s: %s", x11dir, strerror(errno));
		return -1;
	}
	bool x11 = plan->sock_x11 ||
		   (plan->sock_fallback_x11 && !plan->sock_wayland);
	if (tmp_is_exported(plan)) {
		if (m_tmpfs(sb, x11dir) != 0) return -1;
	}
	if (x11 && access("/tmp/.X11-unix", R_OK) == 0) {
		if (m_nullfs(sb, "/tmp/.X11-unix", x11dir, true) != 0)
			fpkg_warnf("could not expose X11 sockets");
		else
			sb->xauth_written = xauth_write_filtered(
				sb->jailroot, sb->app_id, plan->home_dir,
				plan->uid, plan->gid);
	}
	return 0;
}

static bool plan_uses_xdg_runtime(const sandbox_plan_t* plan) {
	return plan->sock_wayland || plan->sock_inherit_wayland ||
	       plan->sock_session_bus || plan_has_xdg_socket_grant(plan);
}

static void stage_xdg_socket_links(sandbox_t* sb, const sandbox_plan_t* plan,
				   const char* runuser, bool hostxdg_bound) {
	if (hostxdg_bound &&
	    (plan->sock_wayland || plan->sock_inherit_wayland)) {
		char base[256];
		char sock[PATH_MAX], link[PATH_MAX];

		host_wayland_display(plan, base, sizeof(base));
		snprintf(sb->wayland_display, sizeof(sb->wayland_display), "%s",
			 base);
		snprintf(sock, sizeof(sock), "%s/%s", plan->xdg_runtime_dir,
			 base);
		if (access(sock, R_OK) == 0) {
			snprintf(link, sizeof(link), "%s/%s", runuser, base);
			(void)unlink(link);
			snprintf(sock, sizeof(sock), "/run/host/xdg/%s", base);
			make_symlink(sock, link);
		}
	}
	if (hostxdg_bound && plan->sock_session_bus) {
		char bsock[PATH_MAX], link[PATH_MAX];

		snprintf(bsock, sizeof(bsock), "%s/bus",
			 plan->xdg_runtime_dir);
		if (access(bsock, R_OK) == 0) {
			snprintf(link, sizeof(link), "%s/bus", runuser);
			(void)unlink(link);
			make_symlink("/run/host/xdg/bus", link);
		}
	}
	if (hostxdg_bound) {
		size_t xl = strlen(plan->xdg_runtime_dir);

		for (size_t gi = 0; gi < plan->ngrants; gi++) {
			const fs_grant_t* g = &plan->grants[gi];

			if (g->mode == FS_GRANT_HIDE) continue;
			if (strncmp(g->host_path, plan->xdg_runtime_dir, xl) !=
				0 ||
			    g->host_path[xl] != '/')
				continue;
			struct stat gst;
			if (lstat(g->host_path, &gst) != 0 ||
			    !S_ISSOCK(gst.st_mode))
				continue;
			const char* base = g->host_path + xl + 1;
			if (!*base || strchr(base, '/')) continue;
			char link[PATH_MAX];
			snprintf(link, sizeof(link), "%s/%s", runuser, base);
			(void)unlink(link);
			char sock[PATH_MAX];
			snprintf(sock, sizeof(sock), "/run/host/xdg/%s", base);
			make_symlink(sock, link);
		}
	}
}

static void stage_xdg_runtime(sandbox_t* sb, const sandbox_plan_t* plan) {
	if (!plan_uses_xdg_runtime(plan) ||
	    access(plan->xdg_runtime_dir, R_OK) != 0)
		return;

	char runuser[PATH_MAX], xdgdir[PATH_MAX];
	snprintf(runuser, sizeof(runuser), "%s/run/user/%u", sb->jailroot,
		 (unsigned)plan->uid);
	snprintf(xdgdir, sizeof(xdgdir), "%s/.flatpak/%s/xdg-run",
		 plan->xdg_runtime_dir, sb->app_id);
	if (mkdir_app_xdg_dir(plan, sb->app_id, "xdg-run") != 0) {
		fpkg_warnf("cannot create %s: %s", xdgdir, strerror(errno));
		return;
	}
	if (m_nullfs(sb, xdgdir, runuser, false) != 0) {
		fpkg_warnf("could not bind per-app xdg-run dir, falling back "
			   "to unioning the host XDG_RUNTIME_DIR");
		if (m_unionfs(sb, plan->xdg_runtime_dir, runuser, true) != 0)
			fpkg_warnf("could not expose XDG_RUNTIME_DIR");
		return;
	}

	{
		char fi[PATH_MAX];
		snprintf(fi, sizeof(fi), "%s/flatpak-info", runuser);
		make_symlink("../../../.flatpak-info", fi);
	}

	char hostxdg[PATH_MAX];
	snprintf(hostxdg, sizeof(hostxdg), "%s/run/host/xdg", sb->jailroot);
	bool hostxdg_bound = false;
	if (plan_uses_xdg_runtime(plan) &&
	    access(plan->xdg_runtime_dir, R_OK) == 0) {
		if (m_nullfs(sb, plan->xdg_runtime_dir, hostxdg, true) == 0)
			hostxdg_bound = true;
		else
			fpkg_warnf("could not expose host XDG_RUNTIME_DIR");
	}
	stage_xdg_socket_links(sb, plan, runuser, hostxdg_bound);
}

static int stage_pulseaudio(sandbox_t* sb, const sandbox_plan_t* plan) {
	char psock[PATH_MAX];
	bool remote = false;

	if (pulse_socket_path(plan, psock, sizeof(psock), &remote) == 0 &&
	    !remote) {
		char pdir[PATH_MAX], base[PATH_MAX];
		char* slash;
		snprintf(pdir, sizeof(pdir), "%s", psock);
		slash = strrchr(pdir, '/');
		if (slash != NULL) {
			*slash = '\0';
			snprintf(base, sizeof(base), "%s", slash + 1);
		} else {
			snprintf(pdir, sizeof(pdir), "/");
			snprintf(base, sizeof(base), "%s", psock);
		}
		// The socket's directory is what gets mounted, so a socket in
		// /tmp would hand the jail the whole of the host's /tmp.
		if (socket_dir_too_broad(pdir)) {
			fpkg_warnf("refusing to expose pulseaudio socket %s: "
				   "%s is a shared directory",
				   psock, pdir);
			return 0;
		}
		char hostdir[PATH_MAX], to[PATH_MAX];
		snprintf(hostdir, sizeof(hostdir), "%s/run/flatpak/pulse-host",
			 sb->jailroot);
		snprintf(to, sizeof(to), "%s/run/flatpak/pulse",
			 sb->jailroot);
		if (mkdir_p(hostdir, 0755) != 0 || mkdir_p(to, 0755) != 0)
			return -1;
		if (m_nullfs(sb, pdir, hostdir, true) == 0) {
			char jtarget[PATH_MAX], link[PATH_MAX];
			sb->pulse_bound = true;
			snprintf(jtarget, sizeof(jtarget),
				 "/run/flatpak/pulse-host/%s", base);
			snprintf(link, sizeof(link), "%s/%s", to, base);
			make_symlink(jtarget, link);
			snprintf(link, sizeof(link), "%s/config", to);
			if (write_file(link, "enable-shm=no\n", 0644) != 0)
				fpkg_warnf("could not write pulse client "
					   "config");
			snprintf(link, sizeof(link), "%s/run/user/%u/pulse",
				 sb->jailroot, (unsigned)plan->uid);
			make_symlink("/run/flatpak/pulse", link);
		} else {
			fpkg_warnf("could not expose pulse socket %s", psock);
		}
	} else if (!remote) {
		fpkg_infof("could not find pulseaudio socket");
	} else {
		sb->pulse_remote = true;
	}
	return 0;
}

static int stage_system_bus(sandbox_t* sb, const sandbox_plan_t* plan) {
	bool sys_proxied = dbus_system_bus_proxied(plan);

	if (plan->sock_system_bus && !sys_proxied) {
		const char* hsock =
			access("/var/run/dbus/system_bus_socket", R_OK) == 0
				? "/var/run/dbus/system_bus_socket"
				: "/run/dbus/system_bus_socket";
		if (bind_host_socket_dir(sb, hsock, "/run/dbus") != 0)
			fpkg_warnf("could not expose system bus (%s)", hsock);
	} else if (sys_proxied) {
		char to[PATH_MAX];
		snprintf(to, sizeof(to), "%s/run/dbus", sb->jailroot);
		if (mkdir_p(to, 0755) != 0) return -1;
	}
	return 0;
}

static void stage_ssh_agent(sandbox_t* sb, const sandbox_plan_t* plan) {
	if (plan->sock_ssh_auth) {
		const char* sock = cli_getenv("SSH_AUTH_SOCK");
		struct stat sst;

		if (sock && *sock && stat(sock, &sst) == 0) {
			if (bind_host_socket_dir(sb, sock,
						 "/run/flatpak/ssh-agent") ==
			    0)
				sb->ssh_auth_bound = true;
			else
				fpkg_warnf("could not expose ssh agent (%s)",
					   sock);
		}
	}
}

static void stage_pcsc(sandbox_t* sb, const sandbox_plan_t* plan) {
	if (plan->sock_pcsc) {
		char hsock[PATH_MAX];

		if (pcsc_socket_path(hsock, sizeof(hsock)) == 0 &&
		    bind_host_socket_dir(sb, hsock, "/run/pcscd") == 0) {
			const char* base = strrchr(hsock, '/');
			base = base ? base + 1 : hsock;
			sb->pcsc_socket = xasprintf("/run/pcscd/%s", base);
		} else {
			fpkg_infof("could not find pcscd socket");
		}
	}
}

static void stage_cups(sandbox_t* sb, const sandbox_plan_t* plan) {
	if (plan->sock_cups) {
		char hsock[PATH_MAX];

		if (cups_socket_path(plan, hsock, sizeof(hsock)) == 0 &&
		    bind_host_socket_dir(sb, hsock, "/run/cups") == 0) {
			const char* base = strrchr(hsock, '/');
			base = base ? base + 1 : hsock;
			sb->cups_server = xasprintf("/run/cups/%s", base);
		} else {
			fpkg_infof("could not find CUPS socket");
		}
	}
}

static void stage_gpg_agent(sandbox_t* sb, const sandbox_plan_t* plan) {
	if (plan->sock_gpg_agent) {
		char hsock[PATH_MAX], jdir[PATH_MAX];

		snprintf(jdir, sizeof(jdir), "/run/user/%u/gnupg",
			 (unsigned)plan->uid);
		if (gpg_agent_socket(plan, hsock, sizeof(hsock)) == 0 &&
		    bind_host_socket_dir(sb, hsock, jdir) == 0)
			;
		else
			fpkg_infof("could not find gpg-agent socket");
	}
}

static void stage_gssproxy(sandbox_t* sb, const sandbox_plan_t* plan) {
	if (plan->want_network && plan->xdg_runtime_dir &&
	    *plan->xdg_runtime_dir) {
		char gss[PATH_MAX], to[PATH_MAX];

		snprintf(gss, sizeof(gss), "%s/gssproxy",
			 plan->xdg_runtime_dir);
		if (access(gss, R_OK) == 0) {
			snprintf(to, sizeof(to), "%s/run/flatpak/gssproxy",
				 sb->jailroot);
			if (mkdir_p(to, 0755) == 0 &&
			    m_nullfs(sb, gss, to, true) == 0) {
				char link[PATH_MAX];
				snprintf(link, sizeof(link),
					 "%s/run/user/%u/gssproxy",
					 sb->jailroot,
					 (unsigned)plan->uid);
				make_symlink("/run/flatpak/gssproxy", link);
			} else {
				fpkg_warnf("could not expose gssproxy");
			}
		}
	}
}

static int stage_document_portal(sandbox_t* sb,
				 const sandbox_plan_t* plan) {
	char doc_host[PATH_MAX], doc_byapp[PATH_MAX];
	char doc_to[PATH_MAX];
	const char* doc_src = NULL;
	bool doc_ro = true;

	snprintf(doc_host, sizeof(doc_host), "%s/doc",
		 plan->xdg_runtime_dir);
	snprintf(doc_byapp, sizeof(doc_byapp), "%s/by-app/%s", doc_host,
		 sb->app_id);
	snprintf(doc_to, sizeof(doc_to), "%s/run/flatpak/doc", sb->jailroot);
	if (access(doc_byapp, R_OK) == 0) {
		doc_src = doc_byapp;
		doc_ro = false;
	} else if (access(doc_host, R_OK) == 0) {
		doc_src = doc_host;
		doc_ro = true;
	}
	if (doc_src != NULL) {
		if (mkdir_p(doc_to, 0755) != 0) {
			fpkg_errorf("mkdir %s: %s", doc_to, strerror(errno));
			return -1;
		}
		if (m_nullfs(sb, doc_src, doc_to, doc_ro) != 0)
			fpkg_warnf("could not expose the document portal dir");
	}
	return 0;
}

static int stage_sockets(sandbox_t* sb, const sandbox_plan_t* plan) {
	if (stage_x11(sb, plan) != 0) return -1;
	stage_xdg_runtime(sb, plan);
	if (stage_pulseaudio(sb, plan) != 0) return -1;
	if (stage_system_bus(sb, plan) != 0) return -1;
	stage_ssh_agent(sb, plan);
	stage_pcsc(sb, plan);
	stage_cups(sb, plan);
	stage_gpg_agent(sb, plan);
	stage_gssproxy(sb, plan);
	if (stage_document_portal(sb, plan) != 0) return -1;
	return 0;
}

static int stage_flatpak_info(sandbox_t* sb) {
	{
		char mnt[PATH_MAX];
		snprintf(mnt, sizeof(mnt), "%s/run/flatpak/.flatpak",
			 sb->jailroot);
		if (mkdir_p(mnt, 0755) != 0) {
			fpkg_errorf("mkdir %s: %s", mnt, strerror(errno));
			return -1;
		}
		if (sb->instance_dir && sb->instance_id) {
			char inst[PATH_MAX];
			snprintf(inst, sizeof(inst),
				 "%s/run/flatpak/.flatpak/%s", sb->jailroot,
				 sb->instance_id);
			if (mkdir_p(inst, 0755) != 0) {
				fpkg_errorf("mkdir %s: %s", inst,
					    strerror(errno));
				return -1;
			}
			if (m_nullfs(sb, sb->instance_dir, inst, true) != 0)
				fpkg_warnf("could not bind the instance "
					   "dir into the jail");
		}
		char fpkginfo[PATH_MAX];
		snprintf(fpkginfo, sizeof(fpkginfo), "%s/.flatpak-info",
			 sb->jailroot);
		if (write_file(fpkginfo, "", 0644) != 0) {
			fpkg_errorf("write %s: %s", fpkginfo, strerror(errno));
			return -1;
		}
		snprintf(fpkginfo, sizeof(fpkginfo), "%s/run/.flatpak-info",
			 sb->jailroot);
		if (write_file(fpkginfo, "", 0644) != 0) {
			fpkg_errorf("write %s: %s", fpkginfo, strerror(errno));
			return -1;
		}
	}
	return 0;
}

static int stage_proc_sys(sandbox_t* sb) {
	char p[PATH_MAX];
	snprintf(p, sizeof(p), "%s/proc", sb->jailroot);
	if (m_special_opts(sb, "linprocfs", p, "hidepid=2") != 0) {
		fpkg_warnf("linprocfs hidepid=2 failed, retrying without");
		if (m_special(sb, "linprocfs", p) != 0) return -1;
	}
	snprintf(p, sizeof(p), "%s/sys", sb->jailroot);
	char sysdir[PATH_MAX];
	snprintf(sysdir, sizeof(sysdir), "%s/sys/.sys", sb->jailroot);
	if (m_special(sb, "linsysfs", sysdir) != 0) return -1;
	return 0;
}

static int stage_drm(sandbox_t* sb, const sandbox_plan_t* plan) {
	{
		char driver[64], slot[64] = "", vendor[64] = "", devid[64] = "",
				 subvendor[64] = "", subdevid[64] = "",
				 devroot[PATH_MAX] = "";
		char idpath[PATH_MAX], drvdir[PATH_MAX];
		static const char* idkeys[] = {"vendor", "device",
					       "subsystem_vendor",
					       "subsystem_device"};
		char* idvals[] = {vendor, devid, subvendor, subdevid};

		drm_kernel_driver(driver, sizeof(driver));
		snprintf(devroot, sizeof(devroot),
			 "%s/sys/.sys/class/drm/card0/device", sb->jailroot);
		for (size_t i = 0; i < sizeof(idkeys) / sizeof(idkeys[0]);
		     i++) {
			snprintf(idpath, sizeof(idpath), "%s/%s", devroot,
				 idkeys[i]);
			read_sysfs_value(idpath, idvals[i], sizeof(vendor));
		}
		snprintf(idpath, sizeof(idpath), "%s/uevent", devroot);
		read_sysfs_key(idpath, "PCI_SLOT_NAME", slot, sizeof(slot));

		if (driver[0] != '\0') {
			snprintf(drvdir, sizeof(drvdir), "%s/sys/.drmdrv/%s",
				 sb->jailroot, driver);
			mkdir_p(drvdir, 0755);
		}
		snprintf(drvdir, sizeof(drvdir), "%s/sys/.drmbus/pci",
			 sb->jailroot);
		mkdir_p(drvdir, 0755);

		write_drm_node(sb->jailroot, "class/drm/card0", 226, 0, "card0",
			       driver, slot, "pci", vendor, devid, subvendor,
			       subdevid);
		write_drm_node(sb->jailroot, "class/drm/renderD128", 226, 128,
			       "renderD128", driver, slot, "pci", vendor, devid,
			       subvendor, subdevid);
		write_drm_node(sb->jailroot, "dev/char/226:0", 226, 0, "card0",
			       driver, slot, "pci", vendor, devid, subvendor,
			       subdevid);
		write_drm_node(sb->jailroot, "dev/char/226:128", 226, 128,
			       "renderD128", driver, slot, "pci", vendor, devid,
			       subvendor, subdevid);

		if (plan->sock_wayland) {
			const char* shim =
				"/usr/local/lib/flatpkg/dmabuf_devt_shim.so";

			if (access(shim, R_OK) == 0) {
				char dir[PATH_MAX], to[PATH_MAX];

				snprintf(dir, sizeof(dir), "%s/run/flatpak",
					 sb->jailroot);
				mkdir_p(dir, 0755);
				snprintf(to, sizeof(to),
					 "%s/dmabuf_devt_shim.so", dir);
				if (write_file(to, "", 0600) != 0)
					fpkg_warnf("cannot create %s", to);
				if (m_nullfs(sb, shim, to, false) != 0)
					fpkg_warnf(
						"could not bind the DRM "
						"dev_t shim into the sandbox");
			} else {
				fpkg_warnf("%s is missing; Wayland GL stays "
					   "on the software path",
					   shim);
			}
		}
	}
	return 0;
}

static int stage_sys_links(sandbox_t* sb) {
	char sysdir[PATH_MAX];
	snprintf(sysdir, sizeof(sysdir), "%s/sys/.sys", sb->jailroot);
	{
		char linkpath[PATH_MAX], lnsrc[PATH_MAX];
		DIR* d = opendir(sysdir);
		if (d) {
			struct dirent* ent;
			while ((ent = readdir(d))) {
				if (ent->d_name[0] == '.') continue;
				snprintf(linkpath, sizeof(linkpath),
					 "%s/sys/%s", sb->jailroot,
					 ent->d_name);
				snprintf(lnsrc, sizeof(lnsrc), "../.sys/%s",
					 ent->d_name);
				if (lstat(linkpath, &(struct stat){0}) != 0)
					(void)symlink(lnsrc, linkpath);
			}
			closedir(d);
		}
	}
	return 0;
}

static int stage_network(sandbox_t* sb, const sandbox_plan_t* plan) {
	if (plan->want_network) {
		// hosts is not in this list: build_etc() stages a reordered
		// copy.
		static const char* etc_binds[] = {"resolv.conf", "host.conf",
						  "gai.conf", NULL};
		for (int i = 0; etc_binds[i]; i++) {
			char from[PATH_MAX], to[PATH_MAX];
			snprintf(from, sizeof(from), "/etc/%s", etc_binds[i]);
			snprintf(to, sizeof(to), "%s/etc/%s", sb->jailroot,
				 etc_binds[i]);
			if (access(from, R_OK) == 0) {
				(void)unlink(to);
				if (ensure_file(to, NULL) == 0)
					m_nullfs(sb, from, to, true);
			}
		}
	}
	return 0;
}

static int stage_host_zoneinfo(sandbox_t* sb) {
	char p[PATH_MAX];
	{
		struct stat st;
		snprintf(p, sizeof(p), "%s/usr/share/zoneinfo", sb->jailroot);
		if (stat("/usr/share/zoneinfo", &st) == 0 &&
		    S_ISDIR(st.st_mode) && stat(p, &st) == 0 &&
		    S_ISDIR(st.st_mode)) {
			if (m_nullfs(sb, "/usr/share/zoneinfo", p, true) != 0) {
				fpkg_warnf("Failed to mount zoneinfo, timezone "
					   "lookups may fail");
			}
		} else {
			fpkg_warnf("Zoneinfo not available, named timezones "
				   "may not work");
		}
	}
	return 0;
}

static int stage_font_cache(sandbox_t* sb, const sandbox_plan_t* plan) {
	{
		char fc[PATH_MAX];
		// Bind the first existing system font cache read-only so the
		// sandbox reuses the host's; otherwise fall back to a tmpfs.
		static const char* sys_caches[] = {"/var/cache/fontconfig",
						   "/usr/lib/fontconfig/cache",
						   NULL};
		bool bound = false;
		for (int i = 0; sys_caches[i]; i++) {
			struct stat st;
			if (stat(sys_caches[i], &st) != 0 ||
			    !S_ISDIR(st.st_mode))
				continue;
			snprintf(fc, sizeof(fc), "%s/run/host/fonts-cache",
				 sb->jailroot);
			if (mkdir_p(fc, 0755) == 0)
				m_nullfs(sb, sys_caches[i], fc, true);
			bound = true;
			break;
		}
		if (!bound) {
			snprintf(fc, sizeof(fc), "%s/run/host/fonts-cache",
				 sb->jailroot);
			if (m_tmpfs(sb, fc) != 0)
				fpkg_warnf("could not tmpfs %s", fc);
		}
		// Same for the user's fontconfig cache: bind read-only, else tmpfs.
		char ucache[PATH_MAX], ust[PATH_MAX];
		snprintf(ucache, sizeof(ucache), "%s/.cache/fontconfig",
			 plan->home_dir);
		struct stat ustat;
		snprintf(ust, sizeof(ust), "%s/run/host/user-fonts-cache",
			 sb->jailroot);
		if (stat(ucache, &ustat) == 0 && S_ISDIR(ustat.st_mode)) {
			if (mkdir_p(ust, 0755) == 0)
				m_nullfs(sb, ucache, ust, true);
		} else {
			snprintf(fc, sizeof(fc), "%s/run/host/user-fonts-cache",
				 sb->jailroot);
			if (m_tmpfs(sb, fc) != 0)
				fpkg_warnf("could not tmpfs %s", fc);
		}
	}
	return 0;
}

static int stage_icons(sandbox_t* sb, const sandbox_plan_t* plan) {
	{
		struct stat st;
		static const char* icon_dirs[] = {"/usr/local/share/icons",
						  "/usr/share/icons", NULL};
		for (int i = 0; icon_dirs[i]; i++) {
			if (stat(icon_dirs[i], &st) != 0 ||
			    !S_ISDIR(st.st_mode))
				continue;
			char ic[PATH_MAX];
			snprintf(ic, sizeof(ic), "%s/run/host/share/icons",
				 sb->jailroot);
			if (mkdir_p(ic, 0755) == 0)
				m_nullfs(sb, icon_dirs[i], ic, true);
			break;
		}
		char uic[PATH_MAX];
		snprintf(uic, sizeof(uic), "%s/.local/share/icons",
			 plan->home_dir);
		if (stat(uic, &st) == 0 && S_ISDIR(st.st_mode)) {
			char ic[PATH_MAX];
			snprintf(ic, sizeof(ic), "%s/run/host/user-share/icons",
				 sb->jailroot);
			if (mkdir_p(ic, 0755) == 0) m_nullfs(sb, uic, ic, true);
		}
	}
	return 0;
}

static int stage_fonts(sandbox_t* sb, const sandbox_plan_t* plan) {
	char p[PATH_MAX];
	{
		struct stat st0;
		struct {
			const char* host;
			const char* jail;
		} fonts[] = {
			{"/usr/share/fonts", "/run/host/fonts"},
			{"/usr/local/share/fonts", "/run/host/local-fonts"},
			{NULL, NULL}};
		for (int i = 0; fonts[i].host; i++) {
			if (stat(fonts[i].host, &st0) != 0 ||
			    !S_ISDIR(st0.st_mode))
				continue;
			snprintf(p, sizeof(p), "%s%s", sb->jailroot,
				 fonts[i].jail);
			mkdir_p(p, 0755);
			m_nullfs(sb, fonts[i].host, p, true);
		}
		static const char* user_dirs[] = {".local/share/fonts",
						  ".fonts", NULL};
		for (int i = 0; user_dirs[i]; i++) {
			char ud[PATH_MAX];
			snprintf(ud, sizeof(ud), "%s/%s", plan->home_dir,
				 user_dirs[i]);
			if (stat(ud, &st0) != 0 || !S_ISDIR(st0.st_mode))
				continue;
			snprintf(p, sizeof(p), "%s/run/host/user-fonts",
				 sb->jailroot);
			mkdir_p(p, 0755);
			m_nullfs(sb, ud, p, true);
			break;
		}
	}
	return 0;
}

// The host's own top-level directories, as a `host` grant gives them.  Laid
// down before the explicit grants so a narrower grant mounts on top of this
// view instead of being covered by it - a later mount is what the app sees.
static int stage_host_mounts(sandbox_t* sb, const sandbox_plan_t* plan) {
	static const char* protected_dirs[] = {
		".",	  "..",	   "app",  "bin",  "boot",	"compat",
		"dev",	  "efi",   "etc",  "lib",  "lib32", "lib64",
		"proc",	  "root",  "run",  "sbin", "sys",   "tmp",
		"usr",	  "var",   NULL};
	DIR* d;
	bool ro = true;

	assert(sb != NULL);
	assert(plan != NULL);
	if (!plan_has_host_grant(plan)) return 0;
	for (size_t i = 0; i < plan->ngrants; i++)
		if (plan->grants[i].mode != FS_GRANT_HIDE &&
		    strcmp(plan->grants[i].jail_path, "/run/host") == 0)
			ro = plan->grants[i].mode == FS_GRANT_RO;
	d = opendir("/");
	if (d == NULL) return 0;
	{
		struct dirent* de;

		while ((de = readdir(d)) != NULL) {
			bool skip = false;
			char from[PATH_MAX], to[PATH_MAX];
			struct stat st;

			for (int i = 0; protected_dirs[i]; i++)
				if (strcmp(protected_dirs[i], de->d_name) == 0) {
					skip = true;
					break;
				}
			if (skip) continue;
			snprintf(from, sizeof(from), "/%s", de->d_name);
			if (stat(from, &st) != 0) continue;
			snprintf(to, sizeof(to), "%s/%s", sb->jailroot,
				 de->d_name);
			if (sb_is_mounted(sb, to)) continue;
			{
				const char* why = NULL;

				if (host_mount_narrowed(plan, de->d_name, ro,
							&why)) {
					fpkg_infof("not mounting host '/%s': "
						   "'%s' asks for the other "
						   "mode",
						   de->d_name, why);
					continue;
				}
			}
			if (S_ISDIR(st.st_mode)) {
				if (mkdir_p(to, 0755) != 0) continue;
				m_nullfs(sb, from, to, ro);
			} else if (S_ISREG(st.st_mode)) {
				if (ensure_file(to, NULL) == 0)
					m_nullfs(sb, from, to, ro);
			}
		}
	}
	closedir(d);
	return 0;
}

// /run/host/<name> views of files the builder staged; last, because their
// sources are the builder's own.
static int stage_host_overlays(sandbox_t* sb) {
	static const char* const overlays[] = {"container-manager",
					       "font-dirs.xml", NULL};

	assert(sb != NULL);
	for (int i = 0; overlays[i] != NULL; i++) {
		char from[PATH_MAX], to[PATH_MAX];

		snprintf(from, sizeof(from), "%s/run/%s", sb->jailroot,
			 overlays[i]);
		snprintf(to, sizeof(to), "%s/run/host/%s", sb->jailroot,
			 overlays[i]);
		if (access(from, R_OK) == 0 && ensure_file(to, NULL) == 0)
			m_nullfs(sb, from, to, true);
	}
	return 0;
}

static int stage_store_links(sandbox_t* sb) {
	// The store is canonical at /var/lib/flatpkg.  Flatpak applications
	// still look for /var/lib/flatpak, so keep that as a symlink.
	{
		char to[PATH_MAX], link[PATH_MAX];
		snprintf(to, sizeof(to), "%s/var/lib/flatpkg", sb->jailroot);
		if (mkdir_p(to, 0755) == 0) {
			if (sb_is_mounted(sb, to))
				fpkg_infof(
					"'/var/lib/flatpkg' is already "
					"bound, keeping that mount instead of "
					"the read-only store default");
			else
				m_nullfs(sb, store_root(), to, true);
			snprintf(link, sizeof(link), "%s/var/lib/flatpak",
				 sb->jailroot);
			(void)unlink(link);
			if (symlink("flatpkg", link) != 0)
				fpkg_warnf("cannot create %s: %s", link,
					   strerror(errno));
		}
	}
	return 0;
}

int sandbox_build_root(sandbox_t* sb, const sandbox_plan_t* plan,
		       const fp_metadata_t* meta) {
	assert(sb != NULL);
	assert(plan != NULL);
	char tmpl[PATH_MAX];
	sb->root_lock_fd = -1;
	snprintf(tmpl, sizeof(tmpl), "%s/%s.XXXXXX", FLATPKG_RUN_DIR,
		 sb->app_id);

	if (mkdir_p(FLATPKG_RUN_DIR, 0755) != 0) {
		fpkg_errorf("mkdir %s: %s", FLATPKG_RUN_DIR, strerror(errno));
		return -1;
	}

	char* root = mkdtemp(tmpl);
	if (!root) {
		fpkg_errorf("mkdtemp %s: %s", tmpl, strerror(errno));
		return -1;
	}
	sb->jailroot = xstrdup(root);
	{
		char lock[PATH_MAX];
		int fd;

		snprintf(lock, sizeof(lock), "%s/.lock", root);
		fd = open(lock, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
		if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
			if (fd >= 0) close(fd);
			fpkg_warnf("cannot lock %s: %s", lock, strerror(errno));
		} else {
			sb->root_lock_fd = fd;
		}
	}

	if (chmod(root, 0755) != 0)
		fpkg_warnf("chmod %s: %s", root, strerror(errno));

	if (build_skeleton(sb, plan) != 0) return -1;
	if (stage_shims(sb) != 0) return -1;
	if (stage_runtime(sb, plan) != 0) return -1;
	if (stage_spawn_shim(sb) != 0) return -1;
	if (stage_app(sb, meta) != 0) return -1;
	if (stage_etc(sb) != 0) return -1;
	if (stage_ld_cache(sb) != 0) return -1;
	if (stage_time(sb) != 0) return -1;
	if (stage_var(sb, plan) != 0) return -1;
	if (stage_p11kit(sb, plan) != 0) return -1;
	if (stage_host_mounts(sb, plan) != 0) return -1;
	if (stage_home(sb, plan) != 0) return -1;
	if (stage_grants(sb, plan) != 0) return -1;
	if (stage_sockets(sb, plan) != 0) return -1;
	if (stage_flatpak_info(sb) != 0) return -1;
	if (stage_proc_sys(sb) != 0) return -1;
	if (stage_drm(sb, plan) != 0) return -1;
	if (stage_sys_links(sb) != 0) return -1;
	if (stage_network(sb, plan) != 0) return -1;
	if (stage_host_zoneinfo(sb) != 0) return -1;
	if (stage_font_cache(sb, plan) != 0) return -1;
	if (stage_icons(sb, plan) != 0) return -1;
	if (stage_fonts(sb, plan) != 0) return -1;
	if (stage_host_overlays(sb) != 0) return -1;
	if (stage_store_links(sb) != 0) return -1;

	return 0;
}

void stage_dev_shm(sandbox_t* sb, const sandbox_plan_t* plan) {
	char shmdir[PATH_MAX];

	snprintf(shmdir, sizeof(shmdir), "%s/dev/shm", sb->jailroot);
	if (plan->dev_shm) {
		return;
	} else if (plan->per_app_dev_shm &&
		   access(plan->xdg_runtime_dir, R_OK) == 0) {
		char shm_host[PATH_MAX];
		snprintf(shm_host, sizeof(shm_host), "%s/.flatpak/%s/dev-shm",
			 plan->xdg_runtime_dir, sb->app_id);
		if (mkdir_app_xdg_dir(plan, sb->app_id, "dev-shm") == 0) {
			chmod(shm_host, 01777);
			if (m_nullfs(sb, shm_host, shmdir, false) != 0) {
				fpkg_warnf("could not bind per-app /dev/shm, "
					   "falling back to tmpfs");
				if (m_tmpfs_sized(sb, shmdir, TMPFS_SIZE_SHM) != 0)
					fpkg_warnf("could not mount /dev/shm");
			}
		} else {
			fpkg_warnf("could not create per-app /dev/shm dir, "
				   "falling back to tmpfs");
			if (m_tmpfs(sb, shmdir) != 0)
				fpkg_warnf("could not mount /dev/shm");
		}
	} else if (m_tmpfs_sized(sb, shmdir, TMPFS_SIZE_SHM) != 0) {
		fpkg_warnf("could not mount /dev/shm; Wayland shm and MIT-SHM "
			   "may fail");
	}
}

// Close every inherited fd above stderr but the forwarded ones: jail_attach(2)
// refuses to chroot while any directory fd is open. Never below 3.
void close_inherited_fds(void) {
	unsigned keep[64];
	size_t nkeep = 0;
	const char* fw = cli_getenv("FLATPKG_FORWARD_FDS");

	if (fw && *fw) {
		const char* s = fw;
		while (*s && nkeep < sizeof(keep) / sizeof(keep[0])) {
			char* end;
			long v = strtol(s, &end, 10);
			if (end == s) break;
			if (v > 2 && v < 65536 && fcntl((int)v, F_GETFD) >= 0)
				keep[nkeep++] = (unsigned)v;
			s = (*end == ',') ? end + 1 : end;
		}
	}
	for (size_t i = 1; i < nkeep; i++) {
		unsigned v = keep[i];
		size_t j = i;
		while (j > 0 && keep[j - 1] > v) {
			keep[j] = keep[j - 1];
			j--;
		}
		keep[j] = v;
	}
	unsigned low = 3;
	for (size_t i = 0; i < nkeep; i++) {
		if (keep[i] > low) (void)close_range(low, keep[i] - 1, 0);
		low = keep[i] + 1;
	}
	(void)close_range(low, ~0u, 0);
	unsetenv("FLATPKG_FORWARD_FDS");
}
