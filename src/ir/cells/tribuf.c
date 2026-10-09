/* tribuf.c — word-level tristate buffer $tribuf: Y is A when EN, else high impedance. */
#include "cells.h"

static const odin3_param_def k_params[] = {ODIN3_P_INT("WIDTH", 1)};
static const odin3_port_def k_ports[] = {
    ODIN3_PORT_VEC("A", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_BIT("EN", ODIN3_DIR_IN),
    ODIN3_PORT_VEC("Y", ODIN3_DIR_OUT, "WIDTH"),
};

static odin3_status tribuf_verify(const odin3_value *params) {
    static const odin3_int_range k_range = {"WIDTH", 1, UINT32_MAX};
    return odin3_cells_check_int(&params[0], &k_range);
}

const odin3_celltype_def odin3_cell_tribuf = {"$tribuf",
                                              ODIN3_GRAN_WORD,
                                              ODIN3_CT_TRISTATE,
                                              k_ports,
                                              3,
                                              k_params,
                                              1,
                                              tribuf_verify,
                                              NULL,
                                              NULL,
                                              NULL};
