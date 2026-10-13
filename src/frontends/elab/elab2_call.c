/*
 * elab2_call.c — user function and task calls at the seam of stage 2's expressions (2D Task 23;
 * EL-17, EL-38): the typer's type of a CALL (the function's result variable), the callee and its
 * result variable, and the interpreter's host over the elaborated store, through which a call with
 * constant arguments and a constant body is folded before anything is inlined (Yosys does the same
 * before inlining). The inlining itself is pg_call.c's.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/symtab.h"
#include "frontends/elab/ceval.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab_internal.h"
#include "odin3/odin3.h"
#include "util/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { INTEGER_BITS = 32 };

static const odin3_symbol *call_sym(const odin3_elab2 *e2, odin3_ast_id node) {
    return odin3_symbol_get(e2->st, odin3_elab_sym_of(&e2->bind, node));
}

odin3_status odin3_elab2_callee(odin3_elab2 *e2, odin3_ast_id call, odin3_ast_id *decl) {
    const odin3_symbol *rec = call_sym(e2, call);
    bool is_call = odin3_ast_kind_of(e2->elab, call) == ODIN3_AST_CALL;
    const char *name = odin3_ast_name_str(e2->elab, call);
    odin3_loc loc = odin3_ast_loc(e2->elab, call);
    if ((odin3_ast_flags(e2->elab, call) & ODIN3_AST_F_CALL_SYSTEM) != 0) {
        odin3_elab2_error(e2, loc, "system function '%s' is not supported", name);
    } else if (rec == NULL) {
        odin3_elab2_error(e2, loc, "'%s' is not declared", name);
    } else if (rec->kind != (is_call ? ODIN3_SYM_FUNCTION : ODIN3_SYM_TASK)) {
        odin3_elab2_error(e2, loc, is_call ? "'%s' is not a function" : "'%s' is not a task", name);
    } else {
        *decl = rec->decl;
        return ODIN3_OK;
    }
    return ODIN3_ERR_PARSE;
}

const odin3_symbol *odin3_elab2_fn_result(const odin3_elab2 *e2, odin3_ast_id decl,
                                          odin3_sym_id *sym) {
    odin3_scope_id scope = odin3_elab_scope_of(&e2->bind, decl);
    for (odin3_sym_id id = odin3_scope_first_symbol(e2->st, scope); id.v != 0;
         id = odin3_scope_next_symbol(e2->st, id)) {
        const odin3_symbol *rec = odin3_symbol_get(e2->st, id);
        if (rec->kind == ODIN3_SYM_VAR && rec->decl.v == decl.v) {
            *sym = id;
            return rec;
        }
    }
    return NULL;
}

odin3_status odin3_elab2_call_type(odin3_elab2 *e2, odin3_ast_id call, odin3_etype *out) {
    odin3_ast_id decl = {0};
    odin3_status st = odin3_elab2_callee(e2, call, &decl);
    odin3_sym_id id = {0};
    const odin3_symbol *res = st == ODIN3_OK ? odin3_elab2_fn_result(e2, decl, &id) : NULL;
    if (st != ODIN3_OK) {
        return st;
    }
    if (res == NULL) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, call), "'%s' has no result variable",
                          odin3_ast_name_str(e2->elab, call));
        return ODIN3_ERR_PARSE;
    }
    uint32_t width = res->has_packed ? (uint32_t)odin3_elab2_dim_size(&res->packed) : 1U;
    *out = (odin3_etype){width, (res->flags & ODIN3_SYM_F_SIGNED) != 0, false};
    return ODIN3_OK;
}

/* --- the interpreter's host ------------------------------------------------------------------- */

static bool param_unsized(const odin3_elab2 *e2, const odin3_symbol *rec) {
    const odin3_ast_number *num =
        odin3_ast_number_of(e2->elab, odin3_ast_child(e2->elab, rec->declarator, 0));
    return num != NULL && num->width == 0;
}

