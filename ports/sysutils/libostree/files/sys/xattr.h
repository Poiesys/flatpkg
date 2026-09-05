/*
 * BSD compatibility shim providing a useful subset of the Linux
 * <sys/xattr.h> extended-attribute API on top of FreeBSD's extattr(2)
 * syscalls.
 *
 * It is placed on the compiler include path AS <sys/xattr.h> so that
 * ostree's Linux-centric xattr calls build unchanged on FreeBSD.
 *
 * Namespace mapping:
 *   Linux "user."     prefix -> EXTATTR_NAMESPACE_USER
 *   Linux "security." / "system." prefix -> EXTATTR_NAMESPACE_SYSTEM
 * The prefix is stripped before the extattr call.
 *
 * Semantics follow the Linux man pages closely enough for ostree:
 *   - getxattr(path, name, buf, 0) returns the value size (and may be used
 *     purely as a size query, matching extattr_get_file(fd,...,NULL,0)).
 *   - ENODATA is returned when the attribute does not exist (FreeBSD uses
 *     ENOATTR internally; we translate it).
 *   - setxattr honours XATTR_CREATE / XATTR_REPLACE flags.
 *   - listxattr returns the Linux NUL-separated name list (FreeBSD stores
 *     a 1-byte length prefix per entry, which we convert).
 */

#ifndef OSTREE_BSD_XATTR_H
#define OSTREE_BSD_XATTR_H

#include <sys/types.h>
#include <sys/extattr.h>
#include <errno.h>
#include <string.h>

#ifndef ENODATA
/* Linux value for "no such attribute"; FreeBSD does not define it. */
#define ENODATA 61
#endif
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif

#ifndef XATTR_CREATE
#define XATTR_CREATE 1
#endif
#ifndef XATTR_REPLACE
#define XATTR_REPLACE 2
#endif

/* Map a Linux xattr name to a FreeBSD extattr namespace. */
static inline int
_ostree_xattr_ns (const char *name)
{
  if (strncmp (name, "user.", 5) == 0)
    return EXTATTR_NAMESPACE_USER;
  if (strncmp (name, "security.", 9) == 0 ||
      strncmp (name, "system.", 7) == 0)
    return EXTATTR_NAMESPACE_SYSTEM;
  return EXTATTR_NAMESPACE_USER;
}

/* Strip the Linux namespace prefix. */
static inline const char *
_ostree_xattr_name (const char *name)
{
  if (strncmp (name, "user.", 5) == 0)
    return name + 5;
  if (strncmp (name, "security.", 9) == 0)
    return name + 9;
  if (strncmp (name, "system.", 7) == 0)
    return name + 7;
  return name;
}

/* ---- get ---- */

static inline ssize_t
getxattr (const char *path, const char *name, void *value, size_t size)
{
  ssize_t r = extattr_get_file (path, _ostree_xattr_ns (name),
                                _ostree_xattr_name (name), value, size);
  if (r < 0 && errno == ENOATTR)
    errno = ENODATA;
  return r;
}

static inline ssize_t
lgetxattr (const char *path, const char *name, void *value, size_t size)
{
  ssize_t r = extattr_get_link (path, _ostree_xattr_ns (name),
                                _ostree_xattr_name (name), value, size);
  if (r < 0 && errno == ENOATTR)
    errno = ENODATA;
  return r;
}

static inline ssize_t
fgetxattr (int fd, const char *name, void *value, size_t size)
{
  ssize_t r = extattr_get_fd (fd, _ostree_xattr_ns (name),
                              _ostree_xattr_name (name), value, size);
  if (r < 0 && errno == ENOATTR)
    errno = ENODATA;
  return r;
}

/* ---- set ---- */

static inline int
setxattr (const char *path, const char *name, const void *value,
          size_t size, int flags)
{
  int ns = _ostree_xattr_ns (name);
  const char *n = _ostree_xattr_name (name);
  if (flags & XATTR_CREATE)
    {
      if (extattr_get_file (path, ns, n, NULL, 0) >= 0)
        { errno = EEXIST; return -1; }
    }
  else if (flags & XATTR_REPLACE)
    {
      if (extattr_get_file (path, ns, n, NULL, 0) < 0 && errno == ENOATTR)
        { errno = ENODATA; return -1; }
    }
  ssize_t r = extattr_set_file (path, ns, n, value, size);
  return r < 0 ? -1 : 0;
}

static inline int
lsetxattr (const char *path, const char *name, const void *value,
           size_t size, int flags)
{
  int ns = _ostree_xattr_ns (name);
  const char *n = _ostree_xattr_name (name);
  if (flags & XATTR_CREATE)
    {
      if (extattr_get_link (path, ns, n, NULL, 0) >= 0)
        { errno = EEXIST; return -1; }
    }
  else if (flags & XATTR_REPLACE)
    {
      if (extattr_get_link (path, ns, n, NULL, 0) < 0 && errno == ENOATTR)
        { errno = ENODATA; return -1; }
    }
  ssize_t r = extattr_set_link (path, ns, n, value, size);
  return r < 0 ? -1 : 0;
}

