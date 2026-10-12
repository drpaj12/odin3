/*
 * elab_abi.c — the public ABI's elaboration entry (ABI v4, 2D Task 17): odin3_ast_elaborate, the
 * seam an adapter that builds an elaborated store itself (the slang adapter) drives stage 2
 * through (plan point (b)). Over src/frontends/elab/.
 */
#include "api/api.h"

#include "ast/ast.h"
#include "ast/check.h"
#include "frontends/elab/elab.h"
#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/attr.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* True when every reserved word is zero. */
static bool reserved_zero(const odin3_elab_options *opts) {
    for (size_t i = 0; i < sizeof opts->reserved / sizeof opts->reserved[0]; i++) {
        if (opts->reserved[i] != 0) {
            return false;
        }
    }
    return true;
}

/* The store is the design's, elaborated and passes the shape rules of EL-4 that 2A's check rule 7
 * covers (an adapter's store is untrusted: this runs in every build); the status otherwise. */
static odin3_status check_store(const odin3_design *design, const odin3_ast *ast) {
    odin3_ast *held = NULL;
    if (odin3_ast_form_of(ast) != ODIN3_AST_FORM_ELABORATED) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_ast_elaborate: the store is not elaborated");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st =
        odin3_design_find_ast(design, odin3_ast_run(ast), ODIN3_AST_FORM_ELABORATED, &held);
    if (st != ODIN3_OK || held != ast) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_ast_elaborate: the store is not held by the design");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (odin3_passrun_name(design, odin3_ast_run(ast)) == 0) {
        odin3_log(ODIN3_LOG_ERROR,
                  "odin3_ast_elaborate: the store's run is not a run of the design");
        return ODIN3_ERR_INVALID_ARG;
    }
    st = odin3_ast_check(ast, ODIN3_CHECK_FULL);
    if (st == ODIN3_ERR_CHECK) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_ast_elaborate: the store breaks the elaborated form");
        return ODIN3_ERR_INVALID_ARG;
    }
    return st;
}

ODIN3_EXPORT odin3_status odin3_ast_elaborate(odin3_design *design, odin3_ast *ast,
                                              const odin3_elab_options *opts) {
    if (design == NULL || ast == NULL) {
        return odin3_api_invalid(__func__);
    }
    if (opts != NULL && !reserved_zero(opts)) {
        return odin3_api_invalid_why(__func__, "the reserved words must be zero");
    }
    odin3_status st = check_store(design, ast);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_elab_opts given = {0};
    if (opts != NULL) {
        given.nolatches = opts->nolatches;
        given.nomem2reg = opts->nomem2reg;
    }
    odin3_pass_ctx ctx = {design, odin3_ast_run(ast), 0};
    return odin3_elab_stage2(&ctx, ast, &given);
}
