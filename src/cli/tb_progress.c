#include "evui.h"
#include "fplog.h"
#include "fplog_int.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define NS_PER_MS 1000000LL
#define NS_PER_S 1000000000LL

#define TB_MAX_BARS 16
#define TB_DSR_TIMEOUT_MS 250

#define TB_GLYPH_BEGIN "["
#define TB_GLYPH_FILL "\xe2\x96\x88"
#define TB_GLYPH_EMPTY "\xe2\x96\x91"
#define TB_GLYPH_END "]"

static const char TB_SPINNER_FRAMES[] =
	"\xe2\xa0\x8b\xe2\xa0\x99\xe2\xa0\xb9\xe2\xa0\xb8\xe2\xa0\xbc"
	"\xe2\xa0\xb4\xe2\xa0\xa6\xe2\xa0\xa7\xe2\xa0\x87\xe2\xa0\x8f";
#define TB_SPINNER_NFRAMES (sizeof(TB_SPINNER_FRAMES) / 3 - 1)

typedef struct {
	const char* begin;
	const char* fill;
	const char* empty;
	const char* end;
} tb_bar_chars_t;

typedef struct {
	bool active;
	bool one_shot;
	const char* scope_name;
	const char* description;
	const char* label;
	uint64_t total;
	uint64_t current;
	tb_bar_chars_t chars;
	bool show_eta;
	char glyphs[4][2];
} tb_slot_t;

static tb_slot_t g_bar_slots[TB_MAX_BARS];
static size_t g_bar_count;
static bool g_anchor;
static unsigned g_one_shot_slot;
static bool g_pin;
static size_t g_rows = 24;
static size_t g_log_row = 1;
static bool g_cursor_hidden;

static uint32_t g_progress_throttle_ms;
static int64_t g_last_progress_draw_ns;
static int64_t g_one_shot_start_ns;
static int64_t g_progress_start_ns;

static bool g_row_scopeless;

typedef struct {
	char* b;
	size_t cap;
	size_t pos;
} tbbuf;

void fp_anchor_after_log_line(size_t rows);

static tb_bar_chars_t chars_from_glyphs(const unsigned char glyphs[4],
					char store[4][2]);
static void write_out_locked(tbbuf* t);
static void bar_redraw(unsigned slot);
static void bar_unregister(unsigned slot);
static void bar_sync(unsigned slot, uint64_t current, uint64_t total);
static void bar_set_label(unsigned slot, const char* label);
static void print_progress_line_ex(const char* sn, const char* desc,
				   const char* label, uint64_t current,
				   uint64_t total,
				   const unsigned char glyphs[4], bool show_eta,
				   bool force);
static void finish_progress(const char* sn, const char* desc, bool success,
			    int64_t start_ns, const char* msg);

static int64_t mono_ns(void) {
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
	return (int64_t)ts.tv_sec * NS_PER_S + (int64_t)ts.tv_nsec;
}

static void tb_add(tbbuf* t, const char* s, size_t n) {
	if (t->pos >= t->cap) return;
	if (n > t->cap - t->pos) n = t->cap - t->pos;
	memcpy(t->b + t->pos, s, n);
	t->pos += n;
}

static void tb_str(tbbuf* t, const char* s) { tb_add(t, s, strlen(s)); }

