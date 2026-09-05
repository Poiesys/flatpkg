// The jail root, jail_set(2), exec and ordered teardown.
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

// How long a launch waits for the runtime's ldconfig before it gives up.
#ifndef FLATPKG_LDCONFIG_TIMEOUT_S
#define FLATPKG_LDCONFIG_TIMEOUT_S 120
#endif

#include "flatpkg.h"
#define FPKG_MODULE "sandbox"

#include "fpkglog.h"

// Set when the worker's peer goes away: the relay must stop reading the
// caller's terminal, or it steals the shell's input until the app exits.
static volatile sig_atomic_t g_client_gone;

void fpkg_client_mark_gone(void) { g_client_gone = 1; }
bool fpkg_client_gone(void) { return g_client_gone != 0; }

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

static fpkg_error_t write_file(const char* path, const char* content,
			       mode_t mode) {
	assert(path != NULL);
	assert(content != NULL);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0) return fpkg_error_from_errno(errno);
	size_t len = strlen(content);
	ssize_t w = write(fd, content, len);
	int e = errno;
	close(fd);
	if (w != (ssize_t)len) return e ? fpkg_error_from_errno(e) : FPKG_EIO;
	return FPKG_OK;
}

static void write_instance_pid(sandbox_t* sb, pid_t pid) {
	assert(sb != NULL);
	if (!sb->instance_dir) return;
	char buf[32];
	int n = snprintf(buf, sizeof(buf), "%ld\n", (long)pid);

	char p[PATH_MAX];
	snprintf(p, sizeof(p), "%s/pid", sb->instance_dir);
	int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		if (write(fd, buf, (size_t)n) != (ssize_t)n)
			fpkg_warnf("write %s: %s", p, strerror(errno));
		close(fd);
	} else {
		fpkg_warnf("cannot create pid file %s: %s", p, strerror(errno));
	}

	if (sb->jailroot && sb->instance_id) {
		snprintf(p, sizeof(p), "%s/run/flatpak/.flatpak/%s/pid",
			 sb->jailroot, sb->instance_id);
		fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd >= 0) {
			if (write(fd, buf, (size_t)n) != (ssize_t)n)
				fpkg_warnf("write %s: %s", p, strerror(errno));
			close(fd);
		}
	}

	instance_write_bwrapinfo(sb->instance_dir, sb->jailroot, (long)pid);
}

