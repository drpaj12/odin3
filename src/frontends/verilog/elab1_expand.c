/*
 * elab1_expand.c — pass E (2D spec §3.4; EL-8, EL-12, EL-16): a definition's environment (the
 * declarations of its module scope, their folded ranges, the lazily evaluated values of its
 * parameters and localparams, and the evaluator whose host it is), then pass E of one
 * specialization: every declaration evaluated, its defparams and pending overrides pushed down to
 * its instances, and each instance resolved and specialized, one decision-log entry per INSTANCE.
 * Task 11 has the module scope only (scope 0); Task 14 adds the generate scopes.
 */
#include "frontends/verilog/elab1.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "frontends/elab/ceval.h"
#include "frontends/elab/cval.h"
#include "frontends/elab/elab.h"
#include "frontends/project/project.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/hash.h"
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
    E1_SLOT_PARAMS = 0,
    E1_SLOT_PORTS = 1,
    E1_ITEMS = 2,
    E1_SCOPE_SHIFT = 32,
    E1_INT_BITS = 32,
    E1_TIME_BITS = 64,
    E1_NET_RANGE_SLOT = 1, /* NET_DECL(STRENGTH?, RANGE?, DELAY?) */
    E1_WHAT_EXTRA = 16     /* the blank, the quotes and the NUL of a <what>, with room */
};

static const uint32_t E1_NONE = UINT32_MAX;
static const int64_t E1_INT32_MIN = INT32_MIN;
static const int64_t E1_INT32_MAX = INT32_MAX;

/* A status that stops a sweep whose user errors are already reported: out of memory, or a
 * failed internal check (a 2D bug, never turned into OK). */
static bool e1_internal(odin3_status st) {
    return st == ODIN3_ERR_NO_MEMORY || st == ODIN3_ERR_CHECK;
}

/* worst, unless it is OK or a user error and st is internal (NO_MEMORY before CHECK). */
static odin3_status e1_keep_internal(odin3_status worst, odin3_status st) {
    if (worst == ODIN3_ERR_NO_MEMORY || !e1_internal(st)) {
        return worst;
    }
    return worst == ODIN3_ERR_CHECK && st != ODIN3_ERR_NO_MEMORY ? worst : st;
}

/* --- recording the declarations ------------------------------------------------------------ */

static odin3_status env_add(odin3_elab1_env *env, const odin3_elab1_slot *proto) {
    uint64_t key = (uint64_t)proto->scope << E1_SCOPE_SHIFT | proto->name;
    if (odin3_u64map_get(env->names, key, NULL)) {
        return ODIN3_OK; /* the first declaration of a name wins (M11 is Task 15's) */
    }
    odin3_elab1_slot *slot = odin3_vec_push(&env->slots);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *proto;
    odin3_status st = odin3_u64map_put(env->names, (odin3_kv){key, env->slots.len - 1});
    if (st != ODIN3_OK) {
        odin3_vec_pop(&env->slots);
    }
    return st;
}

static uint8_t param_kind(uint32_t sub) {
    switch (sub) {
    case ODIN3_AST_PARAM_DECL_PARAMETER:
        return ODIN3_ENTRY_PARAM;
    case ODIN3_AST_PARAM_DECL_LOCALPARAM:
        return ODIN3_ENTRY_LOCALPARAM;
    default:
        return 0; /* a specparam is ignored (P7, Task 16) */
    }
}

/* The DECLARATORs of group as entries of kind. */
static odin3_status add_declarators(odin3_elab1_env *env, odin3_ast_id group, uint8_t kind) {
    const odin3_ast *ast = env->e1->parsed;
    odin3_ast_span kids = odin3_ast_children(ast, group);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = odin3_ast_slot_count(odin3_ast_kind_of(ast, group));
         st == ODIN3_OK && i < kids.n; i++) {
        odin3_ast_id decl = kids.ids[i];
        odin3_elab1_slot proto = {.entry = {.kind = kind, .decl = decl},
                                  .group = group,
                                  .name = odin3_ast_name(ast, decl),
                                  .n_unpacked = odin3_ast_nchild(ast, decl) - 1};
        st = env_add(env, &proto);
    }
    return st;
}

/* An entry named by node itself (a FUNCTION_DECL, a TASK_DECL, an INSTANCE). */
static odin3_status add_named(odin3_elab1_env *env, odin3_ast_id node, odin3_ast_id group,
                              uint8_t kind) {
    odin3_elab1_slot proto = {.entry = {.kind = kind, .decl = node},
                              .group = group,
                              .name = odin3_ast_name(env->e1->parsed, node)};
    return proto.name != 0 ? env_add(env, &proto) : ODIN3_OK;
}

static odin3_status add_instances(odin3_elab1_env *env, odin3_ast_id item) {
    odin3_ast_span kids = odin3_ast_children(env->e1->parsed, item);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_INSTANTIATION); st == ODIN3_OK && i < kids.n;
         i++) {
        st = add_named(env, kids.ids[i], item, ODIN3_ENTRY_INSTANCE);
    }
    return st;
}

static odin3_status record_item(odin3_elab1_env *env, odin3_ast_id item) {
    const odin3_ast *ast = env->e1->parsed;
    switch (odin3_ast_kind_of(ast, item)) {
    case ODIN3_AST_PARAM_DECL: {
        uint8_t kind = param_kind(odin3_ast_sub(ast, item));
        return kind != 0 ? add_declarators(env, item, kind) : ODIN3_OK;
    }
    case ODIN3_AST_NET_DECL:
    case ODIN3_AST_VAR_DECL:
    case ODIN3_AST_PORT_DECL:
        return add_declarators(env, item, ODIN3_ENTRY_SIGNAL);
    case ODIN3_AST_GENVAR_DECL:
        return add_declarators(env, item, ODIN3_ENTRY_GENVAR);
    case ODIN3_AST_FUNCTION_DECL:
        return add_named(env, item, item, ODIN3_ENTRY_FUNCTION);
    case ODIN3_AST_TASK_DECL:
        return add_named(env, item, item, ODIN3_ENTRY_TASK);
    case ODIN3_AST_INSTANTIATION:
        return add_instances(env, item);
    default:
        return ODIN3_OK; /* generate constructs: Task 14 */
    }
}

static odin3_status record_span(odin3_elab1_env *env, odin3_ast_span span) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < span.n; i++) {
        st = span.ids[i].v != 0 ? record_item(env, span.ids[i]) : ODIN3_OK;
    }
    return st;
}

static odin3_status record_module(odin3_elab1_env *env) {
    const odin3_ast *ast = env->e1->parsed;
    odin3_ast_span kids = odin3_ast_children(ast, env->module);
    odin3_status st = record_span(
        env, odin3_ast_children(ast, odin3_ast_child(ast, env->module, E1_SLOT_PARAMS)));
    st = st != ODIN3_OK ? st
                        : record_span(env, odin3_ast_children(ast, odin3_ast_child(ast, env->module,
                                                                                   E1_SLOT_PORTS)));
    if (st == ODIN3_OK && kids.n > E1_ITEMS) {
        st = record_span(env, (odin3_ast_span){kids.ids + E1_ITEMS, kids.n - E1_ITEMS});
    }
    return st;
}

/* --- the evaluator's host -------------------------------------------------------------------- */

/* An error of the environment (dropped while quiet). */
static void env_error(const odin3_elab1_env *env, odin3_ast_id at, const char *fmt, ...)
    ODIN3_PRINTF(3, 4);

