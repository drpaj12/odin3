/*
 * cval.h — 4-state constant values of elaboration (spec §4.1, EL-12) and the operators on them
 * (§4.3, EL-14), with Yosys 0.55's RTLIL::const_* semantics (kernel/calc.cc), which are also the
 * word cells' simlib semantics, so folding in 2D and evaluating a cell agree.
 *
 * A value is `width` bits in two planes, LSB first, ceil(width / 64) words each (VPI aval/bval:
 * 00 = 0, 10 = 1, 01 = z, 11 = x); the plane bits at `width` and above are always 0. Every
 * function that makes a value allocates its planes (and any scratch) in the caller's arena; the
 * value lives as long as the arena. On failure *out is unchanged. Widths are 1 .. the width cap
 * in force (ODIN3_ELAB_MAX_WIDTH unless a test lowered it); a width outside that range is
 * INVALID_ARG (logged): a caller bug, since the evaluator checks widths first.
 */
#ifndef ODIN3_FRONTENDS_ELAB_CVAL_H
#define ODIN3_FRONTENDS_ELAB_CVAL_H

#include "ast/ast.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>

/* A 4-state constant: bits in VPI aval/bval planes (00 = 0, 10 = 1, 01 = z, 11 = x), LSB first. */
typedef struct odin3_cval {
    uint32_t width; /* 1 .. ODIN3_ELAB_MAX_WIDTH */
    uint8_t flags;  /* ODIN3_CVAL_SIGNED | ODIN3_CVAL_UNSIZED | ODIN3_CVAL_STRING */
    uint64_t *aval; /* ceil(width / 64) words, in the evaluation arena */
    uint64_t *bval; /* likewise */
} odin3_cval;

/*
 * SIGNED: the value is signed. UNSIZED: it came from an unsized literal and has not been
 * through an operator (EL-15), so it extends by repeating its top bit. STRING: a string literal
 * or a parameter holding one (L10: printing and EL-9 naming; arithmetic ignores it).
 */
enum { ODIN3_CVAL_SIGNED = 1, ODIN3_CVAL_UNSIZED = 2, ODIN3_CVAL_STRING = 4 };

enum { ODIN3_CVAL_WORD_BITS = 64 };

/* Words per plane of a width-bit value: ceil(width / 64). */
static inline uint32_t odin3_cval_words(uint32_t width) {
    return width / ODIN3_CVAL_WORD_BITS + (width % ODIN3_CVAL_WORD_BITS != 0 ? 1U : 0U);
}

/* --- construction and conversion ----------------------------------------------------------- */

/* A new value's shape (a request struct: width and flags are convertible types). */
typedef struct odin3_cval_spec {
    uint32_t width;
    uint8_t flags;  /* ODIN3_CVAL_* */
    odin3_bit fill; /* the state of every bit */
} odin3_cval_spec;

/*
 * A spec->width-bit value with every bit spec->fill (0, 1, x or z) and spec->flags. INVALID_ARG
 * (logged) for a NULL argument, a width of 0 or above the cap, unknown flags or fill; NO_MEMORY.
 */
odin3_status odin3_cval_new(odin3_arena *arena, const odin3_cval_spec *spec, odin3_cval *out);

/*
 * A literal's value (odin3_ast_number_value): its BITS, SIGNED from is_signed and UNSIZED from
 * is_unsized. INVALID_ARG (logged) for a NULL argument, a value that is not BITS of 1 .. cap
 * bits or holds a byte other than 0 1 x z; NO_MEMORY.
 */
odin3_status odin3_cval_from_literal(odin3_arena *arena, const odin3_ast_literal *lit,
                                     odin3_cval *out);

/*
 * A string's value (L10, IEEE §5.2.3.3, Yosys mkconst_str): 8 bits per byte, the first byte most
 * significant; "" is 8'h00. Flags: STRING (unsigned, sized). INVALID_ARG (logged) for a NULL
 * argument or more than cap / 8 bytes; NO_MEMORY.
 */
odin3_status odin3_cval_from_string(odin3_arena *arena, odin3_bytes text, odin3_cval *out);

/* An integer and the width and sign of the value it becomes (a request struct, as above). */
typedef struct odin3_cval_int {
    int64_t value;
    uint32_t width;
    bool is_signed;
} odin3_cval_int;