static int make_symlink(const char* target, const char* linkpath) {
	assert(target != NULL);
	assert(linkpath != NULL);
	unlink(linkpath);
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
	if (strcmp(dir, "/tmp") == 0 || strcmp(dir, "/var/tmp") == 0 ||
	    strcmp(dir, "/var/run") == 0 || strcmp(dir, "/run") == 0 ||
	    strcmp(dir, "/var") == 0 || strcmp(dir, "/dev") == 0) {
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
		char cmd[PATH_MAX];
		snprintf(cmd, sizeof(cmd),
			 "%s --list-dir agent-socket "
			 "2>/dev/null",
			 gpgconfs[i]);
		FILE* f = popen(cmd, "r");
		if (f == NULL) continue;
		bool got = fgets(out, (int)n, f) != NULL;
		pclose(f);
		if (!got) continue;
		char* e = out + strlen(out);
		while (e > out && (e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';
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

static int run_argv(char* const argv[]) {
	assert(argv != NULL);
	assert(argv[0] != NULL);
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		execv(argv[0], argv);
		_exit(127);
	}
	int status = 0;
	if (waitpid(pid, &status, 0) < 0) return -1;
	if (!WIFEXITED(status)) return -1;
	return WEXITSTATUS(status);
}

static void sb_track(sandbox_t* sb, const char* fstype, const char* from,
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
		if (strcmp(sb->mounts[i].to, path) == 0) return true;
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

static int do_nmount(const char* fstype, const char* from, const char* to,
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

static int mount_tool(sandbox_t* sb, const char* tool, char* const args[],
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
	int rc = run_argv(argv);
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
// writes a mount point through into the HOST as root, and then EDEADLKs.
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

static int m_tmpfs(sandbox_t* sb, const char* to) {
	assert(sb != NULL);
	assert(to != NULL);
	int rc;
	if (do_nmount("tmpfs", "tmpfs", to, NULL, false) == 0) {
		sb_track(sb, "tmpfs", NULL, to);
		rc = 0;
	} else {
		char* args[] = {"-t", "tmpfs", "tmpfs", (char*)to, NULL};
		rc = mount_tool(sb, "/sbin/mount", args, "tmpfs", NULL, to);
	}
	if (rc == 0)
		// Default tmpfs mode is 0755 root-owned, which the app cannot
		// write.
		(void)chmod(to, 01777);
	return rc;
}

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

static int m_special(sandbox_t* sb, const char* fstype, const char* to) {
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

static int m_special_opts(sandbox_t* sb, const char* fstype, const char* to,
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

void install_devfs_base_rulesets(void) {
	static const char* cmds[] = {
		"/sbin/devfs rule -s 1000 delset 2>/dev/null; "
		"/sbin/devfs rule -s 1000 add hide; "
		"/sbin/devfs rule -s 1000 add path 'null' unhide; "
		"/sbin/devfs rule -s 1000 add path 'zero' unhide; "
		"/sbin/devfs rule -s 1000 add path 'random' unhide; "
		"/sbin/devfs rule -s 1000 add path 'urandom' unhide; "
		"/sbin/devfs rule -s 1000 add path 'shm/*' unhide; "
		"/sbin/devfs rule -s 1000 add path 'pts/*' unhide; "
		"/sbin/devfs rule -s 1000 add path 'ptmx' unhide; "
		"/sbin/devfs rule -s 1000 add path 'tty' unhide; "
		"/sbin/devfs rule -s 1000 add path 'fd' unhide; "
		"/sbin/devfs rule -s 1000 add path 'fd/*' unhide; "
		"/sbin/devfs rule -s 1000 add path 'stdin' unhide; "
		"/sbin/devfs rule -s 1000 add path 'stdout' unhide; "
		"/sbin/devfs rule -s 1000 add path 'stderr' unhide",
		"/sbin/devfs rule -s 1001 delset 2>/dev/null; "
		"/sbin/devfs rule -s 1001 add include 1000; "
		"/sbin/devfs rule -s 1001 add path 'dri' unhide; "
		"/sbin/devfs rule -s 1001 add path 'dri/*' unhide; "
		"/sbin/devfs rule -s 1001 add path 'drm' unhide; "
		"/sbin/devfs rule -s 1001 add path 'drm/*' unhide",
		"/sbin/devfs rule -s 1002 delset 2>/dev/null; "
		"/sbin/devfs rule -s 1002 add include 1000; "
		"/sbin/devfs rule -s 1002 add path 'vmm' unhide; "
		"/sbin/devfs rule -s 1002 add path 'vmm/*' unhide",
		"/sbin/devfs rule -s 1003 delset 2>/dev/null; "
		"/sbin/devfs rule -s 1003 add include 1000; "
		"/sbin/devfs rule -s 1003 add path 'input' unhide; "
		"/sbin/devfs rule -s 1003 add path 'input/*' unhide",
		NULL};
	for (int i = 0; cmds[i]; i++) system(cmds[i]);
}

// Build a fresh devfs ruleset number >= FLATPKG_RULESET_MIN from the base
// ruleset. Released in sandbox_teardown().
static int alloc_devfs_ruleset(const sandbox_plan_t* plan) {
	assert(plan != NULL);
	if (plan->dev_all) return 0;

	char cmd[4096];
	int ruleset = -1;

	for (int attempt = 0; attempt < 2 && ruleset < 0; attempt++) {
		int lockfd = open("/var/run/flatpkg/rulesets.lock",
				  O_CREAT | O_RDWR, 0600);
		if (lockfd >= 0) flock(lockfd, LOCK_EX);

		for (int n = FLATPKG_RULESET_MIN; n <= FLATPKG_RULESET_MAX;
		     n++) {
			snprintf(cmd, sizeof(cmd),
				 "/sbin/devfs rule -s %d show >/dev/null 2>&1",
				 n);
			if (system(cmd) != 0) continue;
			snprintf(cmd, sizeof(cmd),
				 "/sbin/devfs rule -s %d delset 2>/dev/null; "
				 "/sbin/devfs rule -s %d add include %d",
				 n, n, FLATPKG_RULESET_BASE);
			if (system(cmd) == 0) {
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
	size_t off = 0;

	cmd[0] = '\0';
	for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
		if (!rules[i].on) continue;
		off += (size_t)snprintf(
			cmd + off, sizeof(cmd) - off,
			"/sbin/devfs rule -s %d add path '%s' unhide; ",
			ruleset, rules[i].path);
		if (off >= sizeof(cmd)) break;
	}
	if (off >= sizeof(cmd)) {
		fpkg_errorf("devfs ruleset %d command too long", ruleset);
		snprintf(cmd, sizeof(cmd),
			 "/sbin/devfs rule -s %d delset 2>/dev/null", ruleset);
		system(cmd);
		return -1;
	}
	if (cmd[0]) system(cmd);

	return ruleset;
}

static void close_inherited_fds(void);

// Make the host's PKCS#11 trust store reachable: reuse or start one per-user
// `p11-kit server` and bind its directory into the jail.
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
		struct dirent* de;
		while ((de = readdir(d)) != NULL) {
			if (de->d_name[0] == '.') continue;
			char node[PATH_MAX];
			snprintf(node, sizeof(node), "%s/%s", dir, de->d_name);
			struct stat st;
			if (lstat(node, &st) != 0 ||
			    !(S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)))
				continue;
			(void)chown(node, (uid_t)-1, plan->gid);
		}
		closedir(d);
	}
}

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
	unlink(file);
	symlink("../../../.sys/class/drm", file);

	snprintf(file, sizeof(file), "%s/device", dir);
	if (mkdir_p(file, 0755) != 0) return;
	char devdir[PATH_MAX];
	snprintf(devdir, sizeof(devdir), "%s", file);

	// libdrm's drmNodeIsDRM() stat()s <device>/drm; the synthesized device
	// tree replaced the link, so every node was rejected and
	// drmGetDevices2() returned 0.
	snprintf(file, sizeof(file), "%s/drm", devdir);
	if (mkdir_p(file, 0755) != 0) return;

	if (driver != NULL && *driver != '\0') {
		snprintf(file, sizeof(file), "%s/driver", devdir);
		snprintf(buf, sizeof(buf), "../../../../.drmdrv/%s", driver);
		unlink(file);
		symlink(buf, file);
	}

	snprintf(file, sizeof(file), "%s/subsystem", devdir);
	snprintf(buf, sizeof(buf), "../../../../.drmbus/%s",
		 (busname != NULL && *busname != '\0') ? busname : "pci");
	unlink(file);
	symlink(buf, file);

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

static const char* usrmerged_dirs[] = {"bin",  "lib",	"lib64",
				       "sbin", "lib32", NULL};

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
	if (chown(plan->xdg_runtime_dir, plan->uid, plan->gid) != 0 ||
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

	chmod(base, 0755);
	chown(base, plan->uid, plan->gid);
	chmod(appd, 0755);
	chown(appd, plan->uid, plan->gid);
	if (sub && sub[0]) {
		chmod(leaf, 0700);
		chown(leaf, plan->uid, plan->gid);
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
	char passwd[2048];
	snprintf(passwd, sizeof(passwd),
		 "%s:x:%u:%u:%s:%s:/bin/sh\n"
		 "nfsnobody:x:65534:65534:Unmapped user:/:/sbin/nologin\n",
		 plan->user_name, (unsigned)plan->uid, (unsigned)plan->gid,
		 plan->real_name, plan->home_dir);
	if (write_file(path, passwd, 0644) != 0) {
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

	snprintf(path, sizeof(path), "%s/machine-id", etc);
	char mid[64] = "";
	FILE* mf = fopen("/etc/machine-id", "r");
	if (!mf) mf = fopen("/var/lib/dbus/machine-id", "r");
	if (mf) {
		if (fgets(mid, sizeof(mid), mf)) mid[strcspn(mid, "\n")] = '\0';
		fclose(mf);
	}
	if (mid[0] == '\0') {
		int fd = open("/dev/urandom", O_RDONLY);
		if (fd >= 0) {
			unsigned char r[16];
			if (read(fd, r, sizeof(r)) == (ssize_t)sizeof(r)) {
				char* p = mid;
				for (size_t i = 0; i < sizeof(r); i++)
					p += snprintf(p, 3, "%02x", r[i]);
			}
			close(fd);
		}
	}
	if (mid[0]) write_file(path, mid, 0644);
	if (mid[0]) {
		snprintf(path, sizeof(path), "%s/var/lib/dbus/machine-id",
			 sb->jailroot);
		write_file(path, mid, 0644);
	}

	if (plan->want_network && stage_hosts("/etc/hosts", etc) != 0)
		return -1;

	snprintf(path, sizeof(path), "%s/ld.so.preload", etc);
	write_file(path,
		   "/.signalfd_shim.so\n/.seccomp_shim.so\n"
		   "/.memfd_shim.so\n/.syncobj_shim.so\n/.rlimit_shim.so\n"
		   "/.madvise_shim.so\n",
		   0644);

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

	// dconf migration: an app needs its ~/.config/dconf symlink and the
	// host DB dumped in, with DCONF_PROFILE pointing at it.  Best-effort.
	{
		char dconf_dir[PATH_MAX];
		snprintf(dconf_dir, sizeof(dconf_dir), "%s/run/flatpak/dconf",
			 sb->jailroot);
		if (mkdir_p(dconf_dir, 0755) == 0) {
			char dump_path[PATH_MAX];
			snprintf(dump_path, sizeof(dump_path), "%s/user",
				 dconf_dir);
			(void)chown(dconf_dir, plan->uid, plan->gid);
			write_file(dump_path, "", 0644);
			(void)chown(dump_path, plan->uid, plan->gid);
			char kf_dir[PATH_MAX];
			char kf_path[PATH_MAX * 2];
			snprintf(kf_dir, sizeof(kf_dir),
				 "%s/.var/app/%s/config/glib-2.0/settings",
				 plan->home_dir, sb->app_id);
			snprintf(kf_path, sizeof(kf_path), "%s/keyfile",
				 kf_dir);
			char app_fc[PATH_MAX];
			char host_fc[PATH_MAX];
			const char* home = cli_getenv("HOME");
			snprintf(app_fc, sizeof(app_fc),
				 "%s/.var/app/%s/cache/fontconfig",
				 plan->home_dir, sb->app_id);
			snprintf(host_fc, sizeof(host_fc),
				 "%s/.cache/fontconfig",
				 home ? home : "/nonexistent");

			char cmd[PATH_MAX * 9];
			snprintf(cmd, sizeof(cmd),
				 "/usr/local/bin/dconf dump / > '%s' "
				 "2>/dev/null "
				 "|| /usr/bin/dconf dump / > '%s' 2>/dev/null "
				 "|| true; "
				 "mkdir -p '%s' 2>/dev/null && "
				 "{ [ -e '%s' ] || cp '%s' '%s' 2>/dev/null; } "
				 "; "
				 "{ [ -d '%s' ] && [ ! -d '%s' ] && "
				 "  cp -R '%s' '%s' 2>/dev/null; } ; "
				 "true",
				 dump_path, dump_path, kf_dir, kf_path,
				 dump_path, kf_path, host_fc, app_fc, host_fc,
				 app_fc);
			pid_t pid = fork();
			if (pid == 0) {
				if (initgroups(plan->user_name, plan->gid) !=
					    0 ||
				    setgid(plan->gid) != 0 ||
				    setuid(plan->uid) != 0) {
					dprintf(2, "flatpkg: setuid: %s\n",
						strerror(errno));
					_exit(127);
				}
				execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
				_exit(127);
			}
			int ws;
			(void)waitpid(pid, &ws, 0);
		}
	}

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
	chown(p, plan->uid, plan->gid);

	// /tmp: with filesystem=host the jail keeps a private dir, otherwise the
	// per-app host dir is bound rw so the app's temp files persist.
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
		chmod(p, 01777);
	}

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

static int ensure_file(const char* path) {
	assert(path != NULL);
	int fd = open(path, O_CREAT | O_RDWR, 0600);
	if (fd < 0) return -1;
	close(fd);
	return 0;
}

static void detach_bind(const char* path) {
	assert(path != NULL);
	char cmd[PATH_MAX + 64];
	snprintf(cmd, sizeof(cmd), "/sbin/umount '%s' 2>/dev/null", path);
	(void)system(cmd);
	(void)rmdir(path);
}

static void mimic_recreate_one(sandbox_t* sb, const char* src, const char* dst,
			       const struct stat* st) {
	assert(sb != NULL);
	assert(src != NULL);
	assert(dst != NULL);
	if (S_ISDIR(st->st_mode)) {
		if (mkdir_p(dst, st->st_mode & 07777) == 0)
			(void)m_nullfs(sb, src, dst, true);
	} else if (S_ISLNK(st->st_mode)) {
		char target[PATH_MAX];
		ssize_t l = readlink(src, target, sizeof(target) - 1);
		if (l > 0) {
			target[l] = '\0';
			(void)unlink(dst);
			(void)symlink(target, dst);
		}
	} else if (S_ISREG(st->st_mode)) {
		(void)ensure_file(dst);
		(void)m_nullfs(sb, src, dst, true);
	}
}

#define LD_SO_CACHE_KEEP 4

typedef struct {
	char path[PATH_MAX];
	time_t mtime;
} ld_cache_file_t;

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

static bool xauth_write_filtered(const char* root, const char* app_id,
				 const char* home, uid_t uid, gid_t gid) {
	assert(root != NULL);
	assert(app_id != NULL);
	if (home == NULL || *home == '\0') return false;
	char src[PATH_MAX];
	snprintf(src, sizeof(src), "%s/.Xauthority", home);

	int fd = open(src, O_RDONLY);
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
	close(out);
	if (!wrote) {
		(void)unlink(tmp);
		return false;
	}
	if (rename(tmp, dst) != 0) {
		(void)unlink(tmp);
		return false;
	}
	return chown(dst, uid, gid) == 0;
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
		rmdir(safe);
		return -1;
	}

	// Capture the original dir's mode/owner before tmpfs covers it: tmpfs
	// defaults to 0755 root-owned, wrong for e.g. /usr/share.
	struct stat orig_st;
	bool have_orig_st = (lstat(safe, &orig_st) == 0);

	if (m_tmpfs(sb, target_dir) != 0) {
		fpkg_warnf("writable_mimic: tmpfs over %s failed", target_dir);
		detach_bind(safe);
		return -1;
	}
	if (have_orig_st) {
		(void)chmod(target_dir, orig_st.st_mode & 07777);
		// Restore the OWNER too, or a dir that was the user's comes
		// back root-owned and the app loses write access to its own
		// tree.
		if (orig_st.st_uid != 0 || orig_st.st_gid != 0)
			(void)chown(target_dir, orig_st.st_uid, orig_st.st_gid);
	}

	DIR* d = opendir(safe);
	if (!d) {
		fpkg_warnf("writable_mimic: opendir %s failed: %s", safe,
			   strerror(errno));
		detach_bind(safe);
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
	detach_bind(safe);
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

// The per-launch tree is the app's: everything flatpkg created goes to the
// caller, who is who the app runs as. A bind-mounted path keeps the source's
// owner.
static void own_tree_dir(sandbox_t* sb, uid_t uid, gid_t gid, const char* dir) {
	assert(sb != NULL);
	assert(dir != NULL);
	DIR* d = opendir(dir);
	if (d == NULL) return;
	struct dirent* de;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.' &&
		    (de->d_name[1] == '\0' ||
		     (de->d_name[1] == '.' && de->d_name[2] == '\0')))
			continue;
		char child[PATH_MAX];
		snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
		struct stat st;
		if (lstat(child, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
		bool bound = false;
		for (size_t i = 0; i < sb->nmounts; i++) {
			if (sb->mounts[i].to == NULL ||
			    strcmp(sb->mounts[i].to, child) != 0)
				continue;
			bound = strcmp(sb->mounts[i].fstype, "tmpfs") != 0;
		}
		if (bound) continue;
		if (chown(child, uid, gid) != 0) {
			fpkg_debugf("chown %s: %s", child, strerror(errno));
			continue;
		}
		own_tree_dir(sb, uid, gid, child);
	}
	closedir(d);
}

// Extension mounts: a needs_tmpfs extension gets a writable parent, then its
// files/ tree is ro-bound; shallowest first, since nullfs hides inner mounts.
static bool is_mount_point_dir(const char* path) {
	assert(path != NULL);
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

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
			(void)sandbox_ensure_writable_dir(sb, cand[c]);
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

static int drm_devt_map(char* out, size_t outlen) {
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
		off += (size_t)snprintf(out + off, outlen - off, "%s%s:%llu",
					off > 0 ? ";" : "", path,
					(unsigned long long)st.st_rdev);
		if (off >= outlen) break;
	}
	return 0;
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
			(void)chmod(etc_mnt, 0755);
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
						if (ensure_file(sto) == 0)
							m_nullfs(sb, sfrom, sto,
								 true);
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
					(void)ensure_file(sto);
					m_nullfs(sb, sfrom, sto, true);
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

static int stage_ld_cache(sandbox_t* sb) {
	if (sb->use_ld_so_cache && sb->ldconfig_path != NULL) {
		uint64_t hash = 0xcbf29ce484222325ULL;
		const char* sources[] = {sb->runtime_dir, sb->app_files_dir,
					 NULL};
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
		char sum[32];
		snprintf(sum, sizeof(sum), "%016llx", (unsigned long long)hash);

		char cache_dir[PATH_MAX];
		snprintf(cache_dir, sizeof(cache_dir), "%s/cache/ld.so",
			 store_root());
		mkdir_p(cache_dir, 0755);

		char cache_store[PATH_MAX];
		snprintf(cache_store, sizeof(cache_store), "%s/%s.bin",
			 cache_dir, sum);
		char cache_dest[PATH_MAX];
		snprintf(cache_dest, sizeof(cache_dest), "%s/etc/ld.so.cache",
			 sb->jailroot);

		struct stat cst;
		bool hit = stat(cache_store, &cst) == 0 &&
			   S_ISREG(cst.st_mode) && cst.st_size > 0;
		if (hit && (ensure_file(cache_dest) != 0 ||
			    m_nullfs(sb, cache_store, cache_dest, true) != 0))
			hit = false;

		if (!hit) {
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
					 sb->jailroot, sb->jailroot,
					 sb->jailroot, sb->jailroot,
					 sb->jailroot);
				setenv("LD_LIBRARY_PATH", ldp, 1);
				char* const av[] = {(char*)"ldconfig",
						    (char*)"-r", sb->jailroot,
						    NULL};
				execv(sb->ldconfig_path, av);
				_exit(127);
			} else if (lcp > 0) {
				// Bounded: a stuck ldconfig once pinned a worker, a
				// jail and its mounts.
				time_t deadline =
					time(NULL) + FLATPKG_LDCONFIG_TIMEOUT_S;
				bool killed = false;
				for (;;) {
					pid_t w =
						waitpid(lcp, &status, WNOHANG);
					if (w == lcp) break;
					if (w < 0 && errno != EINTR) break;
					if (time(NULL) >= deadline) {
						if (!killed) {
							fpkg_warnf(
								"ldconfig did "
								"not finish in "
								"%ds; "
								"starting "
								"without a "
								"library cache",
								FLATPKG_LDCONFIG_TIMEOUT_S);
							(void)kill(lcp,
								   SIGKILL);
							killed = true;
							deadline =
								time(NULL) + 5;
						} else {
							// It is stuck in the kernel; leave it.
							status = -1;
							break;
						}
					}
					usleep(50000);
				}
				if (killed) {
					unlink(cache_dest);
					status = -1;
				}
			}

			static const char want_magic[] = "glibc-ld.so.cache";
			int in_fd = open(cache_dest, O_RDONLY);
			if (in_fd >= 0) {
				char magic[sizeof(want_magic)] = {0};
				ssize_t got = read(in_fd, magic,
						   sizeof(want_magic) - 1);
				if (got != (ssize_t)(sizeof(want_magic) - 1) ||
				    memcmp(magic, want_magic,
					   sizeof(want_magic) - 1) != 0) {
					if (got > 0)
						fpkg_warnf("ldconfig produced "
							   "a non-glibc cache "
							   "(magic \"%.16s\"), "
							   "not caching",
							   magic);
					unlink(cache_dest);
				}
				close(in_fd);
			}

			in_fd = open(cache_dest, O_RDONLY);
			if (in_fd >= 0) {
				struct stat ist;
				if (fstat(in_fd, &ist) == 0 &&
				    ist.st_size > 0) {
					int out_fd = open(cache_store,
							  O_WRONLY | O_CREAT |
								  O_TRUNC,
							  0644);
					if (out_fd >= 0) {
						char buf[65536];
						ssize_t n = 0;
						bool ok = true;
						while ((n = read(in_fd, buf,
								 sizeof(buf))) >
						       0)
							if (write(out_fd, buf,
								  (size_t)n) !=
							    n) {
								ok = false;
								break;
							}
						close(out_fd);
						if (n < 0) ok = false;
						if (ok) {
							fpkg_infof(
								"ld.so.cache "
								"built and "
								"cached "
								"(%ld bytes, "
								"%s)",
								(long)ist
									.st_size,
								sum);
							prune_ld_so_caches(
								cache_dir);
						} else {
							unlink(cache_store);
						}
					}
				}
				close(in_fd);
			}
		}
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
			unlink(zdb);
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

static int stage_home(sandbox_t* sb, const sandbox_plan_t* plan) {
	bool home_granted = home_is_granted(plan);

	{
		char appto[PATH_MAX];
		snprintf(appto, sizeof(appto), "%s%s/.var/app/%s", sb->jailroot,
			 plan->home_dir, sb->app_id);
		if (home_granted) {
			char hto[PATH_MAX], hide[PATH_MAX];
			snprintf(hto, sizeof(hto), "%s%s", sb->jailroot,
				 plan->home_dir);
			if (mkdir_p(hto, 0755) != 0) {
				fpkg_errorf("mkdir %s: %s", hto,
					    strerror(errno));
				return -1;
			}
			if (m_nullfs(sb, plan->home_dir, hto, false) != 0)
				return -1;
			snprintf(hide, sizeof(hide), "%s%s/.var/app",
				 sb->jailroot, plan->home_dir);
			if (mkdir_p(hide, 0755) != 0) {
				fpkg_errorf("mkdir %s: %s", hide,
					    strerror(errno));
				return -1;
			}
			if (m_tmpfs(sb, hide) != 0) return -1;
		}
		if (mkdir_p(appto, 0700) != 0) {
			fpkg_errorf("mkdir %s: %s", appto, strerror(errno));
			return -1;
		}
		{
			char ch[PATH_MAX];
			snprintf(ch, sizeof(ch), "%s", appto);
			for (char* q = ch + strlen(sb->jailroot) + 1; *q; q++) {
				if (*q == '/') {
					*q = '\0';
					(void)chmod(ch, 0755);
					*q = '/';
				}
			}
			(void)chmod(ch, 0755);
		}
		if (m_nullfs(sb, plan->app_data_dir, appto, false) != 0)
			return -1;
	}
	return 0;
}

static int stage_grants(sandbox_t* sb, const sandbox_plan_t* plan) {
	bool home_granted = home_is_granted(plan);

	size_t missing_paths = 0;
	for (size_t i = 0; i < plan->ngrants; i++) {
		fs_grant_t* g = &plan->grants[i];

		if (g->mode != FS_GRANT_HIDE &&
		    strcmp(g->jail_path, "/run/host") == 0)
			continue;

		if (g->mode == FS_GRANT_HIDE) {
			char to[PATH_MAX];
			snprintf(to, sizeof(to), "%s%s", sb->jailroot,
				 g->jail_path);
			if (mkdir_p(to, 0755) != 0) {
				fpkg_warnf(
					"cannot create hide point %s, skipping",
					g->jail_path);
				continue;
			}
			if (m_tmpfs(sb, to) != 0)
				fpkg_warnf("could not hide %s", g->jail_path);
			continue;
		}

		if (g->persistent && home_granted) continue;

		if (g->mode == FS_GRANT_CREATE) {
			pid_t pid = fork();
			if (pid < 0) {
				fpkg_warnf("fork failed for %s, skipping "
					   "grant",
					   g->host_path);
				continue;
			}
			if (pid == 0) {
				if (initgroups(plan->user_name, plan->gid) !=
					    0 ||
				    setgid(plan->gid) != 0 ||
				    setuid(plan->uid) != 0)
					_exit(1);
				_exit(mkdir_p(g->host_path, 0755) == 0 ? 0 : 1);
			}
			int wstatus = 0;
			if (waitpid(pid, &wstatus, 0) < 0 ||
			    !WIFEXITED(wstatus) || WEXITSTATUS(wstatus) != 0) {
				fpkg_warnf("cannot create %s for grant, "
					   "skipping",
					   g->host_path);
				continue;
			}
		}

		char to[PATH_MAX];
		snprintf(to, sizeof(to), "%s%s", sb->jailroot, g->jail_path);
		{
			// Decided before making the mount point; see
			// mount_covering().
			const sb_mount_t* cover = mount_covering(
				sb->mounts, sb->nmounts, g->host_path, to);
			if (cover != NULL) {
				struct statfs cfs;
				bool cover_ro = statfs(cover->to, &cfs) == 0 &&
						(cfs.f_flags & MNT_RDONLY) != 0;
				bool want_ro = (g->mode == FS_GRANT_RO);
				if (want_ro && !cover_ro)
					fpkg_warnf(
						"'%s' is inside '%s', "
						"which is mounted "
						"read-write: it stays "
						"read-write (wanted "
						"read-only)",
						g->jail_path,
						cover->to +
							strlen(sb->jailroot));
				else
					fpkg_infof("'%s' is already visible "
						   "through the grant of %s, "
						   "skipping",
						   g->jail_path, cover->from);
				continue;
			}
		}
		if (mkdir_p(to, 0755) != 0) {
			fpkg_warnf(
				"cannot create mount point for '%s', skipping",
				g->jail_path);
			continue;
		}
		if (access(g->host_path, F_OK) != 0) {
			char* mapped = store_path_fallback(plan->home_dir,
							   g->host_path);
			if (mapped) {
				free(g->host_path);
				g->host_path = mapped;
			} else {
				fpkg_infof("skipping grant '%s' -> '%s': "
					   "no such host path",
					   g->host_path, g->jail_path);
				missing_paths++;
				continue;
			}
		}
		{
			struct stat st;
			if (lstat(g->host_path, &st) == 0 &&
			    S_ISSOCK(st.st_mode))
				continue;
		}
		if (sb_mounted_from(sb, to, g->host_path)) {
			fpkg_infof("'%s' already bound from %s, skipping "
				   "duplicate grant",
				   g->jail_path, g->host_path);
			continue;
		}
		bool ro = (g->mode == FS_GRANT_RO);
		if (m_nullfs(sb, g->host_path, to, ro) != 0) {
			fpkg_warnf("failed to bind grant '%s', continuing "
				   "without it",
				   g->host_path);
		}
	}
	if (missing_paths > 0)
		fpkg_infof("skipped %zu missing host-path grant(s)",
			   missing_paths);
	return 0;
}

static int stage_sockets(sandbox_t* sb, const sandbox_plan_t* plan) {
	{
		bool tmp_exported = false;
		for (size_t i = 0; i < plan->ngrants; i++) {
			if (strcmp(plan->grants[i].jail_path, "/tmp") == 0) {
				tmp_exported = true;
				break;
			}
		}
		char x11dir[PATH_MAX];
		snprintf(x11dir, sizeof(x11dir), "%s/tmp/.X11-unix",
			 sb->jailroot);
		if (mkdir_p(x11dir, 0755) != 0) {
			fpkg_errorf("mkdir %s: %s", x11dir, strerror(errno));
			return -1;
		}
		bool x11 = plan->sock_x11 ||
			   (plan->sock_fallback_x11 && !plan->sock_wayland);
		if (tmp_exported) {
			if (m_tmpfs(sb, x11dir) != 0) return -1;
		}
		if (x11 && access("/tmp/.X11-unix", R_OK) == 0) {
			if (m_nullfs(sb, "/tmp/.X11-unix", x11dir, true) != 0)
				fpkg_warnf("could not expose X11 sockets");
			else
				sb->xauth_written = xauth_write_filtered(
					sb->jailroot, sb->app_id,
					plan->home_dir, plan->uid, plan->gid);
		}

		// Bind the per-app host dir's wayland-<n> and -socket-bus/.
		if ((plan->sock_wayland || plan->sock_inherit_wayland ||
		     plan->sock_session_bus ||
		     plan_has_xdg_socket_grant(plan)) &&
		    access(plan->xdg_runtime_dir, R_OK) == 0) {
			char runuser[PATH_MAX], xdgdir[PATH_MAX];
			snprintf(runuser, sizeof(runuser), "%s/run/user/%u",
				 sb->jailroot, (unsigned)plan->uid);
			snprintf(xdgdir, sizeof(xdgdir),
				 "%s/.flatpak/%s/"
				 "xdg-run",
				 plan->xdg_runtime_dir, sb->app_id);
			if (mkdir_app_xdg_dir(plan, sb->app_id, "xdg-run") !=
			    0) {
				fpkg_warnf("cannot create %s: %s", xdgdir,
					   strerror(errno));
			} else {
				if (m_nullfs(sb, xdgdir, runuser, false) != 0) {
					fpkg_warnf(
						"could not bind per-app "
						"xdg-run dir, falling back to "
						"unioning the host "
						"XDG_RUNTIME_DIR");
					if (m_unionfs(sb, plan->xdg_runtime_dir,
						      runuser, true) != 0)
						fpkg_warnf("could not expose "
							   "XDG_RUNTIME_DIR");
				} else {
					{
						char fi[PATH_MAX];
						snprintf(fi, sizeof(fi),
							 "%s/flatpak-info",
							 runuser);
						make_symlink("../../../"
							     ".flatpak-info",
							     fi);
					}
					char hostxdg[PATH_MAX];
					snprintf(hostxdg, sizeof(hostxdg),
						 "%s/run/host/xdg",
						 sb->jailroot);
					bool hostxdg_bound = false;
					if ((plan->sock_wayland ||
					     plan->sock_inherit_wayland ||
					     plan->sock_session_bus ||
					     plan_has_xdg_socket_grant(plan)) &&
					    access(plan->xdg_runtime_dir,
						   R_OK) == 0) {
						if (m_nullfs(
							    sb,
							    plan->xdg_runtime_dir,
							    hostxdg, true) == 0)
							hostxdg_bound = true;
						else
							fpkg_warnf(
								"could not "
								"expose host "
								"XDG_RUNTIME_"
								"DIR");
					}
					if (hostxdg_bound &&
					    (plan->sock_wayland ||
					     plan->sock_inherit_wayland)) {
						char base[256];
						char sock[PATH_MAX],
							link[PATH_MAX];
						host_wayland_display(
							plan, base,
							sizeof(base));
						snprintf(
							sb->wayland_display,
							sizeof(sb->wayland_display),
							"%s", base);
						snprintf(sock, sizeof(sock),
							 "%s/%s",
							 plan->xdg_runtime_dir,
							 base);
						if (access(sock, R_OK) == 0) {
							snprintf(link,
								 sizeof(link),
								 "%s/%s",
								 runuser, base);
							unlink(link);
							snprintf(
								sock,
								sizeof(sock),
								"/run/host/xdg/"
								"%s",
								base);
							make_symlink(sock,
								     link);
						}
					}
					if (hostxdg_bound &&
					    plan->sock_session_bus) {
						char bsock[PATH_MAX],
							link[PATH_MAX];
						snprintf(bsock, sizeof(bsock),
							 "%s/bus",
							 plan->xdg_runtime_dir);
						if (access(bsock, R_OK) == 0) {
							snprintf(link,
								 sizeof(link),
								 "%s/bus",
								 runuser);
							unlink(link);
							make_symlink(
								"/run/host/xdg/"
								"bus",
								link);
						}
					}
					if (hostxdg_bound) {
						size_t xl = strlen(
							plan->xdg_runtime_dir);
						for (size_t gi = 0;
						     gi < plan->ngrants; gi++) {
							const fs_grant_t* g =
								&plan->grants
									 [gi];
							if (g->mode ==
							    FS_GRANT_HIDE)
								continue;
							if (strncmp(g->host_path,
								    plan->xdg_runtime_dir,
								    xl) != 0 ||
							    g->host_path[xl] !=
								    '/')
								continue;
							struct stat gst;
							if (lstat(g->host_path,
								  &gst) != 0 ||
							    !S_ISSOCK(
								    gst.st_mode))
								continue;
							const char* base =
								g->host_path +
								xl + 1;
							if (!*base ||
							    strchr(base, '/'))
								continue;
							char link[PATH_MAX];
							snprintf(link,
								 sizeof(link),
								 "%s/%s",
								 runuser, base);
							unlink(link);
							char sock[PATH_MAX];
							snprintf(sock,
								 sizeof(sock),
								 "/run/host/"
								 "xdg/%s",
								 base);
							make_symlink(sock,
								     link);
						}
					}
				}
			}
		}

		if (plan->sock_pulseaudio) {
			char psock[PATH_MAX];
			bool remote = false;
			if (pulse_socket_path(plan, psock, sizeof(psock),
					      &remote) == 0 &&
			    !remote) {
				char pdir[PATH_MAX], base[PATH_MAX];
				char* slash;
				snprintf(pdir, sizeof(pdir), "%s", psock);
				slash = strrchr(pdir, '/');
				if (slash != NULL) {
					*slash = '\0';
					snprintf(base, sizeof(base), "%s",
						 slash + 1);
				} else {
					snprintf(pdir, sizeof(pdir), "/");
					snprintf(base, sizeof(base), "%s",
						 psock);
				}
				char hostdir[PATH_MAX], to[PATH_MAX];
				snprintf(hostdir, sizeof(hostdir),
					 "%s/run/flatpak/pulse-host",
					 sb->jailroot);
				snprintf(to, sizeof(to), "%s/run/flatpak/pulse",
					 sb->jailroot);
				if (mkdir_p(hostdir, 0755) != 0 ||
				    mkdir_p(to, 0755) != 0)
					return -1;
				if (m_nullfs(sb, pdir, hostdir, true) == 0) {
					char jtarget[PATH_MAX], link[PATH_MAX];
					sb->pulse_bound = true;
					snprintf(jtarget, sizeof(jtarget),
						 "/run/flatpak/pulse-host/%s",
						 base);
					snprintf(link, sizeof(link), "%s/%s",
						 to, base);
					make_symlink(jtarget, link);
					snprintf(link, sizeof(link),
						 "%s/config", to);
					if (write_file(link, "enable-shm=no\n",
						       0644) != 0)
						fpkg_warnf(
							"could not write "
							"pulse client config");
					snprintf(link, sizeof(link),
						 "%s/run/user/%u/pulse",
						 sb->jailroot,
						 (unsigned)plan->uid);
					make_symlink("/run/flatpak/pulse",
						     link);
				} else {
					fpkg_warnf("could not expose pulse "
						   "socket %s",
						   psock);
				}
			} else if (!remote) {
				fpkg_infof("could not find pulseaudio socket");
			} else {
				sb->pulse_remote = true;
			}
		}

		{
			bool sys_proxied = dbus_system_bus_proxied(plan);
			if (plan->sock_system_bus && !sys_proxied) {
				const char* hsock =
					access("/var/run/dbus/"
					       "system_bus_socket",
					       R_OK) == 0
						? "/var/run/dbus/"
						  "system_bus_socket"
						: "/run/dbus/system_bus_socket";
				if (bind_host_socket_dir(sb, hsock,
							 "/run/dbus") != 0)
					fpkg_warnf("could not expose system "
						   "bus (%s)",
						   hsock);
			} else if (sys_proxied) {
				char to[PATH_MAX];
				snprintf(to, sizeof(to), "%s/run/dbus",
					 sb->jailroot);
				if (mkdir_p(to, 0755) != 0) return -1;
			}
		}

		if (plan->sock_ssh_auth) {
			const char* sock = cli_getenv("SSH_AUTH_SOCK");
			struct stat sst;
			if (sock && *sock && stat(sock, &sst) == 0) {
				if (bind_host_socket_dir(
					    sb, sock,
					    "/run/flatpak/ssh-agent") == 0)
					sb->ssh_auth_bound = true;
				else
					fpkg_warnf("could not expose ssh agent "
						   "(%s)",
						   sock);
			}
		}

		if (plan->sock_pcsc) {
			char hsock[PATH_MAX];
			if (pcsc_socket_path(hsock, sizeof(hsock)) == 0 &&
			    bind_host_socket_dir(sb, hsock, "/run/pcscd") ==
				    0) {
				const char* base = strrchr(hsock, '/');
				base = base ? base + 1 : hsock;
				sb->pcsc_socket =
					xasprintf("/run/pcscd/%s", base);
			} else {
				fpkg_infof("could not find pcscd socket");
			}
		}

		if (plan->sock_cups) {
			char hsock[PATH_MAX];
			if (cups_socket_path(plan, hsock, sizeof(hsock)) == 0 &&
			    bind_host_socket_dir(sb, hsock, "/run/cups") == 0) {
				const char* base = strrchr(hsock, '/');
				base = base ? base + 1 : hsock;
				sb->cups_server =
					xasprintf("/run/cups/%s", base);
			} else {
				fpkg_infof("could not find CUPS socket");
			}
		}

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

		if (plan->want_network && plan->xdg_runtime_dir &&
		    *plan->xdg_runtime_dir) {
			char gss[PATH_MAX], to[PATH_MAX];
			snprintf(gss, sizeof(gss), "%s/gssproxy",
				 plan->xdg_runtime_dir);
			if (access(gss, R_OK) == 0) {
				snprintf(to, sizeof(to),
					 "%s/run/flatpak/gssproxy",
					 sb->jailroot);
				if (mkdir_p(to, 0755) == 0 &&
				    m_nullfs(sb, gss, to, true) == 0) {
					char link[PATH_MAX];
					snprintf(link, sizeof(link),
						 "%s/run/user/%u/gssproxy",
						 sb->jailroot,
						 (unsigned)plan->uid);
					make_symlink("/run/flatpak/gssproxy",
						     link);
				} else {
					fpkg_warnf("could not expose gssproxy");
				}
			}
		}

		{
			char doc_host[PATH_MAX], doc_byapp[PATH_MAX];
			char doc_to[PATH_MAX];
			const char* doc_src = NULL;
			bool doc_ro = true;

			snprintf(doc_host, sizeof(doc_host), "%s/doc",
				 plan->xdg_runtime_dir);
			snprintf(doc_byapp, sizeof(doc_byapp), "%s/by-app/%s",
				 doc_host, sb->app_id);
			snprintf(doc_to, sizeof(doc_to), "%s/run/flatpak/doc",
				 sb->jailroot);
			if (access(doc_byapp, R_OK) == 0) {
				doc_src = doc_byapp;
				doc_ro = false;
			} else if (access(doc_host, R_OK) == 0) {
				doc_src = doc_host;
				doc_ro = true;
			}
			if (doc_src != NULL) {
				if (mkdir_p(doc_to, 0755) != 0) {
					fpkg_errorf("mkdir %s: %s", doc_to,
						    strerror(errno));
					return -1;
				}
				if (m_nullfs(sb, doc_src, doc_to, doc_ro) != 0)
					fpkg_warnf("could not expose the "
						   "document portal dir");
			}
		}
	}
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
					symlink(lnsrc, linkpath);
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
				unlink(to);
				if (ensure_file(to) == 0)
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

static int stage_host_grants(sandbox_t* sb, const sandbox_plan_t* plan) {
	{
		struct {
			const char* name;
		} overlays[] = {
			{"container-manager"}, {"font-dirs.xml"}, {NULL}};
		for (int i = 0; overlays[i].name; i++) {
			char from[PATH_MAX], to[PATH_MAX];
			snprintf(from, sizeof(from), "%s/run/%s", sb->jailroot,
				 overlays[i].name);
			snprintf(to, sizeof(to), "%s/run/host/%s", sb->jailroot,
				 overlays[i].name);
			if (access(from, R_OK) == 0 && ensure_file(to) == 0)
				m_nullfs(sb, from, to, true);
		}
		if (plan_has_host_grant(plan)) {
			static const char* protected_dirs[] = {
				".",	  "..",	   "app",  "bin",  "boot",
				"compat", "dev",   "efi",  "etc",  "lib",
				"lib32",  "lib64", "proc", "root", "run",
				"sbin",	  "sys",   "tmp",  "usr",  "var",
				NULL};
			bool ro = true;
			for (size_t i = 0; i < plan->ngrants; i++)
				if (plan->grants[i].mode != FS_GRANT_HIDE &&
				    strcmp(plan->grants[i].jail_path,
					   "/run/host") == 0)
					ro = plan->grants[i].mode ==
					     FS_GRANT_RO;
			DIR* d = opendir("/");
			if (d) {
				struct dirent* de;
				while ((de = readdir(d)) != NULL) {
					bool skip = false;
					for (int i = 0; protected_dirs[i]; i++)
						if (strcmp(protected_dirs[i],
							   de->d_name) == 0) {
							skip = true;
							break;
						}
					if (skip) continue;
					char from[PATH_MAX], to[PATH_MAX];
					snprintf(from, sizeof(from), "/%s",
						 de->d_name);
					struct stat st;
					if (stat(from, &st) != 0) continue;
					snprintf(to, sizeof(to), "%s/%s",
						 sb->jailroot, de->d_name);
					if (sb_is_mounted(sb, to)) continue;
					if (S_ISDIR(st.st_mode)) {
						if (mkdir_p(to, 0755) != 0)
							continue;
						m_nullfs(sb, from, to, ro);
					} else if (S_ISREG(st.st_mode)) {
						if (ensure_file(to) == 0)
							m_nullfs(sb, from, to,
								 ro);
					}
				}
				closedir(d);
			}
		}
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
			unlink(link);
			(void)symlink("flatpkg", link);
		}
	}
	return 0;
}
int sandbox_build_root(sandbox_t* sb, const sandbox_plan_t* plan,
		       const fp_metadata_t* meta) {
	assert(sb != NULL);
	assert(plan != NULL);
	char tmpl[PATH_MAX];
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

	(void)chmod(root, 0755);

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
	if (stage_host_grants(sb, plan) != 0) return -1;
	if (stage_store_links(sb) != 0) return -1;

	return 0;
}

static const char* guess_linux_osrelease(const char* runtime_ref) {
	assert(runtime_ref != NULL);
	(void)runtime_ref;
	return "5.15.0";
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
	ADDP("linux.osrelease", guess_linux_osrelease(meta->runtime_ref));

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

	// /dev/shm, now that the devfs is on top of /dev.
	{
		char shmdir[PATH_MAX];
		snprintf(shmdir, sizeof(shmdir), "%s/dev/shm", sb->jailroot);
		if (plan->dev_shm) {
		} else if (plan->per_app_dev_shm &&
			   access(plan->xdg_runtime_dir, R_OK) == 0) {
			char shm_host[PATH_MAX];
			snprintf(shm_host, sizeof(shm_host),
				 "%s/.flatpak/%s/dev-shm",
				 plan->xdg_runtime_dir, sb->app_id);
			if (mkdir_app_xdg_dir(plan, sb->app_id, "dev-shm") ==
			    0) {
				chmod(shm_host, 01777);
				if (m_nullfs(sb, shm_host, shmdir, false) !=
				    0) {
					fpkg_warnf("could not bind per-app "
						   "/dev/shm, falling back to "
						   "tmpfs");
					(void)m_tmpfs(sb, shmdir);
				}
			} else {
				fpkg_warnf("could not create per-app /dev/shm "
					   "dir, falling back to tmpfs");
				(void)m_tmpfs(sb, shmdir);
			}
		} else {
			if (m_tmpfs(sb, shmdir) != 0)
				fpkg_warnf("could not mount /dev/shm; Wayland "
					   "shm and MIT-SHM may fail");
		}
	}

	// Last, now that every mount of the launch is in place: hand the
	// per-launch tree to the app.
	own_tree_dir(sb, plan->uid, plan->gid, sb->jailroot);

	return 0;

fail:
	for (int i = 0; i < nparams; i++) jailparam_free(&params[i], 1);
	return -1;
#undef ADDP
}

// Close every inherited fd above stderr but the forwarded ones: jail_attach(2)
// refuses to chroot while any directory fd is open. Never below 3.
static void close_inherited_fds(void) {
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

static char** rewrite_forwarded_args(const sandbox_plan_t* plan,
				     char* const extra_argv[]) {
	assert(plan != NULL);
	size_t nextra = 0;
	while (extra_argv && extra_argv[nextra]) nextra++;

	char** out = calloc(nextra + 1, sizeof(char*));
	if (!out) {
		fpkg_errorf("out of memory");
		exit(1);
	}
	size_t n = 0;
	for (size_t i = 0; i < nextra; i++) {
		const char* arg = extra_argv[i];
		if (strcmp(arg, "@@") != 0 && strcmp(arg, "@@u") != 0) {
			out[n++] = xstrdup(arg);
			continue;
		}
		bool as_uri = arg[2] == 'u';
		i++;
		for (; i < nextra && strcmp(extra_argv[i], "@@") != 0; i++) {
			const char* ref = extra_argv[i];
			char host[PATH_MAX], jail[PATH_MAX];

			if (ref[0] == '/' || ref[0] == '\0' ||
			    strstr(ref, "..") != NULL) {
				out[n++] = xstrdup(ref);
				continue;
			}
			snprintf(host, sizeof(host), "%s/doc/%s",
				 plan->xdg_runtime_dir, ref);
			if (access(host, F_OK) != 0) {
				fpkg_warnf("document portal file '%s' not "
					   "found on the host, passing through",
					   ref);
				out[n++] = xstrdup(ref);
				continue;
			}
			snprintf(jail, sizeof(jail), "/run/flatpak/doc/%s",
				 ref);
			if (as_uri) {
				char uri[PATH_MAX + 16];
				snprintf(uri, sizeof(uri), "file://%s", jail);
				out[n++] = xstrdup(uri);
			} else {
				out[n++] = xstrdup(jail);
			}
		}
	}
	out[n] = NULL;
	return out;
}

static int jail_process_count(int jid) {
	assert(jid > 0);
	struct kinfo_proc* procs;
	int n = 0, count = 0;

	if (jid <= 0) return 0;
	procs = kinfo_getallproc(&n);
	if (procs == NULL) return 0;
	for (int i = 0; i < n; i++)
		if (procs[i].ki_jid == jid) count++;
	free(procs);
	return count;
}

// Relay between the caller's terminal and a pty master until `pid` is gone:
// app output to out_fd, the caller's typing to the pty, window changes to the
// master.  -1 means no terminal that way; kill_on_eof kills on caller EOF.
int sandbox_pty_relay(int master, int in_fd, int out_fd, int size_fd, pid_t pid,
		      bool kill_on_eof, int* status) {
	int done = 0;
	bool stdin_done = in_fd < 0;
	bool master_done = false;
	// The caller's window (size_fd), copied to the master, which signals the
	// app's foreground group.  The baseline is the pty's current size: a
	// resize during the seconds of assembly must still reach the app.
	struct winsize last_ws;
	memset(&last_ws, 0, sizeof(last_ws));
	bool have_ws = ioctl(master, TIOCGWINSZ, &last_ws) == 0 &&
		       last_ws.ws_row > 0 && last_ws.ws_col > 0;
	while (!done) {
		// The client is the caller's terminal: once it is gone, reading
		// in_fd would take input from the shell that just got its prompt
		// back.  Stop reading and end the app.
		if (!stdin_done && fpkg_client_gone()) {
			stdin_done = true;
			if (kill_on_eof) {
				kill(pid, SIGTERM);
				kill_on_eof = false;
			}
		}
		struct winsize cur_ws;
		if (size_fd >= 0 && ioctl(size_fd, TIOCGWINSZ, &cur_ws) == 0 &&
		    cur_ws.ws_row > 0 && cur_ws.ws_col > 0 &&
		    (!have_ws || cur_ws.ws_row != last_ws.ws_row ||
		     cur_ws.ws_col != last_ws.ws_col)) {
			if (have_ws) (void)ioctl(master, TIOCSWINSZ, &cur_ws);
			last_ws = cur_ws;
			have_ws = true;
		}
		struct pollfd fds[2] = {
			{master_done ? -1 : master, POLLIN, 0},
			{stdin_done ? -1 : in_fd, POLLIN, 0},
		};
		int n = poll(fds, 2, 200);
		if (n > 0) {
			char rbuf[16384];
			if (fds[0].revents & (POLLIN | POLLHUP)) {
				ssize_t r = read(master, rbuf, sizeof(rbuf));
				if (r > 0 && out_fd >= 0) {
					ssize_t w = 0;
					while (w < r) {
						ssize_t x =
							write(out_fd, rbuf + w,
							      (size_t)(r - w));
						if (x <= 0) {
							if (x < 0 &&
							    errno == EINTR)
								continue;
							break;
						}
						w += x;
					}
				} else if (r < 0 && errno == EINTR) {
				} else if (r <= 0) {
					master_done = true;
				}
			}
			if (fds[1].revents & (POLLIN | POLLHUP)) {
				ssize_t r = read(in_fd, rbuf, sizeof(rbuf));
				if (r > 0) {
					ssize_t w = 0;
					while (w < r) {
						ssize_t x =
							write(master, rbuf + w,
							      (size_t)(r - w));
						if (x <= 0) {
							if (x < 0 &&
							    errno == EINTR)
								continue;
							break;
						}
						w += x;
					}
				} else if (r < 0 && errno == EINTR) {
				} else {
					if (kill_on_eof) {
						fpkg_warnf(
							"client disconnected, "
							"terminating the app");
						kill(pid, SIGTERM);
					}
					stdin_done = true;
				}
			}
		}
		pid_t w = waitpid(pid, status, WNOHANG);
		if (w == pid) done = 1;
		else if (w < 0 && errno != EINTR)
			done = 1;
	}
	return 0;
}

// Called by the parent of the fork that gave the app its terminal: the app has
// its own copy by then, and while the parent's stays open a session-leader app
// loses the last few hundred bytes queued in the pty when it exits.
bool sandbox_app_tty(int* fd, unsigned* mask) {
	const char* s = cli_getenv("FLATPKG_APP_TTY");
	if (s == NULL || *s == '\0') return false;
	char* end = NULL;
	long v = strtol(s, &end, 10);
	if (end == s || v < 0 || v > 65535) return false;
	unsigned m = 7;
	if (*end == ':') {
		// The typed worker's spelling for a caller whose stdio is a
		// mixture: only these descriptors get the pty.
		m = 0;
		const char* p = end + 1;
		while (*p != '\0') {
			if (*p < '0' || *p > '2') return false;
			m |= 1u << (*p - '0');
			p++;
			if (*p == ',') p++;
			else if (*p != '\0')
				return false;
		}
		if (m == 0) return false;
	} else if (*end != '\0') {
		return false;
	}
	*fd = (int)v;
	*mask = m;
	return true;
}

void sandbox_release_app_tty(void) {
	const char* ttys = cli_getenv("FLATPKG_APP_TTY");
	if (ttys == NULL || *ttys == '\0') return;
	int fd = atoi(ttys);
	if (fd > 2) close(fd);
	(void)unsetenv("FLATPKG_APP_TTY");
}

int sandbox_exec(sandbox_t* sb, const fp_metadata_t* meta,
		 const sandbox_plan_t* plan, char* const extra_argv[]) {
	assert(sb != NULL);
	assert(meta != NULL);
	// The run-environ snapshot: before jail_attach(), where this path does
	// not exist.
	instance_save_run_environ(sb, meta, plan);

	// The app must not run while this process holds a copy of the pty
	// slave, or FreeBSD discards output queued when the session leader
	// exits.  The child waits here; EOF releases it if the parent dies.
	int gate[2];
	if (pipe(gate) != 0) {
		fpkg_errorf("pipe: %s", strerror(errno));
		return -1;
	}
	(void)fcntl(gate[0], F_SETFD, FD_CLOEXEC);
	(void)fcntl(gate[1], F_SETFD, FD_CLOEXEC);

	pid_t pid = fork();
	if (pid < 0) {
		fpkg_errorf("fork: %s", strerror(errno));
		close(gate[0]);
		close(gate[1]);
		return -1;
	}
	if (pid > 0) write_instance_pid(sb, pid);
	if (pid == 0) {
		close(gate[1]);
		fpkg_debugf("sandbox: jid=%d uid=%u gid=%u", sb->jid,
			    (unsigned)plan->uid, (unsigned)plan->gid);

		int ttyfd = -1;
		unsigned ttymask = 0;
		if (sandbox_app_tty(&ttyfd, &ttymask) && ttyfd >= 0) {
			int ctty = -1;
			// Only the descriptors the caller had on a terminal:
			// the rest keep the caller's own file or pipe.
			for (int i = 0; i < 3; i++)
				if (ttymask & (1u << i)) {
					dup2(ttyfd, i);
					if (ctty < 0) ctty = i;
				}
			if (ctty >= 0) {
				setsid();
				(void)ioctl(ctty, TIOCSCTTY, 0);
			}
		}

		{
			char ch;
			while (read(gate[0], &ch, 1) < 0 && errno == EINTR);
			close(gate[0]);
		}

		// See close_inherited_fds(): this must precede jail_attach().
		close_inherited_fds();

		{
			struct sigaction dfl;
			memset(&dfl, 0, sizeof(dfl));
			dfl.sa_handler = SIG_DFL;
			sigemptyset(&dfl.sa_mask);
			(void)sigaction(SIGCHLD, &dfl, NULL);
			(void)sigaction(SIGPIPE, &dfl, NULL);
			(void)sigaction(SIGSEGV, &dfl, NULL);
		}

		if (setgroups(1, &plan->gid) != 0)
			fpkg_warnf("setgroups(%u): %s", (unsigned)plan->gid,
				   strerror(errno));

		if (jail_attach(sb->jid) != 0) {
			dprintf(2, "flatpkg: jail_attach(jid=%d): %s\n",
				sb->jid, strerror(errno));
			_exit(127);
		}
		if (chdir("/") != 0) {
			dprintf(2, "flatpkg: chdir: %s\n", strerror(errno));
			_exit(127);
		}

		if (setgid(plan->gid) != 0 || setuid(plan->uid) != 0) {
			dprintf(2, "flatpkg: setuid: %s\n", strerror(errno));
			_exit(127);
		}

		if (plan->die_with_parent) {
			int signo = SIGTERM;
			(void)procctl(P_PID, getpid(), PROC_PDEATHSIG_CTL,
				      &signo);
		}

		char buf[4096];

		if (plan->clearenv) clearenv();

		setenv("HOME", plan->home_dir, 1);
		setenv("PATH", "/app/bin:/run/flatpak:/usr/bin", 1);
		snprintf(buf, sizeof(buf), "/run/user/%u", (unsigned)plan->uid);
		setenv("XDG_RUNTIME_DIR", buf, 1);
		snprintf(buf, sizeof(buf), "%s/.var/app/%s/data",
			 plan->home_dir, sb->app_id);
		setenv("XDG_DATA_HOME", buf, 1);
		snprintf(buf, sizeof(buf), "%s/data/flatpak",
			 plan->app_data_dir);
		setenv("FLATPAK_USER_DIR", buf, 1);
		snprintf(buf, sizeof(buf), "%s/.var/app/%s/config",
			 plan->home_dir, sb->app_id);
		setenv("XDG_CONFIG_HOME", buf, 1);

		snprintf(buf, sizeof(buf), "%s/.var/app/%s/cache",
			 plan->home_dir, sb->app_id);
		setenv("XDG_CACHE_HOME", buf, 1);
		snprintf(buf, sizeof(buf), "%s/.var/app/%s/.local/state",
			 plan->home_dir, sb->app_id);
		setenv("XDG_STATE_HOME", buf, 1);
		setenv("container", "flatpak", 1);
		setenv("XDG_DATA_DIRS", "/app/share:/share:/usr/share", 1);
		// /app/etc/xdg carries app-shipped GSettings overrides and XDG
		// configs.
		setenv("XDG_CONFIG_DIRS", "/app/etc/xdg:/etc/xdg", 1);
		setenv("SHELL", "/bin/sh", 1);

		{
			char ld_path[8192] =
				"/app/lib:/app/lib64:/usr/lib:/usr/lib64";
			size_t ld_path_len = strlen(ld_path);

			for (size_t i = 0; i < sb->nextensions; i++) {
				const fp_extension_t* e = &sb->extensions[i];
				if (e->add_ld_path && *e->add_ld_path) {
					char ext_ld[PATH_MAX];
					snprintf(ext_ld, sizeof(ext_ld),
						 "%s/%s", e->target,
						 e->directory);
					if (e->subdir_suffix &&
					    *e->subdir_suffix) {
						char tmp[PATH_MAX];
						snprintf(tmp, sizeof(tmp),
							 "%s/%s", ext_ld,
							 e->subdir_suffix);
						snprintf(ext_ld, sizeof(ext_ld),
							 "%s", tmp);
					}
					if (strcmp(e->add_ld_path, "true") !=
						    0 &&
					    strcmp(e->add_ld_path, "1") != 0) {
						char tmp[PATH_MAX];
						snprintf(tmp, sizeof(tmp),
							 "%s/%s", ext_ld,
							 e->add_ld_path);
						snprintf(ext_ld, sizeof(ext_ld),
							 "%s", tmp);
					}

					size_t ext_len = strlen(ext_ld);
					if (ld_path_len + ext_len + 2 <
					    sizeof(ld_path)) {
						ld_path[ld_path_len++] = ':';
						strlcpy(ld_path + ld_path_len,
							ext_ld,
							sizeof(ld_path) -
								ld_path_len);
						ld_path_len += ext_len;
					}
				}
			}

			setenv("LD_LIBRARY_PATH", ld_path, 1);
		}

		setenv("FLATPAK_ID", sb->app_id, 1);
		setenv("FLATPAK_INFO", "/run/.flatpak-info", 1);
		setenv("PS1", "[\xF0\x9F\x93\xA6 $FLATPAK_ID \\W]\\$ ", 1);

		if (!plan->clearenv) {
			static const char* host_xdg[] = {
				"XDG_DATA_HOME", "XDG_CONFIG_HOME",
				"XDG_CACHE_HOME", "XDG_STATE_HOME", NULL};
			for (int i = 0; host_xdg[i]; i++) {
				const char* v = cli_getenv(host_xdg[i]);
				if (v) {
					snprintf(buf, sizeof(buf), "HOST_%s",
						 host_xdg[i]);
					setenv(buf, v, 1);
				}
			}
		}

		if (!plan->clearenv) {
			static const char* copy_vars[] = {"PWD",
							  "GDMSESSION",
							  "XDG_CURRENT_DESKTOP",
							  "XDG_SESSION_DESKTOP",
							  "DESKTOP_SESSION",
							  "EMAIL_ADDRESS",
							  "HOSTNAME",
							  "LOGNAME",
							  "REAL_NAME",
							  "TERM",
							  "USER",
							  "USERNAME",
							  NULL};
			for (int i = 0; copy_vars[i]; i++) {
				const char* v = cli_getenv(copy_vars[i]);
				// Already set: overwriting it would make libc
				// copy the value onto itself.
				if (v) setenv(copy_vars[i], v, 0);
			}

			if (plan->devel) {
				static const struct {
					const char* name;
					const char* value;
				} devel_exports[] = {
					{"ACLOCAL_PATH", "/app/share/aclocal"},
					{"C_INCLUDE_PATH", "/app/include"},
					{"CPLUS_INCLUDE_PATH", "/app/include"},
					{"LDFLAGS", "-L/app/lib "},
					{"PKG_CONFIG_PATH",
					 "/app/lib/pkgconfig:"
					 "/app/share/pkgconfig:"
					 "/usr/lib/pkgconfig:"
					 "/usr/share/pkgconfig"},
					{"LC_ALL", "en_US.utf8"},
					{NULL, NULL}};
				for (int i = 0; devel_exports[i].name; i++)
					setenv(devel_exports[i].name,
					       devel_exports[i].value, 1);
			} else {
				static const char* locale_vars[] = {
					"LANG",
					"LANGUAGE",
					"LC_ALL",
					"LC_ADDRESS",
					"LC_COLLATE",
					"LC_CTYPE",
					"LC_IDENTIFICATION",
					"LC_MEASUREMENT",
					"LC_MESSAGES",
					"LC_MONETARY",
					"LC_NAME",
					"LC_NUMERIC",
					"LC_PAPER",
					"LC_TELEPHONE",
					"LC_TIME",
					NULL};
				for (int i = 0; locale_vars[i]; i++) {
					const char* v =
						cli_getenv(locale_vars[i]);
					if (v) setenv(locale_vars[i], v, 0);
				}
			}
		}

		static const char* unset_vars[] = {
			"LD_LIBRARY_PATH",
			"LD_PRELOAD",
			"LD_DEBUG",
			"LD_AUDIT",
			"TEMP",
			"TEMPDIR",
			"TMP",
			"TMPDIR",
			"TZDIR",
			"PYTHONPATH",
			"PYTHONPYCACHEPREFIX",
			"PERLLIB",
			"PERL5LIB",
			"XCURSOR_PATH",
			"GST_REGISTRY",
			"GST_REGISTRY_1_0",
			"GST_PLUGIN_PATH",
			"GST_PLUGIN_SYSTEM_PATH",
			"GST_PLUGIN_SCANNER",
			"GST_PLUGIN_SCANNER_1_0",
			"GST_PLUGIN_SYSTEM_PATH_1_0",
			"GST_PRESET_PATH",
			"GST_PTP_HELPER",
			"GST_PTP_HELPER_1_0",
			"GST_INSTALL_PLUGINS_HELPER",
			"KRB5CCNAME",
			"XKB_CONFIG_ROOT",
			"GIO_EXTRA_MODULES",
			"GDK_BACKEND",
			"VK_ADD_DRIVER_FILES",
			"VK_ADD_LAYER_PATH",
			"VK_DRIVER_FILES",
			"VK_ICD_FILENAMES",
			"VK_LAYER_PATH",
			"__EGL_EXTERNAL_PLATFORM_CONFIG_DIRS",
			"__EGL_EXTERNAL_PLATFORM_CONFIG_FILENAMES",
			"__EGL_VENDOR_LIBRARY_DIRS",
			"__EGL_VENDOR_LIBRARY_FILENAMES",
			NULL};
		for (int i = 0; unset_vars[i]; i++) unsetenv(unset_vars[i]);

		if (sb->egl_vendor_files && sb->egl_vendor_files[0])
			setenv("__EGL_VENDOR_LIBRARY_FILENAMES",
			       sb->egl_vendor_files, 1);
		if (sb->vulkan_icd_files && sb->vulkan_icd_files[0]) {
			setenv("VK_ICD_FILENAMES", sb->vulkan_icd_files, 1);
			setenv("VK_DRIVER_FILES", sb->vulkan_icd_files, 1);
		}
		if (sb->vulkan_layer_paths && sb->vulkan_layer_paths[0])
			setenv("VK_LAYER_PATH", sb->vulkan_layer_paths, 1);

		setenv("GIO_USE_VFS", "local", 0);

		setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
		{
			const char* candidates[] = {
				"/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/"
				"2.10.0/loaders.cache",
				"/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache",
				"/usr/lib64/gdk-pixbuf-2.0/2.10.0/"
				"loaders.cache",
				NULL};
			for (int i = 0; candidates[i]; i++) {
				char p[PATH_MAX];
				snprintf(p, sizeof(p), "%s%s", sb->jailroot,
					 candidates[i]);
				if (access(p, R_OK) == 0) {
					setenv("GDK_PIXBUF_MODULE_FILE",
					       candidates[i], 0);
					break;
				}
			}
		}

		if (cli_getenv("FLATPKG_NO_SHIMS") == NULL)
			setenv("LD_PRELOAD",
			       "/.signalfd_shim.so:"
			       "/.seccomp_shim.so:/.memfd_shim.so:"
			       "/.syncobj_shim.so:/.rlimit_shim.so:"
			       "/.madvise_shim.so",
			       1);

		if (plan->sock_wayland &&
		    cli_getenv("FLATPKG_NO_SHIMS") == NULL &&
		    access("/run/flatpak/dmabuf_devt_shim.so", R_OK) == 0) {
			char map[512];
			const char* cur = cli_getenv("LD_PRELOAD");

			drm_devt_map(map, sizeof(map));
			if (map[0] != '\0') {
				char pl[512];

				setenv("WAYLAND_DMABUF_DEVT_MAP", map, 1);
				snprintf(pl, sizeof(pl),
					 "%s:/run/flatpak/"
					 "dmabuf_devt_shim.so",
					 cur != NULL ? cur : "");
				setenv("LD_PRELOAD", pl, 1);
			}
		}

		if (plan->devel) setenv("FLATPKG_SECCOMP_DEVEL", "1", 1);
		if (plan->canbus) setenv("FLATPKG_SECCOMP_ALLOW_CAN", "1", 1);
		if (plan->bluetooth) setenv("FLATPKG_SECCOMP_ALLOW_BT", "1", 1);

		if (sb->ld_library_path && sb->ld_library_path[0])
			setenv("LD_LIBRARY_PATH", sb->ld_library_path, 1);

		// ZyPAK's host-spawn strategy expects flatpak-spawn's stub
		// pairing and loses the child inside a flatpkg jail; its spawn
		// strategy runs the Electron sandbox in-process and works.
		setenv("ZYPAK_ZYGOTE_STRATEGY_SPAWN", "0", 1);

		bool x11 = plan->sock_x11 ||
			   (plan->sock_fallback_x11 && !plan->sock_wayland);
		if (!x11) unsetenv("DISPLAY");
		if (sb->xauth_written)
			setenv("XAUTHORITY", "/run/flatpak/Xauthority", 1);
		unsetenv("P11_KIT_SERVER_ADDRESS");
		unsetenv("P11_KIT_SERVER_PID");
		if (sb->p11kit_socket_name != NULL) {
			snprintf(buf, sizeof(buf),
				 "unix:path=/run/flatpak/p11-kit/%s",
				 sb->p11kit_socket_name);
			setenv("P11_KIT_SERVER_ADDRESS", buf, 1);
		}
		if (plan->sock_wayland || plan->sock_inherit_wayland) {
			if (sb->wayland_display[0] != '\0')
				setenv("WAYLAND_DISPLAY", sb->wayland_display,
				       1);
			else
				setenv("WAYLAND_DISPLAY", "wayland-0", 1);
		} else {
			unsetenv("WAYLAND_DISPLAY");
		}
		if (plan->sock_pulseaudio) {
			if (sb->pulse_bound) {
				setenv("PULSE_SERVER",
				       "unix:/run/flatpak/pulse/native", 1);
				setenv("PULSE_CLIENTCONFIG",
				       "/run/flatpak/pulse/config", 1);
			} else if (!sb->pulse_remote) {
				unsetenv("PULSE_SERVER");
			}
		}
		if (plan->sock_ssh_auth) {
			if (sb->ssh_auth_bound) {
				const char* hsock = cli_getenv("SSH_AUTH_SOCK");
				const char* base =
					hsock ? strrchr(hsock, '/') : NULL;
				base = base ? base + 1 : hsock;
				if (base && *base) {
					snprintf(buf, sizeof(buf),
						 "/run/flatpak/ssh-agent/%s",
						 base);
					setenv("SSH_AUTH_SOCK", buf, 1);
				}
			} else {
				unsetenv("SSH_AUTH_SOCK");
			}
		}
		if (plan->sock_cups) {
			if (sb->cups_server)
				setenv("CUPS_SERVER", sb->cups_server, 1);
			else
				unsetenv("CUPS_SERVER");
		}
		if (plan->sock_pcsc) {
			if (sb->pcsc_socket)
				setenv("PCSCLITE_CSOCK_NAME", sb->pcsc_socket,
				       1);
			else
				unsetenv("PCSCLITE_CSOCK_NAME");
		}
		if (plan->sock_gpg_agent) {
			snprintf(buf, sizeof(buf), "/run/user/%u/gnupg",
				 (unsigned)plan->uid);
			setenv("GNUPGHOME", buf, 1);
		}
		if (!plan->sock_inherit_wayland) unsetenv("WAYLAND_SOCKET");

		unsetenv("DBUS_SESSION_BUS_ADDRESS");
		unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
		unsetenv("AT_SPI_BUS_ADDRESS");
		if (sb->session_bus_proxied && sb->bus_proxy_pid > 0 &&
		    sb->session_bus_sock)
			setenv("DBUS_SESSION_BUS_ADDRESS", sb->session_bus_sock,
			       1);
		if (sb->system_bus_proxied && sb->bus_proxy_pid > 0 &&
		    sb->system_bus_sock)
			setenv("DBUS_SYSTEM_BUS_ADDRESS", sb->system_bus_sock,
			       1);
		if (sb->a11y_bus_proxied && sb->bus_proxy_pid > 0 &&
		    sb->a11y_bus_sock)
			setenv("AT_SPI_BUS_ADDRESS", sb->a11y_bus_sock, 1);
		else {
			setenv("AT_SPI_BUS_ADDRESS",
			       "unix:path=/run/flatpak/.no-a11y-bus", 1);
		}

		for (size_t i = 0; i < meta->env_pairs.count; i++) {
			char kv[4224];
			snprintf(kv, sizeof(kv), "%s",
				 meta->env_pairs.items[i]);
			char* eq = strchr(kv, '=');
			if (eq) {
				*eq = '\0';
				setenv(kv, eq + 1, 1);
			}
		}

		for (size_t i = 0; i < plan->env_overrides.count; i++) {
			char kv[4224];
			snprintf(kv, sizeof(kv), "%s",
				 plan->env_overrides.items[i]);
			char* eq = strchr(kv, '=');
			if (eq) {
				*eq = '\0';
				setenv(kv, eq + 1, 1);
			}
		}

		for (size_t i = 0; i < meta->unset_environment.count; i++) {
			const char* var = meta->unset_environment.items[i];
			if (!var || !*var || strchr(var, '/')) continue;
			unsetenv(var);
		}

		{
			const char* cur = cli_getenv("PATH");
			if (cur == NULL ||
			    strstr(cur, "/run/flatpak") == NULL) {
				char np[PATH_MAX + 64];
				snprintf(np, sizeof(np), "/run/flatpak:%s",
					 cur != NULL ? cur
						     : "/app/bin:/usr/bin");
				setenv("PATH", np, 1);
			}
		}

		const char* cmd = (meta->command && meta->command[0])
					  ? meta->command
					  : "/bin/sh";

		size_t nextra = 0;
		while (extra_argv && extra_argv[nextra]) nextra++;

		char** extra = rewrite_forwarded_args(plan, extra_argv);
		nextra = 0;
		while (extra && extra[nextra]) nextra++;

		char** argv = calloc(nextra + 2, sizeof(char*));
		argv[0] = (char*)cmd;
		for (size_t i = 0; i < nextra; i++) argv[i + 1] = extra[i];
		argv[nextra + 1] = NULL;

		umask(0);
		execvp(cmd, argv);
		dprintf(2, "flatpkg: execvp %s: %s (PATH=%s home=%s)\n", cmd,
			strerror(errno), cli_getenv("PATH"),
			cli_getenv("HOME"));
		_exit(127);
	}

	int ttyfd = -1;
	unsigned ttymask = 7;
	(void)sandbox_app_tty(&ttyfd, &ttymask);
	sandbox_release_app_tty();
	close(gate[0]);
	{
		char ch = 'g';
		(void)write(gate[1], &ch, 1);
	}
	close(gate[1]);
	int status = 0;
	const char* master_s = cli_getenv("FLATPKG_APP_MASTER");
	if (master_s && *master_s) {
		const int in_fd = (ttymask & 1u) ? 0 : -1;
		const int out_fd =
			(ttymask & 2u) ? 1 : ((ttymask & 4u) ? 2 : -1);
		const int size_fd = (ttymask & 1u)   ? 0
				    : (ttymask & 2u) ? 1
				    : (ttymask & 4u) ? 2
						     : -1;
		(void)sandbox_pty_relay(atoi(master_s), in_fd, out_fd, size_fd,
					pid, !plan->spawned_via_portal,
					&status);
	} else {
		pid_t w = waitpid(pid, &status, 0);
		if (w < 0) {
			if (errno == EINTR) return 128;
			fpkg_errorf("waitpid: %s", strerror(errno));
			return -1;
		}
	}

	if (sb->jid > 0) {
		while (jail_process_count(sb->jid) > 0) usleep(100000);
	}

	if (WIFSIGNALED(status)) {
		fpkg_errorf("app terminated by signal %d", WTERMSIG(status));
		return 128 + WTERMSIG(status);
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int rm_cb(const char* path, const struct stat* st, int type,
		 struct FTW* ftw) {
	(void)st;
	(void)ftw;
	if (type == FTW_DP || type == FTW_D) rmdir(path);
	else
		unlink(path);
	return 0;
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

	if (sb->jailroot) {
		for (size_t i = sb->nmounts; i > 0; i--) {
			sb_mount_t* m = &sb->mounts[i - 1];
			if (unmount(m->to, 0) != 0 && errno == EBUSY)
				unmount(m->to, MNT_FORCE);
		}
		for (int pass = 0; pass < 5; pass++) {
			bool busy = false;
			for (size_t i = 0; i < sb->nmounts; i++) {
				sb_mount_t* m = &sb->mounts[i];
				if (unmount(m->to, 0) != 0 && errno == EBUSY) {
					busy = true;
					unmount(m->to, MNT_FORCE);
				}
			}
			if (!busy) break;
			usleep(100000 * (unsigned)(pass + 1));
		}
		for (size_t i = 0; i < sb->nmounts; i++) {
			sb_mount_t* m = &sb->mounts[i];
			if (unmount(m->to, 0) != 0 && errno != EINVAL)
				fpkg_warnf("still mounted after teardown: "
					   "%s",
					   m->to);
		}
		nftw(sb->jailroot, rm_cb, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
	}

	if (sb->devfs_ruleset > 0) {
		char cmd[128];
		snprintf(cmd, sizeof(cmd),
			 "/sbin/devfs rule -s %d delset 2>/dev/null",
			 sb->devfs_ruleset);
		system(cmd);
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
