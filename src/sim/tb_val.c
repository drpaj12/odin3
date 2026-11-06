/* tb_val.c — 4-state values of the testbench interpreter (tbv in tb.h; design note
 * docs/specs/2026-11-05-sim-4state-design.md section 2): fills, copies, width changes, bit fields
 * and the digits of based numbers. Every operation touches only the limbs of its width. */
#include "sim/tb_internal.h"

#include <ctype.h>
#include <string.h>

enum {
    LIMB_BITS = ODIN3_WORD_LIMB_BITS,
    HALF_BITS = 32,
    CODE_V = 1, /* the v bit of a code */
    CODE_U = 2, /* the u bit of a code */
    BASE_BIN = 2,
    BASE_OCT = 8,
    BASE_DEC = 10,
    BASE_HEX = 16,
    BITS_OCT = 3,
    BITS_HEX = 4
};

static const uint64_t HALF_MASK = 0xFFFFFFFFU;

static uint32_t limbs_of(uint32_t width) {
    return odin3_word_limbs(width);
}

/* The bits of the top limb of a width-bit value that belong to it. */
static uint64_t top_mask(uint32_t width) {
    uint32_t rem = width % LIMB_BITS;
    return rem == 0 ? UINT64_MAX : (((uint64_t)1 << rem) - 1U);
}

/* A mask of the low count bits (count 0..64). */
static uint64_t low_mask(uint32_t count) {
    return count >= LIMB_BITS ? UINT64_MAX : (((uint64_t)1 << count) - 1U);
}

static void mask_top(tbv *val, uint32_t width) {
    uint32_t top = limbs_of(width) - 1U;
    val->v[top] &= top_mask(width);
    val->u[top] &= top_mask(width);
}

void tbv_zero(tbv *val, uint32_t width) {
    uint32_t count = limbs_of(width);
    for (uint32_t i = 0; i < count; i++) {
        val->v[i] = 0;
        val->u[i] = 0;
    }
}

void tbv_fill(tbv *val, uint32_t width, uint32_t code) {
    uint64_t fill_v = (code & CODE_V) != 0 ? UINT64_MAX : 0;
    uint64_t fill_u = (code & CODE_U) != 0 ? UINT64_MAX : 0;
    uint32_t count = limbs_of(width);
    for (uint32_t i = 0; i < count; i++) {
        val->v[i] = fill_v;
        val->u[i] = fill_u;
    }
    mask_top(val, width);
}

void tbv_set_u64(tbv *val, uint32_t width, uint64_t num) {
    tbv_zero(val, width);
    val->v[0] = num;
    mask_top(val, width);
}

void tbv_set_code(tbv *val, uint32_t width, uint32_t code) {
    tbv_zero(val, width);
    val->v[0] = code & CODE_V;
    val->u[0] = (code & CODE_U) >> 1U;
}

void tbv_copy(tbv *dst, const tbv *src, uint32_t width) {
    size_t bytes = (size_t)limbs_of(width) * sizeof(uint64_t);
    memcpy(dst->v, src->v, bytes);
    memcpy(dst->u, src->u, bytes);
}

bool tbv_known(const tbv *val, uint32_t width) {
    uint32_t count = limbs_of(width);
    for (uint32_t i = 0; i < count; i++) {
        if (val->u[i] != 0) {
            return false;
        }
    }
    return true;
}

bool tbv_same(const tbv *lhs, const tbv *rhs, uint32_t width) {
    uint32_t count = limbs_of(width);
    for (uint32_t i = 0; i < count; i++) {
        if (lhs->v[i] != rhs->v[i] || lhs->u[i] != rhs->u[i]) {
            return false;
        }
    }
    return true;
}

uint32_t tbv_code(const tbv *val, uint32_t pos) {
    return odin3_w4_bit((odin3_w4c){val->v, val->u}, pos);
}

bool tbv_u64(const tbv *val, uint32_t width, uint64_t *out) {
    if (!tbv_known(val, width)) {
        return false;
    }
    uint32_t count = limbs_of(width);
    *out = val->v[0];
    for (uint32_t i = 1; i < count; i++) {
        if (val->v[i] != 0) {
            *out = UINT64_MAX;
        }
    }
    return true;
}

/* The code the bits past rs->from take. */
static uint32_t fill_code(const tbv *val, const tb_resize *rs) {
    if (rs->from == 0) {
        return 0;
    }
    uint32_t top = tbv_code(val, rs->from - 1U);
    if (rs->sgn) {
        return top;
    }
    return rs->xfill && (top & CODE_U) != 0 ? top : 0U;
}

