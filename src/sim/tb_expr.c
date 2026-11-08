/* tb_expr.c — testbench expression evaluation and lvalue writes (spec section 2: widths and signs
 * follow IEEE 1364-2005 5.4/5.5 for the supported operators; values are 4-state and up to
 * TB_MAX_WIDTH bits, design note docs/specs/2026-11-05-sim-4state-design.md section 5).
 *
 * The repository forbids recursion, so tb_eval walks the expression tree with an explicit frame
 * stack: a frame names its node, the width and signedness it is evaluated in, how many children
 * are done and their values; a finished frame leaves its value in its first slot and hands it to
 * the frame below. */
#include "sim/tb_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool op_is(const tb_expr *expr, const char *text) {
    return strcmp(expr->op, text) == 0;
}

static uint32_t max_u32(uint32_t lhs, uint32_t rhs) {
    return lhs > rhs ? lhs : rhs;
}

int32_t tb_random(int32_t *seed) {
    /* Park-Miller minimal standard generator (see the spec: matched against iverilog in the gate).
     */
    int64_t state = *seed;
    state = state <= 0 ? 1 : state;
    state = (state * TB_RAND_MULT) % TB_RAND_MOD;
    *seed = (int32_t)state;
    return *seed;
}

/* val (from bits) extended in place to `to` bits: by its sign when sgn, else with 0. */
static void extend(tbv *val, uint32_t from, uint32_t to, bool sgn) {
    tb_resize rs = {from, to, sgn, false};
    tbv_extend(val, &rs);
}

/* --- lvalue writes (no evaluation inside: indices arrive already evaluated) --- */

/* The index expression of a select lvalue, or NULL. */
static const tb_expr *lv_index(const tb_expr *lv) {
    if (lv->kind == E_BITSEL) {
        return lv->b;
    }
    return lv->kind == E_MEM ? lv->a : NULL;
}

/* Writes val (lv->width bits) to the leaf; an index out of range or unknown (UINT64_MAX) drops
 * the write. */
static void write_leaf(tb_ev *ev, const tb_expr *lv, const tbv *val, uint64_t idx) {
    bool select = lv->kind == E_BITSEL || lv->kind == E_PART;
    if (tb_forced(ev->tb, select ? lv->a->var : lv->var)) {
        return;
    }
    switch (lv->kind) {
    case E_VAR:
        tb_var_store(&ev->tb->vars[lv->var], 0, val);
        break;
    case E_MEM: {
        const tb_var *var = &ev->tb->vars[lv->var];
        if (idx < var->depth) {
            tb_var_store(var, (uint32_t)idx, val);
        }
        break;
    }
    case E_BITSEL: {
        const tb_var *var = &ev->tb->vars[lv->a->var];
        if (idx < var->width) {
            tb_field_put(tb_var_word(var, 0), (tb_field){(uint32_t)idx, 1}, val);
        }
        break;
    }
    case E_PART:
        tb_field_put(tb_var_word(&ev->tb->vars[lv->a->var], 0), (tb_field){lv->aux, lv->width},
                     val);
        break;
    default:
        ev->failed = true;
        break;
    }
}

/* A known number written to a leaf, at the leaf's width. */
static void write_number(tb_ev *ev, uint64_t num, const tb_expr *lv, uint64_t idx) {
    tbv val;
    tbv_set_u64(num, &val, lv->width);
    write_leaf(ev, lv, &val, idx);
}

/* --- evaluation --- */
typedef enum {
    C_PLAIN,
    C_LOGIC,
    C_CMP,
    C_SHIFT,
    C_POW,
    C_ARITH,
    C_UN_ARITH,
    C_UN_RED,
    C_TIME,
    C_RANDOM,
    C_FOPEN,
    C_CLOG2,
    C_TEST_PLUSARGS,
    C_VALUE_PLUSARGS,
    C_CAST
} ev_class;

typedef struct ev_frame {
    const tb_expr *expr;
    uint32_t width; /* context width, at least the node's own */
    bool sgn;
    ev_class cls;
    uint32_t stage;  /* children delivered so far */
    uint32_t reps;   /* concatenation: repeats per item */
    uint32_t placed; /* concatenation: bits placed so far, from the top */
    uint32_t cond;   /* ?: the condition's truth */
    tbv val[2];      /* the children's values; the result goes in val[0] */
} ev_frame;

enum { EV_LOCAL_FRAMES = 32 };

