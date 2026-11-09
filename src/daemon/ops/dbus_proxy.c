#include <assert.h>
#include <dirent.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/user.h>
#include <sys/wait.h>

#include <libutil.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "flatpkg.h"
#define FPKG_MODULE "dbus"
#include "fpkglog.h"


static bool unix_socket_live(const char* path) {
	assert(path != NULL);
	struct sockaddr_un sa;
	int fd, rc;

	if (strlen(path) >= sizeof(sa.sun_path)) return false;
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return false;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	rc = connect(fd, (struct sockaddr*)&sa, sizeof(sa));
	close(fd);
	return rc == 0;
}

static bool address_has_unix_path(const char* address) {
	assert(address != NULL);
	char* copy = xstrdup(address);
	char* save = NULL;
	bool found = false;
	for (char* tok = strtok_r(copy, ";", &save); tok;
	     tok = strtok_r(NULL, ";", &save)) {
		if (strncmp(tok, "unix:path=", 10) == 0 && tok[10] != '\0') {
			found = true;
			break;
		}
	}
	free(copy);
	return found;
}

static bool path_is_user_socket(const char* path, uid_t uid) {
	assert(path != NULL);
	struct stat st;
	if (stat(path, &st) != 0) return false;
	return S_ISSOCK(st.st_mode) && st.st_uid == uid;
}

// The host-side session bus address to proxy (caller frees), or NULL when there
// is none: DBUS_SESSION_BUS_ADDRESS, $XDG_RUNTIME_DIR/bus, /run/user/<uid>/bus.
char* dbus_host_session_address(const sandbox_plan_t* plan) {
	assert(plan != NULL);
	const char* addr = cli_getenv("DBUS_SESSION_BUS_ADDRESS");
	if (addr && *addr && address_has_unix_path(addr)) return xstrdup(addr);

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/bus", plan->xdg_runtime_dir);
	if (path_is_user_socket(path, plan->uid))
		return xasprintf("unix:path=%s", path);

	snprintf(path, sizeof(path), "/run/user/%u/bus", (unsigned)plan->uid);
	if (path_is_user_socket(path, plan->uid))
		return xasprintf("unix:path=%s", path);

	return NULL;
}

char* dbus_host_system_address(void) {
	const char* addr = cli_getenv("DBUS_SYSTEM_BUS_ADDRESS");
	if (addr && *addr && address_has_unix_path(addr)) return xstrdup(addr);
	if (access("/var/run/dbus/system_bus_socket", F_OK) == 0)
		return xstrdup("unix:path=/var/run/dbus/system_bus_socket");
	if (access("/run/dbus/system_bus_socket", F_OK) == 0)
		return xstrdup("unix:path=/run/dbus/system_bus_socket");
	return NULL;
}

// The proxy is the default without the --socket=session-bus grant; with the
// grant it still runs, just without --filter (no raw socket to ro-bind on
// FreeBSD).
bool dbus_system_bus_proxied(const sandbox_plan_t* plan) {
	assert(plan != NULL);
	if (plan->sock_system_bus || !dbus_proxy_binary()) return false;
	if (plan->system_bus_policy.count == 0) return false;
	char* addr = dbus_host_system_address();
	bool ok = addr != NULL;
	free(addr);
	return ok;
}

typedef struct {
	char** items;
	size_t count, cap;
} argvec_t;

static void av_push(argvec_t* av, const char* s) {
	assert(av != NULL);
	assert(s != NULL);
	if (av->count == av->cap) {
		av->cap = av->cap ? av->cap * 2 : 16;
		char** new_items = realloc(av->items, av->cap * sizeof(char*));
		if (!new_items) {
			fpkg_errorf("out of memory");
			exit(1);
		}
		av->items = new_items;
	}
	av->items[av->count++] = xstrdup(s);
}

static void av_free(argvec_t* av) {
	assert(av != NULL);
	for (size_t i = 0; i < av->count; i++) free(av->items[i]);
	free(av->items);
	memset(av, 0, sizeof(*av));
}

