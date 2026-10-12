/*
 * elab1_params.c — parameters (2D spec §3.3, §4.5; EL-8, EL-16): a definition's `parameter`s
 * with their overrides applied in precedence order (project, named, defparam (the first wins),
 * ordered, default), each evaluated lazily in declaration order in the definition's environment
 * and converted to its declared type (odin3_cval_to_param), and the values a specialization is
 * keyed and named by (EL-9: whether each differs from its default).
 */
#include "frontends/verilog/elab1.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "frontends/elab/ceval.h"
#include "frontends/elab/cval.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint32_t E1_NONE = UINT32_MAX;

bool odin3_elab1_override_beats(const odin3_elab_override *lhs, const odin3_elab_override *rhs) {
    return lhs->source != rhs->source ? lhs->source < rhs->source : lhs->order < rhs->order;
}

/* The text of a defparam's target as written (for its warning and errors). */
static const char *target_text(odin3_elab1 *e1, const odin3_elab_override *ovr, odin3_strbuf *buf) {
    odin3_status st = odin3_elab1_override_text(e1, ovr, buf);
    if (st != ODIN3_OK) {
        e1->fatal = e1->fatal != ODIN3_OK ? e1->fatal : st;
        return "";
    }
    return buf->data != NULL ? buf->data : "";
}

/* The instance named is the component before the parameter. */
void odin3_elab1_override_lost(odin3_elab1 *e1, const odin3_elab_override *loser,
                               const odin3_elab_override *winner) {
    if (loser->source != ODIN3_OVR_DEFPARAM || winner->source != ODIN3_OVR_NAMED) {
        return;
    }
    odin3_ast_id hier = odin3_ast_child(e1->parsed, loser->from, 0);
    odin3_ast_span parts = odin3_ast_children(e1->parsed, hier);
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    const char *text = target_text(e1, loser, &buf);
    const char *inst = parts.n >= 2 ? odin3_ast_name_str(e1->parsed, parts.ids[parts.n - 2]) : "";
    odin3_elab1_warn_once(e1, hier,
                          "defparam of '%s' is overridden by the named parameter assignment of "
                          "'%s'",
                          text, inst);
    odin3_strbuf_free(&buf);
}

/* An override of a name that is not a `parameter` of the definition (§9.1). */
static void bad_name(odin3_elab1_env *env, const odin3_elab_override *ovr, bool localparam) {
    odin3_elab1 *e1 = env->e1;
    const char *module = odin3_elab1_str(e1, odin3_elab1_def(e1, env->def)->name);
    const char *name = odin3_elab1_str(e1, ovr->path);
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    if (ovr->source == ODIN3_OVR_PROJECT) {
        odin3_elab1_error(e1, ovr->at, "param '%s' does not name a parameter",
                          target_text(e1, ovr, &buf));
    } else if (localparam) {
        odin3_elab1_error(e1, ovr->at,
                          "'%s' is a localparam of module '%s' and cannot be overridden", name,
                          module);
    } else if (ovr->source == ODIN3_OVR_DEFPARAM) {
        odin3_elab1_error(e1, ovr->at,
                          "defparam target '%s' does not name a parameter of an instance below "
                          "this module",
                          target_text(e1, ovr, &buf));
    } else {
        odin3_elab1_error(e1, ovr->at, "module '%s' has no parameter '%s'", module, name);
    }
    odin3_strbuf_free(&buf);
}

/* One override of a parameter of the definition: kept when it takes precedence. */
static odin3_status apply(odin3_elab1_env *env, const odin3_elab_override *ovr) {
    uint32_t idx = odin3_elab1_env_find(env, 0, ovr->path);
    uint8_t kind = idx != E1_NONE ? odin3_elab1_env_slot(env, idx)->entry.kind : 0;
    if (kind != ODIN3_ENTRY_PARAM) {
        bad_name(env, ovr, kind == ODIN3_ENTRY_LOCALPARAM);
        return ODIN3_ERR_PARSE;
    }
    odin3_elab1_slot *slot = odin3_elab1_env_slot(env, idx);
    if (!slot->overridden) {
        slot->overridden = true;
        slot->ovr = *ovr;
    } else if (odin3_elab1_override_beats(ovr, &slot->ovr)) {
        odin3_elab1_override_lost(env->e1, &slot->ovr, ovr);
        slot->ovr = *ovr;
    } else {
        odin3_elab1_override_lost(env->e1, ovr, &slot->ovr);
    }
    return ODIN3_OK;
}

/* The deeper overrides go to pending; the others are applied (every error reported). */
static odin3_status apply_all(odin3_elab1_env *env, const odin3_elab_spec_req *req,
                              odin3_elab_override *pending, uint32_t *n_pending) {
    odin3_status worst = ODIN3_OK;
    for (uint32_t i = 0; i < req->n_ovr; i++) {
        const odin3_elab_override *ovr = &req->ovr[i];
        if (strchr(odin3_elab1_str(env->e1, ovr->path), ODIN3_ELAB_PATH_SEP) != NULL) {
            pending[(*n_pending)++] = *ovr;
            continue;
        }
        odin3_status st = apply(env, ovr);
        worst = worst != ODIN3_OK ? worst : st;
    }
    return worst;
}

/* Whether parameter slot idx's final value differs from what its default would give in this
 * specialization (EL-9's name), judged quietly into *out: a default that cannot be evaluated
 * differs. ODIN3_ERR_NO_MEMORY from either evaluation is returned (EL-46): the restoring one
 * failing would leave the default's value in the slot. */