typedef struct ev_stack {
    ev_frame *frames;
    uint32_t top;
    uint32_t cap;
    ev_frame local[EV_LOCAL_FRAMES];
} ev_stack;

static ev_class class_of_bin(const tb_expr *expr) {
    if (op_is(expr, "&&") || op_is(expr, "||")) {
        return C_LOGIC;
    }
    static const char *const CMPS[] = {"==", "!=", "===", "!==", "<", "<=", ">", ">=", NULL};
    for (size_t i = 0; CMPS[i] != NULL; i++) {
        if (op_is(expr, CMPS[i])) {
            return C_CMP;
        }
    }
    if (op_is(expr, "**")) {
        return C_POW;
    }
    bool shift =
        (expr->op[0] == '<' && expr->op[1] == '<') || (expr->op[0] == '>' && expr->op[1] == '>');
    return shift ? C_SHIFT : C_ARITH;
}

static ev_class class_of_sys(const tb_expr *expr) {
    static const struct {
        const char *name;
        ev_class cls;
    } TABLE[] = {{"$time", C_TIME},
                 {"$random", C_RANDOM},
                 {"$urandom", C_RANDOM},
                 {"$fopen", C_FOPEN},
                 {"$clog2", C_CLOG2},
                 {"$test$plusargs", C_TEST_PLUSARGS},
                 {"$value$plusargs", C_VALUE_PLUSARGS},
                 {NULL, C_CAST}};
    size_t i = 0;
    while (TABLE[i].name != NULL && strcmp(expr->str, TABLE[i].name) != 0) {
        i++;
    }
    return TABLE[i].cls;
}

static ev_class class_of(const tb_expr *expr) {
    if (expr->kind == E_BIN) {
        return class_of_bin(expr);
    }
    if (expr->kind == E_SYS) {
        return class_of_sys(expr);
    }
    if (expr->kind == E_UN) {
        bool arith = op_is(expr, "+") || op_is(expr, "-") || op_is(expr, "~");
        return arith ? C_UN_ARITH : C_UN_RED;
    }
    return C_PLAIN;
}

static bool stack_grow(ev_stack *stk) {
    uint32_t cap = stk->cap * 2U;
    ev_frame *grown = malloc((size_t)cap * sizeof *grown);
    if (grown == NULL) {
        return false;
    }
    memcpy(grown, stk->frames, (size_t)stk->top * sizeof *grown);
    if (stk->frames != stk->local) {
        free(stk->frames);
    }
    stk->frames = grown;
    stk->cap = cap;
    return true;
}

static bool stack_push(ev_stack *stk, const tb_expr *expr, uint32_t width, bool sgn) {
    if (stk->top == stk->cap && !stack_grow(stk)) {
        return false;
    }
    ev_frame *fr = &stk->frames[stk->top++];
    fr->expr = expr;
    fr->width = max_u32(width, expr->width);
    fr->sgn = sgn;
    fr->cls = class_of(expr);
    fr->stage = 0;
    fr->reps = expr->kind == E_REPL ? (uint32_t)expr->num : 1U;
    fr->placed = 0;
    fr->cond = 0;
    if (expr->kind == E_CONCAT || expr->kind == E_REPL) {
        tbv_zero(&fr->val[0], fr->width);
    }
    return true;
}

/* The child to evaluate at the frame's current stage and the context to evaluate it in. */
typedef struct ev_child {
    const tb_expr *expr;
    uint32_t width;
    bool sgn;
} ev_child;

static bool child_bin(const ev_frame *fr, ev_child *out) {
    const tb_expr *expr = fr->expr;
    const tb_expr *side = fr->stage == 0 ? expr->a : expr->b;
    *out = (ev_child){side, side->width, false};
    bool own_ctx = fr->cls == C_SHIFT || fr->cls == C_POW;
    if (fr->cls == C_CMP) {
        out->width = max_u32(expr->a->width, expr->b->width);
        out->sgn = expr->a->sgn && expr->b->sgn;
    } else if (fr->cls == C_ARITH || (own_ctx && fr->stage == 0)) {
        out->width = fr->width;
        out->sgn = fr->sgn;
    } else if (fr->cls == C_POW) {
        out->sgn = side->sgn; /* the exponent is self-determined, with its own sign */
    }
    return fr->stage < 2;
}

