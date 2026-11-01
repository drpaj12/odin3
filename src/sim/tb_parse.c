/* tb_parse.c — the testbench parser: module items, statements, expressions (spec section 2).
 *
 * The repository forbids recursion, so nesting is kept on explicit stacks: expressions by an
 * operator-precedence parser (operators, open parentheses, argument lists, concatenations, selects
 * and conditionals sit on one context stack, finished operands on a value stack), statements by a
 * stack of open compound statements. */
#include "sim/tb_internal.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdlib.h>
#include <string.h>

enum {
    MSG_LEN = 64,
    ARRAY_MAX_LOG2 = 20,
    UNARY_ARITH_COUNT = 3, /* + - ~ come first in the unary table */
    PREC_LOGIC_AND = 2,
    PREC_EQUALITY = 6,
    PREC_RELATION = 7,
    PREC_SHIFT = 8,
    CASE_MAX_ITEMS = 64,
    BLOCK_CAP0 = 8,
    INST_CONNS_MAX = 16,
    VARS_CAP0 = 32,
    OP_KEEP = 3, /* operator characters kept in tb_expr.op */
    MSG_EXTRA = 32,
    CHAR_BITS = 8
};

typedef struct parser {
    tb_module *tb;
    const tb_tok *toks;
    uint32_t pos;
    const char *file;
    bool failed;
    size_t efloor;     /* expression engine: context stack height at entry */
    bool primary_only; /* expression engine: stop after one primary */
    odin3_vec ents;    /* xent: open expression contexts */
    odin3_vec vals;    /* tb_expr *: finished operands */
    odin3_vec frames;  /* sframe: open compound statements */
} parser;

static const tb_tok *cur(const parser *pr) {
    return &pr->toks[pr->pos];
}

static bool fail(parser *pr, const char *what) {
    if (!pr->failed) {
        tb_error(pr->file, cur(pr)->line, what);
    }
    pr->failed = true;
    return false;
}

static bool is_op(const parser *pr, const char *op) {
    return cur(pr)->kind == TK_OP && strcmp(cur(pr)->text, op) == 0;
}

static bool is_kw(const parser *pr, const char *kw) {
    return cur(pr)->kind == TK_ID && strcmp(cur(pr)->text, kw) == 0;
}

static bool accept_op(parser *pr, const char *op) {
    if (is_op(pr, op)) {
        pr->pos++;
        return true;
    }
    return false;
}

static bool accept_kw(parser *pr, const char *kw) {
    if (is_kw(pr, kw)) {
        pr->pos++;
        return true;
    }
    return false;
}

static bool expect_op(parser *pr, const char *op) {
    if (accept_op(pr, op)) {
        return true;
    }
    char msg[TB_NAME + MSG_EXTRA];
    (void)snprintf(msg, sizeof msg, "expected `%s`", op);
    return fail(pr, msg);
}

static void *pnew(parser *pr, size_t size) {
    void *ptr = tb_alloc(pr->tb, size);
    if (ptr == NULL) {
        fail(pr, "out of memory");
    }
    return ptr;
}

/* A fresh array of count pointers (all NULL). */
static void *pnew_ptrs(parser *pr, size_t count) {
    return pnew(pr, count * sizeof(void *));
}

/* A copy of the first count pointers of src in parse memory. */
static void *pdup_ptrs(parser *pr, const void *src, size_t count) {
    void *dst = pnew_ptrs(pr, count + 1U);
    if (dst != NULL && count > 0) {
        memcpy(dst, src, count * sizeof(void *));
    }
    return dst;
}

static tb_expr *new_expr(parser *pr, tb_ekind kind) {
    tb_expr *expr = pnew(pr, sizeof *expr);
    if (expr != NULL) {
        expr->kind = kind;
        expr->line = cur(pr)->line;
    }
    return expr;
}

