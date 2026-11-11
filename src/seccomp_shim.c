// flatpak-parity syscall blocklist, as a glibc interposer: the Linuxulator has
// no seccomp(2). LD_PRELOADed from sandbox_exec() alongside signalfd_shim.so.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <sched.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/personality.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

// The knobs decide which filters are installed, so they are read once, before
// the app's first call into the shim: a process that can setenv() them later
// would be rewriting the gate it is subject to.
static bool g_allow_can, g_allow_bt, g_devel;

__attribute__((constructor)) static void read_knobs(void) {
	const char* v;

	v = getenv("FLATPKG_SECCOMP_ALLOW_CAN");
	g_allow_can = v != NULL && (*v == '1' || *v == 't');
	v = getenv("FLATPKG_SECCOMP_ALLOW_BT");
	g_allow_bt = v != NULL && (*v == '1' || *v == 't');
	v = getenv("FLATPKG_SECCOMP_DEVEL");
	g_devel = v != NULL && (*v == '1' || *v == 't');
}

#ifndef SYS_mount
#define SYS_mount 165
#endif
#ifndef SYS_umount
// x86_64 umount is 22 and umount2 is 166; aliasing them to one number would
// leave the other syscall unfiltered.
#define SYS_umount 22
#endif
#ifndef SYS_umount2
#define SYS_umount2 166
#endif
#ifndef SYS_pivot_root
#define SYS_pivot_root 155
#endif
#ifndef SYS_chroot
#define SYS_chroot 161
#endif
#ifndef SYS_unshare
#define SYS_unshare 272
#endif
#ifndef SYS_setns
#define SYS_setns 308
#endif
#ifndef SYS_clone3
#define SYS_clone3 435
#endif
#ifndef SYS_open_tree
#define SYS_open_tree 428
#endif
#ifndef SYS_move_mount
#define SYS_move_mount 429
#endif
#ifndef SYS_fsopen
#define SYS_fsopen 430
#endif
#ifndef SYS_fsconfig
#define SYS_fsconfig 431
#endif
#ifndef SYS_fsmount
#define SYS_fsmount 432
#endif
#ifndef SYS_fspick
#define SYS_fspick 433
#endif
#ifndef SYS_mount_setattr
#define SYS_mount_setattr 442
#endif
#ifndef SYS_acct
#define SYS_acct 163
#endif
#ifndef SYS_quotactl
#define SYS_quotactl 179
#endif
#ifndef SYS_syslog
#define SYS_syslog 103
#endif
#ifndef SYS_uselib
#define SYS_uselib 134
#endif
#ifndef SYS_add_key
#define SYS_add_key 248
#endif
#ifndef SYS_request_key
#define SYS_request_key 249
#endif
#ifndef SYS_keyctl
#define SYS_keyctl 250
#endif
#ifndef SYS_move_pages
#define SYS_move_pages 279
#endif
#ifndef SYS_mbind
#define SYS_mbind 237
#endif
#ifndef SYS_get_mempolicy
#define SYS_get_mempolicy 239
#endif
#ifndef SYS_set_mempolicy
#define SYS_set_mempolicy 238
#endif
#ifndef SYS_migrate_pages
#define SYS_migrate_pages 256
#endif
#ifndef SYS_perf_event_open
#define SYS_perf_event_open 298
#endif
#ifndef SYS_personality
#define SYS_personality 135
#endif
#ifndef SYS_ptrace
#define SYS_ptrace 101
#endif
#ifndef SYS_socket
#define SYS_socket 41
#endif
#ifndef SYS_clone
#define SYS_clone 56
#endif
#ifndef SYS_ioctl
#define SYS_ioctl 16
#endif

#ifndef TIOCSTI
#define TIOCSTI 0x5412
#endif
#ifndef TIOCLINUX
#define TIOCLINUX 0x541C
#endif

#ifndef PER_LINUX
#define PER_LINUX 0x0000
#endif

#ifndef AF_UNSPEC
#define AF_UNSPEC 0
#endif
#ifndef AF_LOCAL
#define AF_LOCAL 1
#endif
#ifndef AF_INET
#define AF_INET 2
#endif
#ifndef AF_INET6
#define AF_INET6 10
#endif
#ifndef AF_NETLINK
#define AF_NETLINK 16
#endif
#ifndef AF_CAN
#define AF_CAN 29
#endif
#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#ifndef SOCK_RAW
#define SOCK_RAW 3
#endif