static bool child_sys(const ev_frame *fr, ev_child *out) {
    const tb_expr *expr = fr->expr;
    if (fr->cls == C_TIME || fr->cls == C_FOPEN || fr->cls == C_TEST_PLUSARGS) {
        return false;
    }
    if (fr->cls == C_VALUE_PLUSARGS) { /* only the index of the target, if it has one */
        const tb_expr *index = lv_index(expr->list[1]);
        *out = (ev_child){index, index != NULL ? index->width : 0U, false};
        return fr->stage == 0 && index != NULL;
    }
    if (fr->cls == C_RANDOM) {
        if (expr->n == 0) {
            return false;
        }
        const tb_expr *index = lv_index(expr->list[0]);
        if (fr->stage == 0) {
            *out = (ev_child){expr->list[0], TB_INT_BITS, false};
            return true;
        }
        *out = (ev_child){index, index != NULL ? index->width : 0U, false};
        return fr->stage == 1 && index != NULL;
    }
    *out = (ev_child){expr->list[0], expr->list[0]->width, false};
    return fr->stage == 0;
}

static bool child_select(const ev_frame *fr, ev_child *out) {
    const tb_expr *expr = fr->expr;
    const tb_expr *side = fr->stage == 0 ? expr->a : expr->b;
    if (expr->kind == E_BITSEL && fr->stage == 1) {
        side = expr->b;
    }
    *out = (ev_child){side, side->width, false};
    return expr->kind == E_BITSEL ? fr->stage < 2 : fr->stage == 0;
}

/* ?: evaluates the condition, then the side it picks, or both sides when it is x or z. */
static bool child_cond(const ev_frame *fr, ev_child *out) {
    const tb_expr *expr = fr->expr;
    if (fr->stage == 0) {
        *out = (ev_child){expr->a, expr->a->width, false};
        return true;
    }
    bool second = fr->stage == 2;
    *out = (ev_child){fr->cond == ODIN3_W4_0 || second ? expr->c : expr->b, fr->width, fr->sgn};
    return fr->stage == 1 || (second && fr->cond != ODIN3_W4_0 && fr->cond != ODIN3_W4_1);
}

/* Sets *out to the next child of fr; false when all are done. */
static bool next_child(const ev_frame *fr, ev_child *out) {
    const tb_expr *expr = fr->expr;
    switch (expr->kind) {
    case E_BITSEL:
    case E_PART:
    case E_MEM:
        return child_select(fr, out);
    case E_HIERMEM:
        if (expr->a == NULL) {
            return false;
        }
        *out = (ev_child){expr->a, expr->a->width, false};
        return fr->stage == 0;
    case E_CONCAT:
    case E_REPL:
        if (fr->stage >= expr->n * fr->reps) {
            return false;
        }
        *out = (ev_child){expr->list[fr->stage / fr->reps], expr->list[fr->stage / fr->reps]->width,
                          false};
        return true;
    case E_UN:
        *out = (ev_child){expr->a, fr->cls == C_UN_ARITH ? fr->width : expr->a->width,
                          fr->cls == C_UN_ARITH && fr->sgn};
        return fr->stage == 0;
    case E_BIN:
        return child_bin(fr, out);
    case E_COND:
        return child_cond(fr, out);
    case E_SYS:
        return child_sys(fr, out);
    default:
        return false;
    }
}

/* A child's value (width bits) reaches its parent frame. */
static void deliver(ev_frame *fr, const tbv *value, uint32_t width) {
    const tb_expr *expr = fr->expr;
    if (expr->kind == E_CONCAT || expr->kind == E_REPL) {
        uint32_t item = expr->list[fr->stage / fr->reps]->width;
        fr->placed += item;
        tb_field_put((tb_planes){fr->val[0].v, fr->val[0].u, fr->width},
                     (tb_field){expr->width - fr->placed, item}, value);
    } else if (expr->kind == E_COND && fr->stage == 0) {
        fr->cond = odin3_w4_truth((odin3_w4c){value->v, value->u}, width);
    } else if (expr->kind == E_COND) {
        tbv_copy(&fr->val[fr->stage - 1U], value, width);
    } else {
        tbv_copy(&fr->val[fr->stage & 1U], value, width);
    }
    fr->stage++;
}

/* --- finishing a node whose children are done: the result goes to fr->val[0] --- */

/* An index value as a number: UINT64_MAX when it has x or z bits (reads x, writes are dropped). */
static uint64_t index_of(const tbv *val, uint32_t width) {
    uint64_t idx = 0;
    return tbv_u64(val, width, &idx) ? idx : UINT64_MAX;
}

