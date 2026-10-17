#include "fplog.h"
#include "fplog_int.h"
#include "translations.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "evui.h"

typedef enum {
	FP_LINE_FULL = 0,
	FP_LINE_NO_NEWLINE,
	FP_LINE_PROGRESS
} fp_line_kind_t;

static pthread_once_t g_mu_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_out_mu;

static void mu_init(void) {
	pthread_mutexattr_t at;

	(void)pthread_mutexattr_init(&at);
	(void)pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
	(void)pthread_mutex_init(&g_out_mu, &at);
	(void)pthread_mutexattr_destroy(&at);
}

void fp_out_lock(void) {
	(void)pthread_once(&g_mu_once, mu_init);
	(void)pthread_mutex_lock(&g_out_mu);
}

void fp_out_unlock(void) { (void)pthread_mutex_unlock(&g_out_mu); }

void fp_write_fd(int fd, const char* s, size_t n) {
	while (n > 0) {
		ssize_t w = write(fd, s, n);
		if (w < 0) {
			if (errno == EINTR) continue;
			return;
		}
		if (w == 0) return;
		s += (size_t)w;
		n -= (size_t)w;
	}
}

void fp_write_stderr(const char* s, size_t n) {
	fp_write_fd(STDERR_FILENO, s, n);
}

#define FP_LINEBUF (FP_MAX_MSG + FP_LEFT_W + 128)

typedef struct {
	int fd;
	size_t len;
	char buf[FP_LINEBUF];
} fp_obuf_t;

static void ob_init(fp_obuf_t* o) {
	o->fd = STDERR_FILENO;
	o->len = 0;
}

static const char* s0(const char* s) { return s ? s : ""; }

static void ob_flush(fp_obuf_t* o) {
	if (o->len > 0) fp_write_fd(o->fd, o->buf, o->len);
	o->len = 0;
}

static void ob_write(fp_obuf_t* o, const char* s, size_t n) {
	if (n > sizeof(o->buf) - o->len) n = sizeof(o->buf) - o->len;
	memcpy(o->buf + o->len, s, n);
	o->len += n;
}

static void ob_cstr(fp_obuf_t* o, const char* s) { ob_write(o, s, strlen(s)); }

static void ob_pad(fp_obuf_t* o, size_t n) {
	static const char spaces[64] = "                                       "
				       "                         ";

	while (n > 0) {
		size_t chunk = n < sizeof(spaces) ? n : sizeof(spaces);
		ob_write(o, spaces, chunk);
		n -= chunk;
	}
}