static void tb_fmt(tbbuf* t, const char* fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void tb_fmt(tbbuf* t, const char* fmt, ...) {
	va_list ap;
	int n;

	if (t->pos >= t->cap) return;
	va_start(ap, fmt);
	n = vsnprintf(t->b + t->pos, t->cap - t->pos, fmt, ap);
	va_end(ap);
	if (n > 0)
		t->pos += (size_t)n < t->cap - t->pos ? (size_t)n
						      : t->cap - t->pos;
}

static void tb_splat(tbbuf* t, const char* s, size_t n) {
	for (size_t i = 0; i < n; i++) tb_str(t, s);
}

static void tb_spaces(tbbuf* t, size_t n) { tb_splat(t, " ", n); }

static void tb_emit(tbbuf* t) {
	if (t->pos == 0) return;
	fp_out_lock();
	fp_write_stderr(t->b, t->pos);
	fp_out_unlock();
}

static void cursor_hide(void) {
	char buf[16];
	tbbuf t;

	if (g_cursor_hidden) return;
	if (!fp_term_info().is_tty) return;
	g_cursor_hidden = true;
	t.b = buf;
	t.cap = sizeof(buf);
	t.pos = 0;
	tb_str(&t, FP_CSI_HIDE_CURSOR);
	tb_emit(&t);
}

static void cursor_show(void) {
	char buf[16];
	tbbuf t;

	if (!g_cursor_hidden) return;
	g_cursor_hidden = false;
	t.b = buf;
	t.cap = sizeof(buf);
	t.pos = 0;
	tb_str(&t, FP_CSI_SHOW_CURSOR);
	tb_emit(&t);
}

static void write_out_locked(tbbuf* t) {
	if (t->pos == 0) return;
	fp_write_stderr(t->b, t->pos);
}

static int fg_code_num(int bg) {
	unsigned luma;

	if (bg >= 232) {
		luma = (unsigned)(bg - 232) * 11;
	} else if (bg >= 16) {
		int i = bg - 16;
		luma = (unsigned)(i / 36) * 15 + (unsigned)((i % 36) / 6) * 30 +
		       (unsigned)(i % 6) * 6;
	} else {
		switch (bg) {
		case 0:
		case 1:
		case 4:
		case 5:
		case 8:
			luma = 255;
			break;
		default:
			luma = 0;
			break;
		}
	}
	return luma > 100 ? 0 : 255;
}

static bool colors_on(void) { return fp_get_color(); }

static void write_name_style(tbbuf* t, const char* s) {
	const char* colon;

	if (!colors_on()) {
		tb_str(t, s);
		return;
	}
	colon = strchr(s, ':');
	tb_fmt(t, FP_SGR_DIM_FG256, fp_level_style(FP_LVL_INFO)->bg);
	if (colon == NULL) {
		tb_str(t, s);
	} else {
		tb_add(t, s, (size_t)(colon - s));
		tb_str(t, FP_SGR_RESET ":");
		tb_fmt(t, FP_SGR_DIM_FG256, fp_level_style(FP_LVL_OKAY)->bg);
		tb_str(t, colon + 1);
	}
	tb_str(t, FP_SGR_RESET);
}

static size_t write_left(tbbuf* t, const char* scope_name, const char* desc) {
	const size_t budget = FP_LEFT_W;
	const char* prefix = fp_get_prefix();
	char base[FP_LEFT_W + 64 + 4];
	const char* raw;
	size_t raw_len;
	size_t cut;

	if (scope_name == NULL) scope_name = "";
	if (desc == NULL) desc = "";

	if (desc[0] == '\0' || strcmp(desc, scope_name) == 0) {
		snprintf(base, sizeof(base), "%s", scope_name);
	} else {
		snprintf(base, sizeof(base), "%s: %s", scope_name, desc);
	}
	raw = base;
	raw_len = fp_utf8_prefix(raw, budget);

	if (prefix != NULL && prefix[0] != '\0') {
		char full[sizeof(base) + 64];
		// The prefix is the app id, so it goes in SCOPE and the module in the
		// description: a launch reads "gimp: export".
		snprintf(full, sizeof(full), "%s: %s", prefix, base);
		raw = full;
		raw_len = fp_utf8_prefix(raw, budget);
	}

	// > and not >=: a field exactly `budget` wide is not over it.
	if (raw_len > budget && budget > 2) {
		char part[sizeof(base)];

		cut = fp_utf8_prefix(raw, budget - 2);
		memcpy(part, raw, cut);
		part[cut] = '\0';
		write_name_style(t, part);
		write_name_style(t, "..");
		return fp_visible_width(part) + 2;
	}
	{
		char part[sizeof(base)];
		memcpy(part, raw, raw_len);
		part[raw_len] = '\0';
		write_name_style(t, part);
	}
	tb_spaces(t, budget - raw_len);
	return budget;
}

static void write_progress_badge(tbbuf* t, int pct) {
	const fp_level_t* lv = fp_level_style(FP_LVL_PROGRESS);
	char text[FP_BADGE_TEXT_W + 1];
	size_t len;

	if (pct == 127) {
		memset(text, ' ', sizeof(text));
		memcpy(text + 2, "...", 3);
		len = FP_BADGE_TEXT_W;
	} else {
		snprintf(text, sizeof(text), " %3d%%  ", pct);
		len = FP_BADGE_TEXT_W;
	}
	text[len] = '\0';

	if (!colors_on()) {
		tb_str(t, "[");
		tb_add(t, text, len);
		tb_str(t, "]");
		return;
	}
	tb_fmt(t, FP_SGR_BAR_BADGE_OPEN, fg_code_num(lv->bg), lv->bg);
	tb_add(t, text, len);
	tb_str(t, "]" FP_SGR_RESET);
}

static void write_progress_line(tbbuf* t, const char* scope_name,
				const char* desc, const char* msg, int pct) {
	size_t cols = fp_term_cols();
	size_t left_w;
	size_t avail, vis, written_msg, rows_up, badge_col;
	bool colors = colors_on();
	bool has_ansi;
	char plain[FP_MAX_MSG + 8];
	size_t mlen;

	mlen = fp_utf8_prefix(msg, sizeof(plain) - 1);
	memcpy(plain, msg, mlen);
	plain[mlen] = '\0';

	if (g_row_scopeless) {
		left_w = 0;
	} else {
		left_w = write_left(t, scope_name, desc);
		tb_spaces(t, FP_COL_GAP);
	}
	if (colors)
		tb_fmt(t, FP_SGR_BOLD_FG256,
		       fp_level_style(FP_LVL_PROGRESS)->msg);

	avail = fp_msg_avail(cols) +
		(g_row_scopeless ? FP_LEFT_W + FP_COL_GAP : 0);
	vis = fp_visible_width(plain);
	has_ansi = strchr(plain, FP_ESC_CHAR) != NULL;
	if (vis > avail && avail > 1) written_msg = has_ansi ? vis : avail + 2;
	else
		written_msg = avail > vis ? avail : vis;

	if (vis > avail && avail > 1) {
		if (has_ansi) {
			tb_str(t, plain);
		} else {
			tb_add(t, plain, avail - 1);
			tb_str(t, "...");
		}
	} else {
		tb_str(t, plain);
		if (avail > vis) tb_spaces(t, avail - vis);
	}

	rows_up = cols > 0 ? (left_w + FP_COL_GAP + written_msg) / cols : 0;
	badge_col = cols > FP_BADGE_W ? cols - FP_BADGE_W + 1 : 1;
	if (rows_up > 0) tb_fmt(t, FP_CUU, rows_up);
	tb_fmt(t, FP_CR_CHA, badge_col);
	write_progress_badge(t, pct);
	if (rows_up > 0) tb_fmt(t, FP_CUD, rows_up);
}

static void format_eta(double secs, char* buf, size_t cap) {
	double v = secs > 0.0 ? secs : 0.0;
	uint64_t total_s = (uint64_t)v;
	uint64_t m, s, h, rm;

	if (total_s < 60) {
		snprintf(buf, cap, "%llus", (unsigned long long)total_s);
		return;
	}
	m = total_s / 60;
	s = total_s % 60;
	if (m < 60) {
		snprintf(buf, cap, "%llum%llus", (unsigned long long)m,
			 (unsigned long long)s);
		return;
	}
	h = m / 60;
	rm = m % 60;
	snprintf(buf, cap, "%lluh%llum", (unsigned long long)h,
		 (unsigned long long)rm);
}

static void format_duration(int64_t ns, char* buf, size_t cap) {
	double secs = (double)ns / (double)NS_PER_S;

	if (secs < 60.0) {
		double t10 = secs * 10.0;
		uint64_t tenths;

		if (t10 < 0.0) tenths = 0;
		else
			tenths = (uint64_t)(t10 + 0.5);
		snprintf(buf, cap, "%llu.%llus",
			 (unsigned long long)(tenths / 10),
			 (unsigned long long)(tenths % 10));
		return;
	}
	format_eta(secs, buf, cap);
}

static size_t took_suffix(int64_t start_ns, char* buf, size_t cap) {
	int64_t now = mono_ns();
	char dur[32];

	if (start_ns == 0 || now <= start_ns) {
		if (cap > 0) buf[0] = '\0';
		return 0;
	}
	format_duration(now - start_ns, dur, sizeof(dur));
	return (size_t)snprintf(buf, cap, " (took %s)", dur);
}

static unsigned pct_of(uint64_t current, uint64_t total) {
	uint64_t pct = total > 0 ? current * 100 / total : 0;

	return pct > 100 ? 100u : (unsigned)pct;
}

#define TB_BAR_MIN 12	// the least a bar shrinks to on a narrow terminal
#define TB_LABEL_MIN 12 // below this label window the bar goes entirely

// Fit a label into w columns by dropping its middle, so the id start and the
// "//branch" end both stay readable.
static void fit_label(const char* label, size_t w, char* out, size_t cap) {
	size_t len = strlen(label);

	if (cap == 0) return;
	if (len <= w || w < 4) {
		snprintf(out, cap, "%s", len <= w ? label : "");
		if (len > w && w > 0) {
			size_t keep = fp_utf8_prefix(label, w);
			memcpy(out, label, keep < cap - 1 ? keep : cap - 1);
			out[keep < cap - 1 ? keep : cap - 1] = '\0';
		}
		return;
	}
	{
		size_t tail = w / 3 < 16 ? w / 3 : 16;
		size_t head = w - tail - 1;
		size_t hbytes = fp_utf8_prefix(label, head);
		const char* t = label + len - tail;

		// never start the tail in the middle of a character
		while (*t != '\0' && ((unsigned char)*t & 0xC0) == 0x80) t++;
		snprintf(out, cap, "%.*s\xE2\x80\xA6%s", (int)hbytes, label, t);
	}
}

static size_t build_progress_msg(const char* label, size_t label_w,
				 uint64_t current, uint64_t total,
				 tb_bar_chars_t chars, bool show_eta,
				 char* msg_buf, size_t cap) {
	size_t avail = fp_msg_avail(fp_term_cols());
	bool colors = colors_on();
	size_t pos = 0;
	char eta_buf[24];
	size_t eta_len = 0;
	tbbuf t;

	eta_buf[0] = '\0';
	if (show_eta && total > 0) {
		if (current == 0) g_progress_start_ns = 0;
		if (current > 0) {
			int64_t now = mono_ns();
			if (g_progress_start_ns == 0) g_progress_start_ns = now;
			if (now > g_progress_start_ns) {
				double elapsed_s =
					(double)(now - g_progress_start_ns) /
					(double)NS_PER_S;
				if (elapsed_s > 0.0) {
					double rate =
						(double)current / elapsed_s;
					if (rate > 0.0) {
						uint64_t left =
							total > current
								? total - current
								: 0;
						format_eta((double)left / rate,
							   eta_buf,
							   sizeof(eta_buf));
						eta_len = strlen(eta_buf);
					}
				}
			}
		}
	}

	{
		size_t chrome = 2 + 2 + (eta_len > 0 ? 1 + eta_len : 0);
		size_t room = avail > chrome ? avail - chrome : 0;
		size_t bar_w, lw, filled, empty, i;
		char fitted[512];
		bool scopeless = false;

		if (room >= label_w + TB_BAR_MIN) {
			bar_w = room - label_w;
			lw = label_w;
		} else {
			scopeless = true;
			room += FP_LEFT_W + FP_COL_GAP;
			if (room >= label_w + TB_BAR_MIN) {
				bar_w = room - label_w;
				lw = label_w;
			} else if (room >= TB_BAR_MIN + TB_LABEL_MIN) {
				bar_w = TB_BAR_MIN;
				lw = room - TB_BAR_MIN;
			} else {
				bar_w = 0;
				lw = room;
			}
		}
		g_row_scopeless = scopeless;

		t.b = msg_buf;
		t.cap = cap;
		t.pos = 0;

		if (bar_w == 0) {
			if (scopeless) avail += FP_LEFT_W + FP_COL_GAP;
			fit_label(label, avail, fitted, sizeof(fitted));
			tb_str(&t, fitted);
			pos = t.pos;
			goto done;
		}

		fit_label(label, lw, fitted, sizeof(fitted));
		tb_str(&t, fitted);
		{
			size_t vis = fp_visible_width(fitted);

			tb_spaces(&t, lw > vis ? lw - vis : 0);
		}
		tb_str(&t, "  ");
		tb_str(&t, chars.begin);
		filled = total > 0 ? (size_t)((current * bar_w) / total) : 0;
		if (filled > bar_w) filled = bar_w;
		if (filled > 0) {
			if (colors)
				tb_fmt(&t, FP_SGR_FG256,
				       fp_level_style(FP_LVL_PROGRESS)->msg);
			for (i = 0; i < filled; i++) tb_str(&t, chars.fill);
			if (colors) tb_str(&t, FP_SGR_RESET);
		}
		empty = bar_w - filled;
		if (empty > 0) {
			if (colors) tb_str(&t, FP_SGR_DIM);
			for (i = 0; i < empty; i++) tb_str(&t, chars.empty);
			if (colors) tb_str(&t, FP_SGR_RESET);
		}
		tb_str(&t, chars.end);
		if (eta_len > 0) {
			tb_str(&t, " ");
			tb_add(&t, eta_buf, eta_len);
		}
		pos = t.pos;
	}
done:
	if (cap > 0) msg_buf[pos < cap ? pos : cap - 1] = '\0';
	return pos;
}

static tb_bar_chars_t tb_default_chars(void) {
	tb_bar_chars_t c;

	c.begin = TB_GLYPH_BEGIN;
	c.fill = TB_GLYPH_FILL;
	c.empty = TB_GLYPH_EMPTY;
	c.end = TB_GLYPH_END;
	return c;
}

static void slot_set_glyphs(tb_slot_t* s, const unsigned char glyphs[4]) {
	for (int i = 0; i < 4; i++) {
		s->glyphs[i][0] = (char)glyphs[i];
		s->glyphs[i][1] = '\0';
	}
}

static tb_bar_chars_t tb_slot_chars(const tb_slot_t* s) {
	tb_bar_chars_t c = tb_default_chars();

	if (s->glyphs[0][0] != '\0') c.begin = s->glyphs[0];
	if (s->glyphs[1][0] != '\0') c.fill = s->glyphs[1];
	if (s->glyphs[2][0] != '\0') c.empty = s->glyphs[2];
	if (s->glyphs[3][0] != '\0') c.end = s->glyphs[3];
	return c;
}

#define TB_SCOPE_TRANSACTION "transaction"

static bool bar_is_bottom(size_t i) {
	return strcmp(g_bar_slots[i].scope_name, TB_SCOPE_TRANSACTION) == 0;
}

static size_t bar_rank(size_t i) {
	bool bot = bar_is_bottom(i);
	size_t rank = 0;

	for (size_t j = 0; j < TB_MAX_BARS; j++) {
		if (!g_bar_slots[j].active) continue;
		if (bar_is_bottom(j) == bot) {
			if (j <= i) rank++;
		} else if (bot) {
			rank++; // a top bar is above a bottom one
		}
	}
	return rank;
}

static size_t bar_line_of(size_t i) { return bar_rank(i); }

static size_t bar_row_of(size_t i) {
	return g_rows - g_bar_count + bar_rank(i);
}

static size_t active_slot_in_draw_order(size_t k) {
	size_t seen = 0;

	for (size_t i = 0; i < TB_MAX_BARS; i++) {
		if (!g_bar_slots[i].active || bar_is_bottom(i)) continue;
		if (++seen == k) return i;
	}
	for (size_t i = 0; i < TB_MAX_BARS; i++) {
		if (!g_bar_slots[i].active || !bar_is_bottom(i)) continue;
		if (++seen == k) return i;
	}
	return 0;
}

static size_t max_label_w(void) {
	size_t m = 0;

	for (size_t i = 0; i < TB_MAX_BARS; i++)
		if (g_bar_slots[i].active) {
			size_t n = strlen(g_bar_slots[i].label);
			if (n > m) m = n;
		}
	return m;
}

static void render_bar_body(tbbuf* t, size_t i) {
	tb_slot_t* s = &g_bar_slots[i];
	char msg[1024];
	size_t len;
	int pct = (int)pct_of(s->current, s->total);

	len = build_progress_msg(s->label, max_label_w(), s->current, s->total,
				 tb_slot_chars(s), s->show_eta, msg,
				 sizeof(msg));
	msg[len] = '\0';
	tb_str(t, FP_CR_EL_LINE);
	write_progress_line(t, s->scope_name, s->description, msg, pct);
}

static void draw_bar_at(tbbuf* t, size_t i) {
	size_t line = bar_line_of(i);

	tb_fmt(t, FP_CUD, line);
	render_bar_body(t, i);
	tb_fmt(t, FP_CUU FP_CR, line);
}

static void draw_bar_row_at_row(tbbuf* t, size_t i) {
	tb_fmt(t, FP_CUP_EL_LINE, bar_row_of(i));
	render_bar_body(t, i);
	tb_fmt(t, FP_CUP_CR, g_log_row);
}

static void anchor_scroll(tbbuf* t) {
	size_t below = g_rows - g_log_row;

	if (below > 0) tb_fmt(t, FP_CUD, below);
	tb_str(t, "\n");
	g_log_row = g_rows - g_bar_count;
	for (size_t i = 0; i < TB_MAX_BARS; i++)
		if (g_bar_slots[i].active) draw_bar_row_at_row(t, i);
	tb_fmt(t, FP_CUP_EL_LINE, g_log_row);
}

static void reflow_pin(tbbuf* t) {
	size_t freed = g_rows - g_bar_count;

	for (size_t k = 1; k <= g_bar_count; k++)
		draw_bar_row_at_row(t, active_slot_in_draw_order(k));
	tb_fmt(t, FP_CUP_EL_LINE, freed);
	tb_fmt(t, FP_CUP_CR, g_log_row);
}

static void reflow_bars(tbbuf* t) {
	size_t line;

	for (size_t k = 1; k <= g_bar_count; k++)
		draw_bar_at(t, active_slot_in_draw_order(k));
	line = g_bar_count + 1;
	tb_fmt(t, FP_CUD_EL_CUU, line, line);
}

static void shift_bars_down(tbbuf* t) {
	size_t n = g_bar_count;

	for (size_t i = 0; i < n; i++) tb_str(t, "\n");
	for (size_t k = n; k > 0; k--) {
		render_bar_body(t, active_slot_in_draw_order(k));
		tb_str(t, FP_CUU_ONE);
	}
	tb_str(t, FP_CR_EL_LINE);
}

static void pin_room_for_bar(void) {
	tbbuf t;
	char buf[4096];

	t.b = buf;
	t.cap = sizeof(buf);
	t.pos = 0;
	if (g_log_row > g_rows - g_bar_count) anchor_scroll(&t);
	else
		reflow_pin(&t);
	tb_emit(&t);
}

static bool query_cursor_row(size_t* out_row) {
	struct termios orig, raw;
	struct pollfd pfd;
	char resp[32];
	size_t n = 0;
	ssize_t got;
	bool ok = false;

	*out_row = 0;
	if (!fp_fd_is_tty(STDIN_FILENO)) return false;
	if (tcgetattr(STDIN_FILENO, &orig) != 0) return false;

	raw = orig;
	raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return false;

	fp_out_lock();
	fp_write_fd(STDERR_FILENO, FP_DSR_CURSOR_POS, 4);
	while (n < sizeof(resp)) {
		pfd.fd = STDIN_FILENO;
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, TB_DSR_TIMEOUT_MS) <= 0) break;
		got = read(STDIN_FILENO, resp + n, sizeof(resp) - n);
		if (got <= 0) break;
		n += (size_t)got;
		if (memchr(resp, 'R', n) != NULL) {
			ok = true;
			break;
		}
	}
	fp_out_unlock();

	(void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig);

	if (!ok) return false;

	{
		const char *end, *semi, *digits;
		size_t row = 0;

		if (n < 4 || resp[0] != FP_ESC_CHAR || resp[1] != '[')
			return false;
		end = memchr(resp, 'R', n);
		if (end == NULL) return false;
		semi = memchr(resp + 2, ';', (size_t)(end - (resp + 2)));
		if (semi == NULL) return false;
		digits = resp + 2;
		for (const char* p = digits; p < semi; p++) {
			if (*p < '0' || *p > '9') return false;
			row = row * 10 + (size_t)(*p - '0');
		}
		if (row == 0) return false;
		*out_row = row;
	}
	return true;
}

