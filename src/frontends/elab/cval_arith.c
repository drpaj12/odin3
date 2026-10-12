/*
 * cval_arith.c — the operators on 4-state constants (cval.h, spec §4.3, EL-14): Yosys 0.55's
 * RTLIL::const_* (kernel/calc.cc) as its constant folding calls them (frontends/ast/simplify.cc).
 *
 * Bitwise operators and reductions work word-parallel on the planes; arithmetic runs on the
 * value plane through the 1E word helpers (sim/word.h: one machine operation per word up to 64
 * bits, schoolbook loops above), and division above 64 bits is Knuth's algorithm D on 32-bit
 * digits. No function recurses or allocates outside the caller's arena.
 */
#include "frontends/elab/cval.h"

#include "frontends/elab/cval_internal.h"
#include "frontends/elab/elab_test.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "sim/word.h"
#include "util/arena.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert((uint32_t)ODIN3_CVAL_WORD_BITS == (uint32_t)ODIN3_WORD_LIMB_BITS,
               "a value plane is a 1E word");

enum {
    WORD_BITS = ODIN3_CVAL_WORD_BITS,
    DIGIT_BITS = 32,
    DIGITS_PER_WORD = 2,
    SMALL_POW_WIDTH = 2, /* odd bases: the exponent matters modulo 2^(width - 2) above this */
};

static const uint64_t k_digit_mask = 0xFFFFFFFFU;

typedef struct op_ctx {
    odin3_arena *arena;
    const odin3_cop_args *args;
} op_ctx;

typedef odin3_status (*op_fn)(const op_ctx *ctx, odin3_cval *out);

/* --- small helpers ------------------------------------------------------------------------- */

static uint8_t sign_flag(bool is_signed) {
    return is_signed ? ODIN3_CVAL_SIGNED : 0;
}

static bool top_is_one(const odin3_cval *val) {
    return odin3_cval_bit(val, val->width - 1) == ODIN3_BIT_1;
}

static odin3_bit invert(odin3_bit bit) {
    if (bit == ODIN3_BIT_0 || bit == ODIN3_BIT_1) {
        return bit == ODIN3_BIT_0 ? ODIN3_BIT_1 : ODIN3_BIT_0;
    }
    return ODIN3_BIT_X;
}

/* A value of the context width whose every bit is `fill`, signed by the context. */
static odin3_status filled(const op_ctx *ctx, odin3_bit fill, odin3_cval *out) {
    const odin3_cval_spec spec = {ctx->args->width, sign_flag(ctx->args->is_signed), fill};
    return odin3_cval_new(ctx->arena, &spec, out);
}

/* One result bit, zero-extended to the context width, unsigned (comparisons, reductions). */
static odin3_status one_bit(const op_ctx *ctx, odin3_bit bit, odin3_cval *out) {
    const odin3_cval_spec spec = {ctx->args->width, 0, ODIN3_BIT_0};
    odin3_cval res;
    odin3_status st = odin3_cval_new(ctx->arena, &spec, &res);
    if (st != ODIN3_OK) {
        return st;
    }
    res.aval[0] |= bit == ODIN3_BIT_1 || bit == ODIN3_BIT_X ? 1U : 0U;
    res.bval[0] |= bit == ODIN3_BIT_X || bit == ODIN3_BIT_Z ? 1U : 0U;
    *out = res;
    return ODIN3_OK;
}

/* val at the context width and sign (Yosys bitsAsConst(width_hint, sign_hint)). */
static odin3_status extend(const op_ctx *ctx, const odin3_cval *val, odin3_cval *out) {
    return odin3_cval_resize(ctx->arena, val, ctx->args->width, ctx->args->is_signed, out);
}

/* Both operands at the context width and sign. */
static odin3_status extend_both(const op_ctx *ctx, odin3_cval *lhs, odin3_cval *rhs) {
    odin3_status st = extend(ctx, ctx->args->a, lhs);
    return st == ODIN3_OK ? extend(ctx, ctx->args->b, rhs) : st;
}

static uint64_t *scratch_words(const op_ctx *ctx, size_t words) {
    return odin3_arena_alloc(ctx->arena, words * sizeof(uint64_t));
}

/* --- bitwise (word-parallel IEEE 4-state tables; z reads as x) ------------------------------ */

typedef struct word4 {
    uint64_t aval;
    uint64_t bval;
} word4;

