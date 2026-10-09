/* width.c — compiled width expressions (the IR's fourth width rule) and the identifier walk. */
#include "techlib/width.h"

#include <stdio.h>
#include <string.h>

/* The producer data behind an odin3_width_expr: the tree and its distinct identifiers. */
typedef struct width_impl {
    const odin3_expr *expr;
    const odin3_strtab *strtab;
    const uint32_t *idents; /* strtab IDs, first-occurrence order */
    const uint32_t *params; /* per identifier: its parameter index in the definition */
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

static const char *ident_name(const width_impl *impl, uint32_t idx) {
    return odin3_strtab_get(impl->strtab, impl->idents[idx]);
}

static bool width_check(const odin3_width_expr *wexpr, const odin3_celltype_def *def,
                        odin3_width_why *why) {
    const width_impl *impl = wexpr->impl;
    for (uint32_t i = 0; i < impl->n_idents; i++) {
        uint32_t idx = impl->params[i];
        const char *name = ident_name(impl, i);
        if (idx >= def->n_params || strcmp(def->params[idx].name, name) != 0 ||
            def->params[idx].kind != ODIN3_VAL_INT) {
            (void)snprintf(why->text, sizeof why->text,
                           "width expression names '%s', which is not an int parameter of the type",
                           name);
            return false;
        }
    }
    return true;
}

/* Lookup context of one evaluation: the compiled identifiers and the node's parameter values. */
typedef struct width_env {
    const width_impl *impl;
    const odin3_value *params;
} width_env;

static bool width_lookup(const void *user, uint32_t ident, int64_t *value) {
    const width_env *env = user;
    for (uint32_t i = 0; i < env->impl->n_idents; i++) {
        if (env->impl->idents[i] == ident) {
            *value = env->params[env->impl->params[i]].i;
            return true;
        }
    }
    return false;
}

static odin3_status width_eval(const odin3_width_expr *wexpr, const odin3_width_args *args,
                               uint32_t *width) {
    const width_impl *impl = wexpr->impl;
    odin3_width_why *why = args->why;
    for (uint32_t i = 0; i < impl->n_idents; i++) {
        if (args->params[impl->params[i]].kind != ODIN3_VAL_INT) {
            (void)snprintf(why->text, sizeof why->text, "parameter '%s' is not an int",
                           ident_name(impl, i));
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    const width_env env = {impl, args->params};
    const odin3_expr_env eval_env = {width_lookup, &env, impl->strtab};
    odin3_expr_error err = {0, ""};
    int64_t value = 0;
    odin3_status st = odin3_expr_eval_int_quiet(impl->expr, &eval_env, &value, &err);
    if (st == ODIN3_ERR_INVALID_ARG) {
        (void)snprintf(why->text, sizeof why->text, "%s", err.text);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    if (value < 0 || value > (int64_t)UINT32_MAX) {
        (void)snprintf(why->text, sizeof why->text, "width %lld is outside 0..%u", (long long)value,
                       UINT32_MAX);
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

/* The definition index of parameter name, or UINT32_MAX (rejected later by the check hook). */
static uint32_t resolve(const odin3_width_source *src, uint32_t name) {
    for (uint32_t i = 0; i < src->n_params; i++) {
        if (src->param_names[i] == name) {
            return i;
        }
    }
    return UINT32_MAX;
}

/* Distinct identifier IDs of nodes (const odin3_expr *) and their parameter indices. */
static odin3_status resolve_idents(const odin3_width_source *src, const odin3_vec *nodes,
                                   width_impl *impl) {
    impl->idents = NULL;
    impl->params = NULL;
    impl->n_idents = 0;
    if (nodes->len == 0) {
        return ODIN3_OK;
    }
    uint32_t *ids = odin3_arena_alloc(src->arena, sizeof *ids * nodes->len);
    uint32_t *idx = odin3_arena_alloc(src->arena, sizeof *idx * nodes->len);
    if (ids == NULL || idx == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t count = 0;
    for (size_t i = 0; i < nodes->len; i++) {
        const odin3_expr *const *node = (const odin3_expr *const *)odin3_vec_cat(nodes, i);
        if (!seen((*node)->ident, ids, count)) {
            ids[count] = (*node)->ident;
            idx[count] = resolve(src, (*node)->ident);
            count++;
        }
    }
    impl->idents = ids;
    impl->params = idx;
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
    odin3_status st = resolve_idents(src, nodes, impl);
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
