#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

struct signalfd_siginfo {
	uint32_t ssi_signo;
	int32_t ssi_errno;
	int32_t ssi_code;
	uint32_t ssi_pid;
	uint32_t ssi_uid;
	int32_t ssi_fd;
	uint32_t ssi_tid;
	uint32_t ssi_band;
	uint32_t ssi_overrun;
	uint32_t ssi_trapno;
	int32_t ssi_status;
	int32_t ssi_int;
	uint64_t ssi_ptr;
	uint64_t ssi_utime;
	uint64_t ssi_stime;
	uint64_t ssi_addr;
	uint16_t ssi_addr_lsb;
	uint16_t __pad2;
	int32_t ssi_syscall;
	uint64_t ssi_call_addr;
	uint32_t ssi_arch;
	uint8_t __pad[28];
};

static int shim_rfd = -1, shim_wfd = -1;
static sigset_t shim_mask;
static pthread_mutex_t shim_lock = PTHREAD_MUTEX_INITIALIZER;

static void* shim_sigloop(void* arg) {
	pthread_sigmask(SIG_BLOCK, &shim_mask, NULL);
	int my_wfd = shim_wfd;
	for (;;) {
		siginfo_t si;
		int sig;
		sig = sigwaitinfo(&shim_mask, &si);
		if (sig < 0) {
			if (errno == EINTR) continue;
			break;
		}
		struct signalfd_siginfo s;
		memset(&s, 0, sizeof(s));
		s.ssi_signo = (uint32_t)sig;
		s.ssi_errno = si.si_errno;
		s.ssi_code = si.si_code;
		s.ssi_pid = (uint32_t)si.si_pid;
		s.ssi_uid = (uint32_t)si.si_uid;
		s.ssi_status = si.si_status;
		s.ssi_utime = (uint64_t)si.si_utime;
		s.ssi_stime = (uint64_t)si.si_stime;
		ssize_t n = write(my_wfd, &s, sizeof(s));
		if (n < 0 && (errno == EPIPE || errno == EBADF)) {
			pthread_mutex_lock(&shim_lock);
			if (shim_wfd == my_wfd) {
				shim_rfd = -1;
				shim_wfd = -1;
			}
			pthread_mutex_unlock(&shim_lock);
			break;
		}
	}
	return NULL;
}

int signalfd4(int fd, const sigset_t* mask, int flags) {
	pthread_mutex_lock(&shim_lock);
	if (shim_rfd < 0 || fcntl(shim_rfd, F_GETFD) < 0) {
		if (shim_rfd >= 0) {
			(void)close(shim_rfd);
			shim_rfd = -1;
			shim_wfd = -1;
		}
		int p[2];
		if (pipe(p) != 0) {
			pthread_mutex_unlock(&shim_lock);
			return -1;
		}
		shim_rfd = p[0];
		shim_wfd = p[1];
		int fl = fcntl(shim_wfd, F_GETFL);
		if (fl >= 0) (void)fcntl(shim_wfd, F_SETFL, fl | O_NONBLOCK);
		shim_mask = *mask;
		pthread_sigmask(SIG_BLOCK, mask, NULL);
		pthread_t th;
		if (pthread_create(&th, NULL, shim_sigloop, NULL) != 0) {
			pthread_mutex_unlock(&shim_lock);
			return -1;
		}
		pthread_detach(th);
	}
	int r = shim_rfd;
	pthread_mutex_unlock(&shim_lock);
	if (fd >= 0) close(fd);
	return r;
}

// The legacy 3-arg signalfd()
__asm__(".symver signalfd, signalfd@@GLIBC_2.7");
int signalfd(int fd, const sigset_t* mask, int flags) {
	return signalfd4(fd, mask, flags);
}

#ifndef SO_PASSPIDFD
#define SO_PASSPIDFD 76
#endif
#ifndef SO_PEERPIDFD
#define SO_PEERPIDFD 77
#endif

__asm__(".symver getsockopt, getsockopt@@GLIBC_2.2.5");
int getsockopt(int s, int level, int optname, void* optval, socklen_t* optlen) {
	static int (*real)(int, int, int, void*, socklen_t*);
	int r;

	if (!real) {
		real = (int (*)(int, int, int, void*, socklen_t*))dlsym(
			RTLD_NEXT, "getsockopt");
		if (!real) {
			errno = ENOSYS;
			return -1;
		}
	}
	r = real(s, level, optname, optval, optlen);
	if (r < 0 && level == SOL_SOCKET && errno == EINVAL &&
	    (optname == SO_PEERPIDFD || optname == SO_PASSPIDFD))
		errno = ENOPROTOOPT;
	return r;
}
