/* tb_expr.c — testbench expression evaluation and lvalue writes (spec section 2: widths and signs
 * follow IEEE 1364-2005 5.4/5.5 for the supported operators; values are at most 64 bits).
 *
 * The repository forbids recursion, so tb_eval walks the expression tree with an explicit frame
 * stack: a frame names its node, the width and signedness it is evaluated in, how many children
 * are done and their values; a finished frame hands its value to the frame below. */
#include "sim/tb_internal.h"
#include "util/alloc.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t tb_mask(uint32_t width) {
    return width >= TB_MAX_WIDTH ? ~(uint64_t)0 : (((uint64_t)1 << width) - 1U);
}

static int64_t sx(uint64_t val, uint32_t width) {
    val &= tb_mask(width);
    if (width > 0 && width < TB_MAX_WIDTH && ((val >> (width - 1U)) & 1U) != 0) {
        val |= ~tb_mask(width);
    }
    return (int64_t)val;
}

/* val of `from` bits extended to width bits (sign-extended when sgn). */
static uint64_t ext(uint64_t val, uint32_t from, bool sgn, uint32_t width) {
    return (sgn ? (uint64_t)sx(val, from) : (val & tb_mask(from))) & tb_mask(width);
}

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

/* --- lvalue writes (no evaluation inside: indices arrive already evaluated) --- */
typedef struct bitfield {
    uint32_t lsb;
    uint32_t width;
} bitfield;

static void write_bits(tb_var *var, bitfield field, uint64_t val) {
    if (field.lsb >= TB_MAX_WIDTH) {
        return;
    }
    uint64_t mask = tb_mask(field.width) << field.lsb;
    var->val = ((var->val & ~mask) | ((val << field.lsb) & mask)) & tb_mask(var->width);
}

/* The index expression of a select lvalue, or NULL. */
static const tb_expr *lv_index(const tb_expr *lv) {
    if (lv->kind == E_BITSEL) {
        return lv->b;
    }
    return lv->kind == E_MEM ? lv->a : NULL;
}

static void write_leaf(tb_ev *ev, const tb_expr *lv, uint64_t val, uint64_t idx) {
    switch (lv->kind) {
    case E_VAR:
        ev->tb->vars[lv->var].val = val & tb_mask(lv->width);
        break;
    case E_MEM: {
        tb_var *var = &ev->tb->vars[lv->var];
        if (idx < var->depth) {
            var->mem[idx] = val & tb_mask(var->width);
        }
        break;
    }
    case E_BITSEL:
        write_bits(&ev->tb->vars[lv->a->var], (bitfield){(uint32_t)idx, 1}, val);
        break;
    case E_PART:
        write_bits(&ev->tb->vars[lv->a->var], (bitfield){lv->aux, lv->width}, val);
        break;
    default:
        ev->failed = true;
        break;
    }
}

