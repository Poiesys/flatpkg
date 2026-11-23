#include "flatpkg.h"
#define FPKG_MODULE "daemon"
#include "fpkglog.h"
#include "typed_worker.h"
#include "repo.h"
#include "bus.h"

#include <grp.h>
#include <libutil.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FPKG_RUNDIR "/var/run/flatpkg"

// The root daemon serves the system installation; --user serves one user's
// store and socket and runs with that user's privileges.
static bool g_user;
static const char* g_socket = FPKG_SOCKET;
static const char* g_store = FLATPKG_STORE_DIR;
static char g_user_socket_path[PATH_MAX];
static char g_user_store_path[PATH_MAX];

bool daemon_is_user(void) { return g_user; }

const char* daemon_store(void) { return g_store; }

// The group is fixed for the daemon's life; a lookup per connection in the
// single-threaded loop stalls every user when the source is slow (LDAP, NIS).
static gid_t g_flatpkg_gid = (gid_t)-1;
static bool g_flatpkg_gid_known;

static int group_allowed(uid_t uid) {
	if (uid == 0) return 1;
	if (!g_flatpkg_gid_known) {
		struct group* g = getgrnam("flatpkg");

		g_flatpkg_gid = g != NULL ? g->gr_gid : (gid_t)-1;
		g_flatpkg_gid_known = true;
	}
	if (g_flatpkg_gid == (gid_t)-1 || g_flatpkg_gid_known == false)
		return 0;
	struct passwd* pw = getpwuid(uid);
	if (!pw) return 0;
	struct group* g = NULL;
	gid_t want = g_flatpkg_gid;
	gid_t groups[NGROUPS_MAX];
	int ngroups = NGROUPS_MAX;
	if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) < 0)
		return 0;
	(void)g;
	for (int i = 0; i < ngroups; i++)
		if (groups[i] == want) return 1;
	return 0;
}

