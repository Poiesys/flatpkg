// run-program-sinks.c - run_program() with one fd for both sinks.
//
// Callers that silence a child pass the same /dev/null as stdout and stderr.
// Closing that fd after the stdout dup leaves the stderr dup with EBADF, so the
// child writes to the caller's terminal instead of the sink.
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int run_program(char* const argv[], int in_fd, int out_fd, int err_fd);

int main(void) {
	char* talk[] = {"/bin/sh", "-c", "echo out; echo boom >&2", NULL};
	int devnull = open("/dev/null", O_WRONLY);
	char buf[64];
	ssize_t n;
	int p[2];

	if (devnull < 0 || pipe(p) != 0) {
		perror("setup");
		return 2;
	}
	// Both sinks are this fd: the child's stderr must reach /dev/null, so
	// nothing below appears on this process's stderr.
	(void)run_program(talk, -1, devnull, devnull);
	// Distinct sinks still work, so the case above is not passing because
	// stderr was never written at all.
	(void)run_program(talk, -1, devnull, p[1]);
	close(p[1]);
	n = read(p[0], buf, sizeof(buf) - 1);
	close(p[0]);
	close(devnull);
	if (n <= 0) {
		fprintf(stderr, "FAIL: the child's stderr did not reach the sink\n");
		return 1;
	}
	buf[n] = '\0';
	if (strstr(buf, "boom") == NULL) {
		fprintf(stderr, "FAIL: the child's stderr went somewhere else: %s",
			buf);
		return 1;
	}
	if (strstr(buf, "out") != NULL) {
		fprintf(stderr, "FAIL: the child's stdout leaked into the stderr "
				"sink\n");
		return 1;
	}
	return 0;
}