void tbv_extend(tbv *val, const tb_resize *rs) {
    if (rs->to <= rs->from) {
        mask_top(val, rs->to);
        return;
    }
    uint32_t code = fill_code(val, rs);
    uint64_t fill_v = (code & CODE_V) != 0 ? UINT64_MAX : 0;
    uint64_t fill_u = (code & CODE_U) != 0 ? UINT64_MAX : 0;
    uint32_t first = 0;
    if (rs->from > 0) {
        uint32_t last = limbs_of(rs->from) - 1U;
        uint64_t keep = top_mask(rs->from);
        val->v[last] = (val->v[last] & keep) | (fill_v & ~keep);
        val->u[last] = (val->u[last] & keep) | (fill_u & ~keep);
        first = last + 1U;
    }
    uint32_t count = limbs_of(rs->to);
    for (uint32_t i = first; i < count; i++) {
        val->v[i] = fill_v;
        val->u[i] = fill_u;
    }
    mask_top(val, rs->to);
}

/* 64 bits of a plane of width bits from bit pos on (bits past width read 0). */
static uint64_t chunk(const uint64_t *plane, uint32_t width, uint32_t pos) {
    if (pos >= width) {
        return 0;
    }
    uint32_t at = pos / LIMB_BITS;
    uint32_t shift = pos % LIMB_BITS;
    uint64_t bits = plane[at] >> shift;
    if (shift != 0 && at + 1U < limbs_of(width)) {
        bits |= plane[at + 1U] << (LIMB_BITS - shift);
    }
    return bits;
}

void tb_field_get(tbv *dst, tb_cplanes src, tb_field field) {
    uint32_t count = limbs_of(field.width);
    for (uint32_t i = 0; i < count; i++) {
        dst->v[i] = chunk(src.v, src.width, field.lsb + (i * LIMB_BITS));
        dst->u[i] = chunk(src.u, src.width, field.lsb + (i * LIMB_BITS));
    }
    mask_top(dst, field.width);
}

void tb_field_put(tb_planes dst, tb_field field, const tbv *src) {
    for (uint32_t off = 0; off < field.width;) {
        uint32_t pos = field.lsb + off;
        if (pos >= dst.width) {
            return;
        }
        uint32_t shift = pos % LIMB_BITS;
        uint32_t count = LIMB_BITS - shift;
        count = count < field.width - off ? count : field.width - off;
        count = count < dst.width - pos ? count : dst.width - pos;
        uint64_t mask = low_mask(count);
        uint64_t *word_v = &dst.v[pos / LIMB_BITS];
        uint64_t *word_u = &dst.u[pos / LIMB_BITS];
        *word_v = (*word_v & ~(mask << shift)) | ((chunk(src->v, field.width, off) & mask) << shift);
        *word_u = (*word_u & ~(mask << shift)) | ((chunk(src->u, field.width, off) & mask) << shift);
        off += count;
    }
}

tb_planes tb_var_word(const tb_var *var, uint32_t index) {
    size_t at = (size_t)index * limbs_of(var->width);
    return (tb_planes){var->v + at, var->u + at, var->width};
}

void tb_var_load(tbv *dst, const tb_var *var, uint32_t index) {
    tb_planes word = tb_var_word(var, index);
    size_t bytes = (size_t)limbs_of(var->width) * sizeof(uint64_t);
    memcpy(dst->v, word.v, bytes);
    memcpy(dst->u, word.u, bytes);
}

void tb_var_store(const tb_var *var, uint32_t index, const tbv *src) {
    tb_planes word = tb_var_word(var, index);
    size_t bytes = (size_t)limbs_of(var->width) * sizeof(uint64_t);
    memcpy(word.v, src->v, bytes);
    memcpy(word.u, src->u, bytes);
    word.v[limbs_of(var->width) - 1U] &= top_mask(var->width);
    word.u[limbs_of(var->width) - 1U] &= top_mask(var->width);
}

void tb_var_fill(const tb_var *var, uint32_t code) {
    uint32_t words = var->depth > 0 ? var->depth : 1U;
    tbv fill;
    tbv_fill(&fill, var->width, code);
    for (uint32_t i = 0; i < words; i++) {
        tb_var_store(var, i, &fill);
    }
}

/* --- digits of based numbers --- */

