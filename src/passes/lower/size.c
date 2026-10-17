/*
 * size.c — the size model of `lower` (3B spec §11; plan Task 1, point (d)): saturating helpers and
 * the pre-flight against the caps. The per-rule formulas (upper bounds on the gate nodes a builder
 * creates, from the widths alone) sit beside their builders' tests and are listed in the dispatch
 * table; the unit tests assert `created <= estimate` and print the ratio.
 */
#include "passes/lower/lower_int.h"
#include "passes/lower/lower_test.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdint.h>

uint64_t lw_sat_add(uint64_t lhs, uint64_t rhs) {
    return lhs > UINT64_MAX - rhs ? UINT64_MAX : lhs + rhs;
}

uint64_t lw_sat_mul(uint64_t lhs, uint64_t rhs) {
    return rhs != 0 && lhs > UINT64_MAX / rhs ? UINT64_MAX : lhs * rhs;
}

/* The canonical constants a cell may add to a module: $_CONST0_, $_CONST1_, $_CONSTX_. */
enum { CONST_SLACK = 3 };

odin3_status lw_check_size(const lw_ctx *lw, const lw_cell *cell, uint64_t estimate) {
    uint64_t nodes = lw_sat_add(estimate, CONST_SLACK);
    odin3_lower_limits caps = odin3_lower_limits_now();
    uint64_t cap = caps.max_cell_nodes;
    uint64_t total = nodes;
    if (nodes <= caps.max_cell_nodes) {
        total = lw_sat_add(lw->live_nodes, nodes);
        cap = caps.max_module_nodes;
    }
    if (total <= cap) {
        return ODIN3_OK;
    }
    char label[LW_LABEL_LEN];
    lw_node_label(lw, cell->node, label, sizeof label);
    odin3_log(ODIN3_LOG_ERROR,
              "lower: module '%s': '%s' '%s' would create %" PRIu64 " nodes (cap %" PRIu64 ")",
              odin3_strtab_get(odin3_design_strtab(lw->design), odin3_module_name(lw->module)),
              cell->def->name, label, total, cap);
    return ODIN3_ERR_INVALID_ARG;
}

/* --- per-rule formulas (spec §4): the exact gate counts, which are the upper bounds ----------- */

static uint64_t y_width(const lw_cell *cell) {
    return cell->port[cell->out_port].n;
}

uint64_t lw_est_not(const lw_cell *cell) {
    return y_width(cell);
}

uint64_t lw_est_and(const lw_cell *cell) {
    return y_width(cell);
}

uint64_t lw_est_or(const lw_cell *cell) {
    return y_width(cell);
}

uint64_t lw_est_xor(const lw_cell *cell) {
    return y_width(cell);
}

uint64_t lw_est_xnor(const lw_cell *cell) {
    return y_width(cell);
}

uint64_t lw_est_mux(const lw_cell *cell) {
    return y_width(cell);
}

/* A pairing tree over n nets has n - 1 gates (none for one net). */
static uint64_t tree_gates(uint64_t count) {
    return count - 1;
}

uint64_t lw_est_reduce_and(const lw_cell *cell) {
    return tree_gates(cell->port[0].n);
}

uint64_t lw_est_reduce_or(const lw_cell *cell) {
    return tree_gates(cell->port[0].n);
}

uint64_t lw_est_reduce_xor(const lw_cell *cell) {
    return tree_gates(cell->port[0].n);
}

uint64_t lw_est_reduce_xnor(const lw_cell *cell) {
    return tree_gates(cell->port[0].n) + 1;
}

uint64_t lw_est_logic_not(const lw_cell *cell) {
    return tree_gates(cell->port[0].n) + 1;
}

uint64_t lw_est_logic_and(const lw_cell *cell) {
    return tree_gates(cell->port[0].n) + tree_gates(cell->port[1].n) + 1;
}

uint64_t lw_est_logic_or(const lw_cell *cell) {
    return lw_est_logic_and(cell);
}

/* $ne: W xors and a tree of W - 1 ors; $eq: one more $_NOT_. */
uint64_t lw_est_ne(const lw_cell *cell) {
    uint64_t width = cell->port[0].n > cell->port[1].n ? cell->port[0].n : cell->port[1].n;
    return width + tree_gates(width);
}

uint64_t lw_est_eq(const lw_cell *cell) {
    return lw_est_ne(cell) + 1;
}
