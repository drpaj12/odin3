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
 * Call-site form (1.1): a SOURCE record with two locations whose second is the span of a CALL or
 * TASK_CALL gets copies of the rows from its node up to the function or task declaration, under
 * the call's row, keyed by (run, ast, part, call). A span that matches no call or more than one
 * leaves the record a span-only leaf and a recorded miss.
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

enum {
    NO_FRAME = UINT32_MAX,
    FLAG_MARKED = 1, /* a SOURCE record of the run points at the node */
    FLAG_SEEN = 2,   /* the walk reached the node */
    IF_THEN_SLOT = 1,
    IF_ELSE_SLOT = 2
};

typedef struct frame {
    uint32_t node; /* AST node; for an arm frame the IF */
    uint32_t idx;  /* next child slot */
    uint32_t n;    /* child slots (1 for an arm frame) */
    uint32_t up;   /* nearest levelled ancestor frame, NO_FRAME for none */
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
    odin3_vec frames;  /* frame */
    odin3_vec path;    /* uint32_t frame indices being emitted */
    odin3_vec sites;   /* site, sorted by span */
    odin3_vec pending; /* pending, in record order */
    odin3_vec chain;   /* uint32_t row numbers of a call-instance copy */
} snap;

/* --- helpers ------------------------------------------------------------------------------- */

static int span_cmp(const odin3_srcloc *a, const odin3_srcloc *b) {
    const uint32_t ka[] = {a->file, a->line, a->col, a->end_line, a->end_col};
    const uint32_t kb[] = {b->file, b->line, b->col, b->end_line, b->end_col};
    for (size_t i = 0; i < sizeof ka / sizeof ka[0]; i++) {
        if (ka[i] != kb[i]) {
            return ka[i] < kb[i] ? -1 : 1;
        }
    }
    return 0;
}

static int site_cmp(const void *a, const void *b) {
    return span_cmp(&((const site *)a)->span, &((const site *)b)->span);
}

/* The index of the site with this span, SIZE_MAX when none. */
static size_t site_find(const snap *s, const odin3_srcloc *span) {
    size_t lo = 0;
    size_t hi = s->sites.len;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) / 2);
        int cmp = span_cmp(&((const site *)odin3_vec_cat(&s->sites, mid))->span, span);
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

static frame *frame_at(snap *s, uint32_t idx) {
    return odin3_vec_at(&s->frames, idx);
}

static bool level_fits(uint8_t parent, uint8_t row) {
    return parent <= row ||
           (row == (uint8_t)ODIN3_LEVEL_BRANCH && parent == (uint8_t)ODIN3_LEVEL_STMT);
}

/* The nearest levelled ancestor of frame k that may be its parent. */
static uint32_t fit_parent(snap *s, uint32_t k) {
    uint8_t level = frame_at(s, k)->level;
    uint32_t p = frame_at(s, k)->up;
    while (p != NO_FRAME && !level_fits(frame_at(s, p)->level, level)) {
        p = frame_at(s, p)->up;
    }
    return p;
}

/* --- rows ---------------------------------------------------------------------------------- */

/* A row for node (part of it) with the span of span_node. */
static odin3_status add_row(snap *s, const odin3_construct_row *proto, uint32_t span_node,
                            uint32_t *index) {
    odin3_construct_row row = *proto;
    odin3_srcloc sl;
    odin3_ast_id id = {span_node};
    odin3_srcman_srcloc(s->sm, (odin3_range){odin3_ast_loc(s->ast, id), odin3_ast_end(s->ast, id)},
                        &sl);
    row.file = sl.file;
    row.line = sl.line;
    row.col = sl.col;
    row.end_line = sl.end_line;
    row.end_col = sl.end_col;
    return odin3_construct_add(s->design, &row, index);
}

static odin3_status emit_frame(snap *s, uint32_t k, uint32_t parent_row) {
    const frame f = *frame_at(s, k);
    odin3_construct_row row = {.run = s->run.v,
                               .ast = f.node,
                               .parent = parent_row,
                               .name =
                                   f.part == 0 ? odin3_ast_name(s->ast, (odin3_ast_id){f.node}) : 0,
                               .part = f.part,
                               .level = f.level,
                               .kind = f.kind};
    /* A node reached twice (shared in the store) keeps the row of its first visit. */
    const odin3_construct_key key = {s->run, f.node, f.part, 0};
    uint32_t index = odin3_construct_find(s->design, &key);
    odin3_status st =
        index != 0 ? ODIN3_OK : add_row(s, &row, f.part == 0 ? f.node : f.arm_child, &index);
    if (st == ODIN3_OK) {
        frame_at(s, k)->row = index;
    }
    return st;
}

