/*
 * activity.c — the activity pass (spec docs/specs/2026-11-08-activity-design.md): vectorless static
 * probability and transition density of every net of the top module, written as a VPR `.act` file,
 * a JSON file and module attributes odin3:activity_*.
 */
#include "passes/activity/act_int.h"

#include "passes/builtin.h"
#include "passes/sim_netnames.h"
#include "util/clock.h"
#include "util/log.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { DECIMAL = 10, NUM_BUF = 64 };

static const double k_density_max = 2.0;
static const double k_default_prob = 0.5;
static const double k_default_density = 0.2;
static const double k_default_tol = 1e-6;

static const char k_usage[] =
    "activity: usage: activity [--pi-prob P] [--pi-density D] [--pi-file F] [--vcd F [--vcd-scope "
    "S] "
    "[--vcd-clock C]] [--clock NAME] [--ff-start P] [--tol E] [--max-iter N] [--exact K] "
    "[--opaque-prob P] [--opaque-density D] [--opaque-type NAME]... [--out F.act] [--name-style "
    "short|provenance|hash] "
    "[--json F] [--no-attrs]";

static odin3_status bad_option(const char *why) {
    odin3_log(ODIN3_LOG_ERROR, "activity: %s", why);
    odin3_log(ODIN3_LOG_ERROR, "%s", k_usage);
    return ODIN3_ERR_INVALID_ARG;
}

static bool word_text(odin3_bytes *rest, char *text, size_t size) {
    odin3_bytes word = odin3_pass_arg_next(rest);
    if (word.ptr == NULL || word.len == 0 || word.len >= size) {
        return false;
    }
    memcpy(text, word.ptr, word.len);
    text[word.len] = '\0';
    return true;
}

static bool next_double(odin3_bytes *rest, double low, double high, double *val) {
    char text[NUM_BUF];
    char *end = NULL;
    if (!word_text(rest, text, sizeof text)) {
        return false;
    }
    errno = 0;
    *val = strtod(text, &end);
    return *end == '\0' && errno == 0 && *val >= low && *val <= high;
}

static bool next_uint(odin3_bytes *rest, unsigned long long max, unsigned long long *val) {
    char text[NUM_BUF];
    char *end = NULL;
    if (!word_text(rest, text, sizeof text) || text[0] < '0' || text[0] > '9') {
        return false;
    }
    errno = 0;
    *val = strtoull(text, &end, DECIMAL);
    return *end == '\0' && errno == 0 && *val <= max;
}

/* The string option slots, in the order of k_string_flags. */
enum { SLOT_OUT, SLOT_JSON, SLOT_PI_FILE, SLOT_VCD, SLOT_VCD_SCOPE, SLOT_VCD_CLOCK, SLOT_CLOCK };

static odin3_strbuf *string_slot(act_opts *ob, size_t idx) {
    switch (idx) {
    case SLOT_OUT:
        return &ob->out;
    case SLOT_JSON:
        return &ob->json;
    case SLOT_PI_FILE:
        return &ob->pi_file;
    case SLOT_VCD:
        return &ob->vcd;
    case SLOT_VCD_SCOPE:
        return &ob->vcd_scope;
    case SLOT_VCD_CLOCK:
        return &ob->vcd_clock;
    default:
        return &ob->clock;
    }
}

static const char *const k_string_flags[] = {"--out",       "--json",      "--pi-file", "--vcd",
                                             "--vcd-scope", "--vcd-clock", "--clock"};

static odin3_status string_option(odin3_bytes word, odin3_bytes *rest, act_opts *ob, bool *used) {
    *used = true;
    for (size_t kk = 0; kk < sizeof k_string_flags / sizeof k_string_flags[0]; kk++) {
        if (odin3_pass_arg_is(word, k_string_flags[kk])) {
            odin3_bytes val = odin3_pass_arg_next(rest);
            return val.ptr != NULL && val.len > 0 ? odin3_strbuf_append(string_slot(ob, kk), val)
                                                  : bad_option("an option expects a value");
        }
    }
    *used = false;
    return ODIN3_OK;
}

