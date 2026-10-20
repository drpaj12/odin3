/*
 * lpmmap.c — the driver of the `lpmmap` pass (5.1 spec P-4, H-8): module by module in creation
 * order, cell by cell in node-ID order, the kinds in this order: `$add`/`$sub`, `$mul`, the six
 * comparisons, `$mem`.
 */
#include "passes/lpmmap/lpmmap.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/lpmmap/lm_kit.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>

/* One kind of source cell: the type names it looks for and the rule that maps one cell. */
/* The most type names one kind lists. */
enum { LM_KIND_TYPES = 6 };

typedef struct lm_kind {
    const char *types[LM_KIND_TYPES];
    odin3_status (*rule)(lm_ctx *lc, odin3_node_id src);
} lm_kind;

static odin3_module *module_at(odin3_design *design, uint32_t index) {
    return odin3_module_get(design, (odin3_module_id){index});
}

/* True when the type of node is one of the kind's. */
static bool kind_has(const odin3_celltype_id *ids, size_t n, odin3_celltype_id type) {
    for (size_t i = 0; i < n; i++) {
        if (ids[i].v != 0 && ids[i].v == type.v) {
            return true;
        }
    }
    return false;
}

static uint64_t total_made(const lm_ctx *lc) {
    return lc->stats.add_sub + lc->stats.mult + lc->stats.compare + lc->stats.ram;
}

/* The phase of one kind in one module: every cell of the kind live at the start, in node-ID order.
 */
static odin3_status phase(lm_ctx *lc, odin3_module *mod, const lm_kind *kind,
                          const odin3_celltype_id *ids, size_t n_ids) {
    uint32_t end = odin3_module_node_end(mod);
    lm_set_module(lc, mod);
    odin3_status st = lm_budget_check(lc);
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(mod, node) || !kind_has(ids, n_ids, odin3_node_type(mod, node))) {
            continue;
        }
        uint64_t before = total_made(lc);
        st = kind->rule(lc, node);
        if (st == ODIN3_OK && total_made(lc) != before) {
            char label[ODIN3_LOG_BUF / 4];
            lm_label(lc, node, label, sizeof label);
            odin3_log(ODIN3_LOG_DEBUG, "lpmmap: cell '%s' mapped to an LPM block", label);
        }
    }
    return st;
}

static const lm_kind k_kinds[] = {
    {{"$add", "$sub"}, lm_map_addsub},
    {{"$mul"}, lm_map_mult},
    {{"$lt", "$le", "$gt", "$ge", "$eq", "$ne"}, lm_map_compare},
    {{"$mem"}, lm_map_mem},
};

/* Runs one kind over every module. A kind whose types the design does not know is skipped. */
static odin3_status run_kind(lm_ctx *lc, const lm_kind *kind) {
    odin3_celltype_id ids[LM_KIND_TYPES] = {{0}};
    size_t found = 0;
    for (size_t i = 0; i < LM_KIND_TYPES && kind->types[i] != NULL; i++) {
        found += lm_find_type(lc->design, kind->types[i], &ids[found]) ? 1U : 0U;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t mi = 1; found > 0 && st == ODIN3_OK && mi < odin3_design_module_end(lc->design);
         mi++) {
        odin3_module *mod = module_at(lc->design, mi);
        st = mod != NULL ? phase(lc, mod, kind, ids, found) : st;
    }
    return st;
}

static void log_info(const lm_ctx *lc) {
    odin3_log(ODIN3_LOG_INFO,
              "lpmmap: %" PRIu64 " lpm_add_sub, %" PRIu64 " lpm_mult, %" PRIu64
              " lpm_compare, %" PRIu64 " altsyncram; %" PRIu64 " cells left soft",
              lc->stats.add_sub, lc->stats.mult, lc->stats.compare, lc->stats.ram, lc->stats.soft);
}

odin3_status odin3_lpmmap_run(odin3_pass_ctx *ctx, odin3_design *design,
                              const odin3_lpmmap_opts *opts, odin3_lpmmap_stats *stats) {
    if (ctx == NULL || design == NULL) {
        return ODIN3_ERR_INVALID_ARG;
    }
    lm_ctx lc;
    lm_init(&lc, ctx, design, opts != NULL ? opts->family : NULL);
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < sizeof k_kinds / sizeof k_kinds[0]; i++) {
        st = run_kind(&lc, &k_kinds[i]);
    }
    if (st == ODIN3_OK) {
        log_info(&lc);
    }
    if (stats != NULL) {
        *stats = lc.stats;
    }
    lm_free(&lc);
    return st;
}
