#include "evui.h"
#include "fplog.h"
#include "fplog_int.h"
#include "translations.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define ESC_WAIT_MS 50

#define MAX_TABLE_COLS 32

static void tui_title_line(const char* msg) {
	fp_logf("menu", "", FP_LEVEL_INFO, "%s", msg);
}

struct tui_raw {
	struct termios saved;
	bool active;
};

static bool tui_raw_enter(struct tui_raw* r) {
	struct termios t;

	// Progress hides the cursor while it draws; take it back.
	fp_progress_cursor_restore();

	if (tcgetattr(STDIN_FILENO, &t) != 0) return false;
	r->saved = t;
	t.c_lflag &= (tcflag_t) ~(ICANON | ECHO | ISIG | IEXTEN);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) != 0) return false;
	r->active = true;
	return true;
}

static void tui_raw_leave(struct tui_raw* r) {
	if (!r->active) return;
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &r->saved);
	r->active = false;
}

#define tui_is_tty() fp_fd_is_tty(STDIN_FILENO)

static bool tui_raw_begin(struct tui_raw* r) {
	r->active = false;
	return tui_is_tty() && tui_raw_enter(r);
}

enum tui_key_kind {
	TUI_K_UNKNOWN = 0,
	TUI_K_UP,
	TUI_K_DOWN,
	TUI_K_LEFT,
	TUI_K_RIGHT,
	TUI_K_HOME,
	TUI_K_END,
	TUI_K_PAGE_UP,
	TUI_K_PAGE_DOWN,
	TUI_K_DELETE,
	TUI_K_INSERT,
	TUI_K_ENTER,
	TUI_K_TAB,
	TUI_K_BACKSPACE,
	TUI_K_ESCAPE,
	TUI_K_SPACE,
	TUI_K_CHAR,
	TUI_K_FUNC
};

struct tui_key {
	enum tui_key_kind kind;
	unsigned char c;
};

static int tui_read_byte(void) {
	unsigned char b;
	ssize_t n;

	for (;;) {
		n = read(STDIN_FILENO, &b, 1);
		if (n == 1) return (int)b;
		if (n < 0 && errno == EINTR) continue;
		return -1;
	}
}

#define TUI_SEQ_TIMEOUT (-1)
#define TUI_SEQ_EOF (-2)

static int tui_read_seq_byte(void) {
	struct pollfd fds = {STDIN_FILENO, POLLIN, 0};
	int c;

	if (poll(&fds, 1, ESC_WAIT_MS) <= 0) return TUI_SEQ_TIMEOUT;
	c = tui_read_byte();
	return c < 0 ? TUI_SEQ_EOF : c;
}

// Same numbering xterm uses for "CSI <n> ~".
static enum tui_key_kind tui_fn_from_code(unsigned num, unsigned char* fn) {
	switch (num) {
	case 2:
		return TUI_K_INSERT;
	case 3:
		return TUI_K_DELETE;
	case 5:
		return TUI_K_PAGE_UP;
	case 6:
		return TUI_K_PAGE_DOWN;
	}
	if (num == 1 || num == 4 || (num >= 7 && num <= 9))
		*fn = (unsigned char)num;
	else if (num >= 11 && num <= 15)
		*fn = (unsigned char)(num - 10);
	else if (num >= 17 && num <= 21)
		*fn = (unsigned char)(num - 11);
	else if (num == 23 || num == 24)
		*fn = (unsigned char)(num - 12);
	else
		return TUI_K_UNKNOWN;
	return TUI_K_FUNC;
}

static enum tui_key_kind tui_read_function_key(unsigned char first_digit,
					       unsigned char* fn) {
	unsigned num = (unsigned)(first_digit - '0');

	for (;;) {
		int d = tui_read_seq_byte();

		if (d == TUI_SEQ_TIMEOUT || d == TUI_SEQ_EOF)
			return TUI_K_UNKNOWN;
		if (d == '~') break;
		if (d < '0' || d > '9') return TUI_K_UNKNOWN;
		num = num * 10 + (unsigned)(d - '0');
		if (num > 999) return TUI_K_UNKNOWN;
	}
	return tui_fn_from_code(num, fn);
}

