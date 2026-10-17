/*
 * hardmap.c — the driver of the `hardmap` pass (3C spec H-8, H-9, H-10): the explicit-instance
 * pre-check over the whole design (M-6), then, module by module in creation order and cell by
 * cell in node-ID order, the `$mul` phase. PR 3C-2 adds `$add`/`$sub` and PR 3C-3 the memories
 * as further phases in spec order (H-8).
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

/* M-6: no explicit `multiply` wider than the arch's, in any module; nothing is changed. */
static odin3_status precheck_multiply(hm_ctx *hc) {
    odin3_celltype_id type = {0};
    if (!hm_find_type(hc->design, "multiply", &type)) {
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
                st = hm_check_multiply(hc, node);
            }
        }
    }
    return st;
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

static odin3_status all_mul_phases(hm_ctx *hc) {
    odin3_celltype_id mul = {0};
    if (!hm_find_type(hc->design, "$mul", &mul)) {
        return ODIN3_OK;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t mi = 1; st == ODIN3_OK && mi < odin3_design_module_end(hc->design); mi++) {
        odin3_module *mod = module_at(hc->design, mi);
        st = mod != NULL ? mul_phase(hc, mod, mul) : st;
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
    odin3_status st = precheck_multiply(&hc);
    st = st == ODIN3_OK ? all_mul_phases(&hc) : st;
    if (st == ODIN3_OK) {
        report(&hc);
    }
    if (stats != NULL) {
        *stats = hc.stats;
    }
    hm_free(&hc);
    return st;
}