static void add_bus_filter_args(argvec_t* av, const char* app_id,
				const strlist_t* policy) {
	assert(av != NULL);
	av_push(av, "--filter");

	if (app_id) {
		av_push(av, "--talk=org.freedesktop.dconf");
	}

	if (app_id) {
		char arg[512];
		snprintf(arg, sizeof(arg), "--own=%s.*", app_id);
		av_push(av, arg);
		snprintf(arg, sizeof(arg), "--own=org.mpris.MediaPlayer2.%s.*",
			 app_id);
		av_push(av, arg);
	}

	for (size_t i = 0; i < policy->count; i++) {
		const char* entry = policy->items[i];
		const char* eq = strchr(entry, '=');
		if (!eq || eq == entry) continue;
		char name[512];
		size_t nlen = (size_t)(eq - entry);
		if (nlen >= sizeof(name)) continue;
		memcpy(name, entry, nlen);
		name[nlen] = '\0';
		const char* pol = eq + 1;

		if (strcmp(pol, "none") == 0) continue;
		if (strcmp(pol, "see") == 0 || strcmp(pol, "talk") == 0 ||
		    strcmp(pol, "own") == 0) {
			char arg[512];
			snprintf(arg, sizeof(arg), "--%s=%s", pol, name);
			av_push(av, arg);
		} else {
			fpkg_warnf("unknown bus policy '%s' for '%s', "
				   "ignoring",
				   pol, name);
		}
	}
}

typedef struct {
	char* session;
	char* system;
	char* a11y;
	bool session_raw;
} bus_addrs_t;

static void bus_addrs_free(bus_addrs_t* a) {
	free(a->session);
	free(a->system);
	free(a->a11y);
	memset(a, 0, sizeof(*a));
}

// Addresses to proxy: session unless the grant is unrestricted, system only on a
// policy, and accessibility from the environment, runtime dir, or org.a11y.Bus.
static int bus_addrs_resolve(bus_addrs_t* a, const sandbox_plan_t* plan) {
	a->session = dbus_host_session_address(plan);
	if (!a->session) {
		fpkg_warnf(
			"no session bus found (looked for "
			"$XDG_RUNTIME_DIR/bus and /run/user/%u/bus; set "
			"DBUS_SESSION_BUS_ADDRESS if it lives elsewhere) - the "
			"app gets no session bus, and a --socket=session-bus "
			"grant could not be honoured",
			(unsigned)plan->uid);
	} else if (plan->sock_session_bus) {
		// Unrestricted grant: FreeBSD cannot mount a socket inode and the bus may live
		// outside XDG_RUNTIME_DIR, so serve it through the proxy unfiltered.
		a->session_raw = true;
	}
	if (!plan->sock_system_bus && plan->system_bus_policy.count > 0) {
		a->system = dbus_host_system_address();
		if (!a->system)
			fpkg_warnf("no system bus found on the host; "
				   "declared system dbus access will not work");
	}
	{
		const char* a11y = cli_getenv("AT_SPI_BUS_ADDRESS");
		if (a11y && *a11y) a->a11y = xstrdup(a11y);
	}
	if (!a->a11y && plan->xdg_runtime_dir && *plan->xdg_runtime_dir) {
		static const char* names[] = {"bus_0", "bus"};

		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			char path[PATH_MAX], addr[PATH_MAX + 16];

			snprintf(path, sizeof(path), "%s/at-spi/%s",
				 plan->xdg_runtime_dir, names[i]);
			if (!unix_socket_live(path)) continue;
			snprintf(addr, sizeof(addr), "unix:path=%s", path);
			a->a11y = xstrdup(addr);
			break;
		}
	}
	if (!a->a11y) {
		char* sess = dbus_host_session_address(plan);

		int fd[2];
		FILE* fp;
		pid_t pid;

		if (pipe(fd) != 0) {
			free(sess);
			bus_addrs_free(a);
			return -1;
		}
		pid = fork();
		if (pid == 0) {
			char* av[] = {(char*)"dbus-send",
				      (char*)"--session",
				      (char*)"--dest=org.a11y.Bus",
				      (char*)"--type=method_call",
				      (char*)"--print-reply",
				      (char*)"--reply-timeout=30000",
				      (char*)"/org/a11y/bus",
				      (char*)"org.a11y.Bus.GetAddress",
				      NULL};
			close(fd[0]);
			dup2(fd[1], STDOUT_FILENO);
			if (fd[1] != STDOUT_FILENO) close(fd[1]);
			{
				int null = open("/dev/null", O_WRONLY);
				if (null >= 0) {
					dup2(null, STDERR_FILENO);
					if (null != STDERR_FILENO) close(null);
				}
			}
			if (sess != NULL)
				setenv("DBUS_SESSION_BUS_ADDRESS", sess, 1);
			execvp("/usr/local/bin/dbus-send", av);
			_exit(127);
		}
		close(fd[1]);
		free(sess);
		if (pid < 0) {
			close(fd[0]);
			bus_addrs_free(a);
			return -1;
		}
		fp = fdopen(fd[0], "r");
		if (fp) {
			char line[2048];
			while (fgets(line, sizeof(line), fp)) {
				char* p = strstr(line, "string \"");
				if (!p) continue;
				p += 8;
				char* end = strchr(p, '"');
				if (!end || p == end) continue;
				*end = '\0';
				if (strncmp(p, "unix:path=", 10) == 0)
					a->a11y = xstrdup(p);
				break;
			}
			fclose(fp);
			if (pid > 0) {
				int ws;
				while (waitpid(pid, &ws, 0) < 0 &&
				       errno == EINTR);
			}
		}
		if (a->a11y == NULL)
			fpkg_infof(
				"no accessibility bus found (org.a11y.Bus "
				"reported none); the app will run without one");
	}

	return 0;
}

