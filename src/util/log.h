/*
 * log.h — process-global diagnostics with levels, per-level counts and a replaceable sink.
 */
#ifndef ODIN3_UTIL_LOG_H
#define ODIN3_UTIL_LOG_H

#include "odin3/odin3.h"
#include "util/attr.h"

#include <stddef.h>

/* Size of the stack buffer a message is formatted into, including the NUL. */
enum { ODIN3_LOG_BUF = 1024 };

/*
 * odin3_log_level, odin3_log_sink, odin3_log_set_level, odin3_log_get_level and
 * odin3_log_set_sink are part of the public ABI (odin3.h): messages above the level (default
 * ODIN3_LOG_INFO) are counted but not delivered; a NULL sink restores stderr.
 */

/*
 * Counts the message, then delivers it if its level passes the filter. The text is formatted into
 * ODIN3_LOG_BUF bytes; longer text is cut to ODIN3_LOG_BUF - 1 characters ending in "...". A
 * message logged from inside a sink is counted but not delivered. Out-of-range levels are ignored.
 */
void odin3_log(odin3_log_level level, const char *fmt, ...) ODIN3_PRINTF(2, 3);

/* Messages logged at level since the last reset, including filtered ones. */
size_t odin3_log_count(odin3_log_level level);

void odin3_log_reset_counts(void);

#endif