static odin3_status differs(odin3_elab1_env *env, uint32_t idx, bool *out) {
    odin3_elab1_slot *slot = odin3_elab1_env_slot(env, idx);
    *out = false;
    if (!slot->overridden) {
        return ODIN3_OK;
    }
    odin3_elab_override saved = slot->ovr;
    odin3_cval final = slot->entry.value;
    slot->overridden = false;
    slot->state = ODIN3_E1_NEW;
    env->quiet = true;
    odin3_status st = odin3_elab1_env_value(env, idx);
    env->quiet = false;
    if (st == ODIN3_ERR_NO_MEMORY) {
        return st;
    }
    slot = odin3_elab1_env_slot(env, idx);
    bool same = st == ODIN3_OK && odin3_elab1_same_value(&slot->entry.value, &final);
    slot->overridden = true;
    slot->ovr = saved;
    slot->state = ODIN3_E1_NEW;
    st = odin3_elab1_env_value(env, idx); /* restores the final value (no diagnostic: it worked) */
    if (st == ODIN3_ERR_NO_MEMORY) {
        return st;
    }
    *out = !same || st != ODIN3_OK;
    return ODIN3_OK;
}

static odin3_status copy_out(odin3_elab1_env *env, uint32_t idx, odin3_elab1_params_out *out,
                             uint32_t pos) {
    const odin3_elab1_slot *slot = odin3_elab1_env_slot(env, idx);
    out->bases[pos] = slot->base;
    bool differ = false;
    odin3_status st = differs(env, idx, &differ);
    if (st != ODIN3_OK) {
        return st;
    }
    out->differ[pos] = differ;
    slot = odin3_elab1_env_slot(env, idx);
    return odin3_cval_copy(env->e1->scratch, &slot->entry.value, &out->values[pos]);
}

/* The environment slot of the definition's k-th `parameter`; E1_NONE when it has none. */
static uint32_t param_slot(odin3_elab1_env *env, uint32_t pos) {
    odin3_elab1 *e1 = env->e1;
    const odin3_elab_def *def = odin3_elab1_def(e1, env->def);
    odin3_ast_id decl = *(const odin3_ast_id *)odin3_vec_cat(&e1->params, def->first_param + pos);
    return odin3_elab1_env_find(env, 0, odin3_ast_name(e1->parsed, decl));
}

/* Each `parameter` evaluated in declaration order (lazily: one may read a later one), then each
 * copied out with its base and whether it differs from its default (once all are known, so the
 * quiet default evaluations read only finished values). */
static odin3_status evaluate(odin3_elab1_env *env, odin3_elab1_params_out *out) {
    const odin3_elab_def *def = odin3_elab1_def(env->e1, env->def);
    uint32_t count = def->n_header_params + def->n_body_params;
    odin3_status worst = ODIN3_OK;
    for (uint32_t k = 0; k < count && worst != ODIN3_ERR_NO_MEMORY && !env->e1->stopped; k++) {
        uint32_t idx = param_slot(env, k);
        odin3_status st = idx != E1_NONE ? odin3_elab1_env_value(env, idx) : ODIN3_ERR_CHECK;
        worst = worst != ODIN3_OK && st != ODIN3_ERR_NO_MEMORY ? worst : st; /* NO_MEMORY wins */
    }
    for (uint32_t k = 0; k < count && worst == ODIN3_OK; k++) {
        worst = copy_out(env, param_slot(env, k), out, k);
    }
    return worst;
}

odin3_status odin3_elab1_params_ex(odin3_elab1 *e1, const odin3_elab_spec_req *req,
                                   odin3_elab1_params_out *out) {
    const odin3_elab_def *def = odin3_elab1_def(e1, req->def);
    uint32_t count = def->n_header_params + def->n_body_params;
    odin3_arena *fresh = odin3_arena_create(0);
    if (fresh == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_arena_destroy(e1->scratch);
    e1->scratch = fresh;
    size_t room = (size_t)(count > 0 ? count : 1);
    out->values = odin3_arena_alloc(fresh, room * sizeof *out->values);
    out->bases = odin3_arena_alloc(fresh, room);
    out->differ = odin3_arena_alloc(fresh, room * sizeof *out->differ);
    out->pending =
        odin3_arena_alloc(fresh, (size_t)(req->n_ovr > 0 ? req->n_ovr : 1) * sizeof *out->pending);
    out->n_pending = 0;
    if (out->values == NULL || out->bases == NULL || out->differ == NULL || out->pending == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_elab1_env *env = NULL;
    odin3_status st = odin3_elab1_env_create(e1, req->def, &env);
    st = st != ODIN3_OK ? st : apply_all(env, req, out->pending, &out->n_pending);
    odin3_status ev = st == ODIN3_ERR_NO_MEMORY ? st : evaluate(env, out);
    odin3_elab1_env_destroy(env);
    return st != ODIN3_OK ? st : ev;
}

odin3_status odin3_elab1_params(odin3_elab1 *e1, const odin3_elab_spec_req *req, odin3_cval *values,
                                odin3_elab_override *pending, uint32_t *n_pending) {
    odin3_elab1_params_out out;
    odin3_status st = odin3_elab1_params_ex(e1, req, &out);
    const odin3_elab_def *def = odin3_elab1_def(e1, req->def);
    uint32_t count = def->n_header_params + def->n_body_params;
    if (st != ODIN3_OK) {
        return st;
    }
    for (uint32_t k = 0; k < count; k++) {
        values[k] = out.values[k];
    }
    for (uint32_t i = 0; i < out.n_pending; i++) {
        pending[i] = out.pending[i];
    }
    *n_pending = out.n_pending;
    return ODIN3_OK;
}