static void ob_printf(fp_obuf_t* o, const char* fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void ob_printf(fp_obuf_t* o, const char* fmt, ...) {
	char tmp[128];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n > 0)
		ob_write(o, tmp,
			 (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

static bool g_have_override = false;
static fp_term_t g_override = {FP_TERM_FALLBACK_COLS, FP_TERM_FALLBACK_ROWS,
			       false};

static fp_term_t term_info_locked(void) {
	static const fp_term_t fallback = {FP_TERM_FALLBACK_COLS,
					   FP_TERM_FALLBACK_ROWS, false};
	struct winsize ws;

	if (g_have_override) return g_override;
	memset(&ws, 0, sizeof(ws));
	if (ioctl(STDERR_FILENO, TIOCGWINSZ, &ws) != 0) return fallback;
	if (ws.ws_col < FP_TERM_MIN_COLS || ws.ws_row < FP_TERM_MIN_ROWS)
		return fallback;
	fp_term_t ti;
	ti.cols = ws.ws_col;
	ti.rows = ws.ws_row;
	ti.is_tty = true;
	return ti;
}

fp_term_t fp_term_info(void) {
	fp_term_t ti;

	fp_out_lock();
	ti = term_info_locked();
	fp_out_unlock();
	return ti;
}

size_t fp_term_cols(void) { return term_info_locked().cols; }

size_t fp_term_rows(void) { return term_info_locked().rows; }

bool fp_fd_is_tty(int fd) {
	struct winsize ws;

	memset(&ws, 0, sizeof(ws));
	return ioctl(fd, TIOCGWINSZ, &ws) == 0;
}

const fp_level_t FP_LEVELS[] = {
	{"DEBUG", 17, 117},    {"INFO", 51, 159},     {"OKAY", 82, 157},
	{"WARN", 220, 228},    {"ERROR", 196, 210},   {"FATAL", 129, 183},
	{"SKIPPED", 147, 189}, {"RESTART", 213, 225}, {"DOWN", 245, 254},
};

const int FP_NLEVELS = (int)(sizeof(FP_LEVELS) / sizeof(FP_LEVELS[0]));

static const fp_level_t g_progress_begin = {"...", 31, 87};

const fp_level_t* fp_level_style(int level) {
	if (level == FP_LVL_PROGRESS) return &g_progress_begin;
	if (level < 0 || level >= FP_NLEVELS) return &FP_LEVELS[FP_LVL_INFO];
	return &FP_LEVELS[level];
}

static int level_fg_code_num(int level) {
	const int bg = fp_level_style(level)->bg;
	unsigned int luma;

	if (bg >= 232) {
		luma = (unsigned int)(bg - 232) * 11;
	} else if (bg >= 16) {
		const unsigned int i = (unsigned int)(bg - 16);
		luma = (i / 36) * 15 + ((i % 36) / 6) * 30 + (i % 6) * 6;
	} else {
		switch (bg) {
		case 0:
		case 1:
		case 4:
		case 5:
		case 8:
			return 255;
		default:
			return 0;
		}
	}
	return luma > 100 ? 0 : 255;
}

void fp_badge_text(int level, char* out) {
	const char* name = fp_level_style(level)->name;
	size_t len = strlen(name);

	if (len > FP_BADGE_TEXT_W) len = FP_BADGE_TEXT_W;
	memset(out, ' ', FP_BADGE_TEXT_W);
	memcpy(out + (FP_BADGE_TEXT_W - len) / 2, name, len);
}

size_t fp_visible_width(const char* s) {
	size_t width = 0, i = 0, n;

	if (s == NULL) return 0;
	n = strlen(s);
	while (i < n) {
		unsigned char c = (unsigned char)s[i];

		if (c == 0x1b) {
			i += 1;
			if (i < n && s[i] == '[') {
				i += 1;
				while (i < n && (unsigned char)s[i] >= 0x30 &&
				       (unsigned char)s[i] <= 0x3F)
					i += 1;
				while (i < n && (unsigned char)s[i] >= 0x20 &&
				       (unsigned char)s[i] <= 0x2F)
					i += 1;
				if (i < n && (unsigned char)s[i] >= 0x40 &&
				    (unsigned char)s[i] <= 0x7E)
					i += 1;
			}
		} else if ((c & 0x80) == 0) {
			width += 1;
			i += 1;
		} else {
			if ((c & 0xE0) == 0xC0) i += 2;
			else if ((c & 0xF0) == 0xE0)
				i += 3;
			else if ((c & 0xF8) == 0xF0)
				i += 4;
			else
				i += 1;
			width += 1;
		}
	}
	return width;
}

size_t fp_utf8_prefix(const char* s, size_t max) {
	size_t n;

	if (s == NULL) return 0;
	n = strlen(s);
	if (n <= max) return n;
	while (max > 0 && ((unsigned char)s[max] & 0xC0) == 0x80) max -= 1;
	return max;
}

static bool g_color = true;
static bool g_verbose = false;
static int g_logfmt_fd = -1;
static int64_t g_last_begin_ns = 0;

#define FP_LOCKED_GET(type, fn, var)                                           \
	type fn(void) {                                                        \
		type v_;                                                       \
                                                                               \
		fp_out_lock();                                                 \
		v_ = (var);                                                    \
		fp_out_unlock();                                               \
		return v_;                                                     \
	}
#define FP_LOCKED_SET(type, fn, var)                                           \
	void fn(type v_) {                                                     \
		fp_out_lock();                                                 \
		(var) = v_;                                                    \
		fp_out_unlock();                                               \
	}

static bool g_was_progress_line = false;

FP_LOCKED_SET(bool, fp_set_was_progress_line, g_was_progress_line)
FP_LOCKED_GET(bool, fp_get_was_progress_line, g_was_progress_line)

static char g_prefix[64] = "";

void fp_set_prefix(const char* prefix) {
	fp_out_lock();
	snprintf(g_prefix, sizeof(g_prefix), "%s", prefix ? prefix : "");
	fp_out_unlock();
}

const char* fp_get_prefix(void) { return g_prefix; }

static bool colors_on_locked(void) { return g_color; }

FP_LOCKED_GET(bool, fp_get_color, g_color)
FP_LOCKED_SET(bool, fp_set_color, g_color)
FP_LOCKED_SET(bool, fp_set_verbose, g_verbose)
FP_LOCKED_GET(bool, fp_get_verbose, g_verbose)
FP_LOCKED_SET(int, fp_set_logfmt_fd, g_logfmt_fd)

void fp_set_terminal_size(int rows, int cols) {
	fp_out_lock();
	if (rows <= 0 && cols <= 0) {
		g_have_override = false;
	} else {
		const fp_term_t ti = term_info_locked();
		const size_t r = rows > 0 ? (size_t)rows : ti.rows;
		const size_t c = cols > 0 ? (size_t)cols : ti.cols;

		if (c >= FP_TERM_MIN_COLS && r >= FP_TERM_MIN_ROWS) {
			g_override.cols = c;
			g_override.rows = r;
			g_override.is_tty = true;
			g_have_override = true;
		} else {
			g_have_override = false;
		}
	}
	fp_out_unlock();
}

bool fp_is_tty(void) { return fp_fd_is_tty(STDERR_FILENO); }

static bool g_bar_active = false;

FP_LOCKED_GET(bool, fp_bar_held, g_bar_active)

void fp_bar_release(void) {
	fp_out_lock();
	g_bar_active = false;
	fp_out_unlock();
}

static void write_name_style(fp_obuf_t* o, const char* s, bool colors_on) {
	const char* colon;

	if (!colors_on) {
		ob_cstr(o, s);
		return;
	}
	ob_printf(o, FP_SGR_DIM_FG256, FP_LEVELS[FP_LVL_INFO].bg);
	colon = strchr(s, ':');
	if (colon == NULL) {
		ob_cstr(o, s);
	} else {
		ob_write(o, s, (size_t)(colon - s));
		ob_cstr(o, FP_SGR_RESET ":");
		ob_printf(o, FP_SGR_DIM_FG256, FP_LEVELS[FP_LVL_OKAY].bg);
		ob_cstr(o, colon + 1);
	}
	ob_cstr(o, FP_SGR_RESET);
}

static size_t write_left(fp_obuf_t* o, const char* scope_name, const char* desc,
			 bool colors_on) {
	size_t budget, raw_len, cut;
	char base[FP_LEFT_W + 3];
	char raw[FP_LEFT_W + 2 + 64 + 1];
	char cutbuf[FP_LEFT_W + 2];
	int n;

	budget = FP_LEFT_W;

	if (desc[0] == '\0' || strcmp(desc, scope_name) == 0)
		n = snprintf(base, sizeof(base), "%s", scope_name);
	else
		n = snprintf(base, sizeof(base), "%s: %s", scope_name, desc);
	if (n < 0 || (size_t)n > (size_t)FP_LEFT_W + 2)
		(void)snprintf(base, sizeof(base), "%s", scope_name);
	base[fp_utf8_prefix(base, budget)] = '\0';

	if (g_prefix[0] == '\0') {
		(void)snprintf(raw, sizeof(raw), "%s", base);
	} else {
		// The prefix is the app id, so it goes in SCOPE and the module in the
		// description: a launch reads "gimp: export".
		n = snprintf(raw, sizeof(raw), "%s: %s", g_prefix, base);
		if (n < 0 || (size_t)n > (size_t)FP_LEFT_W + 2 + 64)
			(void)snprintf(raw, sizeof(raw), "%s", base);
	}
	raw_len = fp_utf8_prefix(raw, budget);
	raw[raw_len] = '\0';

	// > and not >=: a field exactly `budget` wide is not over it.
	if (raw_len > budget && budget > 2) {
		cut = fp_utf8_prefix(raw, budget - 2);
		memcpy(cutbuf, raw, cut);
		cutbuf[cut] = '\0';
		write_name_style(o, cutbuf, colors_on);
		write_name_style(o, "..", colors_on);
		return fp_visible_width(cutbuf) + 2;
	}
	write_name_style(o, raw, colors_on);
	ob_pad(o, budget - raw_len);
	return fp_visible_width(raw) + (budget - raw_len);
}

static void write_badge(fp_obuf_t* o, int level, bool colors_on) {
	char text[FP_BADGE_TEXT_W];

	fp_badge_text(level, text);
	if (!colors_on) {
		ob_cstr(o, "[");
		ob_write(o, text, FP_BADGE_TEXT_W);
		ob_cstr(o, "]");
		return;
	}
	ob_printf(o, FP_SGR_BADGE_OPEN, level == FP_LVL_FATAL ? ";5" : "",
		  level_fg_code_num(level), fp_level_style(level)->bg);
	ob_write(o, text, FP_BADGE_TEXT_W);
	ob_cstr(o, "]" FP_SGR_RESET);
}

static void render_line_body(fp_obuf_t* o, const char* scope, const char* desc,
			     int level, const char* msg) {
	const bool colors_on = colors_on_locked();
	const bool tty = true;
	const size_t cols = fp_term_cols();
	const size_t avail = fp_msg_avail(cols);
	const size_t vis = fp_visible_width(msg);
	const bool has_ansi = strchr(msg, FP_ESC_CHAR) != NULL;
	const bool trunc = (vis > avail && avail > 1);
	size_t written_msg;
	size_t left_w, rows_up, badge_col;

	left_w = write_left(o, scope, desc, colors_on);
	ob_pad(o, FP_COL_GAP);
	if (colors_on)
		ob_printf(o, FP_SGR_BOLD_FG256, fp_level_style(level)->msg);

	written_msg = trunc ? (has_ansi ? vis : avail + 2)
			    : (avail > vis ? avail : vis);
	if (trunc) {
		if (has_ansi) {
			ob_cstr(o, msg);
		} else {
			ob_write(o, msg, avail - 1);
			ob_cstr(o, "...");
		}
	} else {
		ob_cstr(o, msg);
		if (avail > vis) ob_pad(o, avail - vis);
	}

	rows_up = cols > 0 ? (left_w + FP_COL_GAP + written_msg) / cols : 0;
	badge_col =
		cols > (size_t)FP_BADGE_W ? cols - (size_t)FP_BADGE_W + 1 : 1;
	if (tty) {
		if (rows_up > 0) ob_printf(o, FP_CUU, rows_up);
		ob_printf(o, FP_CR_CHA, badge_col);
		write_badge(o, level, colors_on);
		if (rows_up > 0) ob_printf(o, FP_CUD, rows_up);
	} else {
		ob_pad(o, FP_COL_GAP);
		write_badge(o, level, colors_on);
	}
}

static void print_line_locked(const char* scope, const char* desc, int level,
			      const char* msg, fp_line_kind_t kind) {
	const bool colors_on = colors_on_locked();
	fp_obuf_t o;

	ob_init(&o);

	if (g_was_progress_line) {
		if (colors_on) ob_cstr(&o, FP_CR_EL_LINE);
		else
			ob_cstr(&o, FP_CR);
	}
	render_line_body(&o, scope, desc, level, msg);
	switch (kind) {
	case FP_LINE_FULL:
		ob_cstr(&o, "\n");
		g_was_progress_line = false;
		break;
	case FP_LINE_NO_NEWLINE:
		break;
	case FP_LINE_PROGRESS:
	default:
		ob_cstr(&o, "\r");
		g_was_progress_line = true;
		break;
	}
	ob_flush(&o);
}

__attribute__((weak)) void fp_anchor_after_log_line(void) {}

static bool needs_quoting(const char* s) {
	return strpbrk(s, " \t\n\r=\"") != NULL;
}

static void append_logfmt_quoted(char* buf, size_t buflen, size_t* pos,
				 const char* s) {
	size_t n;

	if (!needs_quoting(s)) {
		n = strlen(s);
		if (n > buflen - *pos) n = buflen - *pos;
		memcpy(buf + *pos, s, n);
		*pos += n;
		return;
	}
	if (*pos < buflen) buf[(*pos)++] = '"';
	for (const char* p = s; *p != '\0'; p++) {
		if ((*p == '"' || *p == '\\') && *pos < buflen)
			buf[(*pos)++] = '\\';
		if (*pos < buflen) buf[(*pos)++] = *p;
	}
	if (*pos < buflen) buf[(*pos)++] = '"';
}

static void logfmt_lit(char* buf, size_t buflen, size_t* pos, const char* lit) {
	size_t n = strlen(lit);

	if (buflen - *pos < n) return;
	memcpy(buf + *pos, lit, n);
	*pos += n;
}

static void emit_logfmt(const char* scope, int level, const char* msg) {
	struct timespec ts;
	char line[FP_MAX_MSG + 256];
	size_t pos = 0;
	int n;

	if (g_logfmt_fd < 0) return;
	(void)clock_gettime(CLOCK_REALTIME, &ts);

	n = snprintf(line, sizeof(line),
		     "ts=%lld level=", (long long)ts.tv_sec);
	if (n < 0) return;
	pos = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
	{
		const char* name = fp_level_style(level)->name;
		size_t len = strlen(name);
		if (len > sizeof(line) - pos) len = sizeof(line) - pos;
		memcpy(line + pos, name, len);
		pos += len;
	}
	logfmt_lit(line, sizeof(line), &pos, " scope=");
	append_logfmt_quoted(line, sizeof(line), &pos, scope);
	logfmt_lit(line, sizeof(line), &pos, " msg=");
	append_logfmt_quoted(line, sizeof(line), &pos, msg);
	if (pos < sizeof(line)) line[pos++] = '\n';

	fp_write_fd(g_logfmt_fd, line, pos);
}

int64_t fp_now_ns(void) {
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static size_t format_took(char* out, size_t outlen, double secs) {
	unsigned long long total_s, m, s, h;
	double tenths;

	if (secs < 60.0) {
		tenths = secs * 10.0;
		tenths += 0.5;
		return (size_t)snprintf(out, outlen, "%llu.%llus",
					(unsigned long long)tenths / 10,
					(unsigned long long)tenths % 10);
	}
	if (secs < 0.0) secs = 0.0;
	total_s = (unsigned long long)secs;
	if (total_s < 60)
		return (size_t)snprintf(out, outlen, "%llus", total_s);
	m = total_s / 60;
	s = total_s % 60;
	if (m < 60) return (size_t)snprintf(out, outlen, "%llum%llus", m, s);
	h = m / 60;
	return (size_t)snprintf(out, outlen, "%lluh%llum", h, m % 60);
}

static size_t took_suffix(char* out, size_t outlen, int64_t start_ns) {
	char dur[32];
	int64_t now;

	if (start_ns == 0) return 0;
	now = fp_now_ns();
	if (now <= start_ns) return 0;
	format_took(dur, sizeof(dur), (double)(now - start_ns) / 1e9);
	return (size_t)snprintf(out, outlen, " (took %s)", dur);
}

static void format_msg(char* buf, size_t buflen, const char* fmt, va_list ap) {
	if (fmt == NULL) {
		buf[0] = '\0';
		return;
	}
	(void)vsnprintf(buf, buflen, get_string(fmt), ap);
}

void fp_anchor_after_log_line(void);

// While a sink is installed (a worker serving a typed request), lines are records
// for that request instead of terminal output; evui.c serves the daemon.
static evui_sink_t g_rec;
static bool g_have_rec;

void evui_set_sink(const evui_sink_t* sink) {
	if (sink == NULL) {
		memset(&g_rec, 0, sizeof(g_rec));
		g_have_rec = false;
		return;
	}
	g_rec = *sink;
	g_have_rec = true;
}

bool fp_record_active(void) { return g_have_rec; }

bool fp_record_progress(const char* stage, uint64_t cur, uint64_t total) {
	if (!g_have_rec || g_rec.progress == NULL) return false;
	g_rec.progress(g_rec.ctx, stage, cur, total);
	return true;
}

static bool record_line(const char* scope, int level, const char* msg) {
	if (!g_have_rec || g_rec.log == NULL) return false;
	g_rec.log(g_rec.ctx, level, scope && *scope ? scope : "daemon", msg);
	return true;
}

bool fp_record_line(int level, const char* text) {
	return record_line("", level, text);
}

bool fp_record_table(const char* const* headers, size_t ncols,
		     const char* const* const* rows, size_t nrows) {
	if (!g_have_rec || g_rec.table == NULL) return false;
	return g_rec.table(g_rec.ctx, headers, ncols, rows, nrows);
}

static void emit_line(const char* scope, const char* desc, int level,
		      const char* msg) {
	if (record_line(scope, level, msg)) return;
	fp_out_lock();
	print_line_locked(scope, desc, level, msg, FP_LINE_FULL);
	// A pinned progress area has to know a row was used, or the next
	// redraw puts the cursor back on top of this line.
	fp_anchor_after_log_line();
	emit_logfmt(scope, level, msg);
	fp_out_unlock();
}

static bool verbose_allows(int level) {
	return level != FP_LEVEL_DEBUG || g_verbose;
}

static void end_line(const char* scope, const char* desc, int level,
		     const char* msg) {
	char joined[FP_MAX_MSG + 40];
	int n;

	n = snprintf(joined, sizeof(joined), "%s", msg);
	if (n < 0) return;
	if ((size_t)n < sizeof(joined))
		(void)took_suffix(joined + n, sizeof(joined) - (size_t)n,
				  g_last_begin_ns);
	emit_line(scope, desc, level, joined);
}

static void emit_begin(const char* scope, const char* desc, const char* msg) {
	if (record_line(scope, FP_LEVEL_INFO, msg)) return;
	fp_out_lock();
	g_last_begin_ns = fp_now_ns();
	print_line_locked(scope, desc, FP_LVL_PROGRESS, msg, FP_LINE_FULL);
	fp_anchor_after_log_line();
	fp_out_unlock();
}

void fp_vlogf(const char* scope, const char* desc, int level, const char* fmt,
	      va_list ap) {
	char msg[FP_MAX_MSG];

	format_msg(msg, sizeof(msg), fmt, ap);
	if (!verbose_allows(level)) return;
	emit_line(s0(scope), s0(desc), level, msg);
}

void fp_logf(const char* scope, const char* desc, int level, const char* fmt,
	     ...) {
	va_list ap;

	va_start(ap, fmt);
	fp_vlogf(scope, desc, level, fmt, ap);
	va_end(ap);
}

static void begin_vf(const char* scope, const char* desc, const char* fmt,
		     va_list ap) {
	char msg[FP_MAX_MSG];

	format_msg(msg, sizeof(msg), fmt, ap);
	emit_begin(s0(scope), s0(desc), msg);
}

static void end_vf(const char* scope, const char* desc, int success,
		   int fail_level, const char* fmt, va_list ap) {
	char msg[FP_MAX_MSG];

	format_msg(msg, sizeof(msg), fmt, ap);
	end_line(s0(scope), s0(desc), success ? FP_LEVEL_OKAY : fail_level,
		 msg);
}

fp_logger_t fp_logger_init(const char* scope, const char* desc) {
	fp_logger_t l;

	memset(&l, 0, sizeof(l));
	(void)snprintf(l.scope, sizeof(l.scope), "%s", scope ? scope : "");
	(void)snprintf(l.desc, sizeof(l.desc), "%s", desc ? desc : "");
	return l;
}

static void logger_vlogf(const fp_logger_t* l, int level, const char* fmt,
			 va_list ap) {
	fp_vlogf(l ? l->scope : "", l ? l->desc : "", level, fmt, ap);
}

#define FP_LOGGER_LEVEL_FN(name, level)                                        \
	void name(const fp_logger_t* l, const char* fmt, ...) {                \
		va_list ap;                                                    \
                                                                               \
		va_start(ap, fmt);                                             \
		logger_vlogf(l, level, fmt, ap);                               \
		va_end(ap);                                                    \
	}

FP_LOGGER_LEVEL_FN(fp_logger_infof, FP_LEVEL_INFO)
FP_LOGGER_LEVEL_FN(fp_logger_okf, FP_LEVEL_OKAY)
FP_LOGGER_LEVEL_FN(fp_logger_warnf, FP_LEVEL_WARN)
FP_LOGGER_LEVEL_FN(fp_logger_errorf, FP_LEVEL_ERROR)
FP_LOGGER_LEVEL_FN(fp_logger_debugf, FP_LEVEL_DEBUG)

#undef FP_LOGGER_LEVEL_FN

void fp_logger_beginf(const fp_logger_t* l, const char* fmt, ...) {
	va_list ap;

	va_start(ap, fmt);
	begin_vf(l ? l->scope : "", l ? l->desc : "", fmt, ap);
	va_end(ap);
}

#define FP_LOGGER_END_FN(name, fail_level)                                     \
	void name(const fp_logger_t* l, int success, const char* fmt, ...) {   \
		va_list ap;                                                    \
                                                                               \
		va_start(ap, fmt);                                             \
		end_vf(l ? l->scope : "", l ? l->desc : "", success,           \
		       fail_level, fmt, ap);                                   \
		va_end(ap);                                                    \
	}

FP_LOGGER_END_FN(fp_logger_endf, FP_LEVEL_ERROR)
FP_LOGGER_END_FN(fp_logger_wendf, FP_LEVEL_WARN)

#undef FP_LOGGER_END_FN

static void raw_write(int fd, const char* s, size_t n) {
	if (s == NULL || n == 0) return;
	fp_out_lock();
	fp_write_fd(fd, s, n);
	fp_out_unlock();
}

void fp_out_raw(const char* s, size_t n) { raw_write(STDOUT_FILENO, s, n); }

void fp_err_raw(const char* s, size_t n) { raw_write(STDERR_FILENO, s, n); }

void fp_outl(const char* fmt, ...) {
	char line[FP_MAX_MSG + 2];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(line, sizeof(line) - 1, get_string(fmt ? fmt : "%s"), ap);
	va_end(ap);
	if (n < 0) return;
	if ((size_t)n >= sizeof(line) - 1) n = (int)(sizeof(line) - 2);
	line[n++] = '\n';
	fp_out_raw(line, (size_t)n);
}

void fp_out(const char* fmt, ...) {
	char line[FP_MAX_MSG];
	va_list ap;

	va_start(ap, fmt);
	(void)vsnprintf(line, sizeof(line), get_string(fmt ? fmt : "%s"), ap);
	va_end(ap);
	fp_out_raw(line, strlen(line));
}
