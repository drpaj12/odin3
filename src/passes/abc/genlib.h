/*
 * genlib.h — ABC's genlib text derived from an .o3lib library's gate cells (gatemap spec 3; plan
 * Task 4, GM-2, GM-3, GM-5). abc --gates hands the text to the linked engine (load_genlib) and logs
 * its SHA-256, so a run traces to the exact library.
 *
 * Text (exact): for each gate cell (techlib/libcells.h) in library order
 *
 *   GATE <name> <area text> <out port>=<formula>;
 *   PIN <input> UNKNOWN 1 999 <d> 0 <d> 0          (one line per input, in port order)
 *
 * with <d> the cell's delay text or 1, then the two constant gates
 *
 *   GATE $odin3_const0 0 Y=CONST0;
 *   GATE $odin3_const1 0 Y=CONST1;
 *
 * (`$` cannot start an .o3lib name, so they never clash with a cell). The formula is the on-set
 * ISOP of the cell's table (odin3_blif_isop_set with offset false), its rows in emitted order
 * joined by `+`; a row's literals for input j ascending (plane `1` the input's name, `0` `!name`,
 * `-` nothing) joined by `*`; no blanks. One PIN line per input (GM-2) keeps ABC's pin order the
 * cell's port order; every input is in the support (libcells drops a cell that ignores one).
 */
#ifndef ODIN3_PASSES_ABC_GENLIB_H
#define ODIN3_PASSES_ABC_GENLIB_H

#include "ir/design.h"
#include "odin3/odin3.h"
#include "techlib/libcells.h"
#include "util/sha256.h"
#include "util/str.h"

#define ODIN3_ABC_GENLIB_CONST0 "$odin3_const0"
#define ODIN3_ABC_GENLIB_CONST1 "$odin3_const1"

/*
 * Checks that cells (collected for the library named library) can be mapped to. Logs one ERROR
 * and returns ODIN3_ERR_INVALID_ARG for the first of: no cell (`abc: --gates: library '<L>' is not
 * loaded; read it with read_techlib first`); a gate cell without a numeric area (`abc: --gates:
 * library '<L>': cell '<C>' has no numeric area`); no gate of 1 input and table 0x1, or none of 2
 * inputs and table 0x8, 0x7, 0xE or 0x1 (`abc: --gates: library '<L>' has no inverter and 2-input
 * AND, NAND, OR or NOR gate; ABC cannot map to it`). ODIN3_OK otherwise, without logging.
 */
odin3_status odin3_abc_genlib_check(const odin3_design *design, const odin3_libcells *cells,
                                    const char *library);

/*
 * Appends the genlib text of cells to out (see above). A gate cell without area text gets area 0
 * (odin3_abc_genlib_check refuses such a library first). ODIN3_ERR_NO_MEMORY (out may hold part of
 * the text); never logs.
 */
odin3_status odin3_abc_genlib_text(const odin3_design *design, const odin3_libcells *cells,
                                   odin3_strbuf *out);

/* The SHA-256 of text's bytes as 64 lower-case hex digits and a NUL. */
void odin3_abc_genlib_sha256(const odin3_strbuf *text, char hex[ODIN3_SHA256_HEX_CHARS]);

#endif
