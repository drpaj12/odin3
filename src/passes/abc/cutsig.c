/*
 * cutsig.c — signatures and the local proof of the exact LUT cut (`abc --prov-cut on`). The old
 * region is built as an AIG (aig.c) and simulated on random patterns; every old net and every
 * result object gets a signature; a result object and an old net with equal signatures (up to
 * complement) are candidates for the same function. A cut is accepted only after cut_prove: the AIG
 * function of the root net over its leaf nets equals the LUT's truth table on every input
 * pattern, so the equivalence of the leaves is the only thing the signatures have to supply.
 */
#include "ir/lut.h"
#include "ir/module.h"
#include "passes/abc/cut_int.h"
#include "passes/abc/region.h"
#include "util/log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { BITS_PER_WORD = 64, SPLIT_A = 30, SPLIT_B = 27, SPLIT_C = 31, STD_VARS = 6 };

static const uint64_t K_SEED = 0x9E3779B97F4A7C15ULL;
static const uint64_t K_MIX1 = 0xBF58476D1CE4E5B9ULL;
static const uint64_t K_MIX2 = 0x94D049BB133111EBULL;
static const uint64_t K_ONES = ~0ULL;

/* The patterns of input v < 6 inside one 64-bit word. */
static const uint64_t K_STD[STD_VARS] = {0xAAAAAAAAAAAAAAAAULL, 0xCCCCCCCCCCCCCCCCULL,
                                         0xF0F0F0F0F0F0F0F0ULL, 0xFF00FF00FF00FF00ULL,
                                         0xFFFF0000FFFF0000ULL, 0xFFFFFFFF00000000ULL};

static uint64_t mix64(uint64_t val) {
    val += K_SEED;
    val = (val ^ (val >> SPLIT_A)) * K_MIX1;
    val = (val ^ (val >> SPLIT_B)) * K_MIX2;
    return val ^ (val >> SPLIT_C);
}

static uint32_t at32(const odin3_vec *vec, size_t idx) {
    return ((const uint32_t *)vec->data)[idx];
}

static int32_t aig_lit(const cut *ct, size_t obj, size_t slot) {
    return ((const int32_t *)ct->aig.objs.data)[obj * 2U + slot];
}

static uint64_t *words_at(odin3_vec *vec, size_t obj) {
    return (uint64_t *)vec->data + obj * CUT_SIG_WORDS;
}

static odin3_status grow64(odin3_vec *vec, size_t count) {
    odin3_vec_init(vec, sizeof(uint64_t));
    odin3_status st = odin3_vec_reserve(vec, count + 1U);
    if (st == ODIN3_OK) {
        memset(vec->data, 0, sizeof(uint64_t) * (count + 1U));
        vec->len = count;
    }
    return st;
}

/* --- simulating the AIG --------------------------------------------------------------------- */

static void lit_words(const cut *ct, int32_t lit, uint64_t *out) {
    const uint64_t *src =
        (const uint64_t *)ct->asig.data + (size_t)((uint32_t)lit >> 1) * CUT_SIG_WORDS;
    uint64_t flip = ((uint32_t)lit & 1U) != 0 ? K_ONES : 0;
    for (uint32_t wd = 0; wd < CUT_SIG_WORDS; wd++) {
        out[wd] = src[wd] ^ flip;
    }
}

static void sim_object(cut *ct, uint32_t obj) {
    uint64_t *dst = words_at(&ct->asig, obj);
    uint64_t one[CUT_SIG_WORDS];
    uint64_t two[CUT_SIG_WORDS];
    if (obj == 0) {
        return;
    }
    if (aig_lit(ct, obj, 0) == ODIN3_ABC_AIG_NULL) {
        for (uint32_t wd = 0; wd < CUT_SIG_WORDS; wd++) {
            dst[wd] = mix64(((uint64_t)obj << STD_VARS) + wd);
        }
        return;
    }
    lit_words(ct, aig_lit(ct, obj, 0), one);
    if (aig_lit(ct, obj, 1) == ODIN3_ABC_AIG_NULL) {
        memcpy(dst, one, sizeof one);
        return;
    }
    lit_words(ct, aig_lit(ct, obj, 1), two);
    for (uint32_t wd = 0; wd < CUT_SIG_WORDS; wd++) {
        dst[wd] = one[wd] & two[wd];
    }
}