static uint64_t known0(word4 word) {
    return ~word.aval & ~word.bval;
}

static uint64_t known1(word4 word) {
    return word.aval & ~word.bval;
}

/* The word whose bits are x where `unknown` is set, else 1 where `one` is set, else 0. */
static word4 make4(uint64_t one, uint64_t unknown) {
    return (word4){(one & ~unknown) | unknown, unknown};
}

static word4 logic4(odin3_cop op, word4 lhs, word4 rhs) {
    uint64_t unknown = lhs.bval | rhs.bval;
    switch (op) {
    case ODIN3_COP_AND: {
        uint64_t zero = known0(lhs) | known0(rhs);
        uint64_t one = known1(lhs) & known1(rhs);
        return make4(one, ~(zero | one));
    }
    case ODIN3_COP_OR: {
        uint64_t one = known1(lhs) | known1(rhs);
        uint64_t zero = known0(lhs) & known0(rhs);
        return make4(one, ~(zero | one));
    }
    case ODIN3_COP_XOR:
        return make4(lhs.aval ^ rhs.aval, unknown);
    case ODIN3_COP_XNOR:
        return make4(~(lhs.aval ^ rhs.aval), unknown);
    default: /* ODIN3_COP_NOT */
        return make4(known0(lhs), lhs.bval);
    }
}

static odin3_status op_bitwise(const op_ctx *ctx, odin3_cval *out) {
    odin3_cval lhs;
    odin3_cval rhs;
    bool unary = ctx->args->op == ODIN3_COP_NOT;
    odin3_status st = unary ? extend(ctx, ctx->args->a, &lhs) : extend_both(ctx, &lhs, &rhs);
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t words = odin3_cval_words(lhs.width);
    for (uint32_t i = 0; i < words; i++) {
        word4 other = unary ? (word4){0, 0} : (word4){rhs.aval[i], rhs.bval[i]};
        word4 res = logic4(ctx->args->op, (word4){lhs.aval[i], lhs.bval[i]}, other);
        lhs.aval[i] = res.aval;
        lhs.bval[i] = res.bval;
    }
    cval_mask_top(&lhs); /* ~ and ~^ set the bits above the width */
    *out = lhs;
    return ODIN3_OK;
}

static odin3_status op_pos(const op_ctx *ctx, odin3_cval *out) {
    return extend(ctx, ctx->args->a, out);
}

/* --- + - * and unary - --------------------------------------------------------------------- */

static odin3_status op_arith(const op_ctx *ctx, odin3_cval *out) {
    odin3_cval lhs;
    odin3_cval rhs;
    bool unary = ctx->args->op == ODIN3_COP_NEG;
    odin3_status st = unary ? extend(ctx, ctx->args->a, &lhs) : extend_both(ctx, &lhs, &rhs);
    if (st != ODIN3_OK) {
        return st;
    }
    if (!odin3_cval_is_known(&lhs) || (!unary && !odin3_cval_is_known(&rhs))) {
        return filled(ctx, ODIN3_BIT_X, out);
    }
    odin3_cval res;
    st = filled(ctx, ODIN3_BIT_0, &res);
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_word_bin bin = {res.aval, lhs.aval, unary ? lhs.aval : rhs.aval, lhs.width};
    if (ctx->args->op == ODIN3_COP_ADD) {
        odin3_word_add(&bin);
    } else if (ctx->args->op == ODIN3_COP_SUB) {
        odin3_word_sub(&bin);
    } else if (ctx->args->op == ODIN3_COP_MUL) {
        odin3_word_mul(&bin);
    } else {
        odin3_word_neg(&bin);
    }
    *out = res;
    return ODIN3_OK;
}

/* --- unsigned division: native up to 64 bits, Knuth's algorithm D above -------------------- */

/* quo = num / den, rem = num % den over `words` words; den is not zero; quo and rem zeroed. */
typedef struct divide {
    const uint64_t *num;
    const uint64_t *den;
    uint64_t *quo;
    uint64_t *rem;
    uint32_t words;
} divide;

/* Algorithm D's state (Knuth TAOCP 4.3.1; Hacker's Delight divmnu) on 32-bit digits. */
typedef struct knuth {
    uint32_t *un; /* the normalized dividend: m + 1 digits */
    uint32_t *vn; /* the normalized divisor: dn digits */
    uint32_t m;   /* significant digits of the dividend */
    uint32_t dn;  /* significant digits of the divisor (at least 2) */
    uint32_t shift;
} knuth;

