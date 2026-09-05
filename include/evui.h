// The daemon links no terminal library: src/daemon/ops/evui.c defines the
// fplog names as plain data, and a sink turns each call into an event.
// Nothing here formats, colours, translates or asks a question.
#ifndef FPKG_EVUI_H
#define FPKG_EVUI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	// A line about the operation.  `level` is an FP_LEVEL_*.
	void (*log)(void* ctx, int level, const char* scope, const char* text);
	// A progress record; `stage` says what is progressing.  Coalescing is
	// the caller's, so a sink may forward every call it gets.
	void (*progress)(void* ctx, const char* stage, uint64_t current,
			 uint64_t total);
	// A table as data: headers (ncols strings, or NULL) and nrows rows of
	// ncols cells.  False means the sink declined it (too big for the
	// protocol, say) and the cells go out as lines instead.
	bool (*table)(void* ctx, const char* const* headers, size_t ncols,
		      const char* const* const* rows, size_t nrows);
	void* ctx;
} evui_sink_t;

// Install a sink (NULL removes it).  While none is installed, log lines go to
// the daemon's own stderr as "scope: text" - daemon-internal matters only.
void evui_set_sink(const evui_sink_t* sink);

// True when a sink took the record (so a terminal implementation must not
// draw it too).  Only the terminal implementation (tb_*.c) defines this.
#include <stdbool.h>
bool fp_record_active(void);
bool fp_record_progress(const char* stage, uint64_t current, uint64_t total);
// A line of output that is data for the requester (a changeset's rows, say),
// handed to the sink as a log record.  False when no sink took it.
bool fp_record_line(int level, const char* text);
// A table as data to the request's sink.  False when there is no sink, it
// takes no tables, or the table is too big for the protocol.
bool fp_record_table(const char* const* headers, size_t ncols,
		     const char* const* const* rows, size_t nrows);

#endif
