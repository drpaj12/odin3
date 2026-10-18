/*
 * lower.c — the driver of `lower` (3B spec §3.1, §10, §12; plan Task 1): modules in module-ID
 * order, nodes in ID order, dispatch by granularity and cell type, one transaction per word cell,
 * the INFO and DEBUG lines, the netlist-view post-condition and the test hooks (lower_test.h).
 */
#include "passes/lower/lower.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/lower/lower_int.h"
#include "passes/lower/lower_test.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* --- caps and test hooks (lower_test.h) --------------------------------------------------- */

static const odin3_lower_limits k_default_limits = {
    ODIN3_LOWER_MAX_CELL_NODES, ODIN3_LOWER_MAX_MODULE_NODES, ODIN3_LOWER_MAX_MEM_BITS};
static odin3_lower_limits g_limits = {ODIN3_LOWER_MAX_CELL_NODES, ODIN3_LOWER_MAX_MODULE_NODES,
                                      ODIN3_LOWER_MAX_MEM_BITS};

static uint64_t clamp_cap(uint64_t value, uint64_t dflt) {
    if (value == 0 || value > dflt) {
        return dflt;
    }
    return value;
}

void odin3_lower_test_set_limits(const odin3_lower_limits *limits) {
    if (limits == NULL) {
        g_limits = k_default_limits;
        return;
    }
    g_limits.max_cell_nodes = clamp_cap(limits->max_cell_nodes, k_default_limits.max_cell_nodes);
    g_limits.max_module_nodes =
        clamp_cap(limits->max_module_nodes, k_default_limits.max_module_nodes);
    g_limits.max_mem_bits = clamp_cap(limits->max_mem_bits, k_default_limits.max_mem_bits);
}

odin3_lower_limits odin3_lower_limits_now(void) {
    return g_limits;
}

/* The types the soft-logic line counts, sorted by name (indices of lw_totals.soft). */
static const char *const k_soft[LW_N_SOFT] = {"$add", "$mem", "$mul", "$sub"};

/* --- one word cell ------------------------------------------------------------------------- */

static const char *module_name(const lw_ctx *lw) {
    return odin3_strtab_get(odin3_design_strtab(lw->design), odin3_module_name(lw->module));
}

/* The build phase: load, size check, builder. On failure the caller rolls back. */
static odin3_status build_cell(lw_ctx *lw, odin3_node_id node, const lw_rule *rule, lw_cell *cell,
                               lw_bits *outs) {
    odin3_status st = lw_begin(lw, node);
    st = st == ODIN3_OK ? lw_load_cell(lw, node, cell) : st;
    st = st == ODIN3_OK ? lw_check_size(lw, cell, rule->estimate(cell)) : st;
    return st == ODIN3_OK ? rule->build(lw, cell, outs) : st;
}

static odin3_status lower_cell(lw_ctx *lw, odin3_node_id node, const lw_rule *rule) {
    lw_cell cell;
    lw_bits outs;
    lw_bits_init(&outs);
    uint64_t nodes_before = lw->totals.nodes;
    char label[LW_LABEL_LEN];
    lw_node_label(lw, node, label, sizeof label);
    odin3_status st = build_cell(lw, node, rule, &cell, &outs);
    st = st == ODIN3_OK ? lw_commit(lw, &cell, &outs) : st;
    if (st != ODIN3_OK) {
        if (lw->created.len > 0 || lw->created_nets.len > 0) {
            if (odin3_node_live(lw->module, node)) {
                lw_rollback(lw); /* the word cell is intact: undo what was built */
            }
        }
        lw_bits_free(&outs);
        return st;
    }
    lw_end(lw);
    lw->totals.cells++;
    for (size_t i = 0; i < LW_N_SOFT; i++) {
        lw->totals.soft[i] += strcmp(cell.def->name, k_soft[i]) == 0 ? 1U : 0U;
    }
    odin3_log(ODIN3_LOG_DEBUG, "lower: module '%s': %s '%s' -> %" PRIu64 " bit cells",
              module_name(lw), cell.def->name, label, lw->totals.nodes - nodes_before);
    lw_bits_free(&outs);
    return ODIN3_OK;
}

/* --- one module ---------------------------------------------------------------------------- */

typedef struct lw_rule_slot {
    const lw_rule *rule; /* NULL: no rule */
} lw_rule_slot;

typedef struct lw_rules {
    lw_rule_slot *by_type; /* indexed by cell type ID */
    uint32_t n_types;
} lw_rules;

