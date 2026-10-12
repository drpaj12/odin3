/*
 * elab1.c — stage 1's run context and diagnostics (elab1.h), EL-2's entry odin3_elab_run
 * (elab.h) with its step 1 (the options), odin3_elab_stage1 (frontends/elab/elab.h) and, until
 * Task 11's elab1_write.c, the writer of the UNIT and its MODULE shells.
 */
#include "frontends/verilog/elab1.h"

#include "ast/ast.h"
#include "ast/check.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "frontends/elab/elab.h"
#include "frontends/elab/elab_test.h"
#include "frontends/project/project.h"
#include "frontends/verilog/elab.h"
#include "ir/check.h"
#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "passes/manager.h"
#include "util/alloc.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Debug builds check the elaborated store whatever the options say (EL-2 step 2). */
#ifdef NDEBUG
enum { E1_ALWAYS_CHECK = 0 };
#else
enum { E1_ALWAYS_CHECK = 1 };
#endif

enum { E1_KEY_SHIFT = 32 };

static const char k_work[] = "work";

/* --- diagnostics ---------------------------------------------------------------------------- */

/* One message at loc (unlocated when 0), in the design's --diag-paths style. */
static void emit(odin3_elab1 *e1, odin3_log_level level, odin3_loc loc, const char *fmt,
                 va_list args) ODIN3_PRINTF(4, 0);

static void emit(odin3_elab1 *e1, odin3_log_level level, odin3_loc loc, const char *fmt,
                 va_list args) {
    if (loc.v != 0) {
        odin3_vdiag(e1->design, level, loc, fmt, args);
        return;
    }
    char text[ODIN3_LOG_BUF];
    if (vsnprintf(text, sizeof text, fmt, args) < 0) {
        text[0] = '\0';
    }
    odin3_log(level, "%s", text);
}

/* True when one more error may be logged (and counts it); at the cap the stop line instead. */
static bool error_slot(odin3_elab1 *e1, odin3_loc loc) {
    if (e1->stopped) {
        return false;
    }
    if (e1->errors < e1->limits->max_errors) {
        e1->errors++;
        return true;
    }
    e1->stopped = true;
    if (loc.v != 0) {
        odin3_diag(e1->design, ODIN3_LOG_ERROR, loc,
                   "too many errors; elaboration stopped after %" PRIu32, e1->limits->max_errors);
    } else {
        odin3_log(ODIN3_LOG_ERROR, "too many errors; elaboration stopped after %" PRIu32,
                  e1->limits->max_errors);
    }
    return false;
}

void odin3_elab1_error(odin3_elab1 *e1, odin3_loc loc, const char *fmt, ...) {
    if (!error_slot(e1, loc)) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    emit(e1, ODIN3_LOG_ERROR, loc, fmt, args);
    va_end(args);
}

/* The index of format fmt among the run's warning formats (point (v)), registered on first use;
 * NO_MEMORY when it cannot be. */
static odin3_status format_index(odin3_elab1 *e1, const char *fmt, uint32_t *index) {
    for (size_t i = 0; i < e1->warn_fmts.len; i++) {
        if (strcmp(*(const char *const *)odin3_vec_cat(&e1->warn_fmts, i), fmt) == 0) {
            *index = (uint32_t)i;
            return ODIN3_OK;
        }
    }
    const char **slot = (const char **)odin3_vec_push(&e1->warn_fmts);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = fmt;
    *index = (uint32_t)e1->warn_fmts.len - 1;
    return ODIN3_OK;
}

void odin3_elab1_warn_once(odin3_elab1 *e1, odin3_ast_id parsed_node, const char *fmt, ...) {
    uint32_t index = 0;
    odin3_status st = format_index(e1, fmt, &index);
    uint64_t key = (uint64_t)parsed_node.v << E1_KEY_SHIFT | index;
    if (st == ODIN3_OK && odin3_u64map_get(e1->warned, key, NULL)) {
        return;
    }
    st = st != ODIN3_OK ? st : odin3_u64map_put(e1->warned, (odin3_kv){key, 1});
    if (st != ODIN3_OK) {
        e1->fatal = e1->fatal != ODIN3_OK ? e1->fatal : st;
        return;
    }
    va_list args;
    va_start(args, fmt);
    emit(e1, ODIN3_LOG_WARN, odin3_ast_loc(e1->parsed, parsed_node), fmt, args);
    va_end(args);
}