static void env_error(const odin3_elab1_env *env, odin3_ast_id at, const char *fmt, ...) {
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

static void host_report(void *user, odin3_log_level level, odin3_loc loc, const char *msg) {
    const odin3_elab1_env *env = user;
    if (!env->quiet) {
        odin3_elab1_report(env->e1, level, loc, msg);
    }
}

/* A CONST answer from a parameter's or localparam's slot. */
static void const_sym(const odin3_elab1_slot *slot, odin3_csym *out) {
    const odin3_cval *val = &slot->entry.value;
    out->kind = ODIN3_CSYM_CONST;
    out->value = *val;
    out->type = (odin3_etype){val->width, (val->flags & ODIN3_CVAL_SIGNED) != 0,
                              (val->flags & ODIN3_CVAL_UNSIZED) != 0};
    out->decl = slot->entry.decl;
    out->name = slot->name;
    out->msb = slot->msb;
    out->lsb = slot->lsb;
}

static void signal_sym(const odin3_elab1_slot *slot, odin3_csym *out) {
    out->kind = ODIN3_CSYM_SIGNAL;
    out->type = (odin3_etype){slot->entry.width, slot->entry.is_signed, false};
    out->decl = slot->entry.decl;
    out->name = slot->name;
    out->msb = slot->msb;
    out->lsb = slot->lsb;
}

static odin3_status lookup_ident(odin3_elab1_env *env, odin3_ast_id ref, odin3_csym *out) {
    uint32_t idx = odin3_elab1_env_find(env, 0, odin3_ast_name(env->e1->parsed, ref));
    if (idx == E1_NONE) {
        return ODIN3_OK; /* NONE: "'x' is not declared" (implicit nets are Task 15's) */
    }
    uint8_t kind = odin3_elab1_env_slot(env, idx)->entry.kind;
    if (kind != ODIN3_ENTRY_PARAM && kind != ODIN3_ENTRY_LOCALPARAM && kind != ODIN3_ENTRY_SIGNAL &&
        kind != ODIN3_ENTRY_GENVAR) {
        return ODIN3_OK;
    }
    odin3_status st = odin3_elab1_env_value(env, idx);
    const odin3_elab1_slot *slot = odin3_elab1_env_slot(env, idx);
    if (kind == ODIN3_ENTRY_PARAM || kind == ODIN3_ENTRY_LOCALPARAM) {
        if (st == ODIN3_OK) {
            const_sym(slot, out);
        }
        return st;
    }
    if (st == ODIN3_OK || slot->state == ODIN3_E1_BUSY) {
        signal_sym(slot, out); /* a range reading its own net: "not constant (it reads 'w')" */
        out->type.width = out->type.width != 0 ? out->type.width : 1;
        out->msb = slot->state == ODIN3_E1_BUSY ? 0 : out->msb;
        out->lsb = slot->state == ODIN3_E1_BUSY ? 0 : out->lsb;
        st = ODIN3_OK;
    }
    return st;
}

/* A SELECT: a memory word when its chain is as deep as the name's unpacked dimensions. */
static odin3_status lookup_word(odin3_elab1_env *env, odin3_ast_id sel, odin3_csym *out) {
    const odin3_ast *ast = env->e1->parsed;
    uint32_t depth = 0;
    odin3_ast_id obj = sel;
    while (odin3_ast_kind_of(ast, obj) == ODIN3_AST_SELECT) {
        obj = odin3_ast_child(ast, obj, 0);
        depth++;
    }
    if (odin3_ast_kind_of(ast, obj) != ODIN3_AST_IDENT) {
        return ODIN3_OK;
    }
    uint32_t idx = odin3_elab1_env_find(env, 0, odin3_ast_name(ast, obj));
    if (idx == E1_NONE) {
        return ODIN3_OK;
    }
    const odin3_elab1_slot *slot = odin3_elab1_env_slot(env, idx);
    if (slot->entry.kind != ODIN3_ENTRY_SIGNAL || slot->n_unpacked != depth) {
        return ODIN3_OK;
    }
    odin3_status st = odin3_elab1_env_value(env, idx);
    if (st == ODIN3_OK) {
        signal_sym(odin3_elab1_env_slot(env, idx), out);
    }
    return st;
}

static odin3_status host_lookup(void *user, odin3_ast_id ref, odin3_csym *out) {
    odin3_elab1_env *env = user;
    switch (odin3_ast_kind_of(env->e1->parsed, ref)) {
    case ODIN3_AST_SELECT:
        return lookup_word(env, ref, out);
    case ODIN3_AST_IDENT:
        return lookup_ident(env, ref, out);
    default: /* a HIER_NAME: generate scopes are Task 14's */
        if (!env->quiet) {
            odin3_elab1_not_yet(env->e1, ref);
        }
        return ODIN3_ERR_PARSE;
    }
}

static odin3_status host_callee(void *user, odin3_ast_id call, odin3_csym *out) {
    odin3_elab1_env *env = user;
    uint32_t idx = odin3_elab1_env_find(env, 0, odin3_ast_name(env->e1->parsed, call));
    if (idx == E1_NONE) {
        return ODIN3_OK;
    }
    const odin3_elab1_slot *slot = odin3_elab1_env_slot(env, idx);
    if (slot->entry.kind == ODIN3_ENTRY_FUNCTION || slot->entry.kind == ODIN3_ENTRY_TASK) {
        out->kind =
            slot->entry.kind == ODIN3_ENTRY_FUNCTION ? ODIN3_CSYM_FUNCTION : ODIN3_CSYM_TASK;
        out->decl = slot->entry.decl;
        out->name = slot->name;
    }
    return ODIN3_OK;
}

/* --- the environment ----------------------------------------------------------------------- */

static odin3_status env_tables(odin3_elab1_env *env) {
    env->arena = odin3_arena_create(0);
    env->names = odin3_u64map_create(0);
    env->ranged = odin3_u64map_create(0);
    if (env->arena == NULL || env->names == NULL || env->ranged == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    const odin3_ceval_host host = {host_lookup, host_callee, host_report, env};
    return odin3_ceval_create(env->e1->parsed, &host, &env->ev);
}

odin3_status odin3_elab1_env_create(odin3_elab1 *e1, uint32_t def, odin3_elab1_env **out) {
    *out = NULL;
    odin3_elab1_env *env = odin3_util_calloc(sizeof *env);
    if (env == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    env->e1 = e1;
    env->def = def;
    env->module = odin3_elab1_def(e1, def)->node;
    odin3_vec_init(&env->slots, sizeof(odin3_elab1_slot));
    odin3_vec_init(&env->ranges, sizeof(odin3_elab1_range));
    odin3_status st = env_tables(env);
    st = st != ODIN3_OK ? st : record_module(env);
    if (st != ODIN3_OK) {
        odin3_elab1_env_destroy(env);
        return st;
    }
    *out = env;
    return ODIN3_OK;
}

void odin3_elab1_env_destroy(odin3_elab1_env *env) {
    if (env == NULL) {
        return;
    }
    odin3_ctype_destroy(env->types);
    odin3_ceval_destroy(env->ev);
    odin3_vec_free(&env->slots);
    odin3_vec_free(&env->ranges);
    odin3_u64map_destroy(env->names);
    odin3_u64map_destroy(env->ranged);
    odin3_arena_destroy(env->arena);
    odin3_util_free(env);
}

uint32_t odin3_elab1_env_find(const odin3_elab1_env *env, uint32_t scope, uint32_t name) {
    uint64_t idx = 0;
    if (name == 0 ||
        !odin3_u64map_get(env->names, (uint64_t)scope << E1_SCOPE_SHIFT | name, &idx)) {
        return E1_NONE;
    }
    return (uint32_t)idx;
}

odin3_elab1_slot *odin3_elab1_env_slot(odin3_elab1_env *env, uint32_t idx) {
    return odin3_vec_at(&env->slots, idx);
}

/* --- evaluation ---------------------------------------------------------------------------- */

uint8_t odin3_elab1_value_base(odin3_elab1_env *env, odin3_ast_id expr) {
    const odin3_ast *ast = env->e1->parsed;
    switch (odin3_ast_kind_of(ast, expr)) {
    case ODIN3_AST_NUMBER:
        return (uint8_t)odin3_ast_sub(ast, expr);
    case ODIN3_AST_IDENT: {
        uint32_t idx = odin3_elab1_env_find(env, 0, odin3_ast_name(ast, expr));
        return idx != E1_NONE ? odin3_elab1_env_slot(env, idx)->base : ODIN3_AST_NUMBER_DEC;
    }
    default:
        return ODIN3_AST_NUMBER_DEC;
    }
}

odin3_status odin3_elab1_eval(odin3_elab1_env *env, const odin3_elab1_eval_req *req,
                              odin3_cval *out) {
    odin3_ceval_req eval = {req->expr, {0, false, false}, req->what};
    odin3_status st = odin3_ceval_expr(env->ev, &eval, env->arena, out);
    if (st == ODIN3_OK && req->base != NULL) {
        *req->base = (out->flags & ODIN3_CVAL_UNSIZED) != 0 ? odin3_elab1_value_base(env, req->expr)
                                                            : (uint8_t)ODIN3_AST_NUMBER_BIN;
    }
    return st;
}

/* |value| + 1 as a width, for the width error of an out-of-range bound. */
static uint64_t magnitude_width(int64_t value) {
    uint64_t mag = value < 0 ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;
    return mag == UINT64_MAX ? mag : mag + 1;
}

odin3_status odin3_elab1_fold_int(odin3_elab1_env *env, odin3_ast_id expr, const char *what,
                                  int32_t *out) {
    odin3_elab1_eval_req req = {expr, what, NULL};
    odin3_cval val = {0};
    odin3_status st = odin3_elab1_eval(env, &req, &val);
    if (st != ODIN3_OK) {
        return st;
    }
    int64_t num = 0;
    if (val.width == 0 || !odin3_cval_is_known(&val)) {
        env_error(env, expr, "%s has x or z bits", what);
        return ODIN3_ERR_PARSE;
    }
    if (!odin3_cval_to_i64(&val, &num) || num < E1_INT32_MIN || num > E1_INT32_MAX) {
        bool fits = odin3_cval_to_i64(&val, &num);
        env_error(env, expr, "width %" PRIu64 " exceeds the limit of %" PRIu32 " bits",
                  fits ? magnitude_width(num) : UINT64_MAX, env->e1->limits->max_width);
        return ODIN3_ERR_PARSE;
    }
    *out = (int32_t)num;
    return ODIN3_OK;
}

/* A RANGE to fold and §9.1's <what> of its bounds. */
typedef struct e1_range_req {
    odin3_ast_id range;
    const char *what;
} e1_range_req;

/* Both bounds of range folded, and its width within the cap. */
static odin3_status fold_range(odin3_elab1_env *env, const e1_range_req *req, int64_t bounds[2]) {
    const odin3_ast *ast = env->e1->parsed;
    odin3_ast_id range = req->range;
    int32_t msb = 0;
    int32_t lsb = 0;
    odin3_status st = odin3_elab1_fold_int(env, odin3_ast_child(ast, range, 0), req->what, &msb);
    st = st != ODIN3_OK
             ? st
             : odin3_elab1_fold_int(env, odin3_ast_child(ast, range, 1), req->what, &lsb);
    if (st != ODIN3_OK) {
        return st;
    }
    uint64_t span = msb >= lsb ? (uint64_t)((int64_t)msb - lsb) : (uint64_t)((int64_t)lsb - msb);
    if (span >= env->e1->limits->max_width) {
        env_error(env, range, "width %" PRIu64 " exceeds the limit of %" PRIu32 " bits", span + 1,
                  env->e1->limits->max_width);
        return ODIN3_ERR_PARSE;
    }
    bounds[0] = msb;
    bounds[1] = lsb;
    return ODIN3_OK;
}

/* A declaration's folded RANGE, memoized per node (req->what names its bounds). */
static odin3_status range_folded(odin3_elab1_env *env, const e1_range_req *req, int64_t *msb,
                                 int64_t *lsb) {
    odin3_ast_id range = req->range;
    uint64_t at = 0;
    if (!odin3_u64map_get(env->ranged, range.v, &at)) {
        odin3_elab1_range *rec = odin3_vec_push(&env->ranges);
        if (rec == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *rec = (odin3_elab1_range){range, ODIN3_E1_BUSY, 0, 0};
        at = env->ranges.len - 1;
        odin3_status st = odin3_u64map_put(env->ranged, (odin3_kv){range.v, at});
        int64_t bounds[2] = {0, 0};
        st = st != ODIN3_OK ? st : fold_range(env, req, bounds);
        rec = odin3_vec_at(&env->ranges, (size_t)at);
        rec->state = st == ODIN3_OK ? ODIN3_E1_DONE : ODIN3_E1_FAILED;
        rec->msb = bounds[0];
        rec->lsb = bounds[1];
        if (st == ODIN3_ERR_NO_MEMORY) {
            return st;
        }
    }
    const odin3_elab1_range *rec = odin3_vec_cat(&env->ranges, (size_t)at);
    if (rec->state != ODIN3_E1_DONE) {
        return ODIN3_ERR_PARSE;
    }
    *msb = rec->msb;
    *lsb = rec->lsb;
    return ODIN3_OK;
}

odin3_status odin3_elab1_env_range(odin3_elab1_env *env, odin3_ast_id range, int64_t *msb,
                                   int64_t *lsb) {
    e1_range_req req = {range, "range bound"};
    return range_folded(env, &req, msb, lsb);
}

/* "<prefix> 'name'" in the environment's arena (§9.1's <what>); prefix alone without memory. */
static const char *named_what(odin3_elab1_env *env, const char *prefix, uint32_t name) {
    const char *text = odin3_elab1_str(env->e1, name);
    size_t len = strlen(prefix) + strlen(text) + E1_WHAT_EXTRA;
    char *what = odin3_arena_alloc(env->arena, len);
    if (what == NULL) {
        return prefix;
    }
    int got = snprintf(what, len, "%s '%s'", prefix, text);
    return got > 0 && (size_t)got < len ? what : prefix;
}

/* "parameter 'P'" (§9.1's <what> of a parameter's value). */
static const char *param_what(odin3_elab1_env *env, uint32_t name) {
    return named_what(env, "parameter", name);
}

/* --- declared types (EL-16, EL-23) ---------------------------------------------------------- */

/* A parameter's declared type and range: P4 for real, realtime and time. */
typedef struct e1_ptype {
    odin3_cparam_type type;
    int64_t msb;
    int64_t lsb;
    bool has_range;
} e1_ptype;

/* The spelling of an unsupported parameter type (P4). */
static const char *dt_name(uint32_t dt) {
    switch (dt) {
    case ODIN3_AST_DT_REAL:
        return "real";
    case ODIN3_AST_DT_REALTIME:
        return "realtime";
    default:
        return "time";
    }
}

static odin3_status param_type(odin3_elab1_env *env, uint32_t idx, e1_ptype *out) {
    const odin3_ast *ast = env->e1->parsed;
    odin3_ast_id group = odin3_elab1_env_slot(env, idx)->group;
    uint32_t flags = odin3_ast_flags(ast, group);
    uint32_t dt = (flags & ODIN3_AST_F_PARAM_DECL_DT_MASK) >> ODIN3_AST_F_PARAM_DECL_DT_SHIFT;
    bool is_signed = (flags & ODIN3_AST_F_PARAM_DECL_SIGNED) != 0;
    *out = (e1_ptype){{ODIN3_CPARAM_UNTYPED, 0, false}, 0, 0, false};
    if (dt == ODIN3_AST_DT_REAL || dt == ODIN3_AST_DT_REALTIME || dt == ODIN3_AST_DT_TIME) {
        env_error(env, group, "parameter type '%s' is not supported", dt_name(dt));
        return ODIN3_ERR_PARSE;
    }
    if (dt == ODIN3_AST_DT_INTEGER) {
        *out = (e1_ptype){{ODIN3_CPARAM_SIZED, E1_INT_BITS, true}, E1_INT_BITS - 1, 0, true};
        return ODIN3_OK;
    }
    odin3_ast_id range = odin3_ast_child(ast, group, 0);
    if (range.v == 0) {
        out->type.mode = is_signed ? ODIN3_CPARAM_SIGNED : ODIN3_CPARAM_UNTYPED;
        return ODIN3_OK;
    }
    e1_range_req req = {
        range, named_what(env, "range bound of parameter", odin3_elab1_env_slot(env, idx)->name)};
    odin3_status st = range_folded(env, &req, &out->msb, &out->lsb);
    uint64_t span =
        out->msb >= out->lsb ? (uint64_t)(out->msb - out->lsb) : (uint64_t)(out->lsb - out->msb);
    out->type = (odin3_cparam_type){ODIN3_CPARAM_SIZED, (uint32_t)span + 1, is_signed};
    out->has_range = true;
    return st;
}

/* The value of the parameter or localparam in slot idx before conversion: its override or its
 * default (whose literal gives the base of an unsized value). */
static odin3_status raw_value(odin3_elab1_env *env, uint32_t idx, odin3_cval *val, uint8_t *base) {
    const odin3_elab1_slot *slot = odin3_elab1_env_slot(env, idx);
    if (slot->overridden) {
        *val = slot->ovr.value;
        *base = slot->ovr.base;
        return ODIN3_OK;
    }
    odin3_ast_id decl = slot->entry.decl;
    odin3_ast_id expr = odin3_ast_child(env->e1->parsed, decl, 0);
    if (expr.v == 0) {
        env_error(env, decl, "parameter '%s' has no value", odin3_elab1_str(env->e1, slot->name));
        return ODIN3_ERR_PARSE;
    }
    odin3_elab1_eval_req req = {expr, param_what(env, slot->name), base};
    return odin3_elab1_eval(env, &req, val);
}

static odin3_status param_value(odin3_elab1_env *env, uint32_t idx) {
    e1_ptype decl;
    odin3_status st = param_type(env, idx, &decl);
    odin3_cval raw = {0};
    uint8_t base = ODIN3_AST_NUMBER_DEC;
    st = st != ODIN3_OK ? st : raw_value(env, idx, &raw, &base);
    odin3_cval val = {0};
    st = st != ODIN3_OK ? st : odin3_cval_to_param(env->arena, &raw, &decl.type, &val);
    odin3_elab1_slot *slot = odin3_elab1_env_slot(env, idx);
    if (st == ODIN3_OK && val.width == 0) {
        env_error(env, slot->entry.decl, "zero-width concatenation");
        st = ODIN3_ERR_PARSE;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    slot->entry.value = val;
    slot->entry.width = val.width;
    slot->entry.is_signed = (val.flags & ODIN3_CVAL_SIGNED) != 0;
    slot->base = (val.flags & ODIN3_CVAL_UNSIZED) != 0 ? base : (uint8_t)ODIN3_AST_NUMBER_BIN;
    bool ranged = decl.has_range && decl.type.width == val.width;
    slot->msb = ranged ? decl.msb : (int64_t)val.width - 1;
    slot->lsb = ranged ? decl.lsb : 0;
    return ODIN3_OK;
}

/* The RANGE of a net, variable or port declaration (0 for a genvar or none). */
static odin3_ast_id signal_range(const odin3_ast *ast, odin3_ast_id group) {
    switch (odin3_ast_kind_of(ast, group)) {
    case ODIN3_AST_NET_DECL:
        return odin3_ast_child(ast, group, E1_NET_RANGE_SLOT);
    case ODIN3_AST_VAR_DECL:
    case ODIN3_AST_PORT_DECL:
        return odin3_ast_child(ast, group, 0);
    default:
        return (odin3_ast_id){0};
    }
}

/* The width of an unranged declaration's value: integer and genvar 32, time 64 (the other
 * variable kinds, rejected by Task 16, alike), else 1; *is_integer for the signed ones. */
static uint32_t unranged_width(const odin3_ast *ast, odin3_ast_id group, bool *is_integer) {
    odin3_ast_kind kind = odin3_ast_kind_of(ast, group);
    uint32_t sub = odin3_ast_sub(ast, group);
    uint32_t dt = 0;
    if (kind == ODIN3_AST_PORT_DECL) {
        uint32_t flags = odin3_ast_flags(ast, group);
        dt = (flags & ODIN3_AST_F_PORT_DECL_DT_MASK) >> ODIN3_AST_F_PORT_DECL_DT_SHIFT;
    }
    *is_integer = kind == ODIN3_AST_GENVAR_DECL || dt == ODIN3_AST_DT_INTEGER ||
                  (kind == ODIN3_AST_VAR_DECL && sub == ODIN3_AST_VAR_DECL_INTEGER);
    if (*is_integer) {
        return E1_INT_BITS;
    }
    bool wide =
        dt == ODIN3_AST_DT_TIME || (kind == ODIN3_AST_VAR_DECL && sub != ODIN3_AST_VAR_DECL_REG);
    return wide ? E1_TIME_BITS : 1;
}

/* The width, sign and range of a net, variable or port (integer: 32 signed; time: 64). */
static odin3_status signal_type(odin3_elab1_env *env, uint32_t idx) {
    const odin3_ast *ast = env->e1->parsed;
    odin3_ast_id group = odin3_elab1_env_slot(env, idx)->group;
    bool integer = false;
    int64_t msb = (int64_t)unranged_width(ast, group, &integer) - 1;
    int64_t lsb = 0;
    odin3_ast_id range = {0};
    if (!integer) {
        range = signal_range(ast, group);
    }
    odin3_status st = range.v != 0 ? odin3_elab1_env_range(env, range, &msb, &lsb) : ODIN3_OK;
    odin3_elab1_slot *slot = odin3_elab1_env_slot(env, idx);
    slot->msb = msb;
    slot->lsb = lsb;
    slot->entry.width = (uint32_t)(msb >= lsb ? msb - lsb : lsb - msb) + 1;
    slot->entry.is_signed =
        integer || (odin3_ast_flags(ast, group) & ODIN3_AST_F_NET_DECL_SIGNED) != 0;
    return st;
}

/* The state an evaluation ending with st leaves (out of memory is remembered as such). */
static uint8_t state_after(odin3_status st) {
    if (st == ODIN3_OK) {
        return ODIN3_E1_DONE;
    }
    return st == ODIN3_ERR_NO_MEMORY ? ODIN3_E1_NO_MEMORY : ODIN3_E1_FAILED;
}

odin3_status odin3_elab1_env_value(odin3_elab1_env *env, uint32_t idx) {
    odin3_elab1_slot *slot = odin3_elab1_env_slot(env, idx);
    uint8_t kind = slot->entry.kind;
    bool param = kind == ODIN3_ENTRY_PARAM || kind == ODIN3_ENTRY_LOCALPARAM;
    switch (slot->state) {
    case ODIN3_E1_DONE:
        return ODIN3_OK;
    case ODIN3_E1_FAILED:
        return ODIN3_ERR_PARSE;
    case ODIN3_E1_NO_MEMORY:
        return ODIN3_ERR_NO_MEMORY;
    case ODIN3_E1_BUSY:
        if (param) {
            env_error(env, slot->entry.decl, "parameter '%s' depends on itself",
                      odin3_elab1_str(env->e1, slot->name));
        }
        return ODIN3_ERR_PARSE;
    default:
        break;
    }
    if (!param && kind != ODIN3_ENTRY_SIGNAL && kind != ODIN3_ENTRY_GENVAR) {
        return ODIN3_OK; /* functions, tasks and instances have no value */
    }
    slot->state = ODIN3_E1_BUSY;
    odin3_status st = param ? param_value(env, idx) : signal_type(env, idx);
    slot = odin3_elab1_env_slot(env, idx);
    slot->state = state_after(st);
    return st;
}

/* --- override paths ------------------------------------------------------------------------- */

/* The HIER_NAME parts of a defparam joined by '.' (an index part by its name; generate scopes
 * are Task 14's). */
static odin3_status append_hier(const odin3_elab1 *e1, odin3_ast_id hier, odin3_strbuf *out) {
    odin3_ast_span parts = odin3_ast_children(e1->parsed, hier);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < parts.n; i++) {
        odin3_ast_id part = parts.ids[i];
        if (odin3_ast_kind_of(e1->parsed, part) == ODIN3_AST_SELECT) {
            part = odin3_ast_child(e1->parsed, part, 0);
        }
        st = odin3_strbuf_appendf(out, "%s%s", i > 0 ? "." : "",
                                  odin3_ast_name_str(e1->parsed, part));
    }
    return st;
}

odin3_status odin3_elab1_override_text(const odin3_elab1 *e1, const odin3_elab_override *ovr,
                                       odin3_strbuf *out) {
    if (ovr->source == ODIN3_OVR_DEFPARAM) {
        return append_hier(e1, odin3_ast_child(e1->parsed, ovr->from, 0), out);
    }
    const odin3_project *proj = odin3_design_project(e1->design);
    if (ovr->source == ODIN3_OVR_PROJECT && proj != NULL && ovr->order < proj->params.len) {
        const odin3_proj_param *param = odin3_vec_cat(&proj->params, ovr->order);
        return odin3_strbuf_appendf(out, "%s.%s", odin3_elab1_str(e1, param->scope),
                                    odin3_elab1_str(e1, param->name));
    }
    size_t from = out->len;
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr(odin3_elab1_str(e1, ovr->path)));
    if (st == ODIN3_OK) {
        odin3_elab1_path_print(out->data + from, out->len - from);
    }
    return st;
}

/* --- pass E ---------------------------------------------------------------------------------- */

/* An override for the instance named inst of the module scope (a defparam of the module, or a
 * pending override of the specialization), its path below that instance. */
typedef struct e1_dp {
    uint32_t inst;
    odin3_elab_override ovr;
    uint32_t next; /* the next one for the same instance + 1, 0 = none */
} e1_dp;

typedef struct e1_pass {
    odin3_elab1 *e1;
    odin3_elab1_env *env;
    uint32_t spec;
    uint32_t def;
    odin3_vec dps;            /* e1_dp */
    odin3_u64map *dp_by_inst; /* instance name → (first dp + 1) | (last dp + 1) << 32 */
    odin3_vec list;           /* odin3_elab_override: one INSTANTIATION's #() values */
} e1_pass;

static odin3_status push_override(odin3_vec *vec, const odin3_elab_override *ovr) {
    odin3_elab_override *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *ovr;
    return ODIN3_OK;
}

/* The override for instance inst, appended to its chain (indexed by name: review m3). */
static odin3_status push_dp(e1_pass *ps, uint32_t inst, const odin3_elab_override *ovr) {
    e1_dp *slot = odin3_vec_push(&ps->dps);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (e1_dp){inst, *ovr, 0};
    uint64_t self = ps->dps.len;
    uint64_t ends = 0;
    if (!odin3_u64map_get(ps->dp_by_inst, inst, &ends)) {
        ends = self | self << E1_SCOPE_SHIFT;
    } else {
        ((e1_dp *)odin3_vec_at(&ps->dps, (ends >> E1_SCOPE_SHIFT) - 1))->next = (uint32_t)self;
        ends = (ends & UINT32_MAX) | self << E1_SCOPE_SHIFT;
    }
    odin3_status st = odin3_u64map_put(ps->dp_by_inst, (odin3_kv){inst, ends});
    if (st != ODIN3_OK) {
        odin3_vec_pop(&ps->dps); /* the chain is unchanged: the run stops on NO_MEMORY */
    }
    return st;
}

/* The first override for instance name + 1 (0 = none); the next one is its e1_dp.next. */
static uint32_t first_dp(const e1_pass *ps, uint32_t name) {
    uint64_t ends = 0;
    return odin3_u64map_get(ps->dp_by_inst, name, &ends) ? (uint32_t)(ends & UINT32_MAX) : 0;
}

/* True when name is an instance of the module scope. */
static bool is_instance(const e1_pass *ps, uint32_t name) {
    uint32_t idx = odin3_elab1_env_find(ps->env, 0, name);
    return idx != E1_NONE && odin3_elab1_env_slot(ps->env, idx)->entry.kind == ODIN3_ENTRY_INSTANCE;
}

/* The ID of bytes when interned already (0 otherwise: nothing has that name). */
static uint32_t find_id(const odin3_elab1 *e1, odin3_bytes bytes) {
    uint32_t id = 0;
    return odin3_strtab_find(odin3_design_strtab(e1->design), bytes, &id) ? id : 0;
}

void odin3_elab1_bad_target(odin3_elab1 *e1, const odin3_elab_override *ovr) {
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    odin3_status st = odin3_elab1_override_text(e1, ovr, &text);
    if (st != ODIN3_OK) {
        e1->fatal = e1->fatal != ODIN3_OK ? e1->fatal : st;
    } else if (ovr->source == ODIN3_OVR_PROJECT) {
        odin3_elab1_error(e1, ovr->at, "param '%s' does not name a parameter", text.data);
    } else {
        odin3_elab1_error(e1, ovr->at,
                          "defparam target '%s' does not name a parameter of an instance below "
                          "this module",
                          text.data);
    }
    odin3_strbuf_free(&text);
}

/* One defparam of the module scope: its target instance and the path below it; its value
 * evaluated here (EL-8). A path that does not start at an instance is the target error. */
static odin3_status collect_defparam(e1_pass *ps, odin3_ast_id item) {
    const odin3_ast *ast = ps->e1->parsed;
    odin3_ast_id hier = odin3_ast_child(ast, item, 0);
    odin3_ast_span parts = odin3_ast_children(ast, hier);
    for (uint32_t i = 0; i < parts.n; i++) {
        if (odin3_ast_kind_of(ast, parts.ids[i]) != ODIN3_AST_IDENT) {
            odin3_elab1_not_yet(ps->e1, hier); /* a generate scope (g[1].u.W): Task 14 */
            return ODIN3_OK;
        }
    }
    odin3_elab_override ovr = {.source = ODIN3_OVR_DEFPARAM,
                               .order = item.v,
                               .at = odin3_ast_loc(ast, hier),
                               .from = item};
    uint32_t inst = odin3_ast_name(ast, parts.ids[0]);
    if (parts.n < 2 || !is_instance(ps, inst)) {
        odin3_elab1_bad_target(ps->e1, &ovr);
        return ODIN3_OK;
    }
    odin3_strbuf rest;
    odin3_strbuf_init(&rest);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 1; st == ODIN3_OK && i < parts.n; i++) {
        if (i > 1) {
            char sep = ODIN3_ELAB_PATH_SEP;
            st = odin3_strbuf_append(&rest, (odin3_bytes){&sep, 1});
        }
        st = st != ODIN3_OK ? st
                            : odin3_strbuf_append(
                                  &rest, odin3_bytes_cstr(odin3_ast_name_str(ast, parts.ids[i])));
    }
    st = st != ODIN3_OK
             ? st
             : odin3_design_intern(ps->e1->design, (odin3_bytes){rest.data, rest.len}, &ovr.path);
    odin3_strbuf_free(&rest);
    odin3_elab1_eval_req req = {odin3_ast_child(ast, item, 1),
                                param_what(ps->env, odin3_ast_name(ast, parts.ids[parts.n - 1])),
                                &ovr.base};
    odin3_status ev = st == ODIN3_OK ? odin3_elab1_eval(ps->env, &req, &ovr.value) : st;
    if (ev == ODIN3_ERR_PARSE) {
        return ODIN3_OK; /* reported; the instance takes no value from it (EL-44) */
    }
    ovr.value.flags &= (uint8_t)~ODIN3_CVAL_UNSIZED; /* an override is sized (v4.7) */
    return ev != ODIN3_OK ? ev : push_dp(ps, inst, &ovr);
}

/* One pending override of the specialization: "u<SEP>rest" goes to instance u as "rest". */
static odin3_status split_pending(e1_pass *ps, const odin3_elab_override *ovr) {
    const char *path = odin3_elab1_str(ps->e1, ovr->path);
    const char *dot = strchr(path, ODIN3_ELAB_PATH_SEP);
    uint32_t inst = dot != NULL ? find_id(ps->e1, (odin3_bytes){path, (size_t)(dot - path)}) : 0;
    if (inst == 0 || !is_instance(ps, inst)) {
        odin3_elab1_bad_target(ps->e1, ovr);
        return ODIN3_OK;
    }
    odin3_elab_override down = *ovr;
    odin3_status st = odin3_design_intern(ps->e1->design, odin3_bytes_cstr(dot + 1), &down.path);
    return st != ODIN3_OK ? st : push_dp(ps, inst, &down);
}

static odin3_status collect_overrides(e1_pass *ps) {
    const odin3_ast *ast = ps->e1->parsed;
    odin3_ast_span kids = odin3_ast_children(ast, ps->env->module);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = E1_ITEMS; st == ODIN3_OK && i < kids.n; i++) {
        if (odin3_ast_kind_of(ast, kids.ids[i]) == ODIN3_AST_DEFPARAM) {
            st = collect_defparam(ps, kids.ids[i]);
        }
    }
    const odin3_elab1_spec *spec = odin3_elab1_spec_at(ps->e1, ps->spec);
    for (uint32_t i = 0; st == ODIN3_OK && i < spec->n_pending; i++) {
        odin3_elab_override ovr = odin3_elab1_spec_at(ps->e1, ps->spec)->pending[i];
        st = split_pending(ps, &ovr);
    }
    return st;
}

/* --- the #() values of an INSTANTIATION (EL-8) ------------------------------------------- */

/* What a #() is for: a definition's `parameter`s, a cell type's parameters (EL-11), or an
 * implicit black box's (any, ordered ones kept unnamed). */
typedef struct e1_ptarget {
    const char *what; /* "module" or "cell type" (the too-many error) */
    const char *name;
    uint32_t def;                   /* a definition, else E1_NONE */
    const odin3_celltype_def *type; /* a cell type, else NULL */
    uint32_t n_params;              /* E1_NONE: no limit (an implicit black box) */
} e1_ptarget;

static e1_ptarget def_target(const odin3_elab1 *e1, uint32_t target) {
    const odin3_elab_def *def = odin3_elab1_def(e1, target);
    return (e1_ptarget){"module", odin3_elab1_str(e1, def->name), target, NULL,
                        def->n_header_params + def->n_body_params};
}

static e1_ptarget res_target(const odin3_elab1 *e1, const odin3_elab_res *res, uint32_t name) {
    if (res->kind != ODIN3_RES_CELLTYPE) {
        return (e1_ptarget){"module", odin3_elab1_str(e1, name), E1_NONE, NULL, E1_NONE};
    }
    const odin3_celltype_def *type = odin3_celltype_get(e1->design, res->type);
    return (e1_ptarget){"cell type", type->name, E1_NONE, type, type->n_params};
}

/* The ordered/named form of the CONNECTION entries; the mixing error at the first entry whose
 * form differs from the first's. */
static odin3_status param_forms(e1_pass *ps, odin3_ast_id item, odin3_ast_span entries,
                                bool *named) {
    const odin3_ast *ast = ps->e1->parsed;
    *named = entries.n > 0 && odin3_ast_name(ast, entries.ids[0]) != 0;
    for (uint32_t i = 1; i < entries.n; i++) {
        if ((odin3_ast_name(ast, entries.ids[i]) != 0) != *named) {
            odin3_ast_id first =
                odin3_ast_child(ast, item, odin3_ast_slot_count(ODIN3_AST_INSTANTIATION));
            odin3_elab1_error(ps->e1, odin3_ast_loc(ast, entries.ids[i]),
                              "parameter assignment of instance '%s' mixes ordered and named "
                              "values",
                              odin3_ast_name_str(ast, first));
            return ODIN3_ERR_PARSE;
        }
    }
    return ODIN3_OK;
}

/* The parameter the ordered value idx goes to (0: an implicit black box's stays unnamed). */
static odin3_status ordered_name(e1_pass *ps, const e1_ptarget *tg, uint32_t idx, uint32_t *name) {
    *name = 0;
    if (tg->type != NULL) {
        return odin3_design_intern(ps->e1->design, odin3_bytes_cstr(tg->type->params[idx].name),
                                   name);
    }
    if (tg->def != E1_NONE) {
        const odin3_elab_def *def = odin3_elab1_def(ps->e1, tg->def);
        *name = odin3_ast_name(ps->e1->parsed, *(const odin3_ast_id *)odin3_vec_cat(
                                                   &ps->e1->params, def->first_param + idx));
    }
    return ODIN3_OK;
}

/* One #() value: evaluated where it is written, for the target's parameter named param (0: an
 * ordered value of an implicit black box). */
static odin3_status param_value_of(e1_pass *ps, odin3_ast_id entry, uint32_t param,
                                   uint8_t source) {
    const odin3_ast *ast = ps->e1->parsed;
    odin3_ast_id expr = odin3_ast_child(ast, entry, 0);
    if (expr.v == 0) {
        return ODIN3_OK; /* `.W()` or an empty ordered value: no override */
    }
    odin3_elab_override ovr = {.path = param,
                               .source = source,
                               .order = entry.v,
                               .at = odin3_ast_loc(ast, entry),
                               .from = entry};
    odin3_elab1_eval_req req = {expr, param != 0 ? param_what(ps->env, param) : "parameter value",
                                &ovr.base};
    odin3_status st = odin3_elab1_eval(ps->env, &req, &ovr.value);
    ovr.value.flags &= (uint8_t)~ODIN3_CVAL_UNSIZED; /* an override is sized (v4.7) */
    return st != ODIN3_OK ? st : push_override(&ps->list, &ovr);
}

/* More ordered values than the target has parameters (§9.1), at the first surplus one. */
static odin3_status too_many(e1_pass *ps, const e1_ptarget *tg, odin3_ast_span entries) {
    if (tg->n_params == E1_NONE || entries.n <= tg->n_params) {
        return ODIN3_OK;
    }
    odin3_elab1_error(ps->e1, odin3_ast_loc(ps->e1->parsed, entries.ids[tg->n_params]),
                      "too many parameter values for %s '%s' (%" PRIu32 " given, %" PRIu32
                      " parameters)",
                      tg->what, tg->name, entries.n, tg->n_params);
    return ODIN3_ERR_PARSE;
}

/* #() entry idx (named, or ordered: the target's idx-th parameter) into ps->list. */
static odin3_status param_entry(e1_pass *ps, const e1_ptarget *tg, odin3_ast_id entry, uint32_t idx,
                                bool named) {
    uint32_t param = odin3_ast_name(ps->e1->parsed, entry);
    odin3_status st = named ? ODIN3_OK : ordered_name(ps, tg, idx, &param);
    return st != ODIN3_OK
               ? st
               : param_value_of(ps, entry, param, named ? ODIN3_OVR_NAMED : ODIN3_OVR_ORDERED);
}

/* The #() of item for target into ps->list: PARSE after mixing, too many values or a value that
 * is not constant. */
static odin3_status param_list(e1_pass *ps, odin3_ast_id item, const e1_ptarget *tg) {
    const odin3_ast *ast = ps->e1->parsed;
    odin3_vec_clear(&ps->list);
    odin3_ast_span entries = odin3_ast_children(ast, odin3_ast_child(ast, item, 0));
    bool named = false;
    odin3_status st = param_forms(ps, item, entries, &named);
    st = st != ODIN3_OK || named ? st : too_many(ps, tg, entries);
    odin3_status worst = st;
    for (uint32_t i = 0; st == ODIN3_OK && i < entries.n; i++) {
        odin3_status one = param_entry(ps, tg, entries.ids[i], i, named);
        st = one == ODIN3_ERR_PARSE ? ODIN3_OK : one;
        worst = worst != ODIN3_OK ? worst : one;
    }
    return st != ODIN3_OK ? st : worst;
}

/* --- instances ------------------------------------------------------------------------------ */

static odin3_status log_decision(e1_pass *ps, odin3_ast_id inst, int32_t value) {
    odin3_elab_decision *slot = odin3_vec_push(&ps->e1->log);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (odin3_elab_decision){inst, value};
    return ODIN3_OK;
}

static odin3_status add_edge(e1_pass *ps, odin3_ast_id inst, uint32_t child) {
    odin3_elab1_edge *slot = odin3_vec_push(&ps->e1->edges);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (odin3_elab1_edge){ps->spec, child, inst};
    return ODIN3_OK;
}

/* The overrides of instance inst into odin3_elab1.inst_ovr: the #() values, then the defparams
 * and pending overrides that name it. */
static odin3_status instance_overrides(e1_pass *ps, uint32_t inst) {
    odin3_vec *out = &ps->e1->inst_ovr;
    odin3_vec_clear(out);
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < ps->list.len; i++) {
        st = push_override(out, odin3_vec_cat(&ps->list, i));
    }
    for (uint32_t at = first_dp(ps, inst); st == ODIN3_OK && at != 0;) {
        const e1_dp *dp = odin3_vec_cat(&ps->dps, at - 1);
        st = push_override(out, &dp->ovr);
        at = dp->next;
    }
    return st;
}

