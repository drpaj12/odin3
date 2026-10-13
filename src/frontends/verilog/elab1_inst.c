/*
 * elab1_inst.c — instances in stage 1 (2D spec §3.5, §3.6; EL-10, EL-11, EL-8's cell-type
 * targets; plan Task 12): connections resolved to ports (ordered and named, the unnamed header
 * positions, the output rules), instance arrays planned (whole or sliced connections), the
 * parameters of cell-type and implicit black-box instances, the black-box table (`(* blackbox *)`
 * specializations and implicit black boxes), the instance cap, and the typer that gives
 * connection expressions their widths over pass E's environment.
 */
#include "frontends/verilog/elab1.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "frontends/elab/ceval.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    E1_SLOT_PORTS = 1,
    E1_ITEMS = 2,
    E1_KEY_SHIFT = 32,
    E1_LV_NET = 0,       /* a net lvalue (I3) */
    E1_LV_CONST = 1,     /* a constant part */
    E1_LV_OTHER = 2,     /* anything else */
    TY_INTEGER_BITS = 32 /* an `integer` function result */
};

static const uint32_t E1_NONE = UINT32_MAX;

static const char *str_of(const odin3_elab1 *e1, uint32_t id) {
    return odin3_elab1_str(e1, id);
}

/* The connections of an INSTANCE (after its array slot). */
static odin3_ast_span inst_conns(const odin3_elab1 *e1, odin3_ast_id instance) {
    odin3_ast_span kids = odin3_ast_children(e1->parsed, instance);
    uint32_t from = odin3_ast_slot_count(ODIN3_AST_INSTANCE);
    return kids.n > from ? (odin3_ast_span){kids.ids + from, kids.n - from}
                         : (odin3_ast_span){NULL, 0};
}

static void latch(odin3_elab1 *e1, odin3_status st) {
    if (st != ODIN3_OK && st != ODIN3_ERR_PARSE && e1->fatal == ODIN3_OK) {
        e1->fatal = st;
    }
}

/* --- the connection typer (a stage-1 odin3_ctype_host over pass E's environment) ------------ */

static odin3_status ty_error(odin3_elab1_env *env, odin3_ast_id node, const char *text) {
    if (!env->quiet) {
        odin3_elab1_error(env->e1, odin3_ast_loc(env->e1->parsed, node), "'%s' is not declared",
                          text);
    }
    return ODIN3_ERR_PARSE;
}

static odin3_status ty_slot(odin3_elab1_env *env, odin3_ast_id node, uint32_t idx,
                            odin3_etype *out);

static odin3_status ty_ident(odin3_elab1_env *env, odin3_ast_id node, odin3_etype *out) {
    const odin3_ast *ast = env->e1->parsed;
    uint32_t idx = odin3_elab1_env_lookup(env, odin3_ast_name(ast, node));
    if (idx == E1_NONE) {
        *out = (odin3_etype){1, false, false}; /* an implicit net (EL-25; Task 15 declares it) */
        return ODIN3_OK;
    }
    return ty_slot(env, node, idx, out);
}

/* The type of slot idx, named at node. */
static odin3_status ty_slot(odin3_elab1_env *env, odin3_ast_id node, uint32_t idx,
                            odin3_etype *out) {
    const odin3_ast *ast = env->e1->parsed;
    uint8_t kind = odin3_elab1_env_slot(env, idx)->entry.kind;
    if (kind != ODIN3_ENTRY_PARAM && kind != ODIN3_ENTRY_LOCALPARAM && kind != ODIN3_ENTRY_SIGNAL &&
        kind != ODIN3_ENTRY_GENVAR) {
        return ty_error(env, node, odin3_ast_name_str(ast, node));
    }
    odin3_status st = odin3_elab1_env_value(env, idx);
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_elab1_slot *slot = odin3_elab1_env_slot(env, idx);
    if (kind == ODIN3_ENTRY_PARAM || kind == ODIN3_ENTRY_LOCALPARAM) {
        const odin3_cval *val = &slot->entry.value;
        *out = (odin3_etype){val->width, (val->flags & ODIN3_CVAL_SIGNED) != 0,
                             (val->flags & ODIN3_CVAL_UNSIZED) != 0};
    } else {
        *out = (odin3_etype){slot->entry.width, slot->entry.is_signed, false};
    }
    return ODIN3_OK;
}

/* A SELECT is a memory word when its chain is as deep as its name's unpacked dimensions. */
static odin3_status ty_word(odin3_elab1_env *env, odin3_ast_id sel, odin3_etype *out) {
    const odin3_ast *ast = env->e1->parsed;
    uint32_t depth = 0;
    odin3_ast_id obj = sel;
    while (odin3_ast_kind_of(ast, obj) == ODIN3_AST_SELECT) {
        obj = odin3_ast_child(ast, obj, 0);
        depth++;
    }
    uint32_t idx = odin3_ast_kind_of(ast, obj) == ODIN3_AST_IDENT
                       ? odin3_elab1_env_lookup(env, odin3_ast_name(ast, obj))
                       : E1_NONE;
    if (idx == E1_NONE || odin3_elab1_env_slot(env, idx)->entry.kind != ODIN3_ENTRY_SIGNAL ||
        odin3_elab1_env_slot(env, idx)->n_unpacked != depth) {
        return ODIN3_OK; /* an ordinary bit or part select */
    }
    odin3_status st = odin3_elab1_env_value(env, idx);
    if (st == ODIN3_OK) {
        const odin3_elab1_slot *slot = odin3_elab1_env_slot(env, idx);
        *out = (odin3_etype){slot->entry.width, slot->entry.is_signed, false};
    }
    return st;
}

/* A user function's call is typed at its declared result (EL-17, §17 #17): its range folded where
 * the function is declared, `integer` as 32 signed bits, else one bit. */
static odin3_status ty_function(odin3_elab1_env *env, uint32_t idx, odin3_etype *out) {
    const odin3_ast *ast = env->e1->parsed;
    odin3_ast_id decl = odin3_elab1_env_slot(env, idx)->entry.decl;
    uint32_t scope = odin3_elab1_env_slot(env, idx)->scope;
    uint32_t flags = odin3_ast_flags(ast, decl);
    uint32_t dt = (flags & (uint32_t)ODIN3_AST_F_FUNCTION_DECL_DT_MASK) >>
                  (uint32_t)ODIN3_AST_F_FUNCTION_DECL_DT_SHIFT;
    *out = (odin3_etype){1, (flags & ODIN3_AST_F_FUNCTION_DECL_SIGNED) != 0, false};
    if (dt == ODIN3_AST_DT_INTEGER) {
        *out = (odin3_etype){TY_INTEGER_BITS, true, false};
        return ODIN3_OK;
    }
    odin3_ast_id range = odin3_ast_child(ast, decl, 0);
    if (range.v == 0) {
        return ODIN3_OK;
    }
    int64_t msb = 0;
    int64_t lsb = 0;
    uint32_t saved = env->cur_scope;
    env->cur_scope = scope;
    odin3_status st = odin3_elab1_env_range(env, range, &msb, &lsb);
    env->cur_scope = saved;
    if (st == ODIN3_OK) {
        out->width = (uint32_t)(msb >= lsb ? msb - lsb : lsb - msb) + 1U;
    }
    return st;
}

