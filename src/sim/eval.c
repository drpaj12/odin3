/* eval.c — the cycle engine: settles, clock edges, INIT, and the primary input/output API. */
#include "ir/design.h"
#include "sim/cell.h"
#include "sim/prng.h"
#include "sim/sim.h"
#include "sim/sim_internal.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* --- the engine ------------------------------------------------------------------------------- */

static void send(odin3_sim *sim, odin3_sim_flat *flat, odin3_sim_event event) {
    flat->view.event = event;
    flat->def->simulate(&flat->view);
    sim->n_hook_calls++;
}

/* Runs every cell's COMB once in level order (the edge-triggered cells come first). */
void odin3_sim_settle(odin3_sim *sim) {
    for (uint32_t i = 0; i < sim->n_cells; i++) {
        send(sim, &sim->cells[sim->order[i]], ODIN3_SIM_COMB);
    }
}

/*
 * The most evaluations of the derived clocks one input edge may take: a chain of clocks made by
 * registers (a divider of a divider) is as long as the design is deep, an oscillator never ends.
 */
enum { DCLK_ROUNDS = 256 };

/* Sends event to the edge-triggered cells clocked by the inputs (every one that is not derived). */
static void send_direct(odin3_sim *sim, odin3_sim_event event) {
    for (uint32_t i = 0; i < sim->n_edge; i++) {
        uint32_t cell = sim->order[i];
        if (sim->dclk_mark[cell] == 0) {
            send(sim, &sim->cells[cell], event);
        }
    }
}

/* Sends the edge to each derived-clock cell whose clock changed since it was last looked at;
 * the number of cells sent to. They copy their inputs as they stand and write no output, so the
 * order does not matter. */
static uint32_t send_derived(odin3_sim *sim) {
    uint32_t sent = 0;
    for (uint32_t k = 0; k < sim->n_dclk; k++) {
        uint8_t now = sim->values[sim->dclk_slot[k]] != 0 ? 1U : 0U;
        if (now != sim->dclk_last[k]) {
            send(sim, &sim->cells[sim->dclk_cell[k]],
                 now != 0 ? ODIN3_SIM_POSEDGE : ODIN3_SIM_NEGEDGE);
            sim->dclk_last[k] = now;
            sent++;
        }
    }
    return sent;
}

/* Takes the edges of the derived clocks that changed, round after round, until none does. */
static odin3_status derived_rounds(odin3_sim *sim) {
    for (uint32_t round = 0; round < DCLK_ROUNDS; round++) {
        if (send_derived(sim) == 0) {
            return ODIN3_OK;
        }
        odin3_sim_settle(sim);
    }
    odin3_log(ODIN3_LOG_ERROR,
              "simulation: a derived clock does not settle after %d rounds (a register clocks "
              "itself)",
              DCLK_ROUNDS);
    return ODIN3_ERR_CHECK;
}

/*
 * The edge of a design with derived clocks, in the order Verilog gives it (the active region, then
 * the non-blocking updates, then what those updates set off): the inputs rise, the logic settles
 * so a clock made of the input shows its new value, the cells on the input clock and on the
 * derived clocks that changed copy their inputs together, and the outputs settle; a derived clock
 * that this changed (one made by a register) triggers its cells in the next round, which copy
 * the inputs as the first round left them. Rounds go on until no derived clock changes. A clock
 * that never stops (a register that clocks itself) is an error after DCLK_ROUNDS.
 */
static odin3_status derived_edge(odin3_sim *sim, odin3_sim_event event) {
    odin3_sim_settle(sim);
    send_direct(sim, event);
    (void)send_derived(sim);
    odin3_sim_settle(sim);
    return derived_rounds(sim);
}

odin3_status odin3_sim_settle_events(odin3_sim *sim) {
    odin3_sim_settle(sim);
    return sim->n_dclk > 0 ? derived_rounds(sim) : ODIN3_OK;
}

/*
 * Raises (POSEDGE) or lowers (NEGEDGE) every clock input, then sends event to every edge-triggered
 * cell (each copies its inputs, as settled before the edge, into its state and writes no output,
 * so their order does not matter), then settles. A design with derived clocks (a cell clocked by
 * a net that logic or a register drives) goes through derived_edge. Any other event:
 * ODIN3_ERR_INVALID_ARG, nothing changed. A design with no clock input still sends the event to
 * its edge-triggered cells (none) and settles.
 */
odin3_status odin3_sim_clock_edge(odin3_sim *sim, odin3_sim_event event) {
    if (event != ODIN3_SIM_POSEDGE && event != ODIN3_SIM_NEGEDGE) {
        return ODIN3_ERR_INVALID_ARG;
    }
    uint8_t level = event == ODIN3_SIM_POSEDGE ? 1 : 0;
    for (uint32_t k = 0; k < sim->n_in_bits; k++) {
        if (sim->in_clock[k] != 0) {
            sim->values[sim->in_bits[k]] = level;
        }
    }
    if (sim->n_dclk > 0) {
        return derived_edge(sim, event);
    }
    for (uint32_t i = 0; i < sim->n_edge; i++) {
        send(sim, &sim->cells[sim->order[i]], event);
    }
    odin3_sim_settle(sim);
    return ODIN3_OK;
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
    odin3_sim_settle(sim);
    for (uint32_t k = 0; k < sim->n_dclk; k++) {
        sim->dclk_last[k] = sim->values[sim->dclk_slot[k]] != 0 ? 1U : 0U;
    }
}

void odin3_sim_reset(odin3_sim *sim) {
    if (sim->values != NULL) {
        memset(sim->values, 0, sim->n_values);
    }
    if (sim->state != NULL) {
        memset(sim->state, 0, sim->n_state);
    }
    odin3_sim_start(sim);
}

odin3_status odin3_sim_cycle(odin3_sim *sim) {
    odin3_status st = odin3_sim_settle_events(sim);
    if (st == ODIN3_OK) {
        st = odin3_sim_clock_edge(sim, ODIN3_SIM_POSEDGE);
    }
    return st == ODIN3_OK ? odin3_sim_clock_edge(sim, ODIN3_SIM_NEGEDGE) : st;
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
