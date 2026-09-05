// memcheck-metadata.c - the metadata parser on a keyfile that repeats keys,
// for tests/memcheck.sh under valgrind: a repeated key must not leave the first
// value allocated and unreachable.  The logging stubs below log nothing.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "flatpkg.h"

const char* get_string(const char* s) {
	return s;
}

void fp_vlogf(const char* module, const char* prefix, int level,
	      const char* fmt, va_list ap) {
	(void)module;
	(void)prefix;
	(void)level;
	(void)fmt;
	(void)ap;
}

int main(void) {
	char path[] = "/tmp/.fpkg-memcheck-meta.XXXXXX";
	int fd = mkstemp(path);
	if (fd < 0) return 2;
	FILE* f = fdopen(fd, "w");
	if (f == NULL) return 2;
	// Every string-valued key of [Application] twice, and an extension
	// group whose scalar keys repeat.
	fputs("[Application]\n"
	      "name=org.test.A\nname=org.test.B\n"
	      "command=one\ncommand=two\n"
	      "runtime=a/b/c\nruntime=d/e/f\n"
	      "sdk=a/b/c\nsdk=d/e/f\n"
	      "[Extension org.test.A.Ext]\n"
	      "directory=x\ndirectory=y\n"
	      "version=1\nversion=2\n"
	      "add-ld-path=p\nadd-ld-path=q\n",
	      f);
	fclose(f);

	fp_metadata_t m;
	int rc = fp_metadata_parse(path, &m);
	unlink(path);
	if (rc != 0) return 3;
	// The last value of a repeated key wins.
	if (m.app_id == NULL || strcmp(m.app_id, "org.test.B") != 0) return 4;
	if (m.command == NULL || strcmp(m.command, "two") != 0) return 5;
	fp_metadata_free(&m);
	return 0;
}
