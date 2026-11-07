#ifndef FPLOG_H
#define FPLOG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FP_LEVEL_DEBUG 0
#define FP_LEVEL_INFO 1
#define FP_LEVEL_OKAY 2
#define FP_LEVEL_WARN 3
#define FP_LEVEL_ERROR 4
#define FP_LEVEL_FATAL 5
#define FP_LEVEL_SKIPPED 6
#define FP_LEVEL_RESTART 7
#define FP_LEVEL_DOWN 8

#define FP_MAX_MSG 512

// Column geometry. FP_SCOPE_W, FP_BADGE_W and FP_COL_GAP come from
// fplog_int.h with the ported implementation, so there is one definition.
#include "fplog_int.h"

void fp_set_color(bool on);
bool fp_get_color(void);
// Whether TERM is `dumb`; set by cli.c from the environment.
void fp_set_term_dumb(bool on);

// Verbose mode. When off, fp_*_v() and the DEBUG level are no-ops.
void fp_set_verbose(bool on);
bool fp_get_verbose(void);

void fp_set_logfmt_fd(int fd);

void fp_set_terminal_size(int rows, int cols);

bool fp_is_tty(void);

void fp_set_prefix(const char* prefix);
const char* fp_get_prefix(void);

// Emit one line. `scope` is the left column, `desc` an optional second field
// (usually ""), `level` an FP_LEVEL_*. NULL scope/desc/msg are treated as "".
void fp_logf(const char* scope, const char* desc, int level, const char* fmt,
	     ...) __attribute__((format(printf, 4, 5)));
void fp_vlogf(const char* scope, const char* desc, int level, const char* fmt,
	      va_list ap);

typedef struct {
	char scope[64];
	char desc[256];
} fp_logger_t;

fp_logger_t fp_logger_init(const char* scope, const char* desc);

void fp_logger_infof(const fp_logger_t* l, const char* fmt, ...)
	__attribute__((format(printf, 2, 3)));
void fp_logger_okf(const fp_logger_t* l, const char* fmt, ...)
	__attribute__((format(printf, 2, 3)));
void fp_logger_warnf(const fp_logger_t* l, const char* fmt, ...)
	__attribute__((format(printf, 2, 3)));
void fp_logger_errorf(const fp_logger_t* l, const char* fmt, ...)
	__attribute__((format(printf, 2, 3)));
void fp_logger_debugf(const fp_logger_t* l, const char* fmt, ...)
	__attribute__((format(printf, 2, 3)));
void fp_logger_beginf(const fp_logger_t* l, const char* fmt, ...)
	__attribute__((format(printf, 2, 3)));
void fp_logger_endf(const fp_logger_t* l, int success, const char* fmt, ...)
	__attribute__((format(printf, 3, 4)));
void fp_logger_wendf(const fp_logger_t* l, int success, const char* fmt, ...)
	__attribute__((format(printf, 3, 4)));

typedef struct {
	char scope[64];
	char desc[64];
	char label[256];
	uint64_t total;
	uint64_t current;
	int finished;
	int claimed;
	uint32_t throttle_ms;
	int show_eta;
	int64_t start_ns;
	int64_t last_draw_ns;
	unsigned slot; // row in the pinned progress area, 0 when not pinned
	int fallback;  // no pinned area here: plain progress lines instead
} fp_bar_t;

// Can a bar be pinned at the bottom of this terminal right now?
bool fp_bar_can_pin(void);
// Is this bar drawn in the pinned area (as opposed to plain lines)?
bool fp_bar_pinned(const fp_bar_t* b);
// Take a bar down without a result line (routine work that went fine).
void fp_bar_dismiss(fp_bar_t* b);

fp_bar_t fp_bar_init(const char* scope, const char* desc, const char* label,
		     uint64_t total);
void fp_bar_set_label(fp_bar_t* b, const char* label);
void fp_bar_set_throttle(fp_bar_t* b, uint32_t ms);
void fp_bar_update(fp_bar_t* b, uint64_t current);
void fp_bar_finishf(fp_bar_t* b, int success, const char* fmt, ...)
	__attribute__((format(printf, 3, 4)));

typedef struct {
	char scope[64];
	char desc[64];
	char label[256];
	unsigned frame;
	int done;
	int64_t start_ns;
} fp_spin_t;

fp_spin_t fp_spin_init(const char* scope, const char* desc, const char* label);
void fp_spin_tick(fp_spin_t* s);
// Clear the spinner line without a result line (routine work that went fine).
void fp_spin_dismiss(fp_spin_t* s);
void fp_spin_finishf(fp_spin_t* s, int success, const char* fmt, ...)
	__attribute__((format(printf, 3, 4)));

void fp_out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void fp_outl(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void fp_out_raw(const char* s, size_t n);

void fp_err_raw(const char* s, size_t n);

bool fp_prompt_confirm_default(const char* question, bool dflt);
int fp_prompt_menu(const char* title, const char* const* items, size_t n);

typedef enum {
	FP_TABLE_WIDGET = 0,
	FP_TABLE_OUTPUT = 1,
} fp_table_sink_t;

// A table's column count follows from the headers it names: one array, so a
// header cannot be added without every count and cell loop following.
#define FP_TABLE_COLS(headers) (sizeof(headers) / sizeof((headers)[0]))

void fp_print_table(const char* const* headers, size_t ncols,
		    const char* const* const* rows, size_t nrows,
		    fp_table_sink_t sink);

#ifdef __cplusplus
}
#endif

#endif