static inline int
fsetxattr (int fd, const char *name, const void *value, size_t size, int flags)
{
  int ns = _ostree_xattr_ns (name);
  const char *n = _ostree_xattr_name (name);
  if (flags & XATTR_CREATE)
    {
      if (extattr_get_fd (fd, ns, n, NULL, 0) >= 0)
        { errno = EEXIST; return -1; }
    }
  else if (flags & XATTR_REPLACE)
    {
      if (extattr_get_fd (fd, ns, n, NULL, 0) < 0 && errno == ENOATTR)
        { errno = ENODATA; return -1; }
    }
  ssize_t r = extattr_set_fd (fd, ns, n, value, size);
  return r < 0 ? -1 : 0;
}

/* ---- remove ---- */

static inline int
removexattr (const char *path, const char *name)
{
  int r = extattr_delete_file (path, _ostree_xattr_ns (name),
                               _ostree_xattr_name (name));
  if (r < 0 && errno == ENOATTR)
    errno = ENODATA;
  return r < 0 ? -1 : 0;
}

static inline int
lremovexattr (const char *path, const char *name)
{
  int r = extattr_delete_link (path, _ostree_xattr_ns (name),
                               _ostree_xattr_name (name));
  if (r < 0 && errno == ENOATTR)
    errno = ENODATA;
  return r < 0 ? -1 : 0;
}

static inline int
fremovexattr (int fd, const char *name)
{
  int r = extattr_delete_fd (fd, _ostree_xattr_ns (name),
                             _ostree_xattr_name (name));
  if (r < 0 && errno == ENOATTR)
    errno = ENODATA;
  return r < 0 ? -1 : 0;
}

/* ---- list ---- */

/* Convert a FreeBSD extattr_list buffer (1-byte length prefix per entry)
 * into the Linux NUL-separated list, appending into `list` (size `size`).
 * Returns the total length that would be written. */
static inline ssize_t
_ostree_listxattr_conv (char *list, size_t size, const char *buf, ssize_t len)
{
  ssize_t total = 0;
  size_t off = 0;
  while (off < (size_t) len)
    {
      unsigned char l = (unsigned char) buf[off];
      off++;
      if (list && total + l + 1 <= size)
        {
          memcpy (list + total, buf + off, l);
          list[total + l] = '\0';
        }
      total += l + 1;
      off += l;
    }
  return total;
}

static inline ssize_t
listxattr (const char *path, char *list, size_t size)
{
  char buf[4096];
  int nses[2] = { EXTATTR_NAMESPACE_USER, EXTATTR_NAMESPACE_SYSTEM };
  ssize_t total = 0;
  for (int i = 0; i < 2; i++)
    {
      ssize_t r = extattr_list_file (path, nses[i], buf, sizeof (buf));
      if (r < 0)
        {
          if (errno == ENOATTR || errno == EPERM)
            continue;
          return -1;
        }
      ssize_t conv = _ostree_listxattr_conv (list ? list + total : NULL,
                                             list ? size - total : 0,
                                             buf, r);
      total += conv;
    }
  return total;
}

static inline ssize_t
llistxattr (const char *path, char *list, size_t size)
{
  char buf[4096];
  int nses[2] = { EXTATTR_NAMESPACE_USER, EXTATTR_NAMESPACE_SYSTEM };
  ssize_t total = 0;
  for (int i = 0; i < 2; i++)
    {
      ssize_t r = extattr_list_link (path, nses[i], buf, sizeof (buf));
      if (r < 0)
        {
          if (errno == ENOATTR || errno == EPERM)
            continue;
          return -1;
        }
      ssize_t conv = _ostree_listxattr_conv (list ? list + total : NULL,
                                             list ? size - total : 0,
                                             buf, r);
      total += conv;
    }
  return total;
}

static inline ssize_t
flistxattr (int fd, char *list, size_t size)
{
  char buf[4096];
  int nses[2] = { EXTATTR_NAMESPACE_USER, EXTATTR_NAMESPACE_SYSTEM };
  ssize_t total = 0;
  for (int i = 0; i < 2; i++)
    {
      ssize_t r = extattr_list_fd (fd, nses[i], buf, sizeof (buf));
      if (r < 0)
        {
          if (errno == ENOATTR || errno == EPERM)
            continue;
          return -1;
        }
      ssize_t conv = _ostree_listxattr_conv (list ? list + total : NULL,
                                             list ? size - total : 0,
                                             buf, r);
      total += conv;
    }
  return total;
}

#endif /* OSTREE_BSD_XATTR_H */
