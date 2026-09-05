// Phase timing: every span prints its duration as it closes, and the final
// table ranks labels by the time spent in them alone (children excluded).

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FPKG_MODULE "profile"
#include "flatpkg.h"
#include "fpkglog.h"

#define PROF_MAX_ROWS 192
#define PROF_MAX_DEPTH 32

typedef struct {
	char label[FPKG_SPAN_LABEL];
	unsigned calls;
	int64_t incl_ns;
	int64_t self_ns;
} prof_row_t;

static prof_row_t g_rows[PROF_MAX_ROWS];
static size_t g_nrows;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_on;
static int64_t g_start_ns;

// Spans nest per thread: parallel pulls open their own from worker threads.
static __thread int g_depth;
static __thread int64_t g_child_ns[PROF_MAX_DEPTH];

static int64_t now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

bool fpkg_prof_on(void) { return g_on > 0; }

void fpkg_prof_enable(void) {
	if (g_on > 0) return;
	g_on = 1;
	g_start_ns = now_ns();
}

fpkg_span_t fpkg_span_begin(const char* fmt, ...) {
	fpkg_span_t s;
	memset(&s, 0, sizeof(s));
	if (!fpkg_prof_on()) return s;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s.label, sizeof(s.label), fmt, ap);
	va_end(ap);
	s.live = true;
	s.t0 = now_ns();
	g_depth++;
	if (g_depth < PROF_MAX_DEPTH) g_child_ns[g_depth] = 0;
	return s;
}

void fpkg_span_end(fpkg_span_t* s) {
	if (!s->live) return;
	s->live = false;
	int64_t incl = now_ns() - s->t0;
	int d = g_depth;
	int64_t kids = d < PROF_MAX_DEPTH ? g_child_ns[d] : 0;
	if (g_depth > 0) g_depth--;
	if (g_depth < PROF_MAX_DEPTH) g_child_ns[g_depth] += incl;
	int64_t self = incl - kids;
	if (self < 0) self = 0;

	pthread_mutex_lock(&g_mu);
	prof_row_t* row = NULL;
	for (size_t i = 0; i < g_nrows; i++)
		if (strcmp(g_rows[i].label, s->label) == 0) {
			row = &g_rows[i];
			break;
		}
	if (!row && g_nrows < PROF_MAX_ROWS) {
		row = &g_rows[g_nrows++];
		memset(row, 0, sizeof(*row));
		snprintf(row->label, sizeof(row->label), "%s", s->label);
	}
	if (row) {
		row->calls++;
		row->incl_ns += incl;
		row->self_ns += self;
	}
	pthread_mutex_unlock(&g_mu);

	fpkg_infof("%*s%9.1f ms  %s", 2 * (d - 1), "", (double)incl / 1e6,
		   s->label);
}

static int row_cmp(const void* a, const void* b) {
	const prof_row_t *x = a, *y = b;
	if (x->self_ns == y->self_ns) return 0;
	return x->self_ns < y->self_ns ? 1 : -1;
}

void fpkg_prof_report(void) {
	if (!fpkg_prof_on()) return;
	pthread_mutex_lock(&g_mu);
	size_t n = g_nrows;
	prof_row_t* rows = malloc((n ? n : 1) * sizeof(*rows));
	if (rows) memcpy(rows, g_rows, n * sizeof(*rows));
	pthread_mutex_unlock(&g_mu);
	if (!rows) return;
	qsort(rows, n, sizeof(*rows), row_cmp);
	fpkg_infof("where the time went (self = without nested phases):");
	fpkg_infof("%9s %9s %6s  %s", "self ms", "total ms", "calls", "phase");
	for (size_t i = 0; i < n; i++)
		fpkg_infof("%9.1f %9.1f %6u  %s", (double)rows[i].self_ns / 1e6,
			   (double)rows[i].incl_ns / 1e6, rows[i].calls,
			   rows[i].label);
	fpkg_infof("wall clock since profiling began: %.1f ms",
		   (double)(now_ns() - g_start_ns) / 1e6);
	free(rows);
}
