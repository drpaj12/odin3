/*
 * build_clock.c — derived clocks (the clock pin of an edge-triggered cell on a net that is not a
 * primary input): the build-time side. A derived clock is a net driven by cells: logic on the
 * input clock (a gate, a mux), or a register (a divided clock). The engine (eval.c) evaluates
 * the net on every clock edge and triggers the cell when it changes; this file finds the cells
 * and decides which primary input, if any, is the clock they hang on.
 */
#include "sim/build_internal.h"
#include "sim/sim_internal.h"
#include "util/alloc.h"

#include <stdbool.h>
#include <stdint.h>

/* What a walk back from a clock net found. */
typedef struct clock_cone {
    bool driven;         /* some cell drives the net */
    uint32_t n_clock;    /* input bits met that already clock a cell */
    uint32_t n_free;     /* input bits met that do not */
    uint32_t free_input; /* the first of those */
} clock_cone;

/* The scratch of the walks: per slot, the cell that drives it and the walk that last met it. */
typedef struct cone_walk {
    uint32_t *driver;
    uint32_t *stamp;
    uint32_t *stack;
    uint32_t top;
    uint32_t walk;
} cone_walk;

static void cone_free(cone_walk *cw) {
    odin3_util_free(cw->driver);
    odin3_util_free(cw->stamp);
    odin3_util_free(cw->stack);
}

/* Calls visit(slot, arg) for the slots flat drives (out) or reads (not out). */
typedef void (*port_visit)(cone_walk *cw, uint32_t slot, const uint32_t *arg);

static void visit_ports(cone_walk *cw, const odin3_sim_flat *flat, bool out, port_visit visit,
                        const uint32_t *arg) {
    for (uint32_t port = 0; port < flat->view.n_ports; port++) {
        bool is_out = flat->def->ports[port].dir != ODIN3_DIR_IN;
        for (uint32_t bit = 0; is_out == out && bit < flat->view.ports[port].width; bit++) {
            visit(cw, flat->view.ports[port].idx[bit], arg);
        }
    }
}

static void note_driver(cone_walk *cw, uint32_t slot, const uint32_t *cell) {
    cw->driver[slot] = cw->driver[slot] == ODIN3_SIM_UNSET ? *cell : cw->driver[slot];
}

/* The driver map: per slot the first cell with an output pin on it, else UNSET. */
static odin3_status cone_init(const odin3_sim *sim, cone_walk *cw) {
    size_t slots = (size_t)sim->n_values;
    *cw = (cone_walk){0};
    cw->driver = odin3_util_malloc(slots * sizeof *cw->driver);
    cw->stamp = odin3_util_calloc(slots * sizeof *cw->stamp);
    cw->stack = odin3_util_malloc(slots * sizeof *cw->stack);
    if (cw->driver == NULL || cw->stamp == NULL || cw->stack == NULL) {
        cone_free(cw);
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t slot = 0; slot < sim->n_values; slot++) {
        cw->driver[slot] = ODIN3_SIM_UNSET;
    }
    for (uint32_t cell = 0; cell < sim->n_cells; cell++) {
        visit_ports(cw, &sim->cells[cell], true, note_driver, &cell);
    }
    return ODIN3_OK;
}

/* Pushes slot unless it is the constant slot or a walk met it already. */
static void push_slot(cone_walk *cw, uint32_t slot, const uint32_t *unused) {
    (void)unused;
    if (slot != ODIN3_SIM_SLOT_ZERO && cw->stamp[slot] != cw->walk) {
        cw->stamp[slot] = cw->walk;
        cw->stack[cw->top++] = slot;
    }
}

/* Counts the input bit as a clock (it clocks a cell) or a free one. */
static void cone_count_input(const odin3_sim *sim, clock_cone *cone, uint32_t input) {
    if (sim->in_clock[input] != 0) {
        cone->n_clock++;
        return;
    }
    cone->free_input = cone->n_free == 0 ? input : cone->free_input;
    cone->n_free++;
}

/* Walks back from slot through combinational cells only (a register ends a branch: its output is
 * a source). */
static clock_cone cone_walk_back(const odin3_sim *sim, cone_walk *cw, const uint32_t *input_of,
                                 uint32_t slot) {
    clock_cone cone = {false, 0, 0, ODIN3_SIM_UNSET};
    cw->walk++;
    cw->top = 0;
    push_slot(cw, slot, NULL);
    while (cw->top > 0) {
        uint32_t at = cw->stack[--cw->top];
        if (input_of[at] != ODIN3_SIM_UNSET) {
            cone_count_input(sim, &cone, input_of[at]);
            continue;
        }
        if (cw->driver[at] == ODIN3_SIM_UNSET) {
            continue;
        }
        const odin3_sim_flat *flat = &sim->cells[cw->driver[at]];
        cone.driven = true;
        if (!odin3_sim_is_seq(flat->def)) {
            visit_ports(cw, flat, false, push_slot, NULL);
        }
    }
    return cone;
}

/* The arrays of the engine: the cells, their clock slots, the last value seen, a mark per cell. */
static odin3_status derived_alloc(odin3_sim *sim, uint32_t count) {
    sim->dclk_cell = odin3_util_malloc((size_t)count * sizeof *sim->dclk_cell);
    sim->dclk_slot = odin3_util_malloc((size_t)count * sizeof *sim->dclk_slot);
    sim->dclk_last = odin3_util_calloc((size_t)count);
    sim->dclk_mark = odin3_util_calloc((size_t)sim->n_cells);
    return sim->dclk_cell == NULL || sim->dclk_slot == NULL || sim->dclk_last == NULL ||
                   sim->dclk_mark == NULL
               ? ODIN3_ERR_NO_MEMORY
               : ODIN3_OK;
}

odin3_status odin3_sim_derived_clocks(const odin3_sim_builder *bld, odin3_sim *sim,
                                      const uint32_t *input_of, const odin3_sim_derived *list,
                                      uint32_t count) {
    cone_walk cw;
    odin3_status st = count > 0 ? cone_init(sim, &cw) : ODIN3_OK;
    st = st == ODIN3_OK && count > 0 ? derived_alloc(sim, count) : st;
    for (uint32_t k = 0; st == ODIN3_OK && k < count; k++) {
        clock_cone cone = cone_walk_back(sim, &cw, input_of, list[k].slot);
        const odin3_sim_flat *flat = &sim->cells[list[k].cell];
        if (!cone.driven) {
            odin3_sim_err_clock(bld, flat, list[k].slot);
            st = ODIN3_ERR_INVALID_ARG;
        } else if (cone.n_clock == 0 && cone.n_free > 1) {
            odin3_sim_err_clock_inputs(bld, flat, (odin3_sim_net_count){list[k].slot, cone.n_free});
            st = ODIN3_ERR_INVALID_ARG;
        } else {
            if (cone.n_clock == 0 && cone.n_free == 1) {
                sim->in_clock[cone.free_input] = 1; /* the one input the clock can hang on */
            }
            sim->dclk_cell[sim->n_dclk] = list[k].cell;
            sim->dclk_slot[sim->n_dclk++] = list[k].slot;
            sim->dclk_mark[list[k].cell] = 1;
        }
    }
    if (count > 0) {
        cone_free(&cw);
    }
    return st;
}
