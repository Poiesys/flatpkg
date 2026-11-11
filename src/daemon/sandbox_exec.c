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

// Set when the worker's peer goes away: the relay must stop reading the
// caller's terminal, or it steals the shell's input until the app exits.
static volatile sig_atomic_t g_client_gone;

void fpkg_client_mark_gone(void) { g_client_gone = 1; }

bool fpkg_client_gone(void) { return g_client_gone != 0; }

static void write_instance_pid(sandbox_t* sb, pid_t pid) {
	assert(sb != NULL);
	if (!sb->instance_dir) return;
	char buf[32];
	int n = snprintf(buf, sizeof(buf), "%ld\n", (long)pid);

	char p[PATH_MAX];
	snprintf(p, sizeof(p), "%s/pid", sb->instance_dir);
	// The tree is the app's by now: a name the app could have swapped for a
	// symlink must not be followed by root.
	int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
		      0644);
	if (fd >= 0) {
		if (write(fd, buf, (size_t)n) != (ssize_t)n)
			fpkg_warnf("write %s: %s", p, strerror(errno));
		close(fd);
	} else {
		fpkg_warnf("cannot create pid file %s: %s", p, strerror(errno));
	}

	// The jail-root copy is written through the directory registration held
	// open: by path, the app could have moved the directory or replaced it.
	if (sb->instance_mirror_fd >= 0) {
		fd = openat(sb->instance_mirror_fd, "pid",
			    O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
			    0644);
		if (fd >= 0) {
			if (write(fd, buf, (size_t)n) != (ssize_t)n)
				fpkg_warnf("write pid in %s: %s",
					   sb->jailroot ? sb->jailroot : "?",
					   strerror(errno));
			close(fd);
		}
	}

	instance_write_bwrapinfo(sb->instance_dir, sb->jailroot, (long)pid);
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

