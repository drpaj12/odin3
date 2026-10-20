/*
 * hardmap.c — the driver of the `partial_map` pass (3C spec H-8, H-9, H-10): the explicit-instance
 * pre-check over the whole design (M-6), then, module by module in creation order and cell by
 * cell in node-ID order, the `$mul` phase, the explicit `multiply` phase and the `$add`/`$sub`
 * phase, then the explicit RAM instances and the inferred `$mem` (H-8; PR 3C-3).
 */
#include "passes/partial_map/hardmap.h"
#include "frontends/arch/arch.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/partial_map/hm_kit.h"
#include "passes/partial_map/mem.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

/* The module IDs run from 1 to odin3_design_module_end (exclusive). */
static odin3_module *module_at(odin3_design *design, uint32_t index) {
    return odin3_module_get(design, (odin3_module_id){index});
}

typedef odin3_status (*check_fn)(hm_ctx *hc, odin3_node_id node);

/* Runs check on every live node of the type named name, in every module; nothing is changed. */
static odin3_status precheck_type(hm_ctx *hc, const char *name, check_fn check) {
    odin3_celltype_id type = {0};
    if (!hm_find_type(hc->design, name, &type)) {
        return ODIN3_OK;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t mi = 1; st == ODIN3_OK && mi < odin3_design_module_end(hc->design); mi++) {
        odin3_module *mod = module_at(hc->design, mi);
        if (mod == NULL) {
            continue;
        }
        hm_set_module(hc, mod);
        for (uint32_t id = 1; st == ODIN3_OK && id < odin3_module_node_end(mod); id++) {
            odin3_node_id node = {id};
            if (odin3_node_live(mod, node) && odin3_node_type(mod, node).v == type.v) {
                st = check(hc, node);
            }
        }
    }
    return st;
}

/* M-6: no explicit `multiply` wider than the arch's (and of the shape hardmap indexes by position)
 * in any module; R-2: no RAM instance with more address bits than hardmap splits; nothing is
 * changed. */
static odin3_status precheck(hm_ctx *hc) {
    odin3_status st =
        hc->rules.have_mul ? precheck_type(hc, "multiply", hm_check_multiply) : ODIN3_OK;
    st = st == ODIN3_OK && hc->rules.have_sp ? precheck_type(hc, "single_port_ram", hm_check_ram)
                                             : st;
    return st == ODIN3_OK && hc->rules.have_dp ? precheck_type(hc, "dual_port_ram", hm_check_ram)
                                               : st;
}

/* The `$mul` phase of one module: every live `$mul`, in node-ID order. The pieces of an oversize
 * split (X-4) are new `$mul` nodes with higher IDs, so the end is read again at each step: that is
 * the worklist of the split, with no recursion. A piece left soft is visited once. */
static odin3_status mul_phase(hm_ctx *hc, odin3_module *mod, odin3_celltype_id mul) {
    hm_set_module(hc, mod);
    odin3_status st = hm_budget_check(hc);
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_module_node_end(mod); id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(mod, node) || odin3_node_type(mod, node).v != mul.v) {
            continue;
        }
        uint64_t before = hc->stats.multiply;
        st = hm_map_mul(hc, node);
        if (st == ODIN3_OK && hc->stats.multiply != before) {
            char label[ODIN3_LOG_BUF / 4];
            hm_label(hc, node, label, sizeof label);
            odin3_log(ODIN3_LOG_DEBUG, "partial_map: '$mul' cell '%s' mapped to a multiply", label);
        }
    }
    return st;
}

/* The explicit `multiply` instances of one module, live at the start (M-6 as amended): after the
 * `$mul` phase, so a `$mul` made from a WIDTH 2 instance is not visited again. */
static odin3_status explicit_phase(hm_ctx *hc, odin3_module *mod, odin3_celltype_id type) {
    uint32_t end = odin3_module_node_end(mod);
    hm_set_module(hc, mod);
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(mod, node) && odin3_node_type(mod, node).v == type.v) {
            st = hm_map_explicit(hc, node);
        }
    }
    return st;
}