/* A parameter or localparam with a value: a CONST. */
static odin3_status host_const(odin3_elab2 *e2, const odin3_symbol *rec, odin3_csym *out) {
    if (!rec->has_value || rec->value.kind != ODIN3_VAL_BITS || rec->value.len == 0) {
        return ODIN3_OK;
    }
    bool unsized = param_unsized(e2, rec);
    const odin3_ast_literal lit = {rec->value, rec->value_signed, unsized};
    odin3_status st = odin3_cval_from_literal(e2->scratch, &lit, &out->value);
    out->kind = ODIN3_CSYM_CONST;
    out->type = (odin3_etype){rec->value.len, rec->value_signed, unsized};
    out->msb = (int64_t)rec->value.len - 1;
    out->lsb = 0;
    out->decl = rec->declarator;
    out->name = rec->name;
    return st;
}

static void host_signal(const odin3_symbol *rec, odin3_csym *out) {
    uint32_t width = rec->has_packed ? (uint32_t)odin3_elab2_dim_size(&rec->packed) : 1U;
    out->kind = ODIN3_CSYM_SIGNAL;
    out->type = (odin3_etype){width, (rec->flags & ODIN3_SYM_F_SIGNED) != 0, false};
    out->msb = rec->has_packed ? rec->packed.msb : 0;
    out->lsb = rec->has_packed ? rec->packed.lsb : 0;
    out->decl = rec->declarator;
    out->name = rec->name;
}

static odin3_status host_lookup(void *user, odin3_ast_id ref, odin3_csym *out) {
    odin3_elab2 *e2 = user;
    if (odin3_ast_kind_of(e2->elab, ref) == ODIN3_AST_SELECT) {
        odin3_elab2_wordref word;
        if (odin3_elab2_mem_word_of(e2, ref, &word)) {
            const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, word.mem);
            const odin3_symbol *rec = odin3_symbol_get(e2->st, mem->sym);
            host_signal(rec, out);
        }
        return ODIN3_OK;
    }
    const odin3_symbol *rec = call_sym(e2, ref);
    if (rec == NULL) {
        return ODIN3_OK;
    }
    if (rec->kind == ODIN3_SYM_PARAM || rec->kind == ODIN3_SYM_LOCALPARAM) {
        return host_const(e2, rec, out);
    }
    if (rec->kind == ODIN3_SYM_NET || rec->kind == ODIN3_SYM_VAR) {
        host_signal(rec, out);
    }
    return ODIN3_OK;
}

static odin3_status host_callee(void *user, odin3_ast_id call, odin3_csym *out) {
    const odin3_elab2 *e2 = user;
    const odin3_symbol *rec = call_sym(e2, call);
    if (rec != NULL && (rec->kind == ODIN3_SYM_FUNCTION || rec->kind == ODIN3_SYM_TASK)) {
        out->kind = rec->kind == ODIN3_SYM_FUNCTION ? ODIN3_CSYM_FUNCTION : ODIN3_CSYM_TASK;
        out->decl = rec->decl;
        out->name = rec->name;
    }
    return ODIN3_OK;
}

static void host_report(void *user, odin3_log_level level, odin3_loc loc, const char *msg) {
    odin3_elab2 *e2 = user;
    if (level == ODIN3_LOG_ERROR) {
        odin3_elab2_error(e2, loc, "%s", msg);
    } else if (level == ODIN3_LOG_WARN) {
        odin3_elab2_warn_msg(e2, loc, msg);
    }
}

odin3_status odin3_elab2_ceval_get(odin3_elab2 *e2, odin3_ceval **out) {
    if (e2->ev == NULL) {
        const odin3_ceval_host host = {host_lookup, host_callee, host_report, e2};
        odin3_status st = odin3_ceval_create(e2->elab, &host, &e2->ev);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    *out = e2->ev;
    return ODIN3_OK;
}

odin3_status odin3_elab2_call_try(odin3_elab2 *e2, odin3_ast_id call, odin3_cval *out,
                                  bool *is_const) {
    *is_const = false;
    odin3_ceval *ev = NULL;
    odin3_status st = odin3_elab2_ceval_get(e2, &ev);
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_ceval_req req = {call, {0, false, false}, "function call"};
    return odin3_ceval_try(ev, &req, e2->scratch, out, is_const);
}
