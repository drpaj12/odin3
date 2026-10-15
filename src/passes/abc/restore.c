/*
 * restore.c — the init-1 restore on the result (3D spec §5.4, AB-13; plan Task 17). A register
 * with INIT 1 went to ABC inverted (it holds ¬Q and reads ¬D, init 0); after the restore the
 * result describes the register that holds the IR value Q from INIT 1, with the same register
 * count. It edits the result as data, before any IR object exists, so the splice never knows.
 */
#include "ir/lut.h"
#include "ir/module.h"
#include "passes/abc/run.h"
#include "passes/abc/splice.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    FIRST_OBJ = 2,         /* objects 0 and 1 are the constants */
    NOT_TABLE = 0x1,       /* a 1-input NOT: bit 0 is 1, bit 1 is 0 */
    IDENTITY_TABLE = 0x2,  /* a 1-input buffer */
    INIT_PARAM = 0,        /* a flip-flop's INIT is its first parameter */
    INIT_ONE = 1
};

/* The scratch of one restore. */
typedef struct rst {
    odin3_abc_run *run;
    odin3_abc_result *res;
    odin3_vec inv;     /* uint8_t per result register: its representative has INIT 1 */
    odin3_vec flipped; /* uint8_t per original LUT: a variable of its table was flipped */
    odin3_vec not_of;  /* uint32_t per original object: the NOT LUT object of it, or 0 */
    odin3_vec fanout;  /* uint32_t per object: readers among LUT fanins and CO drivers */
    uint32_t luts0;    /* the result's LUT count before the restore */
    uint32_t objs0;
} rst;

static uint32_t *u32s(odin3_vec *vec) {
    return vec->data;
}

static bool table_bit(const uint64_t *table, uint32_t idx) {
    return ((table[idx / ODIN3_LUT_WORD_BITS] >> (idx % ODIN3_LUT_WORD_BITS)) & 1U) != 0;
}

static void table_set(uint64_t *table, uint32_t idx, bool val) {
    uint64_t mask = 1ULL << (idx % ODIN3_LUT_WORD_BITS);
    if (val) {
        table[idx / ODIN3_LUT_WORD_BITS] |= mask;
    } else {
        table[idx / ODIN3_LUT_WORD_BITS] &= ~mask;
    }
}

/* The table of a k-input LUT with variable var inverted: f'(x) = f(x with bit var flipped). */
static void table_flip_var(uint64_t *table, uint32_t inputs, uint32_t var) {
    uint32_t bit = 1U << var;
    for (uint32_t idx = 0; idx < (1U << inputs); idx++) {
        if ((idx & bit) == 0) {
            bool low = table_bit(table, idx);
            table_set(table, idx, table_bit(table, idx | bit));
            table_set(table, idx | bit, low);
        }
    }
}

static void table_complement(uint64_t *table, uint32_t inputs) {
    for (uint32_t idx = 0; idx < (1U << inputs); idx++) {
        table_set(table, idx, !table_bit(table, idx));
    }
}

static uint32_t reg_base(const odin3_abc_result *res) {
    return FIRST_OBJ + res->n_cis;
}

/* True when obj is a result register object whose representative has INIT 1. */
static bool is_inv_reg(const rst *rs, uint32_t obj) {
    uint32_t base = reg_base(rs->res);
    return obj >= base && obj < base + rs->res->n_regs_out &&
           ((const uint8_t *)rs->inv.data)[obj - base] != 0;
}

static uint32_t lut_obj(const odin3_abc_result *res, uint32_t lut) {
    return odin3_abc_result_first_lut(res) + lut;
}

static uint32_t *lut_fanins(odin3_abc_result *res, uint32_t lut) {
    return u32s(&res->fanins) + u32s(&res->fanin_start)[lut];
}

static uint64_t *lut_words(odin3_abc_result *res, uint32_t lut) {
    return (uint64_t *)res->truth.data + u32s(&res->truth_start)[lut];
}

/* --- setup --------------------------------------------------------------------------------- */

static odin3_status vec_zeros(odin3_vec *vec, size_t elem, size_t count) {
    odin3_vec_init(vec, elem);
    odin3_status st = odin3_vec_reserve(vec, count == 0 ? 1 : count);
    if (st == ODIN3_OK) {
        memset(vec->data, 0, elem * (count == 0 ? 1 : count));
        vec->len = count;
    }
    return st;
}