/* Stores a numeric option; idx is the position in k_decimal_flags. */
static void store_decimal(act_opts *ob, size_t idx, double val) {
    double *const slots[] = {&ob->pi_prob,        &ob->pi_density, &ob->opaque_prob,
                             &ob->opaque_density, &ob->ff_start,   &ob->tol};
    *slots[idx] = val;
}

static const struct {
    const char *flag;
    double high;
} k_decimal_flags[] = {{"--pi-prob", 1.0},     {"--pi-density", k_density_max},
                       {"--opaque-prob", 1.0}, {"--opaque-density", k_density_max},
                       {"--ff-start", 1.0},    {"--tol", 1.0}};

static odin3_status decimal_option(odin3_bytes word, odin3_bytes *rest, act_opts *ob, bool *used) {
    *used = true;
    for (size_t kk = 0; kk < sizeof k_decimal_flags / sizeof k_decimal_flags[0]; kk++) {
        if (!odin3_pass_arg_is(word, k_decimal_flags[kk].flag)) {
            continue;
        }
        double val = 0.0;
        if (!next_double(rest, 0.0, k_decimal_flags[kk].high, &val)) {
            return bad_option("a numeric option is out of range");
        }
        store_decimal(ob, kk, val);
        ob->have_density = ob->have_density || kk == 1U;
        return ODIN3_OK;
    }
    *used = false;
    return ODIN3_OK;
}

static odin3_status integer_option(odin3_bytes word, odin3_bytes *rest, act_opts *ob, bool *used) {
    unsigned long long num = 0;
    *used = true;
    if (odin3_pass_arg_is(word, "--max-iter")) {
        if (!next_uint(rest, UINT32_MAX, &num) || num == 0) {
            return bad_option("--max-iter expects a positive number");
        }
        ob->max_iter = (uint32_t)num;
        return ODIN3_OK;
    }
    if (odin3_pass_arg_is(word, "--exact")) {
        if (!next_uint(rest, ACT_MAX_INPUTS, &num)) {
            return bad_option("--exact expects 0 to 16");
        }
        ob->exact = (uint32_t)num;
        return ODIN3_OK;
    }
    *used = false;
    return ODIN3_OK;
}

static odin3_status parse_word(odin3_bytes word, odin3_bytes *rest, act_opts *ob, bool *used) {
    *used = true;
    if (odin3_pass_arg_is(word, "--no-attrs")) {
        ob->attrs = false;
        return ODIN3_OK;
    }
    if (odin3_pass_arg_is(word, "--opaque-type")) {
        odin3_bytes val = odin3_pass_arg_next(rest);
        return val.ptr != NULL && val.len > 0 &&
                       odin3_strbuf_append(&ob->opaque_types, val) == ODIN3_OK &&
                       odin3_strbuf_append(&ob->opaque_types, odin3_bytes_cstr("\n")) == ODIN3_OK
                   ? ODIN3_OK
                   : bad_option("--opaque-type expects a cell type name");
    }
    if (odin3_pass_arg_is(word, "--name-style")) {
        char text[NUM_BUF];
        odin3_blif_name_style style = ODIN3_BLIF_NAMES_SHORT;
        if (!word_text(rest, text, sizeof text) || !odin3_sim_netnames_style(text, &style)) {
            return bad_option("--name-style expects short, provenance or hash");
        }
        ob->name_style = (int)style;
        return ODIN3_OK;
    }
    odin3_status st = string_option(word, rest, ob, used);
    if (st != ODIN3_OK || *used) {
        return st;
    }
    st = decimal_option(word, rest, ob, used);
    return st != ODIN3_OK || *used ? st : integer_option(word, rest, ob, used);
}

