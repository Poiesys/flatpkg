// bus-unit.c - the daemon's event bus without a daemon: broker and worker
// sides forked and joined by the same socketpairs, frames as opaque bytes.
// Live fan-out, the late-subscriber snapshot and cancel are the subject.
#include "bus.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail;
static void note(const char* what) {
	printf("FAIL: %s\n", what);
	fail = 1;
}

static void broker_loop(void) {
	for (;;) {
		struct pollfd p[8];
		int n = broker_pollfds(p, 8);
		if (n == 0) break;
		int pr = poll(p, (nfds_t)n, 5000);
		if (pr <= 0) break;
		for (int i = 0; i < n; i++)
			if (p[i].revents != 0) broker_handle(p[i].fd);
	}
	_exit(0);
}

// Read `want` forwarded frames and compare each with the next expected
// string.  Returns 0 when all arrived in order.
static int expect_frames(int want, const char* const* expected) {
	for (int i = 0; i < want; i++) {
		char buf[256];
		size_t len;
		uint32_t type;
		int rc;

		for (;;) {
			rc = bus_next(buf, sizeof(buf), &len, &type);
			if (rc == 0) break;
			if (rc == -2) {
				fprintf(stderr, "bus closed before frame %d\n", i);
				return -1;
			}
			// -1 is "nothing right now"; the broker may not have
			// answered yet.
			struct pollfd p = {bus_worker_fd(), POLLIN, 0};
			if (poll(&p, 1, 2000) <= 0) {
				fprintf(stderr, "timeout waiting frame %d\n", i);
				return -1;
			}
		}
		if (type != BUS_FORWARD || len != strlen(expected[i]) ||
		    memcmp(buf, expected[i], len) != 0) {
			fprintf(stderr, "frame %d is type %u len %zu, not %s\n",
				i, type, len, expected[i]);
			return -1;
		}
	}
	return 0;
}

static int wait_subscribed(void) {
	for (;;) {
		char buf[8];
		size_t len;
		uint32_t type;
		int rc = bus_next(buf, sizeof(buf), &len, &type);
		if (rc == 0 && type == BUS_SUBSCRIBED) return 0;
		if (rc == -2) return -1;
	}
}

static void wait_byte(int fd) {
	char c;
	while (read(fd, &c, 1) < 0 && errno == EINTR) {}
}

static void write_byte(int fd) {
	(void)write(fd, "x", 1);
}

// ---- live fan-out ----------------------------------------------------------

static void test_live(void) {
	int a[2], b[2], ready[2];
	pid_t pa, pb, broker;

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, a) != 0 ||
	    socketpair(AF_UNIX, SOCK_DGRAM, 0, b) != 0 ||
	    pipe(ready) != 0)
		exit(2);

	pb = fork();
	if (pb == 0) {
		static const char* const want[] = {"one", "two"};
		close(a[0]);
		close(a[1]);
		close(b[0]);
		close(ready[0]);
		bus_worker_init(b[1]);
		if (bus_subscribe("t") != 0) _exit(3);
		if (wait_subscribed() != 0) _exit(3);
		write_byte(ready[1]);
		_exit(expect_frames(2, want) == 0 ? 0 : 1);
	}
	pa = fork();
	if (pa == 0) {
		close(b[0]);
		close(b[1]);
		close(a[0]);
		close(ready[0]);
		bus_worker_init(a[1]);
		(void)bus_publish("t", "one", 3);
		(void)bus_publish("t", "two", 3);
		usleep(100000);
		_exit(0);
	}
	broker = fork();
	if (broker == 0) {
		close(a[1]);
		close(b[1]);
		close(ready[0]);
		close(ready[1]);
		(void)broker_add(a[0], pa, 0);
		(void)broker_add(b[0], pb, 0);
		broker_loop();
	}
	close(a[0]);
	close(a[1]);
	close(b[0]);
	close(b[1]);
	close(ready[1]);
	wait_byte(ready[0]);
	int st;
	waitpid(pb, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) note("live fan-out");
	waitpid(pa, &st, 0);
	waitpid(broker, &st, 0);
}