void odin3_elab1_info(odin3_elab1 *e1, odin3_loc loc, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    emit(e1, ODIN3_LOG_INFO, loc, fmt, args);
    va_end(args);
}

bool odin3_elab1_stopped(const odin3_elab1 *e1) {
    return e1->stopped;
}

uint32_t odin3_elab1_errors(const odin3_elab1 *e1) {
    return e1->errors;
}

odin3_status odin3_elab1_status(const odin3_elab1 *e1) {
    return e1->fatal;
}

bool odin3_elab1_pending(odin3_elab1 *e1, odin3_ast_id parsed_node) {
    uint32_t kind = (uint32_t)odin3_ast_kind_of(e1->parsed, parsed_node);
    if (!odin3_elab_pending(ODIN3_ELAB_STAGE1, kind)) {
        return false;
    }
    if (e1->pend_seen[kind]) {
        return true;
    }
    odin3_loc loc = odin3_ast_loc(e1->parsed, parsed_node);
    if (error_slot(e1, loc)) {
        odin3_elab_pend_report(e1->design, loc, kind, e1->pend_seen);
    }
    return true;
}

void odin3_elab1_unhandled(odin3_elab1 *e1, odin3_ast_id parsed_node) {
    odin3_diag(e1->design, ODIN3_LOG_ERROR, odin3_ast_loc(e1->parsed, parsed_node),
               "stage 1: no elaboration of '%s' (a 2D bug)",
               odin3_ast_kind_name(odin3_ast_kind_of(e1->parsed, parsed_node)));
    e1->fatal = e1->fatal != ODIN3_OK ? e1->fatal : ODIN3_ERR_CHECK;
}

void odin3_elab1_not_yet(odin3_elab1 *e1, odin3_ast_id parsed_node) {
    if (!odin3_elab1_pending(e1, parsed_node)) {
        odin3_elab1_unhandled(e1, parsed_node);
    }
}

/* --- the context ----------------------------------------------------------------------------- */

