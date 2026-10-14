/*
 * result.c — the result form's checks and constant folding (3D spec §6.1, AB-14; plan Task 16).
 */
#include "passes/abc/result.h"

#include "ir/lut.h"
#include "odin3/odin3.h"
#include "util/vec.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { FIRST_OBJ = 2 /* objects 0 and 1 are the constants */ };

void odin3_abc_result_init(odin3_abc_result *res) {
    *res = (odin3_abc_result){0};
    odin3_vec_init(&res->fanin_start, sizeof(uint32_t));
    odin3_vec_init(&res->fanins, sizeof(uint32_t));
    odin3_vec_init(&res->lut_k, sizeof(uint32_t));
    odin3_vec_init(&res->truth_start, sizeof(uint32_t));
    odin3_vec_init(&res->truth, sizeof(uint64_t));
    odin3_vec_init(&res->reg_from, sizeof(uint32_t));
    odin3_vec_init(&res->co_driver, sizeof(uint32_t));
    odin3_vec_init(&res->old_lit, sizeof(uint32_t));
}

void odin3_abc_result_free(odin3_abc_result *res) {
    odin3_vec_free(&res->fanin_start);
    odin3_vec_free(&res->fanins);
    odin3_vec_free(&res->lut_k);
    odin3_vec_free(&res->truth_start);
    odin3_vec_free(&res->truth);
    odin3_vec_free(&res->reg_from);
    odin3_vec_free(&res->co_driver);
    odin3_vec_free(&res->old_lit);
    odin3_abc_result_init(res);
}

uint32_t odin3_abc_result_first_lut(const odin3_abc_result *res) {
    return FIRST_OBJ + res->n_cis + res->n_regs_out;
}

static uint32_t u32_at(const odin3_vec *vec, size_t idx) {
    return ((const uint32_t *)vec->data)[idx];
}

/* --- checks -------------------------------------------------------------------------------- */

/* The two numbers of a message: what was found and what was expected (or the limit). */
typedef struct count_pair {
    uint32_t got;
    uint32_t want;
} count_pair;

/* The text is fmt with the pair as its two %u (a format with one uses only the first). */
static odin3_status fail_with(odin3_abc_fail *fail, const char *fmt, count_pair pair) {
    fail->kind = ODIN3_ABC_FAIL_RESULT;
    fail->code = 0;
    (void)snprintf(fail->text, sizeof fail->text, fmt, (unsigned)pair.got, (unsigned)pair.want);
    return ODIN3_ERR_ENGINE;
}

static odin3_status check_counts(const odin3_abc_result *res, const odin3_abc_result_expect *exp,
                                 odin3_abc_fail *fail) {
    if (res->n_cis != exp->n_cis) {
        return fail_with(fail, "%u primary inputs, expected %u",
                         (count_pair){res->n_cis, exp->n_cis});
    }
    if (res->n_cos != exp->n_cos) {
        return fail_with(fail, "%u outputs, expected %u", (count_pair){res->n_cos, exp->n_cos});
    }
    if (res->n_regs_out > exp->n_regs) {
        return fail_with(fail, "%u registers, more than the %u it was given",
                         (count_pair){res->n_regs_out, exp->n_regs});
    }
    for (uint32_t reg = 0; reg < res->n_regs_out; reg++) {
        if (u32_at(&res->reg_from, reg) >= exp->n_regs) {
            return fail_with(fail, "register %u carries no input register's name",
                             (count_pair){reg, 0});
        }
    }
    return ODIN3_OK;
}

odin3_status odin3_abc_result_check(const odin3_abc_result *res,
                                    const odin3_abc_result_expect *expect, odin3_abc_fail *fail) {
    odin3_status st = check_counts(res, expect, fail);
    for (uint32_t lut = 0; st == ODIN3_OK && lut < res->n_luts; lut++) {
        uint32_t inputs = u32_at(&res->lut_k, lut);
        if (inputs > ODIN3_LUT_MAX_WIDTH) {
            st = fail_with(fail, "a LUT with %u inputs (at most %u)",
                           (count_pair){inputs, ODIN3_LUT_MAX_WIDTH});
        } else if (expect->lut_k_max != 0 && inputs > expect->lut_k_max) {
            st = fail_with(fail, "a LUT with %u inputs exceeds --lut %u",
                           (count_pair){inputs, expect->lut_k_max});
        }
    }
    return st;
}

