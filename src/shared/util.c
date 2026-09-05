#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

#include "flatpkg.h"
#define FPKG_MODULE "util"
#include "fpkglog.h"

char* xstrdup(const char* s) {
	char* r = strdup(s ? s : "");
	if (!r) {
		fpkg_errorf("out of memory");
		exit(1);
	}
	return r;
}

fpkg_error_t mkdir_p(const char* path, mode_t mode) {
	assert(path != NULL);
	char tmp[PATH_MAX];

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char* p = tmp + 1; *p; p++) {
		if (*p != '/') continue;
		*p = '\0';
		if (mkdir(tmp, mode) != 0 && errno != EEXIST)
			return fpkg_error_from_errno(errno);
		*p = '/';
	}
	if (mkdir(tmp, mode) != 0 && errno != EEXIST)
		return fpkg_error_from_errno(errno);
	return FPKG_OK;
}
char* xasprintf(const char* fmt, ...) {
	va_list ap;
	char* r = NULL;

	va_start(ap, fmt);
	if (vasprintf(&r, fmt, ap) < 0) r = NULL;
	va_end(ap);
	if (r == NULL) {
		fpkg_errorf("out of memory");
		exit(1);
	}
	return r;
}

fpkg_error_t copy_file(const char* from, const char* to, mode_t mode) {
	int in = open(from, O_RDONLY);
	if (in < 0) return fpkg_error_from_errno(errno);

	int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (out < 0) {
		int e = errno;
		close(in);
		errno = e;
		return fpkg_error_from_errno(e);
	}

	char buf[16384];
	int e = 0;
	for (;;) {
		ssize_t n = read(in, buf, sizeof(buf));
		if (n == 0) break;
		if (n < 0) {
			if (errno == EINTR) continue;
			e = errno;
			break;
		}
		for (ssize_t off = 0; off < n;) {
			ssize_t w = write(out, buf + off, (size_t)(n - off));
			if (w < 0) {
				if (errno == EINTR) continue;
				e = errno;
				break;
			}
			if (w == 0) {
				e = EIO;
				break;
			}
			off += w;
		}
		if (e != 0) break;
	}
	if (close(out) != 0 && e == 0) e = errno;
	close(in);
	errno = e;
	return e == 0 ? FPKG_OK : fpkg_error_from_errno(e);
}
