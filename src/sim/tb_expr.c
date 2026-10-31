/* tb_expr.c — testbench expression evaluation and lvalue writes (spec section 2: widths and signs
 * follow IEEE 1364-2005 5.4/5.5 for the supported operators; values are at most 64 bits). */
#include "sim/tb_internal.h"

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

/* val of `from` bits extended to w bits (sign-extended when sg). */
static uint64_t ext(uint64_t val, uint32_t from, bool sg, uint32_t w) {
    return (sg ? (uint64_t)sx(val, from) : (val & tb_mask(from))) & tb_mask(w);
}

static bool op_is(const tb_expr *e, const char *text) {
    return strcmp(e->op, text) == 0;
}

static uint32_t max_u32(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

int32_t tb_random(int32_t *seed) {
    /* Park-Miller minimal standard generator (see the spec: matched against iverilog in the gate).
     */
    int64_t s = *seed;
    s = s <= 0 ? 1 : s;
    s = (s * 16807) % 2147483647;
    *seed = (int32_t)s;
    return *seed;
}

static uint64_t eval_index(tb_ev *ev, const tb_expr *idx) {
    return tb_eval(ev, idx, idx->width, false);
}

static uint64_t eval_random(tb_ev *ev, const tb_expr *e, uint32_t w, bool sg) {
    int32_t seed = ev->seed;
    if (e->n > 0) {
        seed = (int32_t)tb_eval(ev, e->list[0], 32, false);
    }
    int32_t val = tb_random(&seed);
    if (e->n > 0) {
        tb_write(ev, e->list[0], (uint64_t)(uint32_t)seed);
    } else {
        ev->seed = seed;
    }
    return ext((uint64_t)(uint32_t)val, 32, sg, w);
}

static uint64_t eval_fopen(tb_ev *ev, const tb_expr *e) {
    ev->failed = ev->failed || ev->const_only || e->n < 1 || e->list[0]->kind != E_STR;
    if (ev->failed) {
        return 0;
    }
    const char *mode = e->n > 1 && e->list[1]->kind == E_STR ? e->list[1]->str : "w";
    for (uint32_t i = 0; i < TB_MAX_FILES; i++) {
        if (ev->tb->files[i] == NULL) {
            ev->tb->files[i] = fopen(e->list[0]->str, mode);
            return ev->tb->files[i] != NULL ? i + 3U : 0U;
        }
    }
    return 0;
}

static uint64_t eval_clog2(tb_ev *ev, const tb_expr *e, uint32_t w) {
    uint64_t val = tb_eval(ev, e->list[0], e->list[0]->width, false);
    uint64_t res = 0;
    while (res < TB_MAX_WIDTH && ((uint64_t)1 << res) < val) {
        res++;
    }
    return res & tb_mask(w);
}

static uint64_t eval_sys(tb_ev *ev, const tb_expr *e, uint32_t w, bool sg) {
    const char *name = e->str;
    if (strcmp(name, "$time") == 0) {
        ev->failed = ev->failed || ev->const_only;
        return ev->time & tb_mask(w);
    }
    if (strcmp(name, "$random") == 0 || strcmp(name, "$urandom") == 0) {
        return eval_random(ev, e, w, sg);
    }
    if (strcmp(name, "$fopen") == 0) {
        return eval_fopen(ev, e);
    }
    if (strcmp(name, "$clog2") == 0) {
        return eval_clog2(ev, e, w);
    }
    /* $signed / $unsigned */
    uint64_t val = tb_eval(ev, e->list[0], e->list[0]->width, false);
    return ext(val, e->list[0]->width, sg && e->sgn, w);
}

static uint64_t eval_cmp(tb_ev *ev, const tb_expr *e) {
    uint32_t cw = max_u32(e->a->width, e->b->width);
    bool csg = e->a->sgn && e->b->sgn;
    uint64_t av = tb_eval(ev, e->a, cw, csg);
    uint64_t bv = tb_eval(ev, e->b, cw, csg);
    int64_t as = sx(av, cw);
    int64_t bs = sx(bv, cw);
    bool lt = csg ? as < bs : av < bv;
    bool eq = av == bv;
    if (op_is(e, "==") || op_is(e, "===")) {
        return eq;
    }
    if (op_is(e, "!=") || op_is(e, "!==")) {
        return !eq;
    }
    if (op_is(e, "<")) {
        return lt;
    }
    if (op_is(e, "<=")) {
        return lt || eq;
    }
    return op_is(e, ">") ? (!lt && !eq) : !lt;
}

static uint64_t eval_shift(tb_ev *ev, const tb_expr *e, uint32_t w, bool sg) {
    uint64_t av = tb_eval(ev, e->a, w, sg);
    uint64_t amt = tb_eval(ev, e->b, e->b->width, false);
    if (op_is(e, "<<") || op_is(e, "<<<")) {
        return amt >= TB_MAX_WIDTH ? 0 : (av << amt) & tb_mask(w);
    }
    if (op_is(e, ">>>") && sg) {
        int64_t sv = sx(av, w);
        if (amt >= TB_MAX_WIDTH - 1U) {
            return sv < 0 ? tb_mask(w) : 0;
        }
        return (uint64_t)(sv >> amt) & tb_mask(w);
    }
    return amt >= TB_MAX_WIDTH ? 0 : av >> amt;
}

static uint64_t divmod(uint64_t av, uint64_t bv, uint32_t w, bool sg, bool is_div) {
    if (bv == 0) {
        return 0;
    }
    if (sg) {
        int64_t as = sx(av, w);
        int64_t bs = sx(bv, w);
        if (bs == -1) {
            return is_div ? (uint64_t)(-as) & tb_mask(w) : 0;
        }
        return (uint64_t)(is_div ? as / bs : as % bs) & tb_mask(w);
    }
    return is_div ? av / bv : av % bv;
}

static uint64_t eval_arith(tb_ev *ev, const tb_expr *e, uint32_t w, bool sg) {
    uint64_t av = tb_eval(ev, e->a, w, sg);
    uint64_t bv = tb_eval(ev, e->b, w, sg);
    uint64_t res = 0;
    if (op_is(e, "+")) {
        res = av + bv;
    } else if (op_is(e, "-")) {
        res = av - bv;
    } else if (op_is(e, "*")) {
        res = av * bv;
    } else if (op_is(e, "/") || op_is(e, "%")) {
        res = divmod(av, bv, w, sg, op_is(e, "/"));
    } else if (op_is(e, "&")) {
        res = av & bv;
    } else if (op_is(e, "|")) {
        res = av | bv;
    } else if (op_is(e, "^")) {
        res = av ^ bv;
    } else {
        res = ~(av ^ bv);
    }
    return res & tb_mask(w);
}

static uint64_t eval_bin(tb_ev *ev, const tb_expr *e, uint32_t w, bool sg) {
    if (op_is(e, "&&") || op_is(e, "||")) {
        bool av = tb_eval(ev, e->a, e->a->width, false) != 0;
        bool bv = tb_eval(ev, e->b, e->b->width, false) != 0;
        return op_is(e, "&&") ? (av && bv) : (av || bv);
    }
    static const char *const CMPS[] = {"==", "!=", "===", "!==", "<", "<=", ">", ">=", NULL};
    for (size_t i = 0; CMPS[i] != NULL; i++) {
        if (op_is(e, CMPS[i])) {
            return eval_cmp(ev, e);
        }
    }
    if (e->op[0] == '<' && e->op[1] == '<') {
        return eval_shift(ev, e, w, sg);
    }
    if (e->op[0] == '>' && e->op[1] == '>') {
        return eval_shift(ev, e, w, sg);
    }
    return eval_arith(ev, e, w, sg);
}

static uint64_t eval_un(tb_ev *ev, const tb_expr *e, uint32_t w, bool sg) {
    if (op_is(e, "+") || op_is(e, "-") || op_is(e, "~")) {
        uint64_t av = tb_eval(ev, e->a, w, sg);
        uint64_t res = op_is(e, "-") ? (uint64_t)0 - av : (op_is(e, "~") ? ~av : av);
        return res & tb_mask(w);
    }
    uint64_t av = tb_eval(ev, e->a, e->a->width, false) & tb_mask(e->a->width);
    uint64_t full = tb_mask(e->a->width);
    unsigned ones = 0;
    for (uint64_t t = av; t != 0; t &= t - 1U) {
        ones++;
    }
    bool res = false;
    switch (e->op[0] == '~' ? e->op[1] : e->op[0]) {
    case '!':
        res = av == 0;
        break;
    case '&':
        res = av == full;
        break;
    case '|':
        res = av != 0;
        break;
    default:
        res = (ones & 1U) != 0;
        break;
    }
    return (e->op[0] == '~' ? !res : res) ? 1U : 0U;
}

static uint64_t eval_concat(tb_ev *ev, const tb_expr *e) {
    uint64_t acc = 0;
    for (uint32_t i = 0; i < e->n; i++) {
        const tb_expr *item = e->list[i];
        uint32_t reps = e->kind == E_REPL ? (uint32_t)e->num : 1U;
        for (uint32_t r = 0; r < reps; r++) {
            acc = (item->width >= TB_MAX_WIDTH ? 0 : acc << item->width) |
                  tb_eval(ev, item, item->width, false);
        }
    }
    return acc;
}

static uint64_t eval_select(tb_ev *ev, const tb_expr *e) {
    if (e->kind == E_MEM) {
        const tb_var *var = &ev->tb->vars[e->var];
        uint64_t idx = eval_index(ev, e->a);
        return idx < var->depth ? var->mem[idx] : 0;
    }
    uint64_t base = tb_eval(ev, e->a, e->a->width, false);
    if (e->kind == E_PART) {
        return (base >> e->aux) & tb_mask(e->width);
    }
    uint64_t idx = eval_index(ev, e->b);
    return idx < TB_MAX_WIDTH ? (base >> idx) & 1U : 0U;
}

uint64_t tb_eval(tb_ev *ev, const tb_expr *e, uint32_t w, bool sg) {
    w = max_u32(w, e->width);
    switch (e->kind) {
    case E_NUM:
        return ext(e->num, e->width, sg, w);
    case E_VAR:
        return ext(ev->tb->vars[e->var].val, e->width, sg, w);
    case E_BITSEL:
    case E_PART:
    case E_MEM:
        return ext(eval_select(ev, e), e->width, sg && e->sgn, w);
    case E_CONCAT:
    case E_REPL:
        return eval_concat(ev, e) & tb_mask(w);
    case E_UN:
        return eval_un(ev, e, w, sg);
    case E_BIN:
        return eval_bin(ev, e, w, sg);
    case E_COND:
        return tb_eval(ev, e->a, e->a->width, false) != 0 ? tb_eval(ev, e->b, w, sg)
                                                          : tb_eval(ev, e->c, w, sg);
    case E_SYS:
        return eval_sys(ev, e, w, sg);
    default:
        ev->failed = true;
        return 0;
    }
}

uint64_t tb_eval_assign(tb_ev *ev, const tb_expr *e, uint32_t width) {
    uint32_t w = max_u32(width, e->width);
    return tb_eval(ev, e, w, e->sgn) & tb_mask(width);
}

static void write_bits(tb_var *var, uint32_t lsb, uint32_t width, uint64_t val) {
    if (lsb >= TB_MAX_WIDTH) {
        return;
    }
    uint64_t mask = tb_mask(width) << lsb;
    var->val = ((var->val & ~mask) | ((val << lsb) & mask)) & tb_mask(var->width);
}

void tb_write(tb_ev *ev, const tb_expr *lv, uint64_t val) {
    switch (lv->kind) {
    case E_VAR:
        ev->tb->vars[lv->var].val = val & tb_mask(lv->width);
        break;
    case E_MEM: {
        tb_var *var = &ev->tb->vars[lv->var];
        uint64_t idx = eval_index(ev, lv->a);
        if (idx < var->depth) {
            var->mem[idx] = val & tb_mask(var->width);
        }
        break;
    }
    case E_BITSEL:
        write_bits(&ev->tb->vars[lv->a->var], (uint32_t)eval_index(ev, lv->b), 1, val);
        break;
    case E_PART:
        write_bits(&ev->tb->vars[lv->a->var], lv->aux, lv->width, val);
        break;
    case E_CONCAT:
        for (uint32_t i = lv->n; i > 0; i--) {
            tb_write(ev, lv->list[i - 1U], val);
            val = lv->list[i - 1U]->width >= TB_MAX_WIDTH ? 0 : val >> lv->list[i - 1U]->width;
        }
        break;
    default:
        ev->failed = true;
        break;
    }
}

bool tb_const(tb_module *tb, const tb_expr *e, int64_t *out) {
    tb_ev ev = {tb, 0, 0, true, false};
    uint64_t val = tb_eval(&ev, e, TB_MAX_WIDTH, e->sgn);
    *out = sx(val, max_u32(e->width, 1U) < TB_MAX_WIDTH && !e->sgn ? TB_MAX_WIDTH : e->width);
    return !ev.failed;
}