/* A call: a user function at its declared width; a system function or an unknown name is an
 * error. */
static odin3_status ty_call(odin3_elab1_env *env, odin3_ast_id call, odin3_etype *out) {
    odin3_elab1 *e1 = env->e1;
    if ((odin3_ast_flags(e1->parsed, call) & ODIN3_AST_F_CALL_SYSTEM) != 0) {
        if (!env->quiet) {
            (void)odin3_elab1_reject(e1, call); /* E20, E21 */
        }
        return ODIN3_ERR_PARSE;
    }
    uint32_t idx = odin3_elab1_env_lookup(env, odin3_ast_name(e1->parsed, call));
    if (idx == E1_NONE || odin3_elab1_env_slot(env, idx)->entry.kind != ODIN3_ENTRY_FUNCTION) {
        return ty_error(env, call, odin3_ast_name_str(e1->parsed, call));
    }
    return ty_function(env, idx, out);
}

/* The declaration of `name` among the locals of a named begin/end block of a process (a VAR_DECL
 * or NET_DECL declarator); the declaration node, or 0. */
static odin3_ast_id ty_block_decl(const odin3_ast *ast, odin3_ast_id block, uint32_t name) {
    odin3_ast_span locals = odin3_ast_children(ast, odin3_ast_child(ast, block, 0));
    for (uint32_t i = 0; i < locals.n; i++) {
        odin3_ast_kind kind = odin3_ast_kind_of(ast, locals.ids[i]);
        if (kind != ODIN3_AST_VAR_DECL && kind != ODIN3_AST_NET_DECL) {
            continue;
        }
        odin3_ast_span kids = odin3_ast_children(ast, locals.ids[i]);
        for (uint32_t k = odin3_ast_slot_count(kind); k < kids.n; k++) {
            if (odin3_ast_name(ast, kids.ids[k]) == name) {
                return locals.ids[i];
            }
        }
    }
    return (odin3_ast_id){0};
}

/* The type of `blk.count`, a variable or net declared in a named begin/end block of an always or
 * initial (I-1): the binder finds the symbol, the connection typer needs its width and sign. */
static odin3_status ty_proc_member(odin3_elab1_env *env, odin3_ast_id hier, odin3_ast_id block,
                                   odin3_etype *out) {
    const odin3_ast *ast = env->e1->parsed;
    odin3_ast_id last = odin3_ast_child(ast, hier, odin3_ast_nchild(ast, hier) - 1U);
    odin3_ast_id decl = odin3_ast_nchild(ast, hier) == 2
                            ? ty_block_decl(ast, block, odin3_ast_name(ast, last))
                            : (odin3_ast_id){0};
    if (decl.v == 0) {
        return ty_error(env, hier, odin3_ast_name_str(ast, last));
    }
    bool net = odin3_ast_kind_of(ast, decl) == ODIN3_AST_NET_DECL;
    bool integer = !net && odin3_ast_sub(ast, decl) == ODIN3_AST_VAR_DECL_INTEGER;
    odin3_ast_id range = odin3_ast_child(ast, decl, net ? 1 : 0);
    bool is_signed = integer || (odin3_ast_flags(ast, decl) & ODIN3_AST_F_VAR_DECL_SIGNED) != 0;
    *out = (odin3_etype){integer ? TY_INTEGER_BITS : 1, is_signed, false};
    int64_t msb = 0;
    int64_t lsb = 0;
    odin3_status st =
        !integer && range.v != 0 ? odin3_elab1_env_range(env, range, &msb, &lsb) : ODIN3_OK;
    if (st == ODIN3_OK && !integer && range.v != 0) {
        out->width = (uint32_t)(msb >= lsb ? msb - lsb : lsb - msb) + 1U;
    }
    return st;
}

static odin3_status ty_leaf(void *user, odin3_ast_id node, odin3_etype *out) {
    odin3_elab1_env *env = user;
    switch (odin3_ast_kind_of(env->e1->parsed, node)) {
    case ODIN3_AST_SELECT:
        return ty_word(env, node, out);
    case ODIN3_AST_IDENT:
        return ty_ident(env, node, out);
    case ODIN3_AST_CALL:
        return ty_call(env, node, out);
    default: { /* a HIER_NAME into the generate blocks (G7) or a named block of a process (I-1) */
        uint32_t idx = 0;
        odin3_ast_id block = {0};
        odin3_status st = odin3_elab1_scope_proc(env, node, &block, &idx);
        if (st == ODIN3_OK && block.v != 0) {
            return ty_proc_member(env, node, block, out);
        }
        return st != ODIN3_OK ? st : ty_slot(env, node, idx, out);
    }
    }
}

static odin3_status ty_constant(void *user, odin3_ast_id node, int64_t *out) {
    odin3_elab1_env *env = user;
    const char *what = odin3_ctype_constant_what(env->types);
    int32_t value = 0;
    odin3_status st = odin3_elab1_fold_int(env, node, what != NULL ? what : "range bound", &value);
    *out = value;
    return st;
}

static void ty_report(void *user, odin3_log_level level, odin3_loc loc, const char *msg) {
    const odin3_elab1_env *env = user;
    if (!env->quiet) {
        odin3_elab1_report(env->e1, level, loc, msg);
    }
}

static odin3_status env_types(odin3_elab1_env *env) {
    if (env->types != NULL) {
        return ODIN3_OK;
    }
    const odin3_ctype_host host = {ty_leaf, ty_constant, ty_report, env};
    return odin3_ctype_create(env->e1->parsed, &host, &env->types);
}

odin3_status odin3_elab1_expr_width(odin3_elab1 *e1, odin3_ast_id expr, uint32_t *width) {
    *width = 0;
    if (e1->env == NULL) {
        return ODIN3_ERR_CHECK; /* outside passes E and W: a 2D bug */
    }
    odin3_status st = env_types(e1->env);
    st = st != ODIN3_OK ? st : odin3_ctype_expr(e1->env->types, expr, (odin3_etype){0});
    if (st == ODIN3_OK) {
        *width = odin3_ctype_self(e1->env->types, expr).width;
    }
    return st;
}

uint32_t odin3_elab1_typed_width(const odin3_elab1 *e1, odin3_ast_id node) {
    if (e1->env == NULL || e1->env->types == NULL) {
        return 0;
    }
    return odin3_ctype_self(e1->env->types, node).width;
}

/* --- the instance cap, array ranges, decision values ----------------------------------------- */

bool odin3_elab1_count_instances(odin3_elab1 *e1, odin3_ast_id holder, uint64_t n) {
    if (e1->n_instances + n <= e1->limits->max_instances) {
        e1->n_instances += n;
        return true;
    }
    odin3_elab1_fatal(e1, odin3_ast_loc(e1->parsed, holder),
                      "elaboration makes more than %" PRIu32 " instances",
                      e1->limits->max_instances);
    return false;
}