static bool g_term_dumb = false;
static bool g_term_dumb_set = false;

void fp_set_term_dumb(bool on) {
	g_term_dumb = on;
	g_term_dumb_set = true;
}

static bool term_dumb(void) {
	if (g_term_dumb_set) return g_term_dumb;
	return false;
}

static bool anchor_possible(void) {
	return fp_term_info().is_tty && fp_fd_is_tty(STDIN_FILENO) &&
	       !term_dumb();
}

static void engage_area(void) {
	size_t row;

	g_anchor = true;
	g_pin = false;
	if (!query_cursor_row(&row)) return;
	g_pin = true;
	g_rows = fp_term_rows();
	g_log_row = row;
	{
		char buf[32];
		tbbuf t;

		t.b = buf;
		t.cap = sizeof(buf);
		t.pos = 0;
		if (g_log_row >= g_rows) {
			tb_str(&t, "\n");
			if (g_rows > 0) g_log_row = g_rows - 1;
		}
		tb_fmt(&t, FP_CUP_CR, g_log_row);
		tb_emit(&t);
	}
}

static unsigned bar_register(const char* scope_name, const char* desc,
			     const char* label, uint64_t total,
			     uint64_t current, const unsigned char glyphs[4],
			     bool show_eta) {
	unsigned slot;
	tb_slot_t* s;

	if (g_bar_count >= TB_MAX_BARS) return 0;
	if (!anchor_possible()) return 0;
	if (g_bar_count == 0) engage_area();
	if (!g_anchor) return 0;
	// Bars run in parallel and finish in any order, so the next free slot
	// is not count + 1: that one can still be live.
	{
		size_t idx = 0;

		while (idx < TB_MAX_BARS && g_bar_slots[idx].active) idx++;
		if (idx == TB_MAX_BARS) return 0;
		slot = (unsigned)(idx + 1);
	}
	g_bar_count++;
	s = &g_bar_slots[slot - 1];
	memset(s, 0, sizeof(*s));
	s->active = true;
	s->scope_name = scope_name;
	s->description = desc;
	s->label = label;
	s->total = total;
	s->current = current;
	if (glyphs != NULL) slot_set_glyphs(s, glyphs);
	s->chars = tb_slot_chars(s);
	s->show_eta = show_eta;

	cursor_hide();
	if (g_pin) {
		// The bar itself says the work is running; a "started" line
		// would only have to be replaced by the result later.
		pin_room_for_bar();
		return slot;
	}
	fp_logf(scope_name, desc, FP_LVL_PROGRESS, "%s", label);
	bar_redraw(slot);
	return slot;
}