static int tui_read_bracket_seq(struct tui_key* k) {
	static const char finals[] = "ABCDHF";
	static const enum tui_key_kind kinds[] = {
		TUI_K_UP,   TUI_K_DOWN, TUI_K_RIGHT,
		TUI_K_LEFT, TUI_K_HOME, TUI_K_END,
	};
	int c = tui_read_seq_byte();
	const char* p;

	if (c == TUI_SEQ_TIMEOUT || c == TUI_SEQ_EOF) return -1;
	if (c > 0 && (p = strchr(finals, c)) != NULL)
		k->kind = kinds[p - finals];
	else if (c >= '0' && c <= '9')
		k->kind = tui_read_function_key((unsigned char)c, &k->c);
	else
		k->kind = TUI_K_UNKNOWN;
	return 0;
}

static int tui_read_o_seq(struct tui_key* k) {
	int c = tui_read_seq_byte();

	k->kind = TUI_K_UNKNOWN;
	if (c < 0) return (c == TUI_SEQ_EOF) ? -1 : 0;
	if (c >= 'P' && c <= 'S') {
		k->kind = TUI_K_FUNC;
		k->c = (unsigned char)(c - 'P' + 1);
	}
	return 0;
}

static int tui_read_escape_seq(struct tui_key* k) {
	int c = tui_read_seq_byte();

	if (c == TUI_SEQ_TIMEOUT) {
		k->kind = TUI_K_ESCAPE;
		return 0;
	}
	if (c == TUI_SEQ_EOF) return -1;
	switch (c) {
	case '[':
		return tui_read_bracket_seq(k);
	case 'O':
		return tui_read_o_seq(k);
	default:
		k->kind = TUI_K_UNKNOWN;
		return 0;
	}
}

static int tui_read_key(struct tui_key* k) {
	int c = tui_read_byte();

	if (c < 0) return -1;
	switch (c) {
	case 0x0d:
	case 0x0a:
		k->kind = TUI_K_ENTER;
		return 0;
	case 0x09:
		k->kind = TUI_K_TAB;
		return 0;
	case 0x08:
	case 0x7f:
		k->kind = TUI_K_BACKSPACE;
		return 0;
	case 0x1b:
		return tui_read_escape_seq(k);
	}
	k->kind = (c >= 0x20 || c == 0x03 || c == 0x04) ? TUI_K_CHAR
							: TUI_K_UNKNOWN;
	k->c = (unsigned char)c;
	return 0;
}

// Esc, ^C and ^D all mean "give up" in every prompt.
static bool tui_is_cancel(const struct tui_key* k) {
	return k->kind == TUI_K_ESCAPE ||
	       (k->kind == TUI_K_CHAR && (k->c == 0x03 || k->c == 0x04));
}

// vi keys alias the arrows; only for prompts that take no text.
static void tui_vi_key(struct tui_key* k) {
	if (k->kind != TUI_K_CHAR) return;
	switch (k->c) {
	case 'k':
		k->kind = TUI_K_UP;
		break;
	case 'j':
		k->kind = TUI_K_DOWN;
		break;
	case 'h':
		k->kind = TUI_K_LEFT;
		break;
	case 'l':
		k->kind = TUI_K_RIGHT;
		break;
	}
}

static int tui_sink_fd = 2;

static void tui_wmem(const char* s, size_t n) {
	if (tui_sink_fd == 2) fp_write_stderr(s, n);
	else
		fp_write_fd(tui_sink_fd, s, n);
}

static void tui_wstr(const char* s) { tui_wmem(s, strlen(s)); }

static void tui_wch(char c) { tui_wmem(&c, 1); }

static void tui_sgr(bool on, const char* seq) {
	if (on) tui_wstr(seq);
}

static void tui_cuu(size_t n) {
	char seq[24];
	int k = snprintf(seq, sizeof(seq), FP_CUU, n);

	if (k > 0) tui_wmem(seq, (size_t)k);
}

static void tui_wspaces(size_t n) {
	size_t i;

	for (i = 0; i < n; i++) tui_wch(' ');
}

static size_t tui_num_width(size_t n) {
	size_t w = 1;
	size_t v = n;

	while (v >= 10) {
		v /= 10;
		w++;
	}
	return w;
}

