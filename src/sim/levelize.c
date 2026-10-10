/* levelize.c — settle order of a flat simulation program (Kahn's algorithm); loop finding. */
#include "sim/sim_internal.h"
#include "util/alloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* High bit of an in-degree: the loop walk has visited the cell. */
static const uint32_t k_visited = 1U << 31;

/*
 * Per value slot, the cells that write it (one entry per output pin bit) and the cells that read
 * it in the graph (one entry per input pin bit; edge-triggered cells read nothing): the entries
 * of slot s are items[start[s] .. start[s + 1]).
 */
typedef struct slot_lists {
    uint32_t *start;
    uint32_t *items;
} slot_lists;

typedef struct graph {
    odin3_sim *sim;
    slot_lists writers;
    slot_lists readers;
    uint32_t *indeg;
} graph;

/* True for the pin bits of port `port` of flat that reads its slot in the graph. */
static bool reads(const odin3_sim_flat *flat, uint32_t port) {
    return flat->def->ports[port].dir == ODIN3_DIR_IN && !odin3_sim_is_edge(flat->def);
}

static bool writes(const odin3_sim_flat *flat, uint32_t port) {
    return flat->def->ports[port].dir != ODIN3_DIR_IN;
}

/* What scan does with each pin bit: count it (start[s + 1]++) or file it (items[start[s]++]). */
typedef struct scan_mode {
    bool writer;
    bool fill;
} scan_mode;

static void scan_cell(const odin3_sim *sim, slot_lists *lists, scan_mode mode, uint32_t cell) {
    const odin3_sim_flat *flat = &sim->cells[cell];
    for (uint32_t port = 0; port < flat->view.n_ports; port++) {
        if (mode.writer ? !writes(flat, port) : !reads(flat, port)) {
            continue;
        }
        const odin3_sim_span *span = &flat->view.ports[port];
        for (uint32_t k = 0; k < span->width; k++) {
            if (mode.fill) {
                lists->items[lists->start[span->idx[k]]++] = cell;
            } else {
                lists->start[span->idx[k] + 1]++;
            }
        }
    }
}

/* Counts or fills the writer or reader lists of every slot. */
static void scan(const odin3_sim *sim, slot_lists *lists, scan_mode mode) {
    for (uint32_t i = 0; i < sim->n_cells; i++) {
        scan_cell(sim, lists, mode, i);
    }
}