odin3_status odin3_elab1_fold_array(odin3_elab1 *e1, odin3_ast_id instance,
                                    odin3_elab1_array *out) {
    *out = (odin3_elab1_array){0, 0, 1};
    odin3_ast_id range = odin3_ast_child(e1->parsed, instance, 0);
    if (range.v == 0) {
        return ODIN3_OK;
    }
    if (e1->env == NULL) {
        return ODIN3_ERR_CHECK;
    }
    int32_t left = 0;
    int32_t right = 0;
    odin3_status st =
        odin3_elab1_fold_int(e1->env, odin3_ast_child(e1->parsed, range, 0), "range bound", &left);
    st = st != ODIN3_OK ? st
                        : odin3_elab1_fold_int(e1->env, odin3_ast_child(e1->parsed, range, 1),
                                               "range bound", &right);
    if (st != ODIN3_OK) {
        return st;
    }
    uint64_t span =
        left >= right ? (uint64_t)((int64_t)left - right) : (uint64_t)((int64_t)right - left);
    *out = (odin3_elab1_array){left, right, span >= UINT32_MAX ? UINT32_MAX : (uint32_t)span + 1};
    return ODIN3_OK;
}

int32_t odin3_elab1_given_decision(uint32_t idx) {
    return (int32_t)ODIN3_DECISION_GIVEN_BASE - (int32_t)idx;
}

uint32_t odin3_elab1_given_of(int32_t decision) {
    return (uint32_t)((int32_t)ODIN3_DECISION_GIVEN_BASE - decision);
}

/* --- a definition's ports: names by position (port_pos) and directions --------------------- */

/* The direction a non-ANSI port gets from the body's PORT_DECLs (name → dir + 1). */
static odin3_status body_dirs(const odin3_elab1 *e1, odin3_ast_id module, odin3_u64map *dirs) {
    odin3_ast_span kids = odin3_ast_children(e1->parsed, module);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = E1_ITEMS; st == ODIN3_OK && i < kids.n; i++) {
        if (odin3_ast_kind_of(e1->parsed, kids.ids[i]) != ODIN3_AST_PORT_DECL) {
            continue;
        }
        odin3_ast_span decls = odin3_ast_children(e1->parsed, kids.ids[i]);
        uint64_t dir = (uint64_t)odin3_ast_sub(e1->parsed, kids.ids[i]) + 1;
        for (uint32_t j = odin3_ast_slot_count(ODIN3_AST_PORT_DECL); st == ODIN3_OK && j < decls.n;
             j++) {
            uint32_t name = odin3_ast_name(e1->parsed, decls.ids[j]);
            st = odin3_u64map_get(dirs, name, NULL) ? ODIN3_OK
                                                    : odin3_u64map_put(dirs, (odin3_kv){name, dir});
        }
    }
    return st;
}

/* The direction of each position, walking the ports LIST as collect_defs did: an ANSI
 * PORT_DECL's for its declarators, the body's for a non-ANSI name (input when undeclared: Task 15
 * reports it). */
static odin3_status fill_dirs(const odin3_elab1 *e1, const odin3_elab_def *rec, uint8_t *dirs) {
    odin3_ast_id list = odin3_ast_child(e1->parsed, rec->node, E1_SLOT_PORTS);
    odin3_ast_span entries = odin3_ast_children(e1->parsed, list);
    odin3_u64map *body = odin3_u64map_create(0);
    odin3_status st = body != NULL ? body_dirs(e1, rec->node, body) : ODIN3_ERR_NO_MEMORY;
    uint32_t k = 0;
    for (uint32_t i = 0; st == ODIN3_OK && list.v != 0 && i < entries.n; i++) {
        odin3_ast_id entry = entries.ids[i];
        if (odin3_ast_kind_of(e1->parsed, entry) == ODIN3_AST_PORT_DECL) {
            uint32_t n_decls =
                odin3_ast_nchild(e1->parsed, entry) - odin3_ast_slot_count(ODIN3_AST_PORT_DECL);
            for (uint32_t j = 0; j < n_decls && k < rec->n_positions; j++) {
                dirs[k++] = (uint8_t)odin3_ast_sub(e1->parsed, entry);
            }
            continue;
        }
        uint64_t dir = 0;
        bool known = odin3_u64map_get(body, odin3_ast_name(e1->parsed, entry), &dir);
        if (k < rec->n_positions) {
            dirs[k++] = known ? (uint8_t)(dir - 1) : (uint8_t)ODIN3_DIR_IN;
        }
    }
    odin3_u64map_destroy(body);
    return st;
}

/* Definition def's port names in odin3_elab1.port_pos and directions in its defx, once. */
static odin3_status index_def_ports(odin3_elab1 *e1, uint32_t def) {
    odin3_elab1_defx *ext = odin3_vec_at(&e1->defx, def);
    if (ext->ports_indexed) {
        return ODIN3_OK;
    }
    const odin3_elab_def *rec = odin3_elab1_def(e1, def);
    uint8_t *dirs = odin3_arena_alloc(e1->arena, rec->n_positions > 0 ? rec->n_positions : 1);
    odin3_status st = dirs != NULL ? fill_dirs(e1, rec, dirs) : ODIN3_ERR_NO_MEMORY;
    for (uint32_t k = 0; st == ODIN3_OK && k < rec->n_positions; k++) {
        odin3_ast_id port = *(const odin3_ast_id *)odin3_vec_cat(&e1->ports, rec->first_port + k);
        uint32_t name = odin3_ast_name(e1->parsed, port);
        uint64_t key = (uint64_t)def << E1_KEY_SHIFT | name;
        st = name != 0 && !odin3_u64map_get(e1->port_pos, key, NULL)
                 ? odin3_u64map_put(e1->port_pos, (odin3_kv){key, (uint64_t)k + 1})
                 : ODIN3_OK;
    }
    if (st == ODIN3_OK) {
        ext = odin3_vec_at(&e1->defx, def);
        ext->port_dirs = dirs;
        ext->ports_indexed = true;
    }
    return st;
}

/* --- the ports of a specialization ------------------------------------------------------------ */

/* Port k of the definition env belongs to, evaluated in env (its range folded). */
static odin3_status eval_port(odin3_elab1_env *env, uint32_t pos, odin3_elab1_port *out) {
    odin3_elab1 *e1 = env->e1;
    const odin3_elab_def *rec = odin3_elab1_def(e1, env->def);
    const odin3_elab1_defx *ext = odin3_vec_cat(&e1->defx, env->def);
    odin3_ast_id port = *(const odin3_ast_id *)odin3_vec_cat(&e1->ports, rec->first_port + pos);
    *out = (odin3_elab1_port){
        .name = odin3_ast_name(e1->parsed, port), .dir = ext->port_dirs[pos], .decl = port};
    uint32_t idx = out->name != 0 ? odin3_elab1_env_find(env, 0, out->name) : E1_NONE;
    if (idx == E1_NONE || odin3_elab1_env_slot(env, idx)->entry.kind != ODIN3_ENTRY_SIGNAL) {
        return ODIN3_OK; /* an empty position, or a port Task 15 reports undeclared */
    }
    odin3_status st = odin3_elab1_env_value(env, idx);
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_elab1_slot *slot = odin3_elab1_env_slot(env, idx);
    out->width = slot->entry.width;
    out->msb = slot->msb;
    out->lsb = slot->lsb;
    out->is_signed = slot->entry.is_signed;
    out->scalar = slot->entry.width == 1 && slot->msb == 0 && slot->lsb == 0;
    return ODIN3_OK;
}

