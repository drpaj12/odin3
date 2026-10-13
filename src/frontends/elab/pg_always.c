/*
 * pg_always.c — one `always` block through the process generator (2D Task 21; EL-34, EL-35 steps
 * 2-5): the outputs with their temporaries and chunks, the syncs of the event list (and T3's
 * analysis of a level list), the root defaults, and the statements through the frame stack (an
 * `if` or `case` is a frame of pg_case.c's). The scan that finds the outputs is pg_scan.c, the
 * builder pg.c and the node pg_emit.c.
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
#include "util/log.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { PG_CHUNK_SHIFT = 32 };

/* --- the outputs, their temporaries and chunks (steps 1-2) ------------------------------------ */

static int out_compare(const void *lhs, const void *rhs) {
    uint32_t first = ((const pg_out *)lhs)->net.v;
    uint32_t second = ((const pg_out *)rhs)->net.v;
    return (first > second) - (first < second);
}

bool pg_out_continues(const pg_out *prev, const pg_out *out) {
    return prev->wb.wire.v != 0 && prev->wb.wire.v == out->wb.wire.v &&
           out->wb.bit == prev->wb.bit + 1U;
}

/* The sorted outputs split into chunks; every output knows its chunk, and the temporaries of one
 * chunk are one group (position = the bit's place in the chunk). */
