/*
 * pg_call.c — function and task calls inlined into a process, calls in continuous contexts, and
 * the words of register arrays with a variable index (2D Task 23; EL-38, EL-27), after Yosys's
 * `simplify`, which inlines every call that is not constant.
 *
 * A call frame gives every port, local and result variable of the callee (and of the named blocks
 * in it) an unnamed IR wire for the call, by setting the symbol's ir_wire for its duration, so the
 * expression walker and the lvalue code see the callee's names like any variable. The frame's
 * nets have `lv` the net itself and read x until assigned: they are process-local temporaries
 * that the process drives (O pins with no sync action), never flip-flops (Yosys's `nosync`). The
 * actuals are generated in the caller's context first; the inputs are assigned to the formals by
 * blocking assignment, the body runs in the same builder (so `if`, `case` and loops inside work),
 * and for a task each `output`/`inout` formal is copied to its actual. A function's value is
 * `rv(result)`.
 *
 * No call is run from inside an expression walk (PR 2D-7, review I6; the no-recursion rule): the
 * walker is suspendable (odin3_elab2_walk), the statement code that needs a value pushes a
 * PG_F_GEN frame for it (pg_gen_push), and when the walk meets a call the frame loop pushes a
 * PG_F_CALL frame above it. The call frame is a small state machine (the callee's checks, the
 * actuals, each a PG_F_GEN frame of its own, the frame's wires and the binding, the body as an
 * ordinary statement frame, then the copy-out and the value); when it ends the walk is resumed
 * with the value. Everything runs in the order the nested calls used to: the same cells, nets and
 * names. One call inside another costs a few heap records, never C stack. An assignment's value,
 * an if's condition and a call's actuals go this way, as does a task enable.
 *
 * What still runs a call by C recursion (odin3_pg_call: pg_call_hook from a plain
 * odin3_elab2_expr) is an expression the statement code needs whole before it can go on: a case
 * selector or item, an index of a select, a memory word or a register-array word, the lvalue of a
 * copy-out, and the actuals of a call in a continuous context (odin3_pg_comb_call: `assign y =
 * f(f(a))`, review M9). Those nest by C stack, so they are capped at PG_SYNC_NEST_MAX levels (a
 * located error).
 * The call nesting as a whole (ODIN3_ELAB_MAX_INLINE_DEPTH, ODIN3_ELAB_MAX_CALL_DEPTH) is a
 * resource cap on the heap records, not on the stack. Recursion is rejected.
 *
 * A call met outside a process builds a combinational `$proc` of its own (one ALWAYS sync) whose
 * output is the result variable's wire; a register-array word with a variable index is a case over
 * the words, inline in a process or a `$proc` of its own.
 */
#include "frontends/elab/pg_internal.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/symtab.h"
#include "frontends/elab/elab2_mem.h"
#include "frontends/elab/elab_internal.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { FN_PORTS = 1, FN_BODY = 3, TASK_PORTS = 0, TASK_BODY = 2 };
enum { SIGN_BITS = 63, GROUP_SHIFT = 32 };

/* A nested sync call (odin3_pg_call) is C recursion: its depth is capped. */
enum { PG_SYNC_NEST_MAX = 128 };

/* --- the callee ------------------------------------------------------------------------------- */

static uint32_t count_formals(const odin3_ast *ast, odin3_ast_id ports) {
    uint32_t total = 0;
    for (uint32_t k = 0; ports.v != 0 && k < odin3_ast_nchild(ast, ports); k++) {
        uint32_t kids = odin3_ast_nchild(ast, odin3_ast_child(ast, ports, k));
        total += kids > 1 ? kids - 1U : 0U;
    }
    return total;
}

static odin3_status info_make(odin3_pg *pg, odin3_ast_id call, pg_cinfo *info) {
    odin3_elab2 *e2 = pg->e2;
    const odin3_ast *ast = e2->elab;
    odin3_status st = odin3_elab2_callee(e2, call, &info->decl);
    if (st != ODIN3_OK) {
        return st;
    }
    info->call = call;
    info->is_task = odin3_ast_kind_of(ast, call) == ODIN3_AST_TASK_CALL;
    info->name = odin3_ast_name_str(ast, call);
    info->ports = odin3_ast_child(ast, info->decl, info->is_task ? TASK_PORTS : FN_PORTS);
    info->body = odin3_ast_child(ast, info->decl, info->is_task ? TASK_BODY : FN_BODY);
    uint32_t formals = count_formals(ast, info->ports);
    uint32_t given = odin3_ast_nchild(ast, call);
    if (formals != given) {
        odin3_elab2_error(e2, odin3_ast_loc(ast, call),
                          info->is_task ? "task '%s' has %u arguments; %u given"
                                        : "function '%s' has %u inputs; %u arguments given",
                          info->name, formals, given);
        return ODIN3_ERR_PARSE;
    }
    return ODIN3_OK;
}

/* Whether the body uses `while`, which only the interpreter can run (F2). */
static bool uses_while(const odin3_elab2 *e2, odin3_ast_id body) {
    odin3_ast_cursor cur;
    bool found = false;
    if (body.v == 0 || odin3_ast_cursor_init(&cur, e2->elab, body) != ODIN3_OK) {
        return false;
    }
    odin3_ast_event ev;
    odin3_ast_id node;
    while (!found && odin3_ast_cursor_next(&cur, &ev, &node)) {
        found = ev == ODIN3_AST_ENTER && odin3_ast_kind_of(e2->elab, node) == ODIN3_AST_WHILE;
    }
    odin3_ast_cursor_free(&cur);
    return found;
}

/* The refusals of point (aa): a body only the interpreter runs, recursion, the nesting cap. */
static odin3_status info_check(odin3_pg *pg, const pg_cinfo *info) {
    odin3_elab2 *e2 = pg->e2;
    odin3_loc loc = odin3_ast_loc(e2->elab, info->call);
    if (!info->is_task && uses_while(e2, info->body)) {
        odin3_elab2_error(e2, loc,
                          "function '%s' can be called only with constant arguments (it uses "
                          "'while')",
                          info->name);
        return ODIN3_ERR_PARSE;
    }
    if (odin3_u64map_get(pg->active, info->decl.v, NULL)) {
        odin3_elab2_error(e2, loc,
                          "recursive call of '%s' cannot be inlined (its arguments are not "
                          "constant)",
                          info->name);
        return ODIN3_ERR_PARSE;
    }
    /* a resource cap on the heap records of the nesting (not on the stack: see the header) */
    uint32_t cap = e2->limits->max_call_depth < e2->limits->max_inline_depth
                       ? e2->limits->max_call_depth
                       : e2->limits->max_inline_depth;
    if (pg->calls.len >= cap) {
        odin3_elab2_fatal(e2, loc, "function call nesting deeper than %u at '%s'", cap, info->name);
        return ODIN3_ERR_PARSE;
    }
    return ODIN3_OK;
}