/*
 * num->value in two's complement at num->width bits (truncated, or extended by its own sign: a
 * negative value has ones above bit 63), SIGNED when num->is_signed. INVALID_ARG (logged) for a
 * NULL argument or a width of 0 or above the cap; NO_MEMORY.
 */
odin3_status odin3_cval_from_i64(odin3_arena *arena, const odin3_cval_int *num, odin3_cval *out);

/* A copy of src (planes and flags) in arena. INVALID_ARG (logged) for NULL; NO_MEMORY. */
odin3_status odin3_cval_copy(odin3_arena *arena, const odin3_cval *src, odin3_cval *out);

/*
 * The value as an IR value (IR-10): BITS, one odin3_bit per byte, LSB first, `width` bytes in
 * arena. INVALID_ARG (logged) for NULL; NO_MEMORY.
 */
odin3_status odin3_cval_to_value(odin3_arena *arena, const odin3_cval *val, odin3_value *out);

/*
 * A NUMBER payload for odin3_ast_number_new (plan point (bb)): nbits = the value's width, its
 * bits in arena, has_xz from the bits, payload width 0 when the value is UNSIZED and its width
 * otherwise. The node's sub-kind and SIGNED flag are the caller's. A NUMBER built from it gives
 * back the same bits through odin3_ast_number_value when the value is sized, or unsized with at
 * least 32 bits and no signed top 1 (every unsized literal value). INVALID_ARG (logged) for
 * NULL; NO_MEMORY.
 */
odin3_status odin3_cval_to_number(odin3_arena *arena, const odin3_cval *val, odin3_ast_number *out);

/* --- resizing and inspection --------------------------------------------------------------- */

/*
 * val at `width` bits (EL-13): an UNSIZED value repeats its top bit (Yosys bitsAsConst), a sized
 * one is sign-extended when is_signed and zero-extended otherwise (x and z top bits repeat too);
 * truncation keeps the low bits. The result is sized and not STRING, with SIGNED = is_signed,
 * even at the same width. EL-16's typed-parameter conversion is odin3_cval_to_param, not this:
 * at equal widths Yosys keeps the constant, UNSIZED and STRING included (simplify.cc 2381-2390).
 * INVALID_ARG (logged) for NULL or a width of 0 or above the cap; NO_MEMORY.
 */
odin3_status odin3_cval_resize(odin3_arena *arena, const odin3_cval *val, uint32_t width,
                               bool is_signed, odin3_cval *out);

/* EL-16's declaration forms of a parameter or localparam (odin3_cparam_type.mode). */
typedef enum odin3_cparam_mode {
    ODIN3_CPARAM_UNTYPED, /* `parameter P = e` */
    ODIN3_CPARAM_SIGNED,  /* `parameter signed P = e`: no range */
    ODIN3_CPARAM_SIZED    /* `parameter [m:l] P` (signed or not) or `parameter integer P` */
} odin3_cparam_mode;

/* A parameter's declared type: its mode, and for SIZED its width (|m - l| + 1; integer: 32) and
 * sign (`signed` written; integer: signed). */
typedef struct odin3_cparam_type {
    uint8_t mode; /* odin3_cparam_mode */
    uint32_t width;
    bool is_signed;
} odin3_cparam_type;

/*
 * EL-16's conversion of a parameter's value (self-determined) to its declared type, the one rule
 * for every default, localparam, #() and defparam override, project override, cell-type target
 * and generate-block parameter (spec §4.5; Yosys simplify.cc 2381-2390):
 *  - UNTYPED: val as it is (width, SIGNED, UNSIZED and STRING kept);
 *  - SIGNED: val's width and flags, SIGNED set;
 *  - SIZED at another width: val extended by its OWN sign (an UNSIZED val does not repeat its
 *    top bit here: Yosys's extend_u0) or truncated to decl->width; sized, not STRING, SIGNED =
 *    decl->is_signed;
 *  - SIZED at val's width: val kept whole, SIGNED = decl->is_signed, UNSIZED and STRING kept
 *    (the Yosys quirk of §17 #22: `localparam [31:0] Q = 'hFFFFFFFF` stays unsized, so it fills
 *    a 64-bit context with ones, where `[35:0]` zero-extends).
 * A zero-width val (the empty {0{…}}) is copied as it is. The result is a copy in arena. Never
 * call odin3_cval_resize for this: it drops UNSIZED at equal widths. INVALID_ARG (logged) for a
 * NULL argument, an unknown mode or a SIZED width of 0 or above the cap; NO_MEMORY (*out
 * unchanged on failure).
 */