static odin3_status sim_aig(cut *ct) {
    uint32_t n_objs = (uint32_t)(ct->aig.objs.len / 2U);
    odin3_status st = grow64(&ct->asig, (size_t)n_objs * CUT_SIG_WORDS);
    for (uint32_t obj = 0; st == ODIN3_OK && obj < n_objs; obj++) {
        sim_object(ct, obj);
    }
    return st;
}

/* --- the literal of a net ------------------------------------------------------------------- */

static bool reg_inverted(const cut *ct, uint32_t reg) {
    odin3_node_id node = {at32(&ct->run->region->regs, reg)};
    const odin3_value *init = odin3_node_param(ct->mod, node, 0);
    return init != NULL && init->kind == ODIN3_VAL_INT && init->i == 1;
}

/* The AIG literal of an old net (as aig.c reads it), or -1 when the net has none or is a constant.
 */
static int32_t net_lit(const cut *ct, odin3_net_id net) {
    const odin3_abc_region *region = ct->run->region;
    if (!odin3_net_valid(net) || !odin3_net_live(ct->mod, net) ||
        odin3_abc_pin_literal(ct->run, net) >= 0) {
        return -1;
    }
    uint32_t ci = at32(&region->ci_of, net.v);
    uint32_t reg = at32(&region->q_of, net.v);
    if (ci != 0) {
        return 2 * (int32_t)ci;
    }
    if (reg != 0) {
        uint32_t obj = 1U + (uint32_t)region->cis.len + (reg - 1U);
        return (2 * (int32_t)obj) ^ (reg_inverted(ct, reg - 1U) ? 1 : 0);
    }
    return odin3_abc_aig_lit(&ct->aig, net);
}

/* --- canonical signatures and the table ----------------------------------------------------- */

/* Canonical form of a signature (bit 0 clear) and its hash; returns 1 when it complemented. */
static uint32_t canon(const uint64_t *sig, uint64_t *hash, bool *constant) {
    uint32_t inv = (sig[0] & 1U) != 0 ? 1U : 0U;
    uint64_t flip = inv != 0 ? K_ONES : 0;
    uint64_t acc = K_SEED;
    bool zero = true;
    for (uint32_t wd = 0; wd < CUT_SIG_WORDS; wd++) {
        uint64_t word = sig[wd] ^ flip;
        zero = zero && word == 0;
        acc = mix64(acc ^ word);
    }
    *hash = acc;
    *constant = zero;
    return inv;
}

static int cmp_cand(const void *lhs, const void *rhs) {
    const cut_cand *left = lhs;
    const cut_cand *right = rhs;
    if (left->hash != right->hash) {
        return left->hash < right->hash ? -1 : 1;
    }
    if (left->net != right->net) {
        return left->net < right->net ? -1 : 1;
    }
    return 0;
}

static odin3_status add_net(cut *ct, uint32_t net) {
    int32_t lit = net_lit(ct, (odin3_net_id){net});
    if (lit < 0) {
        return ODIN3_OK;
    }
    uint64_t sig[CUT_SIG_WORDS];
    lit_words(ct, lit, sig);
    cut_cand cand = {0, net, 0};
    bool constant = false;
    cand.inv = canon(sig, &cand.hash, &constant);
    if (constant) {
        return ODIN3_OK;
    }
    cut_cand *slot = odin3_vec_push(&ct->table);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = cand;
    return ODIN3_OK;
}

static odin3_status build_table(cut *ct) {
    odin3_status st = ODIN3_OK;
    uint32_t end = odin3_module_net_end(ct->mod);
    for (uint32_t net = 1; st == ODIN3_OK && net < end; net++) {
        st = add_net(ct, net);
    }
    if (st == ODIN3_OK && ct->table.len > 1) {
        qsort(ct->table.data, ct->table.len, sizeof(cut_cand), cmp_cand);
    }
    return st;
}

