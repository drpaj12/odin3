/*
 * design.c — design create/destroy, the design-global string table, module list, prov store,
 * source manager and per-run AST stores.
 */
#include "ir/design.h"
#include "ast/ast.h"
#include "ast/srcman.h"
#include "ir/ir_internal.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>

odin3_design *odin3_design_create(void) {
    odin3_design *design = odin3_util_calloc(sizeof *design);
    if (design == NULL) {
        return NULL;
    }
    odin3_vec_init(&design->asts, sizeof(odin3_ast *));
    design->arena = odin3_arena_create(0);
    design->strtab = odin3_strtab_create();
    if (design->arena == NULL || design->strtab == NULL ||
        odin3_celltype_table_init(design) != ODIN3_OK ||
        odin3_module_table_init(design) != ODIN3_OK || odin3_prov_store_init(design) != ODIN3_OK) {
        odin3_design_destroy(design);
        return NULL;
    }
    return design;
}

void odin3_design_destroy(odin3_design *design) {
    if (design == NULL) {
        return;
    }
    for (size_t i = 0; i < design->asts.len; i++) {
        odin3_ast_destroy(*(odin3_ast **)odin3_vec_at(&design->asts, i));
    }
    odin3_vec_free(&design->asts);
    odin3_srcman_destroy(design->srcman);
    odin3_prov_store_free(design);
    odin3_module_table_free(design);
    odin3_celltype_table_free(design);
    odin3_strtab_destroy(design->strtab);
    odin3_arena_destroy(design->arena);
    odin3_util_free(design);
}

odin3_strtab *odin3_design_strtab(const odin3_design *design) {
    return design->strtab;
}

odin3_status odin3_design_intern(odin3_design *design, odin3_bytes bytes, uint32_t *str) {
    return odin3_strtab_intern(design->strtab, bytes, str);
}

odin3_status odin3_design_get_srcman(odin3_design *design, odin3_srcman **out) {
    if (design->srcman == NULL) {
        design->srcman = odin3_srcman_create(design->strtab);
        if (design->srcman == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    *out = design->srcman;
    return ODIN3_OK;
}

/* --- per-run AST stores (AST-15, AST-16) --------------------------------------------------- */

/* The index of the held store of (run, form), or asts.len when none. */
static size_t find_ast(const odin3_design *design, odin3_passrun_id run, odin3_ast_form form) {
    size_t i = 0;
    for (; i < design->asts.len; i++) {
        const odin3_ast *ast = *(odin3_ast *const *)odin3_vec_cat(&design->asts, i);
        if (odin3_ast_run(ast).v == run.v && odin3_ast_form_of(ast) == form) {
            break;
        }
    }
    return i;
}

odin3_status odin3_design_set_ast(odin3_design *design, odin3_ast *ast) {
    const char *why = design == NULL || ast == NULL ? "NULL argument" : NULL;
    if (why == NULL && odin3_ast_design(ast) != design) {
        why = "the store belongs to another design";
    }
    if (why == NULL &&
        find_ast(design, odin3_ast_run(ast), odin3_ast_form_of(ast)) < design->asts.len) {
        why = "a store of the same run and form is held";
    }
    if (why != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_design_set_ast: %s", why);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_ast **slot = (odin3_ast **)odin3_vec_push(&design->asts);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = ast;
    return ODIN3_OK;
}

odin3_status odin3_design_get_ast(const odin3_design *design, odin3_passrun_id run,
                                  odin3_ast_form form, odin3_ast **out) {
    if (design == NULL || out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_design_get_ast: NULL argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    size_t i = find_ast(design, run, form);
    *out = i < design->asts.len ? *(odin3_ast *const *)odin3_vec_cat(&design->asts, i) : NULL;
    return ODIN3_OK;
}

void odin3_design_drop_ast(odin3_design *design, odin3_passrun_id run, odin3_ast_form form) {
    bool keep =
        design->keep_ast == ODIN3_KEEP_AST_ALL ||
        (design->keep_ast == ODIN3_KEEP_AST_ELABORATED && form == ODIN3_AST_FORM_ELABORATED);
    size_t at = find_ast(design, run, form);
    if (keep || at >= design->asts.len) {
        return;
    }
    odin3_ast **asts = (odin3_ast **)design->asts.data;
    odin3_ast_destroy(asts[at]);
    for (size_t i = at + 1; i < design->asts.len; i++) {
        asts[i - 1] = asts[i];
    }
    odin3_vec_pop(&design->asts);
}

void odin3_design_set_keep_ast(odin3_design *design, odin3_keep_ast level) {
    design->keep_ast = level;
}

odin3_keep_ast odin3_design_keep_ast(const odin3_design *design) {
    return design->keep_ast;
}
