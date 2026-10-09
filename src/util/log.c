/*
 * log.c — process-global diagnostics: level filter, per-level counts, replaceable sink.
 */
#include "util/log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static odin3_log_level g_max_level = ODIN3_LOG_INFO;
static odin3_log_sink g_sink = NULL;
static void *g_user = NULL;
static size_t g_counts[ODIN3_LOG_LEVEL_COUNT];
static bool g_in_sink = false;

static const char *const level_names[ODIN3_LOG_LEVEL_COUNT] = {"error", "warning", "info", "debug"};

ODIN3_EXPORT odin3_status odin3_log_set_level(odin3_log_level max_level) {
    if ((int)max_level < 0 || max_level >= ODIN3_LOG_LEVEL_COUNT) {
        return ODIN3_ERR_INVALID_ARG;
    }
    g_max_level = max_level;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_log_level odin3_log_get_level(void) {
    return g_max_level;
}

ODIN3_EXPORT void odin3_log_set_sink(odin3_log_sink sink, void *user) {
    g_sink = sink;
    g_user = user;
}

size_t odin3_log_count(odin3_log_level level) {
    if ((int)level < 0 || level >= ODIN3_LOG_LEVEL_COUNT) {
        return 0;
    }
    return g_counts[level];
}

void odin3_log_reset_counts(void) {
    memset(g_counts, 0, sizeof g_counts);
}

/* Formats into buf (ODIN3_LOG_BUF bytes), cutting an over-long message to end in "...". */
static void format_message(char *buf, const char *fmt, va_list args) {
    int written = vsnprintf(buf, ODIN3_LOG_BUF, fmt, args);
    if (written < 0) {
        (void)snprintf(buf, ODIN3_LOG_BUF, "%s", "(log formatting error)");
    } else if (written >= ODIN3_LOG_BUF) {
        memcpy(buf + ODIN3_LOG_BUF - 4, "...", 4);
    }
}

static void deliver(odin3_log_level level, const char *msg) {
    if (g_sink == NULL) {
        (void)fprintf(stderr, "odin3: %s: %s\n", level_names[level], msg);
        return;
    }
    g_in_sink = true;
    g_sink(level, msg, g_user);
    g_in_sink = false;
}

void odin3_log(odin3_log_level level, const char *fmt, ...) {
    if ((int)level < 0 || level >= ODIN3_LOG_LEVEL_COUNT) {
        return;
    }
    g_counts[level]++;
    if (level > g_max_level || g_in_sink) {
        return;
    }
    char buf[ODIN3_LOG_BUF];
    va_list args;
    va_start(args, fmt);
    format_message(buf, fmt, args);
    va_end(args);
    deliver(level, buf);
}