/* The code of an x, z or ? digit (CODE_U, CODE_U | CODE_V), or 0 for any other character. */
static uint32_t xz_code(char ch) {
    switch (ch) {
    case 'x':
    case 'X':
        return CODE_U;
    case 'z':
    case 'Z':
    case '?':
        return CODE_U | CODE_V;
    default:
        return 0;
    }
}

/* The value of a digit 0-9, a-f, A-F, or -1. */
static int digit_value(char ch) {
    if (isdigit((unsigned char)ch)) {
        return ch - '0';
    }
    int lower = tolower((unsigned char)ch);
    return lower >= 'a' && lower <= 'f' ? lower - 'a' + BASE_DEC : -1;
}

/* Sets the bits_per bits from pos of out to the digit (a value, or an x/z code with xz). */
typedef struct digit_at {
    uint32_t pos;
    uint32_t bits_per;
    uint32_t value;
    uint32_t xz;
} digit_at;

static void put_digit(tbv *out, digit_at dig) {
    for (uint32_t bit = 0; bit < dig.bits_per && dig.pos + bit < TB_MAX_WIDTH; bit++) {
        uint32_t code = dig.xz != 0 ? dig.xz : (dig.value >> bit) & 1U;
        odin3_w4_set_bit((odin3_w4){out->v, out->u}, (odin3_w4_bit_at){dig.pos + bit, code});
    }
}

/* Binary, octal or hexadecimal digits, least significant first. */
static bool power_digits(const tb_digits *digits, tbv *out, uint32_t *bits) {
    uint32_t bits_per = 1;
    if (digits->base == BASE_OCT) {
        bits_per = BITS_OCT;
    } else if (digits->base == BASE_HEX) {
        bits_per = BITS_HEX;
    }
    uint32_t count = 0;
    for (size_t i = digits->len; i > 0; i--) {
        char ch = digits->text[i - 1U];
        if (ch == '_') {
            continue;
        }
        int value = digit_value(ch);
        uint32_t xz = xz_code(ch);
        if (xz == 0 && (value < 0 || (unsigned)value >= digits->base)) {
            return false;
        }
        put_digit(out, (digit_at){count * bits_per, bits_per, (uint32_t)(value < 0 ? 0 : value),
                                  xz});
        count++;
    }
    uint64_t total = (uint64_t)count * bits_per;
    *bits = total < TB_MAX_WIDTH ? (uint32_t)total : (uint32_t)TB_MAX_WIDTH;
    return count > 0;
}

/* out = out * 10 + digit over all TB_LIMBS limbs (overflow past TB_MAX_WIDTH dropped). */
static void mul10_add(tbv *out, uint32_t digit) {
    uint64_t carry = digit;
    for (uint32_t i = 0; i < TB_LIMBS; i++) {
        uint64_t low = ((out->v[i] & HALF_MASK) * BASE_DEC) + carry;
        uint64_t high = ((out->v[i] >> HALF_BITS) * BASE_DEC) + (low >> HALF_BITS);
        out->v[i] = (low & HALF_MASK) | (high << HALF_BITS);
        carry = high >> HALF_BITS;
    }
}

/* Decimal digits, or a single x or z digit (every bit x or z). */
static bool decimal_digits(const tb_digits *digits, tbv *out, uint32_t *bits) {
    uint32_t count = 0;
    uint32_t xz = 0;
    *bits = TB_MAX_WIDTH;
    for (size_t i = 0; i < digits->len; i++) {
        char ch = digits->text[i];
        if (ch == '_') {
            continue;
        }
        uint32_t code = xz_code(ch);
        if (code != 0) {
            xz = code;
        } else if (isdigit((unsigned char)ch)) {
            mul10_add(out, (uint32_t)(ch - '0'));
        } else {
            return false;
        }
        count++;
    }
    if (xz != 0) {
        tbv_fill(out, TB_MAX_WIDTH, xz);
    }
    return count > 0 && (xz == 0 || count == 1);
}

bool tb_parse_digits(const tb_digits *digits, tbv *out, uint32_t *bits) {
    tbv_zero(out, TB_MAX_WIDTH);
    *bits = 0;
    if (digits->base == BASE_DEC) {
        return decimal_digits(digits, out, bits);
    }
    if (digits->base != BASE_BIN && digits->base != BASE_OCT && digits->base != BASE_HEX) {
        return false;
    }
    return power_digits(digits, out, bits);
}