static void rst_free(rst *rs) {
    odin3_vec_free(&rs->inv);
    odin3_vec_free(&rs->flipped);
    odin3_vec_free(&rs->not_of);
    odin3_vec_free(&rs->fanout);
}

/* Marks the registers whose representative has INIT 1; *any says whether there is one. */
static void mark_inverted(rst *rs, bool *any) {
    const odin3_abc_run *run = rs->run;
    for (uint32_t reg = 0; reg < rs->res->n_regs_out; reg++) {
        uint32_t from = u32s(&rs->res->reg_from)[reg];
        odin3_node_id node = {((const uint32_t *)run->region->regs.data)[from]};
        const odin3_value *init = odin3_node_param(run->module, node, INIT_PARAM);
        bool one = init != NULL && init->kind == ODIN3_VAL_INT && init->i == INIT_ONE;
        ((uint8_t *)rs->inv.data)[reg] = one ? 1U : 0U;
        *any = *any || one;
    }
}

static odin3_status rst_init(rst *rs, odin3_abc_run *run, odin3_abc_result *res, bool *any) {
    *rs = (rst){0};
    rs->run = run;
    rs->res = res;
    rs->luts0 = res->n_luts;
    rs->objs0 = odin3_abc_result_first_lut(res) + res->n_luts;
    odin3_status st = vec_zeros(&rs->inv, 1, res->n_regs_out);
    st = st == ODIN3_OK ? vec_zeros(&rs->flipped, 1, res->n_luts) : st;
    st = st == ODIN3_OK ? vec_zeros(&rs->not_of, sizeof(uint32_t), rs->objs0) : st;
    st = st == ODIN3_OK ? vec_zeros(&rs->fanout, sizeof(uint32_t), rs->objs0) : st;
    if (st == ODIN3_OK) {
        mark_inverted(rs, any);
    }
    return st;
}

/* --- editing ------------------------------------------------------------------------------- */

/* Flips, in every LUT, the variable of each restored register it reads. */
static void flip_readers(rst *rs) {
    for (uint32_t lut = 0; lut < rs->luts0; lut++) {
        uint32_t inputs = u32s(&rs->res->lut_k)[lut];
        const uint32_t *fanins = lut_fanins(rs->res, lut);
        for (uint32_t var = 0; var < inputs; var++) {
            if (is_inv_reg(rs, fanins[var])) {
                table_flip_var(lut_words(rs->res, lut), inputs, var);
                ((uint8_t *)rs->flipped.data)[lut] = 1;
            }
        }
    }
}

/* Appends a NOT LUT of src (once per source) and returns its object in *out. NO_MEMORY leaves res
 * as it was. */
static odin3_status append_not(rst *rs, uint32_t src, uint32_t *out) {
    uint32_t known = u32s(&rs->not_of)[src];
    if (known != 0) {
        *out = known;
        return ODIN3_OK;
    }
    odin3_abc_result *res = rs->res;
    odin3_status st = odin3_vec_reserve(&res->fanins, res->fanins.len + 1);
    st = st == ODIN3_OK ? odin3_vec_reserve(&res->lut_k, res->lut_k.len + 1) : st;
    st = st == ODIN3_OK ? odin3_vec_reserve(&res->truth, res->truth.len + 1) : st;
    st = st == ODIN3_OK ? odin3_vec_reserve(&res->fanin_start, res->fanin_start.len + 1) : st;
    st = st == ODIN3_OK ? odin3_vec_reserve(&res->truth_start, res->truth_start.len + 1) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    *(uint32_t *)odin3_vec_push(&res->fanins) = src;
    *(uint32_t *)odin3_vec_push(&res->lut_k) = 1;
    *(uint64_t *)odin3_vec_push(&res->truth) = NOT_TABLE;
    *(uint32_t *)odin3_vec_push(&res->fanin_start) = (uint32_t)res->fanins.len;
    *(uint32_t *)odin3_vec_push(&res->truth_start) = (uint32_t)res->truth.len;
    res->n_luts++;
    *out = lut_obj(res, res->n_luts - 1U);
    u32s(&rs->not_of)[src] = *out;
    return ODIN3_OK;
}