// ---- late subscriber -------------------------------------------------------

static void test_snapshot(void) {
	int a[2], b[2], ready[2], go[2];
	pid_t pa, pb, broker;

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, a) != 0 ||
	    socketpair(AF_UNIX, SOCK_DGRAM, 0, b) != 0 ||
	    pipe(ready) != 0 || pipe(go) != 0)
		exit(2);
	pb = fork();
	if (pb == 0) {
		static const char* const want[] = {"one", "two"};
		close(a[0]);
		close(a[1]);
		close(b[0]);
		close(ready[0]);
		close(go[1]);
		bus_worker_init(b[1]);
		write_byte(ready[1]);
		wait_byte(go[0]);
		if (bus_subscribe("t") != 0) _exit(3);
		if (wait_subscribed() != 0) _exit(3);
		_exit(expect_frames(2, want) == 0 ? 0 : 1);
	}
	pa = fork();
	if (pa == 0) {
		close(b[0]);
		close(b[1]);
		close(a[0]);
		close(ready[0]);
		close(go[0]);
		close(go[1]);
		bus_worker_init(a[1]);
		(void)bus_publish("t", "one", 3);
		(void)bus_publish("t", "two", 3);
		usleep(100000);
		_exit(0);
	}
	broker = fork();
	if (broker == 0) {
		close(a[1]);
		close(b[1]);
		close(ready[0]);
		close(ready[1]);
		close(go[0]);
		close(go[1]);
		(void)broker_add(a[0], pa, 0);
		(void)broker_add(b[0], pb, 0);
		broker_loop();
	}
	close(a[0]);
	close(a[1]);
	close(b[0]);
	close(b[1]);
	close(ready[1]);
	wait_byte(ready[0]);
	sleep(1); // let A publish and exit; B has not subscribed yet
	write_byte(go[1]);
	int st;
	waitpid(pb, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) note("late-subscriber snapshot");
	waitpid(pa, &st, 0);
	waitpid(broker, &st, 0);
	close(go[1]);
}

// ---- cancel ----------------------------------------------------------------

static volatile sig_atomic_t got_cancel;
static void on_signal(int sig) {
	(void)sig;
	got_cancel = 1;
}

static void test_cancel(void) {
	int a[2], b[2];
	pid_t pa, pb, broker;

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, a) != 0 ||
	    socketpair(AF_UNIX, SOCK_DGRAM, 0, b) != 0)
		exit(2);
	pa = fork();
	if (pa == 0) {
		close(a[0]);
		close(b[0]);
		close(b[1]);
		(void)signal(SIGUSR1, on_signal);
		bus_worker_init(a[1]);
		(void)bus_register(42);
		// Wait for the broker's signal.
		for (int i = 0; i < 500 && !got_cancel; i++) usleep(10000);
		_exit(got_cancel ? 0 : 1);
	}
	pb = fork();
	if (pb == 0) {
		close(b[0]);
		close(a[0]);
		close(a[1]);
		bus_worker_init(b[1]);
		usleep(300000); // let the broker record A's op
		(void)bus_cancel(42);
		_exit(0);
	}
	broker = fork();
	if (broker == 0) {
		close(a[1]);
		close(b[1]);
		(void)broker_add(a[0], pa, 0);
		(void)broker_add(b[0], pb, 0);
		broker_loop();
	}
	close(a[0]);
	close(a[1]);
	close(b[0]);
	close(b[1]);
	int st;
	waitpid(pa, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) note("cancel did not reach the owner");
	waitpid(pb, &st, 0);
	waitpid(broker, &st, 0);
}

int main(void) {
	test_live();
	test_snapshot();
	test_cancel();
	if (fail) return 1;
	printf("PASS: the event bus routes live frames, replays a snapshot to a\n"
	       "      late subscriber, and delivers cancel to the operation's "
	       "owner\n");
	return 0;
}
