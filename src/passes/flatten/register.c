/*
 * register.c — the `flatten` pass definition (3A spec §11, FL-15, FL-16): option parsing, the top
 * check, the INFO lines and the entry point odin3_flatten_run (plan Task 2: pre-flight only; the
 * expansion arrives with Task 3).
 */
#include "ir/design.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/flatten/flatten.h"
#include "passes/manager.h"
#include "util/log.h"
#include "util/str.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* --- options ------------------------------------------------------------------------------- */

/* A separator is one non-empty script word: no blank, no '"', ';' or '#'. */
static bool separator_valid(odin3_bytes word) {
    const char *text = (const char *)word.ptr;
    if (word.len == 0) {
        return false;
    }
    for (size_t i = 0; i < word.len; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (isspace(ch) || ch == '"' || ch == ';' || ch == '#') {
            return false;
        }
    }
    return true;
}

odin3_status odin3_flatten_parse_args(odin3_bytes args, odin3_strbuf *sep,
                                      odin3_flatten_opts *opts) {
    opts->separator = NULL;
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        if (!odin3_pass_arg_is(word, "--separator")) {
            odin3_log(ODIN3_LOG_ERROR,
                      "flatten: unknown option '%.*s' (options: --separator <str>)", (int)word.len,
                      (const char *)word.ptr);
            return ODIN3_ERR_INVALID_ARG;
        }
        odin3_bytes value = odin3_pass_arg_next(&args);
        if (value.ptr == NULL || !separator_valid(value)) {
            odin3_log(ODIN3_LOG_ERROR,
                      "flatten: --separator needs a non-empty word without blanks, '\"', ';' "
                      "or '#'");
            return ODIN3_ERR_INVALID_ARG;
        }
        odin3_strbuf_clear(sep);
        if (odin3_strbuf_append(sep, value) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
        opts->separator = sep->data;
    }
    return ODIN3_OK;
}

/* --- the run ------------------------------------------------------------------------------- */

odin3_status odin3_flatten_run(odin3_pass_ctx *ctx, odin3_design *design,
                               const odin3_flatten_opts *opts) {
    (void)opts;
    odin3_module_id top = odin3_design_top(design);
    if (top.v == 0) {
        odin3_log(ODIN3_LOG_ERROR, "flatten: the design has no top module (run hierarchy)");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_flatten_graph *graph = NULL;
    odin3_status st = odin3_flatten_graph_build(design, top, &graph);
    st = st == ODIN3_OK ? odin3_flatten_preflight(ctx, design, graph) : st;
    if (st == ODIN3_OK) {
        odin3_flatten_totals tot = odin3_flatten_graph_totals(graph);
        odin3_prov_begin_op(ctx); /* Task 3 begins one operation per expanded instance */
        odin3_log(ODIN3_LOG_INFO, "flatten: %" PRIu64 " instances to expand", tot.instances);
        odin3_flatten_graph_log_unused(graph);
    }
    odin3_flatten_graph_free(graph);
    return st;
}

static odin3_status flatten_pass_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                     void *user) {
    (void)user;
    odin3_strbuf sep;
    odin3_strbuf_init(&sep);
    odin3_flatten_opts opts;
    odin3_status st = odin3_flatten_parse_args(args, &sep, &opts);
    st = st == ODIN3_OK ? odin3_flatten_run(ctx, design, &opts) : st;
    odin3_strbuf_free(&sep);
    return st;
}

const odin3_pass_def odin3_pass_flatten = {
    "flatten", "flatten [--separator <str>]", flatten_pass_run, NULL,
    false,     ODIN3_PASS_PROCESSES_REFUSE};