/* The explicit `multiply` instances wider than W of one module (X-6): rewritten into `$mul` pieces
 * before the `$mul` phase, which then reaches them. */
static odin3_status oversize_phase(hm_ctx *hc, odin3_module *mod, odin3_celltype_id type) {
    uint32_t end = odin3_module_node_end(mod);
    hm_set_module(hc, mod);
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(mod, node) && odin3_node_type(mod, node).v == type.v) {
            st = hm_split_explicit(hc, node);
        }
    }
    return st;
}

typedef odin3_status (*phase_fn)(hm_ctx *hc, odin3_module *mod, odin3_celltype_id type);

/* Runs phase on every module in creation order, for the cell type named name (none registered:
 * nothing to do). */
static odin3_status each_module(hm_ctx *hc, const char *name, phase_fn phase) {
    odin3_celltype_id type = {0};
    odin3_status st = ODIN3_OK;
    if (!hm_find_type(hc->design, name, &type)) {
        return st;
    }
    for (uint32_t mi = 1; st == ODIN3_OK && mi < odin3_design_module_end(hc->design); mi++) {
        odin3_module *mod = module_at(hc->design, mi);
        st = mod != NULL ? phase(hc, mod, type) : st;
    }
    return st;
}

/* H-8: the explicit instances wider than W (X-6), the `$mul` cells (with the pieces of a split),
 * then the other explicit instances. */
static odin3_status all_mul_phases(hm_ctx *hc) {
    odin3_celltype_id none = {0};
    bool have_mul = hm_find_type(hc->design, "$mul", &none);
    odin3_status st = have_mul ? each_module(hc, "multiply", oversize_phase) : ODIN3_OK;
    st = st == ODIN3_OK && have_mul ? each_module(hc, "$mul", mul_phase) : st;
    return st == ODIN3_OK ? each_module(hc, "multiply", explicit_phase) : st;
}

/* The `$add` / `$sub` phase of one module (H-8, after every `$mul`): the cells live at the start,
 * in node-ID order. */
static odin3_status add_phase(hm_ctx *hc, odin3_module *mod, const odin3_celltype_id types[2]) {
    uint32_t end = odin3_module_node_end(mod);
    hm_set_module(hc, mod);
    odin3_status st = hm_budget_check(hc);
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_node_id node = {id};
        odin3_celltype_id type = odin3_node_type(mod, node);
        if (!odin3_node_live(mod, node) || (type.v != types[0].v && type.v != types[1].v)) {
            continue;
        }
        uint64_t before = hc->stats.chains;
        st = hm_map_adder(hc, node);
        if (st == ODIN3_OK && hc->stats.chains != before) {
            char label[ODIN3_LOG_BUF / 4];
            hm_label(hc, node, label, sizeof label);
            odin3_log(ODIN3_LOG_DEBUG, "partial_map: '%s' cell '%s' mapped to an adder chain",
                      type.v == types[0].v ? "$add" : "$sub", label);
        }
    }
    return st;
}

static odin3_status all_add_phases(hm_ctx *hc) {
    odin3_celltype_id types[2] = {{0}, {0}};
    if (!hm_find_type(hc->design, "$add", &types[0]) ||
        !hm_find_type(hc->design, "$sub", &types[1])) {
        return ODIN3_OK;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t mi = 1; st == ODIN3_OK && mi < odin3_design_module_end(hc->design); mi++) {
        odin3_module *mod = module_at(hc->design, mi);
        st = mod != NULL ? add_phase(hc, mod, types) : st;
    }
    return st;
}

/* The RAM phase of one module (H-8, after every `$add`/`$sub`): the instances live at the start,
 * in node-ID order. The slices it makes have higher IDs and the arch's shape. */