static void finish_random(tb_ev *ev, ev_frame *fr) {
    const tb_expr *expr = fr->expr;
    int32_t seed = ev->seed;
    if (expr->n > 0) {
        seed = (int32_t)(uint32_t)fr->val[0].v[0];
    }
    int32_t val = tb_random(&seed);
    if (expr->n > 0) {
        const tb_expr *index = lv_index(expr->list[0]);
        uint64_t idx = index != NULL ? index_of(&fr->val[1], index->width) : 0;
        write_number(ev, (uint64_t)(uint32_t)seed, expr->list[0], idx);
    } else {
        ev->seed = seed;
    }
    tbv_set_u64((uint64_t)(uint32_t)val, &fr->val[0], TB_INT_BITS);
    extend(&fr->val[0], TB_INT_BITS, fr->width, fr->sgn);
}

static uint64_t finish_fopen(tb_ev *ev, const tb_expr *expr) {
    if (ev->const_only || expr->n < 1 || expr->list[0]->kind != E_STR) {
        ev->failed = true;
        ev->why = "$fopen needs a constant file name string in a procedural statement";
        return 0;
    }
    const char *mode = expr->n > 1 && expr->list[1]->kind == E_STR ? expr->list[1]->str : "w";
    for (uint32_t i = 0; i < TB_MAX_FILES; i++) {
        if (ev->tb->files[i] == NULL) {
            ev->tb->files[i] = fopen(expr->list[0]->str, mode);
            return ev->tb->files[i] != NULL ? i + TB_FD_BASE : 0U;
        }
    }
    return 0;
}

/* --- plusargs --- */

/* The plusarg text without its optional leading '+'. */
static const char *plusarg_text(const char *arg) {
    return arg[0] == '+' ? arg + 1 : arg;
}

/* The first plusarg that starts with name (len bytes), or NULL. */
static const char *plusarg_find(const tb_ev *ev, const char *name, size_t len) {
    for (uint32_t i = 0; i < ev->tb->n_plusargs; i++) {
        const char *arg = plusarg_text(ev->tb->plusargs[i]);
        if (strncmp(arg, name, len) == 0) {
            return arg;
        }
    }
    return NULL;
}

enum { BASE_BIN = 2, BASE_OCT = 8, BASE_DEC = 10, BASE_HEX = 16, BYTE_BITS = 8 };

/* The numeric base of conversion conv (case-insensitive), or 0 for one that is not numeric. */
static int plusarg_base(char conv) {
    switch (conv) {
    case 'h':
    case 'H':
    case 'x':
    case 'X':
        return BASE_HEX;
    case 'b':
    case 'B':
        return BASE_BIN;
    case 'o':
    case 'O':
        return BASE_OCT;
    case 'd':
    case 'D':
        return BASE_DEC;
    default:
        return 0;
    }
}

/* True when text is an optional '-' then digits only in base (no space, '+' or 0x/0b prefix). */
static bool plusarg_digits_ok(const char *text, int base) {
    const char *digits = text[0] == '-' ? text + 1 : text;
    if (!isxdigit((unsigned char)digits[0])) {
        return false;
    }
    bool prefixed =
        digits[0] == '0' && ((base == BASE_HEX && (digits[1] == 'x' || digits[1] == 'X')) ||
                             (base == BASE_BIN && (digits[1] == 'b' || digits[1] == 'B')));
    return !prefixed;
}

/* Parses text as a value in format conversion conv (d, h or x, b, o, s, any case); false when it
 * is not one. A string keeps its first 8 bytes, the first in the most significant byte used. */
static bool plusarg_parse(char conv, const char *text, uint64_t *val) {
    if (conv == 's' || conv == 'S') {
        *val = 0;
        for (size_t i = 0; i < strlen(text) && i < sizeof(uint64_t); i++) {
            *val = (*val << BYTE_BITS) | (uint8_t)text[i];
        }
        return true;
    }
    int base = plusarg_base(conv);
    if (base == 0 || !plusarg_digits_ok(text, base)) {
        return false;
    }
    char *end = NULL;
    bool neg = text[0] == '-';
    errno = 0;
    uint64_t mag = strtoull(neg ? text + 1 : text, &end, base);
    if (errno == ERANGE || *end != '\0') {
        return false;
    }
    *val = neg ? (uint64_t)0 - mag : mag;
    return true;
}