struct seccomp_rule {
	long nr;
	int err;
};

static const struct seccomp_rule blocklist_eperm[] = {
	{SYS_mount, EPERM},	    {SYS_umount, EPERM},
	{SYS_umount2, EPERM},	    {SYS_pivot_root, EPERM},
	{SYS_chroot, EPERM},	    {SYS_unshare, EPERM},
	{SYS_setns, EPERM},	    {SYS_acct, EPERM},
	{SYS_quotactl, EPERM},	    {SYS_syslog, EPERM},
	{SYS_uselib, EPERM},	    {SYS_add_key, EPERM},
	{SYS_request_key, EPERM},   {SYS_keyctl, EPERM},
	{SYS_move_pages, EPERM},    {SYS_mbind, EPERM},
	{SYS_get_mempolicy, EPERM}, {SYS_set_mempolicy, EPERM},
	{SYS_migrate_pages, EPERM}, {SYS_perf_event_open, EPERM},
};
static const size_t n_blocklist_eperm =
	sizeof(blocklist_eperm) / sizeof(blocklist_eperm[0]);

static const struct seccomp_rule blocklist_enosys[] = {
	{SYS_clone3, ENOSYS},	  {SYS_open_tree, ENOSYS},
	{SYS_move_mount, ENOSYS}, {SYS_fsopen, ENOSYS},
	{SYS_fsconfig, ENOSYS},	  {SYS_fsmount, ENOSYS},
	{SYS_fspick, ENOSYS},	  {SYS_mount_setattr, ENOSYS},
};
static const size_t n_blocklist_enosys =
	sizeof(blocklist_enosys) / sizeof(blocklist_enosys[0]);

static bool in_blocklist(long nr, const struct seccomp_rule* list, size_t n,
			 int* err_out) {
	for (size_t i = 0; i < n; i++) {
		if (list[i].nr == nr) {
			if (err_out) *err_out = list[i].err;
			return true;
		}
	}
	return false;
}

static int socket_allowed(int domain, int type) {
	bool allowed = (domain == AF_UNSPEC || domain == AF_LOCAL ||
			domain == AF_INET || domain == AF_INET6 ||
			domain == AF_NETLINK);
	if (!allowed) {
		if (g_allow_can && domain == AF_CAN) allowed = true;
		if (g_allow_bt && domain == AF_BLUETOOTH) allowed = true;
	}
	if (!allowed) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	if ((type & 0xf) == SOCK_RAW && domain != AF_CAN &&
	    domain != AF_BLUETOOTH) {
		errno = EPERM;
		return -1;
	}
	return 0;
}

static long (*real_syscall)(long, ...);
static void init_real_syscall(void);

int socket(int domain, int type, int protocol) {
	init_real_syscall();
	if (!real_syscall) {
		errno = ENOSYS;
		return -1;
	}
	if (socket_allowed(domain, type) != 0) return -1;
	return (int)real_syscall(SYS_socket, domain, type, protocol);
}

static long (*real_syscall)(long, ...) = NULL;

static void init_real_syscall(void) {
	if (!real_syscall) real_syscall = dlsym(RTLD_NEXT, "syscall");
}

