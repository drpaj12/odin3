/*
 * construct_snapshot.c — odin3_construct_snapshot (V1.2.0 4.3 item 4): the construct table of one
 * read run, built from the ELABORATED store at the end of stage 2.
 *
 * One root-down walk with the true child edges (odin3_ast_child by slot; no parent index, whose
 * span-containment parent is ambiguous for equal spans). The walk keeps a stack of frames, one per
 * node on the way down and one extra frame for the synthesized then or else arm of an IF (2.2).
 * A frame whose kind has a row (2.1) is "levelled"; the transparent kinds have none and their
 * children attach to the nearest levelled ancestor. A node that needs a row emits itself and the
 * ancestors that have none yet, shallowest first, so rows are parents first without a second pass.
 * A parent of a finer level than the row (an INSTANTIATION under a GEN_BLOCK) is skipped for the
 * next ancestor that fits (odin3_construct_parent_fits).
 *
 * Call-site form (1.1): a SOURCE record with exactly two locations whose second is the span of a
 * CALL or TASK_CALL gets copies of the rows from its node up to the function or task declaration,
 * under the call's row, keyed by (run, ast, part, call). A span that matches no call or more than
 * one leaves the record a span-only leaf and a recorded miss.
 *
 * The store shares some subtrees (a node with two parents). The walk enters a node once, so it
 * keeps the chain of the first parent. A second parent whose nearest row differs from the first's
 * makes every SOURCE record in that subtree ambiguous: it is a recorded miss (a span-only leaf),
 * never a silently wrong chain (a call-site record keeps its own chain under its call). A SOURCE
 * record whose node ends with neither a row nor an alias (a transparent node with no construct
 * ancestor) is a recorded miss too, so rule 12 stays whole.
 */
#include "ast/ast.h"
#include "ast/srcman.h"
#include "ir/construct.h"
#include "ir/design.h"
#include "ir/prov.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t NO_FRAME_ID = UINT32_MAX;

enum {
    FLAG_MARKED = 1, /* a SOURCE record of the run points at the node */
    FLAG_SEEN = 2,   /* the walk reached the node */
    FLAG_AMBIG = 4,  /* marked, and in a subtree a second parent shares with another chain */
    FLAG_TAINT = 8,  /* the ambiguity walk reached the node */
    IF_THEN_SLOT = 1,
    IF_ELSE_SLOT = 2
};

typedef struct frame {
    uint32_t node; /* AST node; for an arm frame the IF */
    uint32_t idx;  /* next child slot */
    uint32_t n;    /* child slots (0 for an arm frame: its child is entered by descend) */
    uint32_t up;   /* nearest levelled ancestor frame, NO_FRAME_ID for none */
    uint32_t row;  /* the frame's row, 0 until emitted */
    uint32_t arm_child;
    uint8_t part; /* ODIN3_CONSTRUCT_PART_* */
    uint8_t kind;
    uint8_t level; /* odin3_level; 0 for a transparent kind (no row) */
} frame;

/* A call-site record whose second location names a CALL or TASK_CALL. */
typedef struct pending {
    uint32_t record;
    uint32_t ast;
    uint32_t site; /* index into sites */
    odin3_srcloc span;
    bool missed; /* no call-instance copy was made */
} pending;

/* One distinct second-location span, and what the walk found at it. */
typedef struct site {
    odin3_srcloc span;
    uint32_t count; /* CALL or TASK_CALL nodes with exactly this span */
    uint32_t row;   /* the row of the first of them */
} site;

typedef struct snap {
    odin3_design *design;
    const odin3_ast *ast;
    const odin3_srcman *sm;
    odin3_passrun_id run;
    odin3_construct_mode mode;
    uint32_t nodes;
    uint8_t *flags;
    uint32_t *anchor;  /* per node: the nearest row (node * 4 + part) of its first visit */
    uint32_t shared;   /* edges to a node already reached */
    uint32_t diverged; /* of those, edges whose nearest row differs from the first visit's */
    odin3_vec taint;   /* uint32_t nodes of the ambiguity walk */
    odin3_vec frames;  /* frame */
    odin3_vec path;    /* uint32_t frame indices being emitted */
    odin3_vec sites;   /* site, sorted by span */
    odin3_vec pending; /* pending, in record order */
    odin3_vec chain;   /* uint32_t row numbers of a call-instance copy */
} snap;

