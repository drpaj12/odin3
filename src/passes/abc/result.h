/*
 * result.h — the one result form both exchanges give the splice (3D spec §6.1, AB-14; plan Task
 * 16): objects numbered like MiniLUT. Pure data and checks; nothing here includes ABC.
 */
#ifndef ODIN3_PASSES_ABC_RESULT_H
#define ODIN3_PASSES_ABC_RESULT_H

#include "odin3/odin3.h"
#include "util/vec.h"

#include <stdint.h>

/* reg_from value of a result register that carries no input register's name. */
#define ODIN3_ABC_NO_REG UINT32_MAX

/* What the run sent: the real inputs and outputs, the registers, and --lut K (0: none). */
typedef struct odin3_abc_result_expect {
    uint32_t n_cis, n_cos, n_regs, lut_k_max;
} odin3_abc_result_expect;

/*
 * A mapped network. Objects: 0 and 1 are the constants, then the n_cis CIs as sent (real inputs),
 * then the n_regs_out register outputs, then the n_luts LUTs in topological order (object
 * 2 + n_cis + n_regs_out + i is LUT i).
 * - fanin_start (n_luts + 1 entries) indexes fanins (object numbers, all lower than the LUT's);
 *   lut_k holds each LUT's input count.
 * - truth_start (n_luts + 1 entries) indexes truth, odin3_lut_words(k) uint64 words per LUT: bit i
 *   is the function for input assignment i (input v = fanin v), bits from 2^k up are zero.
 * - reg_from gives each result register its input register k* (ODIN3_ABC_NO_REG: unnamed).
 * - co_driver holds the driver object of each CO: the n_cos real COs, then n_obs observation
 *   outputs (in-memory only), then the n_regs_out register inputs.
 * - old_lit maps objects to old AIG literals (§8.2); empty without a name map.
 * All vecs hold uint32_t except truth (uint64_t).
 */
typedef struct odin3_abc_result {
    uint32_t n_cis, n_regs_out, n_luts, n_cos, n_obs;
    odin3_vec fanin_start, fanins, lut_k, truth_start, truth, reg_from, co_driver, old_lit;
} odin3_abc_result;

/* An empty result with its vecs initialised (also valid after odin3_abc_result_free). */
void odin3_abc_result_init(odin3_abc_result *res);

/* Frees the vecs and re-initialises *res. */
void odin3_abc_result_free(odin3_abc_result *res);

/* The first object number of the LUTs. */
uint32_t odin3_abc_result_first_lut(const odin3_abc_result *res);

/*
 * Checks a result against what was sent (§6.1), in this order, filling fail with kind RESULT,
 * code 0 and the text `<why>` of §11: `N primary inputs, expected M`; `N outputs, expected M`; `N
 * registers, more than the M it was given`; `register J carries no input register's name`; `a LUT
 * with N inputs (at most 16)`; `a LUT with N inputs exceeds --lut K`. The caller logs `abc: ABC
 * returned a malformed network: <why>`. ODIN3_OK, or ODIN3_ERR_ENGINE with *fail filled.
 */
odin3_status odin3_abc_result_check(const odin3_abc_result *res,
                                    const odin3_abc_result_expect *expect, odin3_abc_fail *fail);

/*
 * Constant folding (I10): a LUT whose table is all 0 or all 1 over its 2^k bits becomes object 0
 * or 1. Its readers are rewired and each reader LUT gets that variable's cofactor of its table
 * (and one input fewer); the folded LUTs are removed and the objects renumbered. One pass in
 * topological order, linear in objects, fanins and table words, no recursion. NO_MEMORY leaves
 * *res as it was.
 */
odin3_status odin3_abc_result_fold(odin3_abc_result *res);

#endif
