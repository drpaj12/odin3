/*
 * register.c — the `cone_check` pass definition (Phase 7 spec 3.3): option parsing and the loop
 * over the modules; a group that no module has is an error.
 */
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/cone_check/cone_check_int.h"
#include "passes/manager.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { DECIMAL = 10 };

static const char k_usage[] = "cone_check -attr KEY=V -cell TYPE [-param P=V ...] [-timeout S]";

/* --- options ------------------------------------------------------------------------------- */

static odin3_status usage_error(const char *what, const char *word) {
    odin3_log(ODIN3_LOG_ERROR, "cone_check: %s%s (usage: %s)", what, word, k_usage);
    return ODIN3_ERR_INVALID_ARG;
}

/* The next word as a NUL-terminated copy in the arena, or NULL (error logged) when absent. */
static char *take_text(odin3_bytes *args, const char *option, odin3_arena *arena) {
    odin3_bytes value = odin3_pass_arg_next(args);
    if (value.ptr == NULL || value.len == 0) {
        (void)usage_error(option, " needs a value");
        return NULL;
    }
    return odin3_arena_strndup(arena, (const char *)value.ptr, value.len);
}

/* Splits "NAME=VALUE" at the first '='; both parts must be non-empty. The name is copied into
 * the arena, the value points into text. */
static odin3_status take_pair(odin3_bytes *args, const char *option, cc_opts *opts, cc_param *out) {
    const char *text = take_text(args, option, opts->arena);
    const char *eq = text != NULL ? strchr(text, '=') : NULL;
    if (text == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    if (eq == NULL || eq == text || eq[1] == '\0') {
        return usage_error(option, " needs NAME=VALUE");
    }
    out->name = odin3_arena_strndup(opts->arena, text, (size_t)(eq - text));
    out->value = eq + 1;
    return out->name != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

static odin3_status take_timeout(odin3_bytes *args, cc_opts *opts) {
    const char *text = take_text(args, "-timeout", opts->arena);
    char *end = NULL;
    if (text == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    unsigned long seconds = strtoul(text, &end, DECIMAL);
    if (text[0] < '0' || text[0] > '9' || *end != '\0' || seconds > CC_MAX_TIMEOUT) {
        return usage_error("-timeout needs whole seconds, got ", text);
    }
    opts->timeout = (uint32_t)seconds;
    return ODIN3_OK;
}

static odin3_status take_param(odin3_bytes *args, cc_opts *opts) {
    cc_param param = {NULL, NULL};
    odin3_status st = take_pair(args, "-param", opts, &param);
    cc_param *slot = st == ODIN3_OK ? odin3_vec_push(&opts->params) : NULL;
    if (st == ODIN3_OK && slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (slot != NULL) {
        *slot = param;
    }
    return st;
}

static odin3_status take_option(odin3_bytes word, odin3_bytes *args, cc_opts *opts) {
    if (odin3_pass_arg_is(word, "-attr")) {
        cc_param pair = {NULL, NULL};
        odin3_status st = take_pair(args, "-attr", opts, &pair);
        opts->key = pair.name;
        opts->value = pair.value;
        return st;
    }
    if (odin3_pass_arg_is(word, "-cell")) {
        opts->cell = take_text(args, "-cell", opts->arena);
        return opts->cell != NULL ? ODIN3_OK : ODIN3_ERR_INVALID_ARG;
    }
    if (odin3_pass_arg_is(word, "-param")) {
        return take_param(args, opts);
    }
    if (odin3_pass_arg_is(word, "-timeout")) {
        return take_timeout(args, opts);
    }
    char text[ODIN3_LOG_BUF / 4];
    (void)snprintf(text, sizeof text, "unknown option '%.*s'", (int)word.len,
                   (const char *)word.ptr);
    return usage_error(text, "");
}

static odin3_status parse_args(odin3_bytes args, cc_opts *opts) {
    odin3_status st = ODIN3_OK;
    for (odin3_bytes word = odin3_pass_arg_next(&args); st == ODIN3_OK && word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        st = take_option(word, &args, opts);
    }
    if (st == ODIN3_OK && opts->key == NULL) {
        return usage_error("-attr is required", "");
    }
    return st == ODIN3_OK && opts->cell == NULL ? usage_error("-cell is required", "") : st;
}

/* --- the pass ------------------------------------------------------------------------------ */

static odin3_status cone_check_pass_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                        void *user) {
    (void)user;
    cc_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.timeout = CC_DEFAULT_TIMEOUT;
    opts.arena = odin3_arena_create(0);
    odin3_vec_init(&opts.params, sizeof(cc_param));
    odin3_status st = opts.arena != NULL ? parse_args(args, &opts) : ODIN3_ERR_NO_MEMORY;
    bool any = false;
    uint32_t end = odin3_design_module_end(design);
    for (uint32_t mid = 1; st == ODIN3_OK && mid < end; mid++) {
        bool found = false;
        st = cc_run_module(ctx, odin3_module_get(design, (odin3_module_id){mid}), &opts, &found);
        any = any || found;
    }
    if (st == ODIN3_OK && !any) {
        odin3_log(ODIN3_LOG_ERROR,
                  "cone_check: group '%s=%s' is empty: no live node of any module has it", opts.key,
                  opts.value);
        st = ODIN3_ERR_INVALID_ARG;
    }
    odin3_vec_free(&opts.params);
    odin3_arena_destroy(opts.arena);
    return st;
}

const odin3_pass_def odin3_pass_cone_check = {"cone_check", k_usage, cone_check_pass_run,
                                              NULL,         false,   ODIN3_PASS_PROCESSES_REFUSE};