static void bar_sync(unsigned slot, uint64_t current, uint64_t total) {
	tb_slot_t* s;

	if (slot == 0 || slot > TB_MAX_BARS) return;
	s = &g_bar_slots[slot - 1];
	if (!s->active) return;
	s->current = current;
	s->total = total;
}

static void bar_set_label(unsigned slot, const char* label) {
	if (slot == 0 || slot > TB_MAX_BARS) return;
	if (g_bar_slots[slot - 1].active) g_bar_slots[slot - 1].label = label;
}

static void bar_redraw(unsigned slot) {
	tbbuf t;
	char buf[4096];

	if (slot == 0 || slot > TB_MAX_BARS) return;
	if (!g_bar_slots[slot - 1].active) return;
	t.b = buf;
	t.cap = sizeof(buf);
	t.pos = 0;
	if (g_pin) draw_bar_row_at_row(&t, slot - 1);
	else
		draw_bar_at(&t, slot - 1);
	tb_emit(&t);
}

static void bar_unregister(unsigned slot) {
	tbbuf t;
	char buf[4096];
	size_t idx, line, row;

	if (slot == 0 || slot > TB_MAX_BARS) return;
	idx = slot - 1;
	if (!g_bar_slots[idx].active) return;
	line = bar_line_of(idx);
	row = g_pin ? bar_row_of(idx) : 0;
	g_bar_slots[idx].active = false;
	g_bar_count--;

	t.b = buf;
	t.cap = sizeof(buf);
	t.pos = 0;
	if (g_bar_count == 0) {
		g_anchor = false;
		if (g_pin) {
			tb_fmt(&t, FP_CUP_EL_LINE, row);
			tb_fmt(&t, FP_CUP_CR, g_log_row);
		} else {
			tb_fmt(&t, FP_CUD_EL_CUU, line, line);
		}
		tb_emit(&t);
		fp_set_was_progress_line(false);
		cursor_show();
		return;
	}
	if (g_pin) reflow_pin(&t);
	else
		reflow_bars(&t);
	tb_emit(&t);
}

