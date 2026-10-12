/*
 * elab1_gen.c — generate in stage 1 (2D spec §5; EL-19–EL-22): the scopes of an environment
 * (the module and one per elaborated generate block), label_genblks's names (EL-21: per scope a
 * counter of the GEN_IF, GEN_FOR and GEN_CASE constructs met and a set of the `genblk*` names seen
 * so far, copied on entry by a sequence number), pass E of the three constructs (their constant
 * parts evaluated, the decision logged, the selected blocks made as scopes) and G7's references
 * into the module's generate blocks. Nothing here recurses: the walks are pass E's and pass W's
 * worklists, the name scan an AST cursor.
 */
#include "frontends/verilog/elab1.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab_test.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    G_SHIFT = 32,
    G_INT_BITS = 32,
    G_FOR_INIT = 0,
    G_FOR_COND = 1,
    G_FOR_STEP = 2,
    G_FOR_BODY = 3,
    G_IF_COND = 0,
    G_IF_THEN = 1,
    G_CASE_ITEMS = 1,  /* GEN_CASE(E; CASE_ITEM+) */
    G_ITEM_EXPRS = 1,  /* CASE_ITEM(BODY; E*) */
    G_ASSIGN_RHS = 2,  /* BLOCKING_ASSIGN(lhs, control?, rhs) */
    G_MIN_HIER = 2,    /* a HIER_NAME of one component is a plain name */
    G_MODULE_ITEMS = 2 /* MODULE(params, ports; items) */
};

static const uint32_t G_NONE = UINT32_MAX;
static const char G_PREFIX[] = "genblk";

/* --- scopes ---------------------------------------------------------------------------------- */