/* $value$plusargs("name=%d", var): 1 and var set when a plusarg starts with the text before the
 * '%' and its rest parses in that conversion, else 0 and var unchanged. */
static uint64_t finish_value_plusargs(tb_ev *ev, const ev_frame *fr) {
    const tb_expr *expr = fr->expr;
    const char *fmt = expr->list[0]->str;
    const char *pct = strchr(fmt, '%');
    if (ev->const_only || pct == NULL) {
        ev->failed = true;
        ev->why = "$value$plusargs needs a format with a % conversion in a procedural statement";
        return 0;
    }
    const char *conv = pct + 1;
    while (*conv == '0') {
        conv++;
    }
    const char *arg = plusarg_find(ev, fmt, (size_t)(pct - fmt));
    uint64_t val = 0;
    if (arg == NULL || !plusarg_parse(*conv, arg + (pct - fmt), &val)) {
        return 0;
    }
    const tb_expr *index = lv_index(expr->list[1]);
    uint64_t idx = index != NULL ? index_of(&fr->val[0], index->width) : 0;
    write_number(ev, val, expr->list[1], idx);
    return 1;
}

static uint64_t finish_test_plusargs(tb_ev *ev, const tb_expr *expr) {
    if (ev->const_only || expr->list[0]->kind != E_STR) {
        ev->failed = true;
        ev->why = "$test$plusargs needs a constant string in a procedural statement";
        return 0;
    }
    return plusarg_find(ev, expr->list[0]->str, strlen(expr->list[0]->str)) != NULL ? 1U : 0U;
}

/* $clog2 of a known value: the bits needed to count up to it (0 for 0 and 1). */
static uint64_t clog2_of(const tbv *val, uint32_t width) {
    uint32_t high = 0;
    uint32_t ones = 0;
    for (uint32_t bit = 0; bit < width; bit++) {
        if (odin3_word_bit(val->v, bit)) {
            high = bit;
            ones++;
        }
    }
    if (ones == 0) {
        return 0;
    }
    return ones == 1 ? high : high + 1U;
}

/* A known number result, extended to the frame's width. */
static void set_number(ev_frame *fr, uint64_t num) {
    tbv_set_u64(num, &fr->val[0], fr->width);
}

static void finish_sys(tb_ev *ev, ev_frame *fr) {
    const tb_expr *expr = fr->expr;
    switch (fr->cls) {
    case C_TIME:
        ev->failed = ev->failed || ev->const_only;
        set_number(fr, ev->time);
        break;
    case C_RANDOM:
        finish_random(ev, fr);
        break;
    case C_FOPEN:
        set_number(fr, finish_fopen(ev, expr));
        break;
    case C_TEST_PLUSARGS:
        set_number(fr, finish_test_plusargs(ev, expr));
        break;
    case C_VALUE_PLUSARGS:
        set_number(fr, finish_value_plusargs(ev, fr));
        break;
    case C_CLOG2:
        if (tbv_known(&fr->val[0], expr->list[0]->width)) {
            set_number(fr, clog2_of(&fr->val[0], expr->list[0]->width));
        } else {
            tbv_fill(ODIN3_W4_X, &fr->val[0], fr->width);
        }
        break;
    default: /* $signed / $unsigned */
        extend(&fr->val[0], expr->list[0]->width, fr->width, fr->sgn && expr->sgn);
        break;
    }
}

/* && and || on the operands' truth: 0 && anything is 0, 1 || anything is 1, else x when either is
 * unknown. */
static uint32_t logic_code(const ev_frame *fr) {
    const tb_expr *expr = fr->expr;
    uint32_t lhs = odin3_w4_truth((odin3_w4c){fr->val[0].v, fr->val[0].u}, expr->a->width);
    uint32_t rhs = odin3_w4_truth((odin3_w4c){fr->val[1].v, fr->val[1].u}, expr->b->width);
    uint32_t wins = op_is(expr, "&&") ? (uint32_t)ODIN3_W4_0 : (uint32_t)ODIN3_W4_1;
    if (lhs == wins || rhs == wins) {
        return wins;
    }
    return lhs == rhs ? lhs : (uint32_t)ODIN3_W4_X;
}