static void tui_write_numbered_item(size_t index, size_t nw, const char* item) {
	char num[24];
	int n = snprintf(num, sizeof(num), "%zu", index + 1);

	if (n < 0) n = 0;
	if ((size_t)n > nw) nw = (size_t)n;
	tui_wspaces(nw - (size_t)n);
	tui_wmem(num, (size_t)n);
	tui_wstr(") ");
	tui_wstr(item);
}

static void tui_draw_item_block(const char* const* items, size_t n, size_t sel,
				bool color) {
	size_t nw = tui_num_width(n);
	size_t i;

	for (i = 0; i < n; i++) {
		bool selected = (i == sel);

		tui_wstr(selected ? "> " : "  ");
		tui_sgr(selected && color, FP_SGR_REVERSE);
		tui_write_numbered_item(i, nw, items[i] ? items[i] : "");
		tui_sgr(selected && color, FP_SGR_RESET);
		tui_wch('\n');
	}
}

static void tui_draw_menu(const char* const* items, size_t n, size_t sel,
			  bool move_up) {
	bool color = fp_get_color();

	fp_out_lock();
	if (move_up) {
		tui_cuu(n);
		tui_wstr(FP_CR_ED_DOWN);
	}
	tui_draw_item_block(items, n, sel, color);
	fp_out_unlock();
}

static bool tui_digit_index(unsigned char c, size_t n, size_t* out) {
	size_t d;

	if (c < '1' || c > '9') return false;
	d = (size_t)(c - '0');
	if (d > n) return false;
	*out = d - 1;
	return true;
}

static int tui_run_menu(const char* title, const char* const* items, size_t n) {
	struct tui_raw raw;
	struct tui_key k;
	size_t sel = 0;
	int rc;

	if (n == 0 || items == NULL || !tui_raw_begin(&raw)) return -1;

	tui_title_line(title);
	tui_draw_menu(items, n, sel, false);

	for (;;) {
		size_t next = sel;
		size_t j;

		if (tui_read_key(&k) != 0 || tui_is_cancel(&k)) goto cancel;
		tui_vi_key(&k);
		switch (k.kind) {
		case TUI_K_UP:
			next = (sel == 0) ? n - 1 : sel - 1;
			break;
		case TUI_K_DOWN:
			next = (sel + 1) % n;
			break;
		case TUI_K_ENTER:
			goto pick;
		case TUI_K_CHAR:
			if (k.c == 'z' || k.c == 'Z') goto pick;
			if (k.c == 'x' || k.c == 'X') goto cancel;
			if (tui_digit_index(k.c, n, &j)) next = j;
			break;
		default:
			break;
		}
		if (next != sel) {
			sel = next;
			tui_draw_menu(items, n, sel, true);
		}
	}
pick:
	rc = (int)sel;
	goto out;
cancel:
	rc = -1;
out:
	tui_raw_leave(&raw);
	return rc;
}

static void tui_write_confirm_head(const char* question, bool dflt) {
	static const struct {
		const char *sgr, *label;
	} opt[] = {
		{FP_SGR_YES, "yes"},
		{FP_SGR_NO_SOFT, "no"},
	};
	int i;

	fp_out_lock();
	tui_wstr(FP_SGR_BOLD);
	tui_wstr(question);
	tui_wstr(" " FP_SGR_RESET "[");
	for (i = 0; i < 2; i++) {
		if (i) tui_wch('/');
		tui_wstr(opt[i].sgr);
		// The default option is capitalised
		tui_wch(opt[i].label[0] & ~((i != dflt) * 0x20));
		tui_wstr(opt[i].label + 1);
		tui_wstr(FP_SGR_RESET);
	}
	tui_wstr("] ");
	fp_out_unlock();
}

// Once the answer is known the "[Yes/no]" options have done their job: the
// prompt line becomes the question and the answer that was given.
static void tui_settle_confirm(const char* question, size_t head_cols,
			       bool yes) {
	size_t cols = fp_term_cols();
	size_t up = (cols > 0 && head_cols > 0) ? (head_cols - 1) / cols : 0;
	bool color = fp_get_color();

	fp_out_lock();
	if (up > 0) tui_cuu(up);
	tui_wstr(FP_CR_ED_DOWN);
	tui_sgr(color, FP_SGR_BOLD);
	tui_wstr(question);
	tui_sgr(color, FP_SGR_RESET);
	tui_wch(' ');
	tui_sgr(color, yes ? FP_SGR_YES : FP_SGR_NO);
	tui_wstr(yes ? "Yes" : "No");
	tui_sgr(color, FP_SGR_RESET);
	tui_wch('\n');
	fp_out_unlock();
}