static void rules_free(lw_rules *rules) {
    odin3_util_free(rules->by_type);
    rules->by_type = NULL;
}

/* Resolves every word type of the design to its rule once (plugin types get NULL). */
static odin3_status rules_build(const odin3_design *design, lw_rules *rules) {
    uint32_t count = 0;
    while (odin3_celltype_get(design, (odin3_celltype_id){count + 1}) != NULL) {
        count++;
    }
    rules->n_types = count;
    rules->by_type = odin3_util_calloc(sizeof(lw_rule_slot) * ((size_t)count + 1));
    if (rules->by_type == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t id = 1; id <= count; id++) {
        const odin3_celltype_def *def = odin3_celltype_get(design, (odin3_celltype_id){id});
        rules->by_type[id].rule = def->gran == ODIN3_GRAN_WORD ? lw_rule_find(def->name) : NULL;
    }
    return ODIN3_OK;
}

/* A node's rule when it is a live word cell: sets *is_word; NULL for "word cell without a rule". */
static const lw_rule *rule_of(const lw_ctx *lw, const lw_rules *rules, odin3_node_id node,
                              bool *is_word) {
    odin3_celltype_id type = odin3_node_type(lw->module, node);
    const odin3_celltype_def *def = odin3_celltype_get(lw->design, type);
    *is_word = def != NULL && def->gran == ODIN3_GRAN_WORD;
    return *is_word && type.v <= rules->n_types ? rules->by_type[type.v].rule : NULL;
}

/* The netlist view of IR-9, as a post-condition (spec §12.3): no live word cell is left. */
static odin3_status postcondition(const lw_ctx *lw) {
    uint32_t end = odin3_module_node_end(lw->module);
    for (uint32_t id = 1; id < end; id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(lw->module, node)) {
            continue;
        }
        const odin3_celltype_def *def =
            odin3_celltype_get(lw->design, odin3_node_type(lw->module, node));
        if (def != NULL && def->gran == ODIN3_GRAN_WORD) {
            char label[LW_LABEL_LEN];
            lw_node_label(lw, node, label, sizeof label);
            odin3_log(ODIN3_LOG_ERROR,
                      "lower: module '%s': node '%s' '%s' is not in the netlist view after "
                      "lowering",
                      module_name(lw), def->name, label);
            return ODIN3_ERR_CHECK;
        }
    }
    return ODIN3_OK;
}

static odin3_status lower_module(lw_ctx *lw, const lw_rules *rules, odin3_module *module) {
    lw_ctx_enter(lw, module);
    uint64_t cells_before = lw->totals.cells;
    uint64_t nodes_before = lw->totals.nodes;
    uint32_t end = odin3_module_node_end(module); /* nodes made below are bit cells */
    odin3_status st = ODIN3_OK;
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_node_id node = {id};
        if (!odin3_node_live(module, node)) {
            continue;
        }
        bool is_word = false;
        const lw_rule *rule = rule_of(lw, rules, node, &is_word);
        if (!is_word) {
            continue;
        }
        if (rule == NULL) {
            const odin3_celltype_def *def =
                odin3_celltype_get(lw->design, odin3_node_type(module, node));
            lw_log_no_rule(lw, node, def->name);
            return ODIN3_ERR_INVALID_ARG;
        }
        st = lower_cell(lw, node, rule);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    if (lw->totals.cells > cells_before) {
        lw->totals.modules++;
        odin3_log(ODIN3_LOG_INFO,
                  "lower: module '%s': lowered %" PRIu64 " word cells to %" PRIu64 " bit cells",
                  module_name(lw), lw->totals.cells - cells_before,
                  lw->totals.nodes - nodes_before);
    }
    return postcondition(lw);
}

/* --- the run ------------------------------------------------------------------------------- */

/* The soft-logic line of spec §10: the types a missed `hardmap` leaves, sorted by name. */
static void log_soft(const lw_ctx *lw) {
    const uint64_t *counts = lw->totals.soft;
    odin3_strbuf line;
    odin3_strbuf_init(&line);
    bool any = false;
    for (size_t i = 0; i < sizeof k_soft / sizeof k_soft[0]; i++) {
        if (counts[i] == 0) {
            continue;
        }
        if (odin3_strbuf_appendf(&line, "%s%" PRIu64 " %s", any ? ", " : "", counts[i],
                                 k_soft[i]) != ODIN3_OK) {
            odin3_strbuf_free(&line);
            return;
        }
        any = true;
    }
    if (any) {
        odin3_log(ODIN3_LOG_INFO, "lower: lowered %s as soft logic", line.data);
    }
    odin3_strbuf_free(&line);
}