odin3_status odin3_elab1_options(odin3_design *design, const odin3_elab_opts *given,
                                 odin3_elab_opts *out) {
    if (design == NULL || out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab1_options: NULL argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    *out = given != NULL ? *given : (odin3_elab_opts){0};
    if (out->top != 0) {
        return ODIN3_OK;
    }
    const char *cli_top = odin3_pass_get_options().top;
    if (cli_top != NULL && cli_top[0] != '\0') {
        out->top_at = (odin3_loc){0};
        return odin3_design_intern(design, odin3_bytes_cstr(cli_top), &out->top);
    }
    const odin3_project *proj = odin3_design_project(design);
    if (proj != NULL && proj->finished && proj->top != 0) {
        out->top = proj->top;
        out->top_at = proj->top_at;
    }
    return ODIN3_OK;
}

static void init_containers(odin3_elab1 *e1) {
    odin3_vec_init(&e1->defs, sizeof(odin3_elab_def));
    odin3_vec_init(&e1->defx, sizeof(odin3_elab1_defx));
    odin3_vec_init(&e1->params, sizeof(odin3_ast_id));
    odin3_vec_init(&e1->ports, sizeof(odin3_ast_id));
    odin3_vec_init(&e1->insts, sizeof(odin3_ast_id));
    odin3_vec_init(&e1->order, sizeof(uint32_t));
    odin3_vec_init(&e1->warn_fmts, sizeof(const char *));
}

odin3_status odin3_elab1_create(odin3_pass_ctx *ctx, odin3_ast *parsed, const odin3_elab_opts *opts,
                                odin3_elab1 **out) {
    if (out != NULL) {
        *out = NULL;
    }
    if (ctx == NULL || ctx->design == NULL || parsed == NULL || out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab1_create: NULL argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_elab1 *e1 = odin3_util_calloc(sizeof *e1);
    if (e1 == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    e1->ctx = ctx;
    e1->design = ctx->design;
    e1->parsed = parsed;
    e1->opts = opts != NULL ? *opts : (odin3_elab_opts){0};
    e1->limits = odin3_elab_limits_get();
    e1->fatal = ODIN3_OK;
    init_containers(e1);
    e1->by_name = odin3_u64map_create(0);
    e1->instantiated = odin3_u64map_create(0);
    e1->noted = odin3_u64map_create(0);
    e1->warned = odin3_u64map_create(0);
    odin3_status st = ODIN3_OK;
    if (e1->by_name == NULL || e1->instantiated == NULL || e1->noted == NULL ||
        e1->warned == NULL) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    st = st != ODIN3_OK ? st : odin3_design_intern(e1->design, odin3_bytes_cstr(k_work), &e1->work);
    if (st != ODIN3_OK) {
        odin3_elab1_destroy(e1);
        return st;
    }
    *out = e1;
    return ODIN3_OK;
}

void odin3_elab1_destroy(odin3_elab1 *e1) {
    if (e1 == NULL) {
        return;
    }
    odin3_ast_destroy(e1->elab);
    odin3_vec_free(&e1->defs);
    odin3_vec_free(&e1->defx);
    odin3_vec_free(&e1->params);
    odin3_vec_free(&e1->ports);
    odin3_vec_free(&e1->insts);
    odin3_vec_free(&e1->order);
    odin3_vec_free(&e1->warn_fmts);
    odin3_u64map_destroy(e1->by_name);
    odin3_u64map_destroy(e1->instantiated);
    odin3_u64map_destroy(e1->noted);
    odin3_u64map_destroy(e1->warned);
    odin3_util_free(e1);
}

/* --- Task 10's pass W: the UNIT and its MODULE shells --------------------------------------- */

/* A node of kind made in the elaborated store with parsed node from's range and name. */
static odin3_ast_spec spec_from(const odin3_elab1 *e1, uint32_t kind, odin3_ast_id from) {
    return (odin3_ast_spec){.kind = (uint8_t)kind,
                            .loc = odin3_ast_loc(e1->parsed, from).v,
                            .end = odin3_ast_end(e1->parsed, from).v};
}

/* Every entry of a parsed list (a slot LIST's entries or a module's items): no kind inside a
 * module has landed in stage 1 yet (Task 11 writes the first), so each is pending. */
static void pend_span(odin3_elab1 *e1, odin3_ast_span span) {
    for (uint32_t i = 0; i < span.n && !e1->stopped && e1->fatal == ODIN3_OK; i++) {
        if (span.ids[i].v != 0) {
            odin3_elab1_not_yet(e1, span.ids[i]);
        }
    }
}

/* A slot LIST of the elaborated MODULE: present when the parsed slot is (ports) and holds the
 * entries written (none yet); the parsed entries are reported. *out is 0 when absent. */
static odin3_status write_slot(odin3_elab1 *e1, odin3_ast_id parsed_list, bool keep_empty,
                               odin3_ast_id *out) {
    *out = (odin3_ast_id){0};
    if (parsed_list.v == 0) {
        return ODIN3_OK;
    }
    pend_span(e1, odin3_ast_children(e1->parsed, parsed_list));
    if (!keep_empty) {
        return ODIN3_OK;
    }
    odin3_ast_spec spec = spec_from(e1, ODIN3_AST_LIST, parsed_list);
    return odin3_ast_make(e1->elab, &spec, NULL, 0, out);
}

enum { E1_SLOT_PARAMS = 0, E1_SLOT_PORTS = 1, E1_MODULE_SLOTS = 2 };

/* One MODULE shell of definition def (name, range, the ports LIST); its contents reported. */
static odin3_status write_module(odin3_elab1 *e1, const odin3_elab_def *def, odin3_ast_id *out) {
    odin3_ast_id node = def->node;
    pend_span(e1, odin3_ast_attrs(e1->parsed, node));
    odin3_ast_id slots[E1_MODULE_SLOTS] = {{0}, {0}};
    odin3_ast_id params = odin3_ast_child(e1->parsed, node, E1_SLOT_PARAMS);
    odin3_ast_id ports = odin3_ast_child(e1->parsed, node, E1_SLOT_PORTS);
    odin3_status st = write_slot(e1, params, false, &slots[E1_SLOT_PARAMS]);
    st = st != ODIN3_OK ? st : write_slot(e1, ports, true, &slots[E1_SLOT_PORTS]);
    odin3_ast_span kids = odin3_ast_children(e1->parsed, node);
    if (kids.n > E1_MODULE_SLOTS) {
        pend_span(e1, (odin3_ast_span){kids.ids + E1_MODULE_SLOTS, kids.n - E1_MODULE_SLOTS});
    }
    odin3_ast_spec spec = spec_from(e1, ODIN3_AST_MODULE, node);
    spec.name = def->name;
    return st != ODIN3_OK ? st : odin3_ast_make(e1->elab, &spec, slots, E1_MODULE_SLOTS, out);
}

/* The directives of the design UNITs (default_nettype, timescale, …): none has landed. */
static void pend_unit_directives(odin3_elab1 *e1) {
    uint32_t from = odin3_ast_library_units_from(e1->parsed);
    uint32_t units = from != 0 ? from : odin3_ast_root_count(e1->parsed);
    for (uint32_t idx = 0; idx < units && !e1->stopped; idx++) {
        odin3_ast_span kids = odin3_ast_children(e1->parsed, odin3_ast_root(e1->parsed, idx));
        for (uint32_t i = 0; i < kids.n && !e1->stopped; i++) {
            if (odin3_ast_kind_of(e1->parsed, kids.ids[i]) == ODIN3_AST_DIRECTIVE) {
                odin3_elab1_not_yet(e1, kids.ids[i]);
            }
        }
    }
}

/* The MODULEs in order on the store's pending stack (above mark). */
static odin3_status push_modules(odin3_elab1 *e1) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < e1->order.len; i++) {
        uint32_t idx = *(const uint32_t *)odin3_vec_cat(&e1->order, i);
        const odin3_elab_def *def = odin3_elab1_def(e1, idx);
        const odin3_elab1_defx *ext = odin3_vec_cat(&e1->defx, idx);
        if (def->kind != ODIN3_DEF_MODULE || ext->taken != ODIN3_TAKEN_NONE) {
            continue; /* a black box becomes a cell type (EL-11); a taken name failed (EL-44) */
        }
        if (odin3_elab1_pending(e1, def->node)) {
            continue;
        }
        odin3_ast_id module = {0};
        st = write_module(e1, def, &module);
        st = st != ODIN3_OK ? st : odin3_ast_push(e1->elab, module);
    }
    return st;
}

odin3_status odin3_elab1_write_unit(odin3_elab1 *e1, odin3_ast **out) {
    *out = NULL;
    if (e1->order.len == 0) {
        return ODIN3_OK;
    }
    odin3_status st =
        odin3_ast_create(e1->design, ODIN3_AST_FORM_ELABORATED, e1->ctx->run, &e1->elab);
    if (st != ODIN3_OK) {
        return st;
    }
    pend_unit_directives(e1);
    uint32_t mark = odin3_ast_mark(e1->elab);
    st = push_modules(e1);
    const odin3_elab_def *top =
        odin3_elab1_def(e1, *(const uint32_t *)odin3_vec_cat(&e1->order, 0));
    odin3_ast_spec spec = spec_from(e1, ODIN3_AST_UNIT, top->node);
    spec.name = top->name;
    odin3_ast_id unit = {0};
    st = st != ODIN3_OK ? st : odin3_ast_make_marked(e1->elab, &spec, mark, &unit);
    if (st != ODIN3_OK) {
        odin3_ast_destroy(e1->elab);
        e1->elab = NULL;
        return st;
    }
    *out = e1->elab;
    e1->elab = NULL;
    return ODIN3_OK;
}

/* --- odin3_elab_stage1 ----------------------------------------------------------------------- */

static bool stage1_args_ok(const odin3_pass_ctx *ctx, const odin3_ast *parsed,
                           odin3_ast **elab_out) {
    if (ctx == NULL || parsed == NULL || elab_out == NULL || ctx->design == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab_stage1: NULL argument");
        return false;
    }
    if (odin3_ast_form_of(parsed) != ODIN3_AST_FORM_PARSED ||
        odin3_ast_design(parsed) != ctx->design) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab_stage1: not a parsed store of this design");
        return false;
    }
    return true;
}