/* A real CO that reads a restored register directly is its inverse: a NOT LUT. */
static odin3_status direct_readers(rst *rs) {
    for (uint32_t co = 0; co < rs->res->n_cos; co++) {
        uint32_t drv = u32s(&rs->res->co_driver)[co];
        if (is_inv_reg(rs, drv)) {
            uint32_t not_obj = 0;
            odin3_status st = append_not(rs, drv, &not_obj);
            if (st != ODIN3_OK) {
                return st;
            }
            u32s(&rs->res->co_driver)[co] = not_obj;
        }
    }
    return ODIN3_OK;
}

/* A flipped one-input identity LUT of a restored register (formerly its inverter): a real CO
 * that reads it reads the register instead, so the CO becomes a merge. */
static void cancel_inverters(rst *rs) {
    for (uint32_t lut = 0; lut < rs->luts0; lut++) {
        const uint32_t *fanins = lut_fanins(rs->res, lut);
        if (((const uint8_t *)rs->flipped.data)[lut] == 0 || u32s(&rs->res->lut_k)[lut] != 1 ||
            lut_words(rs->res, lut)[0] != IDENTITY_TABLE || !is_inv_reg(rs, fanins[0])) {
            continue;
        }
        for (uint32_t co = 0; co < rs->res->n_cos; co++) {
            if (u32s(&rs->res->co_driver)[co] == lut_obj(rs->res, lut)) {
                u32s(&rs->res->co_driver)[co] = fanins[0];
            }
        }
    }
}

static odin3_status count_fanout(rst *rs) {
    odin3_abc_result *res = rs->res;
    odin3_vec_free(&rs->fanout);
    odin3_status st =
        vec_zeros(&rs->fanout, sizeof(uint32_t), odin3_abc_result_first_lut(res) + res->n_luts);
    for (size_t at = 0; st == ODIN3_OK && at < res->fanins.len; at++) {
        u32s(&rs->fanout)[u32s(&res->fanins)[at]]++;
    }
    for (size_t at = 0; st == ODIN3_OK && at < res->co_driver.len; at++) {
        u32s(&rs->fanout)[u32s(&res->co_driver)[at]]++;
    }
    return st;
}

/* The register input of restored register reg: D' = ¬(the ABC value of its source). */
static odin3_status restore_input(rst *rs, uint32_t reg) {
    odin3_abc_result *res = rs->res;
    uint32_t slot = res->n_cos + res->n_obs + reg;
    uint32_t src = u32s(&res->co_driver)[slot];
    if (src < FIRST_OBJ) {
        u32s(&res->co_driver)[slot] = 1U - src;
    } else if (is_inv_reg(rs, src)) {
        return ODIN3_OK; /* ¬(¬Q2) is the restored register itself */
    } else if (src >= odin3_abc_result_first_lut(res) && u32s(&rs->fanout)[src] == 1 &&
               src - odin3_abc_result_first_lut(res) < rs->luts0) {
        uint32_t lut = src - odin3_abc_result_first_lut(res);
        table_complement(lut_words(res, lut), u32s(&res->lut_k)[lut]);
    } else {
        uint32_t not_obj = 0;
        odin3_status st = append_not(rs, src, &not_obj);
        if (st != ODIN3_OK) {
            return st;
        }
        u32s(&res->co_driver)[slot] = not_obj;
    }
    return ODIN3_OK;
}

static odin3_status restore_inputs(rst *rs) {
    for (uint32_t reg = 0; reg < rs->res->n_regs_out; reg++) {
        if (((const uint8_t *)rs->inv.data)[reg] != 0) {
            odin3_status st = restore_input(rs, reg);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

odin3_status odin3_abc_restore(odin3_abc_run *run, odin3_abc_result *res) {
    rst rs;
    bool any = false;
    odin3_status st = rst_init(&rs, run, res, &any);
    if (st == ODIN3_OK && any) {
        flip_readers(&rs);
        st = direct_readers(&rs);
        if (st == ODIN3_OK) {
            cancel_inverters(&rs);
            st = count_fanout(&rs);
            st = st == ODIN3_OK ? restore_inputs(&rs) : st;
        }
    }
    rst_free(&rs);
    return st;
}