static void set_osc_progress(bool have_pct, int pct, int state) {
	char buf[32];
	tbbuf t;

	if (!fp_term_info().is_tty) return;
	t.b = buf;
	t.cap = sizeof(buf);
	t.pos = 0;
	tb_str(&t, FP_OSC_PROGRESS);
	tb_fmt(&t, "%d", state);
	if (have_pct) tb_fmt(&t, ";%d", pct);
	tb_str(&t, "\007");
	tb_emit(&t);
}

#define TB_OSC_CLEAR 0
#define TB_OSC_NORMAL 1
#define TB_OSC_ERR 2

static void progress_clear_frame(void) {
	char buf[8];
	tbbuf t;

	if (!fp_get_was_progress_line()) return;
	t.b = buf;
	t.cap = sizeof(buf);
	t.pos = 0;
	if (colors_on()) tb_str(&t, FP_CR_EL_LINE);
	else
		tb_str(&t, "\r");
	tb_emit(&t);
	fp_set_was_progress_line(false);
}

static void print_progress_line_ex(const char* sn, const char* desc,
				   const char* label, uint64_t current,
				   uint64_t total,
				   const unsigned char glyphs[4], bool show_eta,
				   bool force) {
	if (g_one_shot_start_ns == 0) g_one_shot_start_ns = mono_ns();
	if (!force) {
		int64_t now = mono_ns();
		bool final = total > 0 && current >= total;

		if (g_progress_throttle_ms > 0 && !final &&
		    g_last_progress_draw_ns > 0 &&
		    now - g_last_progress_draw_ns <
			    (int64_t)g_progress_throttle_ms * NS_PER_MS)
			return;
		g_last_progress_draw_ns = now;
	}

	if (g_one_shot_slot == 0 && anchor_possible()) {
		if (g_bar_count == 0) engage_area();
		if (g_anchor) {
			unsigned slot;

			size_t free_idx = 0;

			while (free_idx < TB_MAX_BARS &&
			       g_bar_slots[free_idx].active)
				free_idx++;
			if (g_bar_count >= TB_MAX_BARS ||
			    free_idx == TB_MAX_BARS)
				slot = 0;
			else {
				tb_slot_t* s;

				slot = (unsigned)(free_idx + 1);
				g_bar_count++;
				s = &g_bar_slots[slot - 1];
				memset(s, 0, sizeof(*s));
				s->active = true;
				s->one_shot = true;
				s->scope_name = sn;
				s->description = desc;
				s->label = label;
				s->total = total;
				s->current = current;
				if (glyphs != NULL) slot_set_glyphs(s, glyphs);
				s->chars = tb_slot_chars(s);
				s->show_eta = show_eta;
			}
			g_one_shot_slot = slot;
			if (g_one_shot_slot != 0) {
				cursor_hide();
				bar_redraw(g_one_shot_slot);
				set_osc_progress(true,
						 (int)pct_of(current, total),
						 TB_OSC_NORMAL);
				return;
			}
		}
	}
	if (g_one_shot_slot != 0) {
		bar_sync(g_one_shot_slot, current, total);
		bar_redraw(g_one_shot_slot);
		set_osc_progress(true, (int)pct_of(current, total),
				 TB_OSC_NORMAL);
		return;
	}

	{
		char buf[2048];
		tbbuf t;
		char msg[1024];
		char store[4][2];
		size_t len, own, stack;

		own = strlen(label);
		stack = max_label_w();
		len = build_progress_msg(label, own > stack ? own : stack,
					 current, total,
					 chars_from_glyphs(glyphs, store),
					 show_eta, msg, sizeof(msg));
		msg[len] = '\0';
		progress_clear_frame();
		t.b = buf;
		t.cap = sizeof(buf);
		t.pos = 0;
		write_progress_line(&t, sn, desc, msg,
				    (int)pct_of(current, total));
		tb_str(&t, "\r");
		cursor_hide();
		tb_emit(&t);
		fp_set_was_progress_line(true);
	}
	set_osc_progress(true, (int)pct_of(current, total), TB_OSC_NORMAL);
}