/* --- signatures of the result objects ------------------------------------------------------- */

static uint32_t bit_of(const uint64_t *sig, uint32_t pos) {
    return (uint32_t)((sig[pos / BITS_PER_WORD] >> (pos % BITS_PER_WORD)) & 1U);
}

static void lut_sig(cut *ct, uint32_t lut, uint64_t *dst) {
    const odin3_abc_result *res = ct->res;
    uint32_t inputs = at32(&res->lut_k, lut);
    const uint32_t *fanins = (const uint32_t *)res->fanins.data + at32(&res->fanin_start, lut);
    const uint64_t *truth = (const uint64_t *)res->truth.data + at32(&res->truth_start, lut);
    for (uint32_t pos = 0; pos < CUT_SIG_WORDS * BITS_PER_WORD; pos++) {
        uint32_t idx = 0;
        for (uint32_t var = 0; var < inputs; var++) {
            idx |= bit_of(words_at(&ct->osig, fanins[var]), pos) << var;
        }
        dst[pos / BITS_PER_WORD] |=
            (uint64_t)((truth[idx / BITS_PER_WORD] >> (idx % BITS_PER_WORD)) & 1U)
            << (pos % BITS_PER_WORD);
    }
}

static odin3_status sign_objects(cut *ct) {
    const odin3_abc_result *res = ct->res;
    uint32_t first = odin3_abc_result_first_lut(res);
    odin3_status st = grow64(&ct->osig, (size_t)(first + res->n_luts) * CUT_SIG_WORDS);
    if (st != ODIN3_OK) {
        return st;
    }
    for (uint32_t wd = 0; wd < CUT_SIG_WORDS; wd++) {
        words_at(&ct->osig, 1)[wd] = K_ONES;
    }
    for (uint32_t ci = 0; ci < res->n_cis; ci++) {
        memcpy(words_at(&ct->osig, CUT_FIRST_OBJ + ci), words_at(&ct->asig, 1U + ci),
               sizeof(uint64_t) * CUT_SIG_WORDS);
    }
    for (uint32_t reg = 0; reg < res->n_regs_out; reg++) {
        uint32_t from = at32(&res->reg_from, reg);
        if (from != ODIN3_ABC_NO_REG) {
            memcpy(words_at(&ct->osig, CUT_FIRST_OBJ + res->n_cis + reg),
                   words_at(&ct->asig, 1U + res->n_cis + from), sizeof(uint64_t) * CUT_SIG_WORDS);
        }
    }
    for (uint32_t lut = 0; lut < res->n_luts; lut++) {
        lut_sig(ct, lut, words_at(&ct->osig, first + lut));
    }
    return ODIN3_OK;
}

odin3_status cut_sigs_build(cut *ct) {
    odin3_status st = odin3_abc_aig_build(ct->run, ct->run->region, &ct->aig);
    st = st == ODIN3_OK ? sim_aig(ct) : st;
    st = st == ODIN3_OK ? build_table(ct) : st;
    return st == ODIN3_OK ? sign_objects(ct) : st;
}

void cut_free(cut *ct) {
    odin3_abc_aig_free(&ct->aig);
    odin3_vec_free(&ct->asig);
    odin3_vec_free(&ct->osig);
    odin3_vec_free(&ct->table);
    odin3_vec_free(&ct->leaf);
    odin3_vec_free(&ct->lvar);
    odin3_vec_free(&ct->linv);
    odin3_vec_free(&ct->seen);
    odin3_vec_free(&ct->stack);
    odin3_vec_free(&ct->gates);
    odin3_vec_free(&ct->vstamp);
    odin3_vec_free(&ct->voff);
    odin3_vec_free(&ct->vpool);
    odin3_vec_free(&ct->leafs);
}