odin3_status odin3_elab1_eval_ports(odin3_elab1 *e1, uint32_t spec, bool quiet,
                                    odin3_elab1_port *ports) {
    uint32_t def = odin3_elab1_spec_at(e1, spec)->def;
    odin3_status st = index_def_ports(e1, def);
    odin3_elab1_env *env = NULL;
    st = st != ODIN3_OK ? st : odin3_elab1_env_create(e1, def, &env);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_elab1_env_preload(env, spec);
    env->quiet = quiet;
    odin3_status worst = ODIN3_OK;
    uint32_t count = odin3_elab1_def(e1, def)->n_positions;
    for (uint32_t k = 0; k < count && worst != ODIN3_ERR_NO_MEMORY; k++) {
        odin3_status one = eval_port(env, k, &ports[k]);
        worst = worst != ODIN3_OK ? worst : one;
    }
    odin3_elab1_env_destroy(env);
    return worst;
}

const odin3_elab1_port *odin3_elab1_spec_ports(odin3_elab1 *e1, uint32_t spec, uint32_t *n) {
    odin3_elab1_spec *rec = odin3_vec_at(&e1->specs, spec);
    *n = 0;
    if (!rec->ports_done) {
        uint32_t count = odin3_elab1_def(e1, rec->def)->n_positions;
        odin3_elab1_port *ports =
            odin3_arena_alloc(e1->arena, (size_t)(count > 0 ? count : 1) * sizeof *ports);
        odin3_status st =
            ports != NULL ? odin3_elab1_eval_ports(e1, spec, true, ports) : ODIN3_ERR_NO_MEMORY;
        if (st == ODIN3_ERR_NO_MEMORY || ports == NULL) {
            latch(e1, ODIN3_ERR_NO_MEMORY);
            return NULL;
        }
        rec = odin3_vec_at(&e1->specs, spec);
        rec->ports = ports;
        rec->n_ports = count;
        rec->ports_done = true;
    }
    *n = rec->n_ports;
    return rec->ports;
}

/* --- connections (EL-10) ------------------------------------------------------------------- */

/* What an instance connects to: a definition's positions or a cell type's ports. */
typedef struct e1_target {
    const char *kind_text; /* "module" or "cell type" (the no-port error) */
    const char *name;
    uint32_t def;                   /* a definition target */
    const odin3_celltype_def *type; /* a cell-type target, else NULL */
    uint32_t n;                     /* positions or ports */
} e1_target;

/* Position pos's port name (0: an empty position, or a type port no strtab ID names). */
static uint32_t tg_name(const odin3_elab1 *e1, const e1_target *tg, uint32_t pos) {
    if (tg->type != NULL) {
        uint32_t id = 0;
        return odin3_strtab_find(odin3_design_strtab(e1->design),
                                 odin3_bytes_cstr(tg->type->ports[pos].name), &id)
                   ? id
                   : 0;
    }
    const odin3_elab_def *rec = odin3_elab1_def(e1, tg->def);
    return odin3_ast_name(e1->parsed,
                          *(const odin3_ast_id *)odin3_vec_cat(&e1->ports, rec->first_port + pos));
}

static uint8_t tg_dir(const odin3_elab1 *e1, const e1_target *tg, uint32_t pos) {
    if (tg->type != NULL) {
        return (uint8_t)tg->type->ports[pos].dir;
    }
    return ((const odin3_elab1_defx *)odin3_vec_cat(&e1->defx, tg->def))->port_dirs[pos];
}

/* The position of the port named name, E1_NONE when none. */
static uint32_t tg_find(const odin3_elab1 *e1, const e1_target *tg, uint32_t name) {
    if (tg->type == NULL) {
        uint64_t at = 0;
        return odin3_u64map_get(e1->port_pos, (uint64_t)tg->def << E1_KEY_SHIFT | name, &at)
                   ? (uint32_t)at - 1
                   : E1_NONE;
    }
    for (uint32_t k = 0; k < tg->n; k++) {
        if (strcmp(tg->type->ports[k].name, str_of(e1, name)) == 0) {
            return k;
        }
    }
    return E1_NONE;
}

/* The instance's name for messages. */
static const char *inst_name(const odin3_elab1 *e1, const odin3_elab_inst *inst) {
    return odin3_ast_name_str(e1->parsed, inst->instance);
}

static odin3_status push_conn(odin3_elab1 *e1, const odin3_elab1_conn *conn) {
    odin3_elab1_conn *slot = odin3_vec_push(&e1->conns);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *conn;
    return ODIN3_OK;
}

/* I2: every connection ordered or every one named; *named is the form. */
static odin3_status conn_form(odin3_elab1 *e1, const odin3_elab_inst *inst, bool *named) {
    odin3_ast_span conns = inst_conns(e1, inst->instance);
    bool any_named = false;
    bool any_ordered = false;
    for (uint32_t i = 0; i < conns.n; i++) {
        bool is_named = odin3_ast_name(e1->parsed, conns.ids[i]) != 0;
        any_named = any_named || is_named;
        any_ordered = any_ordered || !is_named;
    }
    *named = any_named;
    if (any_named && any_ordered) {
        odin3_elab1_error(e1, odin3_ast_loc(e1->parsed, inst->instance),
                          "instance '%s' mixes ordered and named connections", inst_name(e1, inst));
        return ODIN3_ERR_PARSE;
    }
    return ODIN3_OK;
}

/* One instance's resolution: the instance, its target, whether any connection failed. */
typedef struct e1_connect {
    odin3_elab1 *e1;
    const odin3_elab_inst *inst;
    const e1_target *tg;
    bool bad;
} e1_connect;

/* Ordered connection idx goes to position idx; one with an expression to an empty header
 * position (a definition's) is dropped with the warning, once. False when it is dropped. */
static bool ordered_kept(const e1_connect *cx, odin3_ast_id conn, uint32_t idx) {
    odin3_elab1 *e1 = cx->e1;
    if (cx->tg->type != NULL || tg_name(e1, cx->tg, idx) != 0) {
        return true;
    }
    if (odin3_ast_child(e1->parsed, conn, 0).v != 0) {
        odin3_elab1_warn_once(e1, conn,
                              "connection %" PRIu32
                              " of instance '%s' goes to an unnamed port position and is ignored",
                              idx + 1, inst_name(e1, cx->inst));
    }
    return false;
}

/* The key of a named connection of the instance in odin3_elab1.conn_seen. */
static uint64_t seen_key(const e1_connect *cx, uint32_t name) {
    return (uint64_t)cx->inst->instance.v << E1_KEY_SHIFT | name;
}

/* A named connection's position: an error for an unknown port or a second connection. */
static uint32_t named_position(e1_connect *cx, odin3_ast_id conn) {
    odin3_elab1 *e1 = cx->e1;
    uint32_t name = odin3_ast_name(e1->parsed, conn);
    odin3_loc at = odin3_ast_loc(e1->parsed, conn);
    uint32_t pos = tg_find(e1, cx->tg, name);
    if (pos == E1_NONE) {
        odin3_elab1_error(e1, at, "%s '%s' has no port '%s'", cx->tg->kind_text, cx->tg->name,
                          str_of(e1, name));
        cx->bad = true;
        return E1_NONE;
    }
    if (odin3_u64map_get(e1->conn_seen, seen_key(cx, name), NULL)) {
        odin3_elab1_error(e1, at, "port '%s' of instance '%s' is connected twice", str_of(e1, name),
                          inst_name(e1, cx->inst));
        cx->bad = true;
        return E1_NONE;
    }
    odin3_status st = odin3_u64map_put(e1->conn_seen, (odin3_kv){seen_key(cx, name), 1});
    if (st != ODIN3_OK) {
        latch(e1, st);
        cx->bad = true;
        return E1_NONE;
    }
    return pos;
}

