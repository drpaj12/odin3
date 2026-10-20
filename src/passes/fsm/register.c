/*
 * register.c — the pass definitions of the fsm family (spec F-2): `fsm_detect` and (Task 10)
 * `fsm`, with their option parsing. Both are processes = REFUSE: proc must have run.
 */
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/fsm/fsm.h"
#include "passes/manager.h"
#include "util/log.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static odin3_status detect_pass_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                    void *user) {
    (void)ctx;
    (void)user;
    odin3_fsm_opts opts;
    memset(&opts, 0, sizeof opts);
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        if (!odin3_pass_arg_is(word, "-ignore-self-reset")) {
            odin3_log(ODIN3_LOG_ERROR, "fsm_detect: unknown argument '%.*s'", (int)word.len,
                      (const char *)word.ptr);
            return ODIN3_ERR_INVALID_ARG;
        }
        opts.ignore_self_reset = true;
    }
    return odin3_fsm_detect_run(design, &opts);
}

/* The options Yosys's fsm knows and this port does not (F-1, V-2). */
static bool unsupported_option(odin3_bytes word) {
    static const char *const k_words[] = {"-expand", "-fullexpand", "-export", "-fm_set_fsm_file",
                                          "-encfile"};
    for (size_t i = 0; i < sizeof k_words / sizeof k_words[0]; i++) {
        if (odin3_pass_arg_is(word, k_words[i])) {
            return true;
        }
    }
    return false;
}

/* `-encoding TYPE`: one-hot or binary; the text lives as long as the pass arguments. */
static odin3_status parse_encoding(odin3_bytes *args, odin3_fsm_opts *opts, char *storage) {
    odin3_bytes value = odin3_pass_arg_next(args);
    if (value.ptr == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "fsm: -encoding needs a type");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (!odin3_pass_arg_is(value, "one-hot") && !odin3_pass_arg_is(value, "binary")) {
        odin3_log(ODIN3_LOG_ERROR, "fsm: unsupported encoding '%.*s'", (int)value.len,
                  (const char *)value.ptr);
        return ODIN3_ERR_INVALID_ARG;
    }
    (void)snprintf(storage, ODIN3_FSM_ENC_MAX, "%.*s", (int)value.len, (const char *)value.ptr);
    opts->encoding = storage;
    return ODIN3_OK;
}

/* Reads one word of the fsm arguments into opts. */
static odin3_status parse_fsm_word(odin3_bytes word, odin3_bytes *args, odin3_fsm_opts *opts,
                                   char *storage) {
    if (odin3_pass_arg_is(word, "-nodetect")) {
        opts->nodetect = true;
    } else if (odin3_pass_arg_is(word, "-norecode")) {
        opts->norecode = true;
    } else if (odin3_pass_arg_is(word, "-nomap")) {
        opts->nomap = true;
    } else if (odin3_pass_arg_is(word, "-encoding")) {
        return parse_encoding(args, opts, storage);
    } else {
        odin3_log(ODIN3_LOG_ERROR,
                  unsupported_option(word) ? "fsm: option '%.*s' is not supported"
                                           : "fsm: unknown argument '%.*s'",
                  (int)word.len, (const char *)word.ptr);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

static odin3_status fsm_pass_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                 void *user) {
    (void)user;
    odin3_fsm_opts opts;
    char encoding[ODIN3_FSM_ENC_MAX];
    memset(&opts, 0, sizeof opts);
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        odin3_status st = parse_fsm_word(word, &args, &opts, encoding);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return odin3_fsm_run(ctx, design, &opts);
}

const odin3_pass_def odin3_pass_fsm = {
    "fsm",
    "fsm [-nodetect] [-norecode] [-nomap] [-encoding one-hot|binary]: extract, recode and map the "
    "FSMs (Yosys fsm; no -expand, -export)",
    fsm_pass_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};

const odin3_pass_def odin3_pass_fsm_detect = {
    "fsm_detect",
    "fsm_detect [-ignore-self-reset]: mark the FSM state registers (fsm_encoding = \"auto\"); "
    "Yosys fsm_detect",
    detect_pass_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};