static void finish_bin(ev_frame *fr) {
    const tb_expr *expr = fr->expr;
    switch (fr->cls) {
    case C_LOGIC:
        tbv_set_code(logic_code(fr), &fr->val[0], fr->width);
        break;
    case C_CMP: {
        tb_binop bo = {&fr->val[0], &fr->val[1], max_u32(expr->a->width, expr->b->width),
                       expr->a->sgn && expr->b->sgn};
        tbv_set_code(tb_op_compare(expr->op, &bo), &fr->val[0], fr->width);
        break;
    }
    case C_SHIFT: {
        tb_shiftop so = {&fr->val[0], &fr->val[1], fr->width, expr->b->width, fr->sgn};
        tb_op_shift(expr->op, &so);
        break;
    }
    case C_POW: {
        tb_powop po = {&fr->val[0], &fr->val[1], fr->width, expr->b->width, fr->sgn, expr->b->sgn};
        tb_op_pow(&po);
        break;
    }
    default: {
        tb_binop bo = {&fr->val[0], &fr->val[1], fr->width, fr->sgn};
        tb_op_arith(expr->op, &bo);
        break;
    }
    }
}

/* A reduction or ! of the operand (its own width): one bit, 0 1 or x. */
static uint32_t reduce_code(const ev_frame *fr) {
    const tb_expr *expr = fr->expr;
    odin3_w4c operand = {fr->val[0].v, fr->val[0].u};
    uint32_t width = expr->a->width;
    uint32_t code = 0;
    switch (expr->op[0] == '~' ? expr->op[1] : expr->op[0]) {
    case '!':
        code = tb_code_not(odin3_w4_truth(operand, width));
        break;
    case '&':
        code = odin3_w4_reduce_and(operand, width);
        break;
    case '|':
        code = odin3_w4_reduce_or(operand, width);
        break;
    default:
        code = odin3_w4_reduce_xor(operand, width);
        break;
    }
    return expr->op[0] == '~' ? tb_code_not(code) : code;
}

static void finish_un(ev_frame *fr) {
    if (fr->cls == C_UN_ARITH) {
        tb_op_unary(fr->expr->op, &fr->val[0], fr->width);
        return;
    }
    tbv_set_code(reduce_code(fr), &fr->val[0], fr->width);
}

/* base[index] (one bit) or base[msb:lsb]: an unknown or out-of-range bit index reads x. */
static void finish_select(ev_frame *fr) {
    const tb_expr *expr = fr->expr;
    uint32_t base = expr->a->width;
    if (expr->kind == E_PART) {
        tb_cplanes src = {fr->val[0].v, fr->val[0].u, base};
        tb_field_get(&fr->val[1], src, (tb_field){expr->aux, expr->width});
        tbv_copy(&fr->val[0], &fr->val[1], expr->width);
        extend(&fr->val[0], expr->width, fr->width, false);
        return;
    }
    uint64_t idx = index_of(&fr->val[1], expr->b->width);
    uint32_t code = idx < base ? tbv_code(&fr->val[0], (uint32_t)idx) : (uint32_t)ODIN3_W4_X;
    tbv_set_code(code, &fr->val[0], fr->width);
}

/* An array word: an unknown or out-of-range address reads x. */
static void finish_mem(const tb_ev *ev, ev_frame *fr) {
    const tb_expr *expr = fr->expr;
    const tb_var *var = &ev->tb->vars[expr->var];
    uint64_t idx = index_of(&fr->val[0], expr->a->width);
    if (idx < var->depth) {
        tb_var_load(&fr->val[0], var, (uint32_t)idx);
    } else {
        tbv_fill(ODIN3_W4_X, &fr->val[0], expr->width);
    }
    extend(&fr->val[0], expr->width, fr->width, fr->sgn && expr->sgn);
}

/* A hierarchical read: the DUT signal as of its last settle. Not a constant expression. */
static void finish_hier(tb_ev *ev, ev_frame *fr) {
    const tb_expr *expr = fr->expr;
    if (ev->const_only) {
        ev->failed = true;
        tbv_zero(&fr->val[0], fr->width);
        return;
    }
    tb_inst_peek(&ev->tb->insts[expr->var], (uint32_t)expr->num, &fr->val[0]);
    extend(&fr->val[0], expr->width, fr->width, false);
}