/* Every connection of the instance resolved, in written order, into odin3_elab1.conns. */
static odin3_status resolve_all(e1_connect *cx, bool named) {
    odin3_elab1 *e1 = cx->e1;
    odin3_ast_span conns = inst_conns(e1, cx->inst->instance);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && e1->fatal == ODIN3_OK && i < conns.n; i++) {
        odin3_ast_id conn = conns.ids[i];
        uint32_t pos = i;
        if (named) {
            pos = named_position(cx, conn);
        } else if (!ordered_kept(cx, conn, i)) {
            pos = E1_NONE;
        }
        if (pos == E1_NONE) {
            continue;
        }
        uint32_t name = named ? odin3_ast_name(e1->parsed, conn) : tg_name(e1, cx->tg, pos);
        odin3_elab1_conn rec = {.name = name,
                                .dir = tg_dir(e1, cx->tg, pos),
                                .pos = pos,
                                .conn = conn,
                                .expr = odin3_ast_child(e1->parsed, conn, 0)};
        st = push_conn(e1, &rec);
    }
    for (uint32_t i = 0; named && i < conns.n; i++) {
        (void)odin3_u64map_remove(e1->conn_seen,
                                  seen_key(cx, odin3_ast_name(e1->parsed, conns.ids[i])));
    }
    return st != ODIN3_OK ? st : e1->fatal;
}

/* --- I3: what an output or inout may be connected to ------------------------------------------ */

/* True when expr reads a name pass E's environment does not have (an implicit net, Task 15, or a
 * scope reference, Task 14): never a constant, and never handed to the evaluator. */
static odin3_status reads_unknown(const odin3_elab1 *e1, odin3_ast_id expr, bool *out) {
    odin3_ast_cursor cur;
    odin3_status st = odin3_ast_cursor_init(&cur, e1->parsed, expr);
    odin3_ast_event ev = ODIN3_AST_ENTER;
    odin3_ast_id node = {0};
    *out = false;
    while (st == ODIN3_OK && !*out && odin3_ast_cursor_next(&cur, &ev, &node)) {
        odin3_ast_kind kind = odin3_ast_kind_of(e1->parsed, node);
        *out = ev == ODIN3_AST_ENTER &&
               (kind == ODIN3_AST_HIER_NAME ||
                (kind == ODIN3_AST_IDENT &&
                 odin3_elab1_env_lookup(e1->env, odin3_ast_name(e1->parsed, node)) == E1_NONE));
    }
    odin3_ast_cursor_free(&cur);
    return st;
}

odin3_status odin3_elab1_try_constant(odin3_elab1 *e1, odin3_ast_id expr, odin3_cval *val,
                                      bool *is_const) {
    bool unknown = false;
    odin3_status st = reads_unknown(e1, expr, &unknown);
    *is_const = false;
    if (st != ODIN3_OK || unknown) {
        return st;
    }
    odin3_ceval_req req = {expr, {0, false, false}, "expression"};
    return odin3_ceval_try(e1->env->ev, &req, e1->env->arena, val, is_const);
}

static odin3_status lv_value(odin3_elab1 *e1, odin3_ast_id node, uint8_t *out) {
    odin3_cval val = {0};
    bool is_const = false;
    odin3_status st = odin3_elab1_try_constant(e1, node, &val, &is_const);
    *out = is_const ? E1_LV_CONST : E1_LV_OTHER;
    return st;
}

static uint8_t lv_ident(odin3_elab1 *e1, odin3_ast_id node) {
    uint32_t idx = odin3_elab1_env_find(e1->env, 0, odin3_ast_name(e1->parsed, node));
    if (idx == E1_NONE) {
        return E1_LV_NET; /* an implicit net (EL-25) */
    }
    switch (odin3_elab1_env_slot(e1->env, idx)->entry.kind) {
    case ODIN3_ENTRY_SIGNAL:
        return E1_LV_NET;
    case ODIN3_ENTRY_PARAM:
    case ODIN3_ENTRY_LOCALPARAM:
    case ODIN3_ENTRY_GENVAR:
        return E1_LV_CONST;
    default:
        return E1_LV_OTHER;
    }
}

/* A select of a net with constant indices is a net lvalue; of a parameter, a constant. */
static odin3_status lv_select(odin3_elab1 *e1, odin3_ast_id sel, uint8_t *out) {
    const odin3_ast *ast = e1->parsed;
    odin3_ast_id base = sel;
    while (odin3_ast_kind_of(ast, base) == ODIN3_AST_SELECT) {
        base = odin3_ast_child(ast, base, 0);
    }
    if (odin3_ast_kind_of(ast, base) != ODIN3_AST_IDENT) {
        return lv_value(e1, sel, out);
    }
    *out = lv_ident(e1, base);
    odin3_status st = ODIN3_OK;
    for (odin3_ast_id at = sel;
         *out == E1_LV_NET && st == ODIN3_OK && odin3_ast_kind_of(ast, at) == ODIN3_AST_SELECT;
         at = odin3_ast_child(ast, at, 0)) {
        for (uint32_t k = 1; k < odin3_ast_nchild(ast, at) && *out == E1_LV_NET; k++) {
            odin3_ast_id index = odin3_ast_child(ast, at, k);
            odin3_cval val = {0};
            bool is_const = true;
            st = index.v != 0 ? odin3_elab1_try_constant(e1, index, &val, &is_const) : ODIN3_OK;
            *out = st == ODIN3_OK && !is_const ? (uint8_t)E1_LV_OTHER : *out;
        }
    }
    return st;
}

static odin3_status lv_leaf(odin3_elab1 *e1, odin3_ast_id node, uint8_t *out) {
    switch (odin3_ast_kind_of(e1->parsed, node)) {
    case ODIN3_AST_NUMBER:
    case ODIN3_AST_STRING:
        *out = E1_LV_CONST;
        return ODIN3_OK;
    case ODIN3_AST_IDENT:
        *out = lv_ident(e1, node);
        return ODIN3_OK;
    case ODIN3_AST_SELECT:
        return lv_select(e1, node, out);
    default:
        return lv_value(e1, node, out);
    }
}

static odin3_status push_node(odin3_vec *stack, odin3_ast_id node) {
    odin3_ast_id *slot = odin3_vec_push(stack);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = node;
    return ODIN3_OK;
}

/* The worst class over expr's concatenation leaves (explicit stack): NET, CONST or OTHER. */
static odin3_status lv_classify(odin3_elab1 *e1, odin3_ast_id expr, uint8_t *out) {
    odin3_vec stack;
    odin3_vec_init(&stack, sizeof(odin3_ast_id));
    odin3_status st = push_node(&stack, expr);
    *out = E1_LV_NET;
    while (st == ODIN3_OK && stack.len > 0) {
        odin3_ast_id node = *(const odin3_ast_id *)odin3_vec_cat(&stack, stack.len - 1);
        odin3_vec_pop(&stack);
        if (odin3_ast_kind_of(e1->parsed, node) != ODIN3_AST_CONCAT) {
            uint8_t one = E1_LV_NET;
            st = lv_leaf(e1, node, &one);
            *out = one > *out ? one : *out;
            continue;
        }
        odin3_ast_span kids = odin3_ast_children(e1->parsed, node);
        for (uint32_t i = 0; st == ODIN3_OK && i < kids.n; i++) {
            st = push_node(&stack, kids.ids[i]);
        }
    }
    odin3_vec_free(&stack);
    return st;
}