// The proxy's argv: one bus address, its socket path, and the policy filters
// that bus carries.  av_push copies, so the addresses stay the caller's.
static void fill_proxy_argv(argvec_t* av, sandbox_t* sb,
			    const sandbox_plan_t* plan, const char* proxy_dir,
			    const char* bus_id, const bus_addrs_t* a) {
	if (a->session) {
		char sock[PATH_MAX];
		snprintf(sock, sizeof(sock), "%s/%s-session-bus", proxy_dir,
			 bus_id);
		(void)unlink(sock);
		av_push(av, a->session);
		av_push(av, sock);
		if (!a->session_raw) {
			add_bus_filter_args(av, sb->app_id,
					    &plan->session_bus_policy);
			av_push(av, "--call=org.freedesktop.portal.*=*");
			av_push(av, "--broadcast=org.freedesktop.portal.*=@/"
				     "org/freedesktop/portal/*");
		}
	}
	if (a->system) {
		char sock[PATH_MAX];
		snprintf(sock, sizeof(sock), "%s/%s-system-bus", proxy_dir,
			 bus_id);
		(void)unlink(sock);
		av_push(av, a->system);
		av_push(av, sock);
		add_bus_filter_args(av, NULL, &plan->system_bus_policy);
	}
	if (a->a11y) {
		char sock[PATH_MAX];
		snprintf(sock, sizeof(sock), "%s/%s-a11y-bus", proxy_dir,
			 bus_id);
		(void)unlink(sock);
		av_push(av, a->a11y);
		av_push(av, sock);
		av_push(av, "--filter");
		av_push(av, "--sloppy-names");
		static const char* a11y_rules[] = {
			"--broadcast=org.a11y.atspi.Registry=org.a11y.atspi."
			"Registry."
			"EventListenerRegistered@/org/a11y/atspi/registry",
			"--broadcast=org.a11y.atspi.Registry=org.a11y.atspi."
			"Registry."
			"EventListenerDeregistered@/org/a11y/atspi/registry",
			"--call=org.a11y.atspi.Registry=org.a11y.atspi.Socket."
			"Embed@/org/a11y/"
			"atspi/accessible/root",
			"--call=org.a11y.atspi.Registry=org.a11y.atspi.Socket."
			"Unembed@/org/"
			"a11y/atspi/accessible/root",
			"--call=org.a11y.atspi.Registry=org.a11y.atspi."
			"Registry."
			"GetRegisteredEvents@/org/a11y/atspi/registry",
			"--call=org.a11y.atspi.Registry=org.a11y.atspi."
			"DeviceEventController."
			"GetKeystrokeListeners@/org/a11y/atspi/registry/"
			"deviceeventcontroller",
			"--call=org.a11y.atspi.Registry=org.a11y.atspi."
			"DeviceEventController."
			"GetDeviceEventListeners@/org/a11y/atspi/registry/"
			"deviceeventcontroller",
			"--call=org.a11y.atspi.Registry=org.a11y.atspi."
			"DeviceEventController."
			"NotifyListenersSync@/org/a11y/atspi/registry/"
			"deviceeventcontroller",
			"--call=org.a11y.atspi.Registry=org.a11y.atspi."
			"DeviceEventController."
			"NotifyListenersAsync@/org/a11y/atspi/registry/"
			"deviceeventcontroller",
			NULL,
		};
		for (int i = 0; a11y_rules[i]; i++) av_push(av, a11y_rules[i]);
		add_bus_filter_args(av, NULL, &plan->a11y_bus_policy);
	}
}