// How many processes run inside the jail: 0 means nobody is being served.
int jail_process_count(int jid) {
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

// Relay between the caller's terminal and the pty master until `pid` is gone:
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

static void child_set_tty(void) {
	int ttyfd = -1;
	unsigned ttymask = 0;

	if (sandbox_app_tty(&ttyfd, &ttymask) && ttyfd >= 0) {
		int ctty = -1;

		for (int i = 0; i < 3; i++)
			if (ttymask & (1u << i)) {
				dup2(ttyfd, i);
				if (ctty < 0) ctty = i;
			}
		if (ctty >= 0) {
			setsid();
			if (ioctl(ctty, TIOCSCTTY, 0) != 0)
				dprintf(2, "flatpkg: TIOCSCTTY: %s\n",
					strerror(errno));
		}
	}
}

static void child_wait_gate(int fd) {
	char ch;

	while (read(fd, &ch, 1) < 0 && errno == EINTR);
	close(fd);
}

static void child_reset_signals(void) {
	struct sigaction dfl;

	memset(&dfl, 0, sizeof(dfl));
	dfl.sa_handler = SIG_DFL;
	sigemptyset(&dfl.sa_mask);
	static const int sigs[] = {SIGCHLD, SIGPIPE, SIGSEGV};
	for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
		if (sigaction(sigs[i], &dfl, NULL) != 0)
			dprintf(2, "flatpkg: sigaction(%d): %s\n", sigs[i],
				strerror(errno));
}

static void child_jail_attach(sandbox_t* sb, const sandbox_plan_t* plan) {
	if (setgroups(1, &plan->gid) != 0)
		fpkg_warnf("setgroups(%u): %s", (unsigned)plan->gid,
			   strerror(errno));

	if (jail_attach(sb->jid) != 0) {
		dprintf(2, "flatpkg: jail_attach(jid=%d): %s\n", sb->jid,
			strerror(errno));
		_exit(127);
	}
	if (chdir("/") != 0) {
		dprintf(2, "flatpkg: chdir: %s\n", strerror(errno));
		_exit(127);
	}
}

static void child_drop_identity(const sandbox_plan_t* plan) {
	if (setgid(plan->gid) != 0 || setuid(plan->uid) != 0) {
		dprintf(2, "flatpkg: setuid: %s\n", strerror(errno));
		_exit(127);
	}

	if (plan->die_with_parent) {
		int signo = SIGTERM;
		if (procctl(P_PID, getpid(), PROC_PDEATHSIG_CTL, &signo) != 0)
			dprintf(2, "flatpkg: procctl: %s\n", strerror(errno));
	}
}

static void child_set_base_env(sandbox_t* sb, const sandbox_plan_t* plan) {
	char buf[4096];

	if (plan->clearenv) clearenv();

	setenv("HOME", plan->home_dir, 1);
	setenv("PATH", "/app/bin:/run/flatpak:/usr/bin", 1);
	snprintf(buf, sizeof(buf), "/run/user/%u", (unsigned)plan->uid);
	setenv("XDG_RUNTIME_DIR", buf, 1);
	snprintf(buf, sizeof(buf), "%s/.var/app/%s/data", plan->home_dir,
		 sb->app_id);
	setenv("XDG_DATA_HOME", buf, 1);
	snprintf(buf, sizeof(buf), "%s/data/flatpak", plan->app_data_dir);
	setenv("FLATPAK_USER_DIR", buf, 1);
	snprintf(buf, sizeof(buf), "%s/.var/app/%s/config", plan->home_dir,
		 sb->app_id);
	setenv("XDG_CONFIG_HOME", buf, 1);
	snprintf(buf, sizeof(buf), "%s/.var/app/%s/cache", plan->home_dir,
		 sb->app_id);
	setenv("XDG_CACHE_HOME", buf, 1);
	snprintf(buf, sizeof(buf), "%s/.var/app/%s/.local/state",
		 plan->home_dir, sb->app_id);
	setenv("XDG_STATE_HOME", buf, 1);
	setenv("container", "flatpak", 1);
	setenv("XDG_DATA_DIRS", "/app/share:/share:/usr/share", 1);
	// /app/etc/xdg carries app-shipped GSettings overrides and XDG configs.
	setenv("XDG_CONFIG_DIRS", "/app/etc/xdg:/etc/xdg", 1);
	setenv("SHELL", "/bin/sh", 1);

	{
		char ld_path[8192] = "/app/lib:/app/lib64:/usr/lib:/usr/lib64";
		size_t ld_path_len = strlen(ld_path);

		for (size_t i = 0; i < sb->nextensions; i++) {
			const fp_extension_t* e = &sb->extensions[i];
			if (e->add_ld_path && *e->add_ld_path) {
				char ext_ld[PATH_MAX];
				snprintf(ext_ld, sizeof(ext_ld), "%s/%s",
					 e->target, e->directory);
				if (e->subdir_suffix && *e->subdir_suffix) {
					char tmp[PATH_MAX];
					snprintf(tmp, sizeof(tmp), "%s/%s",
						 ext_ld, e->subdir_suffix);
					snprintf(ext_ld, sizeof(ext_ld), "%s",
						 tmp);
				}
				if (strcmp(e->add_ld_path, "true") != 0 &&
				    strcmp(e->add_ld_path, "1") != 0) {
					char tmp[PATH_MAX];
					snprintf(tmp, sizeof(tmp), "%s/%s",
						 ext_ld, e->add_ld_path);
					snprintf(ext_ld, sizeof(ext_ld), "%s",
						 tmp);
				}

				size_t ext_len = strlen(ext_ld);
				if (ld_path_len + ext_len + 2 <
				    sizeof(ld_path)) {
					ld_path[ld_path_len++] = ':';
					strlcpy(ld_path + ld_path_len, ext_ld,
						sizeof(ld_path) - ld_path_len);
					ld_path_len += ext_len;
				}
			}
		}

		setenv("LD_LIBRARY_PATH", ld_path, 1);
	}

	setenv("FLATPAK_ID", sb->app_id, 1);
	setenv("FLATPAK_INFO", "/run/.flatpak-info", 1);
	setenv("PS1", "[\xF0\x9F\x93\xA6 $FLATPAK_ID \\W]\\$ ", 1);
}

static void child_set_host_env(const sandbox_plan_t* plan) {
	char buf[4096];

	if (!plan->clearenv) {
		static const char* host_xdg[] = {
			"XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
			"XDG_STATE_HOME", NULL};
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
			// Already set: overwriting it would make libc copy the
			// value onto itself.
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
				const char* v = cli_getenv(locale_vars[i]);
				if (v) setenv(locale_vars[i], v, 0);
			}
		}
	}
}