/* --- folding ------------------------------------------------------------------------------- */

/* Bit idx of a table (words uint64, little-endian bit order). */
static bool table_bit(const uint64_t *table, uint32_t idx) {
    return ((table[idx / ODIN3_LUT_WORD_BITS] >> (idx % ODIN3_LUT_WORD_BITS)) & 1U) != 0;
}

/* One cofactor: a table of vars variables, variable var fixed to val. */
typedef struct cofactor_req {
    uint32_t vars;
    uint32_t var;
    bool val;
} cofactor_req;

/*
 * The cofactor of a table of req->vars inputs in variable req->var at value req->val: a table of
 * one input fewer into out (odin3_lut_words(vars - 1) words, zeroed here).
 */
static void cofactor(const uint64_t *table, const cofactor_req *req, uint64_t *out) {
    memset(out, 0, sizeof(uint64_t) * odin3_lut_words(req->vars - 1U));
    uint32_t low_mask = (1U << req->var) - 1U;
    for (uint32_t idx = 0; idx < (1U << (req->vars - 1U)); idx++) {
        uint32_t full =
            (idx & low_mask) | ((idx & ~low_mask) << 1U) | (req->val ? 1U << req->var : 0U);
        if (table_bit(table, full)) {
            out[idx / ODIN3_LUT_WORD_BITS] |= 1ULL << (idx % ODIN3_LUT_WORD_BITS);
        }
    }
}

/* 0 for an all-0 table over 2^inputs bits, 1 for all-1, -1 otherwise. */
static int table_constant(const uint64_t *table, uint32_t inputs) {
    uint32_t bits = 1U << inputs;
    uint32_t ones = 0;
    for (uint32_t idx = 0; idx < bits; idx++) {
        ones += table_bit(table, idx) ? 1U : 0U;
    }
    if (ones == 0) {
        return 0;
    }
    return ones == bits ? 1 : -1;
}

/* Scratch of one fold: the new arrays and the object map. */
typedef struct fold {
    odin3_abc_result out;
    odin3_vec map; /* uint32_t per old object: new object number */
} fold;

static odin3_status push_u32(odin3_vec *vec, uint32_t value) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

/* A LUT to append: its fanins (count of them) and its table (words for that count). */
typedef struct lut_in {
    const uint32_t *fanins;
    uint32_t count;
    const uint64_t *table;
} lut_in;

static odin3_status fold_append(fold *fd, const lut_in *lut) {
    odin3_abc_result *out = &fd->out;
    odin3_status st = ODIN3_OK;
    for (uint32_t var = 0; st == ODIN3_OK && var < lut->count; var++) {
        st = push_u32(&out->fanins, lut->fanins[var]);
    }
    st = st == ODIN3_OK ? push_u32(&out->lut_k, lut->count) : st;
    for (uint32_t word = 0; st == ODIN3_OK && word < odin3_lut_words(lut->count); word++) {
        uint64_t *slot = odin3_vec_push(&out->truth);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = lut->table[word];
    }
    st = st == ODIN3_OK ? push_u32(&out->fanin_start, (uint32_t)out->fanins.len) : st;
    st = st == ODIN3_OK ? push_u32(&out->truth_start, (uint32_t)out->truth.len) : st;
    return st;
}

/*
 * Folds LUT number lut of res: reads its fanins through the map, removes the constant ones by
 * cofactoring, and either maps the LUT's object to a constant or appends it.
 */
