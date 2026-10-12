/* ff.c — word-level flip-flops and latches: $dff, $dffe, $adff, $sdff and (2E IR change D) the
 * eleven others of the family, $adffe … $sr. */
#include "cells.h"

enum { FF_WIDTH, FF_VALUE = 3 };

/* $dff: WIDTH CLK_POLARITY. */
static const odin3_param_def k_dff_params[] = {ODIN3_P_INT("WIDTH", 1),
                                               ODIN3_P_INT("CLK_POLARITY", 1)};
static const odin3_port_def k_dff_ports[] = {
    ODIN3_PORT_BIT("CLK", ODIN3_DIR_IN),
    ODIN3_PORT_VEC("D", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_VEC("Q", ODIN3_DIR_OUT, "WIDTH"),
};

static odin3_status dff_verify(const odin3_value *params) {
    static const odin3_int_range k_ranges[2] = {{"WIDTH", 1, UINT32_MAX}, {"CLK_POLARITY", 0, 1}};
    return odin3_cells_check_ints(params, k_ranges, ODIN3_NELEM(k_ranges));
}

const odin3_celltype_def odin3_cell_dff = {
    "$dff", ODIN3_GRAN_WORD, 0, k_dff_ports, 3, k_dff_params, 2, dff_verify, NULL, NULL, NULL};

/* $dffe adds EN / EN_POLARITY. */
static const odin3_param_def k_dffe_params[] = {
    ODIN3_P_INT("WIDTH", 1), ODIN3_P_INT("CLK_POLARITY", 1), ODIN3_P_INT("EN_POLARITY", 1)};
static const odin3_port_def k_dffe_ports[] = {
    ODIN3_PORT_BIT("CLK", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("EN", ODIN3_DIR_IN),
    ODIN3_PORT_VEC("D", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_VEC("Q", ODIN3_DIR_OUT, "WIDTH"),
};

static odin3_status dffe_verify(const odin3_value *params) {
    static const odin3_int_range k_ranges[3] = {
        {"WIDTH", 1, UINT32_MAX}, {"CLK_POLARITY", 0, 1}, {"EN_POLARITY", 0, 1}};
    return odin3_cells_check_ints(params, k_ranges, ODIN3_NELEM(k_ranges));
}

const odin3_celltype_def odin3_cell_dffe = {
    "$dffe", ODIN3_GRAN_WORD, 0, k_dffe_ports, 4, k_dffe_params, 3, dffe_verify, NULL, NULL, NULL};

/* $adff / $sdff add a reset: ARST/SRST, its polarity and its value (WIDTH bits). */
typedef struct reset_names {
    const char *polarity;
    const char *value;
} reset_names;

static odin3_status reset_verify(const odin3_value *params, const reset_names *names) {
    const odin3_int_range ranges[3] = {
        {"WIDTH", 1, UINT32_MAX}, {"CLK_POLARITY", 0, 1}, {names->polarity, 0, 1}};
    if (odin3_cells_check_ints(params, ranges, ODIN3_NELEM(ranges)) != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    return odin3_cells_check_bits(&params[FF_VALUE], names->value, (uint32_t)params[FF_WIDTH].i);
}

static odin3_status adff_verify(const odin3_value *params) {
    static const reset_names k_names = {"ARST_POLARITY", "ARST_VALUE"};
    return reset_verify(params, &k_names);
}
static odin3_status sdff_verify(const odin3_value *params) {
    static const reset_names k_names = {"SRST_POLARITY", "SRST_VALUE"};
    return reset_verify(params, &k_names);
}

static const odin3_param_def k_adff_params[] = {
    ODIN3_P_INT("WIDTH", 1), ODIN3_P_INT("CLK_POLARITY", 1), ODIN3_P_INT("ARST_POLARITY", 1),
    ODIN3_P_BITS("ARST_VALUE", odin3_cells_zero_bit, 1)};
static const odin3_param_def k_sdff_params[] = {
    ODIN3_P_INT("WIDTH", 1), ODIN3_P_INT("CLK_POLARITY", 1), ODIN3_P_INT("SRST_POLARITY", 1),
    ODIN3_P_BITS("SRST_VALUE", odin3_cells_zero_bit, 1)};
static const odin3_port_def k_adff_ports[] = {
    ODIN3_PORT_BIT("CLK", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("ARST", ODIN3_DIR_IN),
    ODIN3_PORT_VEC("D", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_VEC("Q", ODIN3_DIR_OUT, "WIDTH"),
};
static const odin3_port_def k_sdff_ports[] = {
    ODIN3_PORT_BIT("CLK", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("SRST", ODIN3_DIR_IN),
    ODIN3_PORT_VEC("D", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_VEC("Q", ODIN3_DIR_OUT, "WIDTH"),
};

const odin3_celltype_def odin3_cell_adff = {
    "$adff", ODIN3_GRAN_WORD, 0, k_adff_ports, 4, k_adff_params, 4, adff_verify, NULL, NULL, NULL};
const odin3_celltype_def odin3_cell_sdff = {
    "$sdff", ODIN3_GRAN_WORD, 0, k_sdff_ports, 4, k_sdff_params, 4, sdff_verify, NULL, NULL, NULL};

/*
 * --- 2E IR change D (2E spec §13, PR-19): the eleven other word flip-flops and latches ---------
 *
 * Yosys 0.55 simlib's ports (in simlib's order), parameters and meaning; word granularity, flags 0
 * and simulate NULL like the four above (1E simulates the netlist view). WIDTH is always parameter
 * 0 (INT, 1..UINT32_MAX); every other INT parameter is a polarity (0..1); every BITS parameter is
 * a *_VALUE of WIDTH bits. CLK, EN, ARST, SRST and ALOAD are 1 bit; D, Q, AD, SET and CLR are
 * WIDTH bits; Q is the only output.
 *   $adffe    ARST active -> ARST_VALUE; else at the edge, if EN, D
 *   $sdffe    at the edge: SRST -> SRST_VALUE, else if EN, D
 *   $sdffce   at the edge, if EN: SRST -> SRST_VALUE, else D
 *   $aldff    ALOAD active -> Q = AD; else D at the edge
 *   $aldffe   as $aldff, the edge load only if EN
 *   $dffsr    per bit: CLR -> 0, else SET -> 1, else D at the edge
 *   $dffsre   as $dffsr, the edge load only if EN
 *   $dlatch   Q follows D while EN is active, else holds
 *   $adlatch  ARST -> ARST_VALUE, else as $dlatch
 *   $dlatchsr per bit: CLR -> 0, else SET -> 1, else as $dlatch
 *   $sr       per bit: CLR -> 0, else SET -> 1, else holds
 */

/*
 * The one verify of the family, driven by the parameter table: parameter 0 is WIDTH in
 * 1..UINT32_MAX, each other INT parameter a polarity in 0..1 and each BITS parameter a value of
 * WIDTH bits. Covers the three shapes (polarities only; polarities and one *_VALUE; $sr).
 */
static odin3_status ff_table_verify(const odin3_value *params, const odin3_param_def *defs,
                                    uint32_t count) {
    const odin3_int_range width = {defs[FF_WIDTH].name, 1, UINT32_MAX};
    if (odin3_cells_check_int(&params[FF_WIDTH], &width) != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG;
    }
    for (uint32_t i = FF_WIDTH + 1; i < count; i++) {
        odin3_status status = ODIN3_OK;
        if (defs[i].kind == ODIN3_VAL_BITS) {
            status = odin3_cells_check_bits(&params[i], defs[i].name, (uint32_t)params[FF_WIDTH].i);
        } else {
            const odin3_int_range polarity = {defs[i].name, 0, 1};
            status = odin3_cells_check_int(&params[i], &polarity);
        }
        if (status != ODIN3_OK) {
            return status;
        }
    }
    return ODIN3_OK;
}

/* The ports, by shape. */
static const odin3_port_def k_adffe_ports[] = {
    ODIN3_PORT_BIT("CLK", ODIN3_DIR_IN), ODIN3_PORT_BIT("ARST", ODIN3_DIR_IN),
    ODIN3_PORT_BIT("EN", ODIN3_DIR_IN), ODIN3_PORT_VEC("D", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_VEC("Q", ODIN3_DIR_OUT, "WIDTH")};
static const odin3_port_def k_sdffe_ports[] = {/* also $sdffce */
                                               ODIN3_PORT_BIT("CLK", ODIN3_DIR_IN),
                                               ODIN3_PORT_BIT("SRST", ODIN3_DIR_IN),
                                               ODIN3_PORT_BIT("EN", ODIN3_DIR_IN),
                                               ODIN3_PORT_VEC("D", ODIN3_DIR_IN, "WIDTH"),
                                               ODIN3_PORT_VEC("Q", ODIN3_DIR_OUT, "WIDTH")};
static const odin3_port_def k_aldff_ports[] = {
    ODIN3_PORT_BIT("CLK", ODIN3_DIR_IN), ODIN3_PORT_BIT("ALOAD", ODIN3_DIR_IN),
    ODIN3_PORT_VEC("AD", ODIN3_DIR_IN, "WIDTH"), ODIN3_PORT_VEC("D", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_VEC("Q", ODIN3_DIR_OUT, "WIDTH")};
static const odin3_port_def k_aldffe_ports[] = {
    ODIN3_PORT_BIT("CLK", ODIN3_DIR_IN),         ODIN3_PORT_BIT("ALOAD", ODIN3_DIR_IN),
    ODIN3_PORT_VEC("AD", ODIN3_DIR_IN, "WIDTH"), ODIN3_PORT_BIT("EN", ODIN3_DIR_IN),
    ODIN3_PORT_VEC("D", ODIN3_DIR_IN, "WIDTH"),  ODIN3_PORT_VEC("Q", ODIN3_DIR_OUT, "WIDTH")};
static const odin3_port_def k_dffsr_ports[] = {
    ODIN3_PORT_BIT("CLK", ODIN3_DIR_IN), ODIN3_PORT_VEC("SET", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_VEC("CLR", ODIN3_DIR_IN, "WIDTH"), ODIN3_PORT_VEC("D", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_VEC("Q", ODIN3_DIR_OUT, "WIDTH")};
static const odin3_port_def k_dffsre_ports[] = {
    ODIN3_PORT_BIT("CLK", ODIN3_DIR_IN),          ODIN3_PORT_VEC("SET", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_VEC("CLR", ODIN3_DIR_IN, "WIDTH"), ODIN3_PORT_BIT("EN", ODIN3_DIR_IN),
    ODIN3_PORT_VEC("D", ODIN3_DIR_IN, "WIDTH"),   ODIN3_PORT_VEC("Q", ODIN3_DIR_OUT, "WIDTH")};
static const odin3_port_def k_dlatch_ports[] = {ODIN3_PORT_BIT("EN", ODIN3_DIR_IN),
                                                ODIN3_PORT_VEC("D", ODIN3_DIR_IN, "WIDTH"),
                                                ODIN3_PORT_VEC("Q", ODIN3_DIR_OUT, "WIDTH")};
static const odin3_port_def k_adlatch_ports[] = {
    ODIN3_PORT_BIT("EN", ODIN3_DIR_IN), ODIN3_PORT_BIT("ARST", ODIN3_DIR_IN),
    ODIN3_PORT_VEC("D", ODIN3_DIR_IN, "WIDTH"), ODIN3_PORT_VEC("Q", ODIN3_DIR_OUT, "WIDTH")};
static const odin3_port_def k_dlatchsr_ports[] = {
    ODIN3_PORT_BIT("EN", ODIN3_DIR_IN), ODIN3_PORT_VEC("SET", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_VEC("CLR", ODIN3_DIR_IN, "WIDTH"), ODIN3_PORT_VEC("D", ODIN3_DIR_IN, "WIDTH"),
    ODIN3_PORT_VEC("Q", ODIN3_DIR_OUT, "WIDTH")};
static const odin3_port_def k_sr_ports[] = {ODIN3_PORT_VEC("SET", ODIN3_DIR_IN, "WIDTH"),
                                            ODIN3_PORT_VEC("CLR", ODIN3_DIR_IN, "WIDTH"),
                                            ODIN3_PORT_VEC("Q", ODIN3_DIR_OUT, "WIDTH")};

/* The parameters, by cell. */
static const odin3_param_def k_adffe_params[] = {
    ODIN3_P_INT("WIDTH", 1), ODIN3_P_INT("CLK_POLARITY", 1), ODIN3_P_INT("EN_POLARITY", 1),
    ODIN3_P_INT("ARST_POLARITY", 1), ODIN3_P_BITS("ARST_VALUE", odin3_cells_zero_bit, 1)};
static const odin3_param_def k_sdffe_params[] =
    {/* also $sdffce */
     ODIN3_P_INT("WIDTH", 1), ODIN3_P_INT("CLK_POLARITY", 1), ODIN3_P_INT("EN_POLARITY", 1),
     ODIN3_P_INT("SRST_POLARITY", 1), ODIN3_P_BITS("SRST_VALUE", odin3_cells_zero_bit, 1)};
static const odin3_param_def k_aldff_params[] = {
    ODIN3_P_INT("WIDTH", 1), ODIN3_P_INT("CLK_POLARITY", 1), ODIN3_P_INT("ALOAD_POLARITY", 1)};
static const odin3_param_def k_aldffe_params[] = {
    ODIN3_P_INT("WIDTH", 1), ODIN3_P_INT("CLK_POLARITY", 1), ODIN3_P_INT("EN_POLARITY", 1),
    ODIN3_P_INT("ALOAD_POLARITY", 1)};
static const odin3_param_def k_dffsr_params[] = {
    ODIN3_P_INT("WIDTH", 1), ODIN3_P_INT("CLK_POLARITY", 1), ODIN3_P_INT("SET_POLARITY", 1),
    ODIN3_P_INT("CLR_POLARITY", 1)};
static const odin3_param_def k_dffsre_params[] = {
    ODIN3_P_INT("WIDTH", 1), ODIN3_P_INT("CLK_POLARITY", 1), ODIN3_P_INT("SET_POLARITY", 1),
    ODIN3_P_INT("CLR_POLARITY", 1), ODIN3_P_INT("EN_POLARITY", 1)};
static const odin3_param_def k_dlatch_params[] = {ODIN3_P_INT("WIDTH", 1),
                                                  ODIN3_P_INT("EN_POLARITY", 1)};
static const odin3_param_def k_adlatch_params[] = {
    ODIN3_P_INT("WIDTH", 1), ODIN3_P_INT("EN_POLARITY", 1), ODIN3_P_INT("ARST_POLARITY", 1),
    ODIN3_P_BITS("ARST_VALUE", odin3_cells_zero_bit, 1)};
static const odin3_param_def k_dlatchsr_params[] = {
    ODIN3_P_INT("WIDTH", 1), ODIN3_P_INT("EN_POLARITY", 1), ODIN3_P_INT("SET_POLARITY", 1),
    ODIN3_P_INT("CLR_POLARITY", 1)};
static const odin3_param_def k_sr_params[] = {
    ODIN3_P_INT("WIDTH", 1), ODIN3_P_INT("SET_POLARITY", 1), ODIN3_P_INT("CLR_POLARITY", 1)};

/* Each cell's verify: ff_table_verify over its own parameter table. */
static odin3_status adffe_verify(const odin3_value *params) {
    return ff_table_verify(params, k_adffe_params, ODIN3_NELEM(k_adffe_params));
}
static odin3_status sdffe_verify(const odin3_value *params) { /* also $sdffce */
    return ff_table_verify(params, k_sdffe_params, ODIN3_NELEM(k_sdffe_params));
}
static odin3_status aldff_verify(const odin3_value *params) {
    return ff_table_verify(params, k_aldff_params, ODIN3_NELEM(k_aldff_params));
}
static odin3_status aldffe_verify(const odin3_value *params) {
    return ff_table_verify(params, k_aldffe_params, ODIN3_NELEM(k_aldffe_params));
}
static odin3_status dffsr_verify(const odin3_value *params) {
    return ff_table_verify(params, k_dffsr_params, ODIN3_NELEM(k_dffsr_params));
}
static odin3_status dffsre_verify(const odin3_value *params) {
    return ff_table_verify(params, k_dffsre_params, ODIN3_NELEM(k_dffsre_params));
}
static odin3_status dlatch_verify(const odin3_value *params) {
    return ff_table_verify(params, k_dlatch_params, ODIN3_NELEM(k_dlatch_params));
}
static odin3_status adlatch_verify(const odin3_value *params) {
    return ff_table_verify(params, k_adlatch_params, ODIN3_NELEM(k_adlatch_params));
}
static odin3_status dlatchsr_verify(const odin3_value *params) {
    return ff_table_verify(params, k_dlatchsr_params, ODIN3_NELEM(k_dlatchsr_params));
}
static odin3_status sr_verify(const odin3_value *params) {
    return ff_table_verify(params, k_sr_params, ODIN3_NELEM(k_sr_params));
}

/* A definition of the family: word granularity, flags 0, the shape's tables, verify only. */
#define ODIN3_FF_DEF(label, shape)                                                                 \
    {                                                                                              \
        label, ODIN3_GRAN_WORD, 0, k_##shape##_ports, ODIN3_NELEM(k_##shape##_ports),              \
            k_##shape##_params, ODIN3_NELEM(k_##shape##_params), shape##_verify, NULL, NULL, NULL  \
    }

/* $sdffce shares $sdffe's ports, parameters and verify; only the meaning differs. */
const odin3_celltype_def odin3_cell_adffe = ODIN3_FF_DEF("$adffe", adffe);
const odin3_celltype_def odin3_cell_sdffe = ODIN3_FF_DEF("$sdffe", sdffe);
const odin3_celltype_def odin3_cell_sdffce = ODIN3_FF_DEF("$sdffce", sdffe);
const odin3_celltype_def odin3_cell_aldff = ODIN3_FF_DEF("$aldff", aldff);
const odin3_celltype_def odin3_cell_aldffe = ODIN3_FF_DEF("$aldffe", aldffe);
const odin3_celltype_def odin3_cell_dffsr = ODIN3_FF_DEF("$dffsr", dffsr);
const odin3_celltype_def odin3_cell_dffsre = ODIN3_FF_DEF("$dffsre", dffsre);
const odin3_celltype_def odin3_cell_dlatch = ODIN3_FF_DEF("$dlatch", dlatch);
const odin3_celltype_def odin3_cell_adlatch = ODIN3_FF_DEF("$adlatch", adlatch);
const odin3_celltype_def odin3_cell_dlatchsr = ODIN3_FF_DEF("$dlatchsr", dlatchsr);
const odin3_celltype_def odin3_cell_sr = ODIN3_FF_DEF("$sr", sr);