/* The formals in order (a ports LIST of PORT_DECLs, each with its DECLARATORs after the range). */
static odin3_status formals_make(pg_crun *cr) {
    const odin3_ast *ast = cr->pg->e2->elab;
    cr->n = count_formals(ast, cr->info.ports);
    cr->formals = odin3_arena_alloc(cr->pg->e2->scratch, (cr->n + 1U) * sizeof *cr->formals);
    cr->args = odin3_arena_alloc(cr->pg->e2->scratch, (cr->n + 1U) * sizeof *cr->args);
    if (cr->formals == NULL || cr->args == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t at = 0;
    for (uint32_t k = 0; cr->info.ports.v != 0 && k < odin3_ast_nchild(ast, cr->info.ports); k++) {
        odin3_ast_id port = odin3_ast_child(ast, cr->info.ports, k);
        for (uint32_t j = 1; j < odin3_ast_nchild(ast, port); j++) {
            odin3_sym_id sym = odin3_elab_sym_of(&cr->pg->e2->bind, odin3_ast_child(ast, port, j));
            cr->formals[at++] = (pg_cformal){sym, (uint8_t)odin3_ast_sub(ast, port)};
        }
    }
    return ODIN3_OK;
}

/* --- frames ----------------------------------------------------------------------------------- */

/* The nets of a frame wire are one temporary group, each with `lv` itself and `rv` x. */
static odin3_status frame_nets(odin3_pg *pg, odin3_wire_id wire, uint32_t width) {
    uint32_t group = ++pg->groups;
    odin3_status st = ODIN3_OK;
    for (uint32_t bit = 0; st == ODIN3_OK && bit < width; bit++) {
        odin3_net_id net = odin3_wire_net(pg->e2->mod, wire, bit);
        st = odin3_u64map_put(pg->group, (odin3_kv){net.v, ((uint64_t)group << GROUP_SHIFT) | bit});
        st = st != ODIN3_OK ? st : pg_lv_set(pg, net, net);
        st = st != ODIN3_OK ? st : pg_rv_set(pg, net, odin3_sb_of_bit(ODIN3_BIT_X));
    }
    return st;
}

/* The symbol is noted, so the frame's exit gives it back. */
static odin3_status frame_note(odin3_pg *pg, odin3_sym_id id) {
    uint32_t *mark = odin3_vec_push(&pg->fsyms);
    if (mark == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *mark = id.v;
    return ODIN3_OK;
}

/* The wire is noted, so the module's end can delete it when nothing used it. */
static odin3_status fwire_note(odin3_pg *pg, odin3_wire_id wire) {
    pg_fwire *rec = odin3_vec_push(&pg->fwires);
    if (rec == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *rec = (pg_fwire){pg->e2->mod_id, wire};
    return ODIN3_OK;
}

/* The unnamed wire of one symbol of the callee. */
static odin3_status frame_wire(odin3_pg *pg, odin3_sym_id id) {
    odin3_elab2 *e2 = pg->e2;
    const odin3_symbol *rec = odin3_symbol_get(e2->st, id);
    if ((rec->kind != ODIN3_SYM_NET && rec->kind != ODIN3_SYM_VAR) || rec->n_unpacked > 0) {
        return ODIN3_OK;
    }
    uint32_t width = rec->has_packed ? (uint32_t)odin3_elab2_dim_size(&rec->packed) : 1U;
    odin3_wire_spec spec = {.msb = rec->has_packed ? rec->packed.msb : 0,
                            .lsb = rec->has_packed ? rec->packed.lsb : 0,
                            .is_signed = (rec->flags & ODIN3_SYM_F_SIGNED) != 0};
    odin3_elab2_prov_req req = {
        rec->declarator.v != 0 ? rec->declarator : rec->decl, {0}, rec->scope.v};
    odin3_wire_id wire = {0};
    odin3_status st = odin3_elab2_reserve_nets(e2, width, odin3_ast_loc(e2->elab, req.node));
    st = st != ODIN3_OK ? st : odin3_elab2_prov(e2, &req, &spec.prov);
    st = st != ODIN3_OK ? st : odin3_wire_create(e2->mod, &spec, NULL, &wire);
    st = st != ODIN3_OK ? st : fwire_note(pg, wire);
    st = st != ODIN3_OK ? st : frame_note(pg, id);
    odin3_elab2_counts made = {0, 0, width, 1};
    st = st != ODIN3_OK ? st : odin3_symbol_set_ir(e2->st, id, e2->mod_id, wire);
    st = st != ODIN3_OK ? st : odin3_elab2_account(e2, &made);
    return st != ODIN3_OK ? st : frame_nets(pg, wire, width);
}

/* The symbols of one scope get their wires; its child scopes join the worklist. */
static odin3_status frame_scope(odin3_pg *pg, odin3_scope_id scope, odin3_vec *todo) {
    const odin3_symtab *st = pg->e2->st;
    odin3_status status = ODIN3_OK;
    for (odin3_sym_id id = odin3_scope_first_symbol(st, scope); status == ODIN3_OK && id.v != 0;
         id = odin3_scope_next_symbol(st, id)) {
        status = frame_wire(pg, id);
    }
    for (odin3_scope_id kid = odin3_scope_first_child(st, scope); status == ODIN3_OK && kid.v != 0;
         kid = odin3_scope_next_sibling(st, kid)) {
        odin3_scope_id *slot = odin3_vec_push(todo);
        status = slot != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        if (slot != NULL) {
            *slot = kid;
        }
    }
    return status;
}

/* Every symbol of the callee's scope and the scopes below it (named blocks). */
static odin3_status frame_symbols(odin3_pg *pg, odin3_ast_id decl) {
    odin3_vec todo;
    odin3_vec_init(&todo, sizeof(odin3_scope_id));
    odin3_scope_id *root = odin3_vec_push(&todo);
    odin3_status st = root != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    if (root != NULL) {
        *root = odin3_elab_scope_of(&pg->e2->bind, decl);
    }
    while (st == ODIN3_OK && todo.len > 0) {
        odin3_scope_id scope = *(const odin3_scope_id *)odin3_vec_cat(&todo, todo.len - 1);
        odin3_vec_pop(&todo);
        st = frame_scope(pg, scope, &todo);
    }
    odin3_vec_free(&todo);
    return st;
}

static void frame_exit(odin3_pg *pg) {
    const pg_callrec *rec = odin3_vec_cat(&pg->calls, pg->calls.len - 1);
    while (pg->fsyms.len > rec->fsyms_mark) {
        uint32_t sym = *(const uint32_t *)odin3_vec_cat(&pg->fsyms, pg->fsyms.len - 1);
        (void)odin3_symbol_set_ir(pg->e2->st, (odin3_sym_id){sym}, (odin3_module_id){0},
                                  (odin3_wire_id){0});
        odin3_vec_pop(&pg->fsyms);
    }
    pg->e2->call = rec->saved_call;
    (void)odin3_u64map_remove(pg->active, rec->decl.v);
    odin3_vec_pop(&pg->calls);
}

void pg_frames_unwind(odin3_pg *pg) {
    while (pg->calls.len > 0) {
        frame_exit(pg);
    }
    pg->e2->call = (odin3_ast_id){0};
}

static odin3_status frame_enter(pg_crun *cr) {
    odin3_pg *pg = cr->pg;
    pg_callrec *rec = odin3_vec_push(&pg->calls);
    if (rec == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *rec = (pg_callrec){cr->info.decl, cr->info.call, pg->e2->call, (uint32_t)pg->fsyms.len};
    pg->e2->call = cr->info.call;
    odin3_status st = odin3_u64map_put(pg->active, (odin3_kv){cr->info.decl.v, 1});
    return st != ODIN3_OK ? st : frame_symbols(pg, cr->info.decl);
}

/* --- values in and out of the frame ----------------------------------------------------------- */

/* An assignment of value to the nets, raw (T9 is not asked), `=` or `<=`. */
typedef struct net_asg {
    const odin3_net_id *nets;
    uint32_t n;
    const odin3_sig *value;
    odin3_ast_id stmt;
    bool blocking;
} net_asg;

static odin3_status assign_nets(odin3_pg *pg, const net_asg *na) {
    odin3_vec_clear(&pg->slv.nets);
    odin3_vec_clear(&pg->slv.bits);
    for (uint32_t i = 0; i < na->n; i++) {
        odin3_net_id *slot = odin3_vec_push(&pg->slv.nets);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = na->nets[i];
    }
    pg_asg asg = {&pg->slv, na->value, na->stmt, na->blocking, true};
    return pg_apply(pg, &asg);
}

static odin3_status assign_wire(odin3_pg *pg, odin3_wire_id wire, const net_asg *proto) {
    uint32_t width = odin3_wire_width(pg->e2->mod, wire);
    odin3_net_id *nets = odin3_arena_alloc(pg->e2->scratch, (width + 1U) * sizeof *nets);
    if (nets == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < width; i++) {
        nets[i] = odin3_wire_net(pg->e2->mod, wire, i);
    }
    net_asg na = *proto;
    na.nets = nets;
    na.n = width;
    return assign_nets(pg, &na);
}

/* The current value of a wire of the frame (rv), as a signal. */
static odin3_status read_wire(odin3_pg *pg, odin3_wire_id wire, bool is_signed, odin3_sig *out) {
    uint32_t width = odin3_wire_width(pg->e2->mod, wire);
    odin3_status st = odin3_elab2_sig_new(pg->e2, width, out);
    for (uint32_t i = 0; st == ODIN3_OK && i < width; i++) {
        odin3_net_id net = odin3_wire_net(pg->e2->mod, wire, i);
        out->bits[i] = net.v;
        (void)pg_rv_get(pg, net, &out->bits[i]);
    }
    out->is_signed = is_signed;
    return st;
}

static const odin3_symbol *formal_rec(const pg_crun *cr, uint32_t index) {
    return odin3_symbol_get(cr->pg->e2->st, cr->formals[index].sym);
}

static bool is_lvalue_kind(odin3_ast_kind kind) {
    return kind == ODIN3_AST_IDENT || kind == ODIN3_AST_HIER_NAME || kind == ODIN3_AST_SELECT ||
           kind == ODIN3_AST_CONCAT;
}

/* The actuals: an input's expression generated in the caller's context as an assignment to its
 * formal (K1); an output's must be a variable (K9). reads is the caller's hook, NULL outside a
 * process. */
static odin3_status call_args(pg_crun *cr, const odin3_elab2_reads *reads) {
    odin3_elab2 *e2 = cr->pg->e2;
    for (uint32_t i = 0; i < cr->n; i++) {
        odin3_ast_id actual = odin3_ast_child(e2->elab, cr->info.call, i);
        const odin3_symbol *rec = formal_rec(cr, i);
        bool out_only = cr->formals[i].dir == ODIN3_AST_PORT_DECL_OUTPUT;
        if (cr->formals[i].dir != ODIN3_AST_PORT_DECL_INPUT &&
            !is_lvalue_kind(odin3_ast_kind_of(e2->elab, actual))) {
            odin3_elab2_error(e2, odin3_ast_loc(e2->elab, actual),
                              "output argument %u of task '%s' must be a variable", i + 1U,
                              cr->info.name);
            return ODIN3_ERR_PARSE;
        }
        if (out_only) {
            continue;
        }
        uint32_t width = rec->has_packed ? (uint32_t)odin3_elab2_dim_size(&rec->packed) : 1U;
        odin3_elab2_expr_req req = {actual, {width, true, false}, NULL, reads};
        odin3_status st = odin3_elab2_expr(e2, &req, &cr->args[i]);
        st = st != ODIN3_OK
                 ? st
                 : odin3_elab2_extend(e2, &cr->args[i], width, cr->args[i].is_signed, &cr->args[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* The inputs and inouts are assigned to their formals' wires (blocking). */
static odin3_status call_bind(pg_crun *cr) {
    for (uint32_t i = 0; i < cr->n; i++) {
        if (cr->formals[i].dir == ODIN3_AST_PORT_DECL_OUTPUT) {
            continue;
        }
        const odin3_symbol *rec = formal_rec(cr, i);
        net_asg proto = {NULL, 0, &cr->args[i], cr->info.call, true};
        odin3_status st = assign_wire(cr->pg, rec->ir_wire, &proto);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* A task's output or inout formal copied to its actual by blocking assignment (K9). */
static odin3_status copy_out(pg_crun *cr, uint32_t index) {
    odin3_pg *pg = cr->pg;
    odin3_ast_id actual = odin3_ast_child(pg->e2->elab, cr->info.call, index);
    const odin3_symbol *rec = formal_rec(cr, index);
    odin3_sig value;
    odin3_status st = read_wire(pg, rec->ir_wire, (rec->flags & ODIN3_SYM_F_SIGNED) != 0, &value);
    st = st != ODIN3_OK ? st : pg_take_target_of(pg, actual);
    if (st != ODIN3_OK) {
        return st;
    }
    uint32_t width = pg->lvs.var.node.v != 0 ? pg->lvs.var.width : (uint32_t)pg->lvs.nets.len;
    if (pg->lvs.regs.node.v != 0) {
        width = odin3_elab2_mem_get(pg->e2, pg->lvs.regs.mem)->width;
    }
    st = odin3_elab2_extend(pg->e2, &value, width, value.is_signed, &value);
    odin3_pg_assign_req req = {cr->info.call, actual, &value, true};
    return st != ODIN3_OK ? st : odin3_pg_assign(pg, &req);
}

static odin3_status call_copy_out(pg_crun *cr) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < cr->n; i++) {
        if (cr->formals[i].dir != ODIN3_AST_PORT_DECL_INPUT) {
            st = copy_out(cr, i);
        }
    }
    return st;
}

/* The body, in the frame: its switch targets are found (the scan of a call's body), then it runs.
 */
static odin3_status call_body(pg_crun *cr) {
    odin3_pg *pg = cr->pg;
    if (cr->info.body.v == 0) {
        return ODIN3_OK;
    }
    odin3_status st = pg_scan_call(pg, cr->info.body);
    return st != ODIN3_OK ? st : pg_run(pg, cr->info.body);
}

/* The function's value: its result variable's current value. */
static odin3_status call_value(pg_crun *cr, odin3_sig *out) {
    odin3_sym_id sym = {0};
    const odin3_symbol *res = odin3_elab2_fn_result(cr->pg->e2, cr->info.decl, &sym);
    if (res == NULL) {
        return ODIN3_ERR_CHECK;
    }
    return read_wire(cr->pg, res->ir_wire, (res->flags & ODIN3_SYM_F_SIGNED) != 0, out);
}

/* --- the frame wires nothing used ------------------------------------------------------------- */

/* Whether no net of the wire has a pin. */
static bool wire_pinless(const odin3_module *mod, odin3_wire_id wire) {
    uint32_t width = odin3_wire_width(mod, wire);
    for (uint32_t bit = 0; bit < width; bit++) {
        odin3_net_id net = odin3_wire_net(mod, wire, bit);
        if (odin3_net_driver_count(mod, net) != 0 || odin3_net_sinks(mod, net).count != 0) {
            return false;
        }
    }
    return true;
}

/* The wire and its nets are deleted. */
static odin3_status wire_drop(odin3_module *mod, odin3_wire_id wire) {
    uint32_t width = odin3_wire_width(mod, wire);
    odin3_status st = odin3_wire_delete(mod, wire);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < width; bit++) {
        st = odin3_net_delete(mod, odin3_wire_net(mod, wire, bit));
    }
    return st;
}

/* A callee variable nothing assigns or reads (an unused local) leaves its frame wire pinless; the
 * wire is no declared signal of the design but an internal temporary, so it is deleted when the
 * module is done (PR 2D-7: a leaked net, `rule 4: net has no pins`). Named wires are never touched.
 */
odin3_status odin3_elab2_sweep_frames(odin3_elab2 *e2) {
    odin3_pg *pg = e2->pg;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; pg != NULL && st == ODIN3_OK && i < pg->fwires.len; i++) {
        const pg_fwire *rec = odin3_vec_cat(&pg->fwires, i);
        if (rec->mod.v == e2->mod_id.v && odin3_wire_live(e2->mod, rec->wire) &&
            wire_pinless(e2->mod, rec->wire)) {
            st = wire_drop(e2->mod, rec->wire);
        }
    }
    if (pg != NULL) {
        odin3_vec_clear(&pg->fwires);
    }
    return st;
}

/* --- a call in a process: the PG_F_CALL frame --------------------------------------------------
 */

enum { CP_START = 0, CP_ARGS = 1, CP_AFTER = 2 };

/* The innermost call frame's record (a PG_F_CALL frame is on top, or its actuals' frames are
 * above it and done). */
static pg_crun *crun_top(const odin3_pg *pg) {
    return *(pg_crun *const *)odin3_vec_cat(&pg->cruns, pg->cruns.len - 1);
}

/* Gives the call back: its frames' symbols, the statement's target, its record. */
static void call_unwind(odin3_pg *pg) {
    const pg_crun *cr = crun_top(pg);
    while (pg->calls.len > cr->depth) {
        frame_exit(pg);
    }
    odin3_elab2_lvalue_free(&pg->lvs);
    pg->lvs = cr->saved_lvs;
    odin3_vec_pop(&pg->cruns);
}

/* Pushes the call's frame. The call runs with a scratch lvalue: an expression's calls must not
 * disturb the target the statement that holds them has taken apart (it is put back at the end). */
static odin3_status call_push(odin3_pg *pg, odin3_ast_id call, bool from_walk) {
    pg_crun *cr = odin3_arena_alloc(pg->e2->scratch, sizeof *cr);
    pg_crun **slot = cr != NULL ? (pg_crun **)odin3_vec_push(&pg->cruns) : NULL;
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *cr = (pg_crun){.pg = pg, .depth = pg->calls.len, .from_walk = from_walk};
    cr->info.call = call;
    cr->saved_lvs = pg->lvs;
    odin3_elab2_lvalue_init(&pg->lvs);
    pg->lvs.procedural = true;
    *slot = cr;
    pg_frame frame = {PG_F_CALL, call, CP_START, 0, 0, 0};
    odin3_status st = pg_frame_push(pg, &frame);
    if (st != ODIN3_OK) {
        call_unwind(pg);
    }
    return st;
}

/* The callee's checks and its formals. */
static odin3_status call_start(pg_crun *cr) {
    odin3_status st = info_make(cr->pg, cr->info.call, &cr->info);
    st = st != ODIN3_OK ? st : info_check(cr->pg, &cr->info);
    return st != ODIN3_OK ? st : formals_make(cr);
}

/* The next input actual (K1; an output's must be a variable, K9): its PG_F_GEN frame is pushed and
 * *waiting set; none left: the actuals are all generated. */
static odin3_status call_args_next(pg_crun *cr, bool *waiting) {
    odin3_pg *pg = cr->pg;
    odin3_elab2 *e2 = pg->e2;
    pg_frame *frame = odin3_vec_at(&pg->frames, pg->frames.len - 1);
    *waiting = false;
    while (frame->end < cr->n) {
        uint32_t i = frame->end++;
        odin3_ast_id actual = odin3_ast_child(e2->elab, cr->info.call, i);
        const odin3_symbol *rec = formal_rec(cr, i);
        if (cr->formals[i].dir != ODIN3_AST_PORT_DECL_INPUT &&
            !is_lvalue_kind(odin3_ast_kind_of(e2->elab, actual))) {
            odin3_elab2_error(e2, odin3_ast_loc(e2->elab, actual),
                              "output argument %u of task '%s' must be a variable", i + 1U,
                              cr->info.name);
            return ODIN3_ERR_PARSE;
        }
        if (cr->formals[i].dir == ODIN3_AST_PORT_DECL_OUTPUT) {
            continue;
        }
        uint32_t width = rec->has_packed ? (uint32_t)odin3_elab2_dim_size(&rec->packed) : 1U;
        pg_gen_req req = {actual, cr->info.call, {width, true, false}, PG_G_ARG, i};
        *waiting = true;
        return pg_gen_push(pg, &req);
    }
    return ODIN3_OK;
}

/* The frame's wires, the actuals bound to the formals, the body's statement pushed. */
static odin3_status call_enter(pg_crun *cr) {
    odin3_pg *pg = cr->pg;
    odin3_status st = frame_enter(cr);
    st = st != ODIN3_OK ? st : call_bind(cr);
    if (st != ODIN3_OK || cr->info.body.v == 0) {
        return st;
    }
    st = pg_scan_call(pg, cr->info.body);
    pg_frame body = {PG_F_STMT, cr->info.body, 0, 0, 0, 0};
    return st != ODIN3_OK ? st : pg_frame_push(pg, &body);
}

/* The body is done: the outputs copied out, the value read, the call given back, the walk that
 * waits for the value resumed. */
static odin3_status call_after(pg_crun *cr) {
    odin3_pg *pg = cr->pg;
    odin3_status st = call_copy_out(cr);
    st = st != ODIN3_OK || cr->info.is_task ? st : call_value(cr, &pg->call_sig);
    bool resume = cr->from_walk;
    call_unwind(pg);
    odin3_vec_pop(&pg->frames);
    if (st == ODIN3_OK && resume) {
        odin3_elab2_walk *walk = odin3_vec_at(&pg->walks, pg->walks.len - 1);
        st = odin3_elab2_walk_resume(pg->e2, walk, &pg->call_sig);
    }
    return st;
}

odin3_status pg_call_step(odin3_pg *pg) {
    pg_crun *cr = crun_top(pg);
    pg_frame *frame = odin3_vec_at(&pg->frames, pg->frames.len - 1);
    odin3_status st = ODIN3_OK;
    if (frame->next == CP_START) {
        frame->next = CP_ARGS;
        st = call_start(cr);
    }
    if (st != ODIN3_OK || frame->next == CP_AFTER) {
        return st != ODIN3_OK ? st : call_after(cr);
    }
    bool waiting = false;
    st = call_args_next(cr, &waiting);
    if (st != ODIN3_OK || waiting) {
        return st;
    }
    frame = odin3_vec_at(&pg->frames, pg->frames.len - 1);
    frame->next = CP_AFTER;
    return call_enter(cr);
}

/* --- an expression whose calls run from the frame stack ----------------------------------------
 */

odin3_status pg_gen_push(odin3_pg *pg, const pg_gen_req *req) {
    pg->hooks = pg_reads(pg);
    odin3_elab2_expr_req expr = {req->expr, req->context, NULL, &pg->hooks};
    odin3_elab2_walk *walk = odin3_vec_push(&pg->walks);
    if (walk == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = odin3_elab2_walk_begin(pg->e2, &expr, walk);
    if (st != ODIN3_OK) {
        odin3_vec_pop(&pg->walks);
        return st;
    }
    pg_frame frame = {PG_F_GEN, req->node, req->cont, req->aux, 0, 0};
    st = pg_frame_push(pg, &frame);
    if (st != ODIN3_OK) {
        odin3_elab2_walk_abort(pg->e2, walk);
        odin3_vec_pop(&pg->walks);
    }
    return st;
}

/* An actual is generated: it is extended to its formal's width and kept. */
static odin3_status gen_arg(odin3_pg *pg, uint32_t index) {
    pg_crun *cr = crun_top(pg);
    odin3_sig *arg = &cr->args[index];
    const odin3_symbol *rec = formal_rec(cr, index);
    uint32_t width = rec->has_packed ? (uint32_t)odin3_elab2_dim_size(&rec->packed) : 1U;
    *arg = pg->gen_sig;
    return odin3_elab2_extend(pg->e2, arg, width, arg->is_signed, arg);
}

/* What runs once the value is in pg->gen_sig; the frame is already popped. */
static odin3_status gen_done(odin3_pg *pg, const pg_frame *frame) {
    switch (frame->next) {
    case PG_G_ASSIGN:
        return pg_assign_done(pg, frame->node, frame->end);
    case PG_G_IF:
        return pg_if_done(pg, frame->node);
    default:
        return gen_arg(pg, frame->end);
    }
}

odin3_status pg_gen_step(odin3_pg *pg) {
    odin3_elab2_walk *walk = odin3_vec_at(&pg->walks, pg->walks.len - 1);
    odin3_status st = odin3_elab2_walk_run(pg->e2, walk);
    if (st != ODIN3_OK) {
        return st;
    }
    if (walk->call.v != 0) {
        return call_push(pg, walk->call, true);
    }
    pg_frame frame = *(const pg_frame *)odin3_vec_cat(&pg->frames, pg->frames.len - 1);
    odin3_elab2_walk_take(pg->e2, walk, &pg->gen_sig);
    odin3_vec_pop(&pg->walks);
    odin3_vec_pop(&pg->frames);
    return gen_done(pg, &frame);
}

void pg_frame_abort(odin3_pg *pg, const pg_frame *frame) {
    if (frame->kind == PG_F_GEN) {
        odin3_elab2_walk_abort(pg->e2, odin3_vec_at(&pg->walks, pg->walks.len - 1));
        odin3_vec_pop(&pg->walks);
    } else if (frame->kind == PG_F_CALL) {
        call_unwind(pg);
    }
}

/* --- the entries --------------------------------------------------------------------------------
 */

/* One more level of C-recursive call nesting (a nested frame loop, or a call in a call's actuals
 * in a continuous context), or the located resource error at the cap; sync_leave undoes it. */
static odin3_status sync_enter(odin3_pg *pg, odin3_ast_id call) {
    if (pg->cdepth >= PG_SYNC_NEST_MAX) {
        odin3_elab2_fatal(pg->e2, odin3_ast_loc(pg->e2->elab, call),
                          "function call nesting deeper than %u at '%s'",
                          (unsigned)PG_SYNC_NEST_MAX, odin3_ast_name_str(pg->e2->elab, call));
        return ODIN3_ERR_PARSE;
    }
    pg->cdepth++;
    return ODIN3_OK;
}

/* A call run whole before it returns, by a nested frame loop (C recursion, capped). */
static odin3_status call_sync(odin3_pg *pg, odin3_ast_id call, odin3_sig *result) {
    odin3_status st = sync_enter(pg, call);
    if (st != ODIN3_OK) {
        return st;
    }
    size_t base = pg->frames.len;
    st = call_push(pg, call, false);
    st = st != ODIN3_OK ? st : pg_drive(pg, base);
    pg->cdepth--;
    if (st == ODIN3_OK && result != NULL) {
        st = pg->e2->stopped ? ODIN3_ERR_PARSE : ODIN3_OK; /* stopped: there is no value */
        *result = pg->call_sig;
    }
    return st;
}

odin3_status odin3_pg_call(odin3_pg *pg, odin3_ast_id call, odin3_sig *result) {
    odin3_ast_kind kind = odin3_ast_kind_of(pg->e2->elab, call);
    if (kind != ODIN3_AST_CALL && kind != ODIN3_AST_TASK_CALL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pg_call: not a CALL or TASK_CALL");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (kind == ODIN3_AST_CALL && result == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pg_call: a function call needs a result");
        return ODIN3_ERR_INVALID_ARG;
    }
    return call_sync(pg, call, result);
}

odin3_status pg_task_call(odin3_pg *pg, odin3_ast_id call) {
    if ((odin3_ast_flags(pg->e2->elab, call) & ODIN3_AST_F_CALL_SYSTEM) != 0) {
        return ODIN3_OK; /* the $display family was removed by stage 1 (S3) */
    }
    return call_push(pg, call, false);
}

odin3_status pg_call_hook(void *user, odin3_ast_id call, odin3_sig *out) {
    return odin3_pg_call(user, call, out);
}

/* --- a call outside a process ------------------------------------------------------------------
 */

/* The result variable's bits as the block's only outputs (one chunk), the call as their place. */
static odin3_status comb_outputs(odin3_pg *pg, odin3_wire_id wire, odin3_ast_id node) {
    uint32_t width = odin3_wire_width(pg->e2->mod, wire);
    for (uint32_t bit = 0; bit < width; bit++) {
        pg_out *out = odin3_vec_push(&pg->outs);
        if (out == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *out = (pg_out){odin3_wire_net(pg->e2->mod, wire, bit), {0}, {wire, bit}, node, true, 0};
    }
    return ODIN3_OK;
}

/* The block of a call outside a process: no ALWAYS, the call's place, an empty builder. */
static odin3_status comb_block(odin3_pg *pg, odin3_ast_id node) {
    odin3_status st = pg_block_reset(pg);
    pg->always = node;
    pg->control = (odin3_ast_id){0};
    pg->body = (odin3_ast_id){0};
    odin3_elab2_prov_req req = {node, {0}, pg->e2->scope.v};
    return st != ODIN3_OK ? st : odin3_elab2_prov(pg->e2, &req, &pg->prov);
}

/* The nets of a wire as a signal. */
static odin3_status wire_sig(odin3_elab2 *e2, odin3_wire_id wire, bool is_signed, odin3_sig *out) {
    uint32_t width = odin3_wire_width(e2->mod, wire);
    odin3_status st = odin3_elab2_sig_new(e2, width, out);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < width; bit++) {
        out->bits[bit] = odin3_wire_net(e2->mod, wire, bit).v;
    }
    out->is_signed = is_signed;
    return st;
}

static odin3_status comb_run(pg_crun *cr, odin3_sig *result) {
    odin3_pg *pg = cr->pg;
    odin3_elab2 *e2 = pg->e2;
    odin3_status st = comb_block(pg, cr->info.call);
    /* the variables of the module the function assigns are outputs of the block, with the result
     * (found before the frame gives the function's own variables wires: those are not) */
    st = st != ODIN3_OK || cr->info.body.v == 0 ? st : pg_scan(pg, cr->info.body);
    st = st != ODIN3_OK ? st : frame_enter(cr);
    odin3_sym_id sym = {0};
    const odin3_symbol *res =
        st == ODIN3_OK ? odin3_elab2_fn_result(e2, cr->info.decl, &sym) : NULL;
    if (st == ODIN3_OK && res == NULL) {
        return ODIN3_ERR_CHECK;
    }
    odin3_wire_id wire = res != NULL ? res->ir_wire : (odin3_wire_id){0};
    st = st != ODIN3_OK ? st : comb_outputs(pg, wire, cr->info.call);
    st = st != ODIN3_OK ? st : pg_start_block(pg);
    st = st != ODIN3_OK ? st : call_bind(cr);
    st = st != ODIN3_OK ? st : call_body(cr);
    odin3_node_id node = {0};
    st = st != ODIN3_OK ? st : odin3_pg_emit(pg, cr->info.call, &node);
    return st != ODIN3_OK ? st : wire_sig(e2, wire, (res->flags & ODIN3_SYM_F_SIGNED) != 0, result);
}

/* The call's block on a builder of its own: the open block (an edge event's expression being
 * generated while its ALWAYS is built) is put aside and brought back, never cleared (I7). */
static odin3_status comb_nested(pg_crun *cr, odin3_sig *result) {
    odin3_pg *pg = cr->pg;
    pg_saved saved;
    odin3_status st = pg_block_save(pg, &saved);
    st = st != ODIN3_OK ? st : comb_run(cr, result);
    while (pg->calls.len > 0) {
        frame_exit(pg);
    }
    pg_block_restore(pg, &saved);
    return st;
}

odin3_status odin3_pg_comb_call(odin3_pg *pg, odin3_ast_id call, odin3_sig *result) {
    if (odin3_ast_kind_of(pg->e2->elab, call) != ODIN3_AST_CALL || result == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pg_comb_call: not a CALL, or no result");
        return ODIN3_ERR_INVALID_ARG;
    }
    pg_crun cr = {.pg = pg};
    cr.info.call = call;
    odin3_status st = info_make(pg, call, &cr.info);
    st = st != ODIN3_OK ? st : info_check(pg, &cr.info);
    st = st != ODIN3_OK ? st : formals_make(&cr);
    st = st != ODIN3_OK ? st : sync_enter(pg, call); /* the actuals nest calls by C stack (M9) */
    if (st != ODIN3_OK) {
        return st;
    }
    st = call_args(&cr, NULL);
    pg->cdepth--;
    return st != ODIN3_OK ? st : comb_nested(&cr, result);
}

odin3_status odin3_elab2_comb_call(odin3_elab2 *e2, odin3_ast_id call, odin3_sig *out) {
    odin3_pg *pg = NULL;
    odin3_status st = odin3_elab2_pg_get(e2, &pg);
    return st != ODIN3_OK ? st : odin3_pg_comb_call(pg, call, out);
}

/* --- the words of a register array with a variable index ------------------------------------- */

/* Whether the declared index value is one the index signal (its width and sign) can hold. */
static bool value_fits(int64_t value, const odin3_sig *idx) {
    if (idx->width >= SIGN_BITS) {
        return true;
    }
    int64_t span = (int64_t)1 << idx->width;
    return idx->is_signed ? value >= -(span / 2) && value < span / 2 : value >= 0 && value < span;
}

/* The constant of value as the index's width of two's complement bits. */
static odin3_status value_sig(odin3_elab2 *e2, int64_t value, const odin3_sig *idx,
                              odin3_sig *out) {
    odin3_status st = odin3_elab2_sig_new(e2, idx->width, out);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < idx->width; bit++) {
        bool one = bit < SIGN_BITS ? ((value >> bit) & 1) != 0 : value < 0;
        out->bits[bit] = odin3_sb_of_bit(one ? ODIN3_BIT_1 : ODIN3_BIT_0);
    }
    return st;
}

/* The case over the words as the switch of EL-36 builds it: what each arm assigns, and where. */
typedef struct regs_run {
    odin3_pg *pg;
    odin3_ast_id node;
    uint32_t mem;
    const odin3_sig *idx;
    const odin3_sig *value;      /* a write: the word's new value; a read: NULL */
    const odin3_net_id *dst;     /* a read: the nets that get the word */
    const odin3_sigbit *targets; /* the bits the arms assign by `=`, sorted (blocking only) */
    uint32_t n_targets;
    bool blocking;
} regs_run;

/* A read's arm: dst := word pos's current value; x for the default (pos = SIZE_MAX) or a word
 * that has no wire. */
static odin3_status regs_read_arm(const regs_run *rr, size_t pos) {
    odin3_pg *pg = rr->pg;
    const odin3_elab_mem *mem = odin3_elab2_mem_get(pg->e2, rr->mem);
    odin3_wire_id wire = pos < mem->words.len
                             ? *(const odin3_wire_id *)odin3_vec_cat(&mem->words, pos)
                             : (odin3_wire_id){0};
    odin3_sig value;
    odin3_status st = wire.v != 0 ? read_wire(pg, wire, mem->is_signed, &value)
                                  : odin3_elab2_sig_new(pg->e2, mem->width, &value);
    for (uint32_t i = 0; wire.v == 0 && st == ODIN3_OK && i < mem->width; i++) {
        value.bits[i] = odin3_sb_of_bit(ODIN3_BIT_X);
    }
    net_asg na = {rr->dst, mem->width, &value, rr->node, rr->blocking};
    return st != ODIN3_OK ? st : assign_nets(pg, &na);
}

/* A write's arm: the word pos := the value; nothing for the default. */
static odin3_status regs_write_arm(const regs_run *rr, size_t pos) {
    odin3_pg *pg = rr->pg;
    const odin3_elab_mem *mem = odin3_elab2_mem_get(pg->e2, rr->mem);
    if (pos >= mem->words.len || ((const odin3_wire_id *)mem->words.data)[pos].v == 0) {
        return ODIN3_OK;
    }
    net_asg proto = {NULL, 0, rr->value, rr->node, rr->blocking};
    return assign_wire(pg, *(const odin3_wire_id *)odin3_vec_cat(&mem->words, pos), &proto);
}

static odin3_status regs_arm(const regs_run *rr, size_t pos) {
    return rr->value == NULL ? regs_read_arm(rr, pos) : regs_write_arm(rr, pos);
}

/* The default case: a read gives x; a blocking write holds the case temporaries (open and closed
 * empty). A non-blocking write has none. */
static odin3_status regs_default(const regs_run *rr) {
    bool read = rr->value == NULL;
    if (!read && !rr->blocking) {
        return ODIN3_OK;
    }
    odin3_pg_case_req held = {rr->node, NULL, 0};
    odin3_status st = pg_sw_case(rr->pg, &held, false);
    st = st != ODIN3_OK || !read ? st : regs_read_arm(rr, SIZE_MAX);
    if (st == ODIN3_OK) {
        pg_sw_case_end(rr->pg);
    }
    return st;
}

static odin3_status regs_switch(const regs_run *rr) {
    odin3_pg *pg = rr->pg;
    const odin3_elab_mem *mem = odin3_elab2_mem_get(pg->e2, rr->mem);
    pg_sw_req sw = {rr->node, rr->idx,        rr->targets,        rr->n_targets,
                    0,        rr->idx->width, rr->idx->is_signed, PG_WILD_NONE};
    odin3_status st = pg_sw_begin(pg, &sw);
    for (size_t pos = 0; st == ODIN3_OK && pos < mem->words.len; pos++) {
        int64_t value = (int64_t)mem->offset + (int64_t)pos;
        if (!value_fits(value, rr->idx)) {
            continue;
        }
        odin3_sig cmp;
        st = value_sig(pg->e2, value, rr->idx, &cmp);
        odin3_pg_case_req item = {rr->node, &cmp, 1};
        st = st != ODIN3_OK ? st : pg_sw_case(pg, &item, false);
        st = st != ODIN3_OK ? st : regs_arm(rr, pos);
        if (st == ODIN3_OK) {
            pg_sw_case_end(pg);
        }
    }
    st = st != ODIN3_OK ? st : regs_default(rr);
    return st != ODIN3_OK ? st : pg_sw_end(pg);
}

/* A read inside a process: a temporary of the word's width gets the word by a case with case
 * temporaries; the value is the temporary's rv after the switch. */
odin3_status pg_word_hook(void *user, const odin3_elab2_regs_rd *rd, odin3_sig *out) {
    odin3_pg *pg = user;
    const odin3_elab_mem *mem = odin3_elab2_mem_get(pg->e2, rd->mem);
    odin3_net_id first = {0};
    odin3_status st = odin3_pg_temp(pg, mem->width, rd->select, &first);
    odin3_net_id *dst =
        st == ODIN3_OK ? odin3_arena_alloc(pg->e2->scratch, mem->width * sizeof *dst) : NULL;
    odin3_sigbit *bits =
        dst != NULL ? odin3_arena_alloc(pg->e2->scratch, mem->width * sizeof *bits) : NULL;
    if (st == ODIN3_OK && bits == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < mem->width; i++) {
        dst[i] = (odin3_net_id){first.v + i};
        bits[i] = first.v + i;
        st = pg_lv_set(pg, dst[i], dst[i]);
        st = st != ODIN3_OK ? st : pg_rv_set(pg, dst[i], odin3_sb_of_bit(ODIN3_BIT_X));
    }
    regs_run rr = {pg, rd->select, rd->mem, rd->idx, NULL, dst, bits, mem->width, true};
    st = st != ODIN3_OK ? st : regs_switch(&rr);
    st = st != ODIN3_OK ? st : odin3_elab2_sig_new(pg->e2, mem->width, out);
    for (uint32_t i = 0; st == ODIN3_OK && i < mem->width; i++) {
        out->bits[i] = dst[i].v;
        (void)pg_rv_get(pg, dst[i], &out->bits[i]);
    }
    if (st == ODIN3_OK) {
        out->is_signed = mem->is_signed;
    }
    return st;
}

/* The assignment of a word with a variable index (EL-27): the case over the words; no default
 * for `<=`, the held default for `=` (an index past the array changes nothing). */
odin3_status pg_regs_apply(odin3_pg *pg, const odin3_pg_assign_req *req) {
    const odin3_elab2_regsel regs = pg->lvs.regs;
    pg_asg mixed = {&pg->lvs, req->rhs, req->stmt, req->blocking, false};
    odin3_elab2_reads reads = pg_reads(pg);
    odin3_elab2_expr_req ireq = {regs.index, {0, false, false}, NULL, &reads};
    odin3_sig idx;
    pg_mem_ref wref = {regs.mem, NULL, req->stmt, req->blocking};
    odin3_status st = pg_check_mixed(pg, &mixed);
    st = st != ODIN3_OK ? st : pg_mem_check_mixed(pg, &wref);
    st = st != ODIN3_OK ? st : odin3_elab2_expr(pg->e2, &ireq, &idx);
    size_t count = pg->lvs.nets.len;
    odin3_sigbit *targets =
        st == ODIN3_OK ? odin3_arena_alloc(pg->e2->scratch, (count + 1U) * sizeof *targets) : NULL;
    if (st == ODIN3_OK && targets == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; st == ODIN3_OK && i < count; i++) {
        targets[i] = ((const odin3_net_id *)odin3_vec_cat(&pg->lvs.nets, i))->v;
    }
    regs_run rr = {pg,   regs.node, regs.mem,        &idx,         req->rhs,
                   NULL, targets,   (uint32_t)count, req->blocking};
    rr.n_targets = req->blocking ? (uint32_t)count : 0U;
    return st != ODIN3_OK ? st : regs_switch(&rr);
}

/* A word read outside a process: a `$proc` of its own whose output is a fresh wire; the nets
 * are the value. */
/* The unnamed wire a word read process drives. */
static odin3_status word_wire(odin3_elab2 *e2, const odin3_elab2_regs_rd *rd, odin3_wire_id *wire) {
    const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, rd->mem);
    uint32_t width = mem->width;
    odin3_wire_spec spec = {.msb = (int32_t)width - 1, .lsb = 0, .is_signed = mem->is_signed};
    odin3_elab2_prov_req preq = {rd->select, {0}, e2->scope.v};
    odin3_status st = odin3_elab2_reserve_nets(e2, width, odin3_ast_loc(e2->elab, rd->select));
    st = st != ODIN3_OK ? st : odin3_elab2_prov(e2, &preq, &spec.prov);
    st = st != ODIN3_OK ? st : odin3_wire_create(e2->mod, &spec, NULL, wire);
    odin3_elab2_counts made = {0, 0, width, 1};
    return st != ODIN3_OK ? st : odin3_elab2_account(e2, &made);
}

/* The block of a word read: the wire is its only output, the case assigns it, the node is made. */
static odin3_status word_block(odin3_pg *pg, const odin3_elab2_regs_rd *rd, odin3_wire_id wire) {
    odin3_elab2 *e2 = pg->e2;
    uint32_t width = odin3_wire_width(e2->mod, wire);
    odin3_net_id *dst = odin3_arena_alloc(e2->scratch, width * sizeof *dst);
    odin3_status st = dst != NULL ? comb_block(pg, rd->select) : ODIN3_ERR_NO_MEMORY;
    st = st != ODIN3_OK ? st : comb_outputs(pg, wire, rd->select);
    st = st != ODIN3_OK ? st : pg_start_block(pg);
    for (uint32_t bit = 0; st == ODIN3_OK && bit < width; bit++) {
        dst[bit] = odin3_wire_net(e2->mod, wire, bit);
    }
    regs_run rr = {pg, rd->select, rd->mem, rd->idx, NULL, dst, NULL, 0, false};
    st = st != ODIN3_OK ? st : regs_switch(&rr);
    odin3_node_id node = {0};
    return st != ODIN3_OK ? st : odin3_pg_emit(pg, rd->select, &node);
}

odin3_status odin3_elab2_comb_word(odin3_elab2 *e2, const odin3_elab2_regs_rd *rd, odin3_sig *out) {
    odin3_pg *pg = NULL;
    odin3_wire_id wire = {0};
    odin3_status st = odin3_elab2_pg_get(e2, &pg);
    st = st != ODIN3_OK ? st : word_wire(e2, rd, &wire);
    pg_saved saved;
    bool saving = st == ODIN3_OK;
    st = saving ? pg_block_save(pg, &saved) : st; /* the open block is put aside (I7) */
    st = st != ODIN3_OK ? st : word_block(pg, rd, wire);
    if (saving) {
        pg_block_restore(pg, &saved);
    }
    return st != ODIN3_OK ? st
                          : wire_sig(e2, wire, odin3_elab2_mem_get(e2, rd->mem)->is_signed, out);
}

odin3_status odin3_pg_comb_mem_read(odin3_pg *pg, odin3_ast_id select, odin3_sig *result) {
    odin3_elab2_wordref ref;
    if (result == NULL || !odin3_elab2_mem_word_of(pg->e2, select, &ref) || ref.n != 1) {
        odin3_log(ODIN3_LOG_ERROR,
                  "odin3_pg_comb_mem_read: not a word of a one-dimensional register array");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_sig idx;
    odin3_elab2_expr_req req = {
        odin3_elab2_mem_word_index(pg->e2, select, 0), {0, false, false}, NULL, NULL};
    odin3_status st = odin3_elab2_expr(pg->e2, &req, &idx);
    const odin3_elab2_regs_rd rd = {select, ref.mem, &idx};
    return st != ODIN3_OK ? st : odin3_elab2_comb_word(pg->e2, &rd, result);
}