static void finish_progress(const char* sn, const char* desc, bool success,
			    int64_t start_ns, const char* msg) {
	int lv = success ? FP_LVL_OKAY : FP_LVL_ERROR;
	char full[FP_MAX_MSG + 64];
	char took[64];
	size_t n;

	took[0] = '\0';
	if (took_suffix(start_ns, took, sizeof(took)) > 0) {
		snprintf(full, sizeof(full), "%s%s", msg, took);
		n = strlen(full);
	} else {
		snprintf(full, sizeof(full), "%s", msg);
		n = strlen(full);
	}
	full[n < sizeof(full) ? n : sizeof(full) - 1] = '\0';

	if (g_anchor) {
		fp_logf(sn, desc, lv, "%s", full);
		set_osc_progress(false, 0, success ? TB_OSC_CLEAR : TB_OSC_ERR);
		return;
	}
	if (fp_get_was_progress_line()) {
		char nl = '\n';
		char buf[8];
		tbbuf t;

		t.b = buf;
		t.cap = sizeof(buf);
		t.pos = 0;
		tb_add(&t, &nl, 1);
		tb_emit(&t);
		fp_set_was_progress_line(false);
		cursor_show();
	}
	fp_logf(sn, desc, lv, "%s", full);
	set_osc_progress(false, 0, success ? TB_OSC_CLEAR : TB_OSC_ERR);
}