/* --- evaluation --- */
typedef enum {
    C_PLAIN,
    C_LOGIC,
    C_CMP,
    C_SHIFT,
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
    uint32_t stage; /* children delivered so far */
    uint32_t reps;  /* concatenation: repeats per item */
    uint64_t acc;   /* concatenation accumulator */
    uint64_t val[2];
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

static bool stack_push(ev_stack *stk, const tb_expr *expr, uint32_t width, bool sgn) {
    if (stk->top == stk->cap) {
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
    }
    ev_frame *fr = &stk->frames[stk->top++];
    memset(fr, 0, sizeof *fr);
    fr->expr = expr;
    fr->width = max_u32(width, expr->width);
    fr->sgn = sgn;
    fr->cls = class_of(expr);
    fr->reps = expr->kind == E_REPL ? (uint32_t)expr->num : 1U;
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
    if (fr->cls == C_CMP) {
        out->width = max_u32(expr->a->width, expr->b->width);
        out->sgn = expr->a->sgn && expr->b->sgn;
    } else if (fr->cls == C_ARITH || (fr->cls == C_SHIFT && fr->stage == 0)) {
        out->width = fr->width;
        out->sgn = fr->sgn;
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

/* Sets *out to the next child of fr; false when all are done. */
static bool next_child(const ev_frame *fr, ev_child *out) {
    const tb_expr *expr = fr->expr;
    switch (expr->kind) {
    case E_BITSEL:
    case E_PART:
    case E_MEM:
        return child_select(fr, out);
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
        if (fr->stage == 0) {
            *out = (ev_child){expr->a, expr->a->width, false};
            return true;
        }
        *out = (ev_child){fr->val[0] != 0 ? expr->b : expr->c, fr->width, fr->sgn};
        return fr->stage == 1;
    case E_SYS:
        return child_sys(fr, out);
    default:
        return false;
    }
}

static void deliver(ev_frame *fr, uint64_t value) {
    const tb_expr *expr = fr->expr;
    if (expr->kind == E_CONCAT || expr->kind == E_REPL) {
        const tb_expr *item = expr->list[fr->stage / fr->reps];
        fr->acc = (item->width >= TB_MAX_WIDTH ? 0 : fr->acc << item->width) | value;
    } else {
        fr->val[fr->stage & 1U] = value;
    }
    fr->stage++;
}

/* --- finishing a node whose children are done --- */
static uint64_t finish_random(tb_ev *ev, const ev_frame *fr) {
    const tb_expr *expr = fr->expr;
    int32_t seed = ev->seed;
    if (expr->n > 0) {
        seed = (int32_t)fr->val[0];
    }
    int32_t val = tb_random(&seed);
    if (expr->n > 0) {
        write_leaf(ev, expr->list[0], (uint64_t)(uint32_t)seed, fr->val[1]);
    } else {
        ev->seed = seed;
    }
    return ext((uint64_t)(uint32_t)val, TB_INT_BITS, fr->sgn, fr->width);
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
    write_leaf(ev, expr->list[1], val, fr->val[0]);
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

static uint64_t finish_sys(tb_ev *ev, const ev_frame *fr) {
    const tb_expr *expr = fr->expr;
    switch (fr->cls) {
    case C_TIME:
        ev->failed = ev->failed || ev->const_only;
        return ev->time & tb_mask(fr->width);
    case C_RANDOM:
        return finish_random(ev, fr);
    case C_FOPEN:
        return finish_fopen(ev, expr);
    case C_TEST_PLUSARGS:
        return finish_test_plusargs(ev, expr);
    case C_VALUE_PLUSARGS:
        return finish_value_plusargs(ev, fr);
    case C_CLOG2: {
        uint64_t res = 0;
        while (res < TB_MAX_WIDTH && ((uint64_t)1 << res) < fr->val[0]) {
            res++;
        }
        return res & tb_mask(fr->width);
    }
    default: /* $signed / $unsigned */
        return ext(fr->val[0], expr->list[0]->width, fr->sgn && expr->sgn, fr->width);
    }
}

static uint64_t cmp_values(const tb_expr *expr, const ev_frame *fr) {
    uint32_t cw = max_u32(expr->a->width, expr->b->width);
    bool csg = expr->a->sgn && expr->b->sgn;
    uint64_t lhs = fr->val[0];
    uint64_t rhs = fr->val[1];
    bool less = csg ? sx(lhs, cw) < sx(rhs, cw) : lhs < rhs;
    bool equal = lhs == rhs;
    if (op_is(expr, "==") || op_is(expr, "===")) {
        return equal;
    }
    if (op_is(expr, "!=") || op_is(expr, "!==")) {
        return !equal;
    }
    if (op_is(expr, "<")) {
        return less;
    }
    if (op_is(expr, "<=")) {
        return less || equal;
    }
    if (op_is(expr, ">")) {
        return !less && !equal;
    }
    return !less;
}

static uint64_t shift_values(const tb_expr *expr, const ev_frame *fr) {
    uint64_t lhs = fr->val[0];
    uint64_t amt = fr->val[1];
    if (op_is(expr, "<<") || op_is(expr, "<<<")) {
        return amt >= TB_MAX_WIDTH ? 0 : (lhs << amt) & tb_mask(fr->width);
    }
    if (op_is(expr, ">>>") && fr->sgn) {
        int64_t sval = sx(lhs, fr->width);
        if (amt >= TB_MAX_WIDTH - 1U) {
            return sval < 0 ? tb_mask(fr->width) : 0;
        }
        return (uint64_t)(sval >> amt) & tb_mask(fr->width);
    }
    return amt >= TB_MAX_WIDTH ? 0 : lhs >> amt;
}

static uint64_t divmod(const ev_frame *fr, bool is_div) {
    uint64_t lhs = fr->val[0];
    uint64_t rhs = fr->val[1];
    if (rhs == 0) {
        return 0;
    }
    if (fr->sgn) {
        int64_t sl = sx(lhs, fr->width);
        int64_t sr = sx(rhs, fr->width);
        if (sr == -1) {
            return is_div ? (uint64_t)(-sl) & tb_mask(fr->width) : 0;
        }
        return (uint64_t)(is_div ? sl / sr : sl % sr) & tb_mask(fr->width);
    }
    return is_div ? lhs / rhs : lhs % rhs;
}

static uint64_t arith_values(const tb_expr *expr, const ev_frame *fr) {
    uint64_t lhs = fr->val[0];
    uint64_t rhs = fr->val[1];
    uint64_t res = 0;
    if (op_is(expr, "+")) {
        res = lhs + rhs;
    } else if (op_is(expr, "-")) {
        res = lhs - rhs;
    } else if (op_is(expr, "*")) {
        res = lhs * rhs;
    } else if (op_is(expr, "/") || op_is(expr, "%")) {
        res = divmod(fr, op_is(expr, "/"));
    } else if (op_is(expr, "&")) {
        res = lhs & rhs;
    } else if (op_is(expr, "|")) {
        res = lhs | rhs;
    } else if (op_is(expr, "^")) {
        res = lhs ^ rhs;
    } else {
        res = ~(lhs ^ rhs);
    }
    return res & tb_mask(fr->width);
}

static uint64_t finish_bin(const ev_frame *fr) {
    const tb_expr *expr = fr->expr;
    switch (fr->cls) {
    case C_LOGIC: {
        bool lhs = fr->val[0] != 0;
        bool rhs = fr->val[1] != 0;
        return op_is(expr, "&&") ? (lhs && rhs) : (lhs || rhs);
    }
    case C_CMP:
        return cmp_values(expr, fr);
    case C_SHIFT:
        return shift_values(expr, fr);
    default:
        return arith_values(expr, fr);
    }
}

static uint64_t finish_un(const ev_frame *fr) {
    const tb_expr *expr = fr->expr;
    uint64_t operand = fr->val[0];
    if (fr->cls == C_UN_ARITH) {
        uint64_t res = operand;
        if (op_is(expr, "-")) {
            res = (uint64_t)0 - operand;
        } else if (op_is(expr, "~")) {
            res = ~operand;
        }
        return res & tb_mask(fr->width);
    }
    uint64_t masked = operand & tb_mask(expr->a->width);
    unsigned ones = 0;
    for (uint64_t rest = masked; rest != 0; rest &= rest - 1U) {
        ones++;
    }
    bool res = false;
    switch (expr->op[0] == '~' ? expr->op[1] : expr->op[0]) {
    case '!':
        res = masked == 0;
        break;
    case '&':
        res = masked == tb_mask(expr->a->width);
        break;
    case '|':
        res = masked != 0;
        break;
    default:
        res = (ones & 1U) != 0;
        break;
    }
    if (expr->op[0] == '~') {
        res = !res;
    }
    return res ? 1U : 0U;
}

static uint64_t finish_select(const ev_frame *fr) {
    const tb_expr *expr = fr->expr;
    uint64_t sel = 0;
    if (expr->kind == E_PART) {
        sel = (fr->val[0] >> expr->aux) & tb_mask(expr->width);
    } else if (expr->kind == E_BITSEL) {
        sel = fr->val[1] < TB_MAX_WIDTH ? (fr->val[0] >> fr->val[1]) & 1U : 0U;
    }
    return sel;
}

static uint64_t finish(tb_ev *ev, const ev_frame *fr) {
    const tb_expr *expr = fr->expr;
    switch (expr->kind) {
    case E_NUM:
        return ext(expr->num, expr->width, fr->sgn, fr->width);
    case E_VAR:
        return ext(ev->tb->vars[expr->var].val, expr->width, fr->sgn, fr->width);
    case E_MEM: {
        const tb_var *var = &ev->tb->vars[expr->var];
        uint64_t sel = fr->val[0] < var->depth ? var->mem[fr->val[0]] : 0;
        return ext(sel, expr->width, fr->sgn && expr->sgn, fr->width);
    }
    case E_BITSEL:
    case E_PART:
        return ext(finish_select(fr), expr->width, fr->sgn && expr->sgn, fr->width);
    case E_CONCAT:
    case E_REPL:
        return fr->acc & tb_mask(fr->width);
    case E_UN:
        return finish_un(fr);
    case E_BIN:
        return finish_bin(fr);
    case E_COND:
        return fr->val[1];
    case E_SYS:
        return finish_sys(ev, fr);
    default:
        ev->failed = true;
        return 0;
    }
}

uint64_t tb_eval(tb_ev *ev, const tb_expr *expr, uint32_t width, bool sgn) {
    ev_stack stk = {.cap = EV_LOCAL_FRAMES};
    uint64_t result = 0;
    stk.frames = stk.local;
    if (!stack_push(&stk, expr, width, sgn)) {
        ev->failed = true;
        return 0;
    }
    while (stk.top > 0) {
        const ev_frame *fr = &stk.frames[stk.top - 1U];
        ev_child child;
        if (next_child(fr, &child)) {
            if (!stack_push(&stk, child.expr, child.width, child.sgn)) {
                ev->failed = true;
                break;
            }
            continue;
        }
        uint64_t value = finish(ev, fr);
        stk.top--;
        if (stk.top == 0) {
            result = value;
        } else {
            deliver(&stk.frames[stk.top - 1U], value);
        }
    }
    if (stk.frames != stk.local) {
        free(stk.frames);
    }
    return result;
}

uint64_t tb_eval_assign(tb_ev *ev, const tb_expr *expr, uint32_t width) {
    uint32_t ctx = max_u32(width, expr->width);
    return tb_eval(ev, expr, ctx, expr->sgn) & tb_mask(width);
}

static uint64_t index_value(tb_ev *ev, const tb_expr *lv) {
    const tb_expr *index = lv_index(lv);
    return index != NULL ? tb_eval(ev, index, index->width, false) : 0;
}

uint32_t tb_wr_resolve(tb_ev *ev, const tb_expr *lv, uint64_t val, tb_wr *out) {
    /* A concatenation takes its parts from the right; nested ones flatten in place, so the leaves
     * are taken last to first from a worklist (each leaf is at least one bit). */
    const tb_expr *work[TB_WR_MAX];
    uint32_t top = 0;
    uint32_t count = 0;
    work[top++] = lv;
    while (top > 0) {
        const tb_expr *cur = work[--top];
        if (cur->kind != E_CONCAT) {
            if (count == TB_WR_MAX) {
                ev->failed = true;
                return 0;
            }
            out[count++] = (tb_wr){cur, val, index_value(ev, cur)};
            val = cur->width >= TB_MAX_WIDTH ? 0 : val >> cur->width;
            continue;
        }
        if (top + cur->n > TB_WR_MAX) {
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
    write_leaf(ev, wr->leaf, wr->val, wr->idx);
}

void tb_write(tb_ev *ev, const tb_expr *lv, uint64_t val) {
    tb_wr wrs[TB_WR_MAX];
    uint32_t count = tb_wr_resolve(ev, lv, val, wrs);
    for (uint32_t i = 0; i < count; i++) {
        tb_wr_apply(ev, &wrs[i]);
    }
}

bool tb_const(tb_module *tb, const tb_expr *expr, int64_t *out) {
    tb_ev ev = {tb, 0, 0, true, false, NULL};
    uint64_t val = tb_eval(&ev, expr, TB_MAX_WIDTH, expr->sgn);
    *out =
        sx(val, max_u32(expr->width, 1U) < TB_MAX_WIDTH && !expr->sgn ? TB_MAX_WIDTH : expr->width);
    return !ev.failed;
}
