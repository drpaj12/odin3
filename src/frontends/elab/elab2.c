/*
 * elab2.c — stage 2's context and driver (2D Task 17; EL-2 steps 4–5 and 7, EL-28's order, EL-44,
 * EL-46): the context with its cached cell types, the error cap and the located diagnostics, point
 * (l)'s pending report, the byte budget, the module loop (bind, ports of every module, then every
 * body in EL-28's order: wires, the pre-scan, instances and gates, assignments, wired nets and the
 * end-of-module warnings), the top and the summary line. Ports, wires, provenance and attributes
 * are elab2_wires.c; drivers are elab2_claim.c, elab2_wired.c, elab2_prescan.c, elab2_warn.c and
 * elab2_tri.c, instances and gates elab2_inst.c.
 */
#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ast/symtab.h"
#include "frontends/elab/ceval.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_internal.h"
#include "frontends/elab/netlist_locs.h"
#include "frontends/elab/elab_test.h"
#include "frontends/elab/pg.h"
#include "ir/design.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/clock.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* EL-46's per-object estimate of the IR (bytes) and how often the budget is looked at (objects). */
enum {
    E2_BYTES_PER_NODE = 48,
    E2_BYTES_PER_PIN = 28,
    E2_BYTES_PER_NET = 48,
    E2_BYTES_PER_WIRE = 64,
    E2_BUDGET_EVERY = 1 << 16,
    E2_HASH_DIGITS = 16 /* the hex digits of a hashed module name (EL-9) */
};

/* The built-in types by name, in odin3_elab2_type order. */
static const char *const k_type_names[ODIN3_E2T_COUNT] = {
    [ODIN3_E2T_ADD] = "$add",
    [ODIN3_E2T_SUB] = "$sub",
    [ODIN3_E2T_MUL] = "$mul",
    [ODIN3_E2T_DIV] = "$div",
    [ODIN3_E2T_MOD] = "$mod",
    [ODIN3_E2T_POW] = "$pow",
    [ODIN3_E2T_AND] = "$and",
    [ODIN3_E2T_OR] = "$or",
    [ODIN3_E2T_XOR] = "$xor",
    [ODIN3_E2T_XNOR] = "$xnor",
    [ODIN3_E2T_NOT] = "$not",
    [ODIN3_E2T_NEG] = "$neg",
    [ODIN3_E2T_SHL] = "$shl",
    [ODIN3_E2T_SHR] = "$shr",
    [ODIN3_E2T_SSHR] = "$sshr",
    [ODIN3_E2T_SHIFTX] = "$shiftx",
    [ODIN3_E2T_EQ] = "$eq",
    [ODIN3_E2T_NE] = "$ne",
    [ODIN3_E2T_LT] = "$lt",
    [ODIN3_E2T_LE] = "$le",
    [ODIN3_E2T_GT] = "$gt",
    [ODIN3_E2T_GE] = "$ge",
    [ODIN3_E2T_REDUCE_AND] = "$reduce_and",
    [ODIN3_E2T_REDUCE_OR] = "$reduce_or",
    [ODIN3_E2T_REDUCE_XOR] = "$reduce_xor",
    [ODIN3_E2T_REDUCE_XNOR] = "$reduce_xnor",
    [ODIN3_E2T_REDUCE_BOOL] = "$reduce_bool",
    [ODIN3_E2T_LOGIC_NOT] = "$logic_not",
    [ODIN3_E2T_LOGIC_AND] = "$logic_and",
    [ODIN3_E2T_LOGIC_OR] = "$logic_or",
    [ODIN3_E2T_MUX] = "$mux",
    [ODIN3_E2T_TRIBUF] = "$tribuf",
    [ODIN3_E2T_PMUX] = "$pmux",
    [ODIN3_E2T_MEM] = "$mem",
    [ODIN3_E2T_PROC] = "$proc",
    [ODIN3_E2T_CONST0] = "$_CONST0_",
    [ODIN3_E2T_CONST1] = "$_CONST1_",
    [ODIN3_E2T_CONSTX] = "$_CONSTX_",
    [ODIN3_E2T_CONSTZ] = "$_CONSTZ_",
};

const char *odin3_elab2_str_of(const odin3_elab2 *e2, uint32_t id) {
    const char *str = odin3_strtab_get(odin3_design_strtab(e2->design), id);
    return str != NULL ? str : "";
}

odin3_status odin3_elab2_pins_cap(odin3_elab2 *e2, odin3_loc loc, uint64_t pins) {
    if (pins <= e2->limits->max_cell_pins) {
        return ODIN3_OK;
    }
    odin3_elab2_fatal(e2, loc, "cell of %" PRIu64 " pins exceeds the limit of %" PRIu32 " pins",
                      pins, e2->limits->max_cell_pins);
    return ODIN3_ERR_PARSE;
}

/* --- the context ----------------------------------------------------------------------------- */

odin3_celltype_id odin3_elab2_type_id(const odin3_elab2 *e2, odin3_elab2_type type) {
    if (e2 == NULL || (unsigned)type >= (unsigned)ODIN3_E2T_COUNT) {
        return (odin3_celltype_id){0};
    }
    return e2->types[type];
}

