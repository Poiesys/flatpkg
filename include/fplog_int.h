#ifndef FPLOG_INT_H
#define FPLOG_INT_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The left column is the app id plus the emitting subsystem, not a place for a
// sentence: the message is the content.
#define FP_LEFT_W 14
#define FP_BADGE_TEXT_W 7
#define FP_BADGE_W (FP_BADGE_TEXT_W + 2)
#define FP_COL_GAP 2
#define FP_MAX_MSG 512
#define FP_MIN_OVERHEAD (FP_LEFT_W + FP_COL_GAP + FP_BADGE_W + FP_COL_GAP)

#define FP_SCOPE_W FP_LEFT_W

static inline size_t fp_msg_avail(size_t cols) {
	return cols > FP_MIN_OVERHEAD ? cols - FP_MIN_OVERHEAD : 1;
}

#define FP_TERM_FALLBACK_COLS 80
#define FP_TERM_FALLBACK_ROWS 24
#define FP_TERM_MIN_COLS 40
#define FP_TERM_MIN_ROWS 8

// Terminal control sequences: the renderers emit escapes by hand, so each is
// named once here; a second spelling is how a byte drifts from the captured
// golden.
#define FP_ESC "\033"
#define FP_CSI FP_ESC "["
#define FP_OSC FP_ESC "]"
#define FP_BEL "\007"
#define FP_CR "\r"
#define FP_ESC_CHAR '\033'

// SGR with no runtime argument: FP_SGR(0) is "\033[0m".
#define FP_SGR(p) FP_CSI #p "m"

#define FP_SGR_RESET FP_SGR(0)
#define FP_SGR_BOLD FP_SGR(1)
#define FP_SGR_DIM FP_SGR(2)
#define FP_SGR_REVERSE FP_SGR(7)
// The fixed SGR palette: YES/NO are the settled answers and option labels,
// NO_SOFT the confirm head's unpopped alternative, TRUNC a clipped cell, and
// TABLE_HEAD the light-on-dark header row.
#define FP_SGR_YES FP_CSI "1;38;5;46m"
#define FP_SGR_NO FP_CSI "1;38;5;196m"
#define FP_SGR_NO_SOFT FP_CSI "0;38;5;196m"
#define FP_SGR_TRUNC FP_CSI "38;5;196m"
#define FP_SGR_TABLE_HEAD FP_CSI "1;38;5;252;48;5;236m"

// The same, with a colour taken from the level table at runtime.
#define FP_SGR_FG256 FP_CSI "38;5;%dm"
#define FP_SGR_DIM_FG256 FP_CSI "2;38;5;%dm"
#define FP_SGR_BOLD_FG256 FP_CSI "1;38;5;%dm"
#define FP_SGR_BADGE_OPEN FP_CSI "1%s;38;5;%d;48;5;%dm["
// The progress bar's badge has no fatal variant, so there is no %s to fill.
#define FP_SGR_BAR_BADGE_OPEN FP_CSI "1;38;5;%d;48;5;%dm["

#define FP_CUU FP_CSI "%zuA"
#define FP_CUD FP_CSI "%zuB"
#define FP_CUU_ONE FP_CSI "1A"
#define FP_CHA FP_CSI "%zuG"
#define FP_CUP FP_CSI "%zu;1H"

#define FP_EL_LINE FP_CSI "2K"
#define FP_EL_TO_END FP_CSI "K"
#define FP_ED_DOWN FP_CSI "J"

#define FP_CR_EL_LINE FP_CR FP_EL_LINE
#define FP_CR_EL_TO_END FP_CR FP_EL_TO_END
#define FP_CR_ED_DOWN FP_CR FP_ED_DOWN
#define FP_CR_CHA FP_CR FP_CHA
#define FP_CUP_CR FP_CUP FP_CR
#define FP_CUP_EL_LINE FP_CUP FP_EL_LINE
// Down N rows, clear the line there, then back up.
#define FP_CUD_EL_CUU FP_CUD FP_CR FP_EL_LINE FP_CUU

#define FP_DSR_CURSOR_POS FP_CSI "6n"
#define FP_OSC_PROGRESS FP_OSC "9;4;"
// DECTCEM: some terminals animate the hardware cursor themselves, and a
// blinking block dragged around the progress area is a second animation.
#define FP_CSI_HIDE_CURSOR FP_CSI "?25l"
#define FP_CSI_SHOW_CURSOR FP_CSI "?25h"

typedef struct {
	size_t cols;
	size_t rows;
	bool is_tty;
} fp_term_t;

fp_term_t fp_term_info(void);
size_t fp_term_cols(void);
size_t fp_term_rows(void);
bool fp_fd_is_tty(int fd);

void fp_out_lock(void);
void fp_out_unlock(void);
void fp_write_fd(int fd, const char* s, size_t n);
void fp_write_stderr(const char* s, size_t n);

typedef struct {
	const char* name;
	int bg;
	int msg;
} fp_level_t;

extern const fp_level_t FP_LEVELS[];
extern const int FP_NLEVELS;

#define FP_LVL_DEBUG 0
#define FP_LVL_INFO 1
#define FP_LVL_OKAY 2
#define FP_LVL_WARN 3
#define FP_LVL_ERROR 4
#define FP_LVL_FATAL 5
#define FP_LVL_SKIPPED 6
#define FP_LVL_RESTART 7
#define FP_LVL_DOWN 8
#define FP_LVL_PROGRESS 9

const fp_level_t* fp_level_style(int level);

void fp_badge_text(int level, char* out);

bool fp_bar_held(void);
void fp_bar_release(void);

void fp_set_was_progress_line(bool v);
bool fp_get_was_progress_line(void);

// tb_progress: put the terminal cursor back if progress hid it, so a prompt
// never appears with the cursor still hidden from a run that ended elsewhere.
void fp_progress_cursor_restore(void);

size_t fp_visible_width(const char* s);
size_t fp_utf8_prefix(const char* s, size_t budget);

#endif