static uint32_t digit_of(const uint64_t *words, uint32_t idx) {
    return (uint32_t)((words[idx / DIGITS_PER_WORD] >> (DIGIT_BITS * (idx % DIGITS_PER_WORD))) &
                      k_digit_mask);
}

/* Significant 32-bit digits of a word array (0 for zero). */
static uint32_t digit_count(const uint64_t *words, uint32_t n_words) {
    uint32_t k = n_words * DIGITS_PER_WORD;
    while (k > 0 && digit_of(words, k - 1) == 0) {
        k--;
    }
    return k;
}

static void put_digit(uint64_t *words, uint32_t idx, uint32_t digit) {
    words[idx / DIGITS_PER_WORD] |= (uint64_t)digit << (DIGIT_BITS * (idx % DIGITS_PER_WORD));
}

static uint32_t leading_zeros(uint32_t digit) {
    uint32_t count = 0;
    while (count < DIGIT_BITS && (digit & (UINT32_C(1) << (DIGIT_BITS - 1 - count))) == 0) {
        count++;
    }
    return count;
}

/* digits[0..n) of words shifted left by kd->shift into out[0..n], the carry out in out[n]. */
typedef struct shift_in {
    const uint64_t *words;
    uint32_t n;
    uint32_t *out;
} shift_in;

static void normalize(const knuth *kd, const shift_in *in) {
    uint32_t prev = 0;
    for (uint32_t i = 0; i <= in->n; i++) {
        uint32_t cur = i < in->n ? digit_of(in->words, i) : 0;
        uint64_t pair = ((uint64_t)cur << DIGIT_BITS) | prev;
        in->out[i] = (uint32_t)((pair << kd->shift) >> DIGIT_BITS);
        prev = cur;
    }
}

/* One step of the main loop: quotient digit `col` and its estimate q̂. */
typedef struct knuth_step {
    uint32_t col;
    uint64_t qhat;
} knuth_step;

/* The estimate q̂ of quotient digit col, corrected to at most one too large. */
static uint64_t estimate(const knuth *kd, uint32_t col) {
    uint64_t top = kd->vn[kd->dn - 1];
    uint64_t num = ((uint64_t)kd->un[col + kd->dn] << DIGIT_BITS) | kd->un[col + kd->dn - 1];
    uint64_t qhat = num / top;
    uint64_t rhat = num % top;
    while (qhat > k_digit_mask ||
           qhat * kd->vn[kd->dn - 2] > ((rhat << DIGIT_BITS) | kd->un[col + kd->dn - 2])) {
        qhat--;
        rhat += top;
        if (rhat > k_digit_mask) {
            break;
        }
    }
    return qhat;
}

/* un[col .. col + dn] -= qhat * vn (qhat < 2^32); true when that went negative. */
static bool multiply_subtract(const knuth *kd, knuth_step step) {
    uint64_t carry = 0;
    uint64_t borrow = 0;
    uint32_t *un = kd->un + step.col;
    for (uint32_t i = 0; i < kd->dn; i++) {
        uint64_t prod = step.qhat * kd->vn[i] + carry;
        carry = prod >> DIGIT_BITS;
        uint64_t diff = (uint64_t)un[i] - (prod & k_digit_mask) - borrow;
        un[i] = (uint32_t)diff;
        borrow = diff >> (WORD_BITS - 1); /* a wrapped difference has its top bit set */
    }
    uint64_t diff = (uint64_t)un[kd->dn] - carry - borrow;
    un[kd->dn] = (uint32_t)diff;
    return (diff >> (WORD_BITS - 1)) != 0;
}

/* un[col .. col + dn] += vn, the carry out of the top digit dropped (it cancels the borrow). */
static void add_back(const knuth *kd, uint32_t col) {
    uint64_t carry = 0;
    uint32_t *un = kd->un + col;
    for (uint32_t i = 0; i < kd->dn; i++) {
        uint64_t sum = (uint64_t)un[i] + kd->vn[i] + carry;
        un[i] = (uint32_t)sum;
        carry = sum >> DIGIT_BITS;
    }
    un[kd->dn] = (uint32_t)(un[kd->dn] + carry);
}