/* The steps after the context exists: definitions, the top, reachability, the shells. */
static odin3_status stage1_steps(odin3_elab1 *e1, odin3_ast **elab) {
    odin3_status st = odin3_elab1_collect_defs(e1);
    uint32_t top = 0;
    if (st == ODIN3_OK && !e1->stopped) {
        st = odin3_elab1_select_top(e1, &top);
        if (st == ODIN3_ERR_PARSE) {
            return e1->fatal; /* reported (or out of memory): nothing to walk from */
        }
    }
    if (st == ODIN3_OK && !e1->stopped) {
        st = odin3_elab1_reach(e1, top);
    }
    if (st == ODIN3_OK && !e1->stopped) {
        st = odin3_elab1_write_unit(e1, elab);
    }
    return st != ODIN3_OK ? st : e1->fatal;
}

odin3_status odin3_elab_stage1(odin3_pass_ctx *ctx, odin3_ast *parsed, const odin3_elab_opts *opts,
                               odin3_ast **elab_out) {
    if (!stage1_args_ok(ctx, parsed, elab_out)) {
        return ODIN3_ERR_INVALID_ARG;
    }
    *elab_out = NULL;
    odin3_elab1 *e1 = NULL;
    odin3_status st = odin3_elab1_create(ctx, parsed, opts, &e1);
    odin3_ast *elab = NULL;
    st = st != ODIN3_OK ? st : stage1_steps(e1, &elab);
    if (st == ODIN3_OK && (e1->errors > 0 || e1->stopped)) {
        st = ODIN3_ERR_PARSE; /* §2.3: any stage-1 error destroys the elaborated store */
    }
    if (st == ODIN3_OK && elab == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab_stage1: internal: no store written, no error");
        st = ODIN3_ERR_CHECK; /* a 2D bug, never user input (§9.3) */
    }
    st = st != ODIN3_OK ? st : odin3_ast_finish(elab);
    if (st != ODIN3_OK) {
        odin3_ast_destroy(elab);
        elab = NULL;
    }
    odin3_elab1_destroy(e1);
    *elab_out = elab;
    return st;
}