/* The instance cap (point (aa)) for the elements of desc's INSTANCE (an array's folded range), at
 * its INSTANTIATION or GATE_DECL. PARSE after a diagnostic. */
static odin3_status count_elements(e1_pass *ps, const odin3_elab_inst *desc) {
    odin3_elab1_array arr;
    odin3_status st = odin3_elab1_fold_array(ps->e1, desc->instance, &arr);
    if (st != ODIN3_OK) {
        return st;
    }
    return odin3_elab1_count_instances(ps->e1, desc->instantiation, arr.n) ? ODIN3_OK
                                                                           : ODIN3_ERR_PARSE;
}

/* One INSTANCE (an array: all its elements) of a module or `(* blackbox *)` definition: its
 * specialization (EL-9), logged with an edge. */
static odin3_status module_instance(e1_pass *ps, odin3_ast_id item, odin3_ast_id inst,
                                    uint32_t target) {
    odin3_elab_inst desc = {item, inst, 0, {ODIN3_RES_MODULE, target, {0}}, E1_NONE};
    odin3_status st = count_elements(ps, &desc);
    st = st != ODIN3_OK ? st : instance_overrides(ps, odin3_ast_name(ps->e1->parsed, inst));
    if (st != ODIN3_OK) {
        return st == ODIN3_ERR_PARSE ? log_decision(ps, inst, ODIN3_DECISION_SKIPPED) : st;
    }
    odin3_elab_spec_req req = {target, ps->e1->inst_ovr.data, (uint32_t)ps->e1->inst_ovr.len,
                               odin3_elab1_spec_at(ps->e1, ps->spec)->depth + 1};
    ps->e1->creating = (odin3_elab1_creating){inst, item, ps->def};
    uint32_t child = 0;
    st = odin3_elab1_spec_get(ps->e1, &req, &child);
    if (st == ODIN3_ERR_PARSE) {
        return log_decision(ps, inst, ODIN3_DECISION_SKIPPED);
    }
    st = st != ODIN3_OK ? st : log_decision(ps, inst, (int32_t)child);
    return st != ODIN3_OK ? st : add_edge(ps, inst, child);
}

