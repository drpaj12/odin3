/*
 * cells.h — declarations shared by the built-in cell-type sources (private to src/ir/cells).
 */
#ifndef ODIN3_IR_CELLS_H
#define ODIN3_IR_CELLS_H

#include "ir/ir_internal.h"

#include <stdint.h>

/* Parameter and port table entries; the value is an INT default or an empty value of that kind. */
/* Element count of an array. */
#define ODIN3_NELEM(arr) ((uint32_t)(sizeof(arr) / sizeof((arr)[0])))

#define ODIN3_P_INT(pname, num)                                                                    \
    {                                                                                              \
        pname, ODIN3_VAL_INT, {                                                                    \
            ODIN3_VAL_INT, num, NULL, 0, 0, 0                                                      \
        }                                                                                          \
    }
#define ODIN3_P_STR(pname)                                                                         \
    {                                                                                              \
        pname, ODIN3_VAL_STRING, {                                                                 \
            ODIN3_VAL_STRING, 0, NULL, 0, 0, 0                                                     \
        }                                                                                          \
    }
#define ODIN3_P_BITS(pname, bytes, count)                                                          \
    {                                                                                              \
        pname, ODIN3_VAL_BITS, {                                                                   \
            ODIN3_VAL_BITS, 0, bytes, count, 0, 0                                                  \
        }                                                                                          \
    }

/* A scalar (1-bit) port and a vector port whose width is the INT parameter wparam. */
#define ODIN3_PORT_BIT(pname, pdir)                                                                \
    { pname, pdir, true, 1, NULL, NULL }
#define ODIN3_PORT_VEC(pname, pdir, wparam)                                                        \
    { pname, pdir, false, 0, wparam, NULL }
#define ODIN3_PORT_FN(pname, pdir, wfn)                                                            \
    { pname, pdir, false, 0, NULL, wfn }

/* Single-bit default value {0} for bit-vector parameters whose default width is 1. */
extern const uint8_t odin3_cells_zero_bit[1];

/* An INT parameter's inclusive range, for odin3_cells_check_int. */
typedef struct odin3_int_range {
    const char *name;
    int64_t lo;
    int64_t hi;
} odin3_int_range;

/* ODIN3_OK when val is an INT inside the range; else logs and returns ODIN3_ERR_INVALID_ARG. */
odin3_status odin3_cells_check_int(const odin3_value *val, const odin3_int_range *range);

/* Checks params[i] against ranges[i] for i < count. */
odin3_status odin3_cells_check_ints(const odin3_value *params, const odin3_int_range *ranges,
                                    uint32_t count);

/* ODIN3_OK when val is a BITS value of exactly width bits; else logs and returns INVALID_ARG. */
odin3_status odin3_cells_check_bits(const odin3_value *val, const char *name, uint32_t width);

/* Shared by $add/$and/$shl/$eq...: A_SIGNED B_SIGNED A_WIDTH B_WIDTH Y_WIDTH; ports A B Y. */
extern const odin3_port_def odin3_cells_binary_ports[3];
extern const odin3_param_def odin3_cells_binary_params[5];
odin3_status odin3_cells_binary_verify(const odin3_value *params);

/* Shared by $not/$reduce_*: A_SIGNED A_WIDTH Y_WIDTH; ports A Y. */
extern const odin3_port_def odin3_cells_unary_ports[2];
extern const odin3_param_def odin3_cells_unary_params[3];
odin3_status odin3_cells_unary_verify(const odin3_value *params);

/* Shared by the bit-level latches/flip-flops: INT parameter INIT (0..3, default 3). */
extern const odin3_param_def odin3_cells_init_params[1];
odin3_status odin3_cells_init_verify(const odin3_value *params);

extern const odin3_celltype_def odin3_cell_add;
extern const odin3_celltype_def odin3_cell_sub;
extern const odin3_celltype_def odin3_cell_mul;
extern const odin3_celltype_def odin3_cell_div;
extern const odin3_celltype_def odin3_cell_mod;
extern const odin3_celltype_def odin3_cell_and;
extern const odin3_celltype_def odin3_cell_or;
extern const odin3_celltype_def odin3_cell_xor;
extern const odin3_celltype_def odin3_cell_not;
extern const odin3_celltype_def odin3_cell_shl;
extern const odin3_celltype_def odin3_cell_shr;
extern const odin3_celltype_def odin3_cell_sshr;
extern const odin3_celltype_def odin3_cell_eq;
extern const odin3_celltype_def odin3_cell_ne;
extern const odin3_celltype_def odin3_cell_lt;
extern const odin3_celltype_def odin3_cell_le;
extern const odin3_celltype_def odin3_cell_gt;
extern const odin3_celltype_def odin3_cell_ge;
extern const odin3_celltype_def odin3_cell_reduce_and;
extern const odin3_celltype_def odin3_cell_reduce_or;
extern const odin3_celltype_def odin3_cell_reduce_xor;
extern const odin3_celltype_def odin3_cell_mux;
extern const odin3_celltype_def odin3_cell_pmux;
extern const odin3_celltype_def odin3_cell_dff;
extern const odin3_celltype_def odin3_cell_dffe;
extern const odin3_celltype_def odin3_cell_adff;
extern const odin3_celltype_def odin3_cell_sdff;
extern const odin3_celltype_def odin3_cell_mem;
extern const odin3_celltype_def odin3_cell_memrd;
extern const odin3_celltype_def odin3_cell_memwr;
extern const odin3_celltype_def odin3_cell_tribuf;
extern const odin3_celltype_def odin3_cell_g_buf;
extern const odin3_celltype_def odin3_cell_g_not;
extern const odin3_celltype_def odin3_cell_g_and;
extern const odin3_celltype_def odin3_cell_g_or;
extern const odin3_celltype_def odin3_cell_g_xor;
extern const odin3_celltype_def odin3_cell_g_nand;
extern const odin3_celltype_def odin3_cell_g_nor;
extern const odin3_celltype_def odin3_cell_g_xnor;
extern const odin3_celltype_def odin3_cell_g_mux;
extern const odin3_celltype_def odin3_cell_dff_p;
extern const odin3_celltype_def odin3_cell_dff_n;
extern const odin3_celltype_def odin3_cell_dlatch_p;
extern const odin3_celltype_def odin3_cell_dlatch_n;
extern const odin3_celltype_def odin3_cell_ff;
extern const odin3_celltype_def odin3_cell_sop;

#endif