static void report_run(lw_ctx *lw) {
    if (lw->totals.cells == 0) {
        odin3_log(ODIN3_LOG_INFO, "lower: nothing to lower");
        return;
    }
    log_soft(lw);
    if (lw->totals.async_bits > 0) {
        odin3_log(ODIN3_LOG_WARN,
                  "lower: asynchronous reset/set/load made synchronous on %" PRIu64
                  " flip-flop bits in %" PRIu64
                  " cells (PHASE3 decision 19); the netlist differs from the RTL on reset timing; "
                  "see --report",
                  lw->totals.async_bits, lw->totals.async_cells);
    }
    odin3_strbuf info;
    odin3_strbuf_init(&info);
    uint32_t str = 0;
    if (odin3_strbuf_appendf(&info, "modules=%" PRIu64 " cells=%" PRIu64 " nodes=%" PRIu64,
                             lw->totals.modules, lw->totals.cells, lw->totals.nodes) == ODIN3_OK &&
        odin3_design_intern(lw->design, odin3_bytes_cstr(info.data), &str) == ODIN3_OK) {
        (void)odin3_passrun_set_info(lw->pass, str);
    }
    odin3_strbuf_free(&info);
}

/* True when module holds a live word cell without a rule, one `--async error` refuses or a `$mem`
 * the pre-scan refuses (spec §7); logs every such cell (spec 12.2: the knowable failure comes
 * before any change). */
static bool module_has_ruleless(lw_ctx *lw, const lw_rules *rules, odin3_module *module) {
    lw_ctx_enter(lw, module);
    bool found = false;
    uint32_t end = odin3_module_node_end(module);
    for (uint32_t id = 1; id < end; id++) {
        odin3_node_id node = {id};
        bool is_word = false;
        const lw_rule *rule =
            odin3_node_live(module, node) ? rule_of(lw, rules, node, &is_word) : NULL;
        if (!is_word) {
            continue;
        }
        const odin3_celltype_def *def =
            odin3_celltype_get(lw->design, odin3_node_type(module, node));
        if (rule != NULL) {
            found = lw_ff_async_refused(lw, node, def->name) || found;
            found = lw_mem_refused(lw, node, def->name) || found;
            continue;
        }
        lw_log_no_rule(lw, node, def->name);
        found = true;
    }
    return found;
}

/* Reports every word cell of the design that has no rule, before anything is lowered. */
static odin3_status prescan(lw_ctx *lw, const lw_rules *rules) {
    bool found = false;
    uint32_t end = odin3_design_module_end(lw->design);
    for (uint32_t id = 1; id < end; id++) {
        odin3_module *module = odin3_module_get(lw->design, (odin3_module_id){id});
        found = module_has_ruleless(lw, rules, module) || found;
    }
    return found ? ODIN3_ERR_INVALID_ARG : ODIN3_OK;
}

odin3_status odin3_lower_run(odin3_pass_ctx *ctx, odin3_design *design,
                             const odin3_lower_opts *opts) {
    if (ctx == NULL || design == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "lower: no pass context or design");
        return ODIN3_ERR_INVALID_ARG;
    }
    static const odin3_lower_opts k_none = {false, false, 0, NULL, false, ""};
    lw_ctx lw;
    lw_rules rules = {NULL, 0};
    odin3_status st = lw_ctx_init(&lw, ctx, design, opts != NULL ? opts : &k_none);
    st = st == ODIN3_OK ? rules_build(design, &rules) : st;
    st = st == ODIN3_OK ? prescan(&lw, &rules) : st;
    if (st == ODIN3_OK) {
        odin3_lower_snapshot_before(lw.opts, design);
    }
    uint32_t end = odin3_design_module_end(design);
    for (uint32_t id = 1; st == ODIN3_OK && id < end; id++) {
        odin3_module *module = odin3_module_get(design, (odin3_module_id){id});
        st = lower_module(&lw, &rules, module);
    }
    if (st == ODIN3_OK) {
        odin3_lower_snapshot_after(lw.opts, design);
        report_run(&lw);
        st = lw_report_write(&lw);
    }
    rules_free(&rules);
    lw_ctx_free(&lw);
    return st;
}
