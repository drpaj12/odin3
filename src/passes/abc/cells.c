/*
 * cells.c — the gate-mapped result of abc --gates (gatemap spec 4.3; plan Task 11, Review Focus
 * 5): a gate whose name, input count or function does not match its library cell is a malformed
 * network, so pin-order drift never reaches the splice.
 */
#include "passes/abc/cells.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "techlib/libcells.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    FIRST_OBJ = 2, /* objects 0 and 1 are the constants */
    MINI_NULL = 0x7FFFFFFF,
    MINI_NULL2 = 0x7FFFFFFE,
    WHY_BYTES = 320,
    TABLE_BITS = 64
};

#define NO_CELL UINT32_MAX /* a gate name that is no cell of the library */

static odin3_status cells_fail(const char *why) {
    odin3_log(ODIN3_LOG_ERROR, "abc: ABC returned a malformed network: %s", why);
    return ODIN3_ERR_ENGINE;
}

/* Per gate name: the index of the library gate of that name, or NO_CELL. NULL on NO_MEMORY. */
static uint32_t *name_index(const odin3_abc_run *run, const odin3_abc_cells *cells,
                            odin3_arena *arena) {
    uint32_t *index = odin3_arena_alloc(arena, sizeof(uint32_t) * ((size_t)cells->n_names + 1U));
    if (index == NULL) {
        return NULL;
    }
    for (uint32_t name = 0; name < cells->n_names; name++) {
        index[name] = NO_CELL;
        for (size_t at = 0; cells->names[name] != NULL && at < run->lib.gates.len; at++) {
            const odin3_libgate *gate = odin3_vec_cat(&run->lib.gates, at);
            const odin3_celltype_def *def = odin3_celltype_get(run->design, gate->type);
            if (def != NULL && strcmp(def->name, cells->names[name]) == 0) {
                index[name] = (uint32_t)at;
                break;
            }
        }
    }
    return index;
}

/* The bits of a table of inputs variables (inputs at most 6). */
static uint64_t table_mask(uint32_t inputs) {
    uint32_t bits = 1U << inputs;
    return bits >= TABLE_BITS ? UINT64_MAX : (UINT64_C(1) << bits) - 1U;
}

/* One read in progress. */
typedef struct cread {
    const odin3_abc_run *run;
    const odin3_abc_cells *cells;
    const uint32_t *index;
    odin3_abc_result *out;
    uint32_t lut; /* the result's next LUT */
} cread;

/* The next LUT of the result came from MiniLUT object obj: its library gate, checked. */
static odin3_status tag_gate(cread *cr, uint32_t obj) {
    const odin3_abc_cells *cells = cr->cells;
    uint32_t lut = cr->lut++;
    int32_t name = cells->gate[obj];
    if (name < 0 || (uint32_t)name >= cells->n_names || cells->names[name] == NULL) {
        return cells_fail("a gate object without a gate name");
    }
    const char *text = cells->names[name];
    uint32_t at = cr->index[name];
    char why[WHY_BYTES];
    if (at == NO_CELL) {
        (void)snprintf(why, sizeof why, "gate '%s' is not a cell of library '%s'", text,
                       cr->run->opts.gates);
        return cells_fail(why);
    }
    const odin3_libgate *gate = odin3_vec_cat(&cr->run->lib.gates, at);
    const odin3_abc_result *res = cr->out;
    uint32_t inputs = ((const uint32_t *)res->lut_k.data)[lut];
    if (inputs != gate->n_inputs) {
        (void)snprintf(why, sizeof why, "gate '%s' has %u inputs; library cell '%s' has %u", text,
                       (unsigned)inputs, text, (unsigned)gate->n_inputs);
        return cells_fail(why);
    }
    uint32_t first = ((const uint32_t *)res->truth_start.data)[lut];
    uint64_t mask = table_mask(inputs);
    if ((((const uint64_t *)res->truth.data)[first] & mask) != (gate->table & mask)) {
        (void)snprintf(why, sizeof why, "gate '%s' does not compute library cell '%s''s function",
                       text, text);
        return cells_fail(why);
    }
    uint32_t *slot = odin3_vec_push(&cr->out->gate);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = at;
    return ODIN3_OK;
}

static bool is_gate_object(const odin3_abc_luts *net, uint32_t obj) {
    const int32_t *fan = net->fanins + (size_t)net->lut_size * obj;
    return fan[0] != MINI_NULL && fan[1] != MINI_NULL2;
}

odin3_status odin3_abc_result_from_cells(const odin3_abc_run *run, const odin3_abc_cells *cells,
                                         odin3_arena *arena, odin3_abc_result *out) {
    const odin3_abc_region *region = run->region;
    const odin3_abc_result_expect expect = {(uint32_t)region->cis.len, (uint32_t)region->cos.len,
                                            (uint32_t)region->regs.len, 0};
    odin3_status st = odin3_abc_result_from_luts(&cells->net, &expect, arena, out);
    if (st != ODIN3_OK) {
        return st;
    }
    if (cells->gate == NULL || (cells->n_names > 0 && cells->names == NULL)) {
        return cells_fail("a gate object without a gate name");
    }
    const uint32_t *index = name_index(run, cells, arena);
    st = index != NULL ? odin3_vec_reserve(&out->gate, out->n_luts) : ODIN3_ERR_NO_MEMORY;
    cread cr = {run, cells, index, out, 0};
    for (uint32_t obj = FIRST_OBJ; st == ODIN3_OK && obj < cells->net.n_objs; obj++) {
        if (is_gate_object(&cells->net, obj)) {
            st = tag_gate(&cr, obj);
        }
    }
    return st;
}