odin3_status odin3_elab1_gen_init(odin3_elab1_env *env) {
    odin3_vec_init(&env->scopes, sizeof(odin3_elab_scope));
    odin3_vec_init(&env->scope_x, sizeof(odin3_elab1_scope_x));
    env->seen = odin3_u64map_create(0);
    env->kids = odin3_u64map_create(0);
    env->iters = odin3_u64map_create(0);
    odin3_elab_scope *top = odin3_vec_push(&env->scopes);
    odin3_elab1_scope_x *ext = top != NULL ? odin3_vec_push(&env->scope_x) : NULL;
    if (env->seen == NULL || env->kids == NULL || env->iters == NULL || ext == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *top = (odin3_elab_scope){ODIN3_ESCOPE_MODULE, 0, 0, false, G_NONE, 0};
    *ext = (odin3_elab1_scope_x){env->module, 0, 0};
    return ODIN3_OK;
}

void odin3_elab1_gen_free(odin3_elab1_env *env) {
    odin3_vec_free(&env->scopes);
    odin3_vec_free(&env->scope_x);
    odin3_u64map_destroy(env->seen);
    odin3_u64map_destroy(env->kids);
    odin3_u64map_destroy(env->iters);
}

const odin3_elab_scope *odin3_elab1_scope_at(const odin3_elab1_env *env, uint32_t scope) {
    return scope < env->scopes.len ? odin3_vec_cat(&env->scopes, scope) : NULL;
}

const odin3_elab1_scope_x *odin3_elab1_scope_ext(const odin3_elab1_env *env, uint32_t scope) {
    return scope < env->scope_x.len ? odin3_vec_cat(&env->scope_x, scope) : NULL;
}

uint32_t odin3_elab1_env_lookup(const odin3_elab1_env *env, uint32_t name) {
    for (uint32_t at = env->cur_scope; at != G_NONE;) {
        uint32_t idx = odin3_elab1_env_find(env, at, name);
        if (idx != G_NONE) {
            return idx;
        }
        const odin3_elab_scope *rec = odin3_elab1_scope_at(env, at);
        at = rec != NULL ? rec->parent : G_NONE;
    }
    return G_NONE;
}

/* A new scope (its copy of the seen set is the clock now); *out its index. */
static odin3_status new_scope(odin3_elab1_env *env, const odin3_elab_scope *rec,
                              odin3_elab1_scope_x ext, uint32_t *out) {
    odin3_elab_scope *slot = odin3_vec_push(&env->scopes);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_elab1_scope_x *xs = odin3_vec_push(&env->scope_x);
    if (xs == NULL) {
        odin3_vec_pop(&env->scopes);
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *rec;
    ext.seq = env->seq;
    *xs = ext;
    *out = (uint32_t)env->scopes.len - 1;
    return ODIN3_OK;
}

odin3_status odin3_elab1_scope_local(odin3_elab1_env *env, uint32_t parent, uint32_t *out) {
    odin3_elab_scope rec = {.kind = ODIN3_ESCOPE_LOCAL, .parent = parent};
    return new_scope(env, &rec, (odin3_elab1_scope_x){{0}, 0, 0}, out);
}

void odin3_elab1_scope_items(const odin3_elab1_env *env, uint32_t scope, odin3_ast_id *node,
                             uint32_t *first, uint32_t *end) {
    const odin3_ast *ast = env->e1->parsed;
    const odin3_elab1_scope_x *ext = odin3_elab1_scope_ext(env, scope);
    if (scope == 0) {
        *node = env->module;
        *first = G_MODULE_ITEMS;
        *end = odin3_ast_nchild(ast, env->module);
        return;
    }
    odin3_ast_id body = odin3_ast_child(ast, ext->holder, ext->slot);
    odin3_ast_kind kind = odin3_ast_kind_of(ast, body);
    if (kind == ODIN3_AST_GEN_BLOCK) {
        *node = body;
        *first = odin3_ast_slot_count(ODIN3_AST_GEN_BLOCK);
        *end = odin3_ast_nchild(ast, body);
        return;
    }
    *node = ext->holder; /* a bare item is the block's one item; a NULL_STMT block is empty */
    *first = ext->slot;
    *end = kind == ODIN3_AST_NULL_STMT ? ext->slot : ext->slot + 1;
}

/* --- the seen sets (label_genblks's `existing`) --------------------------------------------- */

static odin3_status see(odin3_elab1_env *env, uint32_t scope, uint32_t name) {
    uint64_t key = (uint64_t)scope << G_SHIFT | name;
    if (odin3_u64map_get(env->seen, key, NULL)) {
        return ODIN3_OK;
    }
    return odin3_u64map_put(env->seen, (odin3_kv){key, env->seq++});
}

/* name is in scope's set: seen in scope itself, or in an ancestor before the child on the path
 * was made (its copy). */
static bool seen_in(const odin3_elab1_env *env, uint32_t scope, uint32_t name) {
    uint64_t key = (uint64_t)scope << G_SHIFT | name;
    uint64_t limit = UINT64_MAX;
    for (uint32_t at = scope; at != G_NONE;) {
        uint64_t seq = 0;
        if (odin3_u64map_get(env->seen, key, &seq) && seq < limit) {
            return true;
        }
        limit = odin3_elab1_scope_ext(env, at)->seq;
        at = odin3_elab1_scope_at(env, at)->parent;
        key = (uint64_t)at << G_SHIFT | (key & UINT32_MAX);
    }
    return false;
}

static bool named_genblk(const odin3_ast *ast, odin3_ast_id node) {
    return odin3_ast_name(ast, node) != 0 && odin3_ast_kind_of(ast, node) != ODIN3_AST_STRING &&
           strncmp(odin3_ast_name_str(ast, node), G_PREFIX, sizeof G_PREFIX - 1) == 0;
}

odin3_status odin3_elab1_gen_seen(odin3_elab1_env *env, odin3_ast_id node, uint32_t scope) {
    if (node.v == 0) {
        return ODIN3_OK;
    }
    const odin3_ast *ast = env->e1->parsed;
    odin3_ast_cursor cur;
    odin3_status st = odin3_ast_cursor_init(&cur, ast, node);
    odin3_ast_event ev = ODIN3_AST_ENTER;
    odin3_ast_id at = {0};
    while (st == ODIN3_OK && odin3_ast_cursor_next(&cur, &ev, &at)) {
        if (ev != ODIN3_AST_ENTER) {
            continue;
        }
        if (odin3_ast_kind_of(ast, at) == ODIN3_AST_HIER_NAME &&
            odin3_ast_nchild(ast, at) >= G_MIN_HIER) {
            odin3_ast_cursor_skip(&cur); /* Yosys's identifier is the whole dotted path */
        } else if (named_genblk(ast, at)) {
            st = see(env, scope, odin3_ast_name(ast, at));
        }
    }
    odin3_ast_cursor_free(&cur);
    return st;
}

/* label_genblks's candidates for label (scope << 32 | counter), the first not seen. */
static odin3_status pad_name(odin3_elab1 *e1, uint64_t label, uint32_t *name) {
    uint32_t scope = (uint32_t)(label >> G_SHIFT);
    uint32_t counter = (uint32_t)(label & UINT32_MAX);
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    int digits = snprintf(NULL, 0, "%" PRIu32, counter);
    odin3_status st = ODIN3_OK;
    *name = 0;
    for (int pad = 0; st == ODIN3_OK && *name == 0; pad++) {
        odin3_strbuf_clear(&text);
        st = odin3_strbuf_appendf(&text, "%s%0*" PRIu32, G_PREFIX, digits + pad, counter);
        uint32_t id = 0;
        odin3_bytes bytes = {text.data, text.len};
        bool known =
            st == ODIN3_OK && odin3_strtab_find(odin3_design_strtab(e1->design), bytes, &id);
        if (st == ODIN3_OK && (!known || !seen_in(e1->env, scope, id))) {
            st = odin3_design_intern(e1->design, bytes, name);
        }
    }
    odin3_strbuf_free(&text);
    return st;
}

odin3_status odin3_elab1_genblk_name(odin3_elab1 *e1, uint32_t scope, uint32_t counter,
                                     uint32_t *name) {
    return pad_name(e1, (uint64_t)scope << G_SHIFT | counter, name);
}

/* --- shared helpers -------------------------------------------------------------------------- */

static void g_error(const odin3_elab1_env *env, odin3_ast_id at, const char *fmt, ...)
    ODIN3_PRINTF(3, 4);

static void g_error(const odin3_elab1_env *env, odin3_ast_id at, const char *fmt, ...) {
    if (env->quiet) {
        return;
    }
    char text[ODIN3_LOG_BUF];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    odin3_elab1_report(env->e1, ODIN3_LOG_ERROR, odin3_ast_loc(env->e1->parsed, at),
                       len >= 0 ? text : fmt);
}

static odin3_status log_push(odin3_elab1 *e1, odin3_ast_id node, int32_t value) {
    odin3_elab_decision *slot = odin3_vec_push(&e1->log);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (odin3_elab_decision){node, value};
    return ODIN3_OK;
}

/* The name of the block body (child slot of holder) of a construct whose counter value is
 * counter: the user's, else label_genblks's. */
static odin3_status block_name(odin3_elab1 *e1, odin3_ast_id body, uint32_t scope, uint32_t counter,
                               uint32_t *name) {
    const odin3_ast *ast = e1->parsed;
    if (odin3_ast_kind_of(ast, body) == ODIN3_AST_GEN_BLOCK && odin3_ast_name(ast, body) != 0) {
        *name = odin3_ast_name(ast, body);
        return ODIN3_OK;
    }
    return odin3_elab1_genblk_name(e1, scope, counter, name);
}

/* One selected block of a construct in scope made: child slot of holder, named name. */
static odin3_status make_block(odin3_elab1_env *env, const odin3_elab1_scope_x *ext, uint32_t scope,
                               uint32_t name) {
    odin3_elab_scope rec = {ODIN3_ESCOPE_BLOCK, name, 0, false, scope, 0};
    uint32_t made = 0;
    odin3_status st = new_scope(env, &rec, *ext, &made);
    uint64_t key = (uint64_t)scope << G_SHIFT | name;
    if (st == ODIN3_OK && !odin3_u64map_get(env->kids, key, NULL)) {
        st = odin3_u64map_put(env->kids, (odin3_kv){key, (uint64_t)made + 1});
    }
    env->made_first = made;
    env->made_n = st == ODIN3_OK ? 1 : 0;
    return st;
}

/* The construct's counter value in scope (label_genblks: once per construct). */
static uint32_t count_construct(odin3_elab1_env *env, uint32_t scope) {
    odin3_elab_scope *rec = odin3_vec_at(&env->scopes, scope);
    rec->counter++;
    return rec->counter;
}

/* A condition's truth (IEEE: true when a bit is 1; x is false): PARSE after a diagnostic. */
static odin3_status truth_of(odin3_elab1_env *env, odin3_arena *tmp, odin3_ast_id expr,
                             const char *what, bool *out) {
    odin3_elab1_eval_req req = {expr, what, NULL};
    odin3_cval val = {0};
    odin3_status st = odin3_elab1_eval_in(env, tmp, &req, &val);
    *out = st == ODIN3_OK && odin3_cval_truth(&val) == ODIN3_BIT_1;
    return st;
}

/* --- GEN_IF (EL-20) -------------------------------------------------------------------------- */

/* The condition seen and evaluated (temporaries freed at once): *taken its truth. PARSE after a
 * diagnostic; every other failure propagates (a 2D bug is never turned into OK). */
static odin3_status if_truth(odin3_elab1_env *env, odin3_ast_id cond, uint32_t scope, bool *taken) {
    odin3_arena *tmp = odin3_arena_create(ODIN3_E1_TMP_CHUNK);
    if (tmp == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = odin3_elab1_gen_seen(env, cond, scope);
    st = st != ODIN3_OK ? st : truth_of(env, tmp, cond, "generate condition", taken);
    odin3_arena_destroy(tmp);
    return st;
}

odin3_status odin3_elab1_gen_if(odin3_elab1 *e1, odin3_ast_id node, uint32_t scope) {
    odin3_elab1_env *env = e1->env;
    const odin3_ast *ast = e1->parsed;
    env->made_n = 0;
    env->cur_scope = scope;
    uint32_t counter = count_construct(env, scope);
    bool taken = false;
    odin3_status ev = if_truth(env, odin3_ast_child(ast, node, G_IF_COND), scope, &taken);
    if (ev != ODIN3_OK && ev != ODIN3_ERR_PARSE) {
        return ev;
    }
    uint32_t slot = taken ? G_IF_THEN : G_IF_THEN + 1;
    bool has = ev == ODIN3_OK && odin3_ast_child(ast, node, slot).v != 0;
    odin3_status st =
        log_push(e1, node, has ? (int32_t)(slot - G_IF_THEN) : ODIN3_DECISION_SKIPPED);
    uint32_t name = 0;
    st = st != ODIN3_OK || !has
             ? st
             : block_name(e1, odin3_ast_child(ast, node, slot), scope, counter, &name);
    odin3_elab1_scope_x ext = {node, slot, 0};
    return st != ODIN3_OK || !has ? st : make_block(env, &ext, scope, name);
}

/* --- GEN_CASE (EL-20) ------------------------------------------------------------------------ */

/* The GEN_CASE being decided: its value, the item chosen and the default's. */
typedef struct g_case {
    uint32_t scope;
    uint32_t counter;
    odin3_cval value;
    odin3_arena *tmp; /* the evaluator's temporaries, freed with the construct */
    bool valid;       /* the case expression evaluated */
    uint32_t chosen;  /* the item index, G_NONE */
    uint32_t name;    /* its block's name */
    uint32_t dflt;    /* the default item, G_NONE */
    uint32_t dflt_name;
} g_case;

/* Yosys's const_eq(expr, item) at the common width, signed iff both: 1 matches (x never). */
static odin3_status case_match(odin3_elab1_env *env, const g_case *gc, odin3_ast_id expr,
                               bool *out) {
    odin3_elab1_eval_req req = {expr, "case item in a generate case", NULL};
    odin3_cval item = {0};
    odin3_status st = odin3_elab1_eval_in(env, gc->tmp, &req, &item);
    odin3_cval eq = {0};
    odin3_cop_args args = {ODIN3_COP_EQ, &gc->value, &item, 1, false};
    st = st != ODIN3_OK ? st : odin3_cval_op(gc->tmp, &args, &eq);
    *out = st == ODIN3_OK && odin3_cval_truth(&eq) == ODIN3_BIT_1;
    return st;
}

/* Item k's expressions seen and, while nothing matched, tried in order. */
static odin3_status case_item(odin3_elab1_env *env, g_case *gc, uint32_t index, odin3_ast_id item) {
    const odin3_ast *ast = env->e1->parsed;
    odin3_ast_span exprs = odin3_ast_children(ast, item);
    odin3_status st = ODIN3_OK;
    bool hit = false;
    for (uint32_t i = G_ITEM_EXPRS; st == ODIN3_OK && i < exprs.n; i++) {
        st = odin3_elab1_gen_seen(env, exprs.ids[i], gc->scope);
        if (st == ODIN3_OK && gc->valid && gc->chosen == G_NONE && !hit) {
            st = case_match(env, gc, exprs.ids[i], &hit);
            gc->valid = st == ODIN3_OK;
            st = st == ODIN3_ERR_PARSE ? ODIN3_OK : st;
        }
    }
    bool is_default = (odin3_ast_flags(ast, item) & ODIN3_AST_F_CASE_ITEM_DEFAULT) != 0;
    if (st != ODIN3_OK || (!hit && !is_default)) {
        return st;
    }
    uint32_t name = 0;
    st = block_name(env->e1, odin3_ast_child(ast, item, 0), gc->scope, gc->counter, &name);
    if (hit) {
        gc->chosen = index;
        gc->name = name;
    } else if (gc->dflt == G_NONE) {
        gc->dflt = index;
        gc->dflt_name = name;
    }
    return st;
}

/* The case expression seen and evaluated, then each item tried in order (gc->tmp holds the
 * evaluator's temporaries). */
static odin3_status case_decide(odin3_elab1_env *env, g_case *gc, odin3_ast_id node) {
    const odin3_ast *ast = env->e1->parsed;
    odin3_ast_id expr = odin3_ast_child(ast, node, 0);
    odin3_status st = odin3_elab1_gen_seen(env, expr, gc->scope);
    if (st == ODIN3_OK) {
        odin3_elab1_eval_req req = {expr, "generate case expression", NULL};
        st = odin3_elab1_eval_in(env, gc->tmp, &req, &gc->value);
        gc->valid = st == ODIN3_OK;
        st = st == ODIN3_ERR_PARSE ? ODIN3_OK : st;
    }
    odin3_ast_span items = odin3_ast_children(ast, node);
    for (uint32_t k = G_CASE_ITEMS; st == ODIN3_OK && k < items.n; k++) {
        st = case_item(env, gc, k - G_CASE_ITEMS, items.ids[k]);
    }
    return st;
}

odin3_status odin3_elab1_gen_case(odin3_elab1 *e1, odin3_ast_id node, uint32_t scope) {
    odin3_elab1_env *env = e1->env;
    env->made_n = 0;
    env->cur_scope = scope;
    g_case gc = {scope, count_construct(env, scope), {0}, NULL, false, G_NONE, 0, G_NONE, 0};
    gc.tmp = odin3_arena_create(ODIN3_E1_TMP_CHUNK);
    odin3_status st = gc.tmp != NULL ? case_decide(env, &gc, node) : ODIN3_ERR_NO_MEMORY;
    odin3_arena_destroy(gc.tmp);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_ast_span items = odin3_ast_children(e1->parsed, node);
    if (gc.chosen == G_NONE && gc.valid) {
        gc.chosen = gc.dflt;
        gc.name = gc.dflt_name;
    }
    bool has = gc.valid && gc.chosen != G_NONE;
    st = log_push(e1, node, has ? (int32_t)gc.chosen : ODIN3_DECISION_SKIPPED);
    odin3_elab1_scope_x ext = {has ? items.ids[gc.chosen + G_CASE_ITEMS] : node, 0, 0};
    return st != ODIN3_OK || !has ? st : make_block(env, &ext, scope, gc.name);
}

/* --- GEN_FOR (EL-19) ------------------------------------------------------------------------- */

/* One generate loop being unrolled. */
typedef struct g_loop {
    odin3_elab1 *e1;
    odin3_elab1_env *env;
    odin3_ast_id node;
    uint32_t scope;
    uint32_t genvar;        /* the genvar's slot */
    uint32_t name;          /* the genvar's name */
    odin3_vec values;       /* int32_t, in iteration order */
    odin3_u64map *distinct; /* the values seen */
} g_loop;

/* The genvar of the loop: init and step assign one name, a genvar not driving an enclosing loop.
 * PARSE after a diagnostic. */
static odin3_status loop_genvar(g_loop *lp) {
    const odin3_ast *ast = lp->e1->parsed;
    odin3_ast_id init = odin3_ast_child(ast, odin3_ast_child(ast, lp->node, G_FOR_INIT), 0);
    odin3_ast_id step = odin3_ast_child(ast, odin3_ast_child(ast, lp->node, G_FOR_STEP), 0);
    bool idents = odin3_ast_kind_of(ast, init) == ODIN3_AST_IDENT &&
                  odin3_ast_kind_of(ast, step) == ODIN3_AST_IDENT;
    const char *text = odin3_ast_kind_of(ast, init) == ODIN3_AST_IDENT
                           ? odin3_ast_name_str(ast, init)
                           : odin3_ast_name_str(ast, step);
    if (!idents || odin3_ast_name(ast, init) != odin3_ast_name(ast, step)) {
        g_error(lp->env, lp->node,
                "generate loop must assign genvar '%s' in its initialisation and step", text);
        return ODIN3_ERR_PARSE;
    }
    lp->name = odin3_ast_name(ast, init);
    lp->genvar = odin3_elab1_env_lookup(lp->env, lp->name);
    const odin3_elab1_slot *slot =
        lp->genvar != G_NONE ? odin3_elab1_env_slot(lp->env, lp->genvar) : NULL;
    if (slot == NULL || slot->entry.kind != ODIN3_ENTRY_GENVAR) {
        g_error(lp->env, init, "'%s' is not a genvar", text);
        return ODIN3_ERR_PARSE;
    }
    if (slot->bound) {
        g_error(lp->env, init, "genvar '%s' is already used by an enclosing loop", text);
        return ODIN3_ERR_PARSE;
    }
    return ODIN3_OK;
}

/* The genvar's slot holds value (32-bit signed). */
static odin3_status bind_value(odin3_arena *arena, odin3_elab1_slot *slot, int32_t value) {
    odin3_cval_int num = {value, G_INT_BITS, true};
    odin3_cval val = {0};
    odin3_status st = odin3_cval_from_i64(arena, &num, &val);
    if (st == ODIN3_OK) {
        slot->entry.value = val;
        slot->entry.width = G_INT_BITS;
        slot->entry.is_signed = true;
        slot->msb = G_INT_BITS - 1;
        slot->lsb = 0;
        slot->bound = true;
        slot->state = ODIN3_E1_DONE;
    }
    return st;
}

/* Iteration value v of the loop: not repeated, within the per-loop and the run's caps. */
static odin3_status admit(g_loop *lp, int32_t value) {
    const odin3_elab_limits *limits = lp->e1->limits;
    if (odin3_u64map_get(lp->distinct, (uint32_t)value, NULL)) {
        g_error(lp->env, lp->node, "generate loop of '%s' repeats the value %" PRId32,
                odin3_elab1_str(lp->e1, lp->name), value);
        return ODIN3_ERR_PARSE;
    }
    if (lp->values.len >= limits->max_loop_iterations) {
        g_error(lp->env, lp->node, "loop exceeds %" PRIu32 " iterations",
                limits->max_loop_iterations);
        return ODIN3_ERR_PARSE;
    }
    if (lp->e1->n_gen_iters >= limits->max_total_iterations) {
        odin3_elab1_fatal(lp->e1, odin3_ast_loc(lp->e1->parsed, lp->node),
                          "unrolled loops exceed %" PRIu32 " iterations in this design",
                          limits->max_total_iterations);
        return ODIN3_ERR_PARSE;
    }
    int32_t *slot = odin3_vec_push(&lp->values);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    lp->e1->n_gen_iters++;
    return odin3_u64map_put(lp->distinct, (odin3_kv){(uint32_t)value, 1});
}

/* One iteration of the unroll: the genvar bound to *value, the condition tested, the value
 * admitted and the step folded. The genvar's binding and the evaluator's temporaries live in a
 * scratch arena freed by the caller, so a runaway loop at the cap holds nothing per iteration. */
static odin3_status unroll_step(g_loop *lp, odin3_arena *tmp, int32_t *value, bool *more) {
    const odin3_ast *ast = lp->e1->parsed;
    odin3_elab1_env *env = lp->env;
    odin3_ast_id cond = odin3_ast_child(ast, lp->node, G_FOR_COND);
    odin3_ast_id step =
        odin3_ast_child(ast, odin3_ast_child(ast, lp->node, G_FOR_STEP), G_ASSIGN_RHS);
    odin3_status st = bind_value(tmp, odin3_elab1_env_slot(env, lp->genvar), *value);
    st = st != ODIN3_OK ? st : truth_of(env, tmp, cond, "generate loop condition", more);
    st = st != ODIN3_OK || !*more ? st : admit(lp, *value);
    return st != ODIN3_OK || !*more ? st
                                    : odin3_elab1_fold_int(env, step, "generate loop step", value);
}

/* init, then cond and step while cond holds, the genvar bound to each value in turn. */
static odin3_status unroll(g_loop *lp) {
    const odin3_ast *ast = lp->e1->parsed;
    odin3_ast_id init =
        odin3_ast_child(ast, odin3_ast_child(ast, lp->node, G_FOR_INIT), G_ASSIGN_RHS);
    int32_t value = 0;
    odin3_status st = odin3_elab1_fold_int(lp->env, init, "generate loop initialisation", &value);
    bool more = st == ODIN3_OK;
    while (more && !lp->e1->stopped) {
        odin3_arena *tmp = odin3_arena_create(ODIN3_E1_TMP_CHUNK);
        if (tmp == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        st = unroll_step(lp, tmp, &value, &more);
        odin3_arena_destroy(tmp);
        more = more && st == ODIN3_OK;
    }
    return st;
}

/* The iteration scopes of the unrolled loop, each binding the genvar to its value. */
static odin3_status make_iterations(g_loop *lp, uint32_t name) {
    odin3_elab1_env *env = lp->env;
    const odin3_elab1_slot *genvar = odin3_vec_cat(&env->slots, lp->genvar);
    odin3_ast_id decl = genvar->entry.decl; /* the slots grow below */
    odin3_ast_id group = genvar->group;
    odin3_status st = ODIN3_OK;
    uint32_t first = (uint32_t)env->scopes.len;
    for (size_t k = 0; st == ODIN3_OK && k < lp->values.len; k++) {
        int32_t value = *(const int32_t *)odin3_vec_cat(&lp->values, k);
        odin3_elab_scope rec = {ODIN3_ESCOPE_BLOCK, name, value, true, lp->scope, 0};
        odin3_elab1_scope_x ext = {lp->node, G_FOR_BODY, 0};
        uint32_t made = 0;
        st = new_scope(env, &rec, ext, &made);
        odin3_elab1_slot proto = {.entry = {.kind = ODIN3_ENTRY_GENVAR, .decl = decl},
                                  .group = group,
                                  .name = lp->name,
                                  .scope = made};
        st = st != ODIN3_OK ? st : odin3_elab1_env_add(env, &proto);
        uint32_t idx = odin3_elab1_env_find(env, made, lp->name);
        st = st != ODIN3_OK ? st : bind_value(env->arena, odin3_elab1_env_slot(env, idx), value);
        uint64_t key = (uint64_t)first << G_SHIFT | (uint32_t)value;
        st =
            st != ODIN3_OK ? st : odin3_u64map_put(env->iters, (odin3_kv){key, (uint64_t)made + 1});
    }
    uint64_t kid = (uint64_t)lp->scope << G_SHIFT | name;
    if (st == ODIN3_OK && lp->values.len > 0 && !odin3_u64map_get(env->kids, kid, NULL)) {
        st = odin3_u64map_put(env->kids, (odin3_kv){kid, (uint64_t)first + 1});
    }
    env->made_first = first;
    env->made_n = st == ODIN3_OK ? (uint32_t)lp->values.len : 0;
    return st;
}

/* The decisions of the loop: its count, then each value. */
static odin3_status log_loop(g_loop *lp) {
    odin3_status st = log_push(lp->e1, lp->node, (int32_t)lp->values.len);
    for (size_t k = 0; st == ODIN3_OK && k < lp->values.len; k++) {
        st = log_push(lp->e1, lp->node, *(const int32_t *)odin3_vec_cat(&lp->values, k));
    }
    return st;
}

/* The loop's expressions seen, its genvar checked, its block named and its values unrolled (the
 * genvar's slot restored after). */
static odin3_status run_loop(g_loop *lp, uint32_t counter, uint32_t *name) {
    const odin3_ast *ast = lp->e1->parsed;
    odin3_status st = ODIN3_OK;
    for (uint32_t k = G_FOR_INIT; st == ODIN3_OK && k <= G_FOR_STEP; k++) {
        st = odin3_elab1_gen_seen(lp->env, odin3_ast_child(ast, lp->node, k), lp->scope);
    }
    st = st != ODIN3_OK ? st : loop_genvar(lp);
    st = st != ODIN3_OK ? st
                        : block_name(lp->e1, odin3_ast_child(ast, lp->node, G_FOR_BODY), lp->scope,
                                     counter, name);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_elab1_slot saved = *odin3_elab1_env_slot(lp->env, lp->genvar);
    st = unroll(lp);
    *odin3_elab1_env_slot(lp->env, lp->genvar) = saved;
    return st;
}

odin3_status odin3_elab1_gen_for(odin3_elab1 *e1, odin3_ast_id node, uint32_t scope) {
    odin3_elab1_env *env = e1->env;
    env->made_n = 0;
    env->cur_scope = scope;
    g_loop lp = {e1, env, node, scope, G_NONE, 0, {0}, odin3_u64map_create(0)};
    odin3_vec_init(&lp.values, sizeof(int32_t));
    uint32_t counter = count_construct(env, scope);
    uint32_t name = 0;
    odin3_status st = lp.distinct != NULL ? run_loop(&lp, counter, &name) : ODIN3_ERR_NO_MEMORY;
    if (st == ODIN3_ERR_PARSE) {
        odin3_vec_clear(&lp.values); /* reported: nothing is elaborated */
        st = ODIN3_OK;
    }
    st = st != ODIN3_OK ? st : log_loop(&lp);
    if (st == ODIN3_OK && lp.values.len > 0) {
        st = make_iterations(&lp, name);
    }
    odin3_vec_free(&lp.values);
    odin3_u64map_destroy(lp.distinct);
    return st;
}

/* --- references into scopes (EL-22, G7) ------------------------------------------------------ */

/* The first component's scope: the innermost from scope outward that holds it (a block or a
 * declaration), else scope. */
static uint32_t first_scope(const odin3_elab1_env *env, uint32_t scope, uint32_t name) {
    uint64_t key = (uint64_t)scope << G_SHIFT | name;
    for (uint32_t at = scope; at != G_NONE;) {
        if (odin3_u64map_get(env->kids, key, NULL) ||
            odin3_elab1_env_find(env, at, (uint32_t)(key & UINT32_MAX)) != G_NONE) {
            return at;
        }
        at = odin3_elab1_scope_at(env, at)->parent;
        key = (uint64_t)at << G_SHIFT | (key & UINT32_MAX);
    }
    return scope;
}

/* One component's step from scope at: *next the block it names (STOP with *next G_NONE when it
 * names no elaborated block). */
static odin3_elab1_walked walk_step(const odin3_elab1_env *env, uint32_t at,
                                    const odin3_elab1_comp *comp, uint32_t *next) {
    *next = G_NONE;
    uint64_t first = 0;
    if (!odin3_u64map_get(env->kids, (uint64_t)at << G_SHIFT | comp->name, &first)) {
        uint32_t idx = odin3_elab1_env_find(env, at, comp->name);
        const odin3_elab1_slot *slot = idx != G_NONE ? odin3_vec_cat(&env->slots, idx) : NULL;
        bool block = slot != NULL && slot->entry.kind == ODIN3_ENTRY_BLOCK;
        return block ? ODIN3_WALK_NOT_ELAB : ODIN3_WALK_STOP;
    }
    const odin3_elab_scope *rec = odin3_elab1_scope_at(env, (uint32_t)first - 1);
    if (rec->has_index != comp->has_index) {
        return ODIN3_WALK_BAD;
    }
    uint64_t found = first;
    uint64_t key = (first - 1) << G_SHIFT | (uint32_t)comp->index;
    if (rec->has_index && !odin3_u64map_get(env->iters, key, &found)) {
        return ODIN3_WALK_NOT_ELAB;
    }
    *next = (uint32_t)found - 1;
    return ODIN3_WALK_STOP;
}

odin3_elab1_walked odin3_elab1_scope_walk(const odin3_elab1_env *env, const odin3_elab1_path *path,
                                          odin3_elab1_reach *out) {
    uint32_t at =
        path->count > 0 ? first_scope(env, path->scope, path->comps[0].name) : path->scope;
    for (uint32_t k = 0; k < path->count; k++) {
        out->reached = at;
        out->used = k;
        uint32_t next = G_NONE;
        odin3_elab1_walked walked = walk_step(env, at, &path->comps[k], &next);
        if (walked != ODIN3_WALK_STOP || next == G_NONE) {
            return walked;
        }
        at = next;
    }
    out->reached = at;
    out->used = path->count;
    return ODIN3_WALK_STOP;
}

/* comps[0 .. n) as written: `g[0].t`. */
static odin3_status path_text(const odin3_elab1 *e1, const odin3_elab1_comp *comps, uint32_t count,
                              odin3_strbuf *out) {
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < count; k++) {
        st =
            odin3_strbuf_appendf(out, "%s%s", k > 0 ? "." : "", odin3_elab1_str(e1, comps[k].name));
        if (st == ODIN3_OK && comps[k].has_index) {
            st = odin3_strbuf_appendf(out, "[%" PRId32 "]", comps[k].index);
        }
    }
    return st;
}

odin3_status odin3_elab1_hier_comps(odin3_elab1_env *env, odin3_ast_id hier,
                                    odin3_elab1_comp **out) {
    const odin3_ast *ast = env->e1->parsed;
    odin3_ast_span parts = odin3_ast_children(ast, hier);
    odin3_elab1_comp *comps = odin3_arena_alloc(env->arena, parts.n * sizeof *comps);
    if (comps == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; st == ODIN3_OK && k < parts.n; k++) {
        odin3_ast_id part = parts.ids[k];
        bool sel = odin3_ast_kind_of(ast, part) == ODIN3_AST_SELECT;
        comps[k] = (odin3_elab1_comp){
            odin3_ast_name(ast, sel ? odin3_ast_child(ast, part, 0) : part), 0, sel};
        if (sel) {
            st = odin3_elab1_fold_int(env, odin3_ast_child(ast, part, 1), "generate block index",
                                      &comps[k].index);
        }
    }
    *out = comps;
    return st;
}

/* The diagnostic of a path that does not reach a declaration: the walk's result, its use. */
typedef struct g_ref {
    odin3_ast_id hier;
    odin3_elab1_path path;
    odin3_elab1_walked walked;
    uint32_t used;
} g_ref;

static odin3_status ref_error(const odin3_elab1_env *env, const g_ref *ref, const char *fmt) {
    odin3_strbuf full;
    odin3_strbuf part;
    odin3_strbuf_init(&full);
    odin3_strbuf_init(&part);
    odin3_status st = path_text(env->e1, ref->path.comps, ref->path.count, &full);
    st = st != ODIN3_OK ? st : path_text(env->e1, ref->path.comps, ref->used + 1, &part);
    if (st == ODIN3_OK && ref->walked == ODIN3_WALK_NOT_ELAB) {
        g_error(env, ref->hier, "'%s' does not exist (generate block '%s' was not elaborated)",
                full.data, part.data);
    } else if (st == ODIN3_OK) {
        g_error(env, ref->hier, fmt, full.data);
    }
    odin3_strbuf_free(&full);
    odin3_strbuf_free(&part);
    return st != ODIN3_OK ? st : ODIN3_ERR_PARSE;
}

odin3_status odin3_elab1_scope_target(odin3_elab1_env *env, odin3_ast_id hier_name,
                                      uint32_t *slot) {
    *slot = G_NONE;
    odin3_elab1_comp *comps = NULL;
    odin3_status st = odin3_elab1_hier_comps(env, hier_name, &comps);
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t count = odin3_ast_nchild(env->e1->parsed, hier_name);
    g_ref ref = {hier_name, {comps, count, env->cur_scope}, ODIN3_WALK_STOP, 0};
    odin3_elab1_reach reach = {0, 0};
    ref.walked = odin3_elab1_scope_walk(env, &ref.path, &reach);
    ref.used = reach.used;
    if (ref.walked != ODIN3_WALK_STOP || ref.used == 0 || ref.used + 1 != count ||
        comps[count - 1].has_index) {
        ref.used = ref.used < count ? ref.used : count - 1;
        return ref_error(env, &ref, "hierarchical reference '%s' is not supported");
    }
    uint32_t idx = odin3_elab1_env_find(env, reach.reached, comps[count - 1].name);
    if (idx == G_NONE || odin3_elab1_env_slot(env, idx)->entry.kind == ODIN3_ENTRY_BLOCK) {
        return ref_error(env, &ref, "'%s' is not declared");
    }
    *slot = idx;
    return ODIN3_OK;
}

odin3_status odin3_elab1_scope_ref(odin3_elab1 *e1, odin3_ast_id hier_name, uint32_t scope) {
    odin3_elab1_env *env = e1->env;
    uint32_t saved = env->cur_scope;
    env->cur_scope = scope;
    uint32_t slot = 0;
    odin3_status st = odin3_elab1_scope_target(env, hier_name, &slot);
    env->cur_scope = saved;
    return st;
}
