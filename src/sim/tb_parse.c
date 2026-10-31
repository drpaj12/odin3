/* tb_parse.c — the testbench parser: module items, statements, expressions (spec section 2). */
#include "sim/tb_internal.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>

typedef struct ps {
    tb_module *tb;
    const tb_tok *t;
    uint32_t pos;
    const char *file;
    bool failed;
} ps;

static const tb_tok *cur(const ps *p) {
    return &p->t[p->pos];
}

static bool fail(ps *p, const char *what) {
    if (!p->failed) {
        tb_error(p->file, cur(p)->line, what);
    }
    p->failed = true;
    return false;
}

static bool is_op(const ps *p, const char *op) {
    return cur(p)->kind == TK_OP && strcmp(cur(p)->text, op) == 0;
}

static bool is_kw(const ps *p, const char *kw) {
    return cur(p)->kind == TK_ID && strcmp(cur(p)->text, kw) == 0;
}

static bool accept_op(ps *p, const char *op) {
    if (is_op(p, op)) {
        p->pos++;
        return true;
    }
    return false;
}

static bool accept_kw(ps *p, const char *kw) {
    if (is_kw(p, kw)) {
        p->pos++;
        return true;
    }
    return false;
}

static bool expect_op(ps *p, const char *op) {
    if (accept_op(p, op)) {
        return true;
    }
    char msg[TB_NAME + 32];
    (void)snprintf(msg, sizeof msg, "expected `%s`", op);
    return fail(p, msg);
}

static void *pnew(ps *p, size_t size) {
    void *ptr = tb_alloc(p->tb, size);
    if (ptr == NULL) {
        fail(p, "out of memory");
    }
    return ptr;
}

static tb_expr *new_expr(ps *p, tb_ekind kind) {
    tb_expr *e = pnew(p, sizeof *e);
    if (e != NULL) {
        e->kind = kind;
        e->line = cur(p)->line;
    }
    return e;
}