static odin3_status make_chunks(odin3_pg *pg) {
    pg_out *outs = pg->outs.data;
    odin3_vec_clear(&pg->chunks);
    for (uint32_t i = 0; i < pg->outs.len; i++) {
        bool same = i > 0 && pg_out_continues(&outs[i - 1U], &outs[i]);
        if (!same) {
            pg_chunk *chunk = odin3_vec_push(&pg->chunks);
            if (chunk == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            *chunk = (pg_chunk){i, 0};
        }
        pg_chunk *chunk = odin3_vec_at(&pg->chunks, pg->chunks.len - 1);
        outs[i].chunk = (uint32_t)pg->chunks.len - 1U;
        chunk->len++;
    }
    return ODIN3_OK;
}

/* The temporary `t0` of every output, created in the sorted order (Yosys's `$0\x`). */
static odin3_status make_t0(odin3_pg *pg) {
    odin3_elab2 *e2 = pg->e2;
    pg_out *outs = pg->outs.data;
    odin3_status st =
        odin3_elab2_reserve_nets(e2, (uint32_t)pg->outs.len, odin3_ast_loc(e2->elab, pg->always));
    uint32_t group_base = pg->groups;
    pg->groups += (uint32_t)pg->chunks.len;
    for (uint32_t i = 0; st == ODIN3_OK && i < pg->outs.len; i++) {
        const pg_chunk *chunk = odin3_vec_cat(&pg->chunks, outs[i].chunk);
        uint64_t group = (uint64_t)group_base + outs[i].chunk + 1U;
        st = odin3_net_create(e2->mod, 0, pg->prov, &outs[i].t0);
        st = st != ODIN3_OK
                 ? st
                 : odin3_u64map_put(pg->group, (odin3_kv){outs[i].t0.v, (group << PG_CHUNK_SHIFT) |
                                                                            (i - chunk->first)});
        st = st != ODIN3_OK ? st : pg_lv_set(pg, outs[i].net, outs[i].t0);
    }
    odin3_elab2_counts made = {0, 0, (uint32_t)pg->outs.len, 0};
    return st != ODIN3_OK ? st : odin3_elab2_account(e2, &made);
}

/* The outputs sorted by net ID, the maps rebuilt, the chunks and the temporaries made. */
static odin3_status pg_outputs(odin3_pg *pg) {
    qsort(pg->outs.data, pg->outs.len, sizeof(pg_out), out_compare);
    const pg_out *outs = pg->outs.data;
    for (uint32_t i = 0; i < pg->outs.len; i++) {
        odin3_status st = odin3_u64map_put(pg->out_of, (odin3_kv){outs[i].net.v, (uint64_t)i + 1U});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    odin3_status st = make_chunks(pg);
    return st != ODIN3_OK ? st : make_t0(pg);
}

/* --- syncs (EL-34) -----------------------------------------------------------------------------
 */

/* The name an event expression is known by in the error. */
static const char *event_name(const odin3_elab2 *e2, odin3_ast_id expr) {
    while (odin3_ast_kind_of(e2->elab, expr) == ODIN3_AST_SELECT) {
        expr = odin3_ast_child(e2->elab, expr, 0);
    }
    odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, expr);
    return kind == ODIN3_AST_IDENT || kind == ODIN3_AST_HIER_NAME
               ? odin3_ast_name_str(e2->elab, expr)
               : "expression";
}

/* Where a chunk's action goes (a case or a sync) and which way it points. */
typedef struct chunk_to {
    uint32_t owner, sync;
    bool hold; /* t0(chunk) = chunk (a root default); else chunk = t0(chunk) (a sync update) */
} chunk_to;

/* One action over the outputs of chunk `index` and their temporaries. */
static odin3_status chunk_action(odin3_pg *pg, uint32_t index, const chunk_to *to) {
    const pg_out *outs = pg->outs.data;
    const pg_chunk *chunk = odin3_vec_cat(&pg->chunks, index);
    odin3_vec_clear(&pg->run_l);
    odin3_vec_clear(&pg->run_r);
    for (uint32_t i = chunk->first; i < chunk->first + chunk->len; i++) {
        odin3_sigbit *left = odin3_vec_push(&pg->run_l);
        odin3_sigbit *right = left != NULL ? odin3_vec_push(&pg->run_r) : NULL;
        if (right == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *left = to->hold ? outs[i].t0.v : outs[i].net.v;
        *right = to->hold ? outs[i].net.v : outs[i].t0.v;
    }
    pg_act_req req = {to->owner, to->sync, pg->run_l.data, pg->run_r.data, chunk->len, pg->prov};
    return pg_act_add(pg, &req);
}

/* One action per output chunk, `chunk <- t0(chunk)`, for the sync at index sync. */
static odin3_status sync_actions(odin3_pg *pg, uint32_t sync) {
    for (uint32_t chunk = 0; chunk < pg->chunks.len; chunk++) {
        odin3_status st = chunk_action(pg, chunk, &(chunk_to){PG_SYNC_OWNER, sync, false});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* A sync of the given type on signal, with its actions. */
static odin3_status sync_add(odin3_pg *pg, uint8_t type, odin3_sigbit signal) {
    pg_sync *sync = odin3_vec_push(&pg->syncs);
    if (sync == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *sync = (pg_sync){type, signal, 0, 0};
    return sync_actions(pg, (uint32_t)pg->syncs.len - 1U);
}

/* The sync of one edge event: its expression generated as cells (it may be any 1-bit expression)
 * and one bit wide. */
static odin3_status sync_edge(odin3_pg *pg, odin3_ast_id event) {
    odin3_elab2 *e2 = pg->e2;
    odin3_ast_id expr = odin3_ast_child(e2->elab, event, 0);
    odin3_elab2_expr_req req = {expr, {0, false, false}, NULL, NULL};
    odin3_sig sig;
    odin3_status st = odin3_elab2_expr(e2, &req, &sig);
    if (st != ODIN3_OK) {
        return st;
    }
    bool pos = odin3_ast_sub(e2->elab, event) == ODIN3_AST_EVENT_EXPR_POSEDGE;
    if (sig.width != 1) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, event),
                          "%s on '%s', which is %u bits; a 1-bit signal is needed",
                          pos ? "posedge" : "negedge", event_name(e2, expr), sig.width);
        return ODIN3_ERR_PARSE;
    }
    pg->edge = true;
    return sync_add(pg, pos ? ODIN3_SYNC_POSEDGE : ODIN3_SYNC_NEGEDGE, sig.bits[0]);
}

/* --- T3: the signals a level event list names ------------------------------------------------ */

static odin3_status list_wire(odin3_pg *pg, odin3_wire_id wire) {
    uint32_t width = odin3_wire_width(pg->e2->mod, wire);
    for (uint32_t bit = 0; bit < width; bit++) {
        odin3_status st =
            odin3_u64map_put(pg->listed, (odin3_kv){odin3_wire_net(pg->e2->mod, wire, bit).v, 1});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Every word of a register array. */
static odin3_status list_words(odin3_pg *pg, uint32_t mem_index) {
    const odin3_elab_mem *mem = odin3_elab2_mem_get(pg->e2, mem_index);
    for (size_t k = 0; k < mem->words.len; k++) {
        odin3_wire_id wire = *(const odin3_wire_id *)odin3_vec_cat(&mem->words, k);
        odin3_status st = wire.v != 0 ? list_wire(pg, wire) : ODIN3_OK;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* The nets of the signals one level event expression names. */
static odin3_status listed_expr(odin3_pg *pg, odin3_ast_id expr) {
    odin3_elab2 *e2 = pg->e2;
    odin3_ast_cursor cur;
    odin3_status st = odin3_ast_cursor_init(&cur, e2->elab, expr);
    odin3_ast_event ev;
    odin3_ast_id node;
    while (st == ODIN3_OK && odin3_ast_cursor_next(&cur, &ev, &node)) {
        odin3_ast_kind kind = odin3_ast_kind_of(e2->elab, node);
        if (ev != ODIN3_AST_ENTER || (kind != ODIN3_AST_IDENT && kind != ODIN3_AST_HIER_NAME)) {
            continue;
        }
        odin3_sym_id sym = odin3_elab_sym_of(&e2->bind, node);
        const odin3_symbol *rec = odin3_symbol_get(e2->st, sym);
        uint32_t mem = rec != NULL ? odin3_elab2_mem_find(e2, sym) : MEM_NONE;
        if (mem != MEM_NONE) {
            st = list_words(pg, mem);
        } else if (rec != NULL && rec->ir_wire.v != 0) {
            st = list_wire(pg, rec->ir_wire);
        }
    }
    odin3_ast_cursor_free(&cur);
    return st;
}

/* What the event list is: nothing or `*` (star), edges, or levels. */
typedef struct pg_events {
    odin3_ast_span list;
    uint32_t first;
    bool edges, levels;
} pg_events;

static pg_events classify_events(const odin3_pg *pg) {
    const odin3_elab2 *e2 = pg->e2;
    pg_events ev = {{NULL, 0}, odin3_ast_slot_count(ODIN3_AST_EVENT_CONTROL), false, false};
    if (pg->control.v == 0) {
        return ev;
    }
    ev.list = odin3_ast_children(e2->elab, pg->control);
    for (uint32_t i = ev.first; i < ev.list.n; i++) {
        bool level = odin3_ast_sub(e2->elab, ev.list.ids[i]) == ODIN3_AST_EVENT_EXPR_NONE;
        ev.edges |= !level;
        ev.levels |= level;
    }
    return ev;
}

/* The syncs of the event control: one per edge event, or one ALWAYS. */
static odin3_status pg_syncs(odin3_pg *pg) {
    odin3_elab2 *e2 = pg->e2;
    pg_events ev = classify_events(pg);
    if (ev.edges && ev.levels) {
        odin3_elab2_error(e2, odin3_ast_loc(e2->elab, pg->control),
                          "edge and level events in one event list are not supported");
        return ODIN3_ERR_PARSE;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t i = ev.first; st == ODIN3_OK && i < ev.list.n; i++) {
        st = ev.edges ? sync_edge(pg, ev.list.ids[i])
                      : listed_expr(pg, odin3_ast_child(e2->elab, ev.list.ids[i], 0));
    }
    pg->track_reads = ev.levels;
    return st != ODIN3_OK || ev.edges ? st : sync_add(pg, ODIN3_SYNC_ALWAYS, 0);
}

/* T3: a level list that omits a signal the block reads (a read the block's own assignments did
 * not supply): once, naming the first such signal by name. */
static void pg_incomplete(odin3_pg *pg) {
    if (!pg->track_reads) {
        return;
    }
    const odin3_elab2 *e2 = pg->e2;
    const char *first = NULL;
    size_t cursor = 0;
    odin3_kv entry;
    while (odin3_u64map_next(pg->reads, &cursor, &entry)) {
        if (odin3_u64map_get(pg->listed, entry.key, NULL)) {
            continue;
        }
        odin3_wirebit wb = odin3_net_primary(e2->mod, (odin3_net_id){(uint32_t)entry.key});
        const char *name = wb.wire.v != 0 ? odin3_strtab_get(odin3_design_strtab(e2->design),
                                                             odin3_wire_name(e2->mod, wb.wire))
                                          : NULL;
        if (name != NULL && (first == NULL || strcmp(name, first) < 0)) {
            first = name;
        }
    }
    if (first != NULL) {
        odin3_elab2_warn(pg->e2, odin3_ast_loc(e2->elab, pg->always),
                         "incomplete sensitivity list: '%s' is read but not listed; the block is "
                         "built as @*",
                         first);
    }
}

/* --- root defaults (step 3) ----------------------------------------------------------------- */

/* One root action per output chunk, `t0(chunk) = chunk` (the value holds); with --nolatches and no
 * edge sync none, and an output reads x until it is assigned. */
static odin3_status pg_defaults(odin3_pg *pg) {
    const pg_out *outs = pg->outs.data;
    if (pg->e2->opts.nolatches && !pg->edge) {
        odin3_status st = ODIN3_OK;
        for (uint32_t i = 0; st == ODIN3_OK && i < pg->outs.len; i++) {
            st = pg_rv_set(pg, outs[i].net, odin3_sb_of_bit(ODIN3_BIT_X));
        }
        return st;
    }
    for (uint32_t chunk = 0; chunk < pg->chunks.len; chunk++) {
        odin3_status st = chunk_action(pg, chunk, &(chunk_to){PG_ROOT, 0, true});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* --- statements (step 5) -------------------------------------------------------------------- */

odin3_status pg_read_bit(void *user, odin3_net_id net, odin3_sigbit *out) {
    odin3_pg *pg = user;
    if (pg_rv_get(pg, net, out)) {
        return ODIN3_OK;
    }
    return pg->track_reads ? odin3_u64map_put(pg->reads, (odin3_kv){net.v, 1}) : ODIN3_OK;
}

/* `lhs = rhs` / `lhs <= rhs`: the target, the value at max(L(lhs), L(rhs)) through rv, truncated
 * or extended to L(lhs), then the assignment (a select with a non-constant index: the case
 * method, whose width is the select's). */
odin3_elab2_reads pg_reads(odin3_pg *pg) {
    return (odin3_elab2_reads){pg_read_bit, pg, pg_call_hook, pg_word_hook};
}

/* The error of an assignment to the variable of a loop being unrolled (EL-37). */
static odin3_status loop_var_assigned(odin3_pg *pg, odin3_ast_id stmt) {
    const odin3_ast *ast = pg->e2->elab;
    odin3_ast_id lhs = odin3_ast_child(ast, stmt, 0);
    while (odin3_ast_kind_of(ast, lhs) == ODIN3_AST_SELECT) {
        lhs = odin3_ast_child(ast, lhs, 0);
    }
    odin3_sym_id sym = odin3_elab_sym_of(&pg->e2->bind, lhs);
    for (size_t i = 0; sym.v != 0 && i < pg->loops.len; i++) {
        const pg_loop *loop = odin3_vec_cat(&pg->loops, i);
        bool header = odin3_ast_child(ast, loop->node, 0).v == stmt.v ||
                      odin3_ast_child(ast, loop->node, 2).v == stmt.v;
        if (loop->sym.v == sym.v && !header) {
            odin3_elab2_error(pg->e2, odin3_ast_loc(ast, stmt),
                              "loop variable '%s' is assigned in the loop body",
                              odin3_ast_name_str(ast, lhs));
            return ODIN3_ERR_PARSE;
        }
    }
    return ODIN3_OK;
}

odin3_status pg_exec_assign(odin3_pg *pg, odin3_ast_id stmt) {
    odin3_elab2 *e2 = pg->e2;
    odin3_status st = loop_var_assigned(pg, stmt);
    st = st != ODIN3_OK ? st : pg_take_target_of(pg, odin3_ast_child(e2->elab, stmt, 0));
    if (st != ODIN3_OK) {
        return st;
    }
    /* the value's calls run above this statement with a scratch lvalue of their own (pg_call.c) */
    bool variable = pg->lvs.var.node.v != 0;
    bool regs = pg->lvs.regs.node.v != 0;
    uint32_t width = variable ? pg->lvs.var.width : (uint32_t)pg->lvs.nets.len;
    if (regs) {
        const odin3_elab_mem *mem = odin3_elab2_mem_get(e2, pg->lvs.regs.mem);
        width = mem->width;
    }
    odin3_elab2_reads reads = pg_reads(pg);
    odin3_elab2_expr_req req = {
        odin3_ast_child(e2->elab, stmt, 2), {width, true, false}, NULL, &reads};
    odin3_sig sig;
    st = odin3_elab2_expr(e2, &req, &sig);
    st = st != ODIN3_OK ? st : odin3_elab2_extend(e2, &sig, width, sig.is_signed, &sig);
    if (st != ODIN3_OK) {
        return st;
    }
    bool blocking = odin3_ast_kind_of(e2->elab, stmt) == ODIN3_AST_BLOCKING_ASSIGN;
    odin3_pg_assign_req areq = {stmt, odin3_ast_child(e2->elab, stmt, 0), &sig, blocking};
    pg_asg asg = {&pg->lvs, &sig, stmt, blocking, false};
    if (regs) {
        return pg_regs_apply(pg, &areq);
    }
    return variable ? pg_select_apply(pg, &areq) : pg_apply(pg, &asg);
}

/* A kind neither built nor pending is a 2D bug (the stage-1 counterpart is odin3_elab1_unhandled).
 */
static odin3_status exec_unhandled(odin3_pg *pg, odin3_ast_id node) {
    odin3_elab2 *e2 = pg->e2;
    if (odin3_elab2_pending(e2, node)) {
        return ODIN3_ERR_PARSE;
    }
    odin3_diag(e2->design, ODIN3_LOG_ERROR, odin3_ast_loc(e2->elab, node),
               "stage 2: no elaboration of '%s' (a 2D bug)",
               odin3_ast_kind_name(odin3_ast_kind_of(e2->elab, node)));
    e2->fatal = e2->fatal != ODIN3_OK ? e2->fatal : ODIN3_ERR_CHECK;
    return ODIN3_ERR_PARSE;
}

odin3_status pg_frame_push(odin3_pg *pg, const pg_frame *frame) {
    pg_frame *slot = odin3_vec_push(&pg->frames);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = *frame;
    return ODIN3_OK;
}

/* A frame of a lone statement to run. */
static odin3_status frame_stmt(odin3_pg *pg, odin3_ast_id node) {
    pg_frame frame = {PG_F_STMT, node, 0, 0, 0, 0};
    return pg_frame_push(pg, &frame);
}

/* One statement: a block opens a frame, an assignment is built, the rest is pending. */
static odin3_status exec_node(odin3_pg *pg, odin3_ast_id node) {
    switch (odin3_ast_kind_of(pg->e2->elab, node)) {
    case ODIN3_AST_SEQ_BLOCK: {
        pg_frame frame = {PG_F_BLOCK, node, odin3_ast_slot_count(ODIN3_AST_SEQ_BLOCK), 0, 0, 0};
        return pg_frame_push(pg, &frame);
    }
    case ODIN3_AST_BLOCKING_ASSIGN:
    case ODIN3_AST_NONBLOCKING_ASSIGN:
        return pg_exec_assign(pg, node);
    case ODIN3_AST_FOR:
    case ODIN3_AST_REPEAT:
        return pg_loop_open(pg, node);
    case ODIN3_AST_TASK_CALL:
        return pg_task_call(pg, node);
    case ODIN3_AST_IF:
        return pg_if_open(pg, node);
    case ODIN3_AST_CASE:
        return pg_case_open(pg, node);
    case ODIN3_AST_NULL_STMT:
        return ODIN3_OK;
    default:
        return exec_unhandled(pg, node);
    }
}

/* The next statement of the top frame in *node, or none (0) after popping a finished block or
 * after a switch step (which pushes what it needs itself). */
static odin3_status frame_next(odin3_pg *pg, odin3_ast_id *node) {
    pg_frame *frame = odin3_vec_at(&pg->frames, pg->frames.len - 1);
    *node = (odin3_ast_id){0};
    if (frame->kind == PG_F_SWITCH) {
        return pg_switch_step(pg);
    }
    if (frame->kind == PG_F_LOOP) {
        return pg_loop_step(pg);
    }
    if (frame->kind == PG_F_STMT) {
        *node = frame->node;
        odin3_vec_pop(&pg->frames);
        return ODIN3_OK;
    }
    odin3_ast_span kids = odin3_ast_children(pg->e2->elab, frame->node);
    while (frame->next < kids.n) {
        odin3_ast_id kid = kids.ids[frame->next++];
        if (kid.v != 0) {
            *node = kid;
            return ODIN3_OK;
        }
    }
    odin3_vec_pop(&pg->frames);
    return ODIN3_OK;
}

odin3_status pg_run(odin3_pg *pg, odin3_ast_id stmt) {
    size_t base = pg->frames.len; /* a call inlined by an expression runs above the frames there */
    odin3_status st = frame_stmt(pg, stmt);
    while (st == ODIN3_OK && pg->frames.len > base && !pg->e2->stopped) {
        odin3_ast_id node;
        st = frame_next(pg, &node);
        st = st == ODIN3_OK && node.v != 0 ? exec_node(pg, node) : st;
    }
    while (pg->frames.len > base) {
        odin3_vec_pop(&pg->frames);
    }
    return st;
}

odin3_status odin3_pg_stmt(odin3_pg *pg, odin3_ast_id stmt) {
    return stmt.v != 0 ? pg_run(pg, stmt) : ODIN3_OK;
}

/* --- the block ------------------------------------------------------------------------------- */

/* The event control and the body of the ALWAYS (a statement that is no TIMING_STMT is @*). */
static void block_parts(odin3_pg *pg) {
    const odin3_ast *ast = pg->e2->elab;
    odin3_ast_id stmt = odin3_ast_child(ast, pg->always, 0);
    pg->control = (odin3_ast_id){0};
    pg->body = stmt;
    if (stmt.v != 0 && odin3_ast_kind_of(ast, stmt) == ODIN3_AST_TIMING_STMT) {
        odin3_ast_id control = odin3_ast_child(ast, stmt, 0);
        pg->control = odin3_ast_kind_of(ast, control) == ODIN3_AST_EVENT_CONTROL
                          ? control
                          : (odin3_ast_id){0};
        pg->body = odin3_ast_child(ast, stmt, 1);
    }
}

odin3_status pg_start_block(odin3_pg *pg) {
    odin3_status st = pg_outputs(pg);
    st = st != ODIN3_OK ? st : pg_syncs(pg);
    return st != ODIN3_OK ? st : pg_defaults(pg);
}

odin3_status odin3_pg_open(odin3_pg *pg, odin3_ast_id always_node) {
    odin3_elab2 *e2 = pg->e2;
    if (odin3_ast_kind_of(e2->elab, always_node) != ODIN3_AST_ALWAYS) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_pg_open: not an ALWAYS node");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = pg_block_reset(pg);
    pg->always = always_node;
    block_parts(pg);
    odin3_elab2_prov_req req = {always_node, {0}, e2->scope.v};
    st = st != ODIN3_OK ? st : odin3_elab2_prov(e2, &req, &pg->prov);
    st = st != ODIN3_OK ? st : pg_scan(pg, pg->body);
    if (st != ODIN3_OK || pg->outs.len == 0) {
        return st;
    }
    return pg_start_block(pg);
}

odin3_status odin3_pg_always(odin3_pg *pg, odin3_ast_id always_node) {
    odin3_status st = odin3_pg_open(pg, always_node);
    if (st != ODIN3_OK || pg->outs.len == 0) {
        return st;
    }
    st = pg->body.v != 0 ? pg_run(pg, pg->body) : ODIN3_OK;
    if (st != ODIN3_OK) {
        return st;
    }
    pg_incomplete(pg);
    odin3_node_id node = {0};
    return odin3_pg_emit(pg, always_node, &node);
}