static bool tui_settle_leave(struct tui_raw* raw, const char* question,
			     size_t head_cols, bool yes) {
	tui_settle_confirm(question, head_cols, yes);
	tui_raw_leave(raw);
	return yes;
}

static size_t tui_confirm_head_cols(const char* question) {
	return fp_visible_width(question) + 1 + 8 + 1;
}

// Maps a key to an answer; false if the key is not one.
static bool tui_confirm_key(const struct tui_key* k, bool on_enter,
			    bool on_cancel, bool* ans) {
	if (k->kind == TUI_K_ENTER) *ans = on_enter;
	else if (tui_is_cancel(k))
		*ans = on_cancel;
	else if (k->kind == TUI_K_CHAR && (k->c == 'y' || k->c == 'Y'))
		*ans = true;
	else if (k->kind == TUI_K_CHAR && (k->c == 'n' || k->c == 'N'))
		*ans = false;
	else
		return false;
	return true;
}

static bool tui_confirm_default(const char* question, bool dflt) {
	struct tui_raw raw;
	struct tui_key k;
	bool ans;

	if (!tui_raw_begin(&raw)) return dflt;

	tui_write_confirm_head(question, dflt);

	// Escape, Ctrl-C and Ctrl-D mean "no", and so does the end of input,
	// whatever the default is: a prompt that defaults to yes must still let
	// the user say no.
	do {
		if (tui_read_key(&k) != 0) {
			tui_raw_leave(&raw);
			return false;
		}
	} while (!tui_confirm_key(&k, dflt, false, &ans));
	return tui_settle_leave(&raw, question, tui_confirm_head_cols(question),
				ans);
}

struct tui_layout {
	size_t widths[MAX_TABLE_COLS];
	size_t ncols;
	size_t total;
	size_t indent;
};

static size_t tui_cell_width(const char* s) { return strlen(s); }

static size_t tui_table_total(const struct tui_layout* lay) {
	size_t t = 0;
	size_t c;

	for (c = 0; c < lay->ncols; c++) t += lay->widths[c];
	return t + 2 * (lay->ncols - 1);
}

static size_t tui_row_cells(const char* const* row, size_t max) {
	size_t c;

	if (row == NULL) return 0;
	for (c = 0; c < max; c++) {
		if (row[c] == NULL) break;
	}
	return c;
}

static void tui_table_layout(const char* const* headers, size_t ncols,
			     const char* const* const* rows, size_t nrows,
			     size_t max_width, struct tui_layout* lay) {
	size_t nc = ncols;
	size_t guard;
	size_t c, r;

	memset(lay, 0, sizeof(*lay));

	if (nc > MAX_TABLE_COLS) nc = MAX_TABLE_COLS;
	lay->ncols = nc;
	if (nc == 0) return;

	for (c = 0; c < nc; c++) {
		if (headers != NULL && headers[c] != NULL)
			lay->widths[c] = tui_cell_width(headers[c]);
	}
	for (r = 0; r < nrows; r++) {
		size_t cells = tui_row_cells(rows[r], nc);

		for (c = 0; c < cells; c++) {
			size_t w = tui_cell_width(rows[r][c]);

			if (w > lay->widths[c]) lay->widths[c] = w;
		}
	}

	lay->total = tui_table_total(lay);
	for (guard = 0; lay->total > max_width && guard < 1000; guard++) {
		size_t widest = 0;

		for (c = 1; c < nc; c++) {
			if (lay->widths[c] > lay->widths[widest]) widest = c;
		}
		if (lay->widths[widest] <= 1) break;
		lay->widths[widest] -= 1;
		lay->total = tui_table_total(lay);
	}
}

static size_t tui_center_indent(size_t total, size_t cap) {
	return (total >= cap) ? 0 : (cap - total) / 2;
}

