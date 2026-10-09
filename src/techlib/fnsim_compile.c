/* fnsim_compile.c — compiles a tech-library cell's fn expressions for the simulator (fnsim.h). */
#include "ir/celltype.h"
#include "techlib/expr.h"
#include "techlib/fnsim.h"
#include "techlib/fnsim_internal.h"
#include "techlib/reader.h"
#include "techlib/width.h"
#include "util/arena.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { STAGE_EXPAND, STAGE_EMIT, NOT_FOUND = -1 };

typedef struct frame {
    const odin3_expr *node;
    uint32_t stage;
} frame;

typedef struct compiler {
    const odin3_fnsim_source *src;
    odin3_vec nodes;   /* odin3_fnsim_node */
    odin3_vec items;   /* uint32_t: CONCAT operands */
    odin3_vec frames;  /* frame */
    odin3_vec results; /* uint32_t: node indices of finished operands */
    odin3_vec walk;    /* const odin3_expr *: constant-expression checks */
    bool unsupported;
} compiler;

/* --- names ------------------------------------------------------------------------------------ */

static bool names(const compiler *cc, uint32_t ident, const char *name) {
    const char *str = odin3_strtab_get(cc->src->strtab, ident);
    return str != NULL && strcmp(str, name) == 0;
}

static int find_port(const compiler *cc, uint32_t ident) {
    for (uint32_t i = 0; i < cc->src->def->n_ports; i++) {
        if (names(cc, ident, cc->src->def->ports[i].name)) {
            return (int)i;
        }
    }
    return NOT_FOUND;
}

static int find_param(const compiler *cc, uint32_t ident) {
    for (uint32_t i = 0; i < cc->src->def->n_params; i++) {
        if (names(cc, ident, cc->src->def->params[i].name)) {
            return (int)i;
        }
    }
    return NOT_FOUND;
}

/* --- checks ----------------------------------------------------------------------------------- */

static bool supported_binary(odin3_expr_op op) {
    switch (op) {
    case ODIN3_OP_DIV:
    case ODIN3_OP_MOD:
    case ODIN3_OP_POW:
        return false;
    default:
        return true;
    }
}

static odin3_status push_walk(compiler *cc, const odin3_expr *node) {
    const odin3_expr **slot = (const odin3_expr **)odin3_vec_push(&cc->walk);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = node;
    return ODIN3_OK;
}

/* Pushes every operand of node (any kind) onto the walk stack. */
static odin3_status push_operands(compiler *cc, const odin3_expr *node) {
    const odin3_expr *const kids[3] = {node->a, node->b, node->c};
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < 3; i++) {
        st = kids[i] != NULL ? push_walk(cc, kids[i]) : ODIN3_OK;
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < node->nitems; i++) {
        st = push_walk(cc, node->items[i]);
    }
    return st;
}

/*
 * An index or count: an expression over parameters only, of at most ODIN3_EXPR_EVAL_INLINE nodes
 * (odin3_expr_eval_int then evaluates it without allocating, as a hook must). Anything else marks
 * the cell unsupported.
 */
static odin3_status check_const(compiler *cc, const odin3_expr *expr) {
    odin3_vec_clear(&cc->walk);
    odin3_status st = push_walk(cc, expr);
    uint32_t count = 0;
    while (st == ODIN3_OK && cc->walk.len > 0 && !cc->unsupported) {
        const odin3_expr *const *top =
            (const odin3_expr *const *)odin3_vec_cat(&cc->walk, cc->walk.len - 1);
        const odin3_expr *node = *top;
        odin3_vec_pop(&cc->walk);
        count++;
        if (count > ODIN3_EXPR_EVAL_INLINE ||
            (node->kind == ODIN3_EXPR_IDENT && find_param(cc, node->ident) == NOT_FOUND)) {
            cc->unsupported = true;
        }
        st = push_operands(cc, node);
    }
    return st;
}

/* --- the postorder walk ----------------------------------------------------------------------- */

static odin3_status push_frame(compiler *cc, const odin3_expr *node, uint32_t stage) {
    frame *slot = odin3_vec_push(&cc->frames);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (frame){node, stage};
    return ODIN3_OK;
}

/* The operands that are values (not constant indices or counts), in order, into kids. */
static uint32_t value_operands(const odin3_expr *node, const odin3_expr **kids) {
    switch (node->kind) {
    case ODIN3_EXPR_UNARY:
    case ODIN3_EXPR_BITSEL:
    case ODIN3_EXPR_SLICE:
        kids[0] = node->a;
        return 1;
    case ODIN3_EXPR_BINARY:
        kids[0] = node->a;
        kids[1] = node->b;
        return 2;
    case ODIN3_EXPR_TERNARY:
        kids[0] = node->a;
        kids[1] = node->b;
        kids[2] = node->c;
        return 3;
    case ODIN3_EXPR_REPL:
        kids[0] = node->b;
        return 1;
    default:
        return 0;
    }
}

