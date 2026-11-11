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

static int wait_type(uint32_t want) {
	for (;;) {
		char buf[8];
		size_t len;
		uint32_t type;
		int rc = bus_next(buf, sizeof(buf), &len, &type);

		if (rc == 0) {
			if (type == want) return 0;
			continue;
		}
		if (rc == -2) return -1;
		struct pollfd p = {bus_worker_fd(), POLLIN, 0};
		if (poll(&p, 1, 2000) <= 0) return -1;
	}
}

static int wait_subscribed(void) { return wait_type(BUS_SUBSCRIBED); }

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


// ---- authorization --------------------------------------------------------
// The op id is broadcast and the cancel and the per-op subscription both name
// it: a peer of another uid gets a refusal, and the owner is not signalled.
static void test_authorization(void) {
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
		(void)bus_register(4242);
		for (int i = 0; i < 150 && !got_cancel; i++) usleep(10000);
		_exit(got_cancel ? 1 : 0); // a cross-uid cancel must not arrive
	}
	pb = fork();
	if (pb == 0) {
		close(b[0]);
		close(a[0]);
		close(a[1]);
		bus_worker_init(b[1]);
		usleep(300000);
		if (bus_cancel(4242) != 0 ||
		    wait_type(BUS_CANCEL_DENIED) != 0) {
			note("a cancel from another uid was not refused");
			_exit(1);
		}
		if (bus_subscribe("op/4242") != 0 ||
		    wait_type(BUS_SUB_DENIED) != 0) {
			note("another uid's operation was watchable");
			_exit(1);
		}
		_exit(0);
	}
	broker = fork();
	if (broker == 0) {
		close(a[1]);
		close(b[1]);
		(void)broker_add(a[0], pa, 1);
		(void)broker_add(b[0], pb, 2);
		broker_loop();
	}
	close(a[0]);
	close(a[1]);
	close(b[0]);
	close(b[1]);
	int st;
	waitpid(pa, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
		note("a cross-uid cancel reached the owner");
	waitpid(pb, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
		note("the refusals did not reach the requesting peer");
	waitpid(broker, &st, 0);
}

// ---- firehose scope -------------------------------------------------------
// `op` is the firehose, but a non-root subscriber sees its own uid's frames:
// the operations in it are other users' work.
static void test_firehose_scope(void) {
	int p[2], same[2], other[2], ready[2];
	pid_t pub, mine, theirs, broker;

	if (socketpair(AF_UNIX, SOCK_DGRAM, 0, p) != 0 ||
	    socketpair(AF_UNIX, SOCK_DGRAM, 0, same) != 0 ||
	    socketpair(AF_UNIX, SOCK_DGRAM, 0, other) != 0 ||
	    pipe(ready) != 0)
		exit(2);
	theirs = fork();
	if (theirs == 0) {
		char buf[8];
		size_t len;
		uint32_t type;

		close(p[0]);
		close(p[1]);
		close(same[0]);
		close(same[1]);
		close(other[0]);
		close(ready[0]);
		bus_worker_init(other[1]);
		if (bus_subscribe("op") != 0 ||
		    wait_subscribed() != 0)
			_exit(3);
		write_byte(ready[1]);
		usleep(1200000);
		// Nothing was published by uid 2: a readable socket here is a
		// leaked frame.
		_exit(bus_next(buf, sizeof(buf), &len, &type) == 0 ? 1 : 0);
	}
	mine = fork();
	if (mine == 0) {
		close(p[0]);
		close(p[1]);
		close(other[0]);
		close(other[1]);
		close(same[0]);
		close(ready[0]);
		bus_worker_init(same[1]);
		if (bus_subscribe("op") != 0 ||
		    wait_subscribed() != 0)
			_exit(3);
		write_byte(ready[1]);
		_exit(wait_type(BUS_FORWARD) == 0 ? 0 : 1);
	}
	pub = fork();
	if (pub == 0) {
		close(p[0]);
		close(same[0]);
		close(same[1]);
		close(other[0]);
		close(other[1]);
		close(ready[0]);
		close(ready[1]);
		bus_worker_init(p[1]);
		usleep(500000); // both subscriptions are in
		(void)bus_publish("op", "mine", 4);
		usleep(200000);
		_exit(0);
	}
	broker = fork();
	if (broker == 0) {
		close(p[1]);
		close(same[1]);
		close(other[1]);
		close(ready[0]);
		close(ready[1]);
		(void)broker_add(p[0], pub, 1);
		(void)broker_add(same[0], mine, 1);
		(void)broker_add(other[0], theirs, 2);
		broker_loop();
	}
	close(p[0]);
	close(p[1]);
	close(same[0]);
	close(same[1]);
	close(other[0]);
	close(other[1]);
	close(ready[1]);
	// Both subscribers are up before the publisher starts.
	wait_byte(ready[0]);
	wait_byte(ready[0]);
	int st;
	waitpid(mine, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
		note("a same-uid op frame was not delivered");
	waitpid(theirs, &st, 0);
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
		note("another uid's op frame was delivered on the firehose");
	waitpid(pub, &st, 0);
	waitpid(broker, &st, 0);
}

int main(void) {
	test_live();
	test_snapshot();
	test_cancel();
	test_authorization();
	test_firehose_scope();
	if (fail) return 1;
	printf("PASS: the event bus routes live frames, replays a snapshot to a\n"
	       "      late subscriber, delivers cancel to the operation's owner,\n"
	       "      and refuses another user's operation and its frames\n");
	return 0;
}