void fp_progress_cursor_restore(void) { cursor_show(); }

// Rows the log may use before it meets the pinned bars.
static size_t log_room(void) {
	return g_rows > g_bar_count ? g_rows - g_bar_count : 0;
}

// A wrapped line's rows may have scrolled the screen and covered the bars, and
// the cursor is no longer where g_log_row says.  Ask the terminal where it is,
// put the pin below it, and redraw the bars.
static void repin_after_wrapped(void) {
	tbbuf t;
	char buf[4096];
	size_t row = 0;

	if (!query_cursor_row(&row)) {
		// No answer: stop pinning rather than draw bars over the log.
		g_pin = false;
		return;
	}
	g_log_row = row;
	t.b = buf;
	t.cap = sizeof(buf);
	t.pos = 0;
	if (g_log_row > log_room()) {
		size_t scroll = g_log_row - log_room();

		tb_fmt(&t, FP_CUP_CR, g_rows);
		while (scroll-- > 0) tb_str(&t, "\n");
		g_log_row = log_room();
	}
	for (size_t i = 0; i < TB_MAX_BARS; i++)
		if (g_bar_slots[i].active) draw_bar_row_at_row(&t, i);
	tb_fmt(&t, FP_CUP_CR, g_log_row);
	write_out_locked(&t);
}

void fp_anchor_after_log_line(size_t rows) {
	tbbuf t;
	char buf[4096];

	if (rows == 0) rows = 1;
	if (!g_anchor) return;
	if (g_pin) {
		g_log_row += rows;
		if (g_bar_count == 0 || g_log_row <= log_room()) return;
		if (rows > 1) {
			repin_after_wrapped();
			return;
		}
		t.b = buf;
		t.cap = sizeof(buf);
		t.pos = 0;
		anchor_scroll(&t);
		write_out_locked(&t);
		return;
	}
	if (g_bar_count == 0) return;
	for (size_t i = 0; i < rows; i++) {
		t.b = buf;
		t.cap = sizeof(buf);
		t.pos = 0;
		shift_bars_down(&t);
		write_out_locked(&t);
	}
}

