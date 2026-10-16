/*
 * lut.h — the $lut cell's interface (3D spec §2, AB-2; docs/IR.md "LUT cells"): its built-in ID,
 * its parameter and port indices, and the helpers that turn a truth table into its parameters and
 * back. Yosys $lut semantics: Y = LUT[i] with i = sum of A[j] * 2^j, where LUT[i] is bit i of the
 * BITS parameter counted from its least significant bit (one odin3_bit per byte, IR-10).
 */
#ifndef ODIN3_IR_LUT_H
#define ODIN3_IR_LUT_H

#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"

#include <stdbool.h>
#include <stdint.h>

/* The widest $lut: WIDTH is 1 .. ODIN3_LUT_MAX_WIDTH, so LUT has at most 65,536 bits. */
enum { ODIN3_LUT_MAX_WIDTH = 16 };

/* A table is stored in 64-bit words (see odin3_lut_words); the most a $lut's table holds. */
enum {
    ODIN3_LUT_WORD_BITS = 64,
    ODIN3_LUT_WORD_SHIFT = 6, /* log2(ODIN3_LUT_WORD_BITS) */
    ODIN3_LUT_MAX_WORDS = 1 << (ODIN3_LUT_MAX_WIDTH - ODIN3_LUT_WORD_SHIFT)
};

/* Parameter indices (definition order) and port indices of $lut. */
enum { ODIN3_LUT_P_WIDTH = 0, ODIN3_LUT_P_LUT = 1 };
enum { ODIN3_LUT_PORT_A = 0, ODIN3_LUT_PORT_Y = 1 };

/*
 * The index of $lut in the built-in table (cells/builtin.c), which is its built-in cell-type ID.
 * Ruling I5: the only way a test or a pass names that ID; nothing writes it as a number.
 */
uint32_t odin3_lut_builtin_index(void);

/*
 * The 64-bit words of a 2^width-bit table: 1 for width <= 6, else 2^(width - 6). Bit i of a table
 * is bit i % 64 of word i / 64. For width above ODIN3_LUT_MAX_WIDTH the result is that of
 * ODIN3_LUT_MAX_WIDTH.
 */
uint32_t odin3_lut_words(uint32_t width);

/*
 * Builds a $lut's parameters from a truth table: params[ODIN3_LUT_P_WIDTH] = INT width and
 * params[ODIN3_LUT_P_LUT] = BITS of 2^width bits, LUT[i] = bit i of words (word i / 64, bit
 * i % 64). words holds odin3_lut_words(width) words; bits from 2^width up are ignored. The BITS
 * payload is allocated in arena (it lives as long as the arena; node creation copies it).
 * ODIN3_ERR_INVALID_ARG (not logged) for width 0 or above ODIN3_LUT_MAX_WIDTH or a NULL argument;
 * ODIN3_ERR_NO_MEMORY on out of memory. params is untouched on failure.
 */
odin3_status odin3_lut_params(odin3_arena *arena, uint32_t width, const uint64_t *words,
                              odin3_value params[2]);

/*
 * Reads a $lut's table into words (n_words of them, bit i of the table at word i / 64, bit
 * i % 64), every bit from 2^WIDTH up zero. False (nothing logged; words untouched) for parameters
 * the cell's verify rejects, a NULL argument, or n_words below odin3_lut_words(WIDTH).
 * Consumers (3E's odin3_blif_isop): the bits above 2^WIDTH are zero, not replicated as ABC
 * replicates a narrow table across its words, so the result is a WIDTH-variable table and a
 * routine working on whole 64-bit words must mask a table of WIDTH < 6.
 */
bool odin3_lut_table(const odin3_value params[2], uint64_t *words, uint32_t n_words);

/*
 * Builds a $lut's parameters from an engine truth table (3D spec §2, "engine tables"): truth holds
 * W = (lut_size > 5 ? 1 << (lut_size - 5) : 1) 32-bit words, word 0 and bit 0 of each word least
 * significant; LUT[i] for i < 2^n_inputs is bit i of that array, and every bit from 2^n_inputs up
 * is ignored (ABC replicates the table there). WIDTH is n_inputs. The BITS payload is allocated in
 * arena. Callers (the 3D splice): input variable v is A[v] = Mini_LutNodeFanin(p, id, v), the
 * MiniLUT's own fanin order, never the GIA LUT's (giaMini.c rotates the leaves of LUTs above six
 * inputs before computing the truth table); lut_size is Mini_LutSize(p), which is max(2, largest
 * LUT), not the K asked for. "Word 0 least significant" holds because ABC computes the table in
 * 64-bit words and casts them to 32-bit unsigned words, which on a little-endian host (x86-64,
 * AArch64) puts the low half of each 64-bit word first. ODIN3_ERR_INVALID_ARG (not logged) for
 * n_inputs 0, n_inputs above lut_size, lut_size above ODIN3_LUT_MAX_WIDTH or a NULL argument;
 * ODIN3_ERR_NO_MEMORY on out of memory. params is untouched on failure.
 */
odin3_status odin3_lut_from_engine(odin3_arena *arena, uint32_t n_inputs, uint32_t lut_size,
                                   const uint32_t *truth, odin3_value params[2]);

#endif
