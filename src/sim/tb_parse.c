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
    PREC_POWER = 11, /* ** binds tightest and to the right */
    CASE_MAX_ITEMS = 64,
    BLOCK_CAP0 = 8,
    INST_CONNS_FIRST = 16, /* port connections allocated for a DUT instance at first */
    INST_CONNS_MAX = 4096, /* the most port connections of one instance (bounded memory) */
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
    size_t efloor;       /* expression engine: context stack height at entry */
    bool primary_only;   /* expression engine: stop after one primary */
    odin3_vec ents;      /* xent: open expression contexts */
    odin3_vec vals;      /* tb_expr *: finished operands */
    odin3_vec frames;    /* sframe: open compound statements */
    bool lenient;        /* first pass: a hierarchical name is a placeholder, nothing is resolved */
    bool saw_hier;       /* a hierarchical name was met */
    uint32_t next_inst;  /* the instance slot the next instance item fills (second pass) */
    char scope[TB_NAME]; /* the task being parsed: its names are stored as "task.name" */
    odin3_vec calls;     /* tb_stmt *: task calls, linked to their task at the end of the module */
    odin3_vec mods;      /* mod_def: every module of the testbench files (module index) */
    odin3_vec subs;      /* sub_frame: open testbench-side module instances */
    odin3_vec aliases;   /* alias_ent: port names that stand for a variable of the parent scope */
    char prefix[TB_NAME]; /* the instance path of the module being parsed ("u0.u1."), or "" */
} parser;

/* Where a module of the testbench files starts: the index of its `module` token. */
typedef struct mod_def {
    char name[TB_NAME];
    uint32_t pos;
    bool informed; /* the INFO line "the design wins" was printed */
} mod_def;

/* A name of the current instance that is another name for a variable of the parent scope (a port
 * connected to a whole variable of equal width). */
typedef struct alias_ent {
    char name[TB_NAME];
    int var;
    bool refinable; /* declared by direction only: a later reg declaration is absorbed */
    bool is_out;
} alias_ent;

enum {
    SUB_DEPTH = 16,    /* nested testbench-side module instances */
    SUB_PORTS = 128,   /* ports of one module */
    SUB_OVERRIDES = 32 /* parameter overrides of one instance */
};

typedef struct sub_ov {
    char name[TB_NAME]; /* "" for a positional override */
    tb_expr *expr;      /* parsed in the parent scope */
    bool used;
} sub_ov;

typedef struct sub_port {
    char name[TB_NAME];
    int var; /* the port's variable, -1 until declared */
    uint8_t dir;
    bool aliased; /* the name stands for the connected variable of the parent scope */
} sub_port;

/* One open testbench-side module instance: its body is being parsed. */
typedef struct sub_frame {
    uint32_t ret_pos; /* token after the instance statement */
    uint32_t line;    /* the instance statement */
    char module[TB_NAME];
    char prefix[TB_NAME]; /* instance path, "u0.u1." */
    sub_ov *ovs;
    uint32_t nov;
    tb_conn *conns;
    uint32_t nconns;
    sub_port *ports;
    uint32_t nports;
    uint32_t nparam; /* `parameter` declarations met (positional overrides count these) */
    bool ansi;
} sub_frame;

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
static alias_ent *find_alias(const parser *pr, const char *name) {
    for (size_t i = 0; i < pr->aliases.len; i++) {
        alias_ent *ent = odin3_vec_at((odin3_vec *)&pr->aliases, i);
        if (strcmp(ent->name, name) == 0) {
            return ent;
        }
    }
    return NULL;
}

/* The variable called name in full (instance path included), through a port alias. */
static int find_exact(const parser *pr, const char *name) {
    for (uint32_t i = 0; i < pr->tb->nvars; i++) {
        if (strcmp(pr->tb->vars[i].name, name) == 0) {
            return (int)i;
        }
    }
    const alias_ent *ent = find_alias(pr, name);
    return ent != NULL ? ent->var : -1;
}

enum { QNAME = (3 * TB_NAME) + 4 }; /* instance path + task + name */

/* A name inside a task is that task's own first, then the module's; both under the instance
 * path of the module being parsed (empty in the testbench itself). */
static int find_var(const parser *pr, const char *name) {
    char full[QNAME];
    if (pr->scope[0] != '\0') {
        (void)snprintf(full, sizeof full, "%s%s.%s", pr->prefix, pr->scope, name);
        int found = find_exact(pr, full);
        if (found >= 0) {
            return found;
        }
    }
    (void)snprintf(full, sizeof full, "%s%s", pr->prefix, name);
    return find_exact(pr, full);
}

/* (Re)allocates the planes of variable index for its width and depth and gives them their power-on
 * value: in 4-state mode x for a reg, integer or memory and z for a wire (an undriven wire is z,
 * IEEE 1364-2005 4.5), else 0. */
static bool var_storage(parser *pr, int index) {
    tb_var *var = &pr->tb->vars[index];
    size_t words = var->depth > 0 ? var->depth : 1U;
    size_t bytes = words * odin3_word_limbs(var->width) * sizeof(uint64_t);
    var->v = pnew(pr, bytes);
    var->u = pnew(pr, bytes);
    if (var->v == NULL || var->u == NULL) {
        return false;
    }
    if (pr->tb->four_state) {
        tb_var_fill(var, var->is_wire ? (uint32_t)ODIN3_W4_Z : (uint32_t)ODIN3_W4_X);
    }
    return true;
}

typedef struct var_shape {
    uint32_t width;
    bool sgn;
    bool is_wire;
    bool ranged;    /* a [h:l] range was given */
    bool refinable; /* see tb_var.refinable */
    uint32_t lo;    /* the l of the range */
} var_shape;

