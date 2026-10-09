/* port.c — module boundary cells $port_in, $port_out, $port_inout (IR-3). */
#include "ir/ir_internal.h"
#include "util/log.h"

#include <stdint.h>

/* WIDTH is the port's bit count: 1 .. UINT32_MAX. */
static odin3_status port_verify(const odin3_value *params) {
    if (params[0].kind != ODIN3_VAL_INT || params[0].i < 1 || params[0].i > (int64_t)UINT32_MAX) {
        odin3_log(ODIN3_LOG_ERROR, "port cell: WIDTH must be an int in 1..%u", UINT32_MAX);
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

static const odin3_param_def k_port_params[] = {
    {"WIDTH", ODIN3_VAL_INT, {ODIN3_VAL_INT, 1, NULL, 0, 0, 0}},
};

/* Directions are the cell's view: a $port_in pin drives the module net, a $port_out pin sinks. */
static const odin3_port_def k_port_in_ports[] = {
    {"P", ODIN3_DIR_OUT, false, 0, "WIDTH", NULL, NULL}};
static const odin3_port_def k_port_out_ports[] = {
    {"P", ODIN3_DIR_IN, false, 0, "WIDTH", NULL, NULL}};
static const odin3_port_def k_port_inout_ports[] = {
    {"P", ODIN3_DIR_INOUT, false, 0, "WIDTH", NULL, NULL}};

const odin3_celltype_def odin3_cell_port_in = {
    "$port_in", ODIN3_GRAN_PORT, 0, k_port_in_ports, 1, k_port_params, 1, port_verify, NULL, NULL};
const odin3_celltype_def odin3_cell_port_out = {
    "$port_out", ODIN3_GRAN_PORT, 0, k_port_out_ports, 1, k_port_params, 1, port_verify, NULL,
    NULL};
const odin3_celltype_def odin3_cell_port_inout = {
    "$port_inout", ODIN3_GRAN_PORT, 0, k_port_inout_ports, 1, k_port_params, 1, port_verify, NULL,
    NULL};