/* Emits frame k and the ancestors that have no row yet, shallowest first. */
static odin3_status emit_path(snap *s, uint32_t k) {
    s->path.len = 0;
    uint32_t at = k;
    while (at != NO_FRAME && frame_at(s, at)->row == 0) {
        uint32_t *slot = odin3_vec_push(&s->path);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = at;
        at = fit_parent(s, at);
    }
    uint32_t parent_row = at != NO_FRAME ? frame_at(s, at)->row : 0;
    odin3_status st = ODIN3_OK;
    for (size_t i = s->path.len; st == ODIN3_OK && i > 0; i--) {
        uint32_t frame_idx = *(const uint32_t *)odin3_vec_cat(&s->path, i - 1U);
        st = emit_frame(s, frame_idx, parent_row);
        parent_row = frame_at(s, frame_idx)->row;
    }
    return st;
}

/* --- the walk ------------------------------------------------------------------------------ */

static odin3_status push_frame(snap *s, const frame *proto, uint32_t parent) {
    frame *f = odin3_vec_push(&s->frames);
    if (f == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *f = *proto;
    f->up = NO_FRAME;
    if (parent != NO_FRAME) {
        f->up = frame_at(s, parent)->level != 0 ? parent : frame_at(s, parent)->up;
    }
    return ODIN3_OK;
}

static frame node_frame(const snap *s, uint32_t node) {
    odin3_ast_id id = {node};
    uint32_t kind = (uint32_t)odin3_ast_kind_of(s->ast, id);
    return (frame){.node = node,
                   .n = odin3_ast_nchild(s->ast, id),
                   .row = 0,
                   .kind = (uint8_t)kind,
                   .level = (uint8_t)odin3_construct_kind_level(kind)};
}

/* The nearest levelled ancestor of frame k (not k) that a node can be an alias of. */
static uint32_t alias_target(snap *s, uint32_t k) {
    uint32_t p = frame_at(s, k)->up;
    while (p != NO_FRAME) {
        const frame *f = frame_at(s, p);
        if (f->part == 0 &&
            (s->mode != ODIN3_CONSTRUCT_MODE_CONSTRUCTS || f->level != (uint8_t)ODIN3_LEVEL_EXPR)) {
            return p;
        }
        p = f->up;
    }
    return NO_FRAME;
}

/* A CALL or TASK_CALL whose span a call-site record named: it gets a row and is counted. */
static odin3_status note_call(snap *s, uint32_t k) {
    const frame *f = frame_at(s, k);
    if (s->sites.len == 0 || (f->kind != ODIN3_AST_CALL && f->kind != ODIN3_AST_TASK_CALL)) {
        return ODIN3_OK;
    }
    odin3_srcloc sl;
    odin3_ast_id id = {f->node};
    odin3_srcman_srcloc(s->sm, (odin3_range){odin3_ast_loc(s->ast, id), odin3_ast_end(s->ast, id)},
                        &sl);
    size_t at = site_find(s, &sl);
    if (at == SIZE_MAX) {
        return ODIN3_OK;
    }
    odin3_status st = emit_path(s, k);
    site *hit = odin3_vec_at(&s->sites, at);
    if (st == ODIN3_OK && hit->count++ == 0) {
        hit->row = frame_at(s, k)->row;
    }
    return st;
}

/* What frame k needs: its own row, an alias to an ancestor's, or nothing. */
static odin3_status visit(snap *s, uint32_t k) {
    const frame f = *frame_at(s, k);
    s->flags[f.node] |= FLAG_SEEN;
    bool marked = (s->flags[f.node] & FLAG_MARKED) != 0;
    bool own = f.level != 0 && (s->mode == ODIN3_CONSTRUCT_MODE_FULL ||
                                (marked && (s->mode != ODIN3_CONSTRUCT_MODE_CONSTRUCTS ||
                                            f.level != (uint8_t)ODIN3_LEVEL_EXPR)));
    odin3_status st = own ? emit_path(s, k) : ODIN3_OK;
    st = st != ODIN3_OK ? st : note_call(s, k);
    if (st != ODIN3_OK || !marked || frame_at(s, k)->row != 0) {
        return st;
    }
    uint32_t target = alias_target(s, k);
    if (target == NO_FRAME) {
        return ODIN3_OK;
    }
    st = emit_path(s, target);
    return st != ODIN3_OK
               ? st
               : odin3_construct_alias(s->design, s->run, f.node, frame_at(s, target)->row);
}

/* Enters child under frame top, through an arm frame when top is an IF and slot is its then or
 * else statement. */
static odin3_status descend(snap *s, uint32_t top, uint32_t slot, uint32_t child) {
    uint32_t parent = top;
    const frame up = *frame_at(s, top);
    odin3_status st = ODIN3_OK;
    if (up.part == 0 && up.kind == ODIN3_AST_IF && (slot == IF_THEN_SLOT || slot == IF_ELSE_SLOT)) {
        frame arm = {
            .node = up.node,
            .n = 1,
            .arm_child = child,
            .part = slot == IF_THEN_SLOT ? ODIN3_CONSTRUCT_PART_THEN : ODIN3_CONSTRUCT_PART_ELSE,
            .kind = slot == IF_THEN_SLOT ? ODIN3_CONSTRUCT_IF_THEN : ODIN3_CONSTRUCT_IF_ELSE,
            .level = (uint8_t)ODIN3_LEVEL_BRANCH};
        st = push_frame(s, &arm, top);
        parent = (uint32_t)s->frames.len - 1U;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    frame own = node_frame(s, child);
    st = push_frame(s, &own, parent);
    return st != ODIN3_OK ? st : visit(s, (uint32_t)s->frames.len - 1U);
}

static odin3_status walk_root(snap *s, uint32_t root) {
    s->frames.len = 0;
    frame first = node_frame(s, root);
    odin3_status st = push_frame(s, &first, NO_FRAME);
    st = st != ODIN3_OK ? st : visit(s, 0);
    while (st == ODIN3_OK && s->frames.len > 0) {
        uint32_t top = (uint32_t)s->frames.len - 1U;
        frame *f = frame_at(s, top);
        if (f->idx >= f->n) {
            odin3_vec_pop(&s->frames);
            continue;
        }
        uint32_t slot = f->idx++;
        uint32_t child =
            f->part != 0 ? f->arm_child : odin3_ast_child(s->ast, (odin3_ast_id){f->node}, slot).v;
        if (child != 0 && child < s->nodes) {
            st = descend(s, top, slot, child);
        }
    }
    return st;
}

/* A marked node the walk never reached (not under a UNIT): a root row of its own. */
static odin3_status orphans(snap *s) {
    odin3_status st = ODIN3_OK;
    for (uint32_t node = 1; st == ODIN3_OK && node < s->nodes; node++) {
        if ((s->flags[node] & (FLAG_MARKED | FLAG_SEEN)) != FLAG_MARKED) {
            continue;
        }
        frame f = node_frame(s, node);
        if (f.level == 0 ||
            (s->mode == ODIN3_CONSTRUCT_MODE_CONSTRUCTS && f.level == (uint8_t)ODIN3_LEVEL_EXPR)) {
            continue;
        }
        odin3_log(ODIN3_LOG_DEBUG, "construct snapshot: node %u is not under a UNIT", node);
        odin3_construct_row row = {.run = s->run.v,
                                   .ast = node,
                                   .level = f.level,
                                   .kind = f.kind,
                                   .name = odin3_ast_name(s->ast, (odin3_ast_id){node})};
        uint32_t index = 0;
        st = add_row(s, &row, node, &index);
    }
    return st;
}

/* --- the records --------------------------------------------------------------------------- */

/* Marks the nodes the run's SOURCE records point at and collects the call-site records. */
static odin3_status sweep(snap *s) {
    uint32_t end = odin3_prov_end(s->design);
    for (uint32_t i = 1; i < end; i++) {
        const odin3_prov_record *rec = odin3_prov_get(s->design, (odin3_prov_id){i});
        if (rec->run.v != s->run.v || rec->kind != ODIN3_PROV_SOURCE || rec->ast == 0 ||
            rec->ast >= s->nodes) {
            continue;
        }
        s->flags[rec->ast] |= FLAG_MARKED;
        if (rec->n_locs < 2) {
            continue;
        }
        pending *p = odin3_vec_push(&s->pending);
        site *sp = odin3_vec_push(&s->sites);
        if (p == NULL || sp == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *p = (pending){i, rec->ast, 0, rec->locs[1]};
        *sp = (site){rec->locs[1], 0, 0};
    }
    if (s->sites.len > 1) {
        qsort(s->sites.data, s->sites.len, sizeof(site), site_cmp);
    }
    size_t kept = 0;
    for (size_t i = 0; i < s->sites.len; i++) {
        site *cur = odin3_vec_at(&s->sites, i);
        if (kept == 0 ||
            span_cmp(&((site *)odin3_vec_at(&s->sites, kept - 1U))->span, &cur->span) != 0) {
            *(site *)odin3_vec_at(&s->sites, kept++) = *cur;
        }
    }
    s->sites.len = kept;
    for (size_t i = 0; i < s->pending.len; i++) {
        pending *p = odin3_vec_at(&s->pending, i);
        p->site = (uint32_t)site_find(s, &p->span);
    }
    return ODIN3_OK;
}

/* Copies the rows from row leaf up to its function or task declaration under the call's row.
 * False when leaf has no such ancestor. */
static odin3_status copy_chain(snap *s, uint32_t leaf, uint32_t call, bool *copied) {
    s->chain.len = 0;
    *copied = false;
    uint32_t at = leaf;
    bool found = false;
    while (at != 0 && !found) {
        uint32_t *slot = odin3_vec_push(&s->chain);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = at;
        const odin3_construct_row *row = odin3_construct_get(s->design, at);
        found = row->kind == ODIN3_AST_FUNCTION_DECL || row->kind == ODIN3_AST_TASK_DECL;
        at = row->parent;
    }
    if (!found) {
        return ODIN3_OK;
    }
    uint32_t parent = call;
    odin3_status st = ODIN3_OK;
    for (size_t i = s->chain.len; st == ODIN3_OK && i > 0; i--) {
        odin3_construct_row row =
            *odin3_construct_get(s->design, *(const uint32_t *)odin3_vec_cat(&s->chain, i - 1U));
        row.call = call;
        row.parent = parent;
        row.next = 0;
        const odin3_construct_key key = {{row.run}, row.ast, row.part, call};
        parent = odin3_construct_find(s->design, &key);
        if (parent == 0) {
            st = odin3_construct_add(s->design, &row, &parent);
        }
    }
    *copied = st == ODIN3_OK;
    return st;
}

/* The call-instance copies of every call-site record; the ones that cannot be resolved are noted
 * as misses, in record order. */
static odin3_status call_instances(snap *s) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < s->pending.len; i++) {
        const pending *p = odin3_vec_cat(&s->pending, i);
        const site *hit = odin3_vec_cat(&s->sites, p->site);
        const odin3_construct_key key = {s->run, p->ast, ODIN3_CONSTRUCT_PART_NODE, 0};
        uint32_t leaf = odin3_construct_find(s->design, &key);
        const odin3_construct_row *row = odin3_construct_get(s->design, leaf);
        bool copied = false;
        if (hit->count == 1 && row != NULL && row->ast == p->ast) {
            st = copy_chain(s, leaf, hit->row, &copied);
        }
        if (st == ODIN3_OK && !copied) {
            st = odin3_construct_note_miss(s->design, (odin3_prov_id){p->record});
        }
    }
    return st;
}

/* --- entry --------------------------------------------------------------------------------- */

static void snap_free(snap *s) {
    odin3_util_free(s->flags);
    odin3_vec_free(&s->frames);
    odin3_vec_free(&s->path);
    odin3_vec_free(&s->sites);
    odin3_vec_free(&s->pending);
    odin3_vec_free(&s->chain);
}

static odin3_status snap_open(snap *s, odin3_design *design, const odin3_ast *ast,
                              odin3_construct_mode mode) {
    *s = (snap){.design = design, .ast = ast, .run = odin3_ast_run(ast), .mode = mode};
    odin3_vec_init(&s->frames, sizeof(frame));
    odin3_vec_init(&s->path, sizeof(uint32_t));
    odin3_vec_init(&s->sites, sizeof(site));
    odin3_vec_init(&s->pending, sizeof(pending));
    odin3_vec_init(&s->chain, sizeof(uint32_t));
    s->nodes = odin3_ast_node_end(ast).v;
    s->flags = odin3_util_calloc(s->nodes);
    odin3_srcman *sm = NULL;
    odin3_status st = s->flags != NULL ? odin3_design_get_srcman(design, &sm) : ODIN3_ERR_NO_MEMORY;
    s->sm = sm;
    return st;
}

static odin3_status snapshot_run(snap *s) {
    odin3_status st = sweep(s);
    for (uint32_t i = 0; st == ODIN3_OK && i < odin3_ast_root_count(s->ast); i++) {
        st = walk_root(s, odin3_ast_root(s->ast, i).v);
    }
    st = st != ODIN3_OK ? st : orphans(s);
    st = st != ODIN3_OK ? st : call_instances(s);
    return st != ODIN3_OK ? st : odin3_construct_mark_run(s->design, s->run, s->mode);
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
    snap s;
    odin3_status st = snap_open(&s, design, ast, mode);
    st = st != ODIN3_OK ? st : snapshot_run(&s);
    snap_free(&s);
    return st;
}