/* --- variables --- */
static int find_var(const ps *p, const char *name) {
    for (uint32_t i = 0; i < p->tb->nvars; i++) {
        if (strcmp(p->tb->vars[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int add_var(ps *p, const char *name, uint32_t width, bool sgn, bool is_wire) {
    tb_module *tb = p->tb;
    if (find_var(p, name) >= 0) {
        fail(p, "redeclared name");
        return -1;
    }
    if (tb->nvars == tb->cap_vars) {
        uint32_t cap = tb->cap_vars == 0 ? 32U : tb->cap_vars * 2U;
        tb_var *grown = pnew(p, cap * sizeof *grown);
        if (grown == NULL) {
            return -1;
        }
        if (tb->nvars > 0) {
            memcpy(grown, tb->vars, tb->nvars * sizeof *grown);
        }
        tb->vars = grown;
        tb->cap_vars = cap;
    }
    tb_var *var = &tb->vars[tb->nvars];
    memset(var, 0, sizeof *var);
    (void)snprintf(var->name, sizeof var->name, "%s", name);
    var->width = width;
    var->sgn = sgn;
    var->is_wire = is_wire;
    return (int)tb->nvars++;
}

/* --- expressions --- */
static tb_expr *parse_expr(ps *p);

static bool const_int(ps *p, int64_t *out) {
    const tb_expr *e = parse_expr(p);
    if (e == NULL) {
        return false;
    }
    if (!tb_const(p->tb, e, out)) {
        return fail(p, "non-constant expression where a constant is required");
    }
    return true;
}

/* `[h:0]` after the cursor is on `[`; width of the range. */
static bool parse_range(ps *p, uint32_t *width) {
    int64_t high = 0;
    int64_t low = 0;
    if (!expect_op(p, "[") || !const_int(p, &high) || !expect_op(p, ":") || !const_int(p, &low) ||
        !expect_op(p, "]")) {
        return false;
    }
    if (low != 0 || high < 0) {
        return fail(p, "range other than [h:0]");
    }
    if (high >= TB_MAX_WIDTH) {
        return fail(p, "signal wider than 64 bits");
    }
    *width = (uint32_t)high + 1U;
    return true;
}

static bool parse_args(ps *p, tb_expr *e) {
    tb_expr *list[TB_MAX_ARGS];
    uint32_t n = 0;
    if (accept_op(p, "(") && !accept_op(p, ")")) {
        do {
            if (n == TB_MAX_ARGS) {
                return fail(p, "too many arguments");
            }
            list[n] = parse_expr(p);
            if (list[n++] == NULL) {
                return false;
            }
        } while (accept_op(p, ","));
        if (!expect_op(p, ")")) {
            return false;
        }
    }
    e->list = pnew(p, (n + 1U) * sizeof *list);
    if (e->list == NULL) {
        return false;
    }
    memcpy(e->list, list, n * sizeof *list);
    e->n = n;
    return true;
}

static const char *const SYS_EXPR[] = {"$time",     "$random", "$urandom", "$signed",
                                       "$unsigned", "$clog2",  "$fopen",   NULL};

static tb_expr *parse_sys_expr(ps *p) {
    tb_expr *e = new_expr(p, E_SYS);
    if (e == NULL) {
        return NULL;
    }
    e->str = pnew(p, TB_NAME);
    if (e->str == NULL) {
        return NULL;
    }
    (void)snprintf(e->str, TB_NAME, "%s", cur(p)->text);
    bool known = false;
    for (size_t i = 0; SYS_EXPR[i] != NULL; i++) {
        known = known || strcmp(SYS_EXPR[i], e->str) == 0;
    }
    p->pos++;
    if (!known) {
        fail(p, "system function");
        return NULL;
    }
    if (!parse_args(p, e)) {
        return NULL;
    }
    bool is_cast = strcmp(e->str, "$signed") == 0 || strcmp(e->str, "$unsigned") == 0;
    if ((is_cast || strcmp(e->str, "$clog2") == 0) && e->n != 1) {
        fail(p, "wrong argument count");
        return NULL;
    }
    e->width = strcmp(e->str, "$time") == 0 ? TB_MAX_WIDTH : 32U;
    e->sgn = strcmp(e->str, "$random") == 0 || strcmp(e->str, "$signed") == 0 ||
             strcmp(e->str, "$clog2") == 0;
    if (is_cast) {
        e->width = e->list[0]->width;
    }
    return e;
}

static tb_expr *parse_number(ps *p) {
    tb_expr *e = new_expr(p, E_NUM);
    if (e == NULL) {
        return NULL;
    }
    const tb_tok *t = cur(p);
    e->num = t->num;
    e->width = t->width == 0 ? 32U : t->width;
    e->sgn = t->is_signed;
    if (e->width > TB_MAX_WIDTH) {
        fail(p, "literal wider than 64 bits");
        return NULL;
    }
    e->num &= tb_mask(e->width);
    p->pos++;
    return e;
}

/* After the first item: `{n{...}}` makes it a replication. */
static bool concat_replication(ps *p, tb_expr *e, const tb_expr *first, uint64_t *reps) {
    int64_t count = 0;
    if (!tb_const(p->tb, first, &count) || count <= 0) {
        return fail(p, "replication count");
    }
    *reps = (uint64_t)count;
    e->kind = E_REPL;
    p->pos++;
    return true;
}

static bool concat_items(ps *p, tb_expr *e, tb_expr **list, uint32_t *count, uint64_t *reps) {
    uint32_t n = 0;
    do {
        if (n + 1U >= TB_MAX_ARGS) {
            return fail(p, "concatenation too long");
        }
        list[n] = parse_expr(p);
        if (list[n++] == NULL) {
            return false;
        }
        if (n == 1 && is_op(p, "{")) {
            if (!concat_replication(p, e, list[0], reps)) {
                return false;
            }
            n = 0;
        }
    } while (accept_op(p, ","));
    *count = n;
    return true;
}

static tb_expr *parse_concat(ps *p) {
    tb_expr *e = new_expr(p, E_CONCAT);
    tb_expr *list[TB_MAX_ARGS];
    uint32_t n = 0;
    uint64_t reps = 1;
    if (e == NULL) {
        return NULL;
    }
    p->pos++;
    if (!concat_items(p, e, list, &n, &reps) || !expect_op(p, "}") ||
        (e->kind == E_REPL && !expect_op(p, "}"))) {
        return NULL;
    }
    e->list = pnew(p, n * sizeof *list);
    if (e->list == NULL) {
        return NULL;
    }
    memcpy(e->list, list, n * sizeof *list);
    e->n = n;
    e->num = reps;
    uint64_t width = 0;
    for (uint32_t i = 0; i < n; i++) {
        width += list[i]->width;
    }
    width *= reps;
    if (width > TB_MAX_WIDTH) {
        return fail(p, "concatenation wider than 64 bits"), NULL;
    }
    e->width = (uint32_t)width;
    return e;
}

static tb_expr *parse_part_select(ps *p, tb_expr *sel, const tb_expr *idx, const tb_var *v) {
    int64_t high = 0;
    int64_t low = 0;
    if (!tb_const(p->tb, idx, &high) || !const_int(p, &low) || high < low || low < 0 ||
        high >= (int64_t)v->width) {
        return fail(p, "part select (constants within the declared width)"), NULL;
    }
    sel->kind = E_PART;
    sel->aux = (uint32_t)low;
    sel->width = (uint32_t)(high - low) + 1U;
    return sel;
}

static tb_expr *parse_select(ps *p, tb_expr *base, int var) {
    tb_var *v = &p->tb->vars[var];
    if (!accept_op(p, "[")) {
        return base;
    }
    tb_expr *idx = parse_expr(p);
    tb_expr *sel = idx != NULL ? new_expr(p, v->depth > 0 ? E_MEM : E_BITSEL) : NULL;
    if (sel == NULL) {
        return NULL;
    }
    sel->var = var;
    sel->a = v->depth > 0 ? idx : base;
    sel->b = idx;
    sel->width = v->depth > 0 ? v->width : 1U;
    sel->sgn = v->depth > 0 && v->sgn;
    if (v->depth == 0 && accept_op(p, ":")) {
        sel = parse_part_select(p, sel, idx, v);
    } else if (v->depth == 0 && (is_op(p, "+") || is_op(p, "-"))) {
        return fail(p, "indexed part select"), NULL;
    }
    return sel != NULL && expect_op(p, "]") ? sel : NULL;
}

static tb_expr *parse_ident(ps *p) {
    int var = find_var(p, cur(p)->text);
    if (var < 0) {
        return fail(p, "undeclared identifier (declare before use)"), NULL;
    }
    tb_expr *e = new_expr(p, E_VAR);
    if (e == NULL) {
        return NULL;
    }
    p->pos++;
    e->var = var;
    e->width = p->tb->vars[var].width;
    e->sgn = p->tb->vars[var].sgn;
    return parse_select(p, e, var);
}

static tb_expr *parse_primary(ps *p) {
    const tb_tok *t = cur(p);
    if (t->kind == TK_NUM) {
        return parse_number(p);
    }
    if (t->kind == TK_STR) {
        tb_expr *e = new_expr(p, E_STR);
        if (e != NULL) {
            e->str = t->str;
            e->width = 8;
            p->pos++;
        }
        return e;
    }
    if (t->kind == TK_SYS) {
        return parse_sys_expr(p);
    }
    if (t->kind == TK_ID) {
        return parse_ident(p);
    }
    if (accept_op(p, "(")) {
        tb_expr *e = parse_expr(p);
        return e != NULL && expect_op(p, ")") ? e : NULL;
    }
    if (is_op(p, "{")) {
        return parse_concat(p);
    }
    return fail(p, "expression syntax"), NULL;
}

static void copy_op(char *dst, const char *op) {
    size_t n = strlen(op) < 3 ? strlen(op) : 3;
    memcpy(dst, op, n);
    dst[n] = '\0';
    if (strcmp(dst, "^~") == 0) {
        memcpy(dst, "~^", 3);
    }
}

static tb_expr *parse_unary(ps *p) {
    static const char *const UNOPS[] = {"+", "-",  "~",  "!",  "&",  "|",
                                        "^", "~&", "~|", "~^", "^~", NULL};
    for (size_t i = 0; UNOPS[i] != NULL; i++) {
        if (is_op(p, UNOPS[i])) {
            tb_expr *e = new_expr(p, E_UN);
            p->pos++;
            tb_expr *operand = e != NULL ? parse_unary(p) : NULL;
            if (operand == NULL) {
                return NULL;
            }
            copy_op(e->op, UNOPS[i]);
            e->a = operand;
            bool arith = i < 3;
            e->width = arith ? operand->width : 1U;
            e->sgn = arith && operand->sgn;
            return e;
        }
    }
    return parse_primary(p);
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

static void set_bin_shape(tb_expr *e) {
    int prec = bin_prec(e->op);
    e->sgn = false;
    if (prec == 8) {
        e->width = e->a->width;
        e->sgn = e->a->sgn;
    } else if (prec == 6 || prec == 7 || prec <= 2) {
        e->width = 1;
    } else {
        e->width = e->a->width > e->b->width ? e->a->width : e->b->width;
        e->sgn = e->a->sgn && e->b->sgn;
    }
}

static tb_expr *parse_bin(ps *p, int min_prec) {
    tb_expr *lhs = parse_unary(p);
    while (lhs != NULL && cur(p)->kind == TK_OP) {
        int prec = bin_prec(cur(p)->text);
        if (strcmp(cur(p)->text, "**") == 0) {
            return fail(p, "operator **"), NULL;
        }
        if (prec == 0 || prec < min_prec) {
            break;
        }
        tb_expr *e = new_expr(p, E_BIN);
        if (e == NULL) {
            return NULL;
        }
        copy_op(e->op, cur(p)->text);
        p->pos++;
        e->a = lhs;
        e->b = parse_bin(p, prec + 1);
        if (e->b == NULL) {
            return NULL;
        }
        set_bin_shape(e);
        lhs = e;
    }
    return lhs;
}

static tb_expr *parse_expr(ps *p) {
    tb_expr *cond = parse_bin(p, 1);
    if (cond == NULL || !accept_op(p, "?")) {
        return cond;
    }
    tb_expr *e = new_expr(p, E_COND);
    if (e == NULL) {
        return NULL;
    }
    e->a = cond;
    e->b = parse_expr(p);
    if (e->b == NULL || !expect_op(p, ":")) {
        return NULL;
    }
    e->c = parse_expr(p);
    if (e->c == NULL) {
        return NULL;
    }
    e->width = e->b->width > e->c->width ? e->b->width : e->c->width;
    e->sgn = e->b->sgn && e->c->sgn;
    return e;
}

/* --- statements --- */
static tb_stmt *parse_stmt(ps *p);

static tb_stmt *new_stmt(ps *p, tb_skind kind) {
    tb_stmt *s = pnew(p, sizeof *s);
    if (s != NULL) {
        s->kind = kind;
        s->line = cur(p)->line;
    }
    return s;
}

static bool valid_lvalue(const ps *p, const tb_expr *lv) {
    switch (lv->kind) {
    case E_VAR:
        return !p->tb->vars[lv->var].is_param && p->tb->vars[lv->var].depth == 0;
    case E_BITSEL:
    case E_PART:
        return valid_lvalue(p, lv->a);
    case E_MEM:
        return true;
    case E_CONCAT:
        for (uint32_t i = 0; i < lv->n; i++) {
            if (!valid_lvalue(p, lv->list[i])) {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
}

static tb_stmt *parse_assign(ps *p, bool semi) {
    tb_stmt *s = new_stmt(p, S_ASSIGN);
    tb_expr *lv = s != NULL ? parse_primary(p) : NULL;
    if (lv == NULL || !valid_lvalue(p, lv)) {
        return fail(p, "assignment target (a reg, integer, select or concatenation of them)"), NULL;
    }
    s->lhs = lv;
    if (accept_op(p, "<=")) {
        s->kind = S_NBA;
    } else if (!expect_op(p, "=")) {
        return NULL;
    }
    if (is_op(p, "#") || is_kw(p, "repeat") || is_op(p, "@")) {
        return fail(p, "intra-assignment delay or event control"), NULL;
    }
    s->rhs = parse_expr(p);
    return s->rhs != NULL && (!semi || expect_op(p, ";")) ? s : NULL;
}

static const char *const SYS_TASKS[] = {
    "$display",  "$write",    "$strobe",   "$monitor", "$monitoroff", "$monitoron",
    "$fdisplay", "$fwrite",   "$fclose",   "$finish",  "$stop",       "$readmemh",
    "$readmemb", "$dumpfile", "$dumpvars", "$fopen",   NULL};

static tb_expr *parse_sys_task_expr(ps *p) {
    tb_expr *e = new_expr(p, E_SYS);
    if (e == NULL) {
        return NULL;
    }
    e->str = pnew(p, TB_NAME);
    if (e->str == NULL) {
        return NULL;
    }
    (void)snprintf(e->str, TB_NAME, "%s", cur(p)->text);
    p->pos++;
    return parse_args(p, e) ? e : NULL;
}

/* Rejects a format string with a specifier outside the subset (the format is the first argument,
 * or the second for the $f* tasks). */
static bool check_format(ps *p, const tb_expr *call) {
    bool file_task = strncmp(call->str, "$f", 2) == 0 && strcmp(call->str, "$finish") != 0;
    uint32_t at = file_task ? 1U : 0U;
    bool no_fmt = strncmp(call->str, "$read", 5) == 0 || strcmp(call->str, "$dumpfile") == 0 ||
                  strcmp(call->str, "$fopen") == 0;
    char bad = 0;
    if (no_fmt || call->n <= at || call->list[at]->kind != E_STR ||
        tb_fmt_valid(call->list[at]->str, &bad)) {
        return true;
    }
    char msg[64];
    (void)snprintf(msg, sizeof msg, "format specifier %%%c", bad);
    return fail(p, msg);
}

static tb_stmt *parse_systask(ps *p) {
    tb_stmt *s = new_stmt(p, S_SYS);
    bool known = false;
    for (size_t i = 0; SYS_TASKS[i] != NULL; i++) {
        known = known || strcmp(SYS_TASKS[i], cur(p)->text) == 0;
    }
    if (s == NULL || !known) {
        return fail(p, "system task"), NULL;
    }
    s->sys = parse_sys_task_expr(p);
    if (s->sys == NULL || !expect_op(p, ";") || !check_format(p, s->sys)) {
        return NULL;
    }
    return s;
}

static tb_stmt *parse_block(ps *p) {
    tb_stmt *s = new_stmt(p, S_BLOCK);
    uint32_t cap = 8;
    if (s == NULL) {
        return NULL;
    }
    s->list = pnew(p, cap * sizeof *s->list);
    if (accept_op(p, ":") && cur(p)->kind == TK_ID) {
        p->pos++;
    }
    if (s->list == NULL) {
        return NULL;
    }
    while (!accept_kw(p, "end")) {
        if (cur(p)->kind == TK_EOF) {
            return fail(p, "missing `end`"), NULL;
        }
        if (s->n == cap) {
            tb_stmt **grown = pnew(p, cap * 2U * sizeof *grown);
            if (grown == NULL) {
                return NULL;
            }
            memcpy(grown, s->list, cap * sizeof *grown);
            s->list = grown;
            cap *= 2U;
        }
        tb_stmt *child = parse_stmt(p);
        if (child == NULL) {
            return NULL;
        }
        s->list[s->n++] = child;
    }
    return s;
}

static tb_stmt *parse_paren_stmt(ps *p, tb_skind kind) {
    tb_stmt *s = new_stmt(p, kind);
    p->pos++;
    if (s == NULL || !expect_op(p, "(")) {
        return NULL;
    }
    s->rhs = parse_expr(p);
    if (s->rhs == NULL || !expect_op(p, ")")) {
        return NULL;
    }
    s->a = parse_stmt(p);
    return s->a != NULL ? s : NULL;
}

static tb_stmt *parse_if(ps *p) {
    tb_stmt *s = parse_paren_stmt(p, S_IF);
    if (s != NULL && accept_kw(p, "else")) {
        s->b = parse_stmt(p);
        return s->b != NULL ? s : NULL;
    }
    return s;
}

static tb_stmt *parse_for(ps *p) {
    tb_stmt *s = new_stmt(p, S_FOR);
    p->pos++;
    if (s == NULL || !expect_op(p, "(")) {
        return NULL;
    }
    s->init = parse_assign(p, false);
    if (s->init == NULL || !expect_op(p, ";")) {
        return NULL;
    }
    s->rhs = parse_expr(p);
    if (s->rhs == NULL || !expect_op(p, ";")) {
        return NULL;
    }
    s->step = parse_assign(p, false);
    if (s->step == NULL || !expect_op(p, ")")) {
        return NULL;
    }
    s->a = parse_stmt(p);
    return s->a != NULL ? s : NULL;
}

static bool parse_event_terms(ps *p, tb_stmt *s) {
    tb_event_term terms[TB_MAX_ARGS];
    uint32_t n = 0;
    do {
        if (n == TB_MAX_ARGS) {
            return fail(p, "too many event terms");
        }
        terms[n].edge = accept_kw(p, "posedge") ? 1 : (accept_kw(p, "negedge") ? 2 : 0);
        terms[n].sig = parse_expr(p);
        if (terms[n++].sig == NULL) {
            return false;
        }
    } while (accept_kw(p, "or") || accept_op(p, ","));
    s->terms = pnew(p, n * sizeof *terms);
    if (s->terms == NULL) {
        return false;
    }
    memcpy(s->terms, terms, n * sizeof *terms);
    s->nterms = n;
    return true;
}

static tb_stmt *parse_body_or_nop(ps *p, tb_stmt *s) {
    if (accept_op(p, ";")) {
        s->a = new_stmt(p, S_NOP);
    } else {
        s->a = parse_stmt(p);
    }
    return s->a != NULL ? s : NULL;
}

static tb_stmt *parse_event(ps *p) {
    tb_stmt *s = new_stmt(p, S_EVENT);
    p->pos++;
    if (s == NULL) {
        return NULL;
    }
    if (is_op(p, "*") || (is_op(p, "(") && p->t[p->pos + 1].kind == TK_OP &&
                          strcmp(p->t[p->pos + 1].text, "*") == 0)) {
        return fail(p, "@* event control (list the signals)"), NULL;
    }
    if (!expect_op(p, "(") || !parse_event_terms(p, s) || !expect_op(p, ")")) {
        return NULL;
    }
    return parse_body_or_nop(p, s);
}

static tb_stmt *parse_delay(ps *p) {
    tb_stmt *s = new_stmt(p, S_DELAY);
    p->pos++;
    if (s == NULL) {
        return NULL;
    }
    s->rhs = parse_primary(p);
    return s->rhs != NULL ? parse_body_or_nop(p, s) : NULL;
}

static bool case_item_labels(ps *p, tb_stmt *s, tb_expr **labels, uint32_t *nlab) {
    if (accept_kw(p, "default")) {
        (void)accept_op(p, ":");
        return true;
    }
    do {
        if (*nlab == TB_MAX_ARGS) {
            return fail(p, "too many case labels");
        }
        labels[*nlab] = parse_expr(p);
        if (labels[(*nlab)++] == NULL) {
            return false;
        }
    } while (accept_op(p, ","));
    (void)s;
    return expect_op(p, ":");
}

static tb_stmt *parse_case(ps *p) {
    tb_stmt *s = new_stmt(p, S_CASE);
    if (strcmp(cur(p)->text, "case") != 0) {
        return fail(p, "casez/casex (no x/z in a 2-state simulator)"), NULL;
    }
    p->pos++;
    enum { MAX_ITEMS = 64 };
    s->list = pnew(p, MAX_ITEMS * sizeof *s->list);
    s->nlab = pnew(p, MAX_ITEMS * sizeof *s->nlab);
    s->labels = pnew(p, MAX_ITEMS * TB_MAX_ARGS * sizeof *s->labels);
    if (!expect_op(p, "(") || (s->rhs = parse_expr(p)) == NULL || !expect_op(p, ")")) {
        return NULL;
    }
    while (!accept_kw(p, "endcase")) {
        if (s->n == MAX_ITEMS || s->list == NULL || cur(p)->kind == TK_EOF) {
            return fail(p, "case with more than 64 items"), NULL;
        }
        uint32_t nlab = 0;
        if (!case_item_labels(p, s, &s->labels[s->n * TB_MAX_ARGS], &nlab)) {
            return NULL;
        }
        s->nlab[s->n] = nlab;
        s->list[s->n] = parse_stmt(p);
        if (s->list[s->n++] == NULL) {
            return NULL;
        }
    }
    return s;
}

static bool parse_decl(ps *p);

static tb_stmt *parse_begin(ps *p) {
    p->pos++;
    return parse_block(p);
}

static tb_stmt *parse_while(ps *p) {
    return parse_paren_stmt(p, S_WHILE);
}

static tb_stmt *parse_repeat(ps *p) {
    return parse_paren_stmt(p, S_REPEAT);
}

static tb_stmt *parse_wait(ps *p) {
    return parse_paren_stmt(p, S_WAIT);
}

static tb_stmt *parse_forever(ps *p) {
    tb_stmt *s = new_stmt(p, S_FOREVER);
    p->pos++;
    s->a = parse_stmt(p);
    return s->a != NULL ? s : NULL;
}

static tb_stmt *parse_local_decl(ps *p) {
    return parse_decl(p) ? new_stmt(p, S_NOP) : NULL;
}

static tb_stmt *parse_rejected(ps *p) {
    return fail(p, cur(p)->text), NULL;
}

static tb_stmt *parse_kw_stmt(ps *p) {
    static const struct {
        const char *kw;
        tb_stmt *(*fn)(ps *);
    } TABLE[] = {{"begin", parse_begin},
                 {"if", parse_if},
                 {"case", parse_case},
                 {"casez", parse_case},
                 {"casex", parse_case},
                 {"for", parse_for},
                 {"while", parse_while},
                 {"repeat", parse_repeat},
                 {"wait", parse_wait},
                 {"forever", parse_forever},
                 {"reg", parse_local_decl},
                 {"integer", parse_local_decl},
                 {"disable", parse_rejected},
                 {"fork", parse_rejected},
                 {"task", parse_rejected},
                 {"deassign", parse_rejected},
                 {"force", parse_rejected},
                 {"release", parse_rejected},
                 {NULL, NULL}};
    for (size_t i = 0; TABLE[i].kw != NULL; i++) {
        if (is_kw(p, TABLE[i].kw)) {
            return TABLE[i].fn(p);
        }
    }
    return parse_assign(p, true);
}

static tb_stmt *parse_stmt(ps *p) {
    const tb_tok *t = cur(p);
    if (is_op(p, ";")) {
        tb_stmt *s = new_stmt(p, S_NOP);
        p->pos++;
        return s;
    }
    if (is_op(p, "#")) {
        return parse_delay(p);
    }
    if (is_op(p, "@")) {
        return parse_event(p);
    }
    if (t->kind == TK_SYS) {
        return parse_systask(p);
    }
    return t->kind == TK_ID ? parse_kw_stmt(p) : parse_assign(p, true);
}

/* --- module items --- */
static bool parse_array_range(ps *p, int var) {
    int64_t high = 0;
    int64_t low = 0;
    p->pos++;
    if (!const_int(p, &low) || !expect_op(p, ":") || !const_int(p, &high) || !expect_op(p, "]") ||
        low != 0 || high < 0 || high > (1 << 20)) {
        return fail(p, "array range (use [0:N])");
    }
    tb_var *v = &p->tb->vars[var];
    v->depth = (uint32_t)high + 1U;
    v->mem = pnew(p, v->depth * sizeof *v->mem);
    return v->mem != NULL;
}

static bool add_assign(ps *p, int var, tb_expr *rhs) {
    tb_assign *grown = pnew(p, (p->tb->nassigns + 1U) * sizeof *grown);
    if (grown == NULL) {
        return false;
    }
    if (p->tb->nassigns > 0) {
        memcpy(grown, p->tb->assigns, p->tb->nassigns * sizeof *grown);
    }
    grown[p->tb->nassigns] = (tb_assign){var, rhs};
    p->tb->assigns = grown;
    p->tb->nassigns++;
    return true;
}

static bool parse_decl_item(ps *p, const char *kw, uint32_t width, bool sgn) {
    bool is_wire = strcmp(kw, "wire") == 0;
    bool is_param = strcmp(kw, "parameter") == 0 || strcmp(kw, "localparam") == 0;
    if (cur(p)->kind != TK_ID) {
        return fail(p, "declaration syntax");
    }
    char name[TB_NAME];
    (void)snprintf(name, sizeof name, "%s", cur(p)->text);
    p->pos++;
    int var = add_var(p, name, width, sgn, is_wire);
    if (var < 0) {
        return false;
    }
    p->tb->vars[var].is_param = is_param;
    if (is_op(p, "[")) {
        return parse_array_range(p, var);
    }
    if (!accept_op(p, "=")) {
        return !is_param || fail(p, "parameter without a value");
    }
    tb_expr *init = parse_expr(p);
    int64_t val = 0;
    if (init == NULL) {
        return false;
    }
    if (is_wire) {
        return add_assign(p, var, init);
    }
    if (!tb_const(p->tb, init, &val)) {
        return fail(p, "non-constant initialiser");
    }
    p->tb->vars[var].val = (uint64_t)val & tb_mask(width);
    return true;
}

static bool parse_decl(ps *p) {
    char kw[TB_NAME];
    (void)snprintf(kw, sizeof kw, "%s", cur(p)->text);
    p->pos++;
    uint32_t width = strcmp(kw, "integer") == 0 ? 32U : 1U;
    bool sgn = strcmp(kw, "integer") == 0;
    if (accept_kw(p, "signed")) {
        sgn = true;
    }
    if (is_op(p, "[") && !parse_range(p, &width)) {
        return false;
    }
    do {
        if (!parse_decl_item(p, kw, width, sgn)) {
            return false;
        }
    } while (accept_op(p, ","));
    return expect_op(p, ";");
}

static bool add_proc(ps *p, tb_stmt *body, bool always) {
    tb_module *tb = p->tb;
    tb_proc_def *grown = pnew(p, (tb->nprocs + 1U) * sizeof *grown);
    if (grown == NULL || body == NULL) {
        return false;
    }
    if (tb->nprocs > 0) {
        memcpy(grown, tb->procs, tb->nprocs * sizeof *grown);
    }
    grown[tb->nprocs++] = (tb_proc_def){body, always};
    tb->procs = grown;
    return true;
}

static bool starts_with_wait(const tb_stmt *s) {
    while (s->kind == S_BLOCK && s->n > 0) {
        s = s->list[0];
    }
    return s->kind == S_DELAY || s->kind == S_EVENT || s->kind == S_WAIT;
}

static bool parse_always(ps *p) {
    p->pos++;
    tb_stmt *body = parse_stmt(p);
    if (body != NULL && !starts_with_wait(body)) {
        return fail(p, "always block that does not start with #, @ or wait (zero-delay loop)");
    }
    return add_proc(p, body, true);
}

static bool parse_assign_item(ps *p) {
    p->pos++;
    const tb_expr *lhs = parse_primary(p);
    if (lhs == NULL || lhs->kind != E_VAR || !p->tb->vars[lhs->var].is_wire) {
        return fail(p, "assign to anything but a whole wire");
    }
    if (!expect_op(p, "=")) {
        return false;
    }
    tb_expr *rhs = parse_expr(p);
    return rhs != NULL && expect_op(p, ";") && add_assign(p, lhs->var, rhs);
}

static bool parse_inst_conns(ps *p, tb_inst *inst) {
    uint32_t cap = 16;
    inst->conns = pnew(p, cap * sizeof *inst->conns);
    if (inst->conns == NULL || !expect_op(p, "(")) {
        return false;
    }
    while (!accept_op(p, ")")) {
        if (inst->nconns == cap) {
            return fail(p, "more than 16 port connections");
        }
        tb_conn *c = &inst->conns[inst->nconns++];
        if (accept_op(p, ".")) {
            (void)snprintf(c->port, sizeof c->port, "%s", cur(p)->text);
            p->pos++;
            if (!expect_op(p, "(")) {
                return false;
            }
            c->expr = is_op(p, ")") ? NULL : parse_expr(p);
            if ((c->expr == NULL && !is_op(p, ")")) || !expect_op(p, ")")) {
                return false;
            }
        } else {
            c->expr = parse_expr(p);
        }
        if ((c->expr == NULL && !c->port[0]) || (!accept_op(p, ",") && !is_op(p, ")"))) {
            return fail(p, "port connection syntax");
        }
    }
    return true;
}

static bool parse_instance(ps *p) {
    tb_module *tb = p->tb;
    tb_inst *grown = pnew(p, (tb->ninsts + 1U) * sizeof *grown);
    if (grown == NULL) {
        return false;
    }
    if (tb->ninsts > 0) {
        memcpy(grown, tb->insts, tb->ninsts * sizeof *grown);
    }
    tb->insts = grown;
    tb_inst *inst = &grown[tb->ninsts++];
    inst->line = cur(p)->line;
    (void)snprintf(inst->module, sizeof inst->module, "%s", cur(p)->text);
    p->pos++;
    if (is_op(p, "#")) {
        return fail(p, "parameter override on a DUT instance (the DUT is already elaborated)");
    }
    if (cur(p)->kind != TK_ID) {
        return fail(p, "module item");
    }
    (void)snprintf(inst->name, sizeof inst->name, "%s", cur(p)->text);
    p->pos++;
    return parse_inst_conns(p, inst) && expect_op(p, ";");
}

static bool parse_item(ps *p) {
    const tb_tok *t = cur(p);
    if (t->kind != TK_ID) {
        return fail(p, "module item");
    }
    if (is_kw(p, "reg") || is_kw(p, "wire") || is_kw(p, "integer") || is_kw(p, "parameter") ||
        is_kw(p, "localparam")) {
        return parse_decl(p);
    }
    if (is_kw(p, "initial")) {
        p->pos++;
        return add_proc(p, parse_stmt(p), false);
    }
    if (is_kw(p, "always")) {
        return parse_always(p);
    }
    if (is_kw(p, "assign")) {
        return parse_assign_item(p);
    }
    static const char *const REJECT[] = {
        "genvar", "generate", "task",    "function", "real",   "time",  "string",
        "event",  "defparam", "specify", "input",    "output", "inout", "and",
        "or",     "nand",     "nor",     "xor",      "not",    "buf",   NULL};
    for (size_t i = 0; REJECT[i] != NULL; i++) {
        if (is_kw(p, REJECT[i])) {
            return fail(p, t->text);
        }
    }
    return parse_instance(p);
}

static bool parse_header(ps *p, const char *top) {
    if (!accept_kw(p, "module") || cur(p)->kind != TK_ID) {
        return fail(p, "file does not start with a module");
    }
    (void)snprintf(p->tb->name, sizeof p->tb->name, "%s", cur(p)->text);
    p->pos++;
    if (top != NULL && strcmp(top, p->tb->name) != 0) {
        return fail(p,
                    "--top names a module that is not the first (one testbench module per file)");
    }
    if (accept_op(p, "(") && !expect_op(p, ")")) {
        return false;
    }
    return expect_op(p, ";");
}

odin3_status tb_parse(const char *file, const char *text, const char *top, tb_module **out) {
    *out = NULL;
    tb_module *tb = calloc(1, sizeof *tb);
    tb_lexed lexed = {0};
    if (tb == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    ps p = {tb, NULL, 0, file, false};
    if (!tb_lex(file, text, tb, &lexed)) {
        tb_module_free(tb);
        return ODIN3_ERR_INVALID_ARG;
    }
    p.t = lexed.toks;
    if (parse_header(&p, top)) {
        while (!p.failed && !accept_kw(&p, "endmodule")) {
            bool bad = cur(&p)->kind == TK_EOF || is_kw(&p, "module");
            if (bad) {
                fail(&p, cur(&p)->kind == TK_EOF ? "missing endmodule"
                                                 : "more than one module in the file");
            } else {
                (void)parse_item(&p);
            }
        }
    }
    if (!p.failed && cur(&p)->kind != TK_EOF) {
        fail(&p, "more than one module in the file");
    }
    if (p.failed) {
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
    free(tb->allocs);
    free(tb);
}
