/*
 * isop.h — the irredundant sum-of-products cover of a truth table (spec 3E §4.1, VB-4; plan 3E
 * point (t)): how write_blif writes a $lut as `.names` in both dialects (§6), and what 3D's BLIF
 * exchange, AIG builder and Verilog writer use for a LUT's function (3D spec AB-3).
 *
 * A truth table of k inputs is odin3_blif_isop_words(k) 64-bit words: bit i of the table (word
 * i / 64, bit i % 64) is f(i), where bit j of the index i is the value of input j. This is the
 * layout of odin3_lut_table (ir/lut.h), so a $lut's table is f with input j = A[j].
 */
#ifndef ODIN3_BACKENDS_BLIF_ISOP_H
#define ODIN3_BACKENDS_BLIF_ISOP_H

#include "odin3/odin3.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>

/* The widest table: ODIN3_BLIF_VTR_MAX_K (writer.h) and ODIN3_LUT_MAX_WIDTH (ir/lut.h). */
#define ODIN3_BLIF_ISOP_MAX_K 16U

/* The words of a table of k = inputs inputs: max(1, 2^k / 64); that of ODIN3_BLIF_ISOP_MAX_K for
 * a larger k. */
uint32_t odin3_blif_isop_words(uint32_t inputs);

/*
 * An irredundant sum-of-products of the k-input function (k = inputs) whose truth table is tt
 * (bit i = f(i), where bit j of the index i is the value of input j), by the Minato-Morreale ISOP
 * on an explicit stack (no recursion), for on-set and off-set; the shorter is chosen (on-set on a
 * tie). Rows are appended to cover as BLIF plane text (k characters of 0/1/-, character j for input
 * j, then the output character), in the deterministic order the algorithm emits them. A constant
 * function gives *rows = 0 and *constant = 0 or 1 (the caller writes a zero-input generator, §6);
 * otherwise *constant = -1. k <= ODIN3_BLIF_VTR_MAX_K.
 *
 * Exact form (plan 3E point (t)): tt holds odin3_blif_isop_words(k) words, and the bits from 2^k up
 * of word 0 (k < 6) are ignored. Each row is the k plane characters, a blank, the output character
 * (`1` for the on-set, `0` for the off-set, which covers the complement) and a newline: `11 1\n`
 * (for k = 0, the output character and the newline: odin3_blif_isop_set's `1\n`, as BLIF writes a
 * zero-input row). The cover of isop(L, U, n) (L within U, over inputs 0 .. n - 1) splits on input
 * n - 1: C0 = isop(L0 & ~U1, U0), C1 = isop(L1 & ~U0, U1), then L* = (L0 & ~C0) | (L1 & ~C1) and
 * C* = isop(L*, U0 & U1), written as C0's rows with input n - 1 = `0`, then C1's with `1`, then
 * C*'s with `-`; n = 0 gives one row of `-` when L is not 0. "Shorter" counts rows. A constant
 * appends nothing. ODIN3_ERR_INVALID_ARG for a NULL argument or k > ODIN3_BLIF_ISOP_MAX_K;
 * ODIN3_ERR_NO_MEMORY on out of memory. Nothing is logged; on failure cover is as it was and *rows
 * and *constant are untouched. Memory: one arena of k + 1 levels of five tables each (64 KiB at
 * k = 16) besides cover. The off-set is first only counted, and stops once it has as many rows as
 * the on-set; its rows are emitted only when it is the shorter.
 */
odin3_status odin3_blif_isop(const uint64_t *tt, uint32_t inputs, odin3_strbuf *cover,
                             uint32_t *rows, int *constant);

/*
 * One set's ISOP of tt, as odin3_blif_isop computes it: the on-set's cover of f (rows ending `1`)
 * or, when offset, the off-set's cover of the complement of f (rows ending `0`), appended to cover
 * with *rows set to their number. A constant is not special: the cover of a constant-0 set is no
 * row, of a constant-1 set one row of k `-`. For the tests and for 3D's cover choice. Errors as
 * odin3_blif_isop (*rows untouched on failure).
 */
odin3_status odin3_blif_isop_set(const uint64_t *tt, uint32_t inputs, bool offset,
                                 odin3_strbuf *cover, uint32_t *rows);

#endif