/* The name of conn's port for messages. */
static const char *port_text(const odin3_elab1 *e1, const e1_target *tg,
                             const odin3_elab1_conn *rec) {
    if (rec->name != 0) {
        return str_of(e1, rec->name);
    }
    return tg != NULL && tg->type != NULL && rec->pos < tg->n ? tg->type->ports[rec->pos].name : "";
}

/* I3 on every output and inout connection with an expression. */
static odin3_status check_outputs(e1_connect *cx) {
    odin3_elab1 *e1 = cx->e1;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < e1->conns.len; i++) {
        odin3_elab1_conn rec = *(const odin3_elab1_conn *)odin3_vec_cat(&e1->conns, i);
        if (rec.expr.v == 0 || (rec.dir != ODIN3_DIR_OUT && rec.dir != ODIN3_DIR_INOUT)) {
            continue;
        }
        uint8_t cls = E1_LV_NET;
        st = lv_classify(e1, rec.expr, &cls);
        if (st != ODIN3_OK || cls == E1_LV_NET) {
            continue;
        }
        odin3_elab1_error(e1, odin3_ast_loc(e1->parsed, rec.conn),
                          cls == E1_LV_CONST ? "%s port '%s' of instance '%s' is connected to a "
                                               "constant"
                                             : "%s port '%s' of instance '%s' must be connected to "
                                               "a net",
                          rec.dir == ODIN3_DIR_OUT ? "output" : "inout",
                          port_text(e1, cx->tg, &rec), inst_name(e1, cx->inst));
        cx->bad = true;
    }
    return st;
}

/* --- odin3_elab1_connect ------------------------------------------------------------------ */

/* The module's connections in port order: one per named position, unconnected ones without a
 * CONNECTION (the written ones are appended per position, then compacted down). */
static odin3_status by_position(e1_connect *cx) {
    odin3_elab1 *e1 = cx->e1;
    size_t written = e1->conns.len;
    odin3_status st = odin3_vec_reserve(&e1->conns, written + cx->tg->n);
    for (uint32_t k = 0; st == ODIN3_OK && k < cx->tg->n; k++) {
        odin3_elab1_conn rec = {
            .name = tg_name(e1, cx->tg, k), .dir = tg_dir(e1, cx->tg, k), .pos = k};
        st = push_conn(e1, &rec);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    for (size_t i = 0; i < written; i++) {
        const odin3_elab1_conn *from = odin3_vec_cat(&e1->conns, i);
        odin3_elab1_conn *to = odin3_vec_at(&e1->conns, written + from->pos);
        to->conn = from->conn;
        to->expr = from->expr;
    }
    size_t kept = 0;
    for (size_t k = 0; k < cx->tg->n; k++) {
        const odin3_elab1_conn *rec = odin3_vec_cat(&e1->conns, written + k);
        if (rec->name != 0) {
            *(odin3_elab1_conn *)odin3_vec_at(&e1->conns, kept++) = *rec;
        }
    }
    e1->conns.len = kept;
    return ODIN3_OK;
}

/* A gate's terminals, as written. */
static odin3_status gate_terms(odin3_elab1 *e1, const odin3_elab_inst *inst) {
    odin3_ast_span conns = inst_conns(e1, inst->instance);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < conns.n; i++) {
        odin3_elab1_conn rec = {.dir = ODIN3_DIR_IN,
                                .pos = i,
                                .conn = conns.ids[i],
                                .expr = odin3_ast_child(e1->parsed, conns.ids[i], 0)};
        st = push_conn(e1, &rec);
    }
    return st;
}

/* An implicit black box's instance: one entry per port of the black box's union, in its port
 * order (EL-4 v4.8), each with the instance's connection of that name (pass E checked them). */
static odin3_status by_union(odin3_elab1 *e1, const odin3_elab_inst *inst) {
    uint32_t box = odin3_elab1_implicit_box(e1, odin3_ast_name(e1->parsed, inst->instantiation));
    if (box == 0) {
        return ODIN3_ERR_CHECK; /* pass E made it: a 2D bug */
    }
    odin3_ast_span conns = inst_conns(e1, inst->instance);
    uint64_t self = (uint64_t)inst->instance.v << E1_KEY_SHIFT;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < conns.n; i++) {
        uint32_t name = odin3_ast_name(e1->parsed, conns.ids[i]);
        st = odin3_u64map_put(e1->conn_seen, (odin3_kv){self | name, conns.ids[i].v});
    }
    uint32_t pos = 0;
    for (uint32_t at = odin3_elab1_bbox_at(e1, box - 1)->first_port; st == ODIN3_OK && at != 0;
         pos++) {
        const odin3_elab1_bbport *port = odin3_elab1_bbport_at(e1, at - 1);
        uint64_t conn = 0;
        bool connected = odin3_u64map_get(e1->conn_seen, self | port->port.name, &conn);
        odin3_elab1_conn rec = {.name = port->port.name, .dir = ODIN3_DIR_INOUT, .pos = pos};
        rec.conn = (odin3_ast_id){connected ? (uint32_t)conn : 0};
        rec.expr = connected ? odin3_ast_child(e1->parsed, rec.conn, 0) : (odin3_ast_id){0};
        at = port->next;
        st = push_conn(e1, &rec);
    }
    for (uint32_t i = 0; i < conns.n; i++) {
        (void)odin3_u64map_remove(e1->conn_seen, self | odin3_ast_name(e1->parsed, conns.ids[i]));
    }
    return st;
}

static odin3_status target_of(odin3_elab1 *e1, const odin3_elab_inst *inst, e1_target *tg) {
    if (inst->target.kind == ODIN3_RES_CELLTYPE) {
        const odin3_celltype_def *type = odin3_celltype_get(e1->design, inst->target.type);
        if (type == NULL) {
            return ODIN3_ERR_CHECK;
        }
        *tg = (e1_target){"cell type", type->name, 0, type, type->n_ports};
        return ODIN3_OK;
    }
    const odin3_elab_def *rec = odin3_elab1_def(e1, inst->target.def);
    if (inst->target.kind != ODIN3_RES_MODULE || rec == NULL) {
        return ODIN3_ERR_CHECK; /* nothing else is connected: a 2D bug */
    }
    *tg = (e1_target){"module", str_of(e1, rec->name), inst->target.def, NULL, rec->n_positions};
    return index_def_ports(e1, inst->target.def);
}

