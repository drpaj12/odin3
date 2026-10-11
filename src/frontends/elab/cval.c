/*
 * cval.c — 4-state constant values (cval.h): construction, conversion, resizing, parts,
 * inspection, printing and hashing. The operators are in cval_arith.c.
 */
#include "frontends/elab/cval.h"

#include "ast/ast.h"
#include "frontends/elab/elab_test.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    WORD_BITS = ODIN3_CVAL_WORD_BITS,
    BYTE_BITS = 8,
    BYTE_MASK = 0xFF,
    DECIMAL_WIDTH = 32, /* EL-9: a 32-bit signed known value prints as a decimal */
    FLAGS_ALL = ODIN3_CVAL_SIGNED | ODIN3_CVAL_UNSIZED | ODIN3_CVAL_STRING,
    PRINT_CHUNK = 64,
    HIGH_SHIFT = 32, /* where the flags go in the hashed header word */
};

static odin3_status misuse(const char *fn, const char *why) {
    odin3_log(ODIN3_LOG_ERROR, "%s: %s", fn, why);
    return ODIN3_ERR_INVALID_ARG;
}

static bool width_ok(uint64_t width) {
    return width >= 1 && width <= odin3_elab_limits_get()->max_width;
}

static uint64_t low_mask(uint32_t count) {
    return count >= WORD_BITS ? UINT64_MAX : ((uint64_t)1 << count) - 1;
}

/* The used bits of the top word of a width-bit value. */
static uint64_t top_mask(uint32_t width) {
    uint32_t used = width % WORD_BITS;
    return used == 0 ? UINT64_MAX : low_mask(used);
}

static void mask_top(odin3_cval *val) {
    uint32_t top = odin3_cval_words(val->width) - 1;
    val->aval[top] &= top_mask(val->width);
    val->bval[top] &= top_mask(val->width);
}