static odin3_status fold_lut(const odin3_abc_result *res, uint32_t lut, fold *fd) {
    uint32_t inputs = u32_at(&res->lut_k, lut);
    uint32_t obj = odin3_abc_result_first_lut(res) + lut;
    uint32_t fanins[ODIN3_LUT_MAX_WIDTH];
    uint64_t table[ODIN3_LUT_MAX_WORDS];
    uint64_t scratch[ODIN3_LUT_MAX_WORDS];
    const uint32_t *old = (const uint32_t *)res->fanins.data + u32_at(&res->fanin_start, lut);
    const uint64_t *src = (const uint64_t *)res->truth.data + u32_at(&res->truth_start, lut);
    memcpy(table, src, sizeof(uint64_t) * odin3_lut_words(inputs));
    uint32_t kept = 0;
    for (uint32_t var = 0; var < inputs; var++) {
        uint32_t mapped = ((const uint32_t *)fd->map.data)[old[var]];
        if (mapped < FIRST_OBJ) {
            uint32_t vars = inputs - (var - kept); /* the table's variables now */
            cofactor(table, &(cofactor_req){vars, kept, mapped == 1}, scratch);
            memcpy(table, scratch, sizeof(uint64_t) * odin3_lut_words(vars - 1U));
        } else {
            fanins[kept++] = mapped;
        }
    }
    int constant = table_constant(table, kept);
    if (constant >= 0) {
        ((uint32_t *)fd->map.data)[obj] = (uint32_t)constant;
        return ODIN3_OK;
    }
    ((uint32_t *)fd->map.data)[obj] =
        FIRST_OBJ + fd->out.n_cis + fd->out.n_regs_out + (uint32_t)fd->out.lut_k.len;
    return fold_append(fd, &(lut_in){fanins, kept, table});
}

static odin3_status fold_prepare(const odin3_abc_result *res, fold *fd) {
    odin3_abc_result_init(&fd->out);
    odin3_vec_init(&fd->map, sizeof(uint32_t));
    fd->out.n_cis = res->n_cis;
    fd->out.n_regs_out = res->n_regs_out;
    fd->out.n_cos = res->n_cos;
    fd->out.n_obs = res->n_obs;
    uint32_t objects = odin3_abc_result_first_lut(res) + res->n_luts;
    odin3_status st = odin3_vec_reserve(&fd->map, objects);
    for (uint32_t obj = 0; st == ODIN3_OK && obj < objects; obj++) {
        st = push_u32(&fd->map, obj < odin3_abc_result_first_lut(res) ? obj : 0);
    }
    st = st == ODIN3_OK ? push_u32(&fd->out.fanin_start, 0) : st;
    st = st == ODIN3_OK ? push_u32(&fd->out.truth_start, 0) : st;
    return st;
}

/* Copies reg_from and old_lit, and co_driver through the map. */
static odin3_status fold_copy_outputs(const odin3_abc_result *res, fold *fd) {
    odin3_status st = ODIN3_OK;
    const uint32_t *map = fd->map.data;
    for (size_t reg = 0; st == ODIN3_OK && reg < res->reg_from.len; reg++) {
        st = push_u32(&fd->out.reg_from, u32_at(&res->reg_from, reg));
    }
    for (size_t co = 0; st == ODIN3_OK && co < res->co_driver.len; co++) {
        st = push_u32(&fd->out.co_driver, map[u32_at(&res->co_driver, co)]);
    }
    for (size_t obj = 0; st == ODIN3_OK && obj < res->old_lit.len; obj++) {
        st = push_u32(&fd->out.old_lit, u32_at(&res->old_lit, obj));
    }
    return st;
}

odin3_status odin3_abc_result_fold(odin3_abc_result *res) {
    fold fd;
    odin3_status st = fold_prepare(res, &fd);
    for (uint32_t lut = 0; st == ODIN3_OK && lut < res->n_luts; lut++) {
        st = fold_lut(res, lut, &fd);
    }
    st = st == ODIN3_OK ? fold_copy_outputs(res, &fd) : st;
    if (st == ODIN3_OK) {
        fd.out.n_luts = (uint32_t)fd.out.lut_k.len;
        odin3_abc_result_free(res);
        *res = fd.out;
    } else {
        odin3_abc_result_free(&fd.out);
    }
    odin3_vec_free(&fd.map);
    return st;
}