/* Every INSTANCE of item logged with value. */
static odin3_status log_all(e1_pass *ps, odin3_ast_id item, int32_t value) {
    odin3_ast_span kids = odin3_ast_children(ps->e1->parsed, item);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_INSTANTIATION); st == ODIN3_OK && i < kids.n;
         i++) {
        st = log_decision(ps, kids.ids[i], value);
    }
    return st;
}

static odin3_status module_instances(e1_pass *ps, odin3_ast_id item, uint32_t target) {
    e1_ptarget tg = def_target(ps->e1, target);
    odin3_status st = param_list(ps, item, &tg);
    if (st == ODIN3_ERR_PARSE) {
        return log_all(ps, item, ODIN3_DECISION_SKIPPED);
    }
    odin3_ast_span kids = odin3_ast_children(ps->e1->parsed, item);
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_INSTANTIATION);
         st == ODIN3_OK && i < kids.n && !ps->e1->stopped; i++) {
        st = module_instance(ps, item, kids.ids[i], target);
    }
    return st;
}

/* One INSTANCE of a cell type or an implicit black box (EL-11): its ports (implicit) and its
 * parameter record, logged by the record. */
static odin3_status given_instance(e1_pass *ps, odin3_ast_id item, odin3_ast_id inst,
                                   const odin3_elab_res *res) {
    odin3_elab1 *e1 = ps->e1;
    odin3_elab_inst desc = {item, inst, 0, *res, E1_NONE};
    odin3_status st = count_elements(ps, &desc);
    st = st != ODIN3_OK ? st : instance_overrides(ps, odin3_ast_name(e1->parsed, inst));
    if (st == ODIN3_OK && res->kind == ODIN3_RES_IMPLICIT_BLACKBOX) {
        st = odin3_elab1_auto_blackbox(e1, &desc);
    }
    st = st != ODIN3_OK ? st : odin3_elab1_celltype_params(e1, &desc);
    if (st == ODIN3_ERR_PARSE) {
        return log_decision(ps, inst, ODIN3_DECISION_SKIPPED);
    }
    return st != ODIN3_OK
               ? st
               : log_decision(ps, inst, odin3_elab1_given_decision((uint32_t)e1->given.len - 1));
}