/* --- helpers ------------------------------------------------------------------------------- */

static int span_cmp(const odin3_srcloc *lhs, const odin3_srcloc *rhs) {
    const uint32_t ka[] = {lhs->file, lhs->line, lhs->col, lhs->end_line, lhs->end_col};
    const uint32_t kb[] = {rhs->file, rhs->line, rhs->col, rhs->end_line, rhs->end_col};
    for (size_t i = 0; i < sizeof ka / sizeof ka[0]; i++) {
        if (ka[i] != kb[i]) {
            return ka[i] < kb[i] ? -1 : 1;
        }
    }
    return 0;
}

static int site_cmp(const void *lhs, const void *rhs) {
    return span_cmp(&((const site *)lhs)->span, &((const site *)rhs)->span);
}

/* The index of the site with this span, SIZE_MAX when none. */
static size_t site_find(const snap *sn, const odin3_srcloc *span) {
    size_t lo = 0;
    size_t hi = sn->sites.len;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) / 2);
        int cmp = span_cmp(&((const site *)odin3_vec_cat(&sn->sites, mid))->span, span);
        if (cmp == 0) {
            return mid;
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return SIZE_MAX;
}

static frame *frame_at(snap *sn, uint32_t idx) {
    return odin3_vec_at(&sn->frames, idx);
}

static bool level_fits(uint8_t parent, uint8_t row) {
    return parent <= row ||
           (row == (uint8_t)ODIN3_LEVEL_BRANCH && parent == (uint8_t)ODIN3_LEVEL_STMT);
}

/* The nearest levelled ancestor of frame fi that may be its parent. */
static uint32_t fit_parent(snap *sn, uint32_t fi) {
    uint8_t level = frame_at(sn, fi)->level;
    uint32_t pp = frame_at(sn, fi)->up;
    while (pp != NO_FRAME_ID && !level_fits(frame_at(sn, pp)->level, level)) {
        pp = frame_at(sn, pp)->up;
    }
    return pp;
}

/* The anchor of a node entered from frame top through child slot: the node and part of the
 * nearest levelled frame (the arm frame when the slot is an IF's then or else), 0 for none. */
static uint32_t edge_anchor(snap *sn, uint32_t top, uint32_t slot) {
    const frame *up = frame_at(sn, top);
    if (up->part == 0 && up->kind == ODIN3_AST_IF &&
        (slot == IF_THEN_SLOT || slot == IF_ELSE_SLOT)) {
        return (up->node * 4U) +
               (slot == IF_THEN_SLOT ? ODIN3_CONSTRUCT_PART_THEN : ODIN3_CONSTRUCT_PART_ELSE);
    }
    uint32_t at = up->level != 0 ? top : up->up;
    return at == NO_FRAME_ID ? 0 : (frame_at(sn, at)->node * 4U) + frame_at(sn, at)->part;
}

/* Marked nodes in the subtree of root cannot be told apart by their first chain: flag them. Each
 * node is entered once (FLAG_TAINT), so the walk is linear in the store. */