static odin3_status parse_args(odin3_bytes args, act_opts *ob) {
    for (odin3_bytes word = odin3_pass_arg_next(&args); word.ptr != NULL;
         word = odin3_pass_arg_next(&args)) {
        bool used = false;
        odin3_status st = parse_word(word, &args, ob, &used);
        if (st != ODIN3_OK || !used) {
            return st != ODIN3_OK ? st : bad_option("unknown option");
        }
    }
    if (ob->pi_density > k_density_max * fmin(ob->pi_prob, 1.0 - ob->pi_prob)) {
        return bad_option("--pi-density must not exceed 2 * min(P1, 1 - P1) of --pi-prob");
    }
    return ODIN3_OK;
}

static void opts_init(act_opts *ob) {
    memset(ob, 0, sizeof *ob);
    ob->pi_prob = k_default_prob;
    ob->pi_density = k_default_density;
    ob->opaque_prob = k_default_prob;
    ob->opaque_density = k_default_density;
    ob->ff_start = k_default_prob;
    ob->tol = k_default_tol;
    ob->max_iter = ACT_DEFAULT_ITER;
    ob->exact = ACT_MAX_INPUTS;
    ob->name_style = ODIN3_BLIF_NAMES_SHORT;
    ob->attrs = true;
    odin3_strbuf *bufs[] = {&ob->out,       &ob->json,      &ob->pi_file, &ob->vcd,
                            &ob->vcd_scope, &ob->vcd_clock, &ob->clock,   &ob->opaque_types};
    for (size_t kk = 0; kk < sizeof bufs / sizeof bufs[0]; kk++) {
        odin3_strbuf_init(bufs[kk]);
    }
}

static void opts_free(act_opts *ob) {
    odin3_strbuf *bufs[] = {&ob->out,       &ob->json,      &ob->pi_file, &ob->vcd,
                            &ob->vcd_scope, &ob->vcd_clock, &ob->clock,   &ob->opaque_types};
    for (size_t kk = 0; kk < sizeof bufs / sizeof bufs[0]; kk++) {
        odin3_strbuf_free(bufs[kk]);
    }
}

/* --- the run ---------------------------------------------------------------------------------- */

static void log_opaque(const act_model *md) {
    const odin3_design *design = odin3_module_design(md->mod);
    for (size_t k = 0; k < md->opaque_types.len; k++) {
        const uint32_t *cur = odin3_vec_cat(&md->opaque_types, k);
        bool seen = false;
        for (size_t j = 0; j < k; j++) {
            seen = seen || *(const uint32_t *)odin3_vec_cat(&md->opaque_types, j) == *cur;
        }
        const odin3_celltype_def *def = odin3_celltype_get(design, (odin3_celltype_id){*cur});
        if (!seen && def != NULL) {
            odin3_log(ODIN3_LOG_INFO, "activity: opaque cell type %s (outputs act as inputs)",
                      def->name);
        }
    }
    if (md->instances > 0) {
        odin3_log(ODIN3_LOG_WARN,
                  "activity: %llu module instance(s) are opaque; flatten first for a whole-design "
                  "result",
                  (unsigned long long)md->instances);
    }
}

static void warn_clocks(const act_model *md) {
    uint32_t clocks = 0;
    uint64_t flops = 0;
    for (uint32_t id = 1; id < md->n_nets; id++) {
        clocks += md->kind[id] == ACT_CLOCK ? 1U : 0U;
        flops += md->kind[id] == ACT_FF ? 1U : 0U;
    }
    if (clocks > 1U || (clocks == 0U && flops > 0U)) {
        odin3_log(ODIN3_LOG_WARN,
                  "activity: %u primary-input clocks; density is per cycle of one reference clock "
                  "(net %u), every clock input gets (0.5, 2)",
                  clocks, (unsigned)md->ref_clock);
    }
}