long syscall(long nr, ...) {
	init_real_syscall();
	if (!real_syscall) {
		errno = ENOSYS;
		return -1;
	}

	// On a match return -1 with errno set, without the real syscall.
	int err;
	if (in_blocklist(nr, blocklist_eperm, n_blocklist_eperm, &err) ||
	    in_blocklist(nr, blocklist_enosys, n_blocklist_enosys, &err)) {
		errno = err;
		return -1;
	}

	if (nr == SYS_socket) {
		va_list ap;
		va_start(ap, nr);
		int domain = va_arg(ap, int);
		int type = va_arg(ap, int);
		int protocol = va_arg(ap, int);
		va_end(ap);
		if (socket_allowed(domain, type) != 0) return -1;
		return real_syscall(nr, domain, type, protocol);
	}

	if (nr == SYS_clone) {
		va_list ap;
		va_start(ap, nr);
		unsigned long flags = va_arg(ap, unsigned long);
		void* stack = va_arg(ap, void*);
		int* ptid = va_arg(ap, int*);
		int* ctid = va_arg(ap, int*);
		unsigned long tls = va_arg(ap, unsigned long);
		va_end(ap);
		{
			const int ns_flags = CLONE_NEWNS | CLONE_NEWCGROUP |
					     CLONE_NEWUTS | CLONE_NEWIPC |
					     CLONE_NEWUSER | CLONE_NEWPID |
					     CLONE_NEWNET;
			if (flags & ns_flags) {
				errno = EPERM;
				return -1;
			}
		}
		return real_syscall(nr, flags, stack, ptid, ctid, tls);
	}

	if (nr == SYS_ioctl) {
		va_list ap;
		va_start(ap, nr);
		int fd = va_arg(ap, int);
		unsigned long req = va_arg(ap, unsigned long);
		void* argp = va_arg(ap, void*);
		va_end(ap);
		if (req == TIOCSTI || req == TIOCLINUX) {
			errno = EPERM;
			return -1;
		}
		return real_syscall(nr, fd, req, argp);
	}

	if (nr == SYS_personality) {
		va_list ap;
		va_start(ap, nr);
		unsigned long persona = va_arg(ap, unsigned long);
		va_end(ap);
		if (g_devel) {
			return real_syscall(nr, persona);
		}
		if (persona != PER_LINUX) {
			errno = EPERM;
			return -1;
		}
		return real_syscall(nr, persona);
	}

	if (nr == SYS_ptrace) {
		if (!g_devel) {
			errno = EPERM;
			return -1;
		}
	}

	va_list ap;
	va_start(ap, nr);
	long a1 = va_arg(ap, long);
	long a2 = va_arg(ap, long);
	long a3 = va_arg(ap, long);
	long a4 = va_arg(ap, long);
	long a5 = va_arg(ap, long);
	long a6 = va_arg(ap, long);
	va_end(ap);
	return real_syscall(nr, a1, a2, a3, a4, a5, a6);
}

int mount(const char* source, const char* target, const char* fstype,
	  unsigned long flags, const void* data) {
	errno = EPERM;
	return -1;
}

int umount(const char* target) {
	errno = EPERM;
	return -1;
}

int umount2(const char* target, int flags) {
	errno = EPERM;
	return -1;
}

int pivot_root(const char* new_root, const char* put_old) {
	errno = EPERM;
	return -1;
}

int chroot(const char* path) {
	errno = EPERM;
	return -1;
}

int unshare(int flags) {
	errno = EPERM;
	return -1;
}

int setns(int fd, int nstype) {
	errno = EPERM;
	return -1;
}

static int (*real_clone)(int (*)(void*), void*, int, void*, ...) = NULL;

int clone(int (*fn)(void*), void* stack, int flags, void* arg, ...) {
	const int ns_flags = CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS |
			     CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID |
			     CLONE_NEWNET;

	if (flags & ns_flags) {
		errno = EPERM;
		return -1;
	}
	if (!real_clone) real_clone = dlsym(RTLD_NEXT, "clone");
	if (!real_clone || real_clone == clone) {
		errno = ENOSYS;
		return -1;
	}
	va_list ap;
	va_start(ap, arg);
	int r = real_clone(fn, stack, flags, arg, ap);
	va_end(ap);
	return r;
}

struct clone_args;

int clone3(struct clone_args* args, size_t size) {
	errno = ENOSYS;
	return -1;
}

int add_key(const char* type, const char* desc, const void* payload,
	    size_t plen, int ringid) {
	errno = EPERM;
	return -1;
}

long keyctl(int cmd, ...) {
	errno = EPERM;
	return -1;
}

int request_key(const char* type, const char* desc, const char* callout,
		int ringid) {
	errno = EPERM;
	return -1;
}

int ptrace(int req, ...) {
	if (g_devel) {
		init_real_syscall();
		if (real_syscall) {
			va_list ap;
			va_start(ap, req);
			pid_t pid = va_arg(ap, pid_t);
			void* addr = va_arg(ap, void*);
			void* data = va_arg(ap, void*);
			va_end(ap);
			return real_syscall(SYS_ptrace, req, pid, addr, data);
		}
	}
	errno = EPERM;
	return -1;
}