/* Looks every built-in type up once (review M20); a missing one is a 2D bug. */
static odin3_status cache_types(odin3_elab2 *e2) {
    for (unsigned i = 0; i < (unsigned)ODIN3_E2T_COUNT; i++) {
        uint32_t name = 0;
        odin3_status st = odin3_design_intern(e2->design, odin3_bytes_cstr(k_type_names[i]), &name);
        if (st != ODIN3_OK) {
            return st;
        }
        if (!odin3_celltype_find(e2->design, name, &e2->types[i])) {
            odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_create: cell type '%s' is not registered",
                      k_type_names[i]);
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    return ODIN3_OK;
}

static bool create_args_ok(const odin3_pass_ctx *ctx, const odin3_ast *elab, odin3_elab2 **out) {
    if (ctx == NULL || ctx->design == NULL || elab == NULL || out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_create: NULL argument");
        return false;
    }
    if (odin3_ast_form_of(elab) != ODIN3_AST_FORM_ELABORATED ||
        odin3_ast_design(elab) != ctx->design) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_create: not an elaborated store of this design");
        return false;
    }
    return true;
}

odin3_status odin3_elab2_create(odin3_pass_ctx *ctx, odin3_ast *elab, const odin3_elab_opts *opts,
                                odin3_elab2 **out) {
    if (out != NULL) {
        *out = NULL;
    }
    if (!create_args_ok(ctx, elab, out)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_elab2 *e2 = odin3_util_calloc(sizeof *e2);
    if (e2 == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    e2->ctx = ctx;
    e2->design = ctx->design;
    e2->elab = elab;
    e2->opts = opts != NULL ? *opts : (odin3_elab_opts){0};
    odin3_netlist_begin(e2);
    e2->limits = odin3_elab_limits_get();
    e2->fatal = ODIN3_OK;
    e2->iterations = e2->opts.gen_iters;
    e2->next_check = E2_BUDGET_EVERY;
    odin3_strbuf_init(&e2->text);
    odin3_vec_init(&e2->built, sizeof(odin3_module_id));
    odin3_vec_init(&e2->stack, sizeof(odin3_elab2_item));
    odin3_vec_init(&e2->chain, sizeof(odin3_scope_id));
    odin3_vec_init(&e2->vals, sizeof(odin3_sig));
    odin3_vec_init(&e2->mems, sizeof(odin3_elab_mem));
    e2->modules = odin3_u64map_create(0);
    e2->warned = odin3_u64map_create(0);
    e2->scratch = odin3_arena_create(0);
    e2->bboxes = odin3_u64map_create(0);
    odin3_status st =
        e2->modules != NULL && e2->warned != NULL && e2->scratch != NULL && e2->bboxes != NULL
            ? cache_types(e2)
            : ODIN3_ERR_NO_MEMORY;
    if (st != ODIN3_OK) {
        odin3_elab2_destroy(e2);
        return st;
    }
    *out = e2;
    return ODIN3_OK;
}

void odin3_elab2_destroy(odin3_elab2 *e2) {
    if (e2 == NULL) {
        return;
    }
    odin3_elab_binding_free(&e2->bind);
    odin3_elab2_drive_free(e2);
    odin3_elab2_mem_free(e2);
    odin3_elab2_pg_free(e2);
    odin3_u64map_destroy(e2->bboxes);
    odin3_u64map_destroy(e2->modules);
    odin3_u64map_destroy(e2->warned);
    odin3_arena_destroy(e2->scratch);
    odin3_vec_free(&e2->built);
    odin3_vec_free(&e2->stack);
    odin3_vec_free(&e2->chain);
    odin3_vec_free(&e2->frames);
    odin3_vec_free(&e2->vals);
    odin3_ctype_destroy(e2->ty);
    odin3_ceval_destroy(e2->ev);
    odin3_u64map_destroy(e2->bound);
    odin3_strbuf_free(&e2->text);
    odin3_util_free(e2);
}

bool odin3_elab2_stopped(const odin3_elab2 *e2) {
    return e2 == NULL || e2->stopped;
}

bool odin3_elab2_is_blackbox(const odin3_elab2 *e2, odin3_ast_id module) {
    uint32_t key = 0;
    if (!odin3_strtab_find(odin3_design_strtab(e2->design), odin3_bytes_cstr("blackbox"), &key)) {
        return false;
    }
    odin3_ast_span attrs = odin3_ast_attrs(e2->elab, module);
    for (uint32_t i = 0; i < attrs.n; i++) {
        if (odin3_ast_name(e2->elab, attrs.ids[i]) == key) {
            return true;
        }
    }
    return false;
}

/* Whether module carries the value-less attribute named text. */
static bool module_has_attr(const odin3_elab2 *e2, odin3_ast_id module, const char *text) {
    uint32_t key = 0;
    if (!odin3_strtab_find(odin3_design_strtab(e2->design), odin3_bytes_cstr(text), &key)) {
        return false;
    }
    odin3_ast_span attrs = odin3_ast_attrs(e2->elab, module);
    for (uint32_t i = 0; i < attrs.n; i++) {
        if (odin3_ast_name(e2->elab, attrs.ids[i]) == key) {
            return true;
        }
    }
    return false;
}

/* The options of the module: those of its `--lib` read when it came from one (stage 1 marked it
 * `odin3_lib`, its flags `odin3_lib_nolatches`, `odin3_lib_nomem2reg` and
 * `odin3_lib_vtr_compat`), else the run's. */
static void module_options(odin3_elab2 *e2, odin3_ast_id module) {
    e2->mod_nolatches = e2->opts.nolatches;
    e2->mod_nomem2reg = e2->opts.nomem2reg;
    e2->mod_vtr_compat = e2->opts.vtr_compat;
    if (module_has_attr(e2, module, "odin3_lib")) {
        e2->mod_nolatches = module_has_attr(e2, module, "odin3_lib_nolatches");
        e2->mod_nomem2reg = module_has_attr(e2, module, "odin3_lib_nomem2reg");
        e2->mod_vtr_compat = module_has_attr(e2, module, "odin3_lib_vtr_compat");
    }
}

odin3_status odin3_elab2_enter(odin3_elab2 *e2, odin3_ast_id module_node) {
    uint64_t id = 0;
    if (!odin3_u64map_get(e2->modules, module_node.v, &id)) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2: the module of node %" PRIu32 " was not made",
                  module_node.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_arena *fresh = odin3_arena_create(0); /* the previous module's signals are dead */
    if (fresh == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_arena_destroy(e2->scratch);
    e2->scratch = fresh;
    e2->mod_id = (odin3_module_id){(uint32_t)id};
    e2->mod = odin3_module_get(e2->design, e2->mod_id);
    e2->mod_node = module_node;
    module_options(e2, module_node);
    e2->scope = (odin3_scope_id){0};
    e2->mod_name = odin3_ast_name(e2->elab, module_node);
    memset(e2->const_net, 0, sizeof e2->const_net);
    odin3_elab2_drive_reset(e2);
    odin3_elab2_mem_reset(e2);
    odin3_elab2_pg_free(e2);
    return ODIN3_OK;
}

/* --- diagnostics ----------------------------------------------------------------------------- */

/* One message at loc (unlocated when 0) in the design's --diag-paths style. */
static void vreport(odin3_elab2 *e2, odin3_log_level level, odin3_loc loc, const char *fmt,
                    va_list args) ODIN3_PRINTF(4, 0);

static void vreport(odin3_elab2 *e2, odin3_log_level level, odin3_loc loc, const char *fmt,
                    va_list args) {
    if (loc.v != 0) {
        odin3_vdiag(e2->design, level, loc, fmt, args);
        return;
    }
    char text[ODIN3_LOG_BUF];
    if (vsnprintf(text, sizeof text, fmt, args) < 0) {
        text[0] = '\0';
    }
    odin3_log(level, "%s", text);
}

/* True when one more error may be logged (and counts it); at the cap the stop line instead. */
static bool error_slot(odin3_elab2 *e2, odin3_loc loc) {
    if (e2->stopped) {
        return false;
    }
    if (e2->errors < e2->limits->max_errors) {
        e2->errors++;
        return true;
    }
    e2->stopped = true;
    if (loc.v != 0) {
        odin3_diag(e2->design, ODIN3_LOG_ERROR, loc,
                   "too many errors; elaboration stopped after %" PRIu32, e2->limits->max_errors);
    } else {
        odin3_log(ODIN3_LOG_ERROR, "too many errors; elaboration stopped after %" PRIu32,
                  e2->limits->max_errors);
    }
    return false;
}

void odin3_elab2_error(odin3_elab2 *e2, odin3_loc loc, const char *fmt, ...) {
    if (!error_slot(e2, loc)) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vreport(e2, ODIN3_LOG_ERROR, loc, fmt, args);
    va_end(args);
}

void odin3_elab2_fatal(odin3_elab2 *e2, odin3_loc loc, const char *fmt, ...) {
    if (error_slot(e2, loc)) {
        va_list args;
        va_start(args, fmt);
        vreport(e2, ODIN3_LOG_ERROR, loc, fmt, args);
        va_end(args);
    }
    e2->stopped = true;
}

void odin3_elab2_count_parse(odin3_elab2 *e2, odin3_loc loc) {
    if (e2->errors == 0 && !e2->stopped) {
        odin3_elab2_error(e2, loc,
                          "internal error: a construct failed without a located error (a 2D bug)");
    }
}

/* Marks a warning key as given: true when it was new (false after a map failure, latched). */
static bool warn_new(odin3_elab2 *e2, uint64_t key) {
    if (odin3_u64map_get(e2->warned, key, NULL)) {
        return false;
    }
    odin3_status st = odin3_u64map_put(e2->warned, (odin3_kv){key, 1});
    if (st != ODIN3_OK) {
        e2->fatal = e2->fatal != ODIN3_OK ? e2->fatal : st;
        return false;
    }
    return true;
}

void odin3_elab2_warn(odin3_elab2 *e2, odin3_loc loc, const char *fmt, ...) {
    /* Warn once per (loc, format) (point (v)); a 64-bit hash collision would drop one warning. */
    uint64_t key =
        odin3_hash_combine(odin3_hash_bytes(odin3_bytes_cstr(fmt), ODIN3_HASH_SEED), loc.v);
    if (!warn_new(e2, key)) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    vreport(e2, ODIN3_LOG_WARN, loc, fmt, args);
    va_end(args);
}

void odin3_elab2_warn_msg(odin3_elab2 *e2, odin3_loc loc, const char *msg) {
    uint64_t key =
        odin3_hash_combine(odin3_hash_bytes(odin3_bytes_cstr(msg), ODIN3_HASH_SEED), loc.v);
    if (!warn_new(e2, key)) {
        return;
    }
    if (loc.v != 0) {
        odin3_diag(e2->design, ODIN3_LOG_WARN, loc, "%s", msg);
    } else {
        odin3_log(ODIN3_LOG_WARN, "%s", msg);
    }
}

void odin3_elab2_pend_feature(odin3_elab2 *e2, odin3_loc loc, uint32_t feature) {
    if (feature >= ODIN3_PEND_END || e2->pend_seen[feature] || e2->stopped) {
        return;
    }
    if (error_slot(e2, loc)) {
        odin3_elab_pend_report(e2->design, loc, feature, e2->pend_seen);
    }
}

bool odin3_elab2_pending(odin3_elab2 *e2, odin3_ast_id node) {
    uint32_t kind = (uint32_t)odin3_ast_kind_of(e2->elab, node);
    if (!odin3_elab_pending(ODIN3_ELAB_STAGE2, kind)) {
        return false;
    }
    odin3_elab2_pend_feature(e2, odin3_ast_loc(e2->elab, node), kind);
    return true;
}

/* A kind neither pending nor handled: a 2D bug (the stage-1 counterpart is odin3_elab1_unhandled).
 */
static void unhandled(odin3_elab2 *e2, odin3_ast_id node) {
    odin3_diag(e2->design, ODIN3_LOG_ERROR, odin3_ast_loc(e2->elab, node),
               "stage 2: no elaboration of '%s' (a 2D bug)",
               odin3_ast_kind_name(odin3_ast_kind_of(e2->elab, node)));
    e2->fatal = e2->fatal != ODIN3_OK ? e2->fatal : ODIN3_ERR_CHECK;
}

/* --- caps and the byte budget (EL-46) -------------------------------------------------------- */

odin3_status odin3_elab2_reserve_nets(odin3_elab2 *e2, uint32_t n, odin3_loc at) {
    if (e2->net_bits + n > e2->limits->max_net_bits) {
        odin3_elab2_fatal(e2, at, "elaboration needs more than %" PRIu32 " net bits",
                          e2->limits->max_net_bits);
        return ODIN3_ERR_PARSE;
    }
    e2->net_bits += n;
    return ODIN3_OK;
}

static uint64_t add_saturating(uint64_t lhs, uint64_t rhs) {
    return lhs > UINT64_MAX - rhs ? UINT64_MAX : lhs + rhs;
}

/* Bytes of every store the design holds, plus the elaborated one when it does not hold it yet. */
static uint64_t ast_bytes(const odin3_elab2 *e2) {
    uint64_t held = 0;
    bool has_elab = false;
    uint32_t stores = odin3_design_ast_count(e2->design);
    for (uint32_t i = 0; i < stores; i++) {
        const odin3_ast *ast = odin3_design_ast_at(e2->design, i);
        has_elab |= ast == e2->elab;
        held = add_saturating(held, odin3_ast_bytes_reserved(ast));
    }
    return has_elab ? held : add_saturating(held, odin3_ast_bytes_reserved(e2->elab));
}

odin3_status odin3_elab2_budget(odin3_elab2 *e2) {
    if (add_saturating(ast_bytes(e2), e2->est_bytes) <= e2->limits->max_bytes) {
        return ODIN3_OK;
    }
    odin3_elab2_fatal(e2, (odin3_loc){0},
                      "elaboration needs more than %" PRIu64
                      " bytes (the budget is %d%% of physical memory)",
                      e2->limits->max_bytes, ODIN3_ELAB_MAX_BYTES_PERCENT);
    return ODIN3_ERR_PARSE;
}

odin3_status odin3_elab2_account(odin3_elab2 *e2, const odin3_elab2_counts *made) {
    e2->est_bytes +=
        (uint64_t)made->nodes * E2_BYTES_PER_NODE + (uint64_t)made->pins * E2_BYTES_PER_PIN +
        (uint64_t)made->nets * E2_BYTES_PER_NET + (uint64_t)made->wires * E2_BYTES_PER_WIRE;
    e2->objects += (uint64_t)made->nodes + made->pins + made->nets + made->wires;
    if (e2->objects < e2->next_check) {
        return ODIN3_OK;
    }
    e2->next_check = e2->objects - e2->objects % E2_BUDGET_EVERY + E2_BUDGET_EVERY;
    return odin3_elab2_budget(e2);
}

/* --- one module's body (EL-28's order) ------------------------------------------------------- */

/* Pushes the items of GEN_BLOCK or MODULE node (its tail after nslots slots), in the scope they
 * are in, so the first pops first. */
odin3_status odin3_elab2_push_items(odin3_elab2 *e2, odin3_ast_id node, uint32_t nslots,
                                    odin3_scope_id scope) {
    odin3_ast_span kids = odin3_ast_children(e2->elab, node);
    for (uint32_t i = kids.n; i > nslots; i--) {
        odin3_elab2_item *slot = odin3_vec_push(&e2->stack);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = (odin3_elab2_item){kids.ids[i - 1], scope};
    }
    return ODIN3_OK;
}

/* A located error is counted and the walk goes on with the next item; anything else (memory, a
 * fatal cap's stop, a 2D bug) ends it. A PARSE with no error counted at all is counted here as an
 * internal error, so the read can never succeed with the item dropped. */
static odin3_status carry_on(odin3_elab2 *e2, odin3_loc loc, odin3_status st) {
    if (st != ODIN3_ERR_PARSE) {
        return st;
    }
    odin3_elab2_count_parse(e2, loc);
    return ODIN3_OK;
}

/* One net assignment (a continuous assignment or a declarator's initialiser), carried on. */
static odin3_status assign_one(odin3_elab2 *e2, odin3_ast_id node) {
    return carry_on(e2, odin3_ast_loc(e2->elab, node), odin3_elab2_assign(e2, node));
}

/* One declarator's initialiser: a net's is a continuous assignment (Task 18); a variable's is an
 * initial value (Task 24), which odin3_elab2_initials runs in its own step. The symbol says
 * which. */
static odin3_status declarator_initialiser(odin3_elab2 *e2, odin3_ast_id declarator) {
    const odin3_symbol *rec = odin3_symbol_get(e2->st, odin3_elab_sym_of(&e2->bind, declarator));
    if (rec != NULL && rec->kind == ODIN3_SYM_VAR) {
        return ODIN3_OK;
    }
    if (odin3_elab_pending(ODIN3_ELAB_STAGE2, (uint32_t)ODIN3_AST_NET_ASSIGN)) {
        odin3_elab2_pend_feature(e2, odin3_ast_loc(e2->elab, declarator),
                                 (uint32_t)ODIN3_AST_NET_ASSIGN);
        return ODIN3_OK;
    }
    return assign_one(e2, declarator);
}

/* A declaration's initialisers, read from the DECLARATOR in the same slot for a body declaration
 * and a port (ANSI or merged from a non-ANSI one). */
static odin3_status declaration_initialisers(odin3_elab2 *e2, odin3_ast_id decl) {
    odin3_ast_span kids = odin3_ast_children(e2->elab, decl);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < kids.n && !e2->stopped; i++) {
        if (kids.ids[i].v != 0 &&
            odin3_ast_kind_of(e2->elab, kids.ids[i]) == ODIN3_AST_DECLARATOR &&
            odin3_ast_child(e2->elab, kids.ids[i], 0).v != 0) {
            st = declarator_initialiser(e2, kids.ids[i]);
        }
    }
    return st;
}

/* The initialisers of the module's ports (its ports LIST is the second slot). */
static odin3_status port_initialisers(odin3_elab2 *e2, odin3_ast_id module_node) {
    odin3_ast_span ports = odin3_ast_children(e2->elab, odin3_ast_child(e2->elab, module_node, 1));
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < ports.n; i++) {
        st = declaration_initialisers(e2, ports.ids[i]);
    }
    return st;
}

/* The NET_ASSIGNs of a CONT_ASSIGN (its tail), each through odin3_elab2_assign. */
static odin3_status cont_assign(odin3_elab2 *e2, odin3_ast_id node) {
    odin3_ast_span kids = odin3_ast_children(e2->elab, node);
    odin3_status st = ODIN3_OK;
    odin3_netlist_assign_begin(e2, node); /* -netlist: the comment of this assign (D-3b) */
    for (uint32_t i = odin3_ast_slot_count(ODIN3_AST_CONT_ASSIGN);
         st == ODIN3_OK && i < kids.n && !e2->stopped; i++) {
        st = assign_one(e2, kids.ids[i]);
    }
    odin3_netlist_assign_end(e2);
    return st;
}

/* One instantiation or gate declaration through its emitter, carried on after a counted error. */
static odin3_status emit_one(odin3_elab2 *e2, odin3_ast_id item,
                             odin3_status (*emit)(odin3_elab2 *, odin3_ast_id)) {
    return carry_on(e2, odin3_ast_loc(e2->elab, item), emit(e2, item));
}

/* The two walks over the items, in EL-28's order: (4) instances and gates, then (5) the continuous
 * assignments (net declaration assignments included), with the kinds not landed yet. */
enum { PHASE_INSTANCES = 1, PHASE_ASSIGNS = 2, PHASE_PROCESSES = 3 };

/* One module item of the instance walk (4): generate blocks are descended, instances and gates
 * emitted; everything else waits for the assignment walk. */
static odin3_status instance_step(odin3_elab2 *e2, odin3_ast_id item) {
    switch (odin3_ast_kind_of(e2->elab, item)) {
    case ODIN3_AST_GEN_BLOCK:
        return odin3_elab2_pending(e2, item)
                   ? ODIN3_OK
                   : odin3_elab2_push_items(e2, item, 1, odin3_elab_scope_of(&e2->bind, item));
    case ODIN3_AST_INSTANTIATION:
        return emit_one(e2, item, odin3_elab2_instance);
    case ODIN3_AST_GATE_DECL:
        return emit_one(e2, item, odin3_elab2_gate);
    default:
        return ODIN3_OK;
    }
}

/* The kinds the assignment walk leaves to another step: instances and gates (step (4)), `always`
 * blocks (6), initial blocks (odin3_elab2_initials, after this walk), and the declarations that
 * only need their pending check. */
static bool assign_leaves(odin3_elab2 *e2, odin3_ast_id item) {
    switch (odin3_ast_kind_of(e2->elab, item)) {
    case ODIN3_AST_INSTANTIATION:
    case ODIN3_AST_GATE_DECL:
    case ODIN3_AST_ALWAYS:
        return true;
    case ODIN3_AST_INITIAL:
    case ODIN3_AST_PARAM_DECL:
    case ODIN3_AST_FUNCTION_DECL:
    case ODIN3_AST_TASK_DECL:
        (void)odin3_elab2_pending(e2, item); /* a test may force one */
        return true;
    default:
        return false;
    }
}

/* One module item of the assignment walk (5): net declaration assignments and continuous
 * assignments are emitted and every other kind that is still pending is reported. */
static odin3_status assign_step(odin3_elab2 *e2, odin3_ast_id item) {
    if (assign_leaves(e2, item)) {
        return ODIN3_OK;
    }
    switch (odin3_ast_kind_of(e2->elab, item)) {
    case ODIN3_AST_GEN_BLOCK:
        return odin3_elab2_pending(e2, item)
                   ? ODIN3_OK
                   : odin3_elab2_push_items(e2, item, 1, odin3_elab_scope_of(&e2->bind, item));
    case ODIN3_AST_NET_DECL:
    case ODIN3_AST_VAR_DECL:
        return odin3_elab2_pending(e2, item) ? ODIN3_OK : declaration_initialisers(e2, item);
    case ODIN3_AST_CONT_ASSIGN:
        return odin3_elab2_pending(e2, item) ? ODIN3_OK : cont_assign(e2, item);
    default:
        if (!odin3_elab2_pending(e2, item)) {
            unhandled(e2, item);
        }
        return ODIN3_OK;
    }
}

odin3_status odin3_elab2_pg_get(odin3_elab2 *e2, struct odin3_pg **out) {
    if (e2->pg == NULL) {
        odin3_status st = odin3_pg_create(e2, &e2->pg);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    *out = e2->pg;
    return ODIN3_OK;
}

bool odin3_elab2_is_bound(const odin3_elab2 *e2, odin3_sym_id sym) {
    return e2->bound != NULL && sym.v != 0 && odin3_u64map_get(e2->bound, sym.v, NULL);
}

/* One `always` block through the process generator, made on the first one of the module. */
static odin3_status always_step(odin3_elab2 *e2, odin3_ast_id always) {
    odin3_pg *pg = NULL;
    odin3_status st = odin3_elab2_pg_get(e2, &pg);
    if (st != ODIN3_OK) {
        return st;
    }
    return carry_on(e2, odin3_ast_loc(e2->elab, always), odin3_pg_always(e2->pg, always));
}

/* One module item of the process walk (6): generate blocks are descended, `always` blocks built. */
static odin3_status process_step(odin3_elab2 *e2, odin3_ast_id item) {
    switch (odin3_ast_kind_of(e2->elab, item)) {
    case ODIN3_AST_GEN_BLOCK:
        return odin3_elab2_pending(e2, item)
                   ? ODIN3_OK
                   : odin3_elab2_push_items(e2, item, 1, odin3_elab_scope_of(&e2->bind, item));
    case ODIN3_AST_ALWAYS:
        return always_step(e2, item);
    default:
        return ODIN3_OK;
    }
}

/* One module item of the walk of the given phase. */
static odin3_status phase_step(odin3_elab2 *e2, unsigned phase, odin3_ast_id item) {
    switch (phase) {
    case PHASE_INSTANCES:
        return instance_step(e2, item);
    case PHASE_PROCESSES:
        return process_step(e2, item);
    default:
        return assign_step(e2, item);
    }
}

/* One walk over the module's items. */
static odin3_status items_walk(odin3_elab2 *e2, odin3_ast_id module_node, unsigned phase) {
    odin3_vec_clear(&e2->stack);
    e2->scope = (odin3_scope_id){0};
    odin3_status st = phase == PHASE_ASSIGNS ? port_initialisers(e2, module_node) : ODIN3_OK;
    st = st != ODIN3_OK
             ? st
             : odin3_elab2_push_items(e2, module_node, odin3_ast_slot_count(ODIN3_AST_MODULE),
                                      (odin3_scope_id){0});
    while (st == ODIN3_OK && e2->stack.len > 0 && !e2->stopped) {
        odin3_elab2_item item = *(odin3_elab2_item *)odin3_vec_at(&e2->stack, e2->stack.len - 1);
        odin3_vec_pop(&e2->stack);
        e2->scope = item.scope;
        st = phase_step(e2, phase, item.node);
    }
    e2->scope = (odin3_scope_id){0};
    return st != ODIN3_OK ? st : e2->fatal;
}

/* Step (3) of EL-28 (EL-40), run after the assignments walk of step (5) so that the drivers of a
 * net are known (the spec's order puts it first; the results are the same). */
static odin3_status initial_values(odin3_elab2 *e2, odin3_ast_id module_node) {
    return carry_on(e2, odin3_ast_loc(e2->elab, module_node),
                    odin3_elab2_initials(e2, module_node));
}

/* Step (2): the pre-scan, the mem2reg decision and the words it made, then the test hook. */
static odin3_status prescans(odin3_elab2 *e2, odin3_ast_id module_node) {
    odin3_status st = odin3_elab2_prescan(e2, module_node);
    st = st != ODIN3_OK || e2->stopped ? st : odin3_elab2_mem_decide(e2, module_node);
    st = st != ODIN3_OK || e2->stopped ? st : odin3_elab2_prescan_words(e2, module_node);
    const odin3_elab_test_hooks *hooks = odin3_elab_test_hooks_get();
    if (st == ODIN3_OK && hooks->after_prescan != NULL) {
        hooks->after_prescan(hooks->user, e2, module_node);
    }
    return st;
}

/* Steps (4) to (8) and the end-of-module warnings: the instances and gates, the assignments, the
 * processes, the wired nets, the `$mem` cells. */
static odin3_status emissions(odin3_elab2 *e2, odin3_ast_id module_node) {
    odin3_status st = items_walk(e2, module_node, PHASE_INSTANCES);
    st = st != ODIN3_OK || e2->stopped ? st : items_walk(e2, module_node, PHASE_ASSIGNS);
    st = st != ODIN3_OK || e2->stopped ? st : initial_values(e2, module_node);
    st = st != ODIN3_OK || e2->stopped ? st : items_walk(e2, module_node, PHASE_PROCESSES);
    st = st != ODIN3_OK || e2->stopped ? st : odin3_elab2_wired(e2, module_node);
    st = st != ODIN3_OK || e2->stopped ? st : odin3_elab2_tri_logic(e2);
    st = st != ODIN3_OK || e2->stopped ? st : odin3_elab2_mem_emit(e2);
    return st != ODIN3_OK || e2->stopped ? st : odin3_elab2_undriven(e2);
}

static odin3_status steps_after_wires(odin3_elab2 *e2, odin3_ast_id module_node) {
    odin3_status st = prescans(e2, module_node);
    st = st != ODIN3_OK || e2->stopped ? st : emissions(e2, module_node);
    return st != ODIN3_OK || e2->stopped ? st : odin3_elab2_sweep_frames(e2);
}

odin3_status odin3_elab2_module(odin3_elab2 *e2, odin3_ast_id module_node) {
    if (odin3_ast_kind_of(e2->elab, module_node) != ODIN3_AST_MODULE) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab2_module: not a MODULE node");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (odin3_elab2_is_blackbox(e2, module_node)) {
        return ODIN3_OK; /* no body, no IR module: Task 19 declares its cell type */
    }
    odin3_status st = odin3_elab2_enter(e2, module_node);
    const odin3_elab_test_hooks *hooks = odin3_elab_test_hooks_get();
    if (st == ODIN3_OK && hooks->on_body_start != NULL) {
        hooks->on_body_start(hooks->user, module_node);
    }
    st = st != ODIN3_OK ? st : odin3_elab2_budget(e2);
    st = st != ODIN3_OK ? st : odin3_elab2_wires(e2, module_node);
    st = st != ODIN3_OK || e2->stopped ? st : steps_after_wires(e2, module_node);
    st = st != ODIN3_OK ? st : e2->fatal;
    return st == ODIN3_OK && e2->stopped ? ODIN3_ERR_PARSE : st;
}

/* --- the stage (EL-2 steps 4, 5 and 7) ------------------------------------------------------- */

/* The UNIT's MODULEs, in order: calls step on each until one fails or the stage stops. */
static odin3_status each_module(odin3_elab2 *e2,
                                odin3_status (*step)(odin3_elab2 *, odin3_ast_id)) {
    odin3_ast_span modules = odin3_ast_children(e2->elab, odin3_ast_root(e2->elab, 0));
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < modules.n && !e2->stopped; i++) {
        if (odin3_ast_kind_of(e2->elab, modules.ids[i]) == ODIN3_AST_MODULE) {
            st = step(e2, modules.ids[i]);
        }
    }
    return st;
}