odin3_status cut_candidates(const cut *ct, uint32_t obj, odin3_vec *out) {
    odin3_vec_clear(out);
    uint64_t hash = 0;
    bool constant = false;
    const uint64_t *sig = (const uint64_t *)ct->osig.data + (size_t)obj * CUT_SIG_WORDS;
    uint32_t inv = canon(sig, &hash, &constant);
    if (constant) {
        return ODIN3_OK;
    }
    const cut_cand *tab = ct->table.data;
    size_t lo = 0;
    size_t hi = ct->table.len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;
        lo = tab[mid].hash < hash ? mid + 1U : lo;
        hi = tab[mid].hash < hash ? hi : mid;
    }
    for (size_t at = lo; at < ct->table.len && tab[at].hash == hash && out->len < CUT_MAX_CANDS;
         at++) {
        cut_net *slot = odin3_vec_push(out);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = (cut_net){tab[at].net, tab[at].inv ^ inv};
    }
    return ODIN3_OK;
}

/* --- the proof ------------------------------------------------------------------------------ */

static const uint64_t K_ZERO[CUT_PROVE_WORDS_MAX] = {0};

static bool valued(const cut *ct, uint32_t var) {
    return at32(&ct->vstamp, var) == ct->gen;
}

/* Input pattern word of input v. */
static uint64_t input_pattern(uint32_t var, uint32_t word) {
    if (var < STD_VARS) {
        return K_STD[var];
    }
    return ((word >> (var - STD_VARS)) & 1U) != 0 ? K_ONES : 0;
}

/* Gives AIG object var the value words `vals`; false when it already has a value. */
static bool assign_value(cut *ct, uint32_t var, const uint64_t *vals) {
    if (at32(&ct->vstamp, var) == ct->gen) {
        return false;
    }
    ((uint32_t *)ct->vstamp.data)[var] = ct->gen;
    ((uint32_t *)ct->voff.data)[var] = (uint32_t)ct->vpool.len;
    for (uint32_t wd = 0; wd < ct->nw; wd++) {
        uint64_t *slot = odin3_vec_push(&ct->vpool);
        if (slot == NULL) {
            return false;
        }
        *slot = vals[wd];
    }
    return true;
}

static const uint64_t *value_of(const cut *ct, uint32_t var) {
    return (const uint64_t *)ct->vpool.data + at32(&ct->voff, var);
}

/* Gives the AIG object of one leaf net its pattern. Two leaf nets on one object (equal nets) are
 * fine when they ask for the same pattern. */
static bool seed_one(cut *ct, const cut_leaf *leaf) {
    uint64_t vals[CUT_PROVE_WORDS_MAX];
    int32_t lit = net_lit(ct, (odin3_net_id){leaf->net});
    if (lit <= 1) {
        return false;
    }
    uint32_t var = (uint32_t)lit >> 1;
    uint64_t flip = (leaf->inv ^ ((uint32_t)lit & 1U)) != 0 ? K_ONES : 0;
    for (uint32_t wd = 0; wd < ct->nw; wd++) {
        vals[wd] = input_pattern(leaf->v, wd) ^ flip;
    }
    if (valued(ct, var)) {
        return memcmp(value_of(ct, var), vals, sizeof(uint64_t) * ct->nw) == 0;
    }
    return assign_value(ct, var, vals);
}

static bool seed_leaves(cut *ct) {
    for (size_t at = 0; at < ct->leafs.len; at++) {
        if (!seed_one(ct, odin3_vec_cat(&ct->leafs, at))) {
            return false;
        }
    }
    return true;
}

/* Evaluates AND object var from its two children (both valued). */
static bool eval_and(cut *ct, uint32_t var) {
    int32_t l0 = aig_lit(ct, var, 0);
    int32_t l1 = aig_lit(ct, var, 1);
    uint64_t vals[CUT_PROVE_WORDS_MAX];
    const uint64_t *v0 = value_of(ct, (uint32_t)l0 >> 1);
    const uint64_t *v1 = value_of(ct, (uint32_t)l1 >> 1);
    uint64_t f0 = ((uint32_t)l0 & 1U) != 0 ? K_ONES : 0;
    uint64_t f1 = ((uint32_t)l1 & 1U) != 0 ? K_ONES : 0;
    for (uint32_t wd = 0; wd < ct->nw; wd++) {
        vals[wd] = (v0[wd] ^ f0) & (v1[wd] ^ f1);
    }
    return assign_value(ct, var, vals);
}