/* Builds one set of slot lists (counting, prefix sums, filling, shifting the starts back). */
static odin3_status build_lists(const odin3_sim *sim, slot_lists *lists, bool writer) {
    uint32_t n_slots = sim->n_values;
    lists->start = odin3_util_calloc(((size_t)n_slots + 1) * sizeof *lists->start);
    if (lists->start == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    scan(sim, lists, (scan_mode){writer, false});
    for (uint32_t slot_ix = 0; slot_ix < n_slots; slot_ix++) {
        lists->start[slot_ix + 1] += lists->start[slot_ix];
    }
    size_t total = lists->start[n_slots];
    lists->items = odin3_util_malloc((total > 0 ? total : 1) * sizeof *lists->items);
    if (lists->items == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    scan(sim, lists, (scan_mode){writer, true}); /* start[slot_ix] is now the end of slot slot_ix */
    for (uint32_t slot_ix = n_slots; slot_ix > 0; slot_ix--) {
        lists->start[slot_ix] = lists->start[slot_ix - 1];
    }
    lists->start[0] = 0;
    return ODIN3_OK;
}

static uint32_t count(const slot_lists *lists, uint32_t slot) {
    return lists->start[slot + 1] - lists->start[slot];
}

/* In-degree of each cell: the writer bits behind each of its reader bits. */
static void in_degrees(graph *gr) {
    const odin3_sim *sim = gr->sim;
    for (uint32_t i = 0; i < sim->n_cells; i++) {
        const odin3_sim_flat *flat = &sim->cells[i];
        uint32_t deg = 0;
        for (uint32_t port = 0; port < flat->view.n_ports; port++) {
            for (uint32_t k = 0; reads(flat, port) && k < flat->view.ports[port].width; k++) {
                deg += count(&gr->writers, flat->view.ports[port].idx[k]);
            }
        }
        gr->indeg[i] = deg;
    }
}

/* Releases the readers of cell's output bits; appends those that become ready to order. */
static void release(graph *gr, uint32_t cell, uint32_t *tail) {
    const odin3_sim_flat *flat = &gr->sim->cells[cell];
    for (uint32_t port = 0; port < flat->view.n_ports; port++) {
        for (uint32_t k = 0; writes(flat, port) && k < flat->view.ports[port].width; k++) {
            uint32_t slot = flat->view.ports[port].idx[k];
            for (uint32_t edge = gr->readers.start[slot]; edge < gr->readers.start[slot + 1];
                 edge++) {
                uint32_t reader = gr->readers.items[edge];
                if (--gr->indeg[reader] == 0) {
                    gr->sim->order[(*tail)++] = reader;
                }
            }
        }
    }
}

/*
 * Kahn's algorithm, sim->order as the queue; returns the number of cells ordered. The
 * edge-triggered cells are queued first, so their COMB (Q = state) starts every settle.
 */
static uint32_t kahn(graph *gr) {
    const odin3_sim *sim = gr->sim;
    uint32_t tail = 0;
    for (uint32_t pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < sim->n_cells; i++) {
            if (gr->indeg[i] == 0 && odin3_sim_is_edge(sim->cells[i].def) == (pass == 0)) {
                sim->order[tail++] = i;
            }
        }
    }
    for (uint32_t head = 0; head < tail; head++) {
        release(gr, gr->sim->order[head], &tail);
    }
    return tail;
}

/* A slot that cell reads from an unordered writer; *writer gets that writer. */
static uint32_t unordered_input(const graph *gr, uint32_t cell, uint32_t *writer) {
    const odin3_sim_flat *flat = &gr->sim->cells[cell];
    for (uint32_t port = 0; port < flat->view.n_ports; port++) {
        for (uint32_t k = 0; reads(flat, port) && k < flat->view.ports[port].width; k++) {
            uint32_t slot = flat->view.ports[port].idx[k];
            for (uint32_t edge = gr->writers.start[slot]; edge < gr->writers.start[slot + 1];
                 edge++) {
                if (gr->indeg[gr->writers.items[edge]] != 0) {
                    *writer = gr->writers.items[edge];
                    return slot;
                }
            }
        }
    }
    return ODIN3_SIM_SLOT_ZERO; /* unreachable: an unordered cell has an unordered writer */
}

/*
 * After kahn left cells unordered: each unordered cell reads a slot written by another unordered
 * cell. Walks those edges backwards from any unordered cell until a cell repeats; the slot of the
 * edge that closed the walk lies on a loop.
 */
static uint32_t loop_slot(graph *gr) {
    uint32_t cell = 0;
    while (gr->indeg[cell] == 0) {
        cell++;
    }
    for (;;) {
        gr->indeg[cell] |= k_visited;
        uint32_t writer = 0;
        uint32_t slot = unordered_input(gr, cell, &writer);
        if ((gr->indeg[writer] & k_visited) != 0) {
            return slot;
        }
        cell = writer;
    }
}

static void graph_free(graph *gr) {
    odin3_util_free(gr->writers.start);
    odin3_util_free(gr->writers.items);
    odin3_util_free(gr->readers.start);
    odin3_util_free(gr->readers.items);
    odin3_util_free(gr->indeg);
}

odin3_status odin3_sim_levelize(odin3_sim *sim, uint32_t *loop) {
    graph gr = {.sim = sim};
    size_t n_alloc = sim->n_cells > 0 ? sim->n_cells : 1;
    odin3_util_free(sim->order);
    sim->order = odin3_util_malloc(n_alloc * sizeof *sim->order);
    gr.indeg = odin3_util_malloc(n_alloc * sizeof *gr.indeg);
    odin3_status st = sim->order == NULL || gr.indeg == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
    st = st != ODIN3_OK ? st : build_lists(sim, &gr.writers, true);
    st = st != ODIN3_OK ? st : build_lists(sim, &gr.readers, false);
    if (st == ODIN3_OK) {
        in_degrees(&gr);
        if (kahn(&gr) != sim->n_cells) {
            *loop = loop_slot(&gr);
            st = ODIN3_ERR_INVALID_ARG;
        }
    }
    graph_free(&gr);
    return st;
}
