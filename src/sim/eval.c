/* eval.c — the cycle engine: settles, clock edges, INIT, and the primary input/output API. */
#include "ir/design.h"
#include "sim/cell.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "sim/sim_internal.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>

/* --- the engine ------------------------------------------------------------------------------- */

static void send(odin3_sim *sim, odin3_sim_flat *flat, odin3_sim_event event) {
    flat->view.event = event;
    flat->def->simulate(&flat->view);
    sim->n_hook_calls++;
}

/* Runs every cell's COMB once in level order (the edge-triggered cells come first). */
static void settle(odin3_sim *sim) {
    for (uint32_t i = 0; i < sim->n_cells; i++) {
        send(sim, &sim->cells[sim->order[i]], ODIN3_SIM_COMB);
    }
}

/*
 * Raises (POSEDGE) or lowers (NEGEDGE) every clock input, then sends event to every edge-triggered
 * cell (each copies its inputs, as settled before the edge, into its state and writes no output,
 * so their order does not matter).
 */
static void edge(odin3_sim *sim, odin3_sim_event event) {
    uint8_t level = event == ODIN3_SIM_POSEDGE ? 1 : 0;
    for (uint32_t k = 0; k < sim->n_in_bits; k++) {
        if (sim->in_clock[k] != 0) {
            sim->values[sim->in_bits[k]] = level;
        }
    }
    for (uint32_t i = 0; i < sim->n_edge; i++) {
        send(sim, &sim->cells[sim->order[i]], event);
    }
}

void odin3_sim_start(odin3_sim *sim) {
    sim->n_edge = 0;
    while (sim->n_edge < sim->n_cells &&
           odin3_sim_is_edge(sim->cells[sim->order[sim->n_edge]].def)) {
        sim->n_edge++;
    }
    for (uint32_t i = 0; i < sim->n_cells; i++) {
        if (odin3_sim_is_seq(sim->cells[i].def)) {
            send(sim, &sim->cells[i], ODIN3_SIM_INIT);
        }
    }
    settle(sim);
}

void odin3_sim_cycle(odin3_sim *sim) {
    settle(sim);
    edge(sim, ODIN3_SIM_POSEDGE);
    settle(sim);
    edge(sim, ODIN3_SIM_NEGEDGE);
    settle(sim);
}

/* --- ports ------------------------------------------------------------------------------------ */

uint32_t odin3_sim_input_count(const odin3_sim *sim) {
    return sim->n_in_ports;
}

uint32_t odin3_sim_output_count(const odin3_sim *sim) {
    return sim->n_out_ports;
}

/* The name of port (NULL for none). */
static const char *port_name(const odin3_sim *sim, const odin3_sim_port *port) {
    return port != NULL ? odin3_strtab_get(odin3_design_strtab(sim->design), port->name) : NULL;
}

const char *odin3_sim_input_name(const odin3_sim *sim, uint32_t port) {
    return port_name(sim, port < sim->n_in_ports ? &sim->in_ports[port] : NULL);
}

const char *odin3_sim_output_name(const odin3_sim *sim, uint32_t port) {
    return port_name(sim, port < sim->n_out_ports ? &sim->out_ports[port] : NULL);
}

uint32_t odin3_sim_input_width(const odin3_sim *sim, uint32_t port) {
    return port < sim->n_in_ports ? sim->in_ports[port].width : 0;
}

uint32_t odin3_sim_output_width(const odin3_sim *sim, uint32_t port) {
    return port < sim->n_out_ports ? sim->out_ports[port].width : 0;
}

/* The flat bit number (index into in_bits / out_bits) of `at`; false when out of range. */
static bool bit_index(const odin3_sim_port *ports, uint32_t count, odin3_sim_bit at,
                      uint32_t *index) {
    if (at.port >= count || at.bit >= ports[at.port].width) {
        return false;
    }
    *index = ports[at.port].first + at.bit;
    return true;
}

bool odin3_sim_input_is_clock(const odin3_sim *sim, odin3_sim_bit at) {
    uint32_t index = 0;
    return bit_index(sim->in_ports, sim->n_in_ports, at, &index) && sim->in_clock[index] != 0;
}

odin3_status odin3_sim_set_input(odin3_sim *sim, odin3_sim_bit at, bool value) {
    uint32_t index = 0;
    if (!bit_index(sim->in_ports, sim->n_in_ports, at, &index) || sim->in_clock[index] != 0) {
        return ODIN3_ERR_INVALID_ARG;
    }
    sim->values[sim->in_bits[index]] = value ? 1 : 0;
    return ODIN3_OK;
}

odin3_status odin3_sim_get_input(const odin3_sim *sim, odin3_sim_bit at, bool *value) {
    uint32_t index = 0;
    if (!bit_index(sim->in_ports, sim->n_in_ports, at, &index)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    *value = sim->values[sim->in_bits[index]] != 0;
    return ODIN3_OK;
}

odin3_status odin3_sim_get_output(const odin3_sim *sim, odin3_sim_bit at, bool *value) {
    uint32_t index = 0;
    if (!bit_index(sim->out_ports, sim->n_out_ports, at, &index)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    *value = sim->values[sim->out_bits[index]] != 0;
    return ODIN3_OK;
}

void odin3_sim_drive_random(odin3_sim *sim, odin3_prng *prng) {
    for (uint32_t k = 0; k < sim->n_in_bits; k++) {
        if (sim->in_clock[k] == 0) {
            sim->values[sim->in_bits[k]] = odin3_prng_bit(prng) ? 1 : 0;
        }
    }
}