static int add_var(parser *pr, const char *name, var_shape shape) {
    tb_module *tb = pr->tb;
    char scoped[QNAME];
    (void)snprintf(scoped, sizeof scoped, "%s%s%s%s", pr->prefix, pr->scope,
                   pr->scope[0] != '\0' ? "." : "", name);
    name = scoped;
    size_t name_len = strlen(name);
    if (name_len >= TB_NAME) {
        fail(pr, "name longer than 63 characters (instance path included)");
        return -1;
    }
    if (find_exact(pr, name) >= 0) {
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
    memcpy(var->name, name, name_len);
    var->width = shape.width;
    var->sgn = shape.sgn;
    var->is_wire = shape.is_wire;
    var->lo = shape.lo;
    var->refinable = shape.refinable;
    int index = (int)tb->nvars++;
    return var_storage(pr, index) ? index : -1;
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

/* `[h:l]` (the cursor is on `[`, h >= l >= 0): sets the width and the low bound of shape. */
static bool parse_range(parser *pr, var_shape *shape) {
    int64_t high = 0;
    int64_t low = 0;
    if (!expect_op(pr, "[") || !const_int(pr, &high) || !expect_op(pr, ":") ||
        !const_int(pr, &low) || !expect_op(pr, "]")) {
        return false;
    }
    if (low < 0 || high < low) {
        return fail(pr, "range other than [h:l] with h >= l >= 0");
    }
    if (high - low >= TB_MAX_WIDTH) {
        return fail(pr, "signal wider than 1024 bits");
    }
    shape->width = (uint32_t)(high - low) + 1U;
    shape->lo = (uint32_t)low;
    shape->ranged = true;
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

static const char *const SYS_EXPR[] = {"$time",           "$random", "$urandom", "$signed",
                                       "$unsigned",       "$clog2",  "$fopen",   "$test$plusargs",
                                       "$value$plusargs", NULL};

/* True when e is something a plusarg value can be stored in. */
static bool plusarg_target(const tb_expr *expr) {
    return expr->kind == E_VAR || expr->kind == E_BITSEL || expr->kind == E_PART ||
           expr->kind == E_MEM;
}

/* The argument check of $test$plusargs ("text") and $value$plusargs ("format", variable). */
static bool plusargs_ok(parser *pr, const tb_expr *call) {
    if (strcmp(call->str, "$test$plusargs") == 0 && call->n != 1) {
        return fail(pr, "wrong argument count");
    }
    if (strcmp(call->str, "$value$plusargs") != 0) {
        return true;
    }
    if (call->n != 2) {
        return fail(pr, "wrong argument count");
    }
    return (call->list[0]->kind == E_STR && plusarg_target(call->list[1])) ||
           fail(pr, "$value$plusargs expects (\"format\", variable)");
}

/* Width and signedness of a system function call whose arguments are in place. */
static bool sys_finish(parser *pr, tb_expr *call) {
    bool is_cast = strcmp(call->str, "$signed") == 0 || strcmp(call->str, "$unsigned") == 0;
    if ((is_cast || strcmp(call->str, "$clog2") == 0) && call->n != 1) {
        return fail(pr, "wrong argument count");
    }
    if (!plusargs_ok(pr, call)) {
        return false;
    }
    call->width = strcmp(call->str, "$time") == 0 ? (uint32_t)TB_TIME_BITS : (uint32_t)TB_INT_BITS;
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
    expr->nv = tok->nv;
    expr->nu = tok->nu;
    expr->xfill = tok->xfill;
    expr->width = tok->width == 0 ? (uint32_t)TB_INT_BITS : tok->width;
    expr->sgn = tok->is_signed;
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
                 {"*", 10}, {"/", 10}, {"%", 10},  {"**", 11}, {NULL, 0}};
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
    if (prec == PREC_SHIFT || prec == PREC_POWER) {
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
    X_ARGS,    /* system function arguments */
    X_CONCAT,  /* `{ ... }`, possibly a replication */
    X_INDEX,   /* `name[ ...` */
    X_HIERIDX, /* `inst.mem[ ...` */
    X_WORDSEL, /* `mem[i][ ...`: a bit or part of an array word */
    X_WORDLOW, /* `mem[i][high : ...` */
    X_LOW      /* `name[high : ...` */
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

/* --- hierarchical reads of DUT signals: `inst.sub.sig` and `inst.mem[3]` (constant indices) --- */

enum { HIER_PATH = 256 };

static int find_inst(const parser *pr, const char *name) {
    for (uint32_t i = 0; i < pr->tb->ninsts; i++) {
        if (strcmp(pr->tb->insts[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* Appends text to path (NUL-terminated, HIER_PATH bytes); false when it does not fit. */
static bool path_add(char *path, const char *text) {
    size_t used = strlen(path);
    size_t add = strlen(text);
    if (used + add >= HIER_PATH) {
        return false;
    }
    memcpy(path + used, text, add + 1U);
    return true;
}

/* `.name` (the "." is read): appends it to path. */
static bool hier_name(parser *pr, char *path) {
    char piece[TB_NAME + 2U];
    if (cur(pr)->kind != TK_ID) {
        return false;
    }
    (void)snprintf(piece, sizeof piece, "%s%s", path[0] != '\0' ? "." : "", cur(pr)->text);
    pr->pos++;
    return path_add(path, piece);
}

/* `[number]` (the "[" is read): appends it to path. */
static bool hier_index(parser *pr, char *path) {
    char piece[TB_NAME + 2U];
    (void)snprintf(piece, sizeof piece, "[%llu]", (unsigned long long)cur(pr)->num);
    pr->pos += 2U; /* the number and the "]" */
    return path_add(path, piece);
}

/* True when the tokens after the "[" at the cursor are `number ]`. */
static bool const_index_ahead(const parser *pr) {
    const tb_tok *num = &pr->toks[pr->pos + 1U];
    const tb_tok *close = &pr->toks[pr->pos + 2U];
    return num->kind == TK_NUM && !num->sized && close->kind == TK_OP &&
           strcmp(close->text, "]") == 0;
}

typedef enum { HP_FAIL, HP_DONE, HP_INDEX } hp_res;

/* After `inst`: reads `.name` components and `[number]` indices into path (the part after the
 * instance name, as the DUT's signal table spells it). Stops before a `[` whose index is an
 * expression (HP_INDEX). */
static hp_res hier_path(parser *pr, char *path) {
    while (is_op(pr, ".") || is_op(pr, "[")) {
        if (is_op(pr, "[")) {
            if (!const_index_ahead(pr)) {
                return HP_INDEX;
            }
            pr->pos++;
            if (!hier_index(pr, path)) {
                return HP_FAIL;
            }
            continue;
        }
        pr->pos++;
        if (!hier_name(pr, path)) {
            fail(pr, "hierarchical name (name.name[number], constant indices only)");
            return HP_FAIL;
        }
    }
    return HP_DONE;
}

/* A word array of the DUT (`inst.mem[...]`, or the whole array): E_HIERMEM over the words. */
static tb_expr *make_hiermem(parser *pr, int inst, const char *path, uint32_t first) {
    tb_hier_arr arr = {NULL, 0, 1};
    char where[TB_WHERE_MAX];
    if (!pr->lenient) {
        odin3_status st = tb_inst_array(pr->tb, &pr->tb->insts[inst], path, &arr);
        if (st != ODIN3_OK || arr.n == 0) {
            free(arr.ids);
            pr->failed = true;
            if (!is_op(pr, "[")) { /* a plain name: say there is no such signal */
                tb_hier_sig none;
                (void)tb_inst_signal(pr->tb, &pr->tb->insts[inst], path,
                                     (tb_src_loc){pr->file, pr->toks[first].line}, &none);
                return NULL;
            }
            odin3_log(ODIN3_LOG_ERROR,
                      "%s: DUT `%s` has no word array `%s` (its words `%s[0]`, `%s[1]`, ... are "
                      "named by `lower --keep-mem`; `opt` removes the name of a memory nothing "
                      "reads)",
                      tb_where(pr->file, pr->toks[first].line, where, sizeof where),
                      pr->tb->insts[inst].name, path, path, path);
            return NULL;
        }
    }
    tb_expr *expr = new_expr(pr, E_HIERMEM);
    uint32_t *ids = expr != NULL ? pnew(pr, ((size_t)arr.n + 1U) * sizeof *ids) : NULL;
    if (ids != NULL && arr.n > 0) {
        memcpy(ids, arr.ids, (size_t)arr.n * sizeof *ids);
    }
    free(arr.ids);
    if (ids == NULL) {
        return NULL;
    }
    expr->var = inst;
    expr->ids = ids;
    expr->n = arr.n;
    expr->width = arr.width;
    return expr;
}

/* `inst.path` or `inst.path[ index ]` of a word array; the "[" of an expression index is at the
 * cursor. */
static bool begin_hier_array(parser *pr, bool *want, const char *path, uint32_t first) {
    int inst = pr->lenient ? 0 : find_inst(pr, pr->toks[first].text);
    tb_expr *expr = make_hiermem(pr, inst, path, first);
    if (expr == NULL) {
        return false;
    }
    if (is_op(pr, "[")) {
        pr->pos++;
        *want = true;
        return push_ent(pr, X_HIERIDX, expr) != NULL;
    }
    *want = false;
    return push_val(pr, expr);
}

/* After `inst.mem[ index`: the select is complete at `]`. */
static xres step_hieridx(parser *pr, xent *top, bool *want) {
    tb_expr *expr = top->node;
    expr->a = pop_val(pr);
    odin3_vec_pop(&pr->ents);
    *want = false;
    if (is_op(pr, ":")) {
        fail(pr, "hierarchical name (part-selects are not supported)");
        return XR_FAIL;
    }
    return expect_op(pr, "]") && push_val(pr, expr) ? XR_CONT : XR_FAIL;
}

/* `inst.path`: a read of a signal of the DUT instance, resolved now (the width is needed). */
static bool begin_hier(parser *pr, bool *want) {
    int inst = pr->lenient ? 0 : find_inst(pr, cur(pr)->text);
    uint32_t first = pr->pos; /* the instance name: the line of the read, for its errors */
    pr->saw_hier = true;
    if (inst < 0) {
        return fail(pr, "hierarchical name whose first part is not a DUT instance of this "
                        "testbench");
    }
    char path[HIER_PATH] = "";
    tb_hier_sig sig = {0,
                       1}; /* the first pass only finds the instances and the hierarchical names */
    pr->pos++;
    hp_res res = hier_path(pr, path);
    if (res == HP_FAIL) {
        return false;
    }
    uint32_t found = 1;
    if (!pr->lenient && res == HP_DONE &&
        tb_inst_find(pr->tb, &pr->tb->insts[inst], path, &found) != ODIN3_OK) {
        pr->failed = true;
        return false;
    }
    if (res == HP_INDEX || found == 0) {
        return begin_hier_array(pr, want, path, first);
    }
    tb_src_loc loc = {pr->file, pr->toks[first].line};
    if (!pr->lenient && tb_inst_signal(pr->tb, &pr->tb->insts[inst], path, loc, &sig) != ODIN3_OK) {
        pr->failed = true;
        return false;
    }
    tb_expr *expr = new_expr(pr, E_HIER);
    if (expr == NULL) {
        return false;
    }
    expr->var = inst;
    expr->num = sig.id;
    expr->width = sig.width;
    *want = false;
    return push_val(pr, expr);
}

static bool begin_ident(parser *pr, bool *want) {
    if (pr->toks[pr->pos + 1U].kind == TK_OP && strcmp(pr->toks[pr->pos + 1U].text, ".") == 0) {
        return begin_hier(pr, want);
    }
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
        return fail(pr, "concatenation wider than 1024 bits");
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
    if (var->depth == 0 && var->lo != 0) {
        fail(pr, "bit or part select of a vector whose range does not end at 0 ([h:l], l > 0)");
        return XR_FAIL;
    }
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

/* After `mem[i][ index`: `]` closes a bit select of the word, `:` starts a part select. */
static xres step_wordsel(parser *pr, xent *top, bool *want) {
    tb_expr *base = top->node;
    tb_expr *index = pop_val(pr);
    bool part = is_op(pr, ":");
    int64_t high = 0;
    if (part && !tb_const(pr->tb, index, &high)) {
        fail(pr, "part select (constants within the declared width)");
        return XR_FAIL;
    }
    tb_expr *sel = new_expr(pr, part ? E_PART : E_BITSEL);
    if (sel == NULL) {
        return XR_FAIL;
    }
    sel->a = base;
    sel->b = index;
    sel->width = part ? base->width : 1U;
    sel->num = (uint64_t)high;
    if (part) {
        pr->pos++;
        top->kind = X_WORDLOW;
        top->node = sel;
        *want = true;
        return XR_CONT;
    }
    odin3_vec_pop(&pr->ents);
    *want = false;
    return expect_op(pr, "]") && push_val(pr, sel) ? XR_CONT : XR_FAIL;
}

/* After `mem[i][high : low`: the part select is complete at `]`. */
static xres step_wordlow(parser *pr, xent *top, bool *want) {
    tb_expr *sel = top->node;
    int64_t low = 0;
    int64_t high = (int64_t)sel->num;
    const tb_expr *low_expr = pop_val(pr);
    if (!tb_const(pr->tb, low_expr, &low)) {
        fail(pr, "non-constant expression where a constant is required");
        return XR_FAIL;
    }
    if (high < low || low < 0 || (!pr->lenient && high >= (int64_t)sel->a->width)) {
        fail(pr, "part select (constants within the declared width)");
        return XR_FAIL;
    }
    sel->num = 0;
    sel->aux = (uint32_t)low;
    sel->width = (uint32_t)(high - low) + 1U;
    odin3_vec_pop(&pr->ents);
    *want = false;
    return expect_op(pr, "]") && push_val(pr, sel) ? XR_CONT : XR_FAIL;
}

/* A select context (`name[`, `inst.mem[`, `mem[i][`, and their part-select tails) reaches its `]`.
 */
static xres step_select(parser *pr, xent *top, bool *want) {
    switch (top->kind) {
    case X_INDEX:
        return step_index(pr, top, want);
    case X_HIERIDX:
        return step_hieridx(pr, top, want);
    case X_WORDSEL:
        return step_wordsel(pr, top, want);
    case X_WORDLOW:
        return step_wordlow(pr, top, want);
    default:
        return step_low(pr, top, want);
    }
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
    default:
        return step_select(pr, top, want);
    }
}

/* The binary operator at the cursor (precedence prec): reduce tighter ones, push this one. */
static bool push_binary(parser *pr, int prec) {
    if (!reduce_binary(pr, prec == PREC_POWER ? prec + 1 : prec)) {
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

/* True when the `[` at the cursor selects a bit or part of the array word just parsed. */
static bool word_select_ahead(const parser *pr) {
    if (pr->primary_only || !is_op(pr, "[") || pr->vals.len == 0) {
        return false;
    }
    const tb_expr *word = *(tb_expr *const *)odin3_vec_cat(&pr->vals, pr->vals.len - 1U);
    return word->kind == E_MEM || word->kind == E_HIERMEM;
}

static xres begin_word_select(parser *pr, bool *want) {
    pr->pos++;
    *want = true;
    tb_expr *word = pop_val(pr);
    return push_ent(pr, X_WORDSEL, word) != NULL ? XR_CONT : XR_FAIL;
}

/* One step at a position just after an operand. */
static xres step_after(parser *pr, bool *want) {
    if (word_select_ahead(pr)) {
        return begin_word_select(pr, want);
    }
    if (pr->primary_only && pr->ents.len == pr->efloor) {
        return XR_DONE;
    }
    if (!reduce_unary(pr)) {
        return XR_FAIL;
    }
    int prec = cur(pr)->kind == TK_OP ? bin_prec(cur(pr)->text) : 0;
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

/* A whole variable that may be assigned or an array word. */
static bool valid_leaf(const parser *pr, const tb_expr *leaf) {
    return leaf->kind == E_MEM || (leaf->kind == E_VAR && assignable_var(pr, leaf));
}

/* A target is a reg or integer, a select of one, an array word, or a concatenation of those, with
 * at most TB_WR_MAX leaves (so the worklist is bounded). */
static bool valid_lvalue(const parser *pr, const tb_expr *lvalue) {
    enum { WORK_MAX = 2 * TB_WR_MAX };
    const tb_expr *work[WORK_MAX];
    uint32_t top = 0;
    uint32_t leaves = 0;
    work[top++] = lvalue;
    while (top > 0) {
        const tb_expr *cur_lv = work[--top];
        if ((cur_lv->kind == E_CONCAT && top + cur_lv->n > WORK_MAX) || leaves > TB_WR_MAX) {
            return false;
        }
        if (cur_lv->kind == E_CONCAT) {
            for (uint32_t i = 0; i < cur_lv->n; i++) {
                work[top++] = cur_lv->list[i];
            }
            continue;
        }
        leaves++;
        if (cur_lv->kind == E_BITSEL || cur_lv->kind == E_PART) {
            work[top++] = cur_lv->a;
        } else if (!valid_leaf(pr, cur_lv)) {
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
    if (stmt->kind == S_NBA && accept_op(pr, "#")) { /* a <= #d b: the write happens d later */
        stmt->delay = parse_primary(pr);
        if (stmt->delay == NULL) {
            return NULL;
        }
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

/* Skips an optional parenthesised token group, nested parentheses included. */
static bool skip_parens(parser *pr) {
    if (!accept_op(pr, "(")) {
        return true;
    }
    uint32_t depth = 1;
    while (depth > 0) {
        if (cur(pr)->kind == TK_EOF) {
            return fail(pr, "missing )");
        }
        depth += is_op(pr, "(") ? 1U : 0U;
        depth -= is_op(pr, ")") ? 1U : 0U;
        pr->pos++;
    }
    return true;
}

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
    if (strcmp(call->str, "$dumpvars") == 0) { /* ignored: its scope arguments may name modules */
        return skip_parens(pr) ? call : NULL;
    }
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
    if (strcmp(cur(pr)->text, "casez") == 0) {
        stmt->case_kind = TB_CASEZ;
    } else if (strcmp(cur(pr)->text, "casex") == 0) {
        stmt->case_kind = TB_CASEX;
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
    } TABLE[] = {{"begin", KW_BEGIN},       {"if", KW_IF},
                 {"case", KW_CASE},         {"casez", KW_CASE},
                 {"casex", KW_CASE},        {"for", KW_FOR},
                 {"while", KW_WHILE},       {"repeat", KW_REPEAT},
                 {"wait", KW_WAIT},         {"forever", KW_FOREVER},
                 {"reg", KW_DECL},          {"integer", KW_DECL},
                 {"disable", KW_REJECTED},  {"fork", KW_REJECTED},
                 {"deassign", KW_REJECTED}, {"force", KW_REJECTED},
                 {"release", KW_REJECTED},  {NULL, KW_NONE}};
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

/* True when the cursor is at `name (` or `name ;` of a name that is not a variable: a task call. */
static bool at_task_call(const parser *pr) {
    const tb_tok *tok = cur(pr);
    const tb_tok *next = tok + 1;
    return tok->kind == TK_ID && next->kind == TK_OP &&
           (strcmp(next->text, ";") == 0 || strcmp(next->text, "(") == 0) &&
           find_var(pr, tok->text) < 0;
}

/* ( expr {, expr} ) of a task call, when there is an argument list. */
static bool call_args(parser *pr, tb_stmt *stmt) {
    if (!accept_op(pr, "(")) {
        return true;
    }
    do {
        if (stmt->n == TB_MAX_ARGS) {
            return fail(pr, "task call with more than 16 arguments");
        }
        tb_expr *arg = parse_expr(pr);
        if (arg == NULL) {
            return false;
        }
        stmt->args[stmt->n++] = arg;
    } while (accept_op(pr, ","));
    return expect_op(pr, ")");
}

/* name [ ( expr {, expr} ) ] ; : the task is found when the module is complete. */
static tb_stmt *parse_call(parser *pr) {
    tb_stmt *stmt = new_stmt(pr, S_CALL);
    if (stmt == NULL) {
        return NULL;
    }
    stmt->callee = pnew(pr, TB_NAME);
    stmt->args = (tb_expr **)pnew_ptrs(pr, TB_MAX_ARGS);
    if (stmt->callee == NULL || stmt->args == NULL) {
        return NULL;
    }
    char full[QNAME];
    (void)snprintf(full, sizeof full, "%s%s", pr->prefix, cur(pr)->text);
    if (strlen(full) >= TB_NAME) {
        fail(pr, "name longer than 63 characters (instance path included)");
        return NULL;
    }
    memcpy(stmt->callee, full, strlen(full) + 1U);
    pr->pos++;
    tb_stmt **slot = call_args(pr, stmt) ? (tb_stmt **)odin3_vec_push(&pr->calls) : NULL;
    if (slot == NULL || !expect_op(pr, ";")) {
        return NULL;
    }
    *slot = stmt;
    return stmt;
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
    } else if (at_task_call(pr) && keyword_of(pr) == KW_NONE) {
        *out = parse_call(pr);
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
    int64_t first = 0;
    int64_t second = 0;
    pr->pos++;
    if (!const_int(pr, &first) || !expect_op(pr, ":") || !const_int(pr, &second) ||
        !expect_op(pr, "]")) {
        return fail(pr, "array range (use [0:N] or [N:0])");
    }
    int64_t high = first > second ? first : second;
    int64_t low = first > second ? second : first;
    if (low != 0 || high < 0 || high > (1 << ARRAY_MAX_LOG2)) {
        return fail(pr, "array range (use [0:N] or [N:0])");
    }
    pr->tb->vars[var_index].depth = (uint32_t)high + 1U;
    return var_storage(pr, var_index);
}

static bool add_assign(parser *pr, int var, tb_expr *rhs) {
    tb_assign *grown = pnew(pr, ((size_t)pr->tb->nassigns + 1U) * sizeof *grown);
    if (grown == NULL) {
        return false;
    }
    if (pr->tb->nassigns > 0) {
        memcpy(grown, pr->tb->assigns, (size_t)pr->tb->nassigns * sizeof *grown);
    }
    grown[pr->tb->nassigns] = (tb_assign){var, rhs, 0};
    pr->tb->assigns = grown;
    pr->tb->nassigns++;
    return true;
}

/* A string-valued parameter is 8 bits per character, first character in the high byte. */
static bool string_param(parser *pr, int var, const tb_expr *init) {
    tb_var *dst = &pr->tb->vars[var];
    size_t len = strlen(init->str);
    if (len > TB_MAX_WIDTH / CHAR_BITS) {
        return fail(pr, "string parameter longer than 128 characters");
    }
    dst->width = len == 0 ? (uint32_t)CHAR_BITS : (uint32_t)(len * CHAR_BITS);
    dst->sgn = false;
    if (!var_storage(pr, var)) {
        return false;
    }
    tbv val;
    tbv_zero(&val, dst->width);
    for (size_t i = 0; i < len; i++) {
        tbv byte;
        tbv_set_u64((uint8_t)init->str[i], &byte, CHAR_BITS);
        tb_field_put((tb_planes){val.v, val.u, dst->width},
                     (tb_field){(uint32_t)((len - 1U - i) * CHAR_BITS), CHAR_BITS}, &byte);
    }
    tb_var_store(dst, 0, &val);
    return true;
}

/* The value of a reg, integer or parameter declaration's initialiser. A parameter without a range
 * (untyped, not NULL) takes the width and signedness of its value (Verilog-2005 4.10.1). */
static bool var_init(parser *pr, int var, const tb_expr *init, const var_shape *untyped) {
    tb_var *dst = &pr->tb->vars[var];
    if (untyped != NULL) {
        if (init->kind == E_STR) {
            return string_param(pr, var, init);
        }
        dst->width = init->width;
        dst->sgn = untyped->sgn ? true : init->sgn;
        if (!var_storage(pr, var)) {
            return false;
        }
    }
    tbv val;
    if (!tb_const_val(pr->tb, init, dst->width, &val)) {
        return fail(pr, "non-constant initialiser");
    }
    tb_var_store(dst, 0, &val);
    return true;
}

/* The expression a parameter of this instance is overridden with, or NULL (the default stays).
 * Only `parameter` is overridable, positionally in declaration order or by name. */
static tb_expr *param_override(parser *pr, const char *kw, const char *name) {
    if (pr->subs.len == 0 || strcmp(kw, "parameter") != 0) {
        return NULL;
    }
    sub_frame *frm = odin3_vec_at(&pr->subs, pr->subs.len - 1U);
    uint32_t index = frm->nparam++;
    for (uint32_t i = 0; i < frm->nov; i++) {
        bool named = frm->ovs[i].name[0] != '\0';
        if ((named && strcmp(frm->ovs[i].name, name) == 0) || (!named && i == index)) {
            frm->ovs[i].used = true;
            return frm->ovs[i].expr;
        }
    }
    return NULL;
}

/* A reg, wire or integer declaration of a name already declared by direction only (a port, a task
 * argument): it sets the type of that variable. *handled says so. */
static bool refine_var(parser *pr, const char *kw, const char *name, var_shape shape,
                       bool *handled) {
    char full[QNAME];
    (void)snprintf(full, sizeof full, "%s%s%s%s", pr->prefix, pr->scope,
                   pr->scope[0] != '\0' ? "." : "", name);
    *handled = false;
    alias_ent *ali = find_alias(pr, full);
    if (ali != NULL && ali->refinable) {
        ali->refinable = false;
        *handled = true;
        if (ali->is_out && strcmp(kw, "reg") == 0 && pr->tb->four_state) {
            tb_var_fill(&pr->tb->vars[ali->var], (uint32_t)ODIN3_W4_X);
        }
        return true;
    }
    int idx = find_exact(pr, full);
    if (idx < 0 || ali != NULL || !pr->tb->vars[idx].refinable) {
        return true;
    }
    tb_var *var = &pr->tb->vars[idx];
    *handled = true;
    var->refinable = false;
    var->is_wire = shape.is_wire;
    var->sgn = var->sgn || shape.sgn;
    if (strcmp(kw, "integer") == 0 || shape.ranged) {
        var->width = shape.width;
        var->lo = shape.lo;
    }
    return var_storage(pr, idx);
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
    bool refined = false;
    if (!is_param && !refine_var(pr, kw, name, shape, &refined)) {
        return false;
    }
    int var = refined ? find_var(pr, name) : add_var(pr, name, shape);
    if (var < 0) {
        return false;
    }
    pr->tb->vars[var].is_param = is_param;
    if (is_op(pr, "[") && !refined) {
        return parse_array_range(pr, var);
    }
    if (!accept_op(pr, "=")) {
        return !is_param || fail(pr, "parameter without a value");
    }
    tb_expr *over = param_override(pr, kw, name);
    tb_expr *init = parse_expr(pr);
    if (init == NULL) {
        return false;
    }
    if (over != NULL) {
        init = over;
    }
    if (shape.is_wire) {
        return add_assign(pr, var, init);
    }
    return var_init(pr, var, init, is_param && shape.width == 0U ? &shape : NULL);
}

/* After the declaration keyword kw: [signed] [range] into shape. */
static bool decl_shape(parser *pr, const char *kw, var_shape *shape) {
    bool is_int = strcmp(kw, "integer") == 0;
    /* A parameter without a range has the type of its value (width 0 here, set from the
     * initialiser). */
    bool is_param = strcmp(kw, "parameter") == 0 || strcmp(kw, "localparam") == 0;
    bool is_signed = accept_kw(pr, "signed");
    bool is_ranged = is_op(pr, "[");
    bool wide = is_int || (is_param && !is_ranged);
    uint32_t base = wide ? (uint32_t)TB_INT_BITS : 1U;
    if (is_param && !is_ranged) {
        base = 0U;
    }
    *shape = (var_shape){.width = base, .sgn = (wide && !is_param) || is_signed};
    return !is_ranged || parse_range(pr, shape);
}

static bool parse_decl(parser *pr) {
    char kw[TB_NAME];
    (void)snprintf(kw, sizeof kw, "%s", cur(pr)->text);
    pr->pos++;
    var_shape shape;
    if (!decl_shape(pr, kw, &shape)) {
        return false;
    }
    do {
        if (!parse_decl_item(pr, kw, shape)) {
            return false;
        }
    } while (accept_op(pr, ","));
    return expect_op(pr, ";");
}

/* --- tasks --- */

static tb_task *add_task(parser *pr, const char *name) {
    tb_module *tb = pr->tb;
    for (uint32_t i = 0; i < tb->ntasks; i++) {
        if (strcmp(tb->tasks[i].name, name) == 0) {
            fail(pr, "task declared twice");
            return NULL;
        }
    }
    tb_task *grown = pnew(pr, ((size_t)tb->ntasks + 1U) * sizeof *grown);
    if (grown == NULL) {
        return NULL;
    }
    if (tb->ntasks > 0) {
        memcpy(grown, tb->tasks, (size_t)tb->ntasks * sizeof *grown);
    }
    tb->tasks = grown;
    tb_task *task = &tb->tasks[tb->ntasks++];
    memset(task, 0, sizeof *task);
    (void)snprintf(task->name, sizeof task->name, "%s", name);
    task->line = cur(pr)->line;
    task->vars = pnew(pr, TB_MAX_ARGS * sizeof *task->vars);
    task->dir = pnew(pr, TB_MAX_ARGS);
    return task->vars != NULL && task->dir != NULL ? task : NULL;
}

/* One formal argument of a task: variable index and direction. */
typedef struct task_formal {
    int var;
    uint8_t dir;
} task_formal;

/* Records the formal as the next argument of the task. */
static bool task_arg(parser *pr, tb_task *task, task_formal formal) {
    if (task->nargs == TB_MAX_ARGS) {
        return fail(pr, "task with more than 16 arguments");
    }
    task->vars[task->nargs] = formal.var;
    task->dir[task->nargs++] = formal.dir;
    return true;
}

static uint8_t dir_of(const parser *pr) {
    if (is_kw(pr, "input")) {
        return TB_ARG_IN;
    }
    return is_kw(pr, "output") ? (uint8_t)TB_ARG_OUT : (uint8_t)(TB_ARG_IN | TB_ARG_OUT);
}

/* [reg|integer] [signed] [range] name {, name} after the direction keyword. In the ANSI form
 * (ansi) the list ends before a direction keyword or `)`, in the old form at `;`. */
static bool task_port_decl(parser *pr, tb_task *task, uint8_t dir, bool ansi) {
    bool is_int = accept_kw(pr, "integer");
    bool is_reg = accept_kw(pr, "reg");
    bool is_signed = accept_kw(pr, "signed");
    var_shape shape = {.width = is_int ? (uint32_t)TB_INT_BITS : 1U,
                       .sgn = is_int || is_signed,
                       .refinable = !is_int && !is_reg};
    if (is_op(pr, "[") && !parse_range(pr, &shape)) {
        return false;
    }
    do {
        if (cur(pr)->kind != TK_ID) {
            return fail(pr, "task argument declaration");
        }
        int var = add_var(pr, cur(pr)->text, shape);
        pr->pos++;
        if (var < 0 || !task_arg(pr, task, (task_formal){var, dir})) {
            return false;
        }
    } while (accept_op(pr, ",") &&
             !(ansi && (is_kw(pr, "input") || is_kw(pr, "output") || is_kw(pr, "inout"))));
    return ansi || expect_op(pr, ";");
}

static bool task_ansi_ports(parser *pr, tb_task *task) {
    while (!is_op(pr, ")")) {
        if (!(is_kw(pr, "input") || is_kw(pr, "output") || is_kw(pr, "inout"))) {
            return fail(pr, "task argument list (input, output or inout expected)");
        }
        uint8_t dir = dir_of(pr);
        pr->pos++;
        if (!task_port_decl(pr, task, dir, true)) {
            return false;
        }
    }
    return expect_op(pr, ")");
}

/* Appends child to the block's statement list (parse memory). */
static bool block_append(parser *pr, tb_stmt *block, tb_stmt *child) {
    tb_stmt **grown = (tb_stmt **)pdup_ptrs(pr, (const void *)block->list, block->n);
    if (grown == NULL) {
        return false;
    }
    grown[block->n++] = child;
    block->list = grown;
    return true;
}

/* The statements of a task up to `endtask`, as one block. */
static tb_stmt *task_body(parser *pr) {
    tb_stmt *body = new_stmt(pr, S_BLOCK);
    while (body != NULL && !pr->failed && !accept_kw(pr, "endtask")) {
        if (cur(pr)->kind == TK_EOF) {
            fail(pr, "missing endtask");
            return NULL;
        }
        tb_stmt *stmt = parse_stmt(pr);
        if (stmt == NULL || !block_append(pr, body, stmt)) {
            return NULL;
        }
    }
    return body;
}

/* [( ansi ports )] ; {input/output/inout declarations} */
static bool task_header(parser *pr, tb_task *task) {
    if (accept_op(pr, "(") && !task_ansi_ports(pr, task)) {
        return false;
    }
    if (!expect_op(pr, ";")) {
        return false;
    }
    while (is_kw(pr, "input") || is_kw(pr, "output") || is_kw(pr, "inout")) {
        uint8_t dir = dir_of(pr);
        pr->pos++;
        if (!task_port_decl(pr, task, dir, false)) {
            return false;
        }
    }
    return true;
}

static bool parse_task(parser *pr) {
    pr->pos++; /* task */
    (void)accept_kw(pr, "automatic");
    if (cur(pr)->kind != TK_ID) {
        return fail(pr, "task name");
    }
    char local[TB_NAME];
    char full[QNAME];
    (void)snprintf(local, sizeof local, "%s", cur(pr)->text);
    (void)snprintf(full, sizeof full, "%s%s", pr->prefix, local);
    if (strlen(full) >= TB_NAME) {
        return fail(pr, "name longer than 63 characters (instance path included)");
    }
    tb_task *task = add_task(pr, full);
    if (task == NULL) {
        return false;
    }
    pr->pos++;
    (void)snprintf(pr->scope, sizeof pr->scope, "%s", local);
    task->body = task_header(pr, task) ? task_body(pr) : NULL;
    pr->scope[0] = '\0';
    return task->body != NULL && !pr->failed;
}

/* Gives one task call its task, checking the number of arguments and that every output
 * argument is something that can be written. */
static bool link_one(const parser *pr, tb_stmt *call) {
    const tb_module *tb = pr->tb;
    call->task = -1;
    for (uint32_t idx = 0; idx < tb->ntasks; idx++) {
        if (strcmp(tb->tasks[idx].name, call->callee) == 0) {
            call->task = (int)idx;
        }
    }
    if (call->task < 0) {
        tb_error(pr->file, call->line, "call of a task that is not defined in the testbench");
        return false;
    }
    const tb_task *task = &tb->tasks[call->task];
    if (call->n != task->nargs) {
        tb_error(pr->file, call->line, "task call with the wrong number of arguments");
        return false;
    }
    for (uint32_t arg = 0; arg < call->n; arg++) {
        if ((task->dir[arg] & TB_ARG_OUT) != 0 && !valid_lvalue(pr, call->args[arg])) {
            tb_error(pr->file, call->line, "task output argument that is not a reg or select");
            return false;
        }
    }
    return true;
}

static bool link_calls(parser *pr) {
    for (size_t i = 0; i < pr->calls.len; i++) {
        if (!link_one(pr, *(tb_stmt **)odin3_vec_at(&pr->calls, i))) {
            return false;
        }
    }
    return true;
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

/* Room for one more connection: the array doubles (up to INST_CONNS_MAX) in the parse arena. */
static bool grow_conns(parser *pr, tb_inst *inst, uint32_t *cap) {
    if (inst->nconns < *cap) {
        return true;
    }
    if (*cap >= INST_CONNS_MAX) {
        return fail(pr, "more than 4096 port connections");
    }
    uint32_t bigger = *cap * 2U;
    tb_conn *grown = pnew(pr, (size_t)bigger * sizeof *grown);
    if (grown == NULL) {
        return false;
    }
    memcpy(grown, inst->conns, (size_t)inst->nconns * sizeof *grown);
    inst->conns = grown;
    *cap = bigger;
    return true;
}

static bool parse_inst_conns(parser *pr, tb_inst *inst) {
    uint32_t cap = INST_CONNS_FIRST;
    inst->conns = pnew(pr, (size_t)cap * sizeof *inst->conns);
    if (inst->conns == NULL || !expect_op(pr, "(")) {
        return false;
    }
    while (!accept_op(pr, ")")) {
        if (!grow_conns(pr, inst, &cap) || !parse_conn(pr, &inst->conns[inst->nconns++])) {
            return false;
        }
    }
    return true;
}

/* The slot of the next instance item: one the first pass made (the second pass knows every
 * instance, so a hierarchical name may come before the instance item), else a new one. */
static tb_inst *next_inst_slot(parser *pr) {
    tb_module *tb = pr->tb;
    if (pr->next_inst < tb->ninsts) {
        return &tb->insts[pr->next_inst++];
    }
    tb_inst *grown = pnew(pr, ((size_t)tb->ninsts + 1U) * sizeof *grown);
    if (grown == NULL) {
        return NULL;
    }
    if (tb->ninsts > 0) {
        memcpy(grown, tb->insts, (size_t)tb->ninsts * sizeof *grown);
    }
    tb->insts = grown;
    pr->next_inst++;
    return &grown[tb->ninsts++];
}

/* --- testbench-side module instances (design note docs/specs/2026-11-07-sim-tb-submodules.md) --- */

enum { PORT_IN = 1, PORT_OUT = 2 };

static mod_def *find_mod(const parser *pr, const char *name) {
    for (size_t i = 0; i < pr->mods.len; i++) {
        mod_def *def = odin3_vec_at((odin3_vec *)&pr->mods, i);
        if (strcmp(def->name, name) == 0) {
            return def;
        }
    }
    return NULL;
}

/* The module at the cursor is testbench-side: its header is in the testbench files and the design
 * has no module of that name (the design wins on a clash, with an INFO line). */
static mod_def *tb_side_module(parser *pr) {
    mod_def *def = find_mod(pr, cur(pr)->text);
    if (def == NULL) {
        return NULL;
    }
    if (tb_design_has_module(pr->tb->design, def->name)) {
        if (!def->informed && pr->lenient) { /* the first pass always runs */
            odin3_log(ODIN3_LOG_INFO,
                      "sim_tb: module %s is in the design and in the testbench files: the design "
                      "is used",
                      def->name);
            def->informed = true;
        }
        return NULL;
    }
    return def;
}

static sub_frame *top_sub(parser *pr) {
    return pr->subs.len > 0 ? odin3_vec_at(&pr->subs, pr->subs.len - 1U) : NULL;
}

static void set_prefix(parser *pr) {
    const sub_frame *frm = top_sub(pr);
    (void)snprintf(pr->prefix, sizeof pr->prefix, "%s", frm != NULL ? frm->prefix : "");
}

/* `#( expr, .name(expr) )` of an instance: the overrides, parsed in the parent scope. */
static bool parse_overrides(parser *pr, sub_frame *frm) {
    sub_ov list[SUB_OVERRIDES];
    uint32_t count = 0;
    pr->pos++;
    if (!expect_op(pr, "(")) {
        return false;
    }
    do {
        if (count == SUB_OVERRIDES) {
            return fail(pr, "more than 32 parameter overrides");
        }
        list[count] = (sub_ov){0};
        if (accept_op(pr, ".")) {
            if (cur(pr)->kind != TK_ID) {
                return fail(pr, "parameter override syntax");
            }
            (void)snprintf(list[count].name, sizeof list[count].name, "%s", cur(pr)->text);
            pr->pos++;
            if (!expect_op(pr, "(")) {
                return false;
            }
        }
        list[count].expr = parse_expr(pr);
        if (list[count++].expr == NULL || (list[count - 1U].name[0] != '\0' && !expect_op(pr, ")"))) {
            return false;
        }
    } while (accept_op(pr, ","));
    frm->ovs = pnew(pr, count * sizeof *frm->ovs);
    if (frm->ovs == NULL || !expect_op(pr, ")")) {
        return false;
    }
    memcpy(frm->ovs, list, count * sizeof *frm->ovs);
    frm->nov = count;
    return true;
}

/* The connection of port number index called name, or NULL (not connected). Connections are all
 * named or all ordered. */
static tb_conn *conn_of(sub_frame *frm, uint32_t index, const char *name) {
    if (frm->nconns > 0 && frm->conns[0].port[0] == '\0') {
        return index < frm->nconns ? &frm->conns[index] : NULL;
    }
    for (uint32_t i = 0; i < frm->nconns; i++) {
        if (strcmp(frm->conns[i].port, name) == 0) {
            return &frm->conns[i];
        }
    }
    return NULL;
}

static int port_index(const sub_frame *frm, const char *name) {
    for (uint32_t i = 0; i < frm->nports; i++) {
        if (strcmp(frm->ports[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* The port is connected to a whole variable of the parent scope with the same width: the port name
 * becomes another name of that variable (the port and the connection are one net, as in iverilog:
 * a clock passed through a port reaches the module's `@(posedge clk)` in the same delta). */
static bool alias_port(parser *pr, const sub_port *port, const tb_conn *conn, var_shape shape) {
    const tb_expr *expr = conn != NULL ? conn->expr : NULL;
    if (expr == NULL || expr->kind != E_VAR) {
        return false;
    }
    const tb_var *target = &pr->tb->vars[expr->var];
    if (target->is_param || target->depth != 0 || target->width != shape.width) {
        return false;
    }
    char full[QNAME];
    (void)snprintf(full, sizeof full, "%s%s", pr->prefix, port->name);
    alias_ent *ent = odin3_vec_push(&pr->aliases);
    if (ent == NULL || strlen(full) >= TB_NAME) {
        fail(pr, ent == NULL ? "out of memory" : "name longer than 63 characters");
        return false;
    }
    *ent = (alias_ent){.var = expr->var, .refinable = shape.refinable, .is_out = port->dir == PORT_OUT};
    memcpy(ent->name, full, strlen(full) + 1U);
    if (port->dir == PORT_OUT && !shape.is_wire && pr->tb->four_state) {
        tb_var_fill(&pr->tb->vars[expr->var], (uint32_t)ODIN3_W4_X); /* an output reg starts x */
    }
    return true;
}

/* Declares port `name` of the open instance with direction dir and shape. */
static bool declare_port(parser *pr, const char *name, uint8_t dir, var_shape shape) {
    sub_frame *frm = top_sub(pr);
    int idx = port_index(frm, name);
    if (idx < 0 && !frm->ansi) {
        return fail(pr, "port declaration of a name that is not in the module's port list");
    }
    if (idx < 0) {
        if (frm->nports == SUB_PORTS) {
            return fail(pr, "module with more than 128 ports");
        }
        idx = (int)frm->nports++;
        (void)snprintf(frm->ports[idx].name, sizeof frm->ports[idx].name, "%s", name);
    }
    sub_port *port = &frm->ports[idx];
    port->dir = dir;
    port->var = -1;
    shape.is_wire = dir == PORT_IN || shape.is_wire;
    tb_conn *conn = conn_of(frm, (uint32_t)idx, name);
    if (alias_port(pr, port, conn, shape)) {
        port->aliased = true;
        return true;
    }
    if (pr->failed) {
        return false;
    }
    port->var = add_var(pr, name, shape);
    return port->var >= 0;
}

static uint8_t port_dir(const parser *pr) {
    return is_kw(pr, "input") ? (uint8_t)PORT_IN : (uint8_t)PORT_OUT;
}

static bool is_dir_kw(const parser *pr) {
    return is_kw(pr, "input") || is_kw(pr, "output") || is_kw(pr, "inout");
}

/* After the direction: [wire|reg|integer] [signed] [range] into shape. */
static bool port_shape(parser *pr, uint8_t dir, var_shape *shape) {
    bool is_int = accept_kw(pr, "integer");
    bool is_reg = accept_kw(pr, "reg");
    bool is_wire = accept_kw(pr, "wire");
    bool is_signed = accept_kw(pr, "signed");
    *shape = (var_shape){.width = is_int ? (uint32_t)TB_INT_BITS : 1U,
                         .sgn = is_int || is_signed,
                         .is_wire = dir == PORT_IN || !(is_reg || is_int),
                         .refinable = !(is_int || is_reg || is_wire)};
    return !is_op(pr, "[") || parse_range(pr, shape);
}

/* A direction keyword of a port: inout is refused. */
static bool port_dir_ok(parser *pr) {
    return !is_kw(pr, "inout") || fail(pr, "inout port of a testbench module");
}

/* `input|output [kind] [range] a, b;` in the body (old-style port declaration). */
static bool parse_port_decl(parser *pr) {
    if (!port_dir_ok(pr)) {
        return false;
    }
    uint8_t dir = port_dir(pr);
    var_shape shape;
    pr->pos++;
    if (!port_shape(pr, dir, &shape)) {
        return false;
    }
    do {
        if (cur(pr)->kind != TK_ID) {
            return fail(pr, "port declaration syntax");
        }
        char name[TB_NAME];
        (void)snprintf(name, sizeof name, "%s", cur(pr)->text);
        pr->pos++;
        if (!declare_port(pr, name, dir, shape)) {
            return false;
        }
    } while (accept_op(pr, ","));
    return expect_op(pr, ";");
}

/* ANSI port list after `(`: `input wire [7:0] a, b, output reg c`. */
static bool header_ansi_ports(parser *pr) {
    uint8_t dir = 0;
    var_shape shape = {0};
    do {
        if (is_dir_kw(pr)) {
            if (!port_dir_ok(pr)) {
                return false;
            }
            dir = port_dir(pr);
            pr->pos++;
            if (!port_shape(pr, dir, &shape)) {
                return false;
            }
        }
        if (dir == 0 || cur(pr)->kind != TK_ID) {
            return fail(pr, "port list syntax");
        }
        char name[TB_NAME];
        (void)snprintf(name, sizeof name, "%s", cur(pr)->text);
        pr->pos++;
        if (!declare_port(pr, name, dir, shape)) {
            return false;
        }
    } while (accept_op(pr, ","));
    return true;
}

/* Port list after `(`: names only (old style, the directions come in the body) or ANSI. */
static bool header_ports(parser *pr) {
    sub_frame *frm = top_sub(pr);
    if (accept_op(pr, ")")) {
        return true;
    }
    frm->ansi = is_dir_kw(pr);
    if (frm->ansi) {
        return header_ansi_ports(pr) && expect_op(pr, ")");
    }
    do {
        if (cur(pr)->kind != TK_ID || frm->nports == SUB_PORTS) {
            return fail(pr, "port list syntax");
        }
        sub_port *port = &frm->ports[frm->nports++];
        port->var = -1;
        (void)snprintf(port->name, sizeof port->name, "%s", cur(pr)->text);
        pr->pos++;
    } while (accept_op(pr, ","));
    return expect_op(pr, ")");
}

/* `#( parameter a = 1, b = 2 )` of the header. */
static bool header_params(parser *pr) {
    var_shape shape = {0};
    pr->pos++;
    if (!expect_op(pr, "(")) {
        return false;
    }
    do {
        if (accept_kw(pr, "parameter") && !decl_shape(pr, "parameter", &shape)) {
            return false;
        }
        if (!parse_decl_item(pr, "parameter", shape)) {
            return false;
        }
    } while (accept_op(pr, ","));
    return expect_op(pr, ")");
}

/* Every connection names a port of the module, and there are no more ordered ones than ports. */
static bool check_conns(parser *pr, const sub_frame *frm) {
    char msg[(2 * TB_NAME) + MSG_EXTRA];
    for (uint32_t i = 0; i < frm->nconns; i++) {
        const char *port = frm->conns[i].port;
        if (port[0] != '\0' && port_index(frm, port) < 0) {
            (void)snprintf(msg, sizeof msg, "module %s has no port %s", frm->module, port);
            return fail(pr, msg);
        }
    }
    if (frm->nconns > frm->nports && frm->nconns > 0 && frm->conns[0].port[0] == '\0') {
        (void)snprintf(msg, sizeof msg, "more connections than ports in module %s", frm->module);
        return fail(pr, msg);
    }
    return true;
}

/* The header of the module the cursor is on: `module name [#(...)] [(ports)] ;`. */
static bool parse_sub_header(parser *pr) {
    pr->pos += 2U; /* module name */
    if (is_op(pr, "#") && !header_params(pr)) {
        return false;
    }
    sub_frame *frm = top_sub(pr);
    frm->ports = pnew(pr, SUB_PORTS * sizeof *frm->ports);
    if (frm->ports == NULL) {
        return false;
    }
    if (accept_op(pr, "(") && !header_ports(pr)) {
        return false;
    }
    return expect_op(pr, ";") && check_conns(pr, top_sub(pr));
}

/* Pushes the frame and moves the cursor to the module's body. */
static bool push_sub(parser *pr, const sub_frame *frm, const mod_def *def, const char *inst) {
    if (pr->subs.len >= SUB_DEPTH) {
        return fail(pr, "recursive module instance (or nesting deeper than 16)");
    }
    if (strcmp(def->name, pr->tb->name) == 0) {
        return fail(pr, "recursive module instance");
    }
    for (size_t i = 0; i < pr->subs.len; i++) {
        const sub_frame *open = odin3_vec_at(&pr->subs, i);
        if (strcmp(open->module, def->name) == 0) {
            return fail(pr, "recursive module instance");
        }
    }
    sub_frame *slot = odin3_vec_push(&pr->subs);
    if (slot == NULL) {
        return fail(pr, "out of memory");
    }
    *slot = *frm;
    if (snprintf(slot->prefix, sizeof slot->prefix, "%s%s.", pr->prefix, inst) >=
        (int)sizeof slot->prefix) {
        odin3_vec_pop(&pr->subs);
        return fail(pr, "name longer than 63 characters (instance path)");
    }
    set_prefix(pr);
    pr->pos = def->pos;
    return parse_sub_header(pr);
}

/* A testbench-side instance: `mod [#(...)] name (conns);` The parent-scope parts are parsed now,
 * then the cursor jumps to the module; finish_sub returns to the token after the statement. */
static bool parse_sub_instance(parser *pr, const mod_def *def) {
    sub_frame frm = {.line = cur(pr)->line};
    (void)snprintf(frm.module, sizeof frm.module, "%s", def->name);
    pr->pos++;
    if (is_op(pr, "#") && !parse_overrides(pr, &frm)) {
        return false;
    }
    if (cur(pr)->kind != TK_ID) {
        return fail(pr, "module item");
    }
    char inst[TB_NAME];
    (void)snprintf(inst, sizeof inst, "%s", cur(pr)->text);
    pr->pos++;
    if (is_op(pr, "[")) {
        return fail(pr, "instance array of a testbench module");
    }
    tb_inst conns = {.nconns = 0};
    if (!parse_inst_conns(pr, &conns) || !expect_op(pr, ";")) {
        return false;
    }
    frm.conns = conns.conns;
    frm.nconns = conns.nconns;
    frm.ret_pos = pr->pos;
    return push_sub(pr, &frm, def, inst);
}

/* The port of the frame becomes a continuous assignment to or from its connection. */
static bool connect_port(parser *pr, sub_frame *frm, uint32_t index) {
    sub_port *port = &frm->ports[index];
    const tb_conn *conn = conn_of(frm, index, port->name);
    if (port->aliased || conn == NULL || conn->expr == NULL) {
        return true;
    }
    if (port->var < 0) {
        return fail(pr, "connected port without a direction declaration");
    }
    if (port->dir == PORT_IN) {
        return add_assign(pr, port->var, conn->expr);
    }
    const tb_var *target = conn->expr->kind == E_VAR ? &pr->tb->vars[conn->expr->var] : NULL;
    if (target == NULL || !target->is_wire || target->depth != 0) {
        return fail(pr, "output port connected to anything but a whole wire");
    }
    tb_expr *src = pnew(pr, sizeof *src);
    if (src == NULL) {
        return false;
    }
    src->kind = E_VAR;
    src->var = port->var;
    src->width = pr->tb->vars[port->var].width;
    src->sgn = pr->tb->vars[port->var].sgn;
    src->line = conn->expr->line;
    return add_assign(pr, conn->expr->var, src);
}

/* `endmodule` of an instance: ports to assignments, then back to the statement after it. */
static bool finish_sub(parser *pr) {
    sub_frame *frm = top_sub(pr);
    char msg[(2 * TB_NAME) + MSG_EXTRA];
    for (uint32_t i = 0; i < frm->nports; i++) {
        if (!connect_port(pr, frm, i)) {
            return false;
        }
    }
    for (uint32_t i = 0; i < frm->nov; i++) {
        if (frm->ovs[i].name[0] != '\0' ? !frm->ovs[i].used : i >= frm->nparam) {
            (void)snprintf(msg, sizeof msg, "module %s has no parameter %s", frm->module,
                           frm->ovs[i].name[0] != '\0' ? frm->ovs[i].name : "for a positional override");
            return fail(pr, msg);
        }
    }
    pr->pos = frm->ret_pos;
    odin3_vec_pop(&pr->subs);
    set_prefix(pr);
    return true;
}

static bool parse_instance(parser *pr) {
    mod_def *def = tb_side_module(pr);
    if (def != NULL) {
        return parse_sub_instance(pr, def);
    }
    if (pr->subs.len > 0) {
        return fail(pr, "design module instance inside a testbench module");
    }
    tb_inst *inst = next_inst_slot(pr);
    if (inst == NULL) {
        return false;
    }
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

/* A module item keyword outside the subset. */
static bool is_rejected_item(const parser *pr) {
    static const char *const REJECT[] = {"genvar", "generate", "function", "real",    "time",
                                         "string", "event",    "defparam", "specify", "input",
                                         "output", "inout",    "and",      "or",      "nand",
                                         "nor",    "xor",      "not",      "buf",     NULL};
    for (size_t i = 0; REJECT[i] != NULL; i++) {
        if (is_kw(pr, REJECT[i])) {
            return true;
        }
    }
    return false;
}

/* `pullup(w);` or `pulldown(w);`: a wire whose z bits read 1 or 0 (a weak driver: any other driver
 * wins; strengths and delays on the gate are not parsed). */
static bool parse_pull(parser *pr, uint8_t pull) {
    pr->pos++;
    if (!expect_op(pr, "(") || cur(pr)->kind != TK_ID) {
        return fail(pr, "pullup/pulldown syntax (pullup(wire);)");
    }
    int var = find_var(pr, cur(pr)->text);
    if (var < 0) { /* an undeclared name is an implicit one-bit wire (IEEE 1364 4.5) */
        var = add_var(pr, cur(pr)->text, (var_shape){.width = 1, .is_wire = true});
    }
    if (var < 0 || !pr->tb->vars[var].is_wire) {
        return fail(pr, "pullup/pulldown of anything but a wire");
    }
    pr->tb->vars[var].pull = pull;
    if (pr->tb->four_state) { /* the wire starts z: it now reads as pulled */
        tb_var_fill(&pr->tb->vars[var], (uint32_t)ODIN3_W4_Z);
    }
    pr->pos++;
    return expect_op(pr, ")") && expect_op(pr, ";");
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
    if (is_kw(pr, "task")) {
        return parse_task(pr);
    }
    if (is_kw(pr, "assign")) {
        return parse_assign_item(pr);
    }
    if (is_kw(pr, "pullup") || is_kw(pr, "pulldown")) {
        return parse_pull(pr, is_kw(pr, "pullup") ? TB_PULL_UP : TB_PULL_DOWN);
    }
    if (pr->subs.len > 0 && is_dir_kw(pr)) {
        return parse_port_decl(pr);
    }
    if (is_rejected_item(pr)) {
        return fail(pr, tok->text);
    }
    return parse_instance(pr);
}

/* Moves pr->pos to the `module` token of `top` (the first module when top is NULL), so a bench
 * of several files can name its top module wherever it stands among the other modules. */
static void locate_top(parser *pr, const char *top, uint32_t ntoks) {
    if (top == NULL) {
        return;
    }
    for (uint32_t i = 0; i + 1U < ntoks; i++) {
        const tb_tok *tok = &pr->toks[i];
        if (tok->kind == TK_ID && strcmp(tok->text, "module") == 0 &&
            pr->toks[i + 1U].kind == TK_ID && strcmp(pr->toks[i + 1U].text, top) == 0) {
            pr->pos = i;
            return;
        }
    }
    pr->pos = 0;
}

static bool parse_header(parser *pr, const char *top) {
    if (!accept_kw(pr, "module") || cur(pr)->kind != TK_ID) {
        return fail(pr, "file does not start with a module");
    }
    (void)snprintf(pr->tb->name, sizeof pr->tb->name, "%s", cur(pr)->text);
    pr->pos++;
    if (top != NULL && strcmp(top, pr->tb->name) != 0) {
        return fail(pr, "--top names a module that is not in the testbench files");
    }
    if (accept_op(pr, "(") && !expect_op(pr, ")")) {
        return false;
    }
    return expect_op(pr, ";");
}

static void parse_items(parser *pr) {
    while (!pr->failed) {
        if (accept_kw(pr, "endmodule")) {
            if (pr->subs.len == 0) {
                return;
            }
            (void)finish_sub(pr);
        } else if (cur(pr)->kind == TK_EOF) {
            fail(pr, "missing endmodule");
        } else if (is_kw(pr, "module")) {
            fail(pr, "module inside a module");
        } else {
            (void)parse_item(pr);
        }
    }
}

/* The module index: where every `module name` of the testbench files starts. */
static bool index_modules(parser *pr, uint32_t ntoks) {
    for (uint32_t i = 0; i + 1U < ntoks; i++) {
        const tb_tok *tok = &pr->toks[i];
        if (tok->kind != TK_ID || strcmp(tok->text, "module") != 0 || pr->toks[i + 1U].kind != TK_ID) {
            continue;
        }
        mod_def *def = odin3_vec_push(&pr->mods);
        if (def == NULL) {
            return false;
        }
        *def = (mod_def){.pos = i};
        (void)snprintf(def->name, sizeof def->name, "%s", pr->toks[i + 1U].text);
    }
    return true;
}

/* Parses src once. `pre` (may be NULL) is the module of a first pass: its instances are entered
 * before the items are read. */
static odin3_status parse_once(const tb_source *src, const char *top, const tb_module *pre,
                               bool lenient, tb_module **out) {
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
    tb->design = src->design;
    tb->four_state = src->four_state;
    parser pr = {.tb = tb, .toks = lexed.toks, .file = src->file, .lenient = lenient};
    odin3_vec_init(&pr.ents, sizeof(xent));
    odin3_vec_init(&pr.vals, sizeof(tb_expr *));
    odin3_vec_init(&pr.frames, sizeof(sframe));
    odin3_vec_init(&pr.calls, sizeof(tb_stmt *));
    odin3_vec_init(&pr.mods, sizeof(mod_def));
    odin3_vec_init(&pr.subs, sizeof(sub_frame));
    odin3_vec_init(&pr.aliases, sizeof(alias_ent));
    pr.failed = !index_modules(&pr, lexed.n);
    if (pre != NULL && pre->ninsts > 0) {
        tb->insts = pnew(&pr, (size_t)pre->ninsts * sizeof *tb->insts);
        for (uint32_t i = 0; tb->insts != NULL && i < pre->ninsts; i++) {
            tb->insts[i] = (tb_inst){.line = pre->insts[i].line};
            memcpy(tb->insts[i].name, pre->insts[i].name, sizeof tb->insts[i].name);
            memcpy(tb->insts[i].module, pre->insts[i].module, sizeof tb->insts[i].module);
        }
        tb->ninsts = tb->insts != NULL ? pre->ninsts : 0U;
    }
    locate_top(&pr, top, lexed.n);
    if (!pr.failed && parse_header(&pr, top)) {
        parse_items(&pr);
    }
    odin3_vec_free(&pr.ents);
    odin3_vec_free(&pr.vals);
    odin3_vec_free(&pr.frames);
    odin3_vec_free(&pr.mods);
    odin3_vec_free(&pr.subs);
    odin3_vec_free(&pr.aliases);
    if (!pr.failed && !link_calls(&pr)) {
        pr.failed = true;
    }
    odin3_vec_free(&pr.calls);
    if (pr.failed) {
        tb_module_free(tb);
        return ODIN3_ERR_INVALID_ARG;
    }
    tb->saw_hier = pr.saw_hier;
    *out = tb;
    return ODIN3_OK;
}

odin3_status tb_parse(const tb_source *src, const char *top, tb_module **out) {
    tb_module *first = NULL;
    odin3_status st = parse_once(src, top, NULL, true, &first);
    if (st != ODIN3_OK || !first->saw_hier) {
        *out = first;
        return st;
    }
    /* A hierarchical name needs the DUT's signals, so the instance items must be known before
     * the name is read: the first pass found them, the second resolves the names. */
    tb_module *second = NULL;
    st = parse_once(src, top, first, false, &second);
    tb_module_free(first);
    *out = second;
    return st;
}

void tb_module_free(tb_module *tb) {
    if (tb == NULL) {
        return;
    }
    for (uint32_t i = 0; i < tb->ninsts; i++) {
        tb_inst_release(&tb->insts[i]);
    }
    for (uint32_t i = 0; i < tb->nallocs; i++) {
        free(tb->allocs[i]);
    }
    free((void *)tb->allocs);
    free(tb);
}