static odin3_status taint_subtree(snap *sn, uint32_t root) {
    sn->taint.len = 0;
    uint32_t *slot = odin3_vec_push(&sn->taint);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = root;
    while (sn->taint.len > 0) {
        uint32_t node = *(const uint32_t *)odin3_vec_cat(&sn->taint, sn->taint.len - 1U);
        sn->taint.len--;
        if ((sn->flags[node] & FLAG_TAINT) != 0) {
            continue;
        }
        sn->flags[node] |= FLAG_TAINT;
        if ((sn->flags[node] & FLAG_MARKED) != 0) {
            sn->flags[node] |= FLAG_AMBIG;
        }
        odin3_ast_id id = {node};
        for (uint32_t k = 0; k < odin3_ast_nchild(sn->ast, id); k++) {
            uint32_t child = odin3_ast_child(sn->ast, id, k).v;
            if (child == 0 || child >= sn->nodes || (sn->flags[child] & FLAG_TAINT) != 0) {
                continue;
            }
            slot = odin3_vec_push(&sn->taint);
            if (slot == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            *slot = child;
        }
    }
    return ODIN3_OK;
}

/* --- rows ---------------------------------------------------------------------------------- */

/* A row for node (part of it) with the span of span_node. */
static odin3_status add_row(snap *sn, const odin3_construct_row *proto, uint32_t span_node,
                            uint32_t *index) {
    odin3_construct_row row = *proto;
    odin3_srcloc sl;
    odin3_ast_id id = {span_node};
    odin3_srcman_srcloc(sn->sm,
                        (odin3_range){odin3_ast_loc(sn->ast, id), odin3_ast_end(sn->ast, id)}, &sl);
    row.file = sl.file;
    row.line = sl.line;
    row.col = sl.col;
    row.end_line = sl.end_line;
    row.end_col = sl.end_col;
    return odin3_construct_add(sn->design, &row, index);
}

/* Emit frame `frame_idx` under the row `parent_row`. */
typedef struct emit_req {
    uint32_t frame_idx;
    uint32_t parent_row;
} emit_req;

static odin3_status emit_frame(snap *sn, const emit_req *req) {
    const frame fr = *frame_at(sn, req->frame_idx);
    odin3_construct_row row = {
        .run = sn->run.v,
        .ast = fr.node,
        .parent = req->parent_row,
        .name = fr.part == 0 ? odin3_ast_name(sn->ast, (odin3_ast_id){fr.node}) : 0,
        .part = fr.part,
        .level = fr.level,
        .kind = fr.kind};
    /* A node reached twice (shared in the store) keeps the row of its first visit. */
    const odin3_construct_key key = {sn->run, fr.node, fr.part, 0};
    uint32_t index = odin3_construct_find(sn->design, &key);
    odin3_status st =
        index != 0 ? ODIN3_OK : add_row(sn, &row, fr.part == 0 ? fr.node : fr.arm_child, &index);
    if (st == ODIN3_OK) {
        frame_at(sn, req->frame_idx)->row = index;
    }
    return st;
}

/* Emits frame fi and the ancestors that have no row yet, shallowest first. */
static odin3_status emit_path(snap *sn, uint32_t fi) {
    sn->path.len = 0;
    uint32_t at = fi;
    while (at != NO_FRAME_ID && frame_at(sn, at)->row == 0) {
        uint32_t *slot = odin3_vec_push(&sn->path);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = at;
        at = fit_parent(sn, at);
    }
    uint32_t parent_row = at != NO_FRAME_ID ? frame_at(sn, at)->row : 0;
    odin3_status st = ODIN3_OK;
    for (size_t i = sn->path.len; st == ODIN3_OK && i > 0; i--) {
        uint32_t frame_idx = *(const uint32_t *)odin3_vec_cat(&sn->path, i - 1U);
        st = emit_frame(sn, &(emit_req){frame_idx, parent_row});
        parent_row = frame_at(sn, frame_idx)->row;
    }
    return st;
}

/* --- the walk ------------------------------------------------------------------------------ */

static odin3_status push_frame(snap *sn, const frame *proto, uint32_t parent) {
    frame *fr = odin3_vec_push(&sn->frames);
    if (fr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *fr = *proto;
    fr->up = NO_FRAME_ID;
    if (parent != NO_FRAME_ID) {
        fr->up = frame_at(sn, parent)->level != 0 ? parent : frame_at(sn, parent)->up;
    }
    return ODIN3_OK;
}

static frame node_frame(const snap *sn, uint32_t node) {
    odin3_ast_id id = {node};
    uint32_t kind = (uint32_t)odin3_ast_kind_of(sn->ast, id);
    return (frame){.node = node,
                   .n = odin3_ast_nchild(sn->ast, id),
                   .row = 0,
                   .kind = (uint8_t)kind,
                   .level = (uint8_t)odin3_construct_kind_level(kind)};
}

/* The nearest levelled ancestor of frame fi (not fi) that a node can be an alias of. */
static uint32_t alias_target(snap *sn, uint32_t fi) {
    uint32_t pp = frame_at(sn, fi)->up;
    while (pp != NO_FRAME_ID) {
        const frame *fr = frame_at(sn, pp);
        if (fr->part == 0 && (sn->mode != ODIN3_CONSTRUCT_MODE_CONSTRUCTS ||
                              fr->level != (uint8_t)ODIN3_LEVEL_EXPR)) {
            return pp;
        }
        pp = fr->up;
    }
    return NO_FRAME_ID;
}

/* A CALL or TASK_CALL whose span a call-site record named: it gets a row and is counted. */
static odin3_status note_call(snap *sn, uint32_t fi) {
    const frame *fr = frame_at(sn, fi);
    if (sn->sites.len == 0 || (fr->kind != ODIN3_AST_CALL && fr->kind != ODIN3_AST_TASK_CALL)) {
        return ODIN3_OK;
    }
    odin3_srcloc sl;
    odin3_ast_id id = {fr->node};
    odin3_srcman_srcloc(sn->sm,
                        (odin3_range){odin3_ast_loc(sn->ast, id), odin3_ast_end(sn->ast, id)}, &sl);
    size_t at = site_find(sn, &sl);
    if (at == SIZE_MAX) {
        return ODIN3_OK;
    }
    odin3_status st = emit_path(sn, fi);
    site *hit = odin3_vec_at(&sn->sites, at);
    if (st == ODIN3_OK && hit->count++ == 0) {
        hit->row = frame_at(sn, fi)->row;
    }
    return st;
}

/* What frame fi needs: its own row, an alias to an ancestor's, or nothing. */
static odin3_status visit(snap *sn, uint32_t fi) {
    const frame fr = *frame_at(sn, fi);
    sn->flags[fr.node] |= FLAG_SEEN;
    sn->anchor[fr.node] =
        fr.up == NO_FRAME_ID ? 0 : (frame_at(sn, fr.up)->node * 4U) + frame_at(sn, fr.up)->part;
    bool marked = (sn->flags[fr.node] & FLAG_MARKED) != 0;
    bool own = fr.level != 0 && (sn->mode == ODIN3_CONSTRUCT_MODE_FULL ||
                                 (marked && (sn->mode != ODIN3_CONSTRUCT_MODE_CONSTRUCTS ||
                                             fr.level != (uint8_t)ODIN3_LEVEL_EXPR)));
    odin3_status st = own ? emit_path(sn, fi) : ODIN3_OK;
    st = st != ODIN3_OK ? st : note_call(sn, fi);
    const odin3_construct_key own_key = {sn->run, fr.node, ODIN3_CONSTRUCT_PART_NODE, 0};
    /* a node reached twice (shared in the store) keeps what its first visit gave it */
    if (st != ODIN3_OK || !marked || frame_at(sn, fi)->row != 0 ||
        odin3_construct_find(sn->design, &own_key) != 0) {
        return st;
    }
    uint32_t target = alias_target(sn, fi);
    if (target == NO_FRAME_ID) {
        return ODIN3_OK;
    }
    st = emit_path(sn, target);
    return st != ODIN3_OK
               ? st
               : odin3_construct_alias(sn->design, sn->run, fr.node, frame_at(sn, target)->row);
}

/* Enters child under frame top, through an arm frame when top is an IF and slot is its then or
 * else statement. */
typedef struct edge {
    uint32_t top;
    uint32_t slot;
    uint32_t child;
} edge;

static odin3_status descend(snap *sn, const edge *ed) {
    const uint32_t top = ed->top;
    const uint32_t slot = ed->slot;
    const uint32_t child = ed->child;
    uint32_t parent = top;
    const frame up = *frame_at(sn, top);
    odin3_status st = ODIN3_OK;
    if (up.part == 0 && up.kind == ODIN3_AST_IF && (slot == IF_THEN_SLOT || slot == IF_ELSE_SLOT)) {
        frame arm = {
            .node = up.node,
            .n = 0,
            .arm_child = child,
            .part = slot == IF_THEN_SLOT ? ODIN3_CONSTRUCT_PART_THEN : ODIN3_CONSTRUCT_PART_ELSE,
            .kind = slot == IF_THEN_SLOT ? ODIN3_CONSTRUCT_IF_THEN : ODIN3_CONSTRUCT_IF_ELSE,
            .level = (uint8_t)ODIN3_LEVEL_BRANCH};
        st = push_frame(sn, &arm, top);
        parent = (uint32_t)sn->frames.len - 1U;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    frame own = node_frame(sn, child);
    st = push_frame(sn, &own, parent);
    return st != ODIN3_OK ? st : visit(sn, (uint32_t)sn->frames.len - 1U);
}

static odin3_status walk_root(snap *sn, uint32_t root) {
    sn->frames.len = 0;
    frame first = node_frame(sn, root);
    odin3_status st = push_frame(sn, &first, NO_FRAME_ID);
    st = st != ODIN3_OK ? st : visit(sn, 0);
    while (st == ODIN3_OK && sn->frames.len > 0) {
        uint32_t top = (uint32_t)sn->frames.len - 1U;
        frame *fr = frame_at(sn, top);
        if (fr->idx >= fr->n) {
            odin3_vec_pop(&sn->frames);
            continue;
        }
        uint32_t slot = fr->idx++;
        uint32_t child = odin3_ast_child(sn->ast, (odin3_ast_id){fr->node}, slot).v;
        /* a node already reached (a subtree two parents share) keeps its first visit */
        if (child == 0 || child >= sn->nodes) {
            continue;
        }
        if ((sn->flags[child] & FLAG_SEEN) == 0) {
            st = descend(sn, &(edge){top, slot, child});
        } else {
            sn->shared++;
            if (sn->anchor[child] != edge_anchor(sn, top, slot)) {
                sn->diverged++;
                st = taint_subtree(sn, child);
            }
        }
    }
    return st;
}

/* A marked node the walk never reached (not under a UNIT): a root row of its own. */
static odin3_status orphans(snap *sn) {
    odin3_status st = ODIN3_OK;
    for (uint32_t node = 1; st == ODIN3_OK && node < sn->nodes; node++) {
        if ((sn->flags[node] & (FLAG_MARKED | FLAG_SEEN)) != FLAG_MARKED) {
            continue;
        }
        frame fr = node_frame(sn, node);
        if (fr.level == 0 || (sn->mode == ODIN3_CONSTRUCT_MODE_CONSTRUCTS &&
                              fr.level == (uint8_t)ODIN3_LEVEL_EXPR)) {
            continue;
        }
        odin3_log(ODIN3_LOG_DEBUG, "construct snapshot: node %u is not under a UNIT", node);
        odin3_construct_row row = {.run = sn->run.v,
                                   .ast = node,
                                   .level = fr.level,
                                   .kind = fr.kind,
                                   .name = odin3_ast_name(sn->ast, (odin3_ast_id){node})};
        uint32_t index = 0;
        st = add_row(sn, &row, node, &index);
    }
    return st;
}

/* --- the records --------------------------------------------------------------------------- */

/* Marks the nodes the run's SOURCE records point at and collects the call-site records. */
static odin3_status sweep(snap *sn) {
    uint32_t end = odin3_prov_end(sn->design);
    for (uint32_t i = 1; i < end; i++) {
        const odin3_prov_record *rec = odin3_prov_get(sn->design, (odin3_prov_id){i});
        if (rec->run.v != sn->run.v || rec->kind != ODIN3_PROV_SOURCE || rec->ast == 0 ||
            rec->ast >= sn->nodes) {
            continue;
        }
        sn->flags[rec->ast] |= FLAG_MARKED;
        if (rec->n_locs != 2) {
            continue;
        }
        pending *pp = odin3_vec_push(&sn->pending);
        site *sp = odin3_vec_push(&sn->sites);
        if (pp == NULL || sp == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *pp = (pending){i, rec->ast, 0, rec->locs[1], false};
        *sp = (site){rec->locs[1], 0, 0};
    }
    if (sn->sites.len > 1) {
        qsort(sn->sites.data, sn->sites.len, sizeof(site), site_cmp);
    }
    size_t kept = 0;
    for (size_t i = 0; i < sn->sites.len; i++) {
        site *cur = odin3_vec_at(&sn->sites, i);
        if (kept == 0 ||
            span_cmp(&((site *)odin3_vec_at(&sn->sites, kept - 1U))->span, &cur->span) != 0) {
            *(site *)odin3_vec_at(&sn->sites, kept++) = *cur;
        }
    }
    sn->sites.len = kept;
    for (size_t i = 0; i < sn->pending.len; i++) {
        pending *pp = odin3_vec_at(&sn->pending, i);
        pp->site = (uint32_t)site_find(sn, &pp->span);
    }
    return ODIN3_OK;
}

/* Copies the rows from row leaf up to its function or task declaration under the call's row.
 * False when leaf has no such ancestor. */
typedef struct call_copy {
    uint32_t leaf;
    uint32_t call;
} call_copy;

static odin3_status copy_chain(snap *sn, const call_copy *req, bool *copied) {
    const uint32_t leaf = req->leaf;
    const uint32_t call = req->call;
    sn->chain.len = 0;
    *copied = false;
    uint32_t at = leaf;
    bool found = false;
    while (at != 0 && !found) {
        uint32_t *slot = odin3_vec_push(&sn->chain);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = at;
        const odin3_construct_row *row = odin3_construct_get(sn->design, at);
        found = row->kind == ODIN3_AST_FUNCTION_DECL || row->kind == ODIN3_AST_TASK_DECL;
        at = row->parent;
    }
    if (!found) {
        return ODIN3_OK;
    }
    uint32_t parent = call;
    odin3_status st = ODIN3_OK;
    for (size_t i = sn->chain.len; st == ODIN3_OK && i > 0; i--) {
        odin3_construct_row row =
            *odin3_construct_get(sn->design, *(const uint32_t *)odin3_vec_cat(&sn->chain, i - 1U));
        row.call = call;
        row.parent = parent;
        row.next = 0;
        const odin3_construct_key key = {{row.run}, row.ast, row.part, call};
        parent = odin3_construct_find(sn->design, &key);
        if (parent == 0) {
            st = odin3_construct_add(sn->design, &row, &parent);
        }
    }
    *copied = st == ODIN3_OK;
    return st;
}

/* The call-instance copies of every call-site record; a record that cannot be resolved is flagged
 * (missed) for misses(). */
static odin3_status call_instances(snap *sn) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < sn->pending.len; i++) {
        pending *pp = odin3_vec_at(&sn->pending, i);
        const site *hit = odin3_vec_cat(&sn->sites, pp->site);
        const odin3_construct_key key = {sn->run, pp->ast, ODIN3_CONSTRUCT_PART_NODE, 0};
        uint32_t leaf = odin3_construct_find(sn->design, &key);
        const odin3_construct_row *row = odin3_construct_get(sn->design, leaf);
        bool copied = false;
        if (hit->count == 1 && row != NULL && row->ast == pp->ast) {
            st = copy_chain(sn, &(call_copy){leaf, hit->row}, &copied);
        }
        pp->missed = !copied;
    }
    return st;
}

/* The recorded misses, in record order: a call-site record without its copy, a record in a
 * subtree two chains share, and a record whose node ended with neither a row nor an alias. */
static odin3_status misses(snap *sn) {
    odin3_status st = ODIN3_OK;
    size_t next = 0;
    uint32_t end = odin3_prov_end(sn->design);
    uint32_t ambiguous = 0;
    for (uint32_t i = 1; st == ODIN3_OK && i < end; i++) {
        const odin3_prov_record *rec = odin3_prov_get(sn->design, (odin3_prov_id){i});
        if (rec->run.v != sn->run.v || rec->kind != ODIN3_PROV_SOURCE || rec->ast == 0 ||
            rec->ast >= sn->nodes) {
            continue;
        }
        bool missed = (sn->flags[rec->ast] & FLAG_AMBIG) != 0;
        if (next < sn->pending.len &&
            ((const pending *)odin3_vec_cat(&sn->pending, next))->record == i) {
            /* a call-site record has its own chain under the call: the shared function body does
             * not make it ambiguous */
            missed = ((const pending *)odin3_vec_cat(&sn->pending, next))->missed;
            next++;
        }
        ambiguous += missed ? 1U : 0U;
        missed = missed || !odin3_construct_has_node(sn->design, sn->run, rec->ast);
        st = missed ? odin3_construct_note_miss(sn->design, (odin3_prov_id){i}) : ODIN3_OK;
    }
    odin3_log(ODIN3_LOG_DEBUG,
              "construct snapshot: %u shared edges, %u with another chain, %u ambiguous records",
              sn->shared, sn->diverged, ambiguous);
    return st;
}

/* --- entry --------------------------------------------------------------------------------- */

static void snap_free(snap *sn) {
    odin3_util_free(sn->flags);
    odin3_util_free(sn->anchor);
    odin3_vec_free(&sn->taint);
    odin3_vec_free(&sn->frames);
    odin3_vec_free(&sn->path);
    odin3_vec_free(&sn->sites);
    odin3_vec_free(&sn->pending);
    odin3_vec_free(&sn->chain);
}

static odin3_status snap_open(snap *sn, odin3_design *design, const odin3_ast *ast,
                              odin3_construct_mode mode) {
    *sn = (snap){.design = design, .ast = ast, .run = odin3_ast_run(ast), .mode = mode};
    odin3_vec_init(&sn->taint, sizeof(uint32_t));
    odin3_vec_init(&sn->frames, sizeof(frame));
    odin3_vec_init(&sn->path, sizeof(uint32_t));
    odin3_vec_init(&sn->sites, sizeof(site));
    odin3_vec_init(&sn->pending, sizeof(pending));
    odin3_vec_init(&sn->chain, sizeof(uint32_t));
    sn->nodes = odin3_ast_node_end(ast).v;
    /* nodes + 1: an empty store (node_end 0) still gets a block, not a NULL that reads as OOM */
    sn->flags = odin3_util_calloc((size_t)sn->nodes + 1U);
    sn->anchor = odin3_util_calloc(((size_t)sn->nodes + 1U) * sizeof(uint32_t));
    odin3_srcman *sm = NULL;
    odin3_status st = sn->flags != NULL && sn->anchor != NULL ? odin3_design_get_srcman(design, &sm)
                                                              : ODIN3_ERR_NO_MEMORY;
    sn->sm = sm;
    return st;
}

static odin3_status snapshot_run(snap *sn) {
    odin3_status st = sweep(sn);
    for (uint32_t i = 0; st == ODIN3_OK && i < odin3_ast_root_count(sn->ast); i++) {
        st = walk_root(sn, odin3_ast_root(sn->ast, i).v);
    }
    st = st != ODIN3_OK ? st : orphans(sn);
    st = st != ODIN3_OK ? st : call_instances(sn);
    st = st != ODIN3_OK ? st : misses(sn);
    return st != ODIN3_OK ? st : odin3_construct_mark_run(sn->design, sn->run, sn->mode);
}

odin3_status odin3_construct_snapshot(odin3_design *design, const odin3_ast *ast,
                                      odin3_construct_mode mode) {
    if (design == NULL || ast == NULL || mode > ODIN3_CONSTRUCT_MODE_FULL ||
        odin3_ast_form_of(ast) != ODIN3_AST_FORM_ELABORATED || odin3_ast_run(ast).v == 0 ||
        odin3_ast_run(ast).v >= odin3_passrun_end(design) ||
        odin3_construct_run_mode(design, odin3_ast_run(ast)) != ODIN3_CONSTRUCT_MODE_OFF) {
        odin3_log(
            ODIN3_LOG_ERROR,
            "odin3_construct_snapshot: invalid design, store or mode, or the run has a snapshot");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (mode == ODIN3_CONSTRUCT_MODE_OFF) {
        return ODIN3_OK;
    }
    snap sn;
    odin3_status st = snap_open(&sn, design, ast, mode);
    st = st != ODIN3_OK ? st : snapshot_run(&sn);
    snap_free(&sn);
    return st;
}
