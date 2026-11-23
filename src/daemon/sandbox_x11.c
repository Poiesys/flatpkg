#include <sys/param.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "flatpkg.h"
#define FPKG_MODULE "sandbox"
#include "fpkglog.h"
#include "sandbox_internal.h"

// Read a whole Xauthority file; a 1 MiB cap keeps a hostile file bounded.
// The path is the caller's home, which they control: a FIFO here would block
// root's launch, and a symlink would read somewhere else.  No O_NOFOLLOW, no
// regular file, no read.
static bool xauth_read_file(const char* path, unsigned char** out,
			    size_t* outlen) {
	int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	struct stat st;

	if (fd < 0) return false;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		return false;
	}

	size_t cap = 65536, len = 0;
	unsigned char* buf = malloc(cap);
	if (buf == NULL) {
		close(fd);
		return false;
	}
	for (;;) {
		if (len == cap) {
			unsigned char* nb = realloc(buf, cap * 2);
			if (nb == NULL) {
				free(buf);
				close(fd);
				return false;
			}
			buf = nb;
			cap *= 2;
		}
		ssize_t n = read(fd, buf + len, cap - len);

		// A signal during the read used to fail the whole staging, and an
		// app without its Xauthority is the "Authorization required"
		// failure x11-fallback.sh exists to catch.
		if (n < 0 && errno == EINTR) continue;
		if (n < 0) {
			free(buf);
			close(fd);
			return false;
		}
		if (n == 0) break;
		len += (size_t)n;
		if (len > 1024 * 1024) {
			fpkg_warnf("xauth: %s is larger than 1 MiB", path);
			free(buf);
			close(fd);
			return false;
		}
	}
	close(fd);
	if (len == 0) {
		free(buf);
		return false;
	}
	*out = buf;
	*outlen = len;
	return true;
}

bool sandbox_xauth_write_filtered(const char* root, const char* app_id,
				 const char* home, uid_t uid, gid_t gid) {
	assert(root != NULL);
	assert(app_id != NULL);
	if (home == NULL || *home == '\0') {
		fpkg_warnf("xauth: no home directory for the caller");
		return false;
	}
	char src[PATH_MAX];
	snprintf(src, sizeof(src), "%s/.Xauthority", home);

	unsigned char* buf = NULL;
	size_t len = 0;
	if (!xauth_read_file(src, &buf, &len)) {
		fpkg_warnf("xauth: cannot read %s: %s", src, strerror(errno));
		return false;
	}

	char hostname[256] = {0};
	if (gethostname(hostname, sizeof(hostname) - 1) != 0)
		hostname[0] = '\0';
	const char* accept[6];
	size_t naccept = 0;
	if (hostname[0] != '\0') accept[naccept++] = hostname;
	if (app_id != NULL && *app_id != '\0') accept[naccept++] = app_id;
	static const char* const local[] = {
		"localhost", "localhost.localdomain", "127.0.0.1", "unix"};
	for (size_t i = 0; i < sizeof(local) / sizeof(local[0]); i++)
		accept[naccept++] = local[i];

	struct xauth_rec {
		size_t start, len;
		size_t addr_pos, addr_len;
		bool keep, rewrite;
	};
	struct xauth_rec* recs = NULL;
	size_t nrecs = 0, cap_recs = 16;
	recs = calloc(cap_recs, sizeof(*recs));
	if (recs == NULL) {
		free(buf);
		free(recs);
		return false;
	}
	size_t off = 0;
	bool malformed = false;
	while (off < len) {
		if (nrecs == cap_recs) {
			struct xauth_rec* nb =
				realloc(recs, cap_recs * 2 * sizeof(*recs));
			if (nb == NULL) {
				malformed = true;
				break;
			}
			recs = nb;
			cap_recs *= 2;
		}
		size_t start = off;
		if (off + 2 > len) {
			malformed = true;
			break;
		}
		off += 2;
		char addr[256] = {0};
		size_t addr_pos = 0, addr_len = 0;
		int field;
		for (field = 0; field < 4; field++) {
			if (off + 2 > len) {
				malformed = true;
				break;
			}
			size_t flen = ((size_t)buf[off] << 8) | buf[off + 1];
			off += 2;
			if (flen > len || off + flen > len) {
				malformed = true;
				break;
			}
			if (field == 0) {
				addr_pos = off;
				addr_len = flen;
				size_t c = flen < sizeof(addr) - 1
						   ? flen
						   : sizeof(addr) - 1;
				memcpy(addr, buf + off, c);
				addr[c] = '\0';
			}
			off += flen;
		}
		if (malformed) break;
		bool keep = (addr[0] == '\0');
		for (size_t i = 0; !keep && i < naccept; i++)
			if (strcmp(addr, accept[i]) == 0) keep = true;
		recs[nrecs].start = start;
		recs[nrecs].len = off - start;
		recs[nrecs].addr_pos = addr_pos;
		recs[nrecs].addr_len = addr_len;
		recs[nrecs].keep = keep;
		recs[nrecs].rewrite = keep && strcmp(addr, app_id) != 0;
		nrecs++;
	}
	if (malformed) {
		fpkg_warnf("xauth: %s is malformed", src);
		free(buf);
		free(recs);
		return false;
	}

	char rundir[PATH_MAX], dst[PATH_MAX], tmp[PATH_MAX];
	snprintf(rundir, sizeof(rundir), "%s/run/flatpak", root);
	if (mkdir_p(rundir, 0755) != 0) {
		fpkg_warnf("xauth: cannot create %s: %s", rundir,
			   strerror(errno));
		free(buf);
		free(recs);
		return false;
	}
	snprintf(dst, sizeof(dst), "%s/Xauthority", rundir);
	snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
	int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (out < 0) {
		fpkg_warnf("xauth: cannot open %s: %s", tmp, strerror(errno));
		free(buf);
		free(recs);
		return false;
	}
	bool wrote = false;
	for (size_t i = 0; i < nrecs; i++) {
		if (!recs[i].keep) continue;
		if (!recs[i].rewrite) {
			if (!fpkg_write_all(out, buf + recs[i].start,
				       recs[i].len)) {
				wrote = false;
				break;
			}
		} else {
			// The jail's hostname is the app id, so the record's
			// hostname address has to become the app id or Xlib
			// will not match it.
			size_t alen = strlen(app_id);
			unsigned char hdr[2] = {(unsigned char)(alen >> 8),
						(unsigned char)(alen & 0xff)};
			size_t rest = recs[i].addr_pos + recs[i].addr_len;
			size_t rest_len = recs[i].start + recs[i].len - rest;

			if (alen > 0xffff ||
			    !fpkg_write_all(out, buf + recs[i].start, 2) ||
			    !fpkg_write_all(out, hdr, 2) ||
			    !fpkg_write_all(out, app_id, alen) ||
			    !fpkg_write_all(out, buf + rest, rest_len)) {
				wrote = false;
				break;
			}
		}
		wrote = true;
	}
	free(buf);
	free(recs);
	// The owner is set while the file is still open, before rename(2) gives
	// it its final name.
	if (wrote && fchown(out, uid, gid) != 0) wrote = false;
	close(out);
	if (!wrote) {
		fpkg_warnf("xauth: no usable record in %s", src);
		(void)unlink(tmp);
		return false;
	}
	if (rename(tmp, dst) != 0) {
		fpkg_warnf("xauth: cannot rename %s: %s", tmp,
			   strerror(errno));
		(void)unlink(tmp);
		return false;
	}
	return true;
}