typedef enum ev_step { EV_POP, EV_PUSH, EV_FAIL } ev_step;

/* One step on the object on top of the stack: value it, or queue its children. */
static ev_step eval_step(cut *ct, uint32_t var) {
    if (var == 0) {
        return assign_value(ct, 0, K_ZERO) ? EV_POP : EV_FAIL;
    }
    int32_t lit0 = aig_lit(ct, var, 0);
    int32_t lit1 = aig_lit(ct, var, 1);
    if (lit0 == ODIN3_ABC_AIG_NULL || lit1 == ODIN3_ABC_AIG_NULL) {
        return EV_FAIL; /* a primary input that no leaf stands for */
    }
    uint32_t kid0 = (uint32_t)lit0 >> 1;
    uint32_t kid1 = (uint32_t)lit1 >> 1;
    if (valued(ct, kid0) && valued(ct, kid1)) {
        return eval_and(ct, var) ? EV_POP : EV_FAIL;
    }
    const uint32_t kids[2] = {kid0, kid1};
    for (size_t at = 0; at < 2U; at++) {
        uint32_t *slot = valued(ct, kids[at]) ? NULL : odin3_vec_push(&ct->stack);
        if (slot != NULL) {
            *slot = kids[at];
        } else if (!valued(ct, kids[at])) {
            return EV_FAIL;
        }
    }
    return EV_PUSH;
}

/* Evaluates the AIG object `root` over the seeded leaves; false when it needs an input that is
 * not a leaf. */
static bool eval_root(cut *ct, uint32_t root) {
    odin3_vec_clear(&ct->stack);
    uint32_t *top = odin3_vec_push(&ct->stack);
    if (top == NULL) {
        return false;
    }
    *top = root;
    while (ct->stack.len > 0) {
        uint32_t var = at32(&ct->stack, ct->stack.len - 1U);
        if (valued(ct, var)) {
            odin3_vec_pop(&ct->stack);
            continue;
        }
        ev_step step = eval_step(ct, var);
        if (step == EV_FAIL) {
            return false;
        }
        if (step == EV_POP) {
            odin3_vec_pop(&ct->stack);
        }
    }
    return true;
}

typedef struct proof_cmp {
    uint32_t lut;
    uint32_t var;
    uint64_t flip;
} proof_cmp;

static bool same_function(const cut *ct, const proof_cmp *pc) {
    uint32_t lut = pc->lut;
    uint32_t inputs = at32(&ct->res->lut_k, lut);
    uint64_t valid = inputs >= STD_VARS ? K_ONES : ((1ULL << (1U << inputs)) - 1ULL);
    const uint64_t *truth =
        (const uint64_t *)ct->res->truth.data + at32(&ct->res->truth_start, lut);
    const uint64_t *val = value_of(ct, pc->var);
    for (uint32_t wd = 0; wd < ct->nw; wd++) {
        if (((val[wd] ^ pc->flip) ^ truth[wd]) & valid) {
            return false;
        }
    }
    return true;
}

bool cut_prove(cut *ct, uint32_t lut, const cut_net *root) {
    uint32_t inputs = at32(&ct->res->lut_k, lut);
    int32_t lit = net_lit(ct, (odin3_net_id){root->net});
    if (inputs > CUT_PROVE_MAX_K || lit <= 1) {
        return false;
    }
    ct->nw = inputs > STD_VARS ? 1U << (inputs - STD_VARS) : 1U;
    ct->gen++;
    odin3_vec_clear(&ct->vpool);
    proof_cmp pc = {lut, (uint32_t)lit >> 1, 0};
    pc.flip = (root->inv ^ ((uint32_t)lit & 1U)) != 0 ? K_ONES : 0;
    bool ok = seed_leaves(ct) && !valued(ct, pc.var) && eval_root(ct, pc.var);
    return ok && same_function(ct, &pc);
}