/* Checks node for support (an IDENT must be an input, inout or parameter). */
static odin3_status check_node(compiler *cc, const odin3_expr *node) {
    switch (node->kind) {
    case ODIN3_EXPR_IDENT: {
        int port = find_port(cc, node->ident);
        cc->unsupported = port != NOT_FOUND ? cc->src->def->ports[port].dir == ODIN3_DIR_OUT
                                            : find_param(cc, node->ident) == NOT_FOUND;
        return ODIN3_OK;
    }
    case ODIN3_EXPR_BINARY:
        cc->unsupported = !supported_binary(node->op);
        return ODIN3_OK;
    case ODIN3_EXPR_BITSEL:
        return check_const(cc, node->b);
    case ODIN3_EXPR_SLICE: {
        odin3_status st = check_const(cc, node->b);
        return st == ODIN3_OK ? check_const(cc, node->c) : st;
    }
    case ODIN3_EXPR_REPL:
        return check_const(cc, node->a);
    default:
        return ODIN3_OK;
    }
}

/* Stage 1: check node, then come back to emit it after its value operands (left first). */
static odin3_status expand(compiler *cc, const odin3_expr *node) {
    odin3_status st = check_node(cc, node);
    if (st != ODIN3_OK || cc->unsupported) {
        return st;
    }
    st = push_frame(cc, node, STAGE_EMIT);
    if (node->kind == ODIN3_EXPR_CONCAT) {
        for (uint32_t i = node->nitems; st == ODIN3_OK && i-- > 0;) {
            st = push_frame(cc, node->items[i], STAGE_EXPAND);
        }
        return st;
    }
    const odin3_expr *kids[3];
    for (uint32_t i = value_operands(node, kids); st == ODIN3_OK && i-- > 0;) {
        st = push_frame(cc, kids[i], STAGE_EXPAND);
    }
    return st;
}

static odin3_fnsim_kind kind_of(const compiler *cc, const odin3_expr *node) {
    static const odin3_fnsim_kind k_kinds[] = {
        [ODIN3_EXPR_INT] = ODIN3_FNSIM_INT,         [ODIN3_EXPR_SIZED] = ODIN3_FNSIM_SIZED,
        [ODIN3_EXPR_UNARY] = ODIN3_FNSIM_UNARY,     [ODIN3_EXPR_BINARY] = ODIN3_FNSIM_BINARY,
        [ODIN3_EXPR_TERNARY] = ODIN3_FNSIM_TERNARY, [ODIN3_EXPR_SLICE] = ODIN3_FNSIM_SLICE,
        [ODIN3_EXPR_BITSEL] = ODIN3_FNSIM_BITSEL,   [ODIN3_EXPR_CONCAT] = ODIN3_FNSIM_CONCAT,
        [ODIN3_EXPR_REPL] = ODIN3_FNSIM_REPL};
    if (node->kind == ODIN3_EXPR_IDENT) {
        return find_port(cc, node->ident) != NOT_FOUND ? ODIN3_FNSIM_PORT : ODIN3_FNSIM_PARAM;
    }
    return k_kinds[node->kind];
}