static odin3_status ram_phase(hm_ctx *hc, odin3_module *mod, const odin3_celltype_id types[2]) {
    uint32_t end = odin3_module_node_end(mod);
    hm_set_module(hc, mod);
    odin3_status st = hm_budget_check(hc);
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_node_id node = {id};
        odin3_celltype_id type = odin3_node_type(mod, node);
        if (!odin3_node_live(mod, node) || (type.v != types[0].v && type.v != types[1].v)) {
            continue;
        }
        uint64_t before = hc->stats.sp_slices + hc->stats.dp_slices;
        st = hm_map_ram(hc, node);
        if (st == ODIN3_OK && hc->stats.sp_slices + hc->stats.dp_slices != before) {
            char label[ODIN3_LOG_BUF / 4];
            hm_label(hc, node, label, sizeof label);
            odin3_log(ODIN3_LOG_DEBUG, "partial_map: '%s' cell '%s' mapped to %" PRIu64 " slices",
                      type.v == types[0].v ? "single_port_ram" : "dual_port_ram", label,
                      hc->stats.sp_slices + hc->stats.dp_slices - before);
        }
    }
    return st;
}

static odin3_status all_ram_phases(hm_ctx *hc) {
    odin3_celltype_id types[2] = {{0}, {0}};
    bool have_sp = hc->rules.have_sp && hm_find_type(hc->design, "single_port_ram", &types[0]);
    bool have_dp = hc->rules.have_dp && hm_find_type(hc->design, "dual_port_ram", &types[1]);
    if (!have_sp && !have_dp) {
        return ODIN3_OK;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t mi = 1; st == ODIN3_OK && mi < odin3_design_module_end(hc->design); mi++) {
        odin3_module *mod = module_at(hc->design, mi);
        st = mod != NULL ? ram_phase(hc, mod, types) : st;
    }
    return st;
}

/* The `$mem` phase of one module (H-8, after the RAM instances): the cells live at the start, in
 * node-ID order. The RAMs it makes have higher IDs. */
static odin3_status mem_phase(hm_ctx *hc, odin3_module *mod, odin3_celltype_id mem) {
    uint32_t end = odin3_module_node_end(mod);
    hm_set_module(hc, mod);
    odin3_status st = hm_budget_check(hc);
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_node_id node = {id};
        if (odin3_node_live(mod, node) && odin3_node_type(mod, node).v == mem.v) {
            uint64_t before = hc->stats.sp_slices + hc->stats.dp_slices;
            st = hm_map_mem(hc, node);
            if (st == ODIN3_OK && hc->stats.sp_slices + hc->stats.dp_slices != before) {
                char label[ODIN3_LOG_BUF / 4];
                hm_label(hc, node, label, sizeof label);
                odin3_log(ODIN3_LOG_DEBUG,
                          "partial_map: '$mem' cell '%s' mapped to %" PRIu64 " slices", label,
                          hc->stats.sp_slices + hc->stats.dp_slices - before);
            }
        }
    }
    return st;
}

static odin3_status all_mem_phases(hm_ctx *hc) {
    odin3_celltype_id mem = {0};
    if (!hm_find_type(hc->design, "$mem", &mem)) {
        return ODIN3_OK;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t mi = 1; st == ODIN3_OK && mi < odin3_design_module_end(hc->design); mi++) {
        odin3_module *mod = module_at(hc->design, mi);
        st = mod != NULL ? mem_phase(hc, mod, mem) : st;
    }
    return st;
}

/* --- S-2, S-3: the rules, and what an arch without a block leaves soft ---------------------- */

/* The number of live nodes of the type named name over all modules (0 when it is not registered).
 */
static uint64_t count_cells(hm_ctx *hc, const char *name) {
    odin3_celltype_id type = {0};
    uint64_t found = 0;
    if (!hm_find_type(hc->design, name, &type)) {
        return 0;
    }
    for (uint32_t mi = 1; mi < odin3_design_module_end(hc->design); mi++) {
        const odin3_module *mod = module_at(hc->design, mi);
        for (uint32_t id = 1; mod != NULL && id < odin3_module_node_end(mod); id++) {
            odin3_node_id node = {id};
            found += odin3_node_live(mod, node) && odin3_node_type(mod, node).v == type.v ? 1U : 0U;
        }
    }
    return found;
}

