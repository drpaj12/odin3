/* mem.c — memories $mem, $memrd, $memwr (Yosys RTLIL); width-checked only in Phase 1. */
#include "cells.h"
#include "util/log.h"

/* $mem parameter indices (declaration order). */
enum {
    MEM_MEMID,
    MEM_SIZE,
    MEM_OFFSET,
    MEM_ABITS,
    MEM_WIDTH,
    MEM_RD_PORTS,
    MEM_WR_PORTS,
    MEM_RD_CLK_ENABLE,
    MEM_RD_CLK_POLARITY,
    MEM_RD_TRANSPARENT,
    MEM_WR_CLK_ENABLE,
    MEM_WR_CLK_POLARITY,
    MEM_INIT,
    MEM_N_PARAMS
};

/* $mem port indices. */
enum {
    PORT_RD_CLK,
    PORT_RD_EN,
    PORT_RD_ADDR,
    PORT_RD_DATA,
    PORT_WR_CLK,
    PORT_WR_EN,
    PORT_WR_ADDR,
    PORT_WR_DATA,
    MEM_N_PORTS
};

static const odin3_param_def k_mem_params[MEM_N_PARAMS] = {
    ODIN3_P_STR("MEMID"),
    ODIN3_P_INT("SIZE", 1),
    ODIN3_P_INT("OFFSET", 0),
    ODIN3_P_INT("ABITS", 1),
    ODIN3_P_INT("WIDTH", 1),
    ODIN3_P_INT("RD_PORTS", 1),
    ODIN3_P_INT("WR_PORTS", 1),
    ODIN3_P_BITS("RD_CLK_ENABLE", odin3_cells_zero_bit, 1),
    ODIN3_P_BITS("RD_CLK_POLARITY", odin3_cells_zero_bit, 1),
    ODIN3_P_BITS("RD_TRANSPARENT", odin3_cells_zero_bit, 1),
    ODIN3_P_BITS("WR_CLK_ENABLE", odin3_cells_zero_bit, 1),
    ODIN3_P_BITS("WR_CLK_POLARITY", odin3_cells_zero_bit, 1),
    ODIN3_P_BITS("INIT", NULL, 0),
};

/* Width of a $mem port: a per-port count (RD_PORTS/WR_PORTS) times a per-port field width. */
static uint32_t mem_port_width(const odin3_value *params, uint32_t port) {
    static const uint8_t k_count[MEM_N_PORTS] = {MEM_RD_PORTS, MEM_RD_PORTS, MEM_RD_PORTS,
                                                 MEM_RD_PORTS, MEM_WR_PORTS, MEM_WR_PORTS,
                                                 MEM_WR_PORTS, MEM_WR_PORTS};
    static const uint8_t k_field[MEM_N_PORTS] = {0, 0,         MEM_ABITS, MEM_WIDTH,
                                                 0, MEM_WIDTH, MEM_ABITS, MEM_WIDTH};
    const odin3_value *count = &params[k_count[port]];
    const odin3_value *field = k_field[port] == 0 ? NULL : &params[k_field[port]];
    uint64_t prod = count->kind == ODIN3_VAL_INT && count->i >= 0 ? (uint64_t)count->i : UINT64_MAX;
    if (field != NULL) {
        prod = field->kind == ODIN3_VAL_INT && field->i >= 0 && prod != UINT64_MAX
                   ? prod * (uint64_t)field->i
                   : UINT64_MAX;
    }
    if (prod > UINT32_MAX) {
        odin3_log(ODIN3_LOG_ERROR, "$mem: port %u width is not in 0..%u", port, UINT32_MAX);
        return 0;
    }
    return (uint32_t)prod;
}

static const odin3_port_def k_mem_ports[MEM_N_PORTS] = {
    ODIN3_PORT_FN("RD_CLK", ODIN3_DIR_IN, mem_port_width),
    ODIN3_PORT_FN("RD_EN", ODIN3_DIR_IN, mem_port_width),
    ODIN3_PORT_FN("RD_ADDR", ODIN3_DIR_IN, mem_port_width),
    ODIN3_PORT_FN("RD_DATA", ODIN3_DIR_OUT, mem_port_width),
    ODIN3_PORT_FN("WR_CLK", ODIN3_DIR_IN, mem_port_width),
    ODIN3_PORT_FN("WR_EN", ODIN3_DIR_IN, mem_port_width),
    ODIN3_PORT_FN("WR_ADDR", ODIN3_DIR_IN, mem_port_width),
    ODIN3_PORT_FN("WR_DATA", ODIN3_DIR_IN, mem_port_width),
};

static odin3_status mem_verify_ints(const odin3_value *params) {
    static const odin3_int_range k_ranges[] = {
        {"SIZE", 1, UINT32_MAX},  {"OFFSET", INT32_MIN, INT32_MAX}, {"ABITS", 1, UINT32_MAX},
        {"WIDTH", 1, UINT32_MAX}, {"RD_PORTS", 0, UINT32_MAX},      {"WR_PORTS", 0, UINT32_MAX},
    };
    return odin3_cells_check_ints(&params[MEM_SIZE], k_ranges, ODIN3_NELEM(k_ranges));
}