odin3_status odin3_cval_to_param(odin3_arena *arena, const odin3_cval *val,
                                 const odin3_cparam_type *decl, odin3_cval *out);

/*
 * The bytes of a string value (L10; the inverse of odin3_cval_from_string), for writing a STRING
 * node or an EL-9 name: width / 8 bytes, the most significant byte first, NUL-terminated in
 * arena (the terminator not counted in out->len). Every byte is kept, NUL included, except that
 * 8'h00 (the value of "") gives zero bytes, so odin3_cval_from_string of the result gives val's
 * bits back. Callers check STRING themselves (any value of whole known bytes converts).
 * INVALID_ARG (logged) for a NULL argument, a width that is not a multiple of 8 or an x or z
 * bit; NO_MEMORY.
 */
odin3_status odin3_cval_string_bytes(odin3_arena *arena, const odin3_cval *val, odin3_bytes *out);

/* Bit pos (0 = LSB); x for a position at or above the width. */
odin3_bit odin3_cval_bit(const odin3_cval *val, uint32_t pos);

/* The value as a condition (IEEE): 1 when any bit is 1, else x when any bit is x or z, else 0. */
odin3_bit odin3_cval_truth(const odin3_cval *val);

/* True when no bit is x or z. */
bool odin3_cval_is_known(const odin3_cval *val);

/*
 * The value as an integer, signed when SIGNED, in *out. False (*out unchanged) when a bit is x
 * or z or the value is outside int64_t.
 */
bool odin3_cval_to_i64(const odin3_cval *val, int64_t *out);

/* Same width, same flags and the same bits (x and z distinct). */
bool odin3_cval_equal(const odin3_cval *lhs, const odin3_cval *rhs);

/* A hash of width, flags and both planes; equal values hash equally. */
uint64_t odin3_cval_hash(const odin3_cval *val, uint64_t seed);

/*
 * Appends EL-9's spelling: a plain decimal for a 32-bit SIGNED value without x/z (`8`, `-3`); a
 * STRING value as "text" with `"` and `\` escaped when every byte is printable (no NUL, control
 * byte or DEL; the empty string "" is its one NUL byte), so distinct values never print alike;
 * otherwise `<width>'<s>b<bits>` MSB first (`4'b10x0`, `8'sb11110000`). INVALID_ARG (logged) for
 * NULL; NO_MEMORY (the buffer is unchanged).
 */
odin3_status odin3_cval_print(const odin3_cval *val, odin3_strbuf *out);

/* --- operators (EL-14) --------------------------------------------------------------------- */

typedef enum odin3_cop {
    ODIN3_COP_ADD,
    ODIN3_COP_SUB,
    ODIN3_COP_MUL,
    ODIN3_COP_DIV,
    ODIN3_COP_MOD,
    ODIN3_COP_POW,
    ODIN3_COP_AND,
    ODIN3_COP_OR,
    ODIN3_COP_XOR,
    ODIN3_COP_XNOR,
    ODIN3_COP_SHL,
    ODIN3_COP_SHR,
    ODIN3_COP_SSHL,
    ODIN3_COP_SSHR,
    ODIN3_COP_EQ,
    ODIN3_COP_NE,
    ODIN3_COP_EQX,
    ODIN3_COP_NEX,
    ODIN3_COP_LT,
    ODIN3_COP_LE,
    ODIN3_COP_GT,
    ODIN3_COP_GE,
    ODIN3_COP_LAND,
    ODIN3_COP_LOR,
    ODIN3_COP_NEG,
    ODIN3_COP_POS,
    ODIN3_COP_NOT,
    ODIN3_COP_LNOT,
    ODIN3_COP_RAND,
    ODIN3_COP_RNAND,
    ODIN3_COP_ROR,
    ODIN3_COP_RNOR,
    ODIN3_COP_RXOR,
    ODIN3_COP_RXNOR,
    ODIN3_COP_COUNT
} odin3_cop;