static void publish_bus_socks(sandbox_t* sb, const sandbox_plan_t* plan,
			      const char* bus_id, const bus_addrs_t* a) {
	sb->session_bus_proxied = a->session != NULL;
	sb->system_bus_proxied = a->system != NULL;
	sb->a11y_bus_proxied = a->a11y != NULL;
	if (a->session) {
		free(sb->session_bus_sock);
		sb->session_bus_sock = xasprintf(
			"unix:path=/run/flatpak/.dbus-proxy/%s-session-bus",
			bus_id);

		char link[PATH_MAX], target[PATH_MAX];
		snprintf(link, sizeof(link), "%s/run/user/%u/bus", sb->jailroot,
			 (unsigned)plan->uid);
		snprintf(target, sizeof(target),
			 "/run/flatpak/.dbus-proxy/%s-session-bus", bus_id);
		(void)unlink(link);
		if (symlink(target, link) != 0)
			fpkg_warnf("cannot link %s -> %s: %s", link, target,
				   strerror(errno));
	}
	if (a->system) {
		free(sb->system_bus_sock);
		sb->system_bus_sock = xasprintf(
			"unix:path=/run/flatpak/.dbus-proxy/%s-system-bus",
			bus_id);
	}
	if (a->a11y) {
		free(sb->a11y_bus_sock);
		sb->a11y_bus_sock = xasprintf(
			"unix:path=/run/flatpak/.dbus-proxy/%s-a11y-bus",
			bus_id);
	}
}