/* --- variables --- */
static int find_var(const parser *pr, const char *name) {
    for (uint32_t i = 0; i < pr->tb->nvars; i++) {
        if (strcmp(pr->tb->vars[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

typedef struct var_shape {
    uint32_t width;
    bool sgn;
    bool is_wire;
} var_shape;

static int add_var(parser *pr, const char *name, var_shape shape) {
    tb_module *tb = pr->tb;
    if (find_var(pr, name) >= 0) {
        fail(pr, "redeclared name");
        return -1;
    }
    if (tb->nvars == tb->cap_vars) {
        uint32_t cap = tb->cap_vars == 0 ? (uint32_t)VARS_CAP0 : tb->cap_vars * 2U;
        tb_var *grown = pnew(pr, (size_t)cap * sizeof *grown);
        if (grown == NULL) {
            return -1;
        }
        if (tb->nvars > 0) {
            memcpy(grown, tb->vars, (size_t)tb->nvars * sizeof *grown);
        }
        tb->vars = grown;
        tb->cap_vars = cap;
    }
    tb_var *var = &tb->vars[tb->nvars];
    memset(var, 0, sizeof *var);
    (void)snprintf(var->name, sizeof var->name, "%s", name);
    var->width = shape.width;
    var->sgn = shape.sgn;
    var->is_wire = shape.is_wire;
    return (int)tb->nvars++;
}

/* --- expressions: leaf helpers --- */
static tb_expr *parse_expr(parser *pr);

static bool const_int(parser *pr, int64_t *out) {
    const tb_expr *expr = parse_expr(pr);
    if (expr == NULL) {
        return false;
    }
    if (!tb_const(pr->tb, expr, out)) {
        return fail(pr, "non-constant expression where a constant is required");
    }
    return true;
}

/* `[h:0]` after the cursor is on `[`; width of the range. */
static bool parse_range(parser *pr, uint32_t *width) {
    int64_t high = 0;
    int64_t low = 0;
    if (!expect_op(pr, "[") || !const_int(pr, &high) || !expect_op(pr, ":") ||
        !const_int(pr, &low) || !expect_op(pr, "]")) {
        return false;
    }
    if (low != 0 || high < 0) {
        return fail(pr, "range other than [h:0]");
    }
    if (high >= TB_MAX_WIDTH) {
        return fail(pr, "signal wider than 64 bits");
    }
    *width = (uint32_t)high + 1U;
    return true;
}

/* Argument list of a system task statement: `(` expr {, expr} `)` or nothing. */
static bool parse_args(parser *pr, tb_expr *call) {
    tb_expr *list[TB_MAX_ARGS];
    uint32_t count = 0;
    if (accept_op(pr, "(") && !accept_op(pr, ")")) {
        do {
            if (count == TB_MAX_ARGS) {
                return fail(pr, "too many arguments");
            }
            list[count] = parse_expr(pr);
            if (list[count++] == NULL) {
                return false;
            }
        } while (accept_op(pr, ","));
        if (!expect_op(pr, ")")) {
            return false;
        }
    }
    call->list = (tb_expr **)pdup_ptrs(pr, (const void *)list, count);
    call->n = count;
    return call->list != NULL;
}

static const char *const SYS_EXPR[] = {"$time",     "$random", "$urandom", "$signed",
                                       "$unsigned", "$clog2",  "$fopen",   NULL};

/* Width and signedness of a system function call whose arguments are in place. */
static bool sys_finish(parser *pr, tb_expr *call) {
    bool is_cast = strcmp(call->str, "$signed") == 0 || strcmp(call->str, "$unsigned") == 0;
    if ((is_cast || strcmp(call->str, "$clog2") == 0) && call->n != 1) {
        return fail(pr, "wrong argument count");
    }
    call->width = strcmp(call->str, "$time") == 0 ? (uint32_t)TB_MAX_WIDTH : (uint32_t)TB_INT_BITS;
    call->sgn = strcmp(call->str, "$random") == 0 || strcmp(call->str, "$signed") == 0 ||
                strcmp(call->str, "$clog2") == 0;
    if (is_cast) {
        call->width = call->list[0]->width;
    }
    return true;
}

static tb_expr *parse_number(parser *pr) {
    tb_expr *expr = new_expr(pr, E_NUM);
    if (expr == NULL) {
        return NULL;
    }
    const tb_tok *tok = cur(pr);
    expr->num = tok->num;
    expr->width = tok->width == 0 ? (uint32_t)TB_INT_BITS : tok->width;
    expr->sgn = tok->is_signed;
    if (expr->width > TB_MAX_WIDTH) {
        fail(pr, "literal wider than 64 bits");
        return NULL;
    }
    expr->num &= tb_mask(expr->width);
    pr->pos++;
    return expr;
}

static void copy_op(char *dst, const char *op) {
    size_t len = strlen(op) < OP_KEEP ? strlen(op) : (size_t)OP_KEEP;
    memcpy(dst, op, len);
    dst[len] = '\0';
    if (strcmp(dst, "^~") == 0) {
        memcpy(dst, "~^", OP_KEEP);
    }
}

static const char *const UNOPS[] = {"+", "-",  "~",  "!",  "&",  "|",
                                    "^", "~&", "~|", "~^", "^~", NULL};

/* Index of the unary operator at the cursor, or -1. */
static int unary_index(const parser *pr) {
    for (size_t i = 0; UNOPS[i] != NULL; i++) {
        if (is_op(pr, UNOPS[i])) {
            return (int)i;
        }
    }
    return -1;
}

static int bin_prec(const char *op) {
    static const struct {
        const char *op;
        int prec;
    } TABLE[] = {{"||", 1}, {"&&", 2}, {"|", 3},   {"^", 4},   {"~^", 4},  {"^~", 4}, {"&", 5},
                 {"==", 6}, {"!=", 6}, {"===", 6}, {"!==", 6}, {"<", 7},   {"<=", 7}, {">", 7},
                 {">=", 7}, {"<<", 8}, {">>", 8},  {"<<<", 8}, {">>>", 8}, {"+", 9},  {"-", 9},
                 {"*", 10}, {"/", 10}, {"%", 10},  {NULL, 0}};
    for (size_t i = 0; TABLE[i].op != NULL; i++) {
        if (strcmp(TABLE[i].op, op) == 0) {
            return TABLE[i].prec;
        }
    }
    return 0;
}

static void set_bin_shape(tb_expr *expr) {
    int prec = bin_prec(expr->op);
    expr->sgn = false;
    if (prec == PREC_SHIFT) {
        expr->width = expr->a->width;
        expr->sgn = expr->a->sgn;
    } else if (prec == PREC_EQUALITY || prec == PREC_RELATION || prec <= PREC_LOGIC_AND) {
        expr->width = 1;
    } else {
        expr->width = expr->a->width > expr->b->width ? expr->a->width : expr->b->width;
        expr->sgn = expr->a->sgn && expr->b->sgn;
    }
}

/* --- expressions: the context stack --- */
typedef enum {
    X_BIN,    /* a binary operator waiting for its right operand */
    X_UN,     /* a unary operator waiting for its operand */
    X_COND_Q, /* `cond ? ...` waiting for `:` */
    X_COND_E, /* `cond ? a : ...` waiting for the else expression */
    X_PAREN,
    X_ARGS,   /* system function arguments */
    X_CONCAT, /* `{ ... }`, possibly a replication */
    X_INDEX,  /* `name[ ...` */
    X_LOW     /* `name[high : ...` */
} xkind;

typedef struct xent {
    xkind kind;
    tb_expr *node;
    int prec;    /* X_BIN: precedence; X_UN: index in UNOPS */
    size_t base; /* X_ARGS, X_CONCAT: height of the value stack before the first item */
    bool repl;   /* X_CONCAT: the replication count was consumed */
} xent;

typedef enum { XR_CONT, XR_DONE, XR_FAIL } xres;

static xent *push_ent(parser *pr, xkind kind, tb_expr *node) {
    xent *ent = odin3_vec_push(&pr->ents);
    if (ent == NULL) {
        fail(pr, "out of memory");
        return NULL;
    }
    ent->kind = kind;
    ent->node = node;
    ent->base = pr->vals.len;
    return ent;
}

static xent *top_ent(parser *pr) {
    return pr->ents.len > pr->efloor ? odin3_vec_at(&pr->ents, pr->ents.len - 1U) : NULL;
}

static bool push_val(parser *pr, tb_expr *expr) {
    tb_expr **slot = (tb_expr **)odin3_vec_push(&pr->vals);
    if (slot == NULL) {
        return fail(pr, "out of memory");
    }
    *slot = expr;
    return true;
}

static tb_expr *pop_val(parser *pr) {
    tb_expr *expr = *(tb_expr **)odin3_vec_at(&pr->vals, pr->vals.len - 1U);
    odin3_vec_pop(&pr->vals);
    return expr;
}

static void truncate_vec(odin3_vec *vec, size_t len) {
    while (vec->len > len) {
        odin3_vec_pop(vec);
    }
}

/* Operand of every pending unary operator on top of the stack. */
static bool reduce_unary(parser *pr) {
    for (xent *top = top_ent(pr); top != NULL && top->kind == X_UN; top = top_ent(pr)) {
        tb_expr *node = top->node;
        node->a = pop_val(pr);
        copy_op(node->op, UNOPS[top->prec]);
        bool arith = top->prec < UNARY_ARITH_COUNT;
        node->width = arith ? node->a->width : 1U;
        node->sgn = arith && node->a->sgn;
        odin3_vec_pop(&pr->ents);
        if (!push_val(pr, node)) {
            return false;
        }
    }
    return true;
}

/* Binary operators on top of the stack that bind at least as tight as prec. */
static bool reduce_binary(parser *pr, int prec) {
    for (xent *top = top_ent(pr); top != NULL && top->kind == X_BIN && top->prec >= prec;
         top = top_ent(pr)) {
        tb_expr *node = top->node;
        node->b = pop_val(pr);
        node->a = pop_val(pr);
        set_bin_shape(node);
        odin3_vec_pop(&pr->ents);
        if (!push_val(pr, node)) {
            return false;
        }
    }
    return true;
}

/* --- expressions: starting an operand --- */
static bool begin_sys(parser *pr, bool *want) {
    tb_expr *call = new_expr(pr, E_SYS);
    if (call == NULL) {
        return false;
    }
    call->str = pnew(pr, TB_NAME);
    if (call->str == NULL) {
        return false;
    }
    (void)snprintf(call->str, TB_NAME, "%s", cur(pr)->text);
    bool known = false;
    for (size_t i = 0; SYS_EXPR[i] != NULL; i++) {
        known = known || strcmp(SYS_EXPR[i], call->str) == 0;
    }
    pr->pos++;
    if (!known) {
        return fail(pr, "system function");
    }
    if (accept_op(pr, "(") && !accept_op(pr, ")")) {
        *want = true;
        return push_ent(pr, X_ARGS, call) != NULL;
    }
    call->list = (tb_expr **)pnew_ptrs(pr, 1);
    *want = false;
    return call->list != NULL && sys_finish(pr, call) && push_val(pr, call);
}

static bool begin_ident(parser *pr, bool *want) {
    int var = find_var(pr, cur(pr)->text);
    if (var < 0) {
        return fail(pr, "undeclared identifier (declare before use)");
    }
    tb_expr *expr = new_expr(pr, E_VAR);
    if (expr == NULL) {
        return false;
    }
    pr->pos++;
    expr->var = var;
    expr->width = pr->tb->vars[var].width;
    expr->sgn = pr->tb->vars[var].sgn;
    if (accept_op(pr, "[")) {
        *want = true;
        return push_ent(pr, X_INDEX, expr) != NULL;
    }
    *want = false;
    return push_val(pr, expr);
}

static bool begin_string(parser *pr) {
    tb_expr *expr = new_expr(pr, E_STR);
    if (expr == NULL) {
        return false;
    }
    expr->str = cur(pr)->str;
    expr->width = CHAR_BITS;
    pr->pos++;
    return push_val(pr, expr);
}

static bool begin_concat(parser *pr) {
    tb_expr *expr = new_expr(pr, E_CONCAT);
    if (expr == NULL) {
        return false;
    }
    pr->pos++;
    return push_ent(pr, X_CONCAT, expr) != NULL;
}

/* One step at a position that needs an operand. */
static xres step_operand(parser *pr, bool *want) {
    bool ok = true;
    int unary = unary_index(pr);
    const tb_tok *tok = cur(pr);
    *want = true;
    if (unary >= 0 && !(pr->primary_only && pr->ents.len == pr->efloor)) {
        tb_expr *node = new_expr(pr, E_UN);
        pr->pos++;
        xent *ent = node != NULL ? push_ent(pr, X_UN, node) : NULL;
        ok = ent != NULL;
        if (ok) {
            ent->prec = unary;
        }
    } else if (tok->kind == TK_NUM) {
        tb_expr *node = parse_number(pr);
        *want = false;
        ok = node != NULL && push_val(pr, node);
    } else if (tok->kind == TK_STR) {
        *want = false;
        ok = begin_string(pr);
    } else if (tok->kind == TK_SYS) {
        ok = begin_sys(pr, want);
    } else if (tok->kind == TK_ID) {
        ok = begin_ident(pr, want);
    } else if (accept_op(pr, "(")) {
        ok = push_ent(pr, X_PAREN, NULL) != NULL;
    } else if (is_op(pr, "{")) {
        ok = begin_concat(pr);
    } else {
        ok = fail(pr, "expression syntax");
    }
    return ok ? XR_CONT : XR_FAIL;
}

/* --- expressions: ending an operand --- */
static bool close_cond(parser *pr, xent *top) {
    tb_expr *node = top->node;
    node->c = pop_val(pr);
    node->width = node->b->width > node->c->width ? node->b->width : node->c->width;
    node->sgn = node->b->sgn && node->c->sgn;
    odin3_vec_pop(&pr->ents);
    return push_val(pr, node);
}

static bool open_cond(parser *pr, bool *want) {
    pr->pos++;
    tb_expr *node = new_expr(pr, E_COND);
    if (node == NULL) {
        return false;
    }
    node->a = pop_val(pr);
    *want = true;
    return push_ent(pr, X_COND_Q, node) != NULL;
}

static bool close_args(parser *pr, xent *top) {
    tb_expr *call = top->node;
    size_t count = pr->vals.len - top->base;
    call->list = (tb_expr **)pdup_ptrs(pr, odin3_vec_at(&pr->vals, top->base), count);
    call->n = (uint32_t)count;
    truncate_vec(&pr->vals, top->base);
    odin3_vec_pop(&pr->ents);
    return call->list != NULL && sys_finish(pr, call) && push_val(pr, call);
}

static bool start_replication(parser *pr, xent *top) {
    int64_t count = 0;
    const tb_expr *first = *(tb_expr **)odin3_vec_at(&pr->vals, top->base);
    if (!tb_const(pr->tb, first, &count) || count <= 0) {
        return fail(pr, "replication count");
    }
    top->node->num = (uint64_t)count;
    top->node->kind = E_REPL;
    top->repl = true;
    truncate_vec(&pr->vals, top->base);
    pr->pos++;
    return true;
}

static bool close_concat(parser *pr, xent *top) {
    tb_expr *node = top->node;
    size_t count = pr->vals.len - top->base;
    if (!expect_op(pr, "}") || (top->repl && !expect_op(pr, "}"))) {
        return false;
    }
    node->list = (tb_expr **)pdup_ptrs(pr, odin3_vec_at(&pr->vals, top->base), count);
    node->n = (uint32_t)count;
    if (!top->repl) {
        node->num = 1;
    }
    uint64_t width = 0;
    for (size_t i = 0; node->list != NULL && i < count; i++) {
        width += node->list[i]->width;
    }
    width *= node->num;
    truncate_vec(&pr->vals, top->base);
    odin3_vec_pop(&pr->ents);
    if (node->list == NULL) {
        return false;
    }
    if (width > TB_MAX_WIDTH) {
        return fail(pr, "concatenation wider than 64 bits");
    }
    node->width = (uint32_t)width;
    return push_val(pr, node);
}

/* A comma after count items of a concatenation or argument list: room for one more? */
static bool list_next(parser *pr, bool is_concat, size_t count) {
    if (is_concat && count + 1U >= TB_MAX_ARGS) {
        return fail(pr, "concatenation too long");
    }
    if (!is_concat && count == TB_MAX_ARGS) {
        return fail(pr, "too many arguments");
    }
    pr->pos++;
    return true;
}

/* The state of one concatenation or argument list after an item: continue or close. */
static xres step_list(parser *pr, xent *top, bool *want) {
    size_t count = pr->vals.len - top->base;
    bool is_concat = top->kind == X_CONCAT;
    if (is_concat && count == 1 && !top->repl && is_op(pr, "{")) {
        *want = true;
        return start_replication(pr, top) ? XR_CONT : XR_FAIL;
    }
    if (is_op(pr, ",")) {
        *want = true;
        return list_next(pr, is_concat, count) ? XR_CONT : XR_FAIL;
    }
    *want = false;
    if (is_concat) {
        return close_concat(pr, top) ? XR_CONT : XR_FAIL;
    }
    return expect_op(pr, ")") && close_args(pr, top) ? XR_CONT : XR_FAIL;
}

/* Builds the select node `name[index]` (bit or array word) from the base and the index. */
static tb_expr *make_select(parser *pr, tb_expr *base, tb_expr *index) {
    const tb_var *var = &pr->tb->vars[base->var];
    tb_expr *sel = new_expr(pr, var->depth > 0 ? E_MEM : E_BITSEL);
    if (sel == NULL) {
        return NULL;
    }
    sel->var = base->var;
    sel->a = var->depth > 0 ? index : base;
    sel->b = index;
    sel->width = var->depth > 0 ? var->width : 1U;
    sel->sgn = var->depth > 0 && var->sgn;
    return sel;
}

/* After `name[ index`: `]` closes a bit or word select, `:` starts a part select. */
static xres step_index(parser *pr, xent *top, bool *want) {
    tb_expr *base = top->node;
    const tb_var *var = &pr->tb->vars[base->var];
    tb_expr *index = pop_val(pr);
    tb_expr *sel = make_select(pr, base, index);
    if (sel == NULL) {
        return XR_FAIL;
    }
    if (var->depth == 0 && is_op(pr, ":")) {
        int64_t high = 0;
        if (!tb_const(pr->tb, index, &high)) {
            fail(pr, "part select (constants within the declared width)");
            return XR_FAIL;
        }
        pr->pos++;
        top->kind = X_LOW;
        top->node = sel;
        sel->num = (uint64_t)high;
        *want = true;
        return XR_CONT;
    }
    odin3_vec_pop(&pr->ents);
    *want = false;
    return expect_op(pr, "]") && push_val(pr, sel) ? XR_CONT : XR_FAIL;
}

/* After `name[high : low`: the part select is complete at `]`. */
static xres step_low(parser *pr, xent *top, bool *want) {
    tb_expr *sel = top->node;
    const tb_var *var = &pr->tb->vars[sel->var];
    int64_t low = 0;
    int64_t high = (int64_t)sel->num;
    const tb_expr *low_expr = pop_val(pr);
    if (!tb_const(pr->tb, low_expr, &low)) {
        fail(pr, "non-constant expression where a constant is required");
        return XR_FAIL;
    }
    if (high < low || low < 0 || high >= (int64_t)var->width) {
        fail(pr, "part select (constants within the declared width)");
        return XR_FAIL;
    }
    sel->kind = E_PART;
    sel->num = 0;
    sel->aux = (uint32_t)low;
    sel->width = (uint32_t)(high - low) + 1U;
    sel->sgn = false;
    odin3_vec_pop(&pr->ents);
    *want = false;
    return expect_op(pr, "]") && push_val(pr, sel) ? XR_CONT : XR_FAIL;
}

/* After a finished sub-expression with nothing left to reduce: act on the innermost context. */
static xres step_close(parser *pr, bool *want) {
    xent *top = top_ent(pr);
    *want = false;
    if (is_op(pr, "?")) {
        return open_cond(pr, want) ? XR_CONT : XR_FAIL;
    }
    if (top == NULL) {
        return XR_DONE;
    }
    switch (top->kind) {
    case X_COND_Q:
        if (!is_op(pr, ":")) {
            return expect_op(pr, ":") ? XR_CONT : XR_FAIL;
        }
        pr->pos++;
        top->node->b = pop_val(pr);
        top->kind = X_COND_E;
        *want = true;
        return XR_CONT;
    case X_COND_E:
        return close_cond(pr, top) ? XR_CONT : XR_FAIL;
    case X_PAREN:
        odin3_vec_pop(&pr->ents);
        return expect_op(pr, ")") ? XR_CONT : XR_FAIL;
    case X_ARGS:
    case X_CONCAT:
        return step_list(pr, top, want);
    case X_INDEX:
        return step_index(pr, top, want);
    default:
        return step_low(pr, top, want);
    }
}

/* The binary operator at the cursor (precedence prec): reduce tighter ones, push this one. */
static bool push_binary(parser *pr, int prec) {
    if (!reduce_binary(pr, prec)) {
        return false;
    }
    tb_expr *node = new_expr(pr, E_BIN);
    xent *ent = node != NULL ? push_ent(pr, X_BIN, node) : NULL;
    if (ent == NULL) {
        return false;
    }
    ent->prec = prec;
    copy_op(node->op, cur(pr)->text);
    pr->pos++;
    return true;
}

/* One step at a position just after an operand. */
static xres step_after(parser *pr, bool *want) {
    if (pr->primary_only && pr->ents.len == pr->efloor) {
        return XR_DONE;
    }
    if (!reduce_unary(pr)) {
        return XR_FAIL;
    }
    int prec = cur(pr)->kind == TK_OP ? bin_prec(cur(pr)->text) : 0;
    if (cur(pr)->kind == TK_OP && strcmp(cur(pr)->text, "**") == 0) {
        fail(pr, "operator **");
        return XR_FAIL;
    }
    if (prec > 0) {
        *want = true;
        return push_binary(pr, prec) ? XR_CONT : XR_FAIL;
    }
    if (!reduce_binary(pr, 0)) {
        return XR_FAIL;
    }
    return step_close(pr, want);
}

/* The expression engine: a full expression, or only a primary (no operators outside brackets). */
static tb_expr *parse_expr_mode(parser *pr, bool primary_only) {
    size_t floor_len = pr->ents.len;
    pr->efloor = floor_len;
    pr->primary_only = primary_only;
    size_t val_floor = pr->vals.len;
    bool want = true;
    xres res = XR_CONT;
    while (res == XR_CONT && !pr->failed) {
        res = want ? step_operand(pr, &want) : step_after(pr, &want);
    }
    tb_expr *result = NULL;
    if (res == XR_DONE && !pr->failed && pr->vals.len == val_floor + 1U) {
        result = pop_val(pr);
    }
    truncate_vec(&pr->ents, floor_len);
    truncate_vec(&pr->vals, val_floor);
    return result;
}

static tb_expr *parse_expr(parser *pr) {
    return parse_expr_mode(pr, false);
}

static tb_expr *parse_primary(parser *pr) {
    return parse_expr_mode(pr, true);
}

/* --- statements --- */
static tb_stmt *new_stmt(parser *pr, tb_skind kind) {
    tb_stmt *stmt = pnew(pr, sizeof *stmt);
    if (stmt != NULL) {
        stmt->kind = kind;
        stmt->line = cur(pr)->line;
    }
    return stmt;
}

/* A plain name that may be assigned: not a parameter, not an array. */
static bool assignable_var(const parser *pr, const tb_expr *name) {
    const tb_var *var = &pr->tb->vars[name->var];
    return !var->is_param && var->depth == 0;
}

/* A target is a reg or integer, a select of one, an array word, or a concatenation of those. A
 * concatenation holds at most TB_MAX_WIDTH leaves (each is one bit or more), so the worklist is
 * bounded. */
static bool valid_lvalue(const parser *pr, const tb_expr *lvalue) {
    enum { WORK_MAX = 2 * TB_MAX_WIDTH };
    const tb_expr *work[WORK_MAX];
    uint32_t top = 0;
    work[top++] = lvalue;
    while (top > 0) {
        const tb_expr *cur_lv = work[--top];
        if (cur_lv->kind == E_CONCAT && top + cur_lv->n > WORK_MAX) {
            return false;
        }
        if (cur_lv->kind == E_CONCAT) {
            for (uint32_t i = 0; i < cur_lv->n; i++) {
                work[top++] = cur_lv->list[i];
            }
        } else if (cur_lv->kind == E_BITSEL || cur_lv->kind == E_PART) {
            work[top++] = cur_lv->a;
        } else if (cur_lv->kind != E_MEM &&
                   !(cur_lv->kind == E_VAR && assignable_var(pr, cur_lv))) {
            return false;
        }
    }
    return true;
}

static tb_stmt *parse_assign(parser *pr, bool semi) {
    tb_stmt *stmt = new_stmt(pr, S_ASSIGN);
    tb_expr *lvalue = stmt != NULL ? parse_primary(pr) : NULL;
    if (lvalue == NULL || !valid_lvalue(pr, lvalue)) {
        fail(pr, "assignment target (a reg, integer, select or concatenation of them)");
        return NULL;
    }
    stmt->lhs = lvalue;
    if (accept_op(pr, "<=")) {
        stmt->kind = S_NBA;
    } else if (!expect_op(pr, "=")) {
        return NULL;
    }
    if (is_op(pr, "#") || is_kw(pr, "repeat") || is_op(pr, "@")) {
        fail(pr, "intra-assignment delay or event control");
        return NULL;
    }
    stmt->rhs = parse_expr(pr);
    return stmt->rhs != NULL && (!semi || expect_op(pr, ";")) ? stmt : NULL;
}

static const char *const SYS_TASKS[] = {
    "$display",  "$write",    "$strobe",   "$monitor", "$monitoroff", "$monitoron",
    "$fdisplay", "$fwrite",   "$fclose",   "$finish",  "$stop",       "$readmemh",
    "$readmemb", "$dumpfile", "$dumpvars", "$fopen",   NULL};

static tb_expr *parse_sys_task_expr(parser *pr) {
    tb_expr *call = new_expr(pr, E_SYS);
    if (call == NULL) {
        return NULL;
    }
    call->str = pnew(pr, TB_NAME);
    if (call->str == NULL) {
        return NULL;
    }
    (void)snprintf(call->str, TB_NAME, "%s", cur(pr)->text);
    pr->pos++;
    return parse_args(pr, call) ? call : NULL;
}

/* Rejects a format string with a specifier outside the subset (the format is the first argument,
 * or the second for the $f* tasks). */
static bool check_format(parser *pr, const tb_expr *call) {
    bool file_task = strncmp(call->str, "$f", 2) == 0 && strcmp(call->str, "$finish") != 0;
    uint32_t at = file_task ? 1U : 0U;
    bool no_fmt = strncmp(call->str, "$read", sizeof "$read" - 1U) == 0 ||
                  strcmp(call->str, "$dumpfile") == 0 || strcmp(call->str, "$fopen") == 0;
    char bad = 0;
    if (no_fmt || call->n <= at || call->list[at]->kind != E_STR ||
        tb_fmt_valid(call->list[at]->str, &bad)) {
        return true;
    }
    char msg[MSG_LEN];
    (void)snprintf(msg, sizeof msg, "format specifier %%%c", bad);
    return fail(pr, msg);
}

static tb_stmt *parse_systask(parser *pr) {
    tb_stmt *stmt = new_stmt(pr, S_SYS);
    bool known = false;
    for (size_t i = 0; SYS_TASKS[i] != NULL; i++) {
        known = known || strcmp(SYS_TASKS[i], cur(pr)->text) == 0;
    }
    if (stmt == NULL || !known) {
        fail(pr, "system task");
        return NULL;
    }
    stmt->sys = parse_sys_task_expr(pr);
    if (stmt->sys == NULL || !expect_op(pr, ";") || !check_format(pr, stmt->sys)) {
        return NULL;
    }
    return stmt;
}

static bool parse_event_terms(parser *pr, tb_stmt *stmt) {
    tb_event_term terms[TB_MAX_ARGS];
    uint32_t count = 0;
    do {
        if (count == TB_MAX_ARGS) {
            return fail(pr, "too many event terms");
        }
        terms[count].edge = 0;
        if (accept_kw(pr, "posedge")) {
            terms[count].edge = 1;
        } else if (accept_kw(pr, "negedge")) {
            terms[count].edge = 2;
        }
        terms[count].sig = parse_expr(pr);
        if (terms[count++].sig == NULL) {
            return false;
        }
    } while (accept_kw(pr, "or") || accept_op(pr, ","));
    stmt->terms = pnew(pr, count * sizeof *terms);
    if (stmt->terms == NULL) {
        return false;
    }
    memcpy(stmt->terms, terms, count * sizeof *terms);
    stmt->nterms = count;
    return true;
}

static bool case_item_labels(parser *pr, tb_expr **labels, uint32_t *nlab) {
    if (accept_kw(pr, "default")) {
        (void)accept_op(pr, ":");
        return true;
    }
    do {
        if (*nlab == TB_MAX_ARGS) {
            return fail(pr, "too many case labels");
        }
        labels[*nlab] = parse_expr(pr);
        if (labels[(*nlab)++] == NULL) {
            return false;
        }
    } while (accept_op(pr, ","));
    return expect_op(pr, ":");
}

static bool parse_decl(parser *pr);

/* --- statements: the frame stack --- */
typedef enum {
    SF_BLOCK,   /* begin ... end */
    SF_IF_THEN, /* the then branch of an if */
    SF_IF_ELSE,
    SF_BODY, /* the single body of while, repeat, wait, for, forever, a delay or an event */
    SF_CASE
} sfkind;

typedef struct sframe {
    sfkind kind;
    tb_stmt *stmt;
    uint32_t cap; /* SF_BLOCK: capacity of the child list */
} sframe;

/* What starting a statement produced: a finished statement, or an open compound one. */
typedef enum {
    ST_LEAF,  /* *out holds the finished statement */
    ST_WANT,  /* a frame was pushed and its first child statement is next */
    ST_CHECK, /* a frame was pushed; look for its end (or next item) before any child */
    ST_FAIL
} sstart;

static sframe *push_frame(parser *pr, sfkind kind, tb_stmt *stmt) {
    sframe *frm = odin3_vec_push(&pr->frames);
    if (frm == NULL) {
        fail(pr, "out of memory");
        return NULL;
    }
    frm->kind = kind;
    frm->stmt = stmt;
    return frm;
}

static sstart start_block(parser *pr) {
    pr->pos++;
    tb_stmt *stmt = new_stmt(pr, S_BLOCK);
    if (stmt == NULL) {
        return ST_FAIL;
    }
    stmt->list = (tb_stmt **)pnew_ptrs(pr, BLOCK_CAP0);
    if (accept_op(pr, ":") && cur(pr)->kind == TK_ID) {
        pr->pos++;
    }
    sframe *frm = stmt->list != NULL ? push_frame(pr, SF_BLOCK, stmt) : NULL;
    if (frm == NULL) {
        return ST_FAIL;
    }
    frm->cap = BLOCK_CAP0;
    return ST_CHECK;
}

/* `kw ( expr )` header of if, while, repeat and wait. */
static sstart start_paren(parser *pr, tb_skind kind) {
    tb_stmt *stmt = new_stmt(pr, kind);
    pr->pos++;
    if (stmt == NULL || !expect_op(pr, "(")) {
        return ST_FAIL;
    }
    stmt->rhs = parse_expr(pr);
    if (stmt->rhs == NULL || !expect_op(pr, ")")) {
        return ST_FAIL;
    }
    return push_frame(pr, kind == S_IF ? SF_IF_THEN : SF_BODY, stmt) != NULL ? ST_WANT : ST_FAIL;
}

static sstart start_for(parser *pr) {
    tb_stmt *stmt = new_stmt(pr, S_FOR);
    pr->pos++;
    if (stmt == NULL || !expect_op(pr, "(")) {
        return ST_FAIL;
    }
    stmt->init = parse_assign(pr, false);
    if (stmt->init == NULL || !expect_op(pr, ";")) {
        return ST_FAIL;
    }
    stmt->rhs = parse_expr(pr);
    if (stmt->rhs == NULL || !expect_op(pr, ";")) {
        return ST_FAIL;
    }
    stmt->step = parse_assign(pr, false);
    if (stmt->step == NULL || !expect_op(pr, ")")) {
        return ST_FAIL;
    }
    return push_frame(pr, SF_BODY, stmt) != NULL ? ST_WANT : ST_FAIL;
}

static sstart start_forever(parser *pr) {
    tb_stmt *stmt = new_stmt(pr, S_FOREVER);
    pr->pos++;
    return stmt != NULL && push_frame(pr, SF_BODY, stmt) != NULL ? ST_WANT : ST_FAIL;
}

/* A delay or event statement whose control part is parsed: its body is `;` or a statement. */
static sstart start_body_or_nop(parser *pr, tb_stmt *stmt, tb_stmt **out) {
    if (accept_op(pr, ";")) {
        stmt->a = new_stmt(pr, S_NOP);
        *out = stmt;
        return stmt->a != NULL ? ST_LEAF : ST_FAIL;
    }
    return push_frame(pr, SF_BODY, stmt) != NULL ? ST_WANT : ST_FAIL;
}

static sstart start_event(parser *pr, tb_stmt **out) {
    tb_stmt *stmt = new_stmt(pr, S_EVENT);
    pr->pos++;
    if (stmt == NULL) {
        return ST_FAIL;
    }
    if (is_op(pr, "*") || (is_op(pr, "(") && pr->toks[pr->pos + 1U].kind == TK_OP &&
                           strcmp(pr->toks[pr->pos + 1U].text, "*") == 0)) {
        fail(pr, "@* event control (list the signals)");
        return ST_FAIL;
    }
    if (!expect_op(pr, "(") || !parse_event_terms(pr, stmt) || !expect_op(pr, ")")) {
        return ST_FAIL;
    }
    return start_body_or_nop(pr, stmt, out);
}

static sstart start_delay(parser *pr, tb_stmt **out) {
    tb_stmt *stmt = new_stmt(pr, S_DELAY);
    pr->pos++;
    if (stmt == NULL) {
        return ST_FAIL;
    }
    stmt->rhs = parse_primary(pr);
    return stmt->rhs != NULL ? start_body_or_nop(pr, stmt, out) : ST_FAIL;
}

static sstart start_case(parser *pr) {
    tb_stmt *stmt = new_stmt(pr, S_CASE);
    if (stmt == NULL) {
        return ST_FAIL;
    }
    if (strcmp(cur(pr)->text, "case") != 0) {
        fail(pr, "casez/casex (no x/z in a 2-state simulator)");
        return ST_FAIL;
    }
    pr->pos++;
    stmt->list = (tb_stmt **)pnew_ptrs(pr, CASE_MAX_ITEMS);
    stmt->nlab = pnew(pr, CASE_MAX_ITEMS * sizeof *stmt->nlab);
    stmt->labels = (tb_expr **)pnew_ptrs(pr, (size_t)CASE_MAX_ITEMS * TB_MAX_ARGS);
    if (!expect_op(pr, "(")) {
        return ST_FAIL;
    }
    stmt->rhs = parse_expr(pr);
    if (stmt->rhs == NULL || !expect_op(pr, ")")) {
        return ST_FAIL;
    }
    return push_frame(pr, SF_CASE, stmt) != NULL ? ST_CHECK : ST_FAIL;
}

typedef enum {
    KW_NONE,
    KW_BEGIN,
    KW_IF,
    KW_CASE,
    KW_FOR,
    KW_WHILE,
    KW_REPEAT,
    KW_WAIT,
    KW_FOREVER,
    KW_DECL,
    KW_REJECTED
} stmt_kw;

static stmt_kw keyword_of(const parser *pr) {
    static const struct {
        const char *text;
        stmt_kw kw;
    } TABLE[] = {{"begin", KW_BEGIN},      {"if", KW_IF},
                 {"case", KW_CASE},        {"casez", KW_CASE},
                 {"casex", KW_CASE},       {"for", KW_FOR},
                 {"while", KW_WHILE},      {"repeat", KW_REPEAT},
                 {"wait", KW_WAIT},        {"forever", KW_FOREVER},
                 {"reg", KW_DECL},         {"integer", KW_DECL},
                 {"disable", KW_REJECTED}, {"fork", KW_REJECTED},
                 {"task", KW_REJECTED},    {"deassign", KW_REJECTED},
                 {"force", KW_REJECTED},   {"release", KW_REJECTED},
                 {NULL, KW_NONE}};
    for (size_t i = 0; TABLE[i].text != NULL; i++) {
        if (is_kw(pr, TABLE[i].text)) {
            return TABLE[i].kw;
        }
    }
    return KW_NONE;
}

static sstart start_keyword(parser *pr, stmt_kw kw, tb_stmt **out) {
    switch (kw) {
    case KW_BEGIN:
        return start_block(pr);
    case KW_IF:
        return start_paren(pr, S_IF);
    case KW_CASE:
        return start_case(pr);
    case KW_FOR:
        return start_for(pr);
    case KW_WHILE:
        return start_paren(pr, S_WHILE);
    case KW_REPEAT:
        return start_paren(pr, S_REPEAT);
    case KW_WAIT:
        return start_paren(pr, S_WAIT);
    case KW_FOREVER:
        return start_forever(pr);
    case KW_DECL:
        *out = parse_decl(pr) ? new_stmt(pr, S_NOP) : NULL;
        return *out != NULL ? ST_LEAF : ST_FAIL;
    default:
        fail(pr, cur(pr)->text);
        return ST_FAIL;
    }
}

/* Starts the statement at the cursor. */
static sstart start_stmt(parser *pr, tb_stmt **out) {
    const tb_tok *tok = cur(pr);
    if (is_op(pr, ";")) {
        *out = new_stmt(pr, S_NOP);
        pr->pos++;
        return *out != NULL ? ST_LEAF : ST_FAIL;
    }
    if (is_op(pr, "#")) {
        return start_delay(pr, out);
    }
    if (is_op(pr, "@")) {
        return start_event(pr, out);
    }
    if (tok->kind == TK_SYS) {
        *out = parse_systask(pr);
    } else if (tok->kind == TK_ID && keyword_of(pr) != KW_NONE) {
        return start_keyword(pr, keyword_of(pr), out);
    } else {
        *out = parse_assign(pr, true);
    }
    return *out != NULL ? ST_LEAF : ST_FAIL;
}

/* A block: add the finished child (if any), then close at `end` or make room for the next. */
static bool step_block(parser *pr, sframe *frm, tb_stmt *child, bool *done) {
    tb_stmt *stmt = frm->stmt;
    if (child != NULL) {
        stmt->list[stmt->n++] = child;
    }
    *done = accept_kw(pr, "end");
    if (*done) {
        return true;
    }
    if (cur(pr)->kind == TK_EOF) {
        return fail(pr, "missing `end`");
    }
    if (stmt->n == frm->cap) {
        tb_stmt **wider = (tb_stmt **)pnew_ptrs(pr, (size_t)frm->cap * 2U);
        if (wider == NULL) {
            return false;
        }
        memcpy((void *)wider, (const void *)stmt->list, (size_t)stmt->n * sizeof(void *));
        stmt->list = wider;
        frm->cap *= 2U;
    }
    return true;
}

/* A case: add the finished item body (if any), then close at `endcase` or read the next labels. */
static bool step_case(parser *pr, sframe *frm, tb_stmt *child, bool *done) {
    tb_stmt *stmt = frm->stmt;
    if (child != NULL) {
        stmt->list[stmt->n++] = child;
    }
    *done = accept_kw(pr, "endcase");
    if (*done) {
        return true;
    }
    if (stmt->n == CASE_MAX_ITEMS || stmt->list == NULL || cur(pr)->kind == TK_EOF) {
        return fail(pr, "case with more than 64 items");
    }
    uint32_t nlab = 0;
    if (!case_item_labels(pr, &stmt->labels[(size_t)stmt->n * TB_MAX_ARGS], &nlab)) {
        return false;
    }
    stmt->nlab[stmt->n] = nlab;
    return true;
}

/* A finished child reaches the open statement on top: store it; *done when that one is finished
 * too, *again when the open statement wants another child. */
static bool deliver_child(parser *pr, tb_stmt *child, bool *done) {
    sframe *frm = odin3_vec_at(&pr->frames, pr->frames.len - 1U);
    tb_stmt *stmt = frm->stmt;
    *done = true;
    switch (frm->kind) {
    case SF_BODY:
        stmt->a = child;
        return true;
    case SF_IF_THEN:
        stmt->a = child;
        if (accept_kw(pr, "else")) {
            frm->kind = SF_IF_ELSE;
            *done = false;
        }
        return true;
    case SF_IF_ELSE:
        stmt->b = child;
        return true;
    case SF_BLOCK:
        return step_block(pr, frm, child, done);
    default:
        return step_case(pr, frm, child, done);
    }
}

/* Progress of parse_stmt: *have says a finished statement (or the start of a frame) waits in
 * *result; the step either starts a statement or hands the finished one to its parent. */
static bool stmt_step(parser *pr, tb_stmt **result, bool *have) {
    if (!*have) {
        sstart st = start_stmt(pr, result);
        *have = st == ST_LEAF || st == ST_CHECK;
        *result = st == ST_LEAF ? *result : NULL;
        return st != ST_FAIL;
    }
    bool done = false;
    bool ok = deliver_child(pr, *result, &done);
    *have = ok && done;
    *result = NULL;
    if (*have) {
        *result = ((sframe *)odin3_vec_at(&pr->frames, pr->frames.len - 1U))->stmt;
        odin3_vec_pop(&pr->frames);
    }
    return ok;
}

/* Parses one statement; NULL on failure (reported). */
static tb_stmt *parse_stmt(parser *pr) {
    size_t floor_len = pr->frames.len;
    tb_stmt *result = NULL;
    bool have = false;
    bool ok = true;
    while (ok && !pr->failed && !(have && pr->frames.len == floor_len)) {
        ok = stmt_step(pr, &result, &have);
    }
    bool good = ok && !pr->failed && have && result != NULL;
    truncate_vec(&pr->frames, floor_len);
    return good ? result : NULL;
}

/* --- module items --- */
static bool parse_array_range(parser *pr, int var_index) {
    int64_t high = 0;
    int64_t low = 0;
    pr->pos++;
    if (!const_int(pr, &low) || !expect_op(pr, ":") || !const_int(pr, &high) ||
        !expect_op(pr, "]") || low != 0 || high < 0 || high > (1 << ARRAY_MAX_LOG2)) {
        return fail(pr, "array range (use [0:N])");
    }
    tb_var *var = &pr->tb->vars[var_index];
    var->depth = (uint32_t)high + 1U;
    var->mem = pnew(pr, (size_t)var->depth * sizeof *var->mem);
    return var->mem != NULL;
}

static bool add_assign(parser *pr, int var, tb_expr *rhs) {
    tb_assign *grown = pnew(pr, ((size_t)pr->tb->nassigns + 1U) * sizeof *grown);
    if (grown == NULL) {
        return false;
    }
    if (pr->tb->nassigns > 0) {
        memcpy(grown, pr->tb->assigns, (size_t)pr->tb->nassigns * sizeof *grown);
    }
    grown[pr->tb->nassigns] = (tb_assign){var, rhs};
    pr->tb->assigns = grown;
    pr->tb->nassigns++;
    return true;
}

static bool parse_decl_item(parser *pr, const char *kw, var_shape shape) {
    bool is_param = strcmp(kw, "parameter") == 0 || strcmp(kw, "localparam") == 0;
    if (cur(pr)->kind != TK_ID) {
        return fail(pr, "declaration syntax");
    }
    char name[TB_NAME];
    (void)snprintf(name, sizeof name, "%s", cur(pr)->text);
    pr->pos++;
    shape.is_wire = strcmp(kw, "wire") == 0;
    int var = add_var(pr, name, shape);
    if (var < 0) {
        return false;
    }
    pr->tb->vars[var].is_param = is_param;
    if (is_op(pr, "[")) {
        return parse_array_range(pr, var);
    }
    if (!accept_op(pr, "=")) {
        return !is_param || fail(pr, "parameter without a value");
    }
    tb_expr *init = parse_expr(pr);
    int64_t val = 0;
    if (init == NULL) {
        return false;
    }
    if (shape.is_wire) {
        return add_assign(pr, var, init);
    }
    if (!tb_const(pr->tb, init, &val)) {
        return fail(pr, "non-constant initialiser");
    }
    pr->tb->vars[var].val = (uint64_t)val & tb_mask(shape.width);
    return true;
}

static bool parse_decl(parser *pr) {
    char kw[TB_NAME];
    (void)snprintf(kw, sizeof kw, "%s", cur(pr)->text);
    pr->pos++;
    bool is_int = strcmp(kw, "integer") == 0;
    /* A parameter without a range has the type of its value: an unsized one is a 32-bit integer. */
    bool is_param = strcmp(kw, "parameter") == 0 || strcmp(kw, "localparam") == 0;
    bool is_signed = accept_kw(pr, "signed");
    bool is_ranged = is_op(pr, "[");
    bool wide = is_int || (is_param && !is_ranged);
    var_shape shape = {wide ? (uint32_t)TB_INT_BITS : 1U, wide || is_signed, false};
    if (is_ranged && !parse_range(pr, &shape.width)) {
        return false;
    }
    do {
        if (!parse_decl_item(pr, kw, shape)) {
            return false;
        }
    } while (accept_op(pr, ","));
    return expect_op(pr, ";");
}

static bool add_proc(parser *pr, tb_stmt *body, bool always) {
    tb_module *tb = pr->tb;
    tb_proc_def *grown = pnew(pr, ((size_t)tb->nprocs + 1U) * sizeof *grown);
    if (grown == NULL || body == NULL) {
        return false;
    }
    if (tb->nprocs > 0) {
        memcpy(grown, tb->procs, (size_t)tb->nprocs * sizeof *grown);
    }
    grown[tb->nprocs++] = (tb_proc_def){body, always};
    tb->procs = grown;
    return true;
}

static bool starts_with_wait(const tb_stmt *stmt) {
    while (stmt->kind == S_BLOCK && stmt->n > 0) {
        stmt = stmt->list[0];
    }
    return stmt->kind == S_DELAY || stmt->kind == S_EVENT || stmt->kind == S_WAIT;
}

static bool parse_always(parser *pr) {
    pr->pos++;
    tb_stmt *body = parse_stmt(pr);
    if (body != NULL && !starts_with_wait(body)) {
        return fail(pr, "always block that does not start with #, @ or wait (zero-delay loop)");
    }
    return add_proc(pr, body, true);
}

static bool parse_assign_item(parser *pr) {
    pr->pos++;
    const tb_expr *lhs = parse_primary(pr);
    if (lhs == NULL || lhs->kind != E_VAR || !pr->tb->vars[lhs->var].is_wire) {
        return fail(pr, "assign to anything but a whole wire");
    }
    if (!expect_op(pr, "=")) {
        return false;
    }
    tb_expr *rhs = parse_expr(pr);
    return rhs != NULL && expect_op(pr, ";") && add_assign(pr, lhs->var, rhs);
}

/* One port connection: `.port(expr)`, `.port()` or an ordered `expr`. */
static bool parse_conn(parser *pr, tb_conn *conn) {
    if (accept_op(pr, ".")) {
        (void)snprintf(conn->port, sizeof conn->port, "%s", cur(pr)->text);
        pr->pos++;
        if (!expect_op(pr, "(")) {
            return false;
        }
        conn->expr = is_op(pr, ")") ? NULL : parse_expr(pr);
        if ((conn->expr == NULL && !is_op(pr, ")")) || !expect_op(pr, ")")) {
            return false;
        }
    } else {
        conn->expr = parse_expr(pr);
    }
    if ((conn->expr == NULL && conn->port[0] == '\0') || (!accept_op(pr, ",") && !is_op(pr, ")"))) {
        return fail(pr, "port connection syntax");
    }
    return true;
}

static bool parse_inst_conns(parser *pr, tb_inst *inst) {
    uint32_t cap = INST_CONNS_MAX;
    inst->conns = pnew(pr, (size_t)cap * sizeof *inst->conns);
    if (inst->conns == NULL || !expect_op(pr, "(")) {
        return false;
    }
    while (!accept_op(pr, ")")) {
        if (inst->nconns == cap) {
            return fail(pr, "more than 16 port connections");
        }
        if (!parse_conn(pr, &inst->conns[inst->nconns++])) {
            return false;
        }
    }
    return true;
}

static bool parse_instance(parser *pr) {
    tb_module *tb = pr->tb;
    tb_inst *grown = pnew(pr, ((size_t)tb->ninsts + 1U) * sizeof *grown);
    if (grown == NULL) {
        return false;
    }
    if (tb->ninsts > 0) {
        memcpy(grown, tb->insts, (size_t)tb->ninsts * sizeof *grown);
    }
    tb->insts = grown;
    tb_inst *inst = &grown[tb->ninsts++];
    inst->line = cur(pr)->line;
    (void)snprintf(inst->module, sizeof inst->module, "%s", cur(pr)->text);
    pr->pos++;
    if (is_op(pr, "#")) {
        return fail(pr, "parameter override on a DUT instance (the DUT is already elaborated)");
    }
    if (cur(pr)->kind != TK_ID) {
        return fail(pr, "module item");
    }
    (void)snprintf(inst->name, sizeof inst->name, "%s", cur(pr)->text);
    pr->pos++;
    return parse_inst_conns(pr, inst) && expect_op(pr, ";");
}

static bool parse_item(parser *pr) {
    const tb_tok *tok = cur(pr);
    if (tok->kind != TK_ID) {
        return fail(pr, "module item");
    }
    if (is_kw(pr, "reg") || is_kw(pr, "wire") || is_kw(pr, "integer") || is_kw(pr, "parameter") ||
        is_kw(pr, "localparam")) {
        return parse_decl(pr);
    }
    if (is_kw(pr, "initial")) {
        pr->pos++;
        return add_proc(pr, parse_stmt(pr), false);
    }
    if (is_kw(pr, "always")) {
        return parse_always(pr);
    }
    if (is_kw(pr, "assign")) {
        return parse_assign_item(pr);
    }
    static const char *const REJECT[] = {
        "genvar", "generate", "task",    "function", "real",   "time",  "string",
        "event",  "defparam", "specify", "input",    "output", "inout", "and",
        "or",     "nand",     "nor",     "xor",      "not",    "buf",   NULL};
    for (size_t i = 0; REJECT[i] != NULL; i++) {
        if (is_kw(pr, REJECT[i])) {
            return fail(pr, tok->text);
        }
    }
    return parse_instance(pr);
}

static bool parse_header(parser *pr, const char *top) {
    if (!accept_kw(pr, "module") || cur(pr)->kind != TK_ID) {
        return fail(pr, "file does not start with a module");
    }
    (void)snprintf(pr->tb->name, sizeof pr->tb->name, "%s", cur(pr)->text);
    pr->pos++;
    if (top != NULL && strcmp(top, pr->tb->name) != 0) {
        return fail(pr,
                    "--top names a module that is not the first (one testbench module per file)");
    }
    if (accept_op(pr, "(") && !expect_op(pr, ")")) {
        return false;
    }
    return expect_op(pr, ";");
}

static void parse_items(parser *pr) {
    while (!pr->failed && !accept_kw(pr, "endmodule")) {
        if (cur(pr)->kind == TK_EOF) {
            fail(pr, "missing endmodule");
        } else if (is_kw(pr, "module")) {
            fail(pr, "more than one module in the file");
        } else {
            (void)parse_item(pr);
        }
    }
}

odin3_status tb_parse(const tb_source *src, const char *top, tb_module **out) {
    *out = NULL;
    tb_module *tb = calloc(1, sizeof *tb);
    tb_lexed lexed = {0};
    if (tb == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (!tb_lex(src, tb, &lexed)) {
        tb_module_free(tb);
        return ODIN3_ERR_INVALID_ARG;
    }
    parser pr = {.tb = tb, .toks = lexed.toks, .file = src->file};
    odin3_vec_init(&pr.ents, sizeof(xent));
    odin3_vec_init(&pr.vals, sizeof(tb_expr *));
    odin3_vec_init(&pr.frames, sizeof(sframe));
    if (parse_header(&pr, top)) {
        parse_items(&pr);
    }
    if (!pr.failed && cur(&pr)->kind != TK_EOF) {
        fail(&pr, "more than one module in the file");
    }
    odin3_vec_free(&pr.ents);
    odin3_vec_free(&pr.vals);
    odin3_vec_free(&pr.frames);
    if (pr.failed) {
        tb_module_free(tb);
        return ODIN3_ERR_INVALID_ARG;
    }
    *out = tb;
    return ODIN3_OK;
}

void tb_module_free(tb_module *tb) {
    if (tb == NULL) {
        return;
    }
    for (uint32_t i = 0; i < tb->nallocs; i++) {
        free(tb->allocs[i]);
    }
    free((void *)tb->allocs);
    free(tb);
}