/* Moves the last n_kids results into out's operands (kid, or items for a concatenation). */
static odin3_status take_operands(compiler *cc, odin3_fnsim_node *out, uint32_t n_kids) {
    size_t base = cc->results.len - n_kids;
    const uint32_t *results = cc->results.data;
    if (out->kind == ODIN3_FNSIM_CONCAT) {
        out->first = (uint32_t)cc->items.len;
        out->count = n_kids;
        if (odin3_vec_reserve(&cc->items, cc->items.len + n_kids) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
        for (uint32_t i = 0; i < n_kids; i++) {
            *(uint32_t *)odin3_vec_push(&cc->items) = results[base + i];
        }
    } else {
        for (uint32_t i = 0; i < n_kids; i++) {
            out->kid[i] = results[base + i];
        }
    }
    while (cc->results.len > base) {
        odin3_vec_pop(&cc->results);
    }
    return ODIN3_OK;
}

/* Stage 2: node's value operands are the last results; replace them with node. */
static odin3_status emit(compiler *cc, const odin3_expr *node) {
    odin3_fnsim_node out = {kind_of(cc, node), node->op, 0, {0, 0, 0}, 0, 0,
                            ODIN3_FNSIM_NONE,  node};
    if (out.kind == ODIN3_FNSIM_PORT) {
        out.ref = (uint32_t)find_port(cc, node->ident);
    } else if (out.kind == ODIN3_FNSIM_PARAM) {
        out.ref = (uint32_t)find_param(cc, node->ident);
    }
    const odin3_expr *kids[3];
    uint32_t n_kids = out.kind == ODIN3_FNSIM_CONCAT ? node->nitems : value_operands(node, kids);
    if (take_operands(cc, &out, n_kids) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_fnsim_node *slot = odin3_vec_push(&cc->nodes);
    uint32_t *res = slot != NULL ? odin3_vec_push(&cc->results) : NULL;
    if (res == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = out;
    *res = (uint32_t)cc->nodes.len - 1;
    return ODIN3_OK;
}

/* Appends the tree of one fn, its root last, marked with the output it drives. */
static odin3_status compile_fn(compiler *cc, const odin3_techlib_fn *fn) {
    odin3_status st = push_frame(cc, fn->expr, STAGE_EXPAND);
    while (st == ODIN3_OK && cc->frames.len > 0 && !cc->unsupported) {
        const frame top = *(const frame *)odin3_vec_cat(&cc->frames, cc->frames.len - 1);
        odin3_vec_pop(&cc->frames);
        st = top.stage == STAGE_EXPAND ? expand(cc, top.node) : emit(cc, top.node);
    }
    if (st == ODIN3_OK && !cc->unsupported) {
        odin3_fnsim_node *root = odin3_vec_at(&cc->nodes, cc->nodes.len - 1);
        root->out = fn->port;
        odin3_vec_clear(&cc->results);
    }
    return st;
}

/* --- the program ------------------------------------------------------------------------------ */

static void *copy_out(odin3_arena *arena, const odin3_vec *vec, bool *ok) {
    if (vec->len == 0) {
        return NULL;
    }
    void *dst = odin3_arena_alloc(arena, vec->len * vec->elem_size);
    if (dst == NULL) {
        *ok = false;
        return NULL;
    }
    memcpy(dst, vec->data, vec->len * vec->elem_size);
    return dst;
}

static odin3_status finish(const compiler *cc, const odin3_fnsim_prog **out) {
    const odin3_celltype_def *def = cc->src->def;
    odin3_fnsim_prog *prog = odin3_arena_alloc(cc->src->arena, sizeof *prog);
    uint32_t *param_ids =
        odin3_arena_alloc(cc->src->arena, sizeof *param_ids * (def->n_params + 1));
    bool ok = prog != NULL && param_ids != NULL;
    if (!ok) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < def->n_params; i++) {
        param_ids[i] = 0;
        (void)odin3_strtab_find(cc->src->strtab, odin3_bytes_cstr(def->params[i].name),
                                &param_ids[i]);
    }
    *prog = (odin3_fnsim_prog){copy_out(cc->src->arena, &cc->nodes, &ok), (uint32_t)cc->nodes.len,
                               copy_out(cc->src->arena, &cc->items, &ok), param_ids, def->n_params};
    if (!ok) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *out = prog;
    return ODIN3_OK;
}

odin3_status odin3_fnsim_compile(const odin3_fnsim_source *src, const odin3_fnsim_prog **out) {
    const odin3_techlib_cell *lib = src->lib;
    if (lib->n_fns == 0 || lib->n_seqs > 0 || lib->memory != NULL) {
        *out = NULL;
        return ODIN3_OK;
    }
    compiler cc = {.src = src, .unsupported = false};
    odin3_vec_init(&cc.nodes, sizeof(odin3_fnsim_node));
    odin3_vec_init(&cc.items, sizeof(uint32_t));
    odin3_vec_init(&cc.frames, sizeof(frame));
    odin3_vec_init(&cc.results, sizeof(uint32_t));
    odin3_vec_init(&cc.walk, sizeof(const odin3_expr *));
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && !cc.unsupported && i < lib->n_fns; i++) {
        st = compile_fn(&cc, &lib->fns[i]);
    }
    if (st == ODIN3_OK && cc.unsupported) {
        *out = NULL;
    } else if (st == ODIN3_OK) {
        st = finish(&cc, out);
    }
    odin3_vec_free(&cc.nodes);
    odin3_vec_free(&cc.items);
    odin3_vec_free(&cc.frames);
    odin3_vec_free(&cc.results);
    odin3_vec_free(&cc.walk);
    return st;
}