/* R-8: the bits of every live `$mem` (SIZE x WIDTH), which an arch without RAM leaves to lower. */
static void info_bits(hm_ctx *hc) {
    odin3_celltype_id mem = {0};
    uint64_t bits = 0;
    if (!hm_find_type(hc->design, "$mem", &mem)) {
        return;
    }
    for (uint32_t mi = 1; mi < odin3_design_module_end(hc->design); mi++) {
        odin3_module *mod = module_at(hc->design, mi);
        hm_set_module(hc, mod);
        for (uint32_t id = 1; mod != NULL && id < odin3_module_node_end(mod); id++) {
            odin3_node_id node = {id};
            if (odin3_node_live(mod, node) && odin3_node_type(mod, node).v == mem.v) {
                const odin3_value *prm = odin3_node_param(mod, node, 0);
                bits += (uint64_t)prm[MP_SIZE].i * (uint64_t)prm[MP_WIDTH].i;
                pm_mem_warn_big(hc, node, "the architecture has no RAM model");
            }
        }
    }
    hm_set_module(hc, NULL); /* the loop left it on the last module */
    odin3_log(ODIN3_LOG_INFO, "partial_map: %" PRIu64 " bits of '$mem' left soft", bits);
}

static void info_absent(const odin3_arch *arch, const char *model, uint64_t count,
                        const char *cells) {
    odin3_log(ODIN3_LOG_INFO,
              "partial_map: architecture '%s' has no '%s' model; %" PRIu64
              " '%s' cells left to lower",
              pm_arch_name(arch), model, count, cells);
}

/* C-2 and C-3: a record with an `adder` model but no carry pack pattern gets one warning; with
 * one, the number of pack patterns reaching the adder's `cin` is reported as a fact, at DEBUG level
 * so that the pass log of the k6 record stays equal to the log without a record (identity gate). */
static void carry_report(const odin3_arch *arch, bool have_adder) {
    if (!have_adder) {
        return;
    }
    if (!odin3_arch_chain_pattern_reaches_adder(arch)) {
        odin3_log(ODIN3_LOG_WARN,
                  "partial_map: architecture '%s' has an 'adder' model but no carry pack pattern; "
                  "chains may not pack",
                  pm_arch_name(arch));
        return;
    }
    odin3_log(ODIN3_LOG_DEBUG, "partial_map: architecture '%s' has %" PRIu32 " carry pack patterns",
              pm_arch_name(arch), odin3_arch_adder_cin_patterns(arch));
}

/* S-2 and S-3: with an arch record the rules come from it, and a block whose model the library
 * does not hold is simply absent (one INFO line per kind); without a record the 3C values stand
 * and a missing type stays the H-3 error. */
static void setup_rules(hm_ctx *hc, const odin3_partial_map_opts *opts) {
    const odin3_arch *arch = odin3_design_arch(hc->design);
    hc->rules = pm_rules_from_arch(arch);
    if (opts != NULL && opts->min_add != 0) {
        hc->rules.min_add = opts->min_add; /* C-4 */
    }
    if (opts != NULL && opts->min_mult != 0) {
        hc->rules.min_mul = opts->min_mult; /* X-3 */
    }
    if (arch == NULL) {
        return;
    }
    odin3_celltype_id none = {0};
    bool reg_mul = hm_find_type(hc->design, "multiply", &none);
    bool reg_add = hm_find_type(hc->design, "adder", &none);
    bool reg_sp = hm_find_type(hc->design, "single_port_ram", &none);
    bool reg_dp = hm_find_type(hc->design, "dual_port_ram", &none);
    hc->rules.have_mul = hc->rules.have_mul && reg_mul;
    hc->rules.have_adder = hc->rules.have_adder && reg_add;
    hc->rules.have_sp = hc->rules.have_sp && reg_sp;
    hc->rules.have_dp = hc->rules.have_dp && reg_dp;
    if (!reg_mul) {
        info_absent(arch, "multiply", count_cells(hc, "$mul"), "$mul");
    }
    if (!reg_add) {
        info_absent(arch, "adder", count_cells(hc, "$add") + count_cells(hc, "$sub"),
                    "$add'/'$sub");
    }
    if (!hc->rules.have_sp && !hc->rules.have_dp) {
        info_absent(arch, "single_port_ram", count_cells(hc, "$mem"), "$mem");
        info_bits(hc);
    }
    carry_report(arch, hc->rules.have_adder);
}

