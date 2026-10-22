/*
 * libcells.h — the cells of one .o3lib library as gate mapping sees them (gatemap spec 3, 8;
 * plan Task 3): its gate cells (what the genlib text and abc --gates map to), its state cells
 * (dfflibmap's) and the cells neither uses, each with the reason.
 *
 * Rules. The cells of library L are the types whose odin3_techlib_cell_get(...)->library is L's
 * strtab ID, in type-ID order (= file order). A gate cell has techlib kind GATE, one fn, no seq,
 * no memory, is odin3_gate_truth_eligible (one 1-bit output, 1 to ODIN3_GATE_TRUTH_MAX_INPUTS
 * 1-bit inputs, no parameters), its function depends on every input (the two cofactors of its
 * table on each input differ), and its own name and every port name match [A-Za-z_][A-Za-z0-9_]*
 * and are not CONST0 or CONST1 (genlib names: so AND2.X1, nand2-x1, 2NAND and the genlib's
 * reserved $odin3_const0 and $odin3_const1 are never gate cells). A state cell is what
 * odin3_libstate_of accepts. Every other cell is a skip, with the first reason that applies, in
 * this order: NOT_GATE (kind hard or blackbox), STATE (a seq cell: a state cell; a seq cell
 * odin3_libstate_of refuses is SHAPE), CONSTANT (no input, a constant output:
 * odin3_gate_const_value), SHAPE, NO_SUPPORT, CELL_NAME, NAME.
 * buf, inv, tie0 and tie1 are the gate with 1 input and table 0x2, with table 0x1, and the
 * CONSTANT cells of value 0 and 1; each the one of smallest area (odin3_libcells_area), a cell
 * without a numeric area after every one with, ties to library order.
 */
#ifndef ODIN3_TECHLIB_LIBCELLS_H
#define ODIN3_TECHLIB_LIBCELLS_H

#include "ir/design.h"
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "sim/gate_truth.h"
#include "techlib/reader.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>

/* Why a cell of the library is not a gate cell (odin3_libskip.why). */
typedef enum odin3_libcell_skip {
    ODIN3_LIBCELL_NOT_GATE = 1, /* kind hard or blackbox */
    ODIN3_LIBCELL_STATE,        /* a seq cell (dfflibmap's) */
    ODIN3_LIBCELL_CONSTANT,     /* no input: a TIE cell (the splice's, G-7) */
    ODIN3_LIBCELL_SHAPE,        /* not one fn output and 1 to 6 1-bit inputs without parameters */
    ODIN3_LIBCELL_NO_SUPPORT,   /* the function ignores input `port` */
    ODIN3_LIBCELL_NAME,         /* port `port` is not a genlib name */
    ODIN3_LIBCELL_CELL_NAME     /* the cell's own name is not a genlib name (checked before NAME) */
} odin3_libcell_skip;

/* A gate cell. */
typedef struct odin3_libgate {
    odin3_celltype_id type;
    uint32_t n_inputs;
    uint64_t table;                                /* odin3_gate_truth */
    uint32_t in_port[ODIN3_GATE_TRUTH_MAX_INPUTS]; /* port index of input j */
    uint32_t out_port;
    uint32_t area, delay; /* strtab IDs of the texts, 0 absent */
} odin3_libgate;

/* A state cell: a flip-flop or latch of three 1-bit ports. */
typedef struct odin3_libstate {
    odin3_celltype_id type;
    odin3_techlib_trigger trigger;
    int init; /* 0, 1, or -1 for x */
    uint32_t clock_port, data_port, q_port;
    uint32_t area; /* strtab ID of the text, 0 absent */
} odin3_libstate;

/* A cell that is not a gate cell: why (an odin3_libcell_skip) and, for NO_SUPPORT and NAME, the
 * port index. */
typedef struct odin3_libskip {
    odin3_celltype_id type;
    uint8_t why;
    uint32_t port;
} odin3_libskip;

typedef struct odin3_libcells {
    uint32_t library;                       /* strtab ID of the name, 0 when no cell has it */
    uint32_t n_cells;                       /* cells of the library */
    odin3_vec gates;                        /* odin3_libgate, library order */
    odin3_vec states;                       /* odin3_libstate, library order */
    odin3_vec skips;                        /* odin3_libskip: every cell not in gates, in order */
    odin3_celltype_id buf, inv, tie0, tie1; /* {0} when none */
} odin3_libcells;

/*
 * Fills *out (any contents; overwritten) with the cells of the library named library. ODIN3_OK
 * with n_cells 0 and empty vectors for a name no cell has; ODIN3_ERR_NO_MEMORY (out freed and
 * empty). Never logs. Free with odin3_libcells_free.
 */
odin3_status odin3_libcells_collect(const odin3_design *design, const char *library,
                                    odin3_libcells *out);

/* Frees the vectors of cells and empties it; safe to call twice. */
void odin3_libcells_free(odin3_libcells *cells);

/* True and *out = the number when area_str (a strtab ID) is a numeric text (strtod takes all of
 * it); false for 0 (absent) or any other text. */
bool odin3_libcells_area(const odin3_design *design, uint32_t area_str, double *out);

/*
 * True and *out when type is a library state cell: techlib kind GATE, one seq, no fn, no memory,
 * no parameters, exactly three 1-bit ports: the seq's clock input, one more input whose name is
 * the seq's data expression (a bare identifier) and the seq's output; init x gives -1, else the
 * init expression must evaluate to 0 or 1. False otherwise (*out untouched).
 */
bool odin3_libstate_of(const odin3_design *design, odin3_celltype_id type, odin3_libstate *out);

/*
 * The text of a skip reason, a printf format; NO_SUPPORT's and NAME's take the port name (%s):
 * "not a gate cell", "a flip-flop or latch", "a constant cell (used for constants)", "not one
 * output and 1 to 6 one-bit inputs without parameters", "its function ignores input '%s'", "port
 * '%s' is not a genlib name", "its name is not a genlib name". "" for an unknown value.
 */
const char *odin3_libcell_skip_text(uint8_t why);

#endif
