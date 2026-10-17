/*
 * hardmap.c — the driver of the `hardmap` pass (3C spec H-8, H-9, H-10): the explicit-instance
 * pre-check over the whole design (M-6), then, module by module in creation order and cell by
 * cell in node-ID order, the `$mul` phase, the explicit `multiply` phase and the `$add`/`$sub`
 * phase, then the explicit RAM instances and the inferred `$mem` (H-8; PR 3C-3).
 */
#include "passes/hardmap/hardmap.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/hardmap/hm_kit.h"
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
    odin3_status st = precheck_type(hc, "multiply", hm_check_multiply);
    st = st == ODIN3_OK ? precheck_type(hc, "single_port_ram", hm_check_ram) : st;
    return st == ODIN3_OK ? precheck_type(hc, "dual_port_ram", hm_check_ram) : st;
}

/* The `$mul` phase of one module: every `$mul` live at the start, in node-ID order. */
static odin3_status mul_phase(hm_ctx *hc, odin3_module *mod, odin3_celltype_id mul) {
    uint32_t end = odin3_module_node_end(mod);
    hm_set_module(hc, mod);
    odin3_status st = hm_budget_check(hc);
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(mod, node) || odin3_node_type(mod, node).v != mul.v) {
            continue;
        }
        uint64_t before = hc->stats.multiply;
        st = hm_map_mul(hc, node);
        if (st == ODIN3_OK && hc->stats.multiply != before) {
            char label[ODIN3_LOG_BUF / 4];
            hm_label(hc, node, label, sizeof label);
            odin3_log(ODIN3_LOG_DEBUG, "hardmap: '$mul' cell '%s' mapped to a multiply", label);
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

static odin3_status all_mul_phases(hm_ctx *hc) {
    odin3_celltype_id mul = {0};
    odin3_celltype_id multiply = {0};
    bool have_mul = hm_find_type(hc->design, "$mul", &mul);
    bool have_multiply = hm_find_type(hc->design, "multiply", &multiply);
    odin3_status st = ODIN3_OK;
    for (uint32_t mi = 1; have_mul && st == ODIN3_OK && mi < odin3_design_module_end(hc->design);
         mi++) {
        odin3_module *mod = module_at(hc->design, mi);
        st = mod != NULL ? mul_phase(hc, mod, mul) : st;
    }
    for (uint32_t mi = 1;
         have_multiply && st == ODIN3_OK && mi < odin3_design_module_end(hc->design); mi++) {
        odin3_module *mod = module_at(hc->design, mi);
        st = mod != NULL ? explicit_phase(hc, mod, multiply) : st;
    }
    return st;
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
            odin3_log(ODIN3_LOG_DEBUG, "hardmap: '%s' cell '%s' mapped to an adder chain",
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
            odin3_log(ODIN3_LOG_DEBUG, "hardmap: '%s' cell '%s' mapped to %" PRIu64 " slices",
                      type.v == types[0].v ? "single_port_ram" : "dual_port_ram", label,
                      hc->stats.sp_slices + hc->stats.dp_slices - before);
        }
    }
    return st;
}

static odin3_status all_ram_phases(hm_ctx *hc) {
    odin3_celltype_id types[2] = {{0}, {0}};
    bool have_sp = hm_find_type(hc->design, "single_port_ram", &types[0]);
    bool have_dp = hm_find_type(hc->design, "dual_port_ram", &types[1]);
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
                odin3_log(ODIN3_LOG_DEBUG, "hardmap: '$mem' cell '%s' mapped to %" PRIu64 " slices",
                          label, hc->stats.sp_slices + hc->stats.dp_slices - before);
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

static void report(hm_ctx *hc) {
    const odin3_hardmap_stats *st = &hc->stats;
    odin3_log(ODIN3_LOG_INFO,
              "hardmap: %" PRIu64 " multiply, %" PRIu64 " adder in %" PRIu64 " chains, %" PRIu64
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

odin3_status odin3_hardmap_run(odin3_pass_ctx *ctx, odin3_design *design,
                               odin3_hardmap_stats *stats) {
    hm_ctx hc;
    hm_init(&hc, ctx, design);
    odin3_status st = precheck(&hc);
    st = st == ODIN3_OK ? all_mul_phases(&hc) : st;
    st = st == ODIN3_OK ? all_add_phases(&hc) : st;
    st = st == ODIN3_OK ? all_ram_phases(&hc) : st;
    st = st == ODIN3_OK ? all_mem_phases(&hc) : st;
    if (st == ODIN3_OK) {
        report(&hc);
    }
    if (stats != NULL) {
        *stats = hc.stats;
    }
    hm_free(&hc);
    return st;
}