/* Binds the store (EL-5) and makes the context's symbol table the store's. */
static odin3_status bind_store(odin3_elab2 *e2) {
    double started = odin3_now_ms();
    odin3_status st = odin3_elab_bind(e2->elab, &e2->bind);
    e2->st = odin3_ast_symtab(e2->elab);
    if (e2->opts.bind_ms_out != NULL) {
        *e2->opts.bind_ms_out = odin3_now_ms() - started;
    }
    return st;
}

/* Bind, the ports of every module, then every body (EL-2: ports before bodies). */
static odin3_status run_steps(odin3_elab2 *e2) {
    odin3_status st = bind_store(e2);
    st = st != ODIN3_OK ? st : each_module(e2, odin3_elab2_ports);
    st = st != ODIN3_OK ? st : each_module(e2, odin3_elab2_module);
    return st;
}

/* The counts of the summary line, over the modules this run made. */
typedef struct summary {
    uint32_t modules, specializations, processes, cells, nets;
} summary;

static void count_nodes(const odin3_elab2 *e2, const odin3_module *mod, summary *sum) {
    for (uint32_t node = 1; node < odin3_module_node_end(mod); node++) {
        if (!odin3_node_live(mod, (odin3_node_id){node})) {
            continue;
        }
        const odin3_celltype_def *def =
            odin3_celltype_get(e2->design, odin3_node_type(mod, (odin3_node_id){node}));
        if (def == NULL || def->gran == ODIN3_GRAN_PORT) {
            continue;
        }
        bool process = (def->flags & ODIN3_CT_PROCESS) != 0;
        sum->processes += process ? 1U : 0U;
        sum->cells += process ? 0U : 1U;
    }
}

