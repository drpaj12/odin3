/*
 * gate_truth.c — truth tables and constant values of gate types by simulation (gatemap plan
 * Task 3; moved from passes/abc/region.c, 3D spec §5.1).
 */
#include "sim/gate_truth.h"

#include "sim/cell.h"
#include "util/alloc.h"

#include <stdbool.h>
#include <stdint.h>

enum {
    MAX_GATE_PORTS = 8, /* the most ports an eligible type may have (inputs and the output) */
};

/* The port is one scalar bit (a gate's pins are single bits). */
static bool port_is_bit(const odin3_port_def *port) {
    return port->width == 1 && port->width_fn == NULL && port->width_expr == NULL &&
           port->width_param == NULL && port->dir != ODIN3_DIR_INOUT;
}

bool odin3_gate_truth_eligible(const odin3_celltype_def *def) {
    const uint32_t refused_flags =
        ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL | ODIN3_CT_TRISTATE | ODIN3_CT_ANYVIEW;
    if (def->gran != ODIN3_GRAN_BIT || def->n_params != 0 || def->simulate == NULL ||
        (def->flags & refused_flags) != 0 || def->n_ports < 2 || def->n_ports > MAX_GATE_PORTS) {
        return false;
    }
    uint32_t inputs = 0;
    uint32_t outputs = 0;
    for (uint32_t port = 0; port < def->n_ports; port++) {
        if (!port_is_bit(&def->ports[port])) {
            return false;
        }
        inputs += def->ports[port].dir == ODIN3_DIR_IN ? 1U : 0U;
        outputs += def->ports[port].dir == ODIN3_DIR_OUT ? 1U : 0U;
    }
    return outputs == 1 && inputs >= 1 && inputs <= ODIN3_GATE_TRUTH_MAX_INPUTS;
}

uint32_t odin3_gate_truth_inputs(const odin3_celltype_def *def) {
    uint32_t inputs = 0;
    for (uint32_t port = 0; port < def->n_ports; port++) {
        inputs += def->ports[port].dir == ODIN3_DIR_IN ? 1U : 0U;
    }
    return inputs;
}

/* Gives cell the scratch def's sizing hook asks for (none without a hook). */
static odin3_status alloc_scratch(const odin3_celltype_def *def, odin3_sim_cell *cell) {
    if (def->sim_scratch_bytes == NULL) {
        return ODIN3_OK;
    }
    odin3_sim_cell sizing = *cell;
    sizing.values = NULL;
    uint32_t bytes = 0;
    odin3_status st = def->sim_scratch_bytes(&sizing, &bytes);
    if (st != ODIN3_OK || bytes == 0) {
        return st;
    }
    cell->scratch = odin3_util_malloc(bytes);
    if (cell->scratch == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    cell->n_scratch = bytes;
    return ODIN3_OK;
}

/*
 * Runs def's simulate hook over all 2^n_inputs assignments (input j = the j-th input port) and
 * sets bit i of *table to the output for assignment i.
 */
static odin3_status simulate_table(const odin3_celltype_def *def, const void *type_data,
                                   uint32_t n_inputs, uint64_t *table) {
    uint8_t values[MAX_GATE_PORTS] = {0};
    uint32_t index[MAX_GATE_PORTS];
    odin3_sim_span spans[MAX_GATE_PORTS];
    uint32_t next_in = 0;
    uint32_t out_slot = n_inputs;
    for (uint32_t port = 0; port < def->n_ports; port++) {
        index[port] = def->ports[port].dir == ODIN3_DIR_IN ? next_in++ : out_slot;
        spans[port] = (odin3_sim_span){&index[port], 1};
    }
    odin3_sim_cell cell = {values, spans,          def->n_ports, NULL, NULL,
                           0,      ODIN3_SIM_COMB, type_data,    NULL, 0};
    odin3_status st = alloc_scratch(def, &cell);
    if (st != ODIN3_OK) {
        return st;
    }
    uint64_t bits = 0;
    for (uint32_t row = 0; row < (1U << n_inputs); row++) {
        for (uint32_t j = 0; j < n_inputs; j++) {
            values[j] = (uint8_t)((row >> j) & 1U);
        }
        values[out_slot] = 0;
        def->simulate(&cell);
        bits |= (uint64_t)(cell.values[out_slot] != 0 ? 1U : 0U) << row;
    }
    odin3_util_free(cell.scratch);
    *table = bits;
    return ODIN3_OK;
}

odin3_status odin3_gate_truth(const odin3_celltype_def *def, const void *type_data,
                              uint64_t *table) {
    if (!odin3_gate_truth_eligible(def)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    return simulate_table(def, type_data, odin3_gate_truth_inputs(def), table);
}

odin3_status odin3_gate_const_value(const odin3_celltype_def *def, const void *type_data,
                                    int *value) {
    const uint32_t refused_flags = ODIN3_CT_SEQ_EDGE | ODIN3_CT_SEQ_LEVEL;
    if (def->gran != ODIN3_GRAN_BIT || def->n_params != 0 || def->simulate == NULL ||
        (def->flags & refused_flags) != 0 || def->n_ports != 1 ||
        def->ports[0].dir != ODIN3_DIR_OUT || !port_is_bit(&def->ports[0])) {
        return ODIN3_ERR_INVALID_ARG;
    }
    uint64_t table = 0;
    odin3_status st = simulate_table(def, type_data, 0, &table);
    if (st == ODIN3_OK) {
        *value = (int)(table & 1U);
    }
    return st;
}
