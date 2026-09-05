#ifndef FPKGLOG_H
#define FPKGLOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "fplog.h"
#include "cli.h"
#include "translations.h"

#ifndef FPKG_MODULE
#define FPKG_MODULE "flatpkg"
#endif

// True when the user can answer prompts: stdin is a real terminal, or the
// daemon worker is serving a client that has one (FLATPKG_INTERACTIVE=1).
static inline bool fpkg_interactive(void) {
	if (isatty(STDIN_FILENO)) return true;
	const char* v = cli_getenv("FLATPKG_INTERACTIVE");
	return v && *v == '1';
}

static inline void fpkg_logf(int level, const char* fmt, va_list ap) {
	fp_vlogf(FPKG_MODULE, "", level, get_string(fmt), ap);
}

static inline void fpkg_errorf(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fpkg_logf(FP_LEVEL_ERROR, fmt, ap);
	va_end(ap);
}

static inline void fpkg_warnf(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fpkg_logf(FP_LEVEL_WARN, fmt, ap);
	va_end(ap);
}

static inline void fpkg_infof(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fpkg_logf(FP_LEVEL_INFO, fmt, ap);
	va_end(ap);
}

static inline void fpkg_okf(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fpkg_logf(FP_LEVEL_OKAY, fmt, ap);
	va_end(ap);
}

static inline void fpkg_debugf(const char* fmt, ...) {
	if (!fp_get_verbose()) return;
	va_list ap;
	va_start(ap, fmt);
	fpkg_logf(FP_LEVEL_DEBUG, fmt, ap);
	va_end(ap);
}

#endif