/* The text after a '$' of a module name reads as a specialization's: a `P=v` entry before the next
 * '$' (`m$W=8`, `mid$u_leaf.W=4`), the `$2` of a name already taken, or `h` and 16 hex digits
 * (EL-9). An escaped identifier with a '$' in it (`a$b`) is none of them. */
static bool specialization_suffix(const char *seg) {
    size_t len = strcspn(seg, "$");
    if (memchr(seg, '=', len) != NULL) {
        return true;
    }
    if (seg[len] != '\0' || len == 0) {
        return false;
    }
    bool digits = true;
    for (size_t i = 0; i < len; i++) {
        digits = digits && seg[i] >= '0' && seg[i] <= '9';
    }
    bool hashed = len == 1U + E2_HASH_DIGITS && seg[0] == 'h';
    for (size_t i = 1; hashed && i < len; i++) {
        hashed = isxdigit((unsigned char)seg[i]) != 0;
    }
    return digits || hashed;
}

/* A specialization's name has one of those suffixes after a '$' (stage 1 marks nothing else). */
bool odin3_elab2_is_specialization(const char *name) {
    for (const char *at = name[0] != '\0' ? strchr(name + 1, '$') : NULL; at != NULL;
         at = strchr(at + 1, '$')) {
        if (specialization_suffix(at + 1)) {
            return true;
        }
    }
    return false;
}

