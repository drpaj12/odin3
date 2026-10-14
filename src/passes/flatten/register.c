/*
 * register.c — the `flatten` pass definition (3A spec §11, FL-15, FL-16): option parsing, the top
 * check, the INFO lines and the entry point odin3_flatten_run (plan Task 2: pre-flight only; the
 * expansion arrives with Task 3).
 */
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/builtin.h"
#include "passes/flatten/flatten.h"
#include "passes/flatten/flatten_int.h"
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

/* Expands every root, parents before children so that copies come from unflattened modules. */
static odin3_status expand_roots(odin3_flatten_ctx *fc, const odin3_flatten_graph *graph,
                                 odin3_design *design) {
    uint32_t n_order = 0;
    const uint32_t *order = odin3_flatten_graph_order(graph, &n_order);
    for (uint32_t i = 0; i < n_order; i++) {
        if (!odin3_flatten_graph_is_root(graph, order[i])) {
            continue;
        }
        odin3_status st =
            odin3_flatten_expand_root(fc, odin3_module_get(design, (odin3_module_id){order[i]}));
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* What the run cost, for the estimate's calibration (plan Task 5): the design's reserved bytes and
 * provenance records before and after, the pre-flight's flat totals and its estimate. */
typedef struct flatten_before {
    uint64_t bytes;
    uint32_t records;
} flatten_before;

static void log_cost(const odin3_flatten_ctx *fc, const odin3_flatten_size *add,
                     const flatten_before *before, uint64_t estimate) {
    odin3_log(ODIN3_LOG_INFO,
              "flatten: cost: bytes %" PRIu64 " -> %" PRIu64 ", provenance records %" PRIu32
              " -> %" PRIu32 "; added nodes %" PRIu64 " pins %" PRIu64 " nets %" PRIu64
              " wires %" PRIu64 "; estimate %" PRIu64 "; renamed %" PRIu64 "; alias bits %" PRIu64
              " bare %" PRIu64,
              before->bytes, odin3_design_bytes_reserved(fc->design), before->records,
              odin3_prov_end(fc->design), add->nodes, add->pins, add->nets, add->wires, estimate,
              fc->renamed, add->alias_bits, add->bare);
}

static void report(odin3_flatten_ctx *fc, const odin3_flatten_totals *tot) {
    odin3_log(ODIN3_LOG_INFO,
              "flatten: %" PRIu64 " instances expanded (%" PRIu64 " kept) in %" PRIu32
              " roots; %" PRIu32 " modules unused",
              fc->instances, fc->kept, tot->roots, tot->unused);
    odin3_strbuf info;
    odin3_strbuf_init(&info);
    uint32_t str = 0;
    if (odin3_strbuf_appendf(&info,
                             "roots=%" PRIu32 " instances=%" PRIu64 " kept=%" PRIu64
                             " nodes=%" PRIu64 " renamed=%" PRIu64,
                             tot->roots, fc->instances, fc->kept, fc->nodes,
                             fc->renamed) == ODIN3_OK &&
        odin3_design_intern(fc->design, odin3_bytes_cstr(info.data), &str) == ODIN3_OK) {
        (void)odin3_passrun_set_info(fc->pass, str);
    }
    odin3_strbuf_free(&info);
}

odin3_status odin3_flatten_run(odin3_pass_ctx *ctx, odin3_design *design,
                               const odin3_flatten_opts *opts) {
    odin3_module_id top = odin3_design_top(design);
    if (top.v == 0) {
        odin3_log(ODIN3_LOG_ERROR, "flatten: the design has no top module (run hierarchy)");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_flatten_graph *graph = NULL;
    odin3_status st = odin3_flatten_graph_build(design, top, &graph);
    st = st == ODIN3_OK ? odin3_flatten_preflight(ctx, design, graph) : st;
    if (st != ODIN3_OK) {
        odin3_flatten_graph_free(graph);
        return st;
    }
    odin3_flatten_totals tot = odin3_flatten_graph_totals(graph);
    flatten_before before = {odin3_design_bytes_reserved(design), odin3_prov_end(design)};
    uint64_t estimate = odin3_flatten_graph_estimate(graph);
    odin3_flatten_size added = odin3_flatten_graph_added(graph);
    odin3_log(ODIN3_LOG_INFO, "flatten: %" PRIu64 " instances to expand", tot.instances);
    odin3_flatten_ctx fc;
    st = odin3_flatten_ctx_init(&fc, ctx, graph, opts != NULL ? opts->separator : NULL);
    st = st == ODIN3_OK ? expand_roots(&fc, graph, design) : st;
    if (st == ODIN3_OK) {
        report(&fc, &tot);
        log_cost(&fc, &added, &before, estimate);
        odin3_flatten_graph_log_unused(graph);
    }
    odin3_flatten_ctx_free(&fc);
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
