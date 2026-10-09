/* width.c — compiled width expressions (the IR's fourth width rule) and the identifier walk. */
#include "techlib/width.h"

#include "util/log.h"

#include <string.h>

enum { NO_PARAM = -1 };

/* The producer data behind an odin3_width_expr: the tree and its distinct identifiers. */
typedef struct width_impl {
    const odin3_expr *expr;
    const odin3_strtab *strtab;
    const uint32_t *idents; /* strtab IDs, first-occurrence order */
    uint32_t n_idents;
} width_impl;

/* Pushes node (if not NULL) onto the walk stack. */
static odin3_status push_node(odin3_vec *stack, const odin3_expr *node) {
    if (node == NULL) {
        return ODIN3_OK;
    }
    const odin3_expr **slot = (const odin3_expr **)odin3_vec_push(stack);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = node;
    return ODIN3_OK;
}

/* Pushes the children of node so that the leftmost one is popped first. */
static odin3_status push_children(odin3_vec *stack, const odin3_expr *node) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = node->nitems; st == ODIN3_OK && i > 0; i--) {
        st = push_node(stack, node->items[i - 1]);
    }
    const odin3_expr *const kids[3] = {node->c, node->b, node->a};
    for (int i = 0; st == ODIN3_OK && i < 3; i++) {
        st = push_node(stack, kids[i]);
    }
    return st;
}

odin3_status odin3_expr_collect_idents(const odin3_expr *root, odin3_vec *nodes) {
    odin3_vec stack;
    odin3_vec_init(&stack, sizeof(const odin3_expr *));
    odin3_status st = push_node(&stack, root);
    while (st == ODIN3_OK && stack.len > 0) {
        const odin3_expr *const *top =
            (const odin3_expr *const *)odin3_vec_cat(&stack, stack.len - 1);
        const odin3_expr *node = *top;
        odin3_vec_pop(&stack);
        if (node->kind == ODIN3_EXPR_IDENT) {
            st = push_node(nodes, node);
        } else {
            st = push_children(&stack, node);
        }
    }
    odin3_vec_free(&stack);
    return st;
}

static int param_index(const odin3_celltype_def *def, const char *name) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (strcmp(def->params[i].name, name) == 0) {
            return (int)i;
        }
    }
    return NO_PARAM;
}

static const char *width_check(const odin3_width_expr *wexpr, const odin3_celltype_def *def) {
    const width_impl *impl = wexpr->impl;
    for (uint32_t i = 0; i < impl->n_idents; i++) {
        int idx = param_index(def, odin3_strtab_get(impl->strtab, impl->idents[i]));
        if (idx == NO_PARAM || def->params[idx].kind != ODIN3_VAL_INT) {
            return "width expression names something that is not an int parameter of the type";
        }
    }
    return NULL;
}

/* Lookup context of one evaluation: the definition and the node's parameter values. */
typedef struct width_env {
    const odin3_celltype_def *def;
    const odin3_value *params;
    const odin3_strtab *strtab;
} width_env;

static bool width_lookup(const void *user, uint32_t ident, int64_t *value) {
    const width_env *env = user;
    int idx = param_index(env->def, odin3_strtab_get(env->strtab, ident));
    if (idx == NO_PARAM || env->params[idx].kind != ODIN3_VAL_INT) {
        return false;
    }
    *value = env->params[idx].i;
    return true;
}

/* ODIN3_OK when every parameter the expression reads holds an INT. */
static odin3_status check_kinds(const width_impl *impl, const width_env *env) {
    for (uint32_t i = 0; i < impl->n_idents; i++) {
        const char *name = odin3_strtab_get(impl->strtab, impl->idents[i]);
        int idx = param_index(env->def, name);
        if (idx == NO_PARAM || env->params[idx].kind != ODIN3_VAL_INT) {
            odin3_log(ODIN3_LOG_ERROR, "width: parameter '%s' of cell type '%s' is not an int",
                      name, env->def->name);
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    return ODIN3_OK;
}

static odin3_status width_eval(const odin3_width_expr *wexpr, const odin3_celltype_def *def,
                               const odin3_value *params, uint32_t *width) {
    const width_impl *impl = wexpr->impl;
    const width_env env = {def, params, impl->strtab};
    odin3_status st = check_kinds(impl, &env);
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_expr_env eval_env = {width_lookup, &env, impl->strtab};
    int64_t value = 0;
    st = odin3_expr_eval_int(impl->expr, &eval_env, &value);
    if (st != ODIN3_OK) {
        return st;
    }
    if (value < 0 || value > (int64_t)UINT32_MAX) {
        odin3_log(ODIN3_LOG_ERROR, "width: %lld is outside 0..%u", (long long)value, UINT32_MAX);
        return ODIN3_ERR_INVALID_ARG;
    }
    *width = (uint32_t)value;
    return ODIN3_OK;
}

static bool seen(uint32_t id, const uint32_t *ids, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (ids[i] == id) {
            return true;
        }
    }
    return false;
}

/* Distinct identifier IDs of nodes (const odin3_expr *), copied into the arena. */
static odin3_status distinct_idents(odin3_arena *arena, const odin3_vec *nodes, width_impl *impl) {
    impl->idents = NULL;
    impl->n_idents = 0;
    if (nodes->len == 0) {
        return ODIN3_OK;
    }
    uint32_t *ids = odin3_arena_alloc(arena, sizeof *ids * nodes->len);
    if (ids == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t count = 0;
    for (size_t i = 0; i < nodes->len; i++) {
        const odin3_expr *const *node = (const odin3_expr *const *)odin3_vec_cat(nodes, i);
        if (!seen((*node)->ident, ids, count)) {
            ids[count++] = (*node)->ident;
        }
    }
    impl->idents = ids;
    impl->n_idents = count;
    return ODIN3_OK;
}

static odin3_status compile_into(const odin3_width_source *src, const odin3_vec *nodes,
                                 const odin3_width_expr **out) {
    width_impl *impl = odin3_arena_alloc(src->arena, sizeof *impl);
    odin3_width_expr *wexpr = odin3_arena_alloc(src->arena, sizeof *wexpr);
    if (impl == NULL || wexpr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    impl->expr = src->expr;
    impl->strtab = src->strtab;
    odin3_status st = distinct_idents(src->arena, nodes, impl);
    if (st != ODIN3_OK) {
        return st;
    }
    wexpr->check = width_check;
    wexpr->eval = width_eval;
    wexpr->impl = impl;
    *out = wexpr;
    return ODIN3_OK;
}

odin3_status odin3_width_expr_compile(const odin3_width_source *src, const odin3_width_expr **out) {
    odin3_vec nodes;
    odin3_vec_init(&nodes, sizeof(const odin3_expr *));
    odin3_status st = odin3_expr_collect_idents(src->expr, &nodes);
    if (st == ODIN3_OK) {
        st = compile_into(src, &nodes, out);
    }
    odin3_vec_free(&nodes);
    return st;
}

const odin3_expr *odin3_width_expr_tree(const odin3_width_expr *wexpr) {
    const width_impl *impl = wexpr->impl;
    return impl->expr;
}