/* A word of a DUT array by an evaluated index: an unknown or out-of-range index reads x. */
static void finish_hiermem(tb_ev *ev, ev_frame *fr) {
    const tb_expr *expr = fr->expr;
    if (ev->const_only || expr->a == NULL) {
        ev->failed = true;
        ev->why = expr->a == NULL ? "a whole DUT array used as a value (only $readmem takes one)"
                                  : ev->why;
        tbv_zero(&fr->val[0], fr->width);
        return;
    }
    uint64_t idx = index_of(&fr->val[0], expr->a->width);
    if (idx < expr->n && expr->ids[idx] == 0) {
        ev->failed = true;
        ev->why = "a word of a DUT array has no name (opt folded it to a constant)";
        tbv_zero(&fr->val[0], fr->width);
    } else if (idx < expr->n) {
        tb_inst_peek(&ev->tb->insts[expr->var], expr->ids[idx], &fr->val[0]);
    } else {
        tbv_fill(ODIN3_W4_X, &fr->val[0], expr->width);
    }
    extend(&fr->val[0], expr->width, fr->width, false);
}

/* A testbench array word, or a read of the DUT: one signal, or a word of an array. */
static void finish_word(tb_ev *ev, ev_frame *fr) {
    switch (fr->expr->kind) {
    case E_MEM:
        finish_mem(ev, fr);
        break;
    case E_HIER:
        finish_hier(ev, fr);
        break;
    default:
        finish_hiermem(ev, fr);
        break;
    }
}

static void finish_leaf(tb_ev *ev, ev_frame *fr) {
    const tb_expr *expr = fr->expr;
    if (expr->kind == E_NUM) {
        size_t bytes = (size_t)odin3_word_limbs(expr->width) * sizeof(uint64_t);
        memcpy(fr->val[0].v, expr->nv, bytes);
        memcpy(fr->val[0].u, expr->nu, bytes);
        tb_resize rs = {expr->width, fr->width, fr->sgn, expr->xfill};
        tbv_extend(&fr->val[0], &rs);
    } else {
        tb_var_load(&fr->val[0], &ev->tb->vars[expr->var], 0);
        extend(&fr->val[0], expr->width, fr->width, fr->sgn);
        ev->failed = ev->failed || (ev->const_only && !ev->tb->vars[expr->var].is_param);
    }
}

/* ?: with an unknown condition merges the two sides. */
static void finish_cond(ev_frame *fr) {
    if (fr->cond != ODIN3_W4_0 && fr->cond != ODIN3_W4_1) {
        tb_op_merge(&fr->val[0], &fr->val[1], fr->width);
    }
}

static void finish(tb_ev *ev, ev_frame *fr) {
    switch (fr->expr->kind) {
    case E_NUM:
    case E_VAR:
        finish_leaf(ev, fr);
        break;
    case E_MEM:
    case E_HIER:
    case E_HIERMEM:
        finish_word(ev, fr);
        break;
    case E_BITSEL:
    case E_PART:
        finish_select(fr);
        break;
    case E_CONCAT:
    case E_REPL:
        break; /* built in val[0] by deliver */
    case E_UN:
        finish_un(fr);
        break;
    case E_BIN:
        finish_bin(fr);
        break;
    case E_COND:
        finish_cond(fr);
        break;
    case E_SYS:
        finish_sys(ev, fr);
        break;
    default:
        ev->failed = true;
        tbv_zero(&fr->val[0], fr->width);
        break;
    }
}

void tb_eval(tb_ev *ev, const tb_expr *expr, uint32_t width, bool sgn, tbv *out) {
    ev_stack stk = {.cap = EV_LOCAL_FRAMES};
    stk.frames = stk.local;
    tbv_zero(out, max_u32(width, expr->width));
    if (!stack_push(&stk, expr, width, sgn)) {
        ev->failed = true;
        return;
    }
    while (stk.top > 0) {
        ev_frame *fr = &stk.frames[stk.top - 1U];
        ev_child child;
        if (next_child(fr, &child)) {
            if (!stack_push(&stk, child.expr, child.width, child.sgn)) {
                ev->failed = true;
                break;
            }
            continue;
        }
        finish(ev, fr);
        stk.top--;
        if (stk.top == 0) {
            tbv_copy(out, &fr->val[0], fr->width);
        } else {
            deliver(&stk.frames[stk.top - 1U], &fr->val[0], fr->width);
        }
    }
    if (stk.frames != stk.local) {
        free(stk.frames);
    }
}

void tb_eval_assign(tb_ev *ev, const tb_expr *expr, uint32_t width, tbv *out) {
    uint32_t ctx = max_u32(width, expr->width);
    tb_eval(ev, expr, ctx, expr->sgn, out);
    extend(out, ctx, width, false);
}