/* --- odin3_elab_run (EL-2) ------------------------------------------------------------------- */

/* Step 2's check of the elaborated store (Debug, --check); the store is destroyed on failure. */
static odin3_status check_elab(odin3_ast **elab) {
    if (!E1_ALWAYS_CHECK && !odin3_pass_get_options().check) {
        return ODIN3_OK;
    }
    odin3_status st = odin3_ast_check(*elab, ODIN3_CHECK_FULL);
    if (st == ODIN3_OK) {
        return ODIN3_OK;
    }
    odin3_ast_destroy(*elab);
    *elab = NULL;
    return st == ODIN3_ERR_NO_MEMORY ? st : ODIN3_ERR_CHECK;
}

/* Until Task 17 (whose stage 2 calls it as each body starts): the on_body_start test hook for
 * each MODULE of the elaborated store, in order, as stage 2 begins. */
static void body_hooks(const odin3_ast *elab) {
    const odin3_elab_test_hooks *hooks = odin3_elab_test_hooks_get();
    if (hooks->on_body_start == NULL || odin3_ast_root_count(elab) == 0) {
        return;
    }
    odin3_ast_span modules = odin3_ast_children(elab, odin3_ast_root(elab, 0));
    for (uint32_t i = 0; i < modules.n; i++) {
        hooks->on_body_start(hooks->user, modules.ids[i]);
    }
}

/* Step 6: the design holds the elaborated store and drops it unless the keep level keeps it. */
static odin3_status hand_over_elab(odin3_design *design, odin3_ast *elab) {
    odin3_passrun_id run = odin3_ast_run(elab);
    odin3_status st = odin3_design_set_ast(design, elab);
    if (st != ODIN3_OK) {
        odin3_ast_destroy(elab);
        return st;
    }
    odin3_design_drop_ast(design, run, ODIN3_AST_FORM_ELABORATED);
    return ODIN3_OK;
}

odin3_status odin3_elab_run(odin3_pass_ctx *ctx, odin3_design *design, odin3_ast *parsed,
                            const odin3_elab_opts *opts) {
    if (ctx == NULL || design == NULL || parsed == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_elab_run: NULL argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_passrun_id parsed_run = odin3_ast_run(parsed);
    odin3_elab_opts options;
    odin3_status st = odin3_elab1_options(design, opts, &options);
    odin3_elab_test_note_opts(&options);
    odin3_ast *elab = NULL;
    st = st != ODIN3_OK ? st : odin3_elab_stage1(ctx, parsed, &options, &elab);
    st = st != ODIN3_OK ? st : check_elab(&elab);
    odin3_design_drop_ast(design, parsed_run, ODIN3_AST_FORM_PARSED); /* step 3 */
    if (st != ODIN3_OK) {
        return st;
    }
    body_hooks(elab);
    st = odin3_elab_stage2(ctx, elab, &options);
    odin3_status held = hand_over_elab(design, elab);
    return held == ODIN3_ERR_NO_MEMORY || st == ODIN3_OK ? held : st; /* NO_MEMORY first */
}
