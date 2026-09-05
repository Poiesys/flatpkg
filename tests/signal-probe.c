/* signal-probe.c - does the sandbox's child-exit notification actually wake a
 * poll()?  signalfd(SIGCHLD) (emulated in src/signalfd_shim.c) and pidfd_open are
 * both checked, with a plain pipe as the control. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

static int
probe_poll(const char *what, int fd, int timeout_ms)
{
        struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
        int r = poll(&p, 1, timeout_ms);

        printf("%s: poll=%d revents=0x%x%s\n", what, r, p.revents,
            r == 0 ? " (TIMEOUT)" : "");
        return r;
}

int
main(void)
{
        /* Control: a pipe must wake poll. */
        int ctrl[2];
        if (pipe(ctrl) != 0) {
                printf("pipe: %s\n", strerror(errno));
                return 1;
        }
        if (write(ctrl[1], "x", 1) != 1)
                printf("pipe write: %s\n", strerror(errno));
        probe_poll("control-pipe", ctrl[0], 1000);

        /* 1. signalfd(SIGCHLD) + a child that exits. */
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        if (sigprocmask(SIG_BLOCK, &mask, NULL) != 0)
                printf("sigprocmask: %s\n", strerror(errno));
        int sfd = signalfd(-1, &mask, 0);
        printf("signalfd fd=%d%s\n", sfd, sfd < 0 ? strerror(errno) : "");
        if (sfd >= 0) {
                pid_t c = fork();
                if (c == 0) {
                        _exit(0);
                }
                usleep(150000);   /* let the child exit */
                if (probe_poll("signalfd-SIGCHLD", sfd, 3000) > 0) {
                        struct signalfd_siginfo si;
                        ssize_t n = read(sfd, &si, sizeof(si));
                        printf("  read=%zd signo=%u pid=%u code=%d status=%d\n",
                            n, si.ssi_signo, si.ssi_pid, si.ssi_code,
                            si.ssi_status);
                }
                waitpid(c, NULL, 0);
        }

        /* 2. pidfd_open(child) + poll(pidfd). */
        pid_t c2 = fork();
        if (c2 == 0) {
                usleep(200000);
                _exit(0);
        }
        int pfd = (int)syscall(SYS_pidfd_open, c2, 0);
        printf("pidfd_open(%d)=%d%s\n", (int)c2, pfd,
            pfd < 0 ? strerror(errno) : "");
        if (pfd >= 0) {
                usleep(400000);   /* child has exited by now */
                probe_poll("pidfd", pfd, 2000);
        }
        waitpid(c2, NULL, 0);
        printf("done\n");
        return 0;
}