/* One operation: a op b (b unused by the unary ones) at `width` bits in context sign is_signed. */
typedef struct odin3_cop_args {
    odin3_cop op;
    const odin3_cval *a;
    const odin3_cval *b;
    uint32_t width;
    bool is_signed;
} odin3_cop_args;

/*
 * The result of RTLIL::const_<op> at `width` bits (Yosys simplify's constant folding):
 *  - arithmetic (+ - * / % and unary -), bitwise (& | ^ ~^ ~) and unary +: both operands are
 *    first resized to `width` by is_signed (odin3_cval_resize: an UNSIZED operand repeats its top
 *    bit). An x or z bit in an operand of + - * / % or unary - makes every bit x, as does / or %
 *    by zero; signed / truncates toward zero and % takes the dividend's sign; unary + keeps x/z.
 *    The bitwise operators follow the IEEE 4-state tables (z reads as x). SIGNED = is_signed.
 *  - shifts (<< >> <<< >>>): only `a` is resized; `b` is an unsigned amount at its own width; an
 *    x/z bit in b makes every bit x; >>> fills with a's (resized) top bit when is_signed. `**`
 *    resizes `a` likewise and reads `b` as signed when b is SIGNED: 0 ** negative is x,
 *    0 ** positive is 0 (even when x/z bits read as 0 make the base or exponent so: Yosys's
 *    order), 1 ** negative is 1, (-1) ** negative is +-1 by parity, any other negative power 0,
 *    a positive power is taken modulo 2^width; otherwise an x/z bit makes every bit x.
 *  - comparisons (== != === !== < <= > >=): a and b are resized to their common width, signed
 *    when both are SIGNED (is_signed is not used). == gives 0 on any 0/1 mismatch, else x when a
 *    bit is x/z, else 1 (!= its inverse); === and !== compare the four states exactly; < <= > >=
 *    give x when a bit is x/z.
 *  - logical (&& || !) and reductions (& ~& | ~| ^ ~^): the operands as they are; a value is true
 *    when any bit is 1, false when all are 0, else x.
 * Comparisons, logical operators and reductions give one bit, zero-extended to `width`
 * (unsigned). No result is UNSIZED or STRING. INVALID_ARG (logged) for a NULL argument or
 * operand, an unknown op or a width of 0 or above the cap; NO_MEMORY.
 */
odin3_status odin3_cval_op(odin3_arena *arena, const odin3_cop_args *args, odin3_cval *out);

/* --- parts and mux ------------------------------------------------------------------------- */

/* `width` bits of val starting at bit lo (which may be negative). */
typedef struct odin3_cval_part {
    const odin3_cval *val;
    int64_t lo;
    uint32_t width;
} odin3_cval_part;

/*
 * The part as an unsigned sized value; positions outside val read x (E17). INVALID_ARG (logged)
 * for NULL or a width of 0 or above the cap; NO_MEMORY.
 */
odin3_status odin3_cval_select(odin3_arena *arena, const odin3_cval_part *part, odin3_cval *out);

/*
 * {parts[0], …, parts[n-1]}: parts[0] is the most significant. Unsigned and sized. INVALID_ARG
 * (logged) for NULL, n = 0 or a total width above the cap; NO_MEMORY.
 */
odin3_status odin3_cval_concat(odin3_arena *arena, const odin3_cval *parts, uint32_t n,
                               odin3_cval *out);

/*
 * {count{val}}: unsigned and sized. INVALID_ARG (logged) for NULL, count = 0 or a total width
 * above the cap; NO_MEMORY.
 */
odin3_status odin3_cval_replicate(odin3_arena *arena, const odin3_cval *val, uint32_t count,
                                  odin3_cval *out);

/* cond ? a : b, with a and b of one width. */
typedef struct odin3_cval_mux_args {
    const odin3_cval *cond;
    const odin3_cval *a;
    const odin3_cval *b;
} odin3_cval_mux_args;

/*
 * The conditional (IEEE Table 5-21, Yosys const_mux): cond true gives a copy of a, false of b
 * (flags kept but UNSIZED); cond x merges a and b bit by bit, equal bits kept (z = z), different
 * bits x, flags those a and b share but UNSIZED. INVALID_ARG (logged) for NULL or a and b of
 * different widths; NO_MEMORY.
 */
odin3_status odin3_cval_mux(odin3_arena *arena, const odin3_cval_mux_args *args, odin3_cval *out);

#endif
