// The listener, the peer check, and one typed worker per connection.
#include "flatpkg.h"
#define FPKG_MODULE "daemon"
#include "fpkglog.h"
#include "typed_worker.h"
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

static int group_allowed(uid_t uid) {
	if (uid == 0) return 1;
	struct passwd* pw = getpwuid(uid);
	if (!pw) return 0;
	struct group* g = getgrnam("flatpkg");
	if (!g) return 0;
	gid_t groups[NGROUPS_MAX];
	int ngroups = NGROUPS_MAX;
	if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &ngroups) < 0)
		return 0;
	for (int i = 0; i < ngroups; i++)
		if (groups[i] == g->gr_gid) return 1;
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
	if (strlcpy(sa.sun_path, FPKG_SOCKET, sizeof(sa.sun_path)) <
		    sizeof(sa.sun_path) &&
	    connect(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0)
		live = true;
	close(fd);
	return live;
}

static void store_repair_ownership(void) {
	struct group* g = getgrnam("flatpkg");
	struct stat st;
	char cmd[512];
	pid_t pid;
	int devnull;

	if (!g) return;
	if (stat(FLATPKG_STORE_DIR, &st) == 0 && st.st_gid == g->gr_gid) return;
	snprintf(cmd, sizeof(cmd),
		 "/usr/sbin/chown -R 0:%u %s/app %s/extension %s/runtime "
		 "%s/keys %s/world 2>/dev/null; "
		 "/usr/sbin/chown 0:%u %s %s/repo 2>/dev/null",
		 (unsigned)g->gr_gid, FLATPKG_STORE_DIR, FLATPKG_STORE_DIR,
		 FLATPKG_STORE_DIR, FLATPKG_STORE_DIR, FLATPKG_STORE_DIR,
		 (unsigned)g->gr_gid, FLATPKG_STORE_DIR, FLATPKG_STORE_DIR);

	pid = fork();
	if (pid != 0) return;
	(void)setsid();
	devnull = open("/dev/null", O_RDWR);
	if (devnull >= 0) {
		(void)dup2(devnull, 0);
		(void)dup2(devnull, 1);
		(void)dup2(devnull, 2);
		if (devnull > 2) close(devnull);
	}
	closefrom(3);
	execl("/bin/sh", "sh", "-c", cmd, (char*)NULL);
	_exit(127);
}

static void worker(int conn, int bus, uid_t uid, gid_t gid) {
	typed_worker(conn, bus, uid, gid);
}

int daemon_main(void) {
	if (geteuid() != 0) {
		fpkg_errorf("daemon must run as root");
		return 1;
	}
	(void)signal(SIGCHLD, SIG_IGN);
	(void)signal(SIGPIPE, SIG_IGN);
	(void)signal(SIGHUP, SIG_IGN);

	// Re-install base devfs rulesets (1000-1003) at every start: they live
	// in-kernel only and are wiped on reboot.
	install_devfs_base_rulesets();

	if (mkdir_p(FPKG_RUNDIR, 0775) != 0) {
		fpkg_errorf("mkdir %s: %s", FPKG_RUNDIR, strerror(errno));
		return 1;
	}
	(void)chmod(FPKG_RUNDIR, 0775);
	{
		struct group* g = getgrnam("flatpkg");
		if (g) (void)chown(FPKG_RUNDIR, 0, g->gr_gid);
	}
	for (int attempt = 0; attempt < 100; attempt++) {
		if (!control_socket_live()) break;
		if (attempt == 99) {
			fpkg_errorf("another flatpkg daemon is already "
				    "running (socket %s is live); refusing to "
				    "start a second instance",
				    FPKG_SOCKET);
			return 1;
		}
		usleep(100000);
	}
	if (access(FPKG_SOCKET, F_OK) == 0) unlink(FPKG_SOCKET);

	struct sockaddr_un sa;
	memset(&sa, 0, sizeof(sa));
	sa.sun_family = AF_UNIX;
	if (strlcpy(sa.sun_path, FPKG_SOCKET, sizeof(sa.sun_path)) >=
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
		fpkg_errorf("bind %s: %s", FPKG_SOCKET, strerror(errno));
		return 1;
	}
	if (chmod(FPKG_SOCKET, 0666) != 0) {
		fpkg_errorf("chmod %s: %s", FPKG_SOCKET, strerror(errno));
		return 1;
	}
	struct group* g = getgrnam("flatpkg");
	if (g) (void)chown(FPKG_SOCKET, 0, g->gr_gid);
	if (listen(lfd, 16) != 0) {
		fpkg_errorf("listen: %s", strerror(errno));
		return 1;
	}
	fpkg_infof("flatpkg daemon listening on %s", FPKG_SOCKET);

	if (mkdir_p(FLATPKG_STORE_DIR, 0775) != 0) {
		fpkg_errorf("mkdir %s: %s", FLATPKG_STORE_DIR, strerror(errno));
		return 1;
	}
	store_repair_ownership();

	for (;;) {
		struct pollfd pfd[1 + BUS_MAX_PEERS];
		int n = 1 + broker_pollfds(pfd + 1, BUS_MAX_PEERS);
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
		if (!group_allowed(uid)) {
			fpkg_errorf("denied uid=%u: not a member of the "
				    "flatpkg group",
				    (unsigned)uid);
			typed_refuse(conn,
				     "you have to be in the 'flatpkg' group to "
				     "do this action");
			close(conn);
			continue;
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