static odin3_status given_instances(e1_pass *ps, odin3_ast_id item, const odin3_elab_res *res) {
    e1_ptarget tg = res_target(ps->e1, res, odin3_ast_name(ps->e1->parsed, item));
    odin3_status st = param_list(ps, item, &tg);
    if (st == ODIN3_ERR_PARSE) {
        return log_all(ps, item, ODIN3_DECISION_SKIPPED);
    }
    odin3_ast_span kids = odin3_ast_children(ps->e1->parsed, item);
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_INSTANTIATION);
         st == ODIN3_OK && i < kids.n && !ps->e1->stopped; i++) {
        st = given_instance(ps, item, kids.ids[i], res);
    }
    return st;
}

/* One INSTANTIATION of the module scope, resolved (EL-6) and specialized. */
static odin3_status instantiation(e1_pass *ps, odin3_ast_id item) {
    odin3_elab1 *e1 = ps->e1;
    uint32_t name = odin3_ast_name(e1->parsed, item);
    odin3_elab_res res = odin3_elab1_resolve(e1, name, odin3_elab1_def(e1, ps->def)->library);
    switch (res.kind) {
    case ODIN3_RES_MODULE:
        if (odin3_elab1_reach_def(e1, res.def)) {
            return module_instances(ps, item, res.def);
        }
        return log_all(ps, item, ODIN3_DECISION_SKIPPED);
    case ODIN3_RES_CELLTYPE:
    case ODIN3_RES_IMPLICIT_BLACKBOX:
        return given_instances(ps, item, &res);
    case ODIN3_RES_UDP:
        odin3_elab1_reject_def(e1, res.def);
        return log_all(ps, item, ODIN3_DECISION_SKIPPED);
    default:
        odin3_elab1_error(e1, odin3_ast_loc(e1->parsed, item),
                          "unresolved_module: module '%s' is not defined",
                          odin3_elab1_str(e1, name));
        return log_all(ps, item, ODIN3_DECISION_SKIPPED);
    }
}