static void knuth_run(const knuth *kd, const divide *div) {
    for (uint32_t col = kd->m - kd->dn + 1; col-- > 0;) {
        knuth_step step = {col, estimate(kd, col)};
        if (multiply_subtract(kd, step)) {
            step.qhat--;
            add_back(kd, col);
        }
        put_digit(div->quo, col, (uint32_t)step.qhat);
    }
    for (uint32_t i = 0; i < kd->dn; i++) { /* the remainder, shifted back */
        uint64_t pair = ((uint64_t)kd->un[i + 1] << DIGIT_BITS) | kd->un[i];
        put_digit(div->rem, i, (uint32_t)(pair >> kd->shift));
    }
}

/* Division by a single 32-bit digit. */
static void short_divide(const divide *div, uint32_t digits) {
    uint64_t den = digit_of(div->den, 0);
    uint64_t rem = 0;
    for (uint32_t j = digits; j-- > 0;) {
        uint64_t cur = (rem << DIGIT_BITS) | digit_of(div->num, j);
        put_digit(div->quo, j, (uint32_t)(cur / den));
        rem = cur % den;
    }
    put_digit(div->rem, 0, (uint32_t)rem);
}

static odin3_status long_divide(const op_ctx *ctx, const divide *div) {
    knuth kd = {.m = digit_count(div->num, div->words), .dn = digit_count(div->den, div->words)};
    if (kd.m < kd.dn) {
        memcpy(div->rem, div->num, (size_t)div->words * sizeof *div->rem);
        return ODIN3_OK;
    }
    if (kd.dn == 1) {
        short_divide(div, kd.m);
        return ODIN3_OK;
    }
    uint32_t *digits = odin3_arena_alloc(ctx->arena, (2 * (size_t)kd.m + 2) * sizeof *digits);
    if (digits == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    kd.un = digits;
    kd.vn = digits + kd.m + 1;
    kd.shift = leading_zeros(digit_of(div->den, kd.dn - 1));
    const shift_in num = {div->num, kd.m, kd.un};
    normalize(&kd, &num);
    const shift_in den = {div->den, kd.dn, kd.vn}; /* its carry digit is 0 */
    normalize(&kd, &den);
    knuth_run(&kd, div);
    return ODIN3_OK;
}

static odin3_status udivide(const op_ctx *ctx, const divide *div) {
    if (div->words == 1) {
        div->quo[0] = div->num[0] / div->den[0];
        div->rem[0] = div->num[0] % div->den[0];
        return ODIN3_OK;
    }
    return long_divide(ctx, div);
}

/* --- / and % ------------------------------------------------------------------------------- */

static void negate(uint64_t *words, uint32_t width) {
    const odin3_word_bin bin = {words, words, words, width};
    odin3_word_neg(&bin);
}

/* Truncating signed division: magnitudes divided; the quotient's sign from both operands, the
 * remainder's from the dividend (const_div, const_mod). */
static odin3_status divide_known(const op_ctx *ctx, odin3_cval *lhs, odin3_cval *rhs,
                                 odin3_cval *out) {
    bool neg_a = ctx->args->is_signed && top_is_one(lhs);
    bool neg_b = ctx->args->is_signed && top_is_one(rhs);
    if (neg_a) {
        negate(lhs->aval, lhs->width);
    }
    if (neg_b) {
        negate(rhs->aval, rhs->width);
    }
    odin3_cval res;
    odin3_status st = filled(ctx, ODIN3_BIT_0, &res);
    uint32_t words = odin3_cval_words(lhs->width);
    uint64_t *other = st == ODIN3_OK ? scratch_words(ctx, words) : NULL;
    if (other == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    bool is_div = ctx->args->op == ODIN3_COP_DIV;
    const divide div = {lhs->aval, rhs->aval, is_div ? res.aval : other, is_div ? other : res.aval,
                        words};
    st = udivide(ctx, &div);
    if (st != ODIN3_OK) {
        return st;
    }
    if (is_div ? neg_a != neg_b : neg_a) {
        negate(res.aval, res.width);
    }
    *out = res;
    return ODIN3_OK;
}

static odin3_status op_divmod(const op_ctx *ctx, odin3_cval *out) {
    odin3_cval lhs;
    odin3_cval rhs;
    odin3_status st = extend_both(ctx, &lhs, &rhs);
    if (st != ODIN3_OK) {
        return st;
    }
    if (!odin3_cval_is_known(&lhs) || !odin3_cval_is_known(&rhs) ||
        odin3_word_is_zero(rhs.aval, rhs.width)) {
        return filled(ctx, ODIN3_BIT_X, out);
    }
    return divide_known(ctx, &lhs, &rhs, out);
}

/* --- ** ------------------------------------------------------------------------------------ */

/* No bit is a known 1: zero as const2big reads it (x and z bits read as 0). */
static bool reads_zero(const odin3_cval *val) {
    for (uint32_t i = 0; i < odin3_cval_words(val->width); i++) {
        if ((val->aval[i] & ~val->bval[i]) != 0) {
            return false;
        }
    }
    return true;
}

/* Highest set bit + 1 of a known value (0 for zero). */
static uint32_t bit_length(const odin3_cval *val) {
    uint32_t pos = val->width;
    while (pos > 0 && odin3_cval_bit(val, pos - 1) != ODIN3_BIT_1) {
        pos--;
    }
    return pos;
}

static bool is_one(const odin3_cval *val) {
    return val->aval[0] == 1 && bit_length(val) == 1;
}

static bool all_ones(const odin3_cval *val) {
    for (uint32_t pos = 0; pos < val->width; pos++) {
        if (odin3_cval_bit(val, pos) != ODIN3_BIT_1) {
            return false;
        }
    }
    return true;
}

/* A negative exponent (const_pow): 1 ** b = 1, (-1) ** b = +-1 by parity, else 0. */
static odin3_status negative_power(const op_ctx *ctx, const odin3_cval *lhs, odin3_cval *out) {
    if (ctx->args->is_signed && all_ones(lhs)) {
        bool odd = (ctx->args->b->aval[0] & 1U) != 0;
        return odd ? filled(ctx, ODIN3_BIT_1, out) : one_bit(ctx, ODIN3_BIT_1, out);
    }
    if (is_one(lhs)) {
        return one_bit(ctx, ODIN3_BIT_1, out);
    }
    return filled(ctx, ODIN3_BIT_0, out);
}

/* Square-and-multiply modulo 2^width over the exponent's low `steps` bits. */
typedef struct power {
    uint64_t *acc;  /* the result so far */
    uint64_t *base; /* a^(2^k) */
    uint64_t *tmp;
    const odin3_cval *exp;
    uint32_t width;
    uint32_t steps;
} power;

static bool any_bit_from(const odin3_cval *val, uint32_t from) {
    for (uint32_t pos = from; pos < val->width; pos++) {
        if (odin3_cval_bit(val, pos) == ODIN3_BIT_1) {
            return true;
        }
    }
    return false;
}

static void power_run(const power *pw) {
    size_t bytes = (size_t)odin3_word_limbs(pw->width) * sizeof *pw->acc;
    for (uint32_t k = 0; k < pw->steps; k++) {
        if (odin3_cval_bit(pw->exp, k) == ODIN3_BIT_1) {
            const odin3_word_bin mul = {pw->tmp, pw->acc, pw->base, pw->width};
            odin3_word_mul(&mul);
            memcpy(pw->acc, pw->tmp, bytes);
        }
        if (k + 1 == pw->steps) {
            break;
        }
        const odin3_word_bin square = {pw->tmp, pw->base, pw->base, pw->width};
        odin3_word_mul(&square);
        memcpy(pw->base, pw->tmp, bytes);
        if (odin3_word_is_zero(pw->base, pw->width)) { /* an even base: a^(2^k) is 0 from here */
            if (any_bit_from(pw->exp, k + 1)) {
                memset(pw->acc, 0, bytes);
            }
            break;
        }
    }
}

/* a ** b for a known base and a known positive exponent. */
static odin3_status positive_power(const op_ctx *ctx, const odin3_cval *lhs, odin3_cval *out) {
    odin3_cval res;
    odin3_status st = one_bit(ctx, ODIN3_BIT_1, &res);
    uint32_t words = odin3_cval_words(lhs->width);
    uint64_t *scratch = st == ODIN3_OK ? scratch_words(ctx, 2 * (size_t)words) : NULL;
    if (scratch == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(scratch, lhs->aval, (size_t)words * sizeof *scratch);
    power pw = {res.aval,     scratch,    scratch + words,
                ctx->args->b, lhs->width, bit_length(ctx->args->b)};
    bool odd = (lhs->aval[0] & 1U) != 0;
    uint32_t order = lhs->width <= SMALL_POW_WIDTH ? 1 : lhs->width - SMALL_POW_WIDTH;
    if (odd && pw.steps > order) { /* an odd base's powers repeat with period 2^(width - 2) */
        pw.steps = order;
    }
    power_run(&pw);
    res.flags = sign_flag(ctx->args->is_signed);
    *out = res;
    return ODIN3_OK;
}

static odin3_status op_pow(const op_ctx *ctx, odin3_cval *out) {
    odin3_cval lhs;
    odin3_status st = extend(ctx, ctx->args->a, &lhs);
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_cval *rhs = ctx->args->b;
    bool b_neg = (rhs->flags & ODIN3_CVAL_SIGNED) != 0 && top_is_one(rhs);
    bool b_pos = !b_neg && !reads_zero(rhs);
    if (reads_zero(&lhs) && (b_neg || b_pos)) { /* before the x check, as const_pow does */
        return filled(ctx, b_neg ? ODIN3_BIT_X : ODIN3_BIT_0, out);
    }
    if (!odin3_cval_is_known(&lhs) || !odin3_cval_is_known(rhs)) {
        return filled(ctx, ODIN3_BIT_X, out);
    }
    odin3_cval res;
    if (b_neg) {
        st = negative_power(ctx, &lhs, &res);
    } else {
        st = b_pos ? positive_power(ctx, &lhs, &res) : one_bit(ctx, ODIN3_BIT_1, &res);
    }
    if (st == ODIN3_OK) {
        res.flags = sign_flag(ctx->args->is_signed);
        *out = res;
    }
    return st;
}

/* --- shifts -------------------------------------------------------------------------------- */

static odin3_status op_shift(const op_ctx *ctx, odin3_cval *out) {
    odin3_cval lhs;
    odin3_status st = extend(ctx, ctx->args->a, &lhs);
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_cval *rhs = ctx->args->b;
    if (!odin3_cval_is_known(rhs)) {
        return filled(ctx, ODIN3_BIT_X, out);
    }
    uint64_t amount = odin3_word_amount(rhs->aval, rhs->width);
    bool left = ctx->args->op == ODIN3_COP_SHL || ctx->args->op == ODIN3_COP_SSHL;
    odin3_bit fill = ctx->args->op == ODIN3_COP_SSHR && ctx->args->is_signed
                         ? odin3_cval_bit(&lhs, lhs.width - 1)
                         : ODIN3_BIT_0;
    const odin3_word_bin plane_a = {lhs.aval, lhs.aval, lhs.aval, lhs.width};
    const odin3_word_bin plane_b = {lhs.bval, lhs.bval, lhs.bval, lhs.width};
    if (left) {
        odin3_word_shl(&plane_a, amount);
        odin3_word_shl(&plane_b, amount);
    } else {
        odin3_word_shr(&plane_a, amount);
        odin3_word_shr(&plane_b, amount);
    }
    if (fill != ODIN3_BIT_0 && amount > 0) {
        cval_fill_from(&lhs, amount >= lhs.width ? 0 : lhs.width - (uint32_t)amount, fill);
    }
    *out = lhs;
    return ODIN3_OK;
}

/* --- comparisons --------------------------------------------------------------------------- */

/* a and b at their common width, signed only when both are (Yosys's cmp_width, cmp_signed). */
static odin3_status common(const op_ctx *ctx, odin3_cval *lhs, odin3_cval *rhs) {
    const odin3_cval *left = ctx->args->a;
    const odin3_cval *right = ctx->args->b;
    uint32_t width = left->width > right->width ? left->width : right->width;
    bool is_signed = (left->flags & right->flags & ODIN3_CVAL_SIGNED) != 0;
    odin3_status st = odin3_cval_resize(ctx->arena, left, width, is_signed, lhs);
    return st == ODIN3_OK ? odin3_cval_resize(ctx->arena, right, width, is_signed, rhs) : st;
}

/* const_eqx: the same four-state bits (two values of one width; flags are not compared). */
static bool same_bits(const odin3_cval *lhs, const odin3_cval *rhs) {
    size_t bytes = (size_t)odin3_cval_words(lhs->width) * sizeof *lhs->aval;
    return memcmp(lhs->aval, rhs->aval, bytes) == 0 && memcmp(lhs->bval, rhs->bval, bytes) == 0;
}

/* const_eq: 0 on any 0/1 mismatch, else x when a bit is x/z, else 1. */
static odin3_bit equal_bit(const odin3_cval *lhs, const odin3_cval *rhs) {
    bool unknown = false;
    for (uint32_t i = 0; i < odin3_cval_words(lhs->width); i++) {
        word4 left = {lhs->aval[i], lhs->bval[i]};
        word4 right = {rhs->aval[i], rhs->bval[i]};
        if (((known0(left) & known1(right)) | (known1(left) & known0(right))) != 0) {
            return ODIN3_BIT_0;
        }
        unknown = unknown || (left.bval | right.bval) != 0;
    }
    return unknown ? ODIN3_BIT_X : ODIN3_BIT_1;
}

/* < <= > >= of two known values of one width and sign. */
static odin3_bit order_bit(odin3_cop op, const odin3_word_bin *bin, bool is_signed) {
    int cmp = is_signed ? odin3_word_scmp(bin) : odin3_word_ucmp(bin);
    switch (op) {
    case ODIN3_COP_LT:
        return cmp < 0 ? ODIN3_BIT_1 : ODIN3_BIT_0;
    case ODIN3_COP_LE:
        return cmp <= 0 ? ODIN3_BIT_1 : ODIN3_BIT_0;
    case ODIN3_COP_GT:
        return cmp > 0 ? ODIN3_BIT_1 : ODIN3_BIT_0;
    default: /* ODIN3_COP_GE */
        return cmp >= 0 ? ODIN3_BIT_1 : ODIN3_BIT_0;
    }
}

static odin3_bit compare_bit(odin3_cop op, const odin3_cval *lhs, const odin3_cval *rhs) {
    switch (op) {
    case ODIN3_COP_EQ:
        return equal_bit(lhs, rhs);
    case ODIN3_COP_NE:
        return invert(equal_bit(lhs, rhs));
    case ODIN3_COP_EQX:
    case ODIN3_COP_NEX: {
        bool same = same_bits(lhs, rhs);
        return same == (op == ODIN3_COP_EQX) ? ODIN3_BIT_1 : ODIN3_BIT_0;
    }
    default:
        break;
    }
    if (!odin3_cval_is_known(lhs) || !odin3_cval_is_known(rhs)) {
        return ODIN3_BIT_X;
    }
    const odin3_word_bin bin = {NULL, lhs->aval, rhs->aval, lhs->width};
    bool is_signed = (lhs->flags & ODIN3_CVAL_SIGNED) != 0;
    return order_bit(op, &bin, is_signed);
}

static odin3_status op_compare(const op_ctx *ctx, odin3_cval *out) {
    odin3_cval lhs;
    odin3_cval rhs;
    odin3_status st = common(ctx, &lhs, &rhs);
    if (st != ODIN3_OK) {
        return st;
    }
    return one_bit(ctx, compare_bit(ctx->args->op, &lhs, &rhs), out);
}

/* --- logical operators and reductions ------------------------------------------------------ */

static odin3_status op_logic(const op_ctx *ctx, odin3_cval *out) {
    odin3_bit lhs = odin3_cval_truth(ctx->args->a);
    if (ctx->args->op == ODIN3_COP_LNOT) {
        return one_bit(ctx, invert(lhs), out);
    }
    odin3_bit rhs = odin3_cval_truth(ctx->args->b);
    odin3_bit decided = ctx->args->op == ODIN3_COP_LAND ? ODIN3_BIT_0 : ODIN3_BIT_1;
    odin3_bit res = ODIN3_BIT_X;
    if (lhs == decided || rhs == decided) {
        res = decided;
    } else if (lhs != ODIN3_BIT_X && rhs != ODIN3_BIT_X) {
        res = invert(decided);
    }
    return one_bit(ctx, res, out);
}

/* &a: 0 when a bit is 0, else x when a bit is x/z, else 1. */
static odin3_bit reduce_and(const odin3_cval *val) {
    uint32_t words = odin3_cval_words(val->width);
    bool unknown = false;
    for (uint32_t i = 0; i < words; i++) {
        uint64_t used = i + 1 == words ? cval_top_mask(val->width) : UINT64_MAX;
        if ((~val->aval[i] & ~val->bval[i] & used) != 0) {
            return ODIN3_BIT_0;
        }
        unknown = unknown || val->bval[i] != 0;
    }
    return unknown ? ODIN3_BIT_X : ODIN3_BIT_1;
}

static odin3_bit reduce_xor(const odin3_cval *val) {
    if (!odin3_cval_is_known(val)) {
        return ODIN3_BIT_X;
    }
    uint64_t fold = 0;
    for (uint32_t i = 0; i < odin3_cval_words(val->width); i++) {
        fold ^= val->aval[i];
    }
    return __builtin_parityll(fold) != 0 ? ODIN3_BIT_1 : ODIN3_BIT_0;
}

static odin3_status op_reduce(const op_ctx *ctx, odin3_cval *out) {
    const odin3_cval *lhs = ctx->args->a;
    odin3_bit res;
    switch (ctx->args->op) {
    case ODIN3_COP_RAND:
    case ODIN3_COP_RNAND:
        res = reduce_and(lhs);
        break;
    case ODIN3_COP_ROR:
    case ODIN3_COP_RNOR:
        res = odin3_cval_truth(lhs);
        break;
    default:
        res = reduce_xor(lhs);
        break;
    }
    bool inverted = ctx->args->op == ODIN3_COP_RNAND || ctx->args->op == ODIN3_COP_RNOR ||
                    ctx->args->op == ODIN3_COP_RXNOR;
    return one_bit(ctx, inverted ? invert(res) : res, out);
}

/* --- dispatch ------------------------------------------------------------------------------ */

static const op_fn k_ops[ODIN3_COP_COUNT] = {
    [ODIN3_COP_ADD] = op_arith,    [ODIN3_COP_SUB] = op_arith,   [ODIN3_COP_MUL] = op_arith,
    [ODIN3_COP_DIV] = op_divmod,   [ODIN3_COP_MOD] = op_divmod,  [ODIN3_COP_POW] = op_pow,
    [ODIN3_COP_AND] = op_bitwise,  [ODIN3_COP_OR] = op_bitwise,  [ODIN3_COP_XOR] = op_bitwise,
    [ODIN3_COP_XNOR] = op_bitwise, [ODIN3_COP_SHL] = op_shift,   [ODIN3_COP_SHR] = op_shift,
    [ODIN3_COP_SSHL] = op_shift,   [ODIN3_COP_SSHR] = op_shift,  [ODIN3_COP_EQ] = op_compare,
    [ODIN3_COP_NE] = op_compare,   [ODIN3_COP_EQX] = op_compare, [ODIN3_COP_NEX] = op_compare,
    [ODIN3_COP_LT] = op_compare,   [ODIN3_COP_LE] = op_compare,  [ODIN3_COP_GT] = op_compare,
    [ODIN3_COP_GE] = op_compare,   [ODIN3_COP_LAND] = op_logic,  [ODIN3_COP_LOR] = op_logic,
    [ODIN3_COP_NEG] = op_arith,    [ODIN3_COP_POS] = op_pos,     [ODIN3_COP_NOT] = op_bitwise,
    [ODIN3_COP_LNOT] = op_logic,   [ODIN3_COP_RAND] = op_reduce, [ODIN3_COP_RNAND] = op_reduce,
    [ODIN3_COP_ROR] = op_reduce,   [ODIN3_COP_RNOR] = op_reduce, [ODIN3_COP_RXOR] = op_reduce,
    [ODIN3_COP_RXNOR] = op_reduce,
};

static bool operand_ok(const odin3_cval *val) {
    return val != NULL && val->width >= 1 && val->aval != NULL && val->bval != NULL;
}

static const char *args_problem(const odin3_cop_args *args) {
    if ((unsigned)args->op >= (unsigned)ODIN3_COP_COUNT) {
        return "unknown operator";
    }
    bool unary = args->op >= ODIN3_COP_NEG; /* NEG … RXNOR are the unary operators */
    if (!operand_ok(args->a) || (!unary && !operand_ok(args->b))) {
        return "missing operand";
    }
    if (args->width == 0 || args->width > odin3_elab_limits_get()->max_width) {
        return "width out of range";
    }
    return NULL;
}

odin3_status odin3_cval_op(odin3_arena *arena, const odin3_cop_args *args, odin3_cval *out) {
    const char *why = arena == NULL || args == NULL || out == NULL ? "NULL argument" : NULL;
    if (why == NULL) {
        why = args_problem(args);
    }
    if (why != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_cval_op: %s", why);
        return ODIN3_ERR_INVALID_ARG;
    }
    const op_ctx ctx = {arena, args};
    odin3_cval res = {0};
    odin3_status st = k_ops[args->op](&ctx, &res);
    if (st == ODIN3_OK) {
        *out = res;
    }
    return st;
}