static size_t tui_utf8_truncate(const char* s, size_t len, size_t width) {
	size_t n;

	if (len <= width) return len;
	if (width == 0) return 0;
	n = width - 1;
	while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n -= 1;
	return n;
}

static void tui_write_cell(const char* s, size_t width, bool color) {
	size_t len = strlen(s);
	size_t t = tui_utf8_truncate(s, len, width);
	size_t pad = width - t - ((t < len) ? 1 : 0);

	tui_wmem(s, t);
	if (t < len) {
		tui_sgr(color, FP_SGR_TRUNC);
		tui_wch('~');
		tui_sgr(color, FP_SGR_RESET);
	}
	tui_wspaces(pad);
}

static void tui_write_row(const struct tui_layout* lay, const char* const* row,
			  bool is_header, bool color) {
	const fp_level_t* hi = fp_level_style(FP_LVL_INFO);
	size_t ncell = tui_row_cells(row, lay->ncols);
	size_t c;
	char attr[32];

	tui_wspaces(lay->indent);
	tui_sgr(is_header && color, FP_SGR_TABLE_HEAD);
	for (c = 0; c < lay->ncols; c++) {
		const char* cell = (c < ncell) ? row[c] : "";
		bool first = !is_header && c == 0 && color;

		if (first) {
			int m = snprintf(attr, sizeof(attr), FP_SGR_DIM_FG256,
					 hi->bg);

			if (m > 0 && (size_t)m < sizeof(attr)) tui_wstr(attr);
		}
		tui_write_cell(cell, lay->widths[c], color);
		tui_sgr(first, FP_SGR_RESET);
		if (c + 1 < lay->ncols) tui_wstr("  ");
	}
	tui_sgr(is_header && color, FP_SGR_RESET);
	tui_wch('\n');
}

static const char* tui_str0(const char* s) { return s ? s : ""; }

bool fp_prompt_confirm_default(const char* question, bool dflt) {
	return tui_confirm_default(tui_str0(get_string(question)), dflt);
}

int fp_prompt_menu(const char* title, const char* const* items, size_t n) {
	return tui_run_menu(tui_str0(get_string(title)), items, n);
}

void fp_print_table(const char* const* headers, size_t ncols,
		    const char* const* const* rows, size_t nrows,
		    fp_table_sink_t sink) {
	struct tui_layout lay;
	size_t cap;
	size_t r;
	bool color;

	// A worker has no terminal to draw on, so a table goes to the requester as
	// headers and cells and the client draws it as it draws its own; a sink
	// that refuses tables (or one too big to send) gets plain rows instead.
	if (fp_record_active()) {
		if (fp_record_table(headers, ncols, rows, nrows)) return;
		for (r = 0; r < nrows; r++) {
			char line[FP_MAX_MSG];
			size_t pos = 0;
			line[0] = '\0';
			for (size_t c = 0; c < ncols && pos < sizeof(line);
			     c++) {
				int k = snprintf(line + pos, sizeof(line) - pos,
						 "%s%s", c ? "  " : "",
						 rows[r][c] ? rows[r][c] : "");
				if (k < 0) break;
				pos += (size_t)k;
			}
			(void)fp_record_line(FP_LEVEL_INFO, line);
		}
		return;
	}

	cap = fp_term_cols();
	if (cap < 8) cap = 8;
	color = fp_get_color();

	// Translate header labels; the cell contents are data.
	const char* theaders[ncols];
	for (size_t _i = 0; _i < ncols; _i++)
		theaders[_i] = headers != NULL ? get_string(headers[_i]) : NULL;
	if (headers != NULL) headers = theaders;

	tui_table_layout(headers, ncols, rows, nrows, cap, &lay);
	if (lay.ncols == 0) return;
	lay.indent = tui_center_indent(lay.total, cap);

	fp_out_lock();
	tui_sink_fd = (sink == FP_TABLE_OUTPUT) ? 1 : 2;
	if (headers != NULL) tui_write_row(&lay, headers, true, color);
	for (r = 0; r < nrows; r++) tui_write_row(&lay, rows[r], false, color);
	tui_sink_fd = 2;
	fp_out_unlock();
}