/* The instance cap over a GATE_DECL's instances (an array's elements each count). */
static odin3_status gate_counts(e1_pass *ps, odin3_ast_id item) {
    odin3_ast_span kids = odin3_ast_children(ps->e1->parsed, item);
    odin3_status worst = ODIN3_OK;
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_GATE_DECL);
         !e1_internal(worst) && i < kids.n && !ps->e1->stopped; i++) {
        odin3_elab_inst desc = {item, kids.ids[i], 0, {ODIN3_RES_NONE, 0, {0}}, E1_NONE};
        worst = e1_keep_internal(worst, count_elements(ps, &desc));
    }
    return worst;
}

/* Every declaration evaluated (EL-23: ranges folded; EL-16: values), in declaration order. */
static odin3_status evaluate_all(odin3_elab1_env *env) {
    odin3_status worst = ODIN3_OK;
    for (uint32_t i = 0; i < env->slots.len && !e1_internal(worst) && !env->e1->stopped; i++) {
        const odin3_elab1_slot *slot = odin3_elab1_env_slot(env, i);
        worst = e1_keep_internal(worst, odin3_elab1_env_value(env, i));
        uint32_t unpacked = slot->entry.kind == ODIN3_ENTRY_SIGNAL ? slot->n_unpacked : 0;
        odin3_ast_id decl = odin3_elab1_env_slot(env, i)->entry.decl;
        for (uint32_t k = 1; k <= unpacked && !e1_internal(worst); k++) {
            int64_t msb = 0;
            int64_t lsb = 0;
            odin3_ast_id range = odin3_ast_child(env->e1->parsed, decl, k);
            worst = e1_keep_internal(worst, odin3_elab1_env_range(env, range, &msb, &lsb));
        }
    }
    return worst;
}