static bool control_socket_live(void) {
	struct sockaddr_un sa;
	int fd;
	bool live = false;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return false;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (strlcpy(sa.sun_path, g_socket, sizeof(sa.sun_path)) <
		    sizeof(sa.sun_path) &&
	    connect(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0)
		live = true;
	close(fd);
	return live;
}

static void store_repair_ownership(void) {
	struct group* g = getgrnam("flatpkg");
	struct stat st;

	if (!g) return;
	if (stat(FLATPKG_STORE_DIR, &st) == 0 && st.st_gid == g->gr_gid) return;

	// Detached: a recursive chown can take a while on a full store, and the
	// daemon must start serving.  The child reports a failure to stderr.
	pid_t pid = fork();
	if (pid != 0) return;

	char owner[32];
	snprintf(owner, sizeof(owner), "0:%u", (unsigned)g->gr_gid);
	char* recursive[] = {"/usr/sbin/chown", "-R", owner,
			     (char*)FLATPKG_STORE_DIR "/app",
			     (char*)FLATPKG_STORE_DIR "/extension",
			     (char*)FLATPKG_STORE_DIR "/runtime",
			     (char*)FLATPKG_STORE_DIR "/keys",
			     (char*)FLATPKG_STORE_DIR "/world", NULL};
	char* top[] = {"/usr/sbin/chown", owner,
		       (char*)FLATPKG_STORE_DIR,
		       (char*)FLATPKG_STORE_DIR "/repo", NULL};

	(void)setsid();
	run_program(recursive, -1, -1, -1);
	run_program(top, -1, -1, -1);
	_exit(0);
}

static void worker(int conn, int bus, uid_t uid, gid_t gid) {
	typed_worker(conn, bus, uid, gid);
}

int daemon_main(bool user) {
	if (user) {
		if (geteuid() == 0) {
			fpkg_errorf("--user must run as the unprivileged "
				    "user, not root");
			return 1;
		}
		if (user_socket_path(g_user_socket_path,
				     sizeof(g_user_socket_path)) != 0 ||
		    user_store_path(g_user_store_path,
				    sizeof(g_user_store_path)) != 0) {
			fpkg_errorf("cannot locate the user installation");
			return 1;
		}
		g_socket = g_user_socket_path;
		g_store = g_user_store_path;
		if (setenv("FLATPKG_STORE", g_store, 1) != 0) {
			fpkg_errorf("cannot set the user store");
			return 1;
		}
		g_user = true;
	} else if (geteuid() != 0) {
		fpkg_errorf("daemon must run as root");
		return 1;
	}
	// Children have to be waitable: the daemon's own helpers - the base devfs
	// rulesets, the store repair, the ruleset sweep - read an exit status,
	// and with SIGCHLD ignored waitpid() answers ECHILD and every one of them
	// "fails".  Workers are reaped in the loop below instead.
	(void)signal(SIGCHLD, SIG_DFL);
	(void)signal(SIGPIPE, SIG_IGN);
	(void)signal(SIGHUP, SIG_IGN);

	// Re-install base devfs rulesets (1000-1003) at every start: they live
	// in-kernel only and are wiped on reboot.
	if (!g_user) install_devfs_base_rulesets();

	if (g_user) {
		// $XDG_RUNTIME_DIR/flatpkg is the user daemon's run directory;
		// only this user may look in or unlink the socket.
		char dir[PATH_MAX];
		char* slash;

		snprintf(dir, sizeof(dir), "%s", g_socket);
		slash = strrchr(dir, '/');
		if (slash == NULL) {
			fpkg_errorf("user socket has no directory: %s",
				    g_socket);
			return 1;
		}
		*slash = '\0';
		if (mkdir_p(dir, 0700) != 0) {
			fpkg_errorf("mkdir %s: %s", dir, strerror(errno));
			return 1;
		}
		(void)chmod(dir, 0700);
		{
			// A directory another user created first would let them
			// answer the socket; it has to be this user's own 0700.
			struct stat st;

			if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) ||
			    st.st_uid != getuid() ||
			    (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
				fpkg_errorf("the user socket directory %s is "
					    "not this user's own 0700 directory",
					    dir);
				return 1;
			}
		}
	} else {
		// Only the flatpkg group may look in or write here: a
		// world-writable run directory lets anyone unlink the control
		// socket.
		if (mkdir_p(FPKG_RUNDIR, 0770) != 0) {
			fpkg_errorf("mkdir %s: %s", FPKG_RUNDIR,
				    strerror(errno));
			return 1;
		}
		(void)chmod(FPKG_RUNDIR, 0770);
		{
			struct group* g = getgrnam("flatpkg");
			if (g) (void)chown(FPKG_RUNDIR, 0, g->gr_gid);
		}
	}
	for (int attempt = 0; attempt < 100; attempt++) {
		if (!control_socket_live()) break;
		if (attempt == 99) {
			fpkg_errorf("another flatpkg daemon is already "
				    "running (socket %s is live); refusing to "
				    "start a second instance",
				    g_socket);
			return 1;
		}
		usleep(100000);
	}
	if (access(g_socket, F_OK) == 0) unlink(g_socket);

	struct sockaddr_un sa;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (strlcpy(sa.sun_path, g_socket, sizeof(sa.sun_path)) >=
	    sizeof(sa.sun_path)) {
		fpkg_errorf("socket path too long");
		return 1;
	}
	int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0) {
		fpkg_errorf("socket: %s", strerror(errno));
		return 1;
	}
	if (bind(lfd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
		fpkg_errorf("bind %s: %s", g_socket, strerror(errno));
		return 1;
	}
	if (chmod(g_socket, g_user ? 0600 : 0660) != 0) {
		fpkg_errorf("chmod %s: %s", g_socket, strerror(errno));
		return 1;
	}
	if (!g_user) {
		struct group* g = getgrnam("flatpkg");
		if (g) (void)chown(g_socket, 0, g->gr_gid);
	}
	if (listen(lfd, 16) != 0) {
		fpkg_errorf("listen: %s", strerror(errno));
		return 1;
	}
	// One daemon owns the run dir now; a tree no jail refers to is a leak.
	if (!g_user) sandbox_gc_leaks();
	if (!g_user) sandbox_gc_rulesets();
	fpkg_infof("flatpkg daemon listening on %s (libostree %s)", g_socket,
		   fp_repo_version());

	if (mkdir_p(g_store, 0775) != 0) {
		fpkg_errorf("mkdir %s: %s", g_store, strerror(errno));
		return 1;
	}
	if (!g_user) {
		store_repair_ownership();
		ostree_summary_cache_own();
	}
	// A missing world is the installed set; persist that once so a
	// read-only client does not have to write the store to see it.
	(void)world_seed();

	for (;;) {
		struct pollfd pfd[1 + BUS_MAX_PEERS];
		int n;

		// Every worker is this process's child: reap them here.
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;
		n = 1 + broker_pollfds(pfd + 1, BUS_MAX_PEERS);
		pfd[0].fd = lfd;
		pfd[0].events = POLLIN;
		pfd[0].revents = 0;
		if (poll(pfd, (nfds_t)n, -1) < 0) {
			if (errno == EINTR) continue;
			fpkg_errorf("poll: %s", strerror(errno));
			continue;
		}
		for (int i = 1; i < n; i++)
			if (pfd[i].revents != 0) broker_handle(pfd[i].fd);
		if ((pfd[0].revents & POLLIN) == 0) continue;

		int conn = accept(lfd, NULL, NULL);
		if (conn < 0) {
			if (errno == EINTR) continue;
			fpkg_errorf("accept: %s", strerror(errno));
			sleep(1);
			continue;
		}
		uid_t uid = 0;
		gid_t gid = 0;
		if (getpeereid(conn, &uid, &gid) != 0) {
			close(conn);
			continue;
		}
		bool allowed = g_user
				       ? (uid == geteuid() && gid == getegid())
				       : group_allowed(uid);
		if (!allowed) {
			fpkg_errorf("denied uid=%u=%u: not the owner of "
				    "this user installation",
				    (unsigned)uid, (unsigned)geteuid());
			typed_refuse(conn,
				     g_user ? "this user daemon serves only "
					      "its owner"
					    : "you have to be in the "
					      "'flatpkg' group to do this "
					      "action");
			close(conn);
			continue;
		}
		// One root worker per connection, so the count is a resource:
		// refuse before the fork instead of after it.
		if (broker_peer_count() >= BUS_MAX_PEERS ||
		    broker_peer_count_uid(uid) >= BUS_MAX_PEERS_PER_UID) {
			fpkg_errorf("refused uid=%u: %d connections (%d for "
				    "this user)",
				    (unsigned)uid, broker_peer_count(),
				    broker_peer_count_uid(uid));
			typed_refuse(conn, broker_peer_count() >= BUS_MAX_PEERS
						   ? "the daemon is serving as "
						     "many connections as it can"
						   : "this user is holding as "
						     "many connections as it "
						     "may");
			close(conn);
			continue;
		}
		{
			// A connection that sends half a frame and sleeps holds a
			// root worker; the timeout only lasts until HELLO, so a
			// subscribed client can then idle.
			struct timeval tv = {120, 0};

			(void)setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv,
					 sizeof(tv));
		}
		int sp[2];
		if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sp) != 0) {
			close(conn);
			continue;
		}
		pid_t pid = fork();
		if (pid < 0) {
			close(sp[0]);
			close(sp[1]);
			close(conn);
			continue;
		}
		if (pid == 0) {
			close(lfd);
			close(sp[0]);
			worker(conn, sp[1], uid, gid);
			_exit(1);
		}
		close(sp[1]);
		close(conn);
		if (broker_add(sp[0], pid, uid) != 0) {
			(void)kill(pid, SIGTERM);
			close(sp[0]);
		}
	}
}