odin3_status odin3_elab1_connect(odin3_elab1 *e1, const odin3_elab_inst *inst) {
    odin3_vec_clear(&e1->conns);
    if (odin3_ast_kind_of(e1->parsed, inst->instantiation) == ODIN3_AST_GATE_DECL) {
        return gate_terms(e1, inst);
    }
    if (inst->target.kind == ODIN3_RES_IMPLICIT_BLACKBOX) {
        return by_union(e1, inst);
    }
    e1_target tg = {.kind_text = "module", .name = ""};
    odin3_status st = target_of(e1, inst, &tg);
    if (st != ODIN3_OK) {
        return st;
    }
    bool named = false;
    st = conn_form(e1, inst, &named);
    uint32_t given = inst_conns(e1, inst->instance).n;
    if (st == ODIN3_OK && !named && given > tg.n) {
        odin3_elab1_error(e1, odin3_ast_loc(e1->parsed, inst->instance),
                          "instance '%s' of '%s' has %" PRIu32 " connections but '%s' has %" PRIu32
                          " ports",
                          inst_name(e1, inst), tg.name, given, tg.name, tg.n);
        st = ODIN3_ERR_PARSE;
    }
    e1_connect cx = {e1, inst, &tg, false};
    st = st != ODIN3_OK ? st : resolve_all(&cx, named);
    st = st != ODIN3_OK ? st : check_outputs(&cx);
    st = st == ODIN3_OK && cx.bad ? ODIN3_ERR_PARSE : st;
    /* a module's and a black box's connections by port (EL-4 v4.8), a cell type's as written */
    return st == ODIN3_OK && tg.type == NULL ? by_position(&cx) : st;
}

/* --- instance arrays (I6, Q5) -------------------------------------------------------------- */

/* The values of a cell type's parameters for an instance: the defaults, the INTs given. */
static const odin3_value *type_params(odin3_elab1 *e1, const odin3_celltype_def *type,
                                      uint32_t given) {
    odin3_value *vals = odin3_arena_alloc(
        e1->env->arena, (size_t)(type->n_params > 0 ? type->n_params : 1) * sizeof *vals);
    if (vals == NULL) {
        latch(e1, ODIN3_ERR_NO_MEMORY);
        return NULL;
    }
    const odin3_elab1_given *rec = given < e1->given.len ? odin3_vec_cat(&e1->given, given) : NULL;
    for (uint32_t k = 0; k < type->n_params; k++) {
        vals[k] = type->params[k].dflt;
        for (uint32_t i = 0; rec != NULL && i < rec->count; i++) {
            const odin3_elab1_gparam *gp = odin3_vec_cat(&e1->gparams, rec->first + i);
            if (gp->kind == ODIN3_VAL_INT && type->params[k].kind == ODIN3_VAL_INT &&
                strcmp(str_of(e1, gp->name), type->params[k].name) == 0) {
                vals[k] = odin3_value_int(gp->num);
            }
        }
    }
    return vals;
}

/* Whether the instance's parameter record gives the parameter named param. */
static bool given_has(const odin3_elab1 *e1, uint32_t given, const char *param) {
    const odin3_elab1_given *rec = given < e1->given.len ? odin3_vec_cat(&e1->given, given) : NULL;
    for (uint32_t i = 0; rec != NULL && i < rec->count; i++) {
        const odin3_elab1_gparam *gp = odin3_vec_cat(&e1->gparams, rec->first + i);
        if (strcmp(str_of(e1, gp->name), param) == 0) {
            return true;
        }
    }
    return false;
}

/* Whether a cell type's port width is known before stage 2 infers parameters
 * (odin3_celltype_infer_params, from the width_param ports' connections): a constant width; a
 * width_param the instance gives; a width function or expression when no parameter is left to
 * infer (every width_param given). Otherwise the array connection goes whole. */
static bool width_known(const odin3_elab1 *e1, const odin3_celltype_def *type,
                        const odin3_elab_inst *inst, uint32_t port) {
    uint32_t given = inst->spec;
    const odin3_port_def *def = &type->ports[port];
    if (def->width_fn == NULL && def->width_expr == NULL) {
        return def->width_param == NULL || given_has(e1, given, def->width_param);
    }
    for (uint32_t k = 0; k < type->n_ports; k++) {
        const char *param = type->ports[k].width_param;
        if (param != NULL && !given_has(e1, given, param)) {
            return false;
        }
    }
    return true;
}

/* P of a connection: the child specialization's port, the cell type's (with the instance's
 * parameters), the implicit black box's (fixed by its first connection), a gate's 1. 0 when
 * unknown. */
static uint32_t port_width(odin3_elab1 *e1, const odin3_elab_inst *inst,
                           const odin3_elab1_conn *rec) {
    if (odin3_ast_kind_of(e1->parsed, inst->instantiation) == ODIN3_AST_GATE_DECL) {
        return 1;
    }
    if (inst->target.kind == ODIN3_RES_MODULE) {
        uint32_t count = 0;
        const odin3_elab1_port *ports = odin3_elab1_spec_ports(e1, inst->spec, &count);
        return ports != NULL && rec->pos < count ? ports[rec->pos].width : 0;
    }
    if (inst->target.kind == ODIN3_RES_CELLTYPE) {
        const odin3_celltype_def *type = odin3_celltype_get(e1->design, inst->target.type);
        if (type == NULL || rec->pos >= type->n_ports || !width_known(e1, type, inst, rec->pos)) {
            return 0;
        }
        const odin3_value *vals = type_params(e1, type, inst->spec);
        return vals != NULL
                   ? odin3_celltype_port_width(e1->design, inst->target.type, vals, rec->pos)
                   : 0;
    }
    uint32_t box = odin3_elab1_implicit_box(e1, odin3_ast_name(e1->parsed, inst->instantiation));
    uint32_t at = box != 0 ? odin3_elab1_bbox_port(e1, box - 1, rec->name) : 0;
    return at != 0 ? ((const odin3_elab1_bbport *)odin3_vec_cat(&e1->bbports, at - 1))->port.width
                   : 0;
}

/* One connection planned: whole, sliced, or the width error. */
static odin3_status plan_one(odin3_elab1 *e1, const odin3_elab_inst *inst, size_t idx) {
    odin3_elab1_conn rec = *(const odin3_elab1_conn *)odin3_vec_cat(&e1->conns, idx);
    uint32_t want = port_width(e1, inst, &rec);
    uint32_t width = 0;
    odin3_status st = odin3_elab1_expr_width(e1, rec.expr, &width);
    if (st != ODIN3_OK) {
        return st;
    }
    uint64_t all = (uint64_t)e1->array.n * want;
    odin3_elab1_conn *slot = odin3_vec_at(&e1->conns, idx);
    slot->port_width = want;
    slot->width = width;
    slot->sliced = want != 0 && width != want && width == all;
    if (want == 0 || width == want || width == all) {
        return ODIN3_OK;
    }
    const char *name = odin3_ast_name_str(e1->parsed, inst->instance);
    if (odin3_ast_kind_of(e1->parsed, inst->instantiation) == ODIN3_AST_GATE_DECL) {
        odin3_elab1_error(e1, odin3_ast_loc(e1->parsed, rec.conn),
                          "connection %" PRIu32 " of instance array '%s' is %" PRIu32
                          " bits; expected 1 or %" PRIu32,
                          rec.pos + 1, name, width, e1->array.n);
    } else {
        e1_target tg = {.type = inst->target.kind == ODIN3_RES_CELLTYPE
                                    ? odin3_celltype_get(e1->design, inst->target.type)
                                    : NULL};
        tg.n = tg.type != NULL ? tg.type->n_ports : 0;
        odin3_elab1_error(e1, odin3_ast_loc(e1->parsed, rec.conn),
                          "connection of '%s' to instance array '%s' is %" PRIu32
                          " bits; expected %" PRIu32 " or %" PRIu64,
                          port_text(e1, &tg, &rec), name, width, want, all);
    }
    return ODIN3_ERR_PARSE;
}