int dbus_start_proxies(sandbox_t* sb, const sandbox_plan_t* plan) {
	const char* proxy = dbus_proxy_binary();

	sb->bus_proxy_pid = 0;
	sb->bus_sync_fd = -1;

	if (!proxy) {
		if (plan->sock_session_bus)
			fpkg_warnf("xdg-dbus-proxy not found; session bus "
				   "uses the raw socket bind (set "
				   "FLATPAK_DBUSPROXY "
				   "to enable filtering)");
		return 0;
	}

	bus_addrs_t a = {0};
	if (bus_addrs_resolve(&a, plan) != 0) return -1;
	if (!a.session && !a.system && !a.a11y) {
		bus_addrs_free(&a);
		return 0;
	}

	char* proxy_dir = xasprintf("%s/.dbus-proxy", plan->xdg_runtime_dir);
	char bus_id[320];
	snprintf(bus_id, sizeof(bus_id), "%s.%ld", sb->app_id, (long)getpid());
	{
		pid_t pid = fork();
		if (pid == 0) {
			if (initgroups(plan->user_name, plan->gid) != 0 ||
			    setgid(plan->gid) != 0 || setuid(plan->uid) != 0)
				_exit(1);
			int rc = mkdir_p(proxy_dir, 0755);
			// _exit does not run the parent's cleanup; release what this
			// child will not use so a leak check does not blame it.
			free(a.session);
			free(a.system);
			free(a.a11y);
			free(proxy_dir);
			_exit(rc == 0 ? 0 : 1);
		}
		if (pid > 0) {
			int ws = 0;
			while (waitpid(pid, &ws, 0) < 0 && errno == EINTR);
		}
	}

	argvec_t av = {0};
	fill_proxy_argv(&av, sb, plan, proxy_dir, bus_id, &a);

	{
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/run/flatpak/.dbus-proxy",
			 sb->jailroot);
		if (mkdir_p(path, 0755) != 0) {
			fpkg_errorf("mkdir %s: %s", path, strerror(errno));
			goto fail;
		}
		// Unmount a stale bind from an aborted run. No shell: the path comes from the
		// caller's XDG_RUNTIME_DIR, so it goes over as its own argv entry.
		if (sb_is_mounted(sb, path)) {
			char* av[] = {"/sbin/umount", "-f", path, NULL};
			(void)run_program(av, -1, -1, -1);
		}
		if (m_nullfs(sb, proxy_dir, path, true) != 0) {
			fpkg_warnf("could not bind %s into the jail; "
				   "the session bus will be unreachable",
				   proxy_dir);
		}
	}

	int syncfd[2];
	if (pipe(syncfd) != 0) {
		fpkg_errorf("pipe: %s", strerror(errno));
		goto fail;
	}

	char fdarg[32];
	snprintf(fdarg, sizeof(fdarg), "--fd=%d", syncfd[1]);

	pid_t pid = fork();
	if (pid < 0) {
		fpkg_errorf("fork: %s", strerror(errno));
		close(syncfd[0]);
		close(syncfd[1]);
		goto fail;
	}
	if (pid == 0) {
		if (initgroups(plan->user_name, plan->gid) != 0 ||
		    setgid(plan->gid) != 0 || setuid(plan->uid) != 0) {
			dprintf(2,
				"flatpkg: cannot drop to %s (%ld): %s - "
				"refusing to run %s as root\n",
				plan->user_name, (long)plan->uid,
				strerror(errno), proxy);
			_exit(127);
		}

		char** argv = calloc(av.count + 3, sizeof(char*));
		if (!argv) _exit(127);
		argv[0] = (char*)proxy;
		argv[1] = fdarg;
		for (size_t i = 0; i < av.count; i++) argv[i + 2] = av.items[i];
		argv[av.count + 2] = NULL;
		execv(proxy, argv);
		dprintf(2, "flatpkg: exec %s: %s\n", proxy, strerror(errno));
		_exit(127);
	}

	close(syncfd[1]);

	char b;
	ssize_t r;
	do {
		r = read(syncfd[0], &b, 1);
	} while (r < 0 && errno == EINTR);
	if (r != 1) {
		close(syncfd[0]);
		waitpid(pid, NULL, 0);
		av_free(&av);
		free(proxy_dir);
		bus_addrs_free(&a);
		fpkg_errorf("xdg-dbus-proxy failed to start");
		return -1;
	}

	sb->bus_proxy_pid = pid;
	sb->bus_sync_fd = syncfd[0];
	sb->bus_id = strdup(bus_id);
	sb->bus_proxy_dir = strdup(proxy_dir);
	publish_bus_socks(sb, plan, bus_id, &a);

	av_free(&av);
	free(proxy_dir);
	bus_addrs_free(&a);
	return 0;

fail:
	av_free(&av);
	free(proxy_dir);
	bus_addrs_free(&a);
	return -1;
}

void dbus_stop_proxies(sandbox_t* sb) {
	if (sb->bus_proxy_pid > 0) {
		kill(sb->bus_proxy_pid, SIGTERM);
		int status = 0;
		while (waitpid(sb->bus_proxy_pid, &status, 0) < 0 &&
		       errno == EINTR);
		sb->bus_proxy_pid = 0;
	}
	if (sb->bus_sync_fd >= 0) {
		close(sb->bus_sync_fd);
		sb->bus_sync_fd = -1;
	}
	sb->session_bus_proxied = false;
	sb->system_bus_proxied = false;
	sb->a11y_bus_proxied = false;
	if (sb->bus_id != NULL && sb->bus_proxy_dir != NULL) {
		static const char* const suffixes[] = {
			"-session-bus",
			"-system-bus",
			"-a11y-bus",
			NULL,
		};
		for (int i = 0; suffixes[i] != NULL; i++) {
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "%s/%s%s",
				 sb->bus_proxy_dir, sb->bus_id, suffixes[i]);
			(void)unlink(path);
		}
	}
	free(sb->bus_id);
	free(sb->bus_proxy_dir);
	sb->bus_id = NULL;
	sb->bus_proxy_dir = NULL;
	free(sb->session_bus_sock);
	free(sb->system_bus_sock);
	free(sb->a11y_bus_sock);
	sb->session_bus_sock = NULL;
	sb->system_bus_sock = NULL;
	sb->a11y_bus_sock = NULL;
}