void odin3_elab1_env_preload(odin3_elab1_env *env, uint32_t spec_idx) {
    const odin3_elab1_spec *spec = odin3_elab1_spec_at(env->e1, spec_idx);
    const odin3_elab_def *def = odin3_elab1_def(env->e1, spec->def);
    for (uint32_t k = 0; k < spec->n_values; k++) {
        odin3_ast_id decl =
            *(const odin3_ast_id *)odin3_vec_cat(&env->e1->params, def->first_param + k);
        uint32_t idx = odin3_elab1_env_find(env, 0, odin3_ast_name(env->e1->parsed, decl));
        if (idx != E1_NONE) {
            odin3_elab1_slot *slot = odin3_elab1_env_slot(env, idx);
            slot->overridden = true;
            slot->ovr = (odin3_elab_override){
                .source = ODIN3_OVR_PROJECT, .value = spec->values[k], .base = spec->bases[k]};
        }
    }
}

static odin3_status expand_items(e1_pass *ps) {
    odin3_status st = evaluate_all(ps->env);
    st = st != ODIN3_OK ? st : collect_overrides(ps);
    odin3_ast_span kids = odin3_ast_children(ps->e1->parsed, ps->env->module);
    for (uint32_t i = E1_ITEMS; st == ODIN3_OK && i < kids.n && !ps->e1->stopped; i++) {
        odin3_ast_kind kind = odin3_ast_kind_of(ps->e1->parsed, kids.ids[i]);
        if (kind == ODIN3_AST_INSTANTIATION) {
            st = instantiation(ps, kids.ids[i]);
        } else if (kind == ODIN3_AST_GATE_DECL) {
            st = gate_counts(ps, kids.ids[i]);
        }
    }
    return st;
}

