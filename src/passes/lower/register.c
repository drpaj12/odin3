/*
 * register.c — the `lower` pass definition (3B spec §11, §13; plan Task 1): option parsing and the
 * pass entry. `lower [--neg-edge invert|keep] [--async sync|error] [--report <file>]
 * [--max-mem-bits N]`; processes = REFUSE (a $proc has no bit form, O1).
 */
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/lower/lower.h"
#include "passes/manager.h"
#include "util/log.h"
#include "util/str.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* The next word as a C string (valid until the next call with buf); NULL when there is none, or
 * with *st NO_MEMORY. */
static const char *next_word(odin3_bytes *args, odin3_strbuf *buf, odin3_status *st) {
    odin3_bytes word = odin3_pass_arg_next(args);
    if (word.ptr == NULL) {
        return NULL;
    }
    odin3_strbuf_clear(buf);
    *st = odin3_strbuf_append(buf, word);
    if (*st != ODIN3_OK) {
        return NULL;
    }
    return buf->data != NULL ? buf->data : "";
}

static odin3_status bad_value(const char *option, const char *needs) {
    odin3_log(ODIN3_LOG_ERROR, "lower: %s needs %s", option, needs);
    return ODIN3_ERR_INVALID_ARG;
}

enum { DECIMAL_BASE = 10 };

/* A decimal number, nothing else. */
static bool parse_u64(const char *text, uint64_t *out) {
    if (text[0] < '0' || text[0] > '9') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, DECIMAL_BASE);
    if (errno != 0 || *end != '\0') {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

/* One option whose value word is in `value` (the option's own name is `name`). */
static odin3_status apply_option(const char *name, const char *value, odin3_strbuf *report,
                                 odin3_lower_opts *opts) {
    if (strcmp(name, "--neg-edge") == 0) {
        if (strcmp(value, "invert") != 0 && strcmp(value, "keep") != 0) {
            return bad_value(name, "invert or keep");
        }
        opts->keep_neg_edge = strcmp(value, "keep") == 0;
    } else if (strcmp(name, "--async") == 0) {
        if (strcmp(value, "sync") != 0 && strcmp(value, "error") != 0) {
            return bad_value(name, "sync or error");
        }
        opts->async_is_error = strcmp(value, "error") == 0;
    } else if (strcmp(name, "--max-mem-bits") == 0) {
        if (!parse_u64(value, &opts->max_mem_bits) || opts->max_mem_bits == 0) {
            return bad_value(name, "a positive number");
        }
    } else {
        odin3_strbuf_clear(report);
        odin3_status st = odin3_strbuf_append(report, odin3_bytes_cstr(value));
        if (st != ODIN3_OK) {
            return st;
        }
        opts->report_path = report->data;
    }
    return ODIN3_OK;
}

static const char k_enable_first[] = "--async-enable-first";

static bool is_option(const char *word) {
    static const char *const k_options[] = {"--neg-edge", "--async", "--report", "--max-mem-bits"};
    for (size_t i = 0; i < sizeof k_options / sizeof k_options[0]; i++) {
        if (strcmp(word, k_options[i]) == 0) {
            return true;
        }
    }
    return false;
}

odin3_status odin3_lower_parse_args(odin3_bytes args, odin3_strbuf *report,
                                    odin3_lower_opts *opts) {
    memset(opts, 0, sizeof *opts);
    odin3_strbuf word_buf;
    odin3_strbuf value_buf;
    odin3_strbuf_init(&word_buf);
    odin3_strbuf_init(&value_buf);
    odin3_status st = ODIN3_OK;
    for (const char *word = next_word(&args, &word_buf, &st); word != NULL;
         word = next_word(&args, &word_buf, &st)) {
        if (strcmp(word, k_enable_first) == 0) { /* hidden, test-only: a flag without a value */
            opts->async_enable_first = true;
            continue;
        }
        if (!is_option(word)) {
            odin3_log(ODIN3_LOG_ERROR,
                      "lower: unknown argument '%s' (options: --neg-edge invert|keep, --async "
                      "sync|error, --report <file>, --max-mem-bits N)",
                      word);
            st = ODIN3_ERR_INVALID_ARG;
            break;
        }
        const char *value = next_word(&args, &value_buf, &st);
        st = st == ODIN3_OK && value == NULL ? bad_value(word, "a value") : st;
        st = st == ODIN3_OK ? apply_option(word, value, report, opts) : st;
        if (st != ODIN3_OK) {
            break;
        }
    }
    odin3_strbuf_free(&word_buf);
    odin3_strbuf_free(&value_buf);
    return st;
}

static odin3_status lower_pass_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                   void *user) {
    (void)user;
    odin3_strbuf report;
    odin3_strbuf_init(&report);
    odin3_lower_opts opts;
    odin3_status st = odin3_lower_parse_args(args, &report, &opts);
    st = st == ODIN3_OK ? odin3_lower_run(ctx, design, &opts) : st;
    odin3_strbuf_free(&report);
    return st;
}

const odin3_pass_def odin3_pass_lower = {
    "lower",
    "lower [--neg-edge invert|keep] [--async sync|error] [--report <file>] [--max-mem-bits N]",
    lower_pass_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};