/* Zeroed planes for `width` bits (already checked), flags 0. *out unchanged on failure. */
static odin3_status alloc_planes(odin3_arena *arena, uint32_t width, odin3_cval *out) {
    size_t words = odin3_cval_words(width);
    uint64_t *planes = odin3_arena_alloc(arena, 2 * words * sizeof *planes);
    if (planes == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *out = (odin3_cval){.width = width, .flags = 0, .aval = planes, .bval = planes + words};
    return ODIN3_OK;
}

static bool plane_a(odin3_bit bit) {
    return bit == ODIN3_BIT_1 || bit == ODIN3_BIT_X;
}

static bool plane_b(odin3_bit bit) {
    return bit == ODIN3_BIT_X || bit == ODIN3_BIT_Z;
}

static void set_bit(const odin3_cval *val, uint32_t pos, odin3_bit bit) {
    uint64_t mask = (uint64_t)1 << (pos % WORD_BITS);
    uint32_t word = pos / WORD_BITS;
    val->aval[word] = plane_a(bit) ? val->aval[word] | mask : val->aval[word] & ~mask;
    val->bval[word] = plane_b(bit) ? val->bval[word] | mask : val->bval[word] & ~mask;
}

/* Bits [from, width) become `bit`. */
static void fill_from(odin3_cval *val, uint32_t from, odin3_bit bit) {
    uint32_t words = odin3_cval_words(val->width);
    uint32_t word = from / WORD_BITS;
    if (from >= val->width) {
        return;
    }
    uint64_t keep = low_mask(from % WORD_BITS);
    uint64_t fill_a = plane_a(bit) ? UINT64_MAX : 0;
    uint64_t fill_b = plane_b(bit) ? UINT64_MAX : 0;
    val->aval[word] = (val->aval[word] & keep) | (fill_a & ~keep);
    val->bval[word] = (val->bval[word] & keep) | (fill_b & ~keep);
    for (word++; word < words; word++) {
        val->aval[word] = fill_a;
        val->bval[word] = fill_b;
    }
    mask_top(val);
}

/* --- copying bit ranges word by word ------------------------------------------------------- */

typedef struct plane_view {
    const uint64_t *words;
    uint32_t n;
} plane_view;

/* The 64 bits of the plane from bit pos (0 beyond its words). */
static uint64_t read_word(plane_view plane, uint32_t pos) {
    uint32_t word = pos / WORD_BITS;
    uint32_t off = pos % WORD_BITS;
    if (word >= plane.n) {
        return 0;
    }
    uint64_t bits = plane.words[word] >> off;
    if (off != 0 && word + 1 < plane.n) {
        bits |= plane.words[word + 1] << (WORD_BITS - off);
    }
    return bits;
}

/* `count` (1..64) bits of a plane from bit pos. */
typedef struct plane_slot {
    uint64_t *words;
    uint32_t pos;
    uint32_t count;
} plane_slot;

static void write_word(const plane_slot *slot, uint64_t bits) {
    uint32_t word = slot->pos / WORD_BITS;
    uint32_t off = slot->pos % WORD_BITS;
    uint64_t mask = low_mask(slot->count);
    bits &= mask;
    slot->words[word] = (slot->words[word] & ~(mask << off)) | (bits << off);
    if (off != 0 && off + slot->count > WORD_BITS) {
        uint32_t spill = WORD_BITS - off;
        slot->words[word + 1] = (slot->words[word + 1] & ~(mask >> spill)) | (bits >> spill);
    }
}

/* dst[at .. at + count) = src[from .. from + count), both ranges inside their values. */
typedef struct bit_copy {
    const odin3_cval *dst;
    const odin3_cval *src;
    uint32_t at;
    uint32_t from;
    uint32_t count;
} bit_copy;

static void copy_bits(const bit_copy *copy) {
    uint32_t n_words = odin3_cval_words(copy->src->width);
    plane_view src_a = {copy->src->aval, n_words};
    plane_view src_b = {copy->src->bval, n_words};
    for (uint32_t done = 0; done < copy->count; done += WORD_BITS) {
        uint32_t rest = copy->count - done;
        plane_slot slot = {copy->dst->aval, copy->at + done, rest < WORD_BITS ? rest : WORD_BITS};
        write_word(&slot, read_word(src_a, copy->from + done));
        slot.words = copy->dst->bval;
        write_word(&slot, read_word(src_b, copy->from + done));
    }
}

/* --- construction and conversion ----------------------------------------------------------- */

odin3_status odin3_cval_new(odin3_arena *arena, const odin3_cval_spec *spec, odin3_cval *out) {
    if (arena == NULL || spec == NULL || out == NULL) {
        return misuse("odin3_cval_new", "NULL argument");
    }
    if (!width_ok(spec->width) || (spec->flags & ~FLAGS_ALL) != 0 || spec->fill > ODIN3_BIT_Z) {
        return misuse("odin3_cval_new", "width, flags or fill out of range");
    }
    odin3_cval val;
    odin3_status st = alloc_planes(arena, spec->width, &val);
    if (st != ODIN3_OK) {
        return st;
    }
    val.flags = spec->flags;
    fill_from(&val, 0, spec->fill);
    *out = val;
    return ODIN3_OK;
}

static const char *literal_problem(const odin3_ast_literal *lit) {
    if (lit->value.kind != ODIN3_VAL_BITS || lit->value.bits == NULL) {
        return "the literal's value is not BITS";
    }
    if (!width_ok(lit->value.len)) {
        return "the literal's width is out of range";
    }
    for (uint32_t i = 0; i < lit->value.len; i++) {
        if (lit->value.bits[i] > ODIN3_BIT_Z) {
            return "the literal holds a byte that is not 0 1 x z";
        }
    }
    return NULL;
}

odin3_status odin3_cval_from_literal(odin3_arena *arena, const odin3_ast_literal *lit,
                                     odin3_cval *out) {
    if (arena == NULL || lit == NULL || out == NULL) {
        return misuse("odin3_cval_from_literal", "NULL argument");
    }
    const char *why = literal_problem(lit);
    if (why != NULL) {
        return misuse("odin3_cval_from_literal", why);
    }
    odin3_cval val;
    odin3_status st = alloc_planes(arena, lit->value.len, &val);
    if (st != ODIN3_OK) {
        return st;
    }
    for (uint32_t i = 0; i < val.width; i++) {
        set_bit(&val, i, (odin3_bit)lit->value.bits[i]);
    }
    val.flags = (uint8_t)((lit->is_signed ? ODIN3_CVAL_SIGNED : 0) |
                          (lit->is_unsized ? ODIN3_CVAL_UNSIZED : 0));
    *out = val;
    return ODIN3_OK;
}

odin3_status odin3_cval_from_string(odin3_arena *arena, odin3_bytes text, odin3_cval *out) {
    if (arena == NULL || out == NULL || (text.ptr == NULL && text.len > 0)) {
        return misuse("odin3_cval_from_string", "NULL argument");
    }
    size_t bytes = text.len > 0 ? text.len : 1;
    if (bytes > odin3_elab_limits_get()->max_width / BYTE_BITS) {
        return misuse("odin3_cval_from_string", "string longer than the width cap");
    }
    odin3_cval val;
    odin3_status st = alloc_planes(arena, (uint32_t)(bytes * BYTE_BITS), &val);
    if (st != ODIN3_OK) {
        return st;
    }
    const uint8_t *chars = text.ptr;
    for (size_t i = 0; i < text.len; i++) {
        size_t pos = (text.len - 1 - i) * BYTE_BITS; /* the first byte is the most significant */
        val.aval[pos / WORD_BITS] |= (uint64_t)chars[i] << (pos % WORD_BITS);
    }
    val.flags = ODIN3_CVAL_STRING;
    *out = val;
    return ODIN3_OK;
}

odin3_status odin3_cval_from_i64(odin3_arena *arena, const odin3_cval_int *num, odin3_cval *out) {
    if (arena == NULL || num == NULL || out == NULL) {
        return misuse("odin3_cval_from_i64", "NULL argument");
    }
    if (!width_ok(num->width)) {
        return misuse("odin3_cval_from_i64", "width out of range");
    }
    odin3_cval val;
    odin3_status st = alloc_planes(arena, num->width, &val);
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t words = odin3_cval_words(num->width);
    val.aval[0] = (uint64_t)num->value;
    for (uint32_t i = 1; i < words; i++) {
        val.aval[i] = num->value < 0 ? UINT64_MAX : 0;
    }
    mask_top(&val);
    val.flags = num->is_signed ? ODIN3_CVAL_SIGNED : 0;
    *out = val;
    return ODIN3_OK;
}

odin3_status odin3_cval_copy(odin3_arena *arena, const odin3_cval *src, odin3_cval *out) {
    if (arena == NULL || src == NULL || out == NULL) {
        return misuse("odin3_cval_copy", "NULL argument");
    }
    odin3_cval val;
    odin3_status st = alloc_planes(arena, src->width, &val);
    if (st != ODIN3_OK) {
        return st;
    }
    size_t bytes = (size_t)odin3_cval_words(src->width) * sizeof *val.aval;
    memcpy(val.aval, src->aval, bytes);
    memcpy(val.bval, src->bval, bytes);
    val.flags = src->flags;
    *out = val;
    return ODIN3_OK;
}

/* The bits as bytes (odin3_bit), LSB first, in arena; NULL on out of memory. */
static uint8_t *bit_bytes(odin3_arena *arena, const odin3_cval *val) {
    uint8_t *bits = odin3_arena_alloc(arena, val->width);
    if (bits != NULL) {
        for (uint32_t i = 0; i < val->width; i++) {
            bits[i] = (uint8_t)odin3_cval_bit(val, i);
        }
    }
    return bits;
}

odin3_status odin3_cval_to_value(odin3_arena *arena, const odin3_cval *val, odin3_value *out) {
    if (arena == NULL || val == NULL || out == NULL) {
        return misuse("odin3_cval_to_value", "NULL argument");
    }
    uint8_t *bits = bit_bytes(arena, val);
    if (bits == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *out = (odin3_value){.kind = ODIN3_VAL_BITS, .bits = bits, .len = val->width};
    return ODIN3_OK;
}

odin3_status odin3_cval_to_number(odin3_arena *arena, const odin3_cval *val,
                                  odin3_ast_number *out) {
    if (arena == NULL || val == NULL || out == NULL) {
        return misuse("odin3_cval_to_number", "NULL argument");
    }
    uint8_t *bits = bit_bytes(arena, val);
    if (bits == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    bool unsized = (val->flags & ODIN3_CVAL_UNSIZED) != 0;
    *out = (odin3_ast_number){.width = unsized ? 0 : val->width,
                              .nbits = val->width,
                              .bits = bits,
                              .has_xz = !odin3_cval_is_known(val),
                              .has_question = false,
                              .warned = false};
    return ODIN3_OK;
}

/* --- resizing and inspection --------------------------------------------------------------- */

odin3_status odin3_cval_resize(odin3_arena *arena, const odin3_cval *val, uint32_t width,
                               bool is_signed, odin3_cval *out) {
    if (arena == NULL || val == NULL || out == NULL) {
        return misuse("odin3_cval_resize", "NULL argument");
    }
    if (!width_ok(width)) {
        return misuse("odin3_cval_resize", "width out of range");
    }
    odin3_cval res;
    odin3_status st = alloc_planes(arena, width, &res);
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t keep = val->width < width ? val->width : width;
    const bit_copy copy = {&res, val, 0, 0, keep};
    copy_bits(&copy);
    bool repeat = is_signed || (val->flags & ODIN3_CVAL_UNSIZED) != 0;
    fill_from(&res, keep, repeat ? odin3_cval_bit(val, val->width - 1) : ODIN3_BIT_0);
    res.flags = is_signed ? ODIN3_CVAL_SIGNED : 0;
    *out = res;
    return ODIN3_OK;
}

odin3_bit odin3_cval_bit(const odin3_cval *val, uint32_t pos) {
    if (val == NULL || pos >= val->width) {
        return ODIN3_BIT_X;
    }
    uint32_t word = pos / WORD_BITS;
    uint32_t off = pos % WORD_BITS;
    bool abit = ((val->aval[word] >> off) & 1U) != 0;
    bool bbit = ((val->bval[word] >> off) & 1U) != 0;
    if (bbit) {
        return abit ? ODIN3_BIT_X : ODIN3_BIT_Z;
    }
    return abit ? ODIN3_BIT_1 : ODIN3_BIT_0;
}

odin3_bit odin3_cval_truth(const odin3_cval *val) {
    bool unknown = false;
    for (uint32_t i = 0; i < odin3_cval_words(val->width); i++) {
        if ((val->aval[i] & ~val->bval[i]) != 0) {
            return ODIN3_BIT_1;
        }
        unknown = unknown || val->bval[i] != 0;
    }
    return unknown ? ODIN3_BIT_X : ODIN3_BIT_0;
}

bool odin3_cval_is_known(const odin3_cval *val) {
    for (uint32_t i = 0; i < odin3_cval_words(val->width); i++) {
        if (val->bval[i] != 0) {
            return false;
        }
    }
    return true;
}

/* Words 1.. hold only copies of the sign (`ones`): all ones (to the width) or all zeros. */
static bool high_words_are(const odin3_cval *val, bool ones) {
    uint32_t words = odin3_cval_words(val->width);
    for (uint32_t i = 1; i < words; i++) {
        uint64_t used = i + 1 == words ? top_mask(val->width) : UINT64_MAX;
        if (val->aval[i] != (ones ? used : 0)) {
            return false;
        }
    }
    return true;
}

bool odin3_cval_to_i64(const odin3_cval *val, int64_t *out) {
    if (val == NULL || out == NULL || !odin3_cval_is_known(val)) {
        return false;
    }
    bool negative =
        (val->flags & ODIN3_CVAL_SIGNED) != 0 && odin3_cval_bit(val, val->width - 1) == ODIN3_BIT_1;
    uint64_t low = val->aval[0];
    if (val->width < WORD_BITS) {
        low |= negative ? ~low_mask(val->width) : 0;
    } else if ((low >> (WORD_BITS - 1) != 0) != negative || !high_words_are(val, negative)) {
        return false;
    }
    *out = (int64_t)low;
    return true;
}

bool odin3_cval_equal(const odin3_cval *lhs, const odin3_cval *rhs) {
    if (lhs->width != rhs->width || lhs->flags != rhs->flags) {
        return false;
    }
    size_t bytes = (size_t)odin3_cval_words(lhs->width) * sizeof *lhs->aval;
    return memcmp(lhs->aval, rhs->aval, bytes) == 0 && memcmp(lhs->bval, rhs->bval, bytes) == 0;
}

uint64_t odin3_cval_hash(const odin3_cval *val, uint64_t seed) {
    size_t bytes = (size_t)odin3_cval_words(val->width) * sizeof *val->aval;
    uint64_t hash = odin3_hash_combine(seed, (uint64_t)val->flags << HIGH_SHIFT | val->width);
    hash = odin3_hash_bytes((odin3_bytes){val->aval, bytes}, hash);
    return odin3_hash_bytes((odin3_bytes){val->bval, bytes}, hash);
}

/* --- printing (EL-9) ----------------------------------------------------------------------- */

static bool prints_as_string(const odin3_cval *val) {
    return (val->flags & ODIN3_CVAL_STRING) != 0 && val->width % BYTE_BITS == 0 &&
           odin3_cval_is_known(val);
}

static odin3_status print_string(const odin3_cval *val, odin3_strbuf *out) {
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr("\""));
    for (uint32_t k = val->width / BYTE_BITS; k-- > 0 && st == ODIN3_OK;) {
        uint32_t pos = k * BYTE_BITS;
        char ch = (char)((val->aval[pos / WORD_BITS] >> (pos % WORD_BITS)) & BYTE_MASK);
        if (ch == '"' || ch == '\\') {
            st = odin3_strbuf_append(out, (odin3_bytes){"\\", 1});
        }
        if (ch != '\0' && st == ODIN3_OK) {
            st = odin3_strbuf_append(out, (odin3_bytes){&ch, 1});
        }
    }
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("\"")) : st;
}

static odin3_status print_bits(const odin3_cval *val, odin3_strbuf *out) {
    bool is_signed = (val->flags & ODIN3_CVAL_SIGNED) != 0;
    odin3_status st =
        odin3_strbuf_appendf(out, "%" PRIu32 "'%sb", val->width, is_signed ? "s" : "");
    char chunk[PRINT_CHUNK];
    uint32_t used = 0;
    for (uint32_t pos = val->width; pos-- > 0 && st == ODIN3_OK;) {
        chunk[used++] = "01xz"[odin3_cval_bit(val, pos)];
        if (used == PRINT_CHUNK || pos == 0) {
            st = odin3_strbuf_append(out, (odin3_bytes){chunk, used});
            used = 0;
        }
    }
    return st;
}

static odin3_status print_any(const odin3_cval *val, odin3_strbuf *out) {
    int64_t num = 0;
    if (val->width == DECIMAL_WIDTH && (val->flags & ODIN3_CVAL_SIGNED) != 0 &&
        odin3_cval_to_i64(val, &num)) {
        return odin3_strbuf_appendf(out, "%" PRId64, num);
    }
    return prints_as_string(val) ? print_string(val, out) : print_bits(val, out);
}

odin3_status odin3_cval_print(const odin3_cval *val, odin3_strbuf *out) {
    if (val == NULL || out == NULL) {
        return misuse("odin3_cval_print", "NULL argument");
    }
    size_t mark = out->len;
    odin3_status st = print_any(val, out);
    if (st != ODIN3_OK && out->data != NULL) {
        out->len = mark; /* the buffer as it was */
        out->data[mark] = '\0';
    }
    return st;
}

/* --- parts and mux ------------------------------------------------------------------------- */

odin3_status odin3_cval_select(odin3_arena *arena, const odin3_cval_part *part, odin3_cval *out) {
    if (arena == NULL || part == NULL || part->val == NULL || out == NULL) {
        return misuse("odin3_cval_select", "NULL argument");
    }
    if (!width_ok(part->width)) {
        return misuse("odin3_cval_select", "width out of range");
    }
    odin3_cval res;
    odin3_status st = alloc_planes(arena, part->width, &res);
    if (st != ODIN3_OK) {
        return st;
    }
    fill_from(&res, 0, ODIN3_BIT_X);
    int64_t lo = part->lo;
    if (lo < (int64_t)part->val->width && lo > -(int64_t)part->width) {
        int64_t from = lo > 0 ? lo : 0;
        int64_t end = lo + (int64_t)part->width;
        end = end < (int64_t)part->val->width ? end : (int64_t)part->val->width;
        const bit_copy copy = {&res, part->val, (uint32_t)(from - lo), (uint32_t)from,
                               (uint32_t)(end - from)};
        copy_bits(&copy);
    }
    *out = res;
    return ODIN3_OK;
}

odin3_status odin3_cval_concat(odin3_arena *arena, const odin3_cval *parts, uint32_t n,
                               odin3_cval *out) {
    if (arena == NULL || parts == NULL || out == NULL || n == 0) {
        return misuse("odin3_cval_concat", "NULL argument or no parts");
    }
    uint64_t total = 0;
    for (uint32_t i = 0; i < n; i++) {
        total += parts[i].width;
    }
    if (!width_ok(total)) {
        return misuse("odin3_cval_concat", "width out of range");
    }
    odin3_cval res;
    odin3_status st = alloc_planes(arena, (uint32_t)total, &res);
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t at = 0;
    for (uint32_t i = n; i-- > 0;) { /* parts[n - 1] is the least significant */
        const bit_copy copy = {&res, &parts[i], at, 0, parts[i].width};
        copy_bits(&copy);
        at += parts[i].width;
    }
    *out = res;
    return ODIN3_OK;
}

odin3_status odin3_cval_replicate(odin3_arena *arena, const odin3_cval *val, uint32_t count,
                                  odin3_cval *out) {
    if (arena == NULL || val == NULL || out == NULL || count == 0) {
        return misuse("odin3_cval_replicate", "NULL argument or a count of 0");
    }
    uint64_t total = (uint64_t)val->width * count;
    if (!width_ok(total)) {
        return misuse("odin3_cval_replicate", "width out of range");
    }
    odin3_cval res;
    odin3_status st = alloc_planes(arena, (uint32_t)total, &res);
    if (st != ODIN3_OK) {
        return st;
    }
    for (uint32_t k = 0; k < count; k++) {
        const bit_copy copy = {&res, val, k * val->width, 0, val->width};
        copy_bits(&copy);
    }
    *out = res;
    return ODIN3_OK;
}

odin3_status odin3_cval_mux(odin3_arena *arena, const odin3_cval_mux_args *args, odin3_cval *out) {
    if (arena == NULL || args == NULL || args->cond == NULL || args->a == NULL || args->b == NULL ||
        out == NULL) {
        return misuse("odin3_cval_mux", "NULL argument");
    }
    if (args->a->width != args->b->width) {
        return misuse("odin3_cval_mux", "the two branches differ in width");
    }
    odin3_bit cond = odin3_cval_truth(args->cond);
    const odin3_cval *pick = cond == ODIN3_BIT_0 ? args->b : args->a;
    odin3_cval res;
    odin3_status st = odin3_cval_copy(arena, pick, &res);
    if (st != ODIN3_OK) {
        return st;
    }
    if (cond == ODIN3_BIT_X) {
        for (uint32_t i = 0; i < odin3_cval_words(res.width); i++) {
            uint64_t differ =
                (args->a->aval[i] ^ args->b->aval[i]) | (args->a->bval[i] ^ args->b->bval[i]);
            res.aval[i] |= differ;
            res.bval[i] |= differ;
        }
        res.flags = args->a->flags & args->b->flags;
    }
    res.flags &= (uint8_t)~ODIN3_CVAL_UNSIZED;
    *out = res;
    return ODIN3_OK;
}