odin3_status odin3_elab1_expand(odin3_elab1 *e1, uint32_t spec) {
    odin3_elab1_env_destroy(e1->env);
    e1->env = NULL;
    e1_pass ps = {.e1 = e1, .spec = spec, .def = odin3_elab1_spec_at(e1, spec)->def};
    odin3_vec_init(&ps.dps, sizeof(e1_dp));
    odin3_vec_init(&ps.list, sizeof(odin3_elab_override));
    odin3_vec_clear(&e1->given); /* the parameter records pass W of this specialization reads */
    odin3_vec_clear(&e1->gparams);
    ps.dp_by_inst = odin3_u64map_create(0);
    odin3_status st =
        ps.dp_by_inst != NULL ? odin3_elab1_env_create(e1, ps.def, &e1->env) : ODIN3_ERR_NO_MEMORY;
    size_t first_log = e1->log.len;
    size_t first_edge = e1->edges.len;
    if (st == ODIN3_OK) {
        ps.env = e1->env;
        odin3_elab1_env_preload(ps.env, spec);
        st = expand_items(&ps);
    }
    odin3_elab1_spec *rec = odin3_vec_at(&e1->specs, spec);
    rec->first_log = (uint32_t)first_log;
    rec->n_log = (uint32_t)(e1->log.len - first_log);
    rec->first_edge = (uint32_t)first_edge;
    rec->n_edges = (uint32_t)(e1->edges.len - first_edge);
    odin3_vec_free(&ps.dps);
    odin3_u64map_destroy(ps.dp_by_inst);
    odin3_vec_free(&ps.list);
    return st;
}
