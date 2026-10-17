#include "flatpkg.h"
#define FPKG_MODULE "daemon"
#include "fpkglog.h"
#include "evui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static evui_sink_t g_sink;
static bool g_have_sink;
static char g_prefix[64];

void evui_set_sink(const evui_sink_t* sink) {
	if (sink == NULL) {
		memset(&g_sink, 0, sizeof(g_sink));
		g_have_sink = false;
		return;
	}
	g_sink = *sink;
	g_have_sink = true;
}

bool fp_record_active(void) { return g_have_sink; }

bool fp_record_progress(const char* stage, uint64_t current, uint64_t total) {
	if (!g_have_sink || g_sink.progress == NULL) return false;
	g_sink.progress(g_sink.ctx, stage, current, total);
	return true;
}

bool fp_record_line(int level, const char* text) {
	if (!g_have_sink || g_sink.log == NULL) return false;
	g_sink.log(g_sink.ctx, level, "daemon", text);
	return true;
}

bool fp_record_table(const char* const* headers, size_t ncols,
		     const char* const* const* rows, size_t nrows) {
	if (!g_have_sink || g_sink.table == NULL) return false;
	return g_sink.table(g_sink.ctx, headers, ncols, rows, nrows);
}

void fp_vlogf(const char* scope, const char* desc, int level, const char* fmt,
	      va_list ap) {
	char msg[FP_MAX_MSG];
	char line[FP_MAX_MSG + 128];

	vsnprintf(msg, sizeof(msg), fmt, ap);
	if (g_have_sink && g_sink.log != NULL) {
		g_sink.log(g_sink.ctx, level, scope ? scope : "daemon", msg);
		return;
	}
	if (g_prefix[0] != '\0' && scope != NULL)
		snprintf(line, sizeof(line), "%s: %s: %s", g_prefix, scope, msg);
	else if (scope != NULL)
		snprintf(line, sizeof(line), "%s: %s", scope, msg);
	else
		snprintf(line, sizeof(line), "%s", msg);
	dprintf(STDERR_FILENO, "%s\n", line);
}

static void emit_line(const char* scope, const char* desc, int level,
		      const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fp_vlogf(scope, desc, level, fmt, ap);
	va_end(ap);
}

fp_logger_t fp_logger_init(const char* scope, const char* desc) {
	fp_logger_t l;
	memset(&l, 0, sizeof(l));
	if (scope != NULL) snprintf(l.scope, sizeof(l.scope), "%s", scope);
	if (desc != NULL) snprintf(l.desc, sizeof(l.desc), "%s", desc);
	return l;
}

#define LOGGER_LEVEL(name, level)                                              \
	void fp_logger_##name##f(const fp_logger_t* l, const char* fmt, ...) { \
		va_list ap;                                                    \
		va_start(ap, fmt);                                             \
		fp_vlogf(l->scope, l->desc, level, fmt, ap);                   \
		va_end(ap);                                                    \
	}

LOGGER_LEVEL(info, FP_LEVEL_INFO)
LOGGER_LEVEL(ok, FP_LEVEL_OKAY)
LOGGER_LEVEL(warn, FP_LEVEL_WARN)
LOGGER_LEVEL(error, FP_LEVEL_ERROR)
LOGGER_LEVEL(debug, FP_LEVEL_DEBUG)
#undef LOGGER_LEVEL

void fp_logger_beginf(const fp_logger_t* l, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fp_vlogf(l->scope, l->desc, FP_LEVEL_INFO, fmt, ap);
	va_end(ap);
}

void fp_logger_endf(const fp_logger_t* l, int success, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fp_vlogf(l->scope, l->desc, success ? FP_LEVEL_OKAY : FP_LEVEL_ERROR,
		 fmt, ap);
	va_end(ap);
}

void fp_logger_wendf(const fp_logger_t* l, int success, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fp_vlogf(l->scope, l->desc, success ? FP_LEVEL_OKAY : FP_LEVEL_WARN,
		 fmt, ap);
	va_end(ap);
}

void fp_set_prefix(const char* prefix) {
	snprintf(g_prefix, sizeof(g_prefix), "%s", prefix ? prefix : "");
}

bool fp_get_verbose(void) { return false; }
bool fp_is_tty(void) { return false; }
bool fp_fd_is_tty(int fd) {
	return false;
}
size_t fp_term_cols(void) { return 80; }
void fp_set_terminal_size(int rows, int cols) {
}

void fp_out(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	fp_vlogf("daemon", "", FP_LEVEL_INFO, fmt, ap);
	va_end(ap);
}

void fp_outl(const char* fmt, ...) {
	char msg[FP_MAX_MSG];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (!fp_record_line(FP_LEVEL_INFO, msg))
		dprintf(STDERR_FILENO, "%s\n", msg);
}

