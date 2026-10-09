/*
 * fnsim.h — simulation of tech-library cells by interpreting their `fn` expressions (1E).
 *
 * A cell whose every output is driven by an `fn` (no `seq`, no `memory`) is compiled once, when
 * the library is read, into a program: its expressions in postorder with identifiers resolved to
 * input ports and parameters. The reader then gives the cell's type the simulate hook and the
 * sim_scratch_bytes hook below; the simulator passes the cell's odin3_techlib_cell as type_data.
 *
 * Semantics: Verilog-2005 expression sizing and signedness (IEEE 1364-2005 5.4, 5.5) over 2-state
 * bits. Each output's expression is evaluated in the context of the output's width (the
 * assignment), context-determined operands are extended to the context width (sign-extended only
 * when the whole context is signed: every operand port declared `signed`), and the result is
 * truncated to the output. Ports are as wide and as signed as declared; parameters and plain
 * decimals are signed 32-bit integers (64-bit when the value does not fit); sized literals are
 * unsigned, their x and z bits read 0. Supported: ~ ! unary -, & | ^ ~^ ^~ + - * << >> (logical),
 * == != < <= > >= && ||, ?:, bit selects and slices whose indices are constant expressions over
 * parameters (a bit outside the operand, negative indices included, reads 0; a reversed slice
 * reads the same bits as the ordered one; an index that does not evaluate gives one bit reading
 * 0), concatenation and replication with a constant count (one that does not evaluate is 0). A cell
 * using / % ** or a non-constant index, or whose fn reads an output port, gets no hook (the
 * simulator then rejects it as having no simulate hook).
 *
 * Words are little-endian 64-bit limbs (sim/word.h): up to 64 bits every operation is one machine
 * operation; wider values loop over the limbs. The hook allocates nothing: its node records and
 * values live in the cell's scratch, which the sizing hook computes from the cell's port widths
 * and parameters.
 */
#ifndef ODIN3_TECHLIB_FNSIM_H
#define ODIN3_TECHLIB_FNSIM_H

#include "ir/celltype.h"
#include "odin3/odin3.h"
#include "sim/cell.h"
#include "techlib/reader.h"
#include "util/arena.h"
#include "util/str.h"

#include <stdint.h>

typedef struct odin3_fnsim_prog odin3_fnsim_prog;

/* What a program is compiled from: the cell's definition and library data (fns, port mods). */
typedef struct odin3_fnsim_source {
    odin3_arena *arena;         /* receives the program; must live as long as the design */
    const odin3_strtab *strtab; /* names the expressions' identifiers */
    const odin3_celltype_def *def;
    const odin3_techlib_cell *lib;
} odin3_fnsim_source;

/*
 * Compiles the cell's fns into *out: NULL when the cell cannot be simulated (it has no fn, a
 * seq or a memory, or an fn uses something the interpreter does not support; see above).
 * ODIN3_ERR_NO_MEMORY on out of memory (*out untouched).
 */
odin3_status odin3_fnsim_compile(const odin3_fnsim_source *src, const odin3_fnsim_prog **out);

/* The simulate hook of a compiled cell (sim/cell.h); type_data is its odin3_techlib_cell. */
void odin3_fnsim_simulate(const odin3_sim_cell *cell);

/*
 * The sim_scratch_bytes hook of a compiled cell (celltype.h). ODIN3_ERR_INVALID_ARG when a value
 * of the expression would be wider than 2^24 bits or the scratch would pass UINT32_MAX bytes (or
 * the type data is missing); ODIN3_ERR_NO_MEMORY on out of memory.
 */
odin3_status odin3_fnsim_scratch(const odin3_sim_cell *cell, uint32_t *bytes);

#endif