/* R-8, S-5: `--mem-width arch` needs a record with RAM modes; without one it is a no-op. */
static void setup_mem_width(hm_ctx *hc, const odin3_partial_map_opts *opts) {
    if (opts == NULL || !opts->mem_width_arch) {
        return;
    }
    bool modes = hc->rules.n_modes[0] + hc->rules.n_modes[1] > 0;
    hc->rules.mem_arch = hc->rules.has_record && modes && (hc->rules.have_sp || hc->rules.have_dp);
    if (!hc->rules.mem_arch && !(hc->rules.has_record && !modes)) {
        odin3_log(ODIN3_LOG_WARN,
                  "partial_map: --mem-width arch needs an architecture record with RAM models; "
                  "ignored");
    }
}

static void report(hm_ctx *hc) {
    const odin3_hardmap_stats *st = &hc->stats;
    if (hc->b_splits > 0 && !hc->parmys_split_ext) {
        odin3_log(ODIN3_LOG_WARN,
                  "partial_map: %" PRIu64
                  " '$mul' cells split on operand B alone; Parmys sign-extends the low product "
                  "there (wrong when its top bit is set), partial_map zero-extends (D19)",
                  hc->b_splits);
    }
    odin3_log(ODIN3_LOG_INFO,
              "partial_map: %" PRIu64 " multiply, %" PRIu64 " adder in %" PRIu64 " chains, %" PRIu64
              " single_port_ram + %" PRIu64 " dual_port_ram slices; %" PRIu64 " cells left soft",
              st->multiply, st->adder, st->chains, st->sp_slices, st->dp_slices, st->soft);
    odin3_strbuf info;
    odin3_strbuf_init(&info);
    uint32_t str = 0;
    if (odin3_strbuf_appendf(&info,
                             "multiply=%" PRIu64 " adder=%" PRIu64 " chains=%" PRIu64
                             " sp_slices=%" PRIu64 " dp_slices=%" PRIu64 " soft=%" PRIu64,
                             st->multiply, st->adder, st->chains, st->sp_slices, st->dp_slices,
                             st->soft) == ODIN3_OK &&
        odin3_design_intern(hc->design, odin3_bytes_cstr(info.data), &str) == ODIN3_OK) {
        (void)odin3_passrun_set_info(hc->pass, str);
    }
    odin3_strbuf_free(&info);
}

odin3_status odin3_partial_map_run_opts(odin3_pass_ctx *ctx, odin3_design *design,
                                        const odin3_partial_map_opts *opts,
                                        odin3_hardmap_stats *stats) {
    hm_ctx hc;
    hm_init(&hc, ctx, design);
    hc.allow_unknown_blocks = opts != NULL && opts->allow_unknown_blocks;
    hc.parmys_split_ext = opts != NULL && opts->parmys_split_ext;
    setup_rules(&hc, opts);
    setup_mem_width(&hc, opts);
    odin3_status st = hm_generic_phase(&hc);
    st = st == ODIN3_OK ? precheck(&hc) : st;
    st = st == ODIN3_OK && hc.rules.have_mul ? all_mul_phases(&hc) : st;
    st = st == ODIN3_OK && hc.rules.have_adder ? all_add_phases(&hc) : st;
    st = st == ODIN3_OK ? all_ram_phases(&hc) : st;
    st = st == ODIN3_OK && (hc.rules.have_sp || hc.rules.have_dp) ? all_mem_phases(&hc) : st;
    if (st == ODIN3_OK) {
        report(&hc);
    }
    if (stats != NULL) {
        *stats = hc.stats;
    }
    hm_free(&hc);
    return st;
}

odin3_status odin3_partial_map_run(odin3_pass_ctx *ctx, odin3_design *design,
                                   odin3_hardmap_stats *stats) {
    return odin3_partial_map_run_opts(ctx, design, NULL, stats);
}

odin3_status odin3_hardmap_run(odin3_pass_ctx *ctx, odin3_design *design,
                               odin3_hardmap_stats *stats) {
    return odin3_partial_map_run(ctx, design, stats);
}