static void fill_buf(char* dst, size_t dstlen, const char* src) {
	size_t n;

	if (src == NULL) src = "";
	n = strlen(src);
	if (n > dstlen - 1) n = dstlen - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static tb_bar_chars_t chars_from_glyphs(const unsigned char glyphs[4],
					char store[4][2]) {
	tb_bar_chars_t c = tb_default_chars();

	for (int i = 0; i < 4; i++) {
		store[i][0] = glyphs == NULL ? '\0' : (char)glyphs[i];
		store[i][1] = '\0';
	}
	if (glyphs == NULL) return c;
	if (glyphs[0] != 0) c.begin = store[0];
	if (glyphs[1] != 0) c.fill = store[1];
	if (glyphs[2] != 0) c.empty = store[2];
	if (glyphs[3] != 0) c.end = store[3];
	return c;
}

// Bars without a pinned area (no tty, or more than TB_MAX_BARS) fall back to
// plain progress lines, and then only one of them speaks at a time.
static fp_bar_t* g_fp_owner;

bool fp_bar_can_pin(void) {
	bool ok;

	fp_out_lock();
	ok = anchor_possible() && g_bar_count < TB_MAX_BARS;
	fp_out_unlock();
	return ok;
}

bool fp_bar_pinned(const fp_bar_t* b) { return b != NULL && b->slot != 0; }

fp_bar_t fp_bar_init(const char* scope, const char* desc, const char* label,
		     uint64_t total) {
	fp_bar_t b;

	memset(&b, 0, sizeof(b));
	fill_buf(b.scope, sizeof(b.scope), scope);
	fill_buf(b.desc, sizeof(b.desc), desc);
	fill_buf(b.label, sizeof(b.label), label);
	b.total = total;
	b.current = 0;
	b.claimed = 1;
	return b;
}

void fp_bar_set_label(fp_bar_t* b, const char* label) {
	if (b == NULL) return;
	fp_out_lock();
	fill_buf(b->label, sizeof(b->label), label);
	bar_set_label(b->slot, b->label);
	fp_out_unlock();
}

void fp_bar_set_throttle(fp_bar_t* b, uint32_t ms) {
	if (b != NULL) b->throttle_ms = ms;
}

void fp_bar_update(fp_bar_t* b, uint64_t current) {
	int64_t now;
	bool final;

	if (b == NULL || b->finished) return;
	if (fp_record_progress(b->label, current, b->total)) {
		b->current = current;
		return;
	}
	// Pull threads draw their own bars at the same time.
	fp_out_lock();
	b->current = current < b->total ? current : b->total;
	now = mono_ns();
	if (b->start_ns == 0) b->start_ns = now;
	final = b->current >= b->total;

	if (b->slot == 0 && !b->fallback) {
		b->slot = bar_register(b->scope, b->desc, b->label, b->total,
				       b->current, NULL, b->show_eta != 0);
		if (b->slot == 0) b->fallback = 1;
		b->last_draw_ns = now;
		if (b->slot != 0) {
			fp_out_unlock();
			return;
		}
	}

	if (b->fallback) {
		if (g_fp_owner != NULL && g_fp_owner != b) {
			fp_out_unlock();
			return;
		}
		g_fp_owner = b;
		if (!final && b->throttle_ms > 0 && b->last_draw_ns > 0 &&
		    now - b->last_draw_ns <
			    (int64_t)b->throttle_ms * NS_PER_MS) {
			fp_out_unlock();
			return;
		}
		b->last_draw_ns = now;
		print_progress_line_ex(b->scope, b->desc, b->label, b->current,
				       b->total, NULL, b->show_eta != 0, true);
		fp_out_unlock();
		return;
	}

	bar_sync(b->slot, b->current, b->total);
	if (final || b->throttle_ms == 0 || b->last_draw_ns == 0 ||
	    now - b->last_draw_ns >= (int64_t)b->throttle_ms * NS_PER_MS) {
		b->last_draw_ns = now;
		bar_redraw(b->slot);
	}
	fp_out_unlock();
}

void fp_bar_dismiss(fp_bar_t* b) {
	if (b == NULL) return;
	fp_out_lock();
	b->finished = 1;
	if (b->slot != 0) {
		bar_unregister(b->slot);
		b->slot = 0;
	}
	if (g_fp_owner == b) g_fp_owner = NULL;
	if (g_bar_count == 0) set_osc_progress(false, 0, TB_OSC_CLEAR);
	fp_out_unlock();
}

void fp_bar_finishf(fp_bar_t* b, int success, const char* fmt, ...) {
	char msg[FP_MAX_MSG];
	va_list ap;

	if (b == NULL) return;
	if (fmt == NULL) {
		msg[0] = '\0';
	} else {
		va_start(ap, fmt);
		vsnprintf(msg, sizeof(msg), fmt, ap);
		va_end(ap);
	}
	fp_out_lock();
	b->finished = 1;
	if (b->slot != 0) {
		bar_unregister(b->slot);
		b->slot = 0;
	}
	if (g_fp_owner == b) g_fp_owner = NULL;
	{
		char took[48];
		size_t used = strlen(msg);

		// Like a spinner's result: how long it took, once it is worth
		// saying.
		if (success && b->start_ns != 0 &&
		    mono_ns() - b->start_ns >= NS_PER_S &&
		    took_suffix(b->start_ns, took, sizeof(took)) > 0 &&
		    used + strlen(took) < sizeof(msg))
			(void)strlcat(msg, took, sizeof(msg));
	}
	fp_logf(b->scope, b->desc, success ? FP_LVL_OKAY : FP_LVL_ERROR, "%s",
		msg);
	if (g_bar_count == 0)
		set_osc_progress(false, 0, success ? TB_OSC_CLEAR : TB_OSC_ERR);
	fp_out_unlock();
}

static void print_spinner_line(const char* sn, const char* desc,
			       const char* label, unsigned frame) {
	char msg[FP_MAX_MSG];
	size_t pos = 0;
	tbbuf t;
	char buf[FP_MAX_MSG + 256];
	size_t idx = frame % TB_SPINNER_NFRAMES;

	pos = (size_t)snprintf(msg, sizeof(msg), "%s ", label);
	if (pos >= sizeof(msg)) pos = sizeof(msg) - 1;
	if (pos + 3 < sizeof(msg)) {
		memcpy(msg + pos, TB_SPINNER_FRAMES + idx * 3, 3);
		pos += 3;
	}
	msg[pos] = '\0';

	progress_clear_frame();
	t.b = buf;
	t.cap = sizeof(buf);
	t.pos = 0;
	write_progress_line(&t, sn, desc, msg, 127);
	tb_str(&t, "\r");
	cursor_hide();
	tb_emit(&t);
	fp_set_was_progress_line(true);
}

fp_spin_t fp_spin_init(const char* scope, const char* desc, const char* label) {
	fp_spin_t s;

	memset(&s, 0, sizeof(s));
	fill_buf(s.scope, sizeof(s.scope), scope);
	fill_buf(s.desc, sizeof(s.desc), desc);
	fill_buf(s.label, sizeof(s.label), label);
	s.frame = 0;
	s.done = 0;
	s.start_ns = 0;
	return s;
}

void fp_spin_tick(fp_spin_t* s) {
	if (s == NULL || s->done) return;
	if (fp_record_active()) return;
	if (s->start_ns == 0) s->start_ns = mono_ns();
	s->frame = (unsigned)((s->frame + 1) % TB_SPINNER_NFRAMES);
	print_spinner_line(s->scope, s->desc, s->label, s->frame);
}

void fp_spin_dismiss(fp_spin_t* s) {
	if (s == NULL) return;
	s->done = 1;
	fp_out_lock();
	progress_clear_frame();
	fp_out_unlock();
}

void fp_spin_finishf(fp_spin_t* s, int success, const char* fmt, ...) {
	char msg[FP_MAX_MSG];
	va_list ap;

	if (s == NULL) return;
	if (fmt == NULL) {
		msg[0] = '\0';
	} else {
		va_start(ap, fmt);
		vsnprintf(msg, sizeof(msg), fmt, ap);
		va_end(ap);
	}
	s->done = 1;
	finish_progress(s->scope, s->desc, success != 0, s->start_ns, msg);
}
