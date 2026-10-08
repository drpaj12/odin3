/*
 * log.h — process-global diagnostics with levels, per-level counts and a replaceable sink.
 */
#ifndef ODIN3_UTIL_LOG_H
#define ODIN3_UTIL_LOG_H

#include "util/attr.h"

#include <stddef.h>

/* Size of the stack buffer a message is formatted into, including the NUL. */
enum { ODIN3_LOG_BUF = 1024 };

typedef enum odin3_log_level {
    ODIN3_LOG_ERROR,
    ODIN3_LOG_WARN,
    ODIN3_LOG_INFO,
    ODIN3_LOG_DEBUG,
    ODIN3_LOG_LEVEL_COUNT
} odin3_log_level;

typedef void (*odin3_log_sink)(odin3_log_level level, const char *msg, void *user);

/* Messages above max_level are counted but not delivered. Default: ODIN3_LOG_INFO. */
void odin3_log_set_level(odin3_log_level max_level);

/* Replaces the destination of delivered messages; NULL restores stderr. */
void odin3_log_set_sink(odin3_log_sink sink, void *user);

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