static void child_unset_inherited_env(sandbox_t* sb,
				      const sandbox_plan_t* plan) {
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
		setenv("__EGL_VENDOR_LIBRARY_FILENAMES", sb->egl_vendor_files, 1);
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
			"/usr/lib64/gdk-pixbuf-2.0/2.10.0/loaders.cache",
			NULL};
		for (int i = 0; candidates[i]; i++) {
			char p[PATH_MAX];
			snprintf(p, sizeof(p), "%s%s", sb->jailroot,
				 candidates[i]);
			if (access(p, R_OK) == 0) {
				setenv("GDK_PIXBUF_MODULE_FILE", candidates[i],
				       0);
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

	if (plan->sock_wayland && cli_getenv("FLATPKG_NO_SHIMS") == NULL &&
	    access("/run/flatpak/dmabuf_devt_shim.so", R_OK) == 0) {
		char map[512];
		const char* cur = cli_getenv("LD_PRELOAD");

		drm_devt_map(map, sizeof(map));
		if (map[0] != '\0') {
			char pl[512];

			setenv("WAYLAND_DMABUF_DEVT_MAP", map, 1);
			snprintf(pl, sizeof(pl),
				 "%s:/run/flatpak/dmabuf_devt_shim.so",
				 cur != NULL ? cur : "");
			setenv("LD_PRELOAD", pl, 1);
		}
	}

	if (plan->devel) setenv("FLATPKG_SECCOMP_DEVEL", "1", 1);
	if (plan->canbus) setenv("FLATPKG_SECCOMP_ALLOW_CAN", "1", 1);
	if (plan->bluetooth) setenv("FLATPKG_SECCOMP_ALLOW_BT", "1", 1);

	if (sb->ld_library_path && sb->ld_library_path[0])
		setenv("LD_LIBRARY_PATH", sb->ld_library_path, 1);

	// ZyPAK's host-spawn strategy expects flatpak-spawn's stub pairing and
	// loses the child inside a flatpkg jail; its spawn strategy runs the
	// Electron sandbox in-process and works.
	setenv("ZYPAK_ZYGOTE_STRATEGY_SPAWN", "0", 1);
}

static void child_set_socket_env(sandbox_t* sb,
				 const sandbox_plan_t* plan) {
	char buf[4096];
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
			setenv("WAYLAND_DISPLAY", sb->wayland_display, 1);
		else
			setenv("WAYLAND_DISPLAY", "wayland-0", 1);
	} else {
		unsetenv("WAYLAND_DISPLAY");
	}
	if (plan->sock_pulseaudio) {
		if (sb->pulse_bound) {
			setenv("PULSE_SERVER", "unix:/run/flatpak/pulse/native",
			       1);
			setenv("PULSE_CLIENTCONFIG", "/run/flatpak/pulse/config",
			       1);
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
					 "/run/flatpak/ssh-agent/%s", base);
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
			setenv("PCSCLITE_CSOCK_NAME", sb->pcsc_socket, 1);
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
		setenv("DBUS_SESSION_BUS_ADDRESS", sb->session_bus_sock, 1);
	if (sb->system_bus_proxied && sb->bus_proxy_pid > 0 &&
	    sb->system_bus_sock)
		setenv("DBUS_SYSTEM_BUS_ADDRESS", sb->system_bus_sock, 1);
	if (sb->a11y_bus_proxied && sb->bus_proxy_pid > 0 &&
	    sb->a11y_bus_sock)
		setenv("AT_SPI_BUS_ADDRESS", sb->a11y_bus_sock, 1);
	else
		setenv("AT_SPI_BUS_ADDRESS",
		       "unix:path=/run/flatpak/.no-a11y-bus", 1);
}

static void child_set_metadata_env(const fp_metadata_t* meta,
				   const sandbox_plan_t* plan) {
	for (size_t i = 0; i < meta->env_pairs.count; i++) {
		char kv[4224];
		snprintf(kv, sizeof(kv), "%s", meta->env_pairs.items[i]);
		char* eq = strchr(kv, '=');
		if (eq) {
			*eq = '\0';
			setenv(kv, eq + 1, 1);
		}
	}

	for (size_t i = 0; i < plan->env_overrides.count; i++) {
		char kv[4224];
		snprintf(kv, sizeof(kv), "%s", plan->env_overrides.items[i]);
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
		if (cur == NULL || strstr(cur, "/run/flatpak") == NULL) {
			char np[PATH_MAX + 64];
			snprintf(np, sizeof(np), "/run/flatpak:%s",
				 cur != NULL ? cur : "/app/bin:/usr/bin");
			setenv("PATH", np, 1);
		}
	}
}

static void child_set_environment(sandbox_t* sb, const fp_metadata_t* meta,
				  const sandbox_plan_t* plan) {
	child_set_base_env(sb, plan);
	child_set_host_env(plan);
	child_unset_inherited_env(sb, plan);
	child_set_socket_env(sb, plan);
	child_set_metadata_env(meta, plan);
}

static void child_exec(const fp_metadata_t* meta, const sandbox_plan_t* plan,
		       char* const extra_argv[]) {
	const char* cmd =
		(meta->command && meta->command[0]) ? meta->command : "/bin/sh";
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
		strerror(errno), cli_getenv("PATH"), cli_getenv("HOME"));
	_exit(127);
}

static int sandbox_wait_child(const sandbox_plan_t* plan, pid_t pid,
			      int gate0, int gate1, int* status_out) {
	int ttyfd = -1;
	unsigned ttymask = 7;

	(void)sandbox_app_tty(&ttyfd, &ttymask);
	sandbox_release_app_tty();
	close(gate0);
	{
		char ch = 'g';
		(void)write(gate1, &ch, 1);
	}
	close(gate1);
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

	*status_out = status;
	return 0;
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
	// exits. The child waits here; EOF releases it if the parent dies.
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

		child_set_tty();
		child_wait_gate(gate[0]);

		// See close_inherited_fds(): this must precede jail_attach().
		close_inherited_fds();
		child_reset_signals();
		child_jail_attach(sb, plan);
		child_drop_identity(plan);
		child_set_environment(sb, meta, plan);
		child_exec(meta, plan, extra_argv);
	}

	int status = 0;
	int wrc = sandbox_wait_child(plan, pid, gate[0], gate[1], &status);
	if (wrc != 0) return wrc;

	// Wait for the jail, not the exec'd pid: apps that daemonize are still
	// the launch.
	if (sb->jid > 0) {
		while (jail_process_count(sb->jid) > 0) usleep(100000);
	}

	if (WIFSIGNALED(status)) {
		fpkg_errorf("app terminated by signal %d", WTERMSIG(status));
		return 128 + WTERMSIG(status);
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