uint32_t tb_eval_truth(tb_ev *ev, const tb_expr *expr) {
    tbv val;
    tb_eval(ev, expr, expr->width, false, &val);
    return odin3_w4_truth((odin3_w4c){val.v, val.u}, expr->width);
}

bool tb_eval_u64(tb_ev *ev, const tb_expr *expr, uint64_t *out) {
    tbv val;
    tb_eval(ev, expr, expr->width, false, &val);
    *out = 0;
    return tbv_u64(&val, expr->width, out);
}

static uint64_t index_value(tb_ev *ev, const tb_expr *lv) {
    const tb_expr *index = lv_index(lv);
    uint64_t idx = 0;
    if (index == NULL) {
        return 0;
    }
    return tb_eval_u64(ev, index, &idx) ? idx : UINT64_MAX;
}

uint32_t tb_wr_resolve(tb_ev *ev, const tb_expr *lv, const tbv *val, tb_wr *out) {
    /* A concatenation takes its parts from the right; nested ones flatten in place, so the leaves
     * are taken last to first from a worklist (each leaf is at least one bit). */
    const tb_expr *work[2 * TB_WR_MAX];
    uint32_t top = 0;
    uint32_t count = 0;
    uint32_t off = 0;
    work[top++] = lv;
    while (top > 0) {
        const tb_expr *cur = work[--top];
        if (cur->kind != E_CONCAT) {
            if (count == TB_WR_MAX) {
                ev->failed = true;
                return 0;
            }
            tb_wr *wr = &out[count++];
            wr->leaf = cur;
            tb_field_get(&wr->val, (tb_cplanes){val->v, val->u, lv->width},
                         (tb_field){off, cur->width});
            wr->idx = index_value(ev, cur);
            off += cur->width;
            continue;
        }
        if (top + cur->n > 2 * TB_WR_MAX) {
            ev->failed = true;
            return 0;
        }
        for (uint32_t i = 0; i < cur->n; i++) {
            work[top++] = cur->list[i];
        }
    }
    return count;
}

void tb_wr_apply(tb_ev *ev, const tb_wr *wr) {
    write_leaf(ev, wr->leaf, &wr->val, wr->idx);
}

void tb_write(tb_ev *ev, const tb_expr *lv, const tbv *val) {
    if (lv->kind != E_CONCAT) { /* one leaf: no split */
        write_leaf(ev, lv, val, index_value(ev, lv));
        return;
    }
    tb_wr wrs[TB_WR_MAX];
    uint32_t count = tb_wr_resolve(ev, lv, val, wrs);
    for (uint32_t i = 0; i < count; i++) {
        tb_wr_apply(ev, &wrs[i]);
    }
}

bool tb_const(tb_module *tb, const tb_expr *expr, int64_t *out) {
    tb_ev ev = {tb, 0, 0, true, false, NULL};
    tbv val;
    uint32_t width = max_u32(expr->width, TB_TIME_BITS);
    tb_eval(&ev, expr, width, expr->sgn, &val);
    *out = (int64_t)val.v[0];
    return !ev.failed && tbv_known(&val, width);
}

bool tb_const_val(tb_module *tb, const tb_expr *expr, uint32_t width, tbv *out) {
    tb_ev ev = {tb, 0, 0, true, false, NULL};
    tb_eval_assign(&ev, expr, width, out);
    return !ev.failed;
}

bool tb_forced(const tb_module *tb, int var) {
    return tb->force_on != NULL && tb->force_on[var];
}

bool tb_drive(tb_ev *ev, uint32_t drv, const tbv *val) {
    tb_module *tb = ev->tb;
    tb_drv *slot = &tb->drvs[drv - 1U];
    const tb_var *var = &tb->vars[slot->var];
    tbv_copy(&slot->val, val, var->width);
    tbv res;
    tbv_fill(ODIN3_W4_Z, &res, var->width);
    for (uint32_t i = 0; i < tb->ndrvs; i++) {
        if (tb->drvs[i].var == slot->var) {
            const tbv *one = &tb->drvs[i].val;
            odin3_w4_bin bin = {{res.v, res.u}, {res.v, res.u}, {one->v, one->u}, var->width};
            odin3_w4_resolve(&bin);
        }
    }
    tbv old;
    tb_var_load(&old, var, 0);
    if (tbv_same(&old, &res, var->width) || tb_forced(tb, slot->var)) {
        return false;
    }
    tb_var_store(var, 0, &res);
    return true;
}