void fp_out_raw(const char* s, size_t n) {
	if (s != NULL && n > 0) (void)write(STDOUT_FILENO, s, n);
}

void fp_err_raw(const char* s, size_t n) {
	if (s != NULL && n > 0) (void)write(STDERR_FILENO, s, n);
}

bool fp_bar_can_pin(void) { return false; }
bool fp_bar_pinned(const fp_bar_t* b) {
	return false;
}
void fp_bar_dismiss(fp_bar_t* b) { (void)b; }

fp_bar_t fp_bar_init(const char* scope, const char* desc, const char* label,
		     uint64_t total) {
	fp_bar_t b;
	memset(&b, 0, sizeof(b));
	if (scope != NULL) snprintf(b.scope, sizeof(b.scope), "%s", scope);
	if (desc != NULL) snprintf(b.desc, sizeof(b.desc), "%s", desc);
	if (label != NULL) snprintf(b.label, sizeof(b.label), "%s", label);
	b.total = total;
	return b;
}

void fp_bar_set_label(fp_bar_t* b, const char* label) {
	if (b != NULL && label != NULL)
		snprintf(b->label, sizeof(b->label), "%s", label);
}

void fp_bar_set_throttle(fp_bar_t* b, uint32_t ms) {
	if (b != NULL) b->throttle_ms = ms;
}

void fp_bar_update(fp_bar_t* b, uint64_t current) {
	if (b == NULL) return;
	b->current = current;
	if (!fp_record_progress(b->label, current, b->total)) return;
}

void fp_bar_finishf(fp_bar_t* b, int success, const char* fmt, ...) {
	char msg[FP_MAX_MSG];
	va_list ap;

	if (b == NULL) return;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	b->finished = 1;
	emit_line(b->scope[0] ? b->scope : "daemon", b->desc,
		  success ? FP_LEVEL_OKAY : FP_LEVEL_ERROR, "%s", msg);
}

fp_spin_t fp_spin_init(const char* scope, const char* desc, const char* label) {
	fp_spin_t s;
	memset(&s, 0, sizeof(s));
	if (scope != NULL) snprintf(s.scope, sizeof(s.scope), "%s", scope);
	if (desc != NULL) snprintf(s.desc, sizeof(s.desc), "%s", desc);
	if (label != NULL) snprintf(s.label, sizeof(s.label), "%s", label);
	return s;
}

void fp_spin_tick(fp_spin_t* s) {
}

void fp_spin_dismiss(fp_spin_t* s) { (void)s; }

void fp_spin_finishf(fp_spin_t* s, int success, const char* fmt, ...) {
	char msg[FP_MAX_MSG];
	va_list ap;

	if (s == NULL) return;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	s->done = 1;
	emit_line(s->scope[0] ? s->scope : "daemon", s->desc,
		  success ? FP_LEVEL_OKAY : FP_LEVEL_ERROR, "%s", msg);
}

static void table_as_lines(const char* const* headers, size_t ncols,
			   const char* const* const* rows, size_t nrows) {
	char line[FP_MAX_MSG];

	for (size_t r = 0; r < nrows; r++) {
		size_t off = 0;
		line[0] = '\0';
		for (size_t k = 0; k < ncols; k++) {
			const char* cell = rows[r][k] != NULL ? rows[r][k] : "";
			int n = snprintf(line + off, sizeof(line) - off, "%s%s",
					 k ? "  " : "", cell);
			if (n < 0 || (size_t)n >= sizeof(line) - off) break;
			off += (size_t)n;
		}
		if (line[0] != '\0') fp_outl("%s", line);
	}
}

void fp_print_table(const char* const* headers, size_t ncols,
		    const char* const* const* rows, size_t nrows,
		    fp_table_sink_t sink) {
	if (fp_record_table(headers, ncols, rows, nrows)) return;
	table_as_lines(headers, ncols, rows, nrows);
}

// No terminal here: a prompt returns its own default, a menu nothing.

bool fp_prompt_confirm_default(const char* question, bool dflt) {
	return dflt;
}

int fp_prompt_menu(const char* title, const char* const* items, size_t n) {
	return -1;
}

// The worker's environment is already allowlisted; this is no second reader.
const char* cli_getenv(const char* name) { return getenv(name); }

const char* get_string(const char* msgid) { return msgid; }

// fp_now_ns/fp_logf: the CLI's terminal library on the other side.
int64_t fp_now_ns(void) {
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

void fp_logf(const char* scope, const char* desc, int level, const char* fmt,
	     ...) {
	va_list ap;
	va_start(ap, fmt);
	fp_vlogf(scope, desc, level, fmt, ap);
	va_end(ap);
}

// The worker's own environment, after typed_worker.c applied the allowlist.
char** cli_environ(void) {
	extern char** environ;
	return environ;
}