static odin3_status read_inputs(act_model *md, const act_opts *ob, act_namer *nm) {
    odin3_status st = ODIN3_OK;
    if (ob->pi_file.len > 0) {
        st = act_read_pi_file(md, ob->pi_file.data, act_name_net, nm);
    }
    if (st == ODIN3_OK && ob->vcd.len > 0) {
        act_opts local = *ob;
        odin3_strbuf nameclk;
        odin3_strbuf_init(&nameclk);
        if (ob->vcd_clock.len == 0 && md->ref_clock != 0) {
            (void)act_name_net(nm, md->ref_clock, &nameclk);
            local.vcd_clock = nameclk;
        }
        st = act_read_vcd(md, &local, act_name_net, nm);
        odin3_strbuf_free(&nameclk);
    }
    return st;
}

static odin3_status write_outputs(const act_model *md, act_namer *nm, const act_opts *ob,
                                  double ms) {
    odin3_status st = ODIN3_OK;
    st = ob->out.len > 0 ? act_write_act(md, nm, ob->out.data) : st;
    st = st == ODIN3_OK && ob->json.len > 0 ? act_write_json(md, nm, ob, ob->json.data, ms) : st;
    return st == ODIN3_OK && ob->attrs ? act_write_attrs(md, ob) : st;
}

static odin3_status activity_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_bytes args,
                                 void *user) {
    (void)ctx;
    (void)user;
    act_opts ob;
    opts_init(&ob);
    odin3_status st = parse_args(args, &ob);
    odin3_module_id top = odin3_design_top(design);
    if (st == ODIN3_OK && top.v == 0) {
        odin3_log(ODIN3_LOG_ERROR, "activity: the design has no top module");
        st = ODIN3_ERR_INVALID_ARG;
    }
    double start = odin3_now_ms();
    act_model md;
    memset(&md, 0, sizeof md);
    odin3_sim_netnames names;
    odin3_sim_netnames_init(&names, design, "activity", (odin3_blif_name_style)ob.name_style);
    act_namer nm = {&names, st == ODIN3_OK ? odin3_module_get(design, top) : NULL, top};
    st = st == ODIN3_OK ? act_model_build(&md, odin3_module_get(design, top), &ob) : st;
    if (st == ODIN3_OK) {
        warn_clocks(&md);
        st = read_inputs(&md, &ob, &nm);
    }
    st = st == ODIN3_OK ? act_solve(&md, &ob) : st;
    if (st == ODIN3_OK) {
        log_opaque(&md);
        st = write_outputs(&md, &nm, &ob, odin3_now_ms() - start);
    }
    if (st == ODIN3_OK) {
        uint64_t flops = 0;
        for (uint32_t id = 1; id < md.n_nets; id++) {
            flops += md.kind[id] == ACT_FF ? 1U : 0U;
        }
        odin3_log(ODIN3_LOG_INFO,
                  "activity: 1 module, %llu cells, %llu flip-flops (%llu in feedback), %u "
                  "iterations, %s, %.0f ms",
                  (unsigned long long)md.n_cells, (unsigned long long)flops,
                  (unsigned long long)md.ff_loop, md.iterations,
                  md.converged ? "converged" : "NOT converged", odin3_now_ms() - start);
    }
    act_model_free(&md);
    odin3_sim_netnames_free(&names);
    opts_free(&ob);
    return st;
}

const odin3_pass_def odin3_pass_activity = {
    "activity",
    "activity [--pi-prob P] [--pi-density D] [--pi-file F] [--vcd F [--vcd-scope S] [--vcd-clock "
    "C]] "
    "[--clock NAME] [--ff-start P] [--tol E] [--max-iter N] [--exact K] [--opaque-prob P] "
    "[--opaque-density D] [--opaque-type NAME]... [--out F.act] [--name-style "
    "short|provenance|hash] [--json F] "
    "[--no-attrs]: vectorless static probability and transition density of every net (VPR .act, "
    "JSON, module attributes odin3:activity_*)",
    activity_run,
    NULL,
    false,
    ODIN3_PASS_PROCESSES_REFUSE};