static odin3_status mem_verify(const odin3_value *params) {
    if (params[MEM_MEMID].kind != ODIN3_VAL_STRING || mem_verify_ints(params) != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t rd = (uint32_t)params[MEM_RD_PORTS].i;
    uint32_t wr = (uint32_t)params[MEM_WR_PORTS].i;
    if (odin3_cells_check_bits(&params[MEM_RD_CLK_ENABLE], "RD_CLK_ENABLE", rd) != ODIN3_OK ||
        odin3_cells_check_bits(&params[MEM_RD_CLK_POLARITY], "RD_CLK_POLARITY", rd) != ODIN3_OK ||
        odin3_cells_check_bits(&params[MEM_RD_TRANSPARENT], "RD_TRANSPARENT", rd) != ODIN3_OK ||
        odin3_cells_check_bits(&params[MEM_WR_CLK_ENABLE], "WR_CLK_ENABLE", wr) != ODIN3_OK ||
        odin3_cells_check_bits(&params[MEM_WR_CLK_POLARITY], "WR_CLK_POLARITY", wr) != ODIN3_OK ||
        params[MEM_INIT].kind != ODIN3_VAL_BITS) {
        return ODIN3_ERR_INVALID_ARG;
    }
    return ODIN3_OK;
}

const odin3_celltype_def odin3_cell_mem = {"$mem",       ODIN3_GRAN_WORD, 0,
                                           k_mem_ports,  MEM_N_PORTS,     k_mem_params,
                                           MEM_N_PARAMS, mem_verify,      NULL};

/* $memrd: one asynchronous or synchronous read port. */
static const odin3_param_def k_memrd_params[] = {
    ODIN3_P_STR("MEMID"),         ODIN3_P_INT("ABITS", 1),        ODIN3_P_INT("WIDTH", 1),
    ODIN3_P_INT("CLK_ENABLE", 0), ODIN3_P_INT("CLK_POLARITY", 0), ODIN3_P_INT("TRANSPARENT", 0),
};
static const odin3_port_def k_memrd_ports[] = {
    ODIN3_PORT_BIT("CLK", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("EN", ODIN3_DIR_IN),
    ODIN3_PORT_VEC("ADDR", ODIN3_DIR_IN, "ABITS"),
    ODIN3_PORT_VEC("DATA", ODIN3_DIR_OUT, "WIDTH"),
};

static odin3_status memrd_verify(const odin3_value *params) {
    static const odin3_int_range k_ranges[] = {
        {"ABITS", 1, UINT32_MAX}, {"WIDTH", 1, UINT32_MAX}, {"CLK_ENABLE", 0, 1},
        {"CLK_POLARITY", 0, 1},   {"TRANSPARENT", 0, 1},
    };
    if (params[0].kind != ODIN3_VAL_STRING) {
        return ODIN3_ERR_INVALID_ARG;
    }
    return odin3_cells_check_ints(&params[1], k_ranges, ODIN3_NELEM(k_ranges));
}

const odin3_celltype_def odin3_cell_memrd = {
    "$memrd", ODIN3_GRAN_WORD, 0, k_memrd_ports, 4, k_memrd_params, 6, memrd_verify, NULL};

/* $memwr: one write port; EN has one bit per data bit. */
static const odin3_param_def k_memwr_params[] = {
    ODIN3_P_STR("MEMID"),         ODIN3_P_INT("ABITS", 1),        ODIN3_P_INT("WIDTH", 1),
    ODIN3_P_INT("CLK_ENABLE", 0), ODIN3_P_INT("CLK_POLARITY", 0), ODIN3_P_INT("PRIORITY", 0),
};
static const odin3_port_def k_memwr_ports[] = {
    ODIN3_PORT_BIT("CLK", ODIN3_DIR_IN),
    ODIN3_PORT_VEC("EN", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_VEC("ADDR", ODIN3_DIR_IN, "ABITS"),
    ODIN3_PORT_VEC("DATA", ODIN3_DIR_IN, "WIDTH"),
};

static odin3_status memwr_verify(const odin3_value *params) {
    static const odin3_int_range k_ranges[] = {
        {"ABITS", 1, UINT32_MAX}, {"WIDTH", 1, UINT32_MAX},    {"CLK_ENABLE", 0, 1},
        {"CLK_POLARITY", 0, 1},   {"PRIORITY", 0, UINT32_MAX},
    };
    if (params[0].kind != ODIN3_VAL_STRING) {
        return ODIN3_ERR_INVALID_ARG;
    }
    return odin3_cells_check_ints(&params[1], k_ranges, ODIN3_NELEM(k_ranges));
}

const odin3_celltype_def odin3_cell_memwr = {
    "$memwr", ODIN3_GRAN_WORD, 0, k_memwr_ports, 4, k_memwr_params, 6, memwr_verify, NULL};