odin3_status odin3_elab1_inst_array(odin3_elab1 *e1, const odin3_elab_inst *inst) {
    odin3_status st = odin3_elab1_fold_array(e1, inst->instance, &e1->array);
    odin3_status worst = st;
    for (size_t i = 0; st == ODIN3_OK && i < e1->conns.len; i++) {
        if (((const odin3_elab1_conn *)odin3_vec_cat(&e1->conns, i))->expr.v == 0) {
            continue;
        }
        odin3_status one = plan_one(e1, inst, i);
        st = one == ODIN3_ERR_PARSE ? ODIN3_OK : one;
        worst = worst != ODIN3_OK ? worst : one;
    }
    return st != ODIN3_OK ? st : worst;
}

/* --- the parameters of cell-type and implicit black-box instances (EL-8, EL-11) ------------ */

/* The index of the type's parameter named text, E1_NONE when none. */
static uint32_t type_param(const odin3_celltype_def *type, const char *text) {
    for (uint32_t k = 0; k < type->n_params; k++) {
        if (strcmp(type->params[k].name, text) == 0) {
            return k;
        }
    }
    return E1_NONE;
}

/* The record entry of parameter name among gparams[first ..], NULL when none. */
static odin3_elab1_gparam *given_named(odin3_elab1 *e1, uint32_t first, uint32_t name) {
    for (size_t i = first; name != 0 && i < e1->gparams.len; i++) {
        odin3_elab1_gparam *gp = odin3_vec_at(&e1->gparams, i);
        if (gp->name == name) {
            return gp;
        }
    }
    return NULL;
}

const char *odin3_elab1_fixed_width_why(const odin3_celltype_def *type) {
    return type != NULL && type->n_params == 0 && strcmp(type->name, "adder") == 0
               ? "; the library cell has fixed port widths (an n-bit adder is a soft module Parmys "
                 "supplies, not modelled: spec 17 #78)"
               : "";
}

/* A cell type's unknown parameter (§9.1; a project line keeps its own text). */
static void no_param(odin3_elab1 *e1, const odin3_celltype_def *type,
                     const odin3_elab_override *ovr) {
    if (ovr->source == ODIN3_OVR_PROJECT) {
        odin3_elab1_bad_target(e1, ovr);
        return;
    }
    odin3_elab1_error(e1, ovr->at, "cell type '%s' has no parameter '%s'%s", type->name,
                      str_of(e1, ovr->path), odin3_elab1_fixed_width_why(type));
}

/* One override: checked against the type, kept per name by EL-8's precedence. */
static odin3_status given_one(odin3_elab1 *e1, const odin3_celltype_def *type,
                              const odin3_elab_override *ovr, uint32_t first) {
    const char *path = ovr->path != 0 ? str_of(e1, ovr->path) : "";
    if (strchr(path, ODIN3_ELAB_PATH_SEP) != NULL) {
        odin3_elab1_bad_target(e1, ovr); /* a cell type has no hierarchy (EL-8) */
        return ODIN3_ERR_PARSE;
    }
    if (type != NULL && type_param(type, path) == E1_NONE) {
        no_param(e1, type, ovr);
        return ODIN3_ERR_PARSE;
    }
    odin3_elab1_gparam *old = given_named(e1, first, ovr->path);
    if (old != NULL) {
        if (odin3_elab1_override_beats(ovr, &old->ovr)) {
            odin3_elab1_override_lost(e1, &old->ovr, ovr);
            old->ovr = *ovr;
        } else {
            odin3_elab1_override_lost(e1, ovr, &old->ovr);
        }
        return ODIN3_OK;
    }
    odin3_elab1_gparam *slot = odin3_vec_push(&e1->gparams);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (odin3_elab1_gparam){.name = ovr->path, .as_given = type == NULL, .ovr = *ovr};
    return ODIN3_OK;
}

/* Each kept value converted to its parameter's kind (EL-8: INT a signed 64-bit integer). */
static odin3_status convert_all(odin3_elab1 *e1, const odin3_celltype_def *type, uint32_t first) {
    odin3_status worst = ODIN3_OK;
    for (size_t i = first; i < e1->gparams.len; i++) {
        odin3_elab1_gparam *gp = odin3_vec_at(&e1->gparams, i);
        uint32_t k = type_param(type, str_of(e1, gp->name));
        gp->kind = (uint8_t)type->params[k].kind;
        if (gp->kind == ODIN3_VAL_INT && !odin3_cval_to_i64(&gp->ovr.value, &gp->num)) {
            odin3_elab1_error(e1, gp->ovr.at,
                              "value of parameter '%s' of cell type '%s' is not an integer",
                              type->params[k].name, type->name);
            worst = ODIN3_ERR_PARSE;
        }
    }
    return worst;
}

/* gparams[first ..] in the type's parameter order (an insertion sort: few parameters). */
static void sort_by_param(odin3_elab1 *e1, const odin3_celltype_def *type, uint32_t first) {
    for (size_t i = first + (size_t)1; i < e1->gparams.len; i++) {
        odin3_elab1_gparam cur = *(const odin3_elab1_gparam *)odin3_vec_cat(&e1->gparams, i);
        uint32_t key = type_param(type, str_of(e1, cur.name));
        size_t j = i;
        while (j > first &&
               type_param(
                   type,
                   str_of(e1,
                          ((const odin3_elab1_gparam *)odin3_vec_cat(&e1->gparams, j - 1))->name)) >
                   key) {
            *(odin3_elab1_gparam *)odin3_vec_at(&e1->gparams, j) =
                *(const odin3_elab1_gparam *)odin3_vec_cat(&e1->gparams, j - 1);
            j--;
        }
        *(odin3_elab1_gparam *)odin3_vec_at(&e1->gparams, j) = cur;
    }
}

odin3_status odin3_elab1_celltype_params(odin3_elab1 *e1, const odin3_elab_inst *inst) {
    const odin3_celltype_def *type = inst->target.kind == ODIN3_RES_CELLTYPE
                                         ? odin3_celltype_get(e1->design, inst->target.type)
                                         : NULL;
    uint32_t first = (uint32_t)e1->gparams.len;
    odin3_status worst = ODIN3_OK;
    for (size_t i = 0; i < e1->inst_ovr.len && worst != ODIN3_ERR_NO_MEMORY; i++) {
        odin3_elab_override ovr = *(const odin3_elab_override *)odin3_vec_cat(&e1->inst_ovr, i);
        odin3_status st = given_one(e1, type, &ovr, first);
        worst = worst != ODIN3_OK ? worst : st;
    }
    if (worst == ODIN3_OK && type != NULL) {
        worst = convert_all(e1, type, first);
        sort_by_param(e1, type, first);
    }
    odin3_elab1_given *rec = worst != ODIN3_ERR_NO_MEMORY ? odin3_vec_push(&e1->given) : NULL;
    if (rec == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *rec = (odin3_elab1_given){inst->target, first, (uint32_t)e1->gparams.len - first};
    return worst;
}