static void count_module(const odin3_elab2 *e2, odin3_module_id id, summary *sum) {
    const odin3_module *mod = odin3_module_get(e2->design, id);
    const char *name = odin3_strtab_get(odin3_design_strtab(e2->design), odin3_module_name(mod));
    sum->modules++;
    sum->specializations += odin3_elab2_is_specialization(name) ? 1U : 0U;
    for (uint32_t net = 1; net < odin3_module_net_end(mod); net++) {
        sum->nets += odin3_net_live(mod, (odin3_net_id){net}) ? 1U : 0U;
    }
    count_nodes(e2, mod, sum);
}

/* EL-2 steps 5 and 7: the top is the UNIT's first MODULE; then the summary line. */
static odin3_status finish(odin3_elab2 *e2, double started) {
    odin3_ast_id unit = odin3_ast_root(e2->elab, 0);
    odin3_ast_id top = odin3_ast_child(e2->elab, unit, 0);
    uint64_t id = 0;
    if (odin3_ast_kind_of(e2->elab, top) != ODIN3_AST_MODULE ||
        !odin3_u64map_get(e2->modules, top.v, &id)) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, top.v != 0 ? top : unit),
                          "the top module '%s' has no IR module (a black box?)",
                          odin3_ast_name_str(e2->elab, unit));
        return ODIN3_ERR_PARSE;
    }
    odin3_netlist_finish(e2);
    odin3_status st = odin3_design_set_top(e2->design, (odin3_module_id){(uint32_t)id});
    summary sum = {0, 0, 0, 0, 0};
    for (size_t i = 0; st == ODIN3_OK && i < e2->built.len; i++) {
        count_module(e2, *(const odin3_module_id *)odin3_vec_cat(&e2->built, i), &sum);
    }
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO,
                  "read_verilog: elaborated top '%s': %" PRIu32 " modules (%" PRIu32
                  " specializations), %" PRIu32 " processes, %" PRIu32 " cells, %" PRIu32
                  " nets, %.3f + %.3f ms",
                  odin3_ast_name_str(e2->elab, unit), sum.modules, sum.specializations,
                  sum.processes, sum.cells, sum.nets, e2->opts.stage1_ms, odin3_now_ms() - started);
    }
    return st;
}

/* The status of a run that reached its end: a fatal error first, then the located ones. */
static odin3_status end_status(const odin3_elab2 *e2) {
    if (e2->fatal != ODIN3_OK) {
        return e2->fatal;
    }
    return e2->errors > 0 || e2->stopped ? ODIN3_ERR_PARSE : ODIN3_OK;
}

odin3_status odin3_elab_stage2(odin3_pass_ctx *ctx, odin3_ast *elab, const odin3_elab_opts *opts) {
    double started = odin3_now_ms();
    odin3_elab2 *e2 = NULL;
    odin3_status st = odin3_elab2_create(ctx, elab, opts, &e2);
    st = st != ODIN3_OK ? st : run_steps(e2);
    if (st == ODIN3_OK) {
        st = end_status(e2);
    }
    if (st == ODIN3_OK) {
        st = finish(e2, started); /* never after an error: the top stays unset (EL-2) */
    }
    odin3_elab2_destroy(e2);
    return st;
}
