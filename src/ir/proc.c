/*
 * proc.c — process bodies: the packed blob, its view, the allocation-free verify; the process
 * counter queries; the RTLIL spelling of values, signals and process blocks (2D IR change B,
 * EL-33, spec §8.1 and §13; docs/IR.md IR-19).
 */
#include "ir/proc.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/ir_internal.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/attr.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* --- the blob (plan point (e)) ------------------------------------------------------------- */

/* The six arrays of a body, in blob order. */
enum { B_PBITS, B_COMPARES, B_ACTIONS, B_CASES, B_SWITCHES, B_SYNCS, B_ARRAYS };

enum {
    BLOB_ALIGN = 8,                            /* every array starts 8-byte aligned */
    BLOB_HEADER = B_ARRAYS * sizeof(uint32_t), /* the six counts */
};

static const size_t k_record_bytes[B_ARRAYS] = {
    sizeof(odin3_pbit),      sizeof(odin3_pspan),       sizeof(odin3_proc_action),
    sizeof(odin3_proc_case), sizeof(odin3_proc_switch), sizeof(odin3_proc_sync),
};

/* Where each array of a body with these counts lives, and the blob's length. */
typedef struct blob_layout {
    uint32_t count[B_ARRAYS];
    uint64_t offset[B_ARRAYS];
    uint64_t size;
} blob_layout;

static uint64_t align_up(uint64_t bytes) {
    return (bytes + BLOB_ALIGN - 1) & ~(uint64_t)(BLOB_ALIGN - 1);
}

/* Fills offset and size from count (at most 6 * 2^32 * 28 bytes: no overflow in 64 bits). */
static void layout_of(blob_layout *lay) {
    uint64_t at = BLOB_HEADER;
    for (uint32_t i = 0; i < B_ARRAYS; i++) {
        lay->offset[i] = at;
        at = align_up(at + (uint64_t)lay->count[i] * k_record_bytes[i]);
    }
    lay->size = at;
}

static void counts_of(const odin3_proc_body *body, uint32_t *count) {
    count[B_PBITS] = body->n_pbits;
    count[B_COMPARES] = body->n_compares;
    count[B_ACTIONS] = body->n_actions;
    count[B_CASES] = body->n_cases;
    count[B_SWITCHES] = body->n_switches;
    count[B_SYNCS] = body->n_syncs;
}

static bool present(const void *array, uint32_t count) {
    return count == 0 || array != NULL;
}

static bool arrays_present(const odin3_proc_body *body) {
    return present(body->pbits, body->n_pbits) && present(body->compares, body->n_compares) &&
           present(body->actions, body->n_actions) && present(body->cases, body->n_cases) &&
           present(body->switches, body->n_switches) && present(body->syncs, body->n_syncs);
}

/* Field writers: bytes only, so the zeroed padding between fields is never touched. */
static void put_u32(uint8_t *at, uint32_t val) {
    memcpy(at, &val, sizeof val);
}

static void put_span(uint8_t *at, odin3_pspan span) {
    put_u32(at + offsetof(odin3_pspan, first), span.first);
    put_u32(at + offsetof(odin3_pspan, len), span.len);
}

static void put_action(uint8_t *at, const odin3_proc_action *act) {
    put_span(at + offsetof(odin3_proc_action, lhs), act->lhs);
    put_span(at + offsetof(odin3_proc_action, rhs), act->rhs);
    put_u32(at + offsetof(odin3_proc_action, prov), act->prov.v);
}

static void put_case(uint8_t *at, const odin3_proc_case *cs) {
    put_u32(at + offsetof(odin3_proc_case, first_compare), cs->first_compare);
    put_u32(at + offsetof(odin3_proc_case, n_compares), cs->n_compares);
    put_u32(at + offsetof(odin3_proc_case, first_action), cs->first_action);
    put_u32(at + offsetof(odin3_proc_case, n_actions), cs->n_actions);
    put_u32(at + offsetof(odin3_proc_case, first_switch), cs->first_switch);
    put_u32(at + offsetof(odin3_proc_case, n_switches), cs->n_switches);
    put_u32(at + offsetof(odin3_proc_case, prov), cs->prov.v);
}

static void put_switch(uint8_t *at, const odin3_proc_switch *sw) {
    put_span(at + offsetof(odin3_proc_switch, signal), sw->signal);
    put_u32(at + offsetof(odin3_proc_switch, first_case), sw->first_case);
    put_u32(at + offsetof(odin3_proc_switch, n_cases), sw->n_cases);
    memcpy(at + offsetof(odin3_proc_switch, flags), &sw->flags, sizeof sw->flags);
    put_u32(at + offsetof(odin3_proc_switch, prov), sw->prov.v);
}

static void put_sync(uint8_t *at, const odin3_proc_sync *sy) {
    memcpy(at + offsetof(odin3_proc_sync, type), &sy->type, sizeof sy->type);
    put_u32(at + offsetof(odin3_proc_sync, signal), sy->signal);
    put_u32(at + offsetof(odin3_proc_sync, first_action), sy->first_action);
    put_u32(at + offsetof(odin3_proc_sync, n_actions), sy->n_actions);
}

/* Where array `which` starts in the blob. */
static uint8_t *array_base(uint8_t *blob, const blob_layout *lay, uint32_t which) {
    return blob + (size_t)lay->offset[which];
}

static void write_blob(uint8_t *blob, const blob_layout *lay, const odin3_proc_body *body) {
    for (uint32_t i = 0; i < B_ARRAYS; i++) {
        put_u32(blob + (size_t)i * sizeof(uint32_t), lay->count[i]);
    }
    uint8_t *at = array_base(blob, lay, B_PBITS);
    for (uint32_t i = 0; i < body->n_pbits; i++, at += sizeof(odin3_pbit)) {
        put_u32(at, body->pbits[i]);
    }
    at = array_base(blob, lay, B_COMPARES);
    for (uint32_t i = 0; i < body->n_compares; i++, at += sizeof(odin3_pspan)) {
        put_span(at, body->compares[i]);
    }
    at = array_base(blob, lay, B_ACTIONS);
    for (uint32_t i = 0; i < body->n_actions; i++, at += sizeof(odin3_proc_action)) {
        put_action(at, &body->actions[i]);
    }
    at = array_base(blob, lay, B_CASES);
    for (uint32_t i = 0; i < body->n_cases; i++, at += sizeof(odin3_proc_case)) {
        put_case(at, &body->cases[i]);
    }
    at = array_base(blob, lay, B_SWITCHES);
    for (uint32_t i = 0; i < body->n_switches; i++, at += sizeof(odin3_proc_switch)) {
        put_switch(at, &body->switches[i]);
    }
    at = array_base(blob, lay, B_SYNCS);
    for (uint32_t i = 0; i < body->n_syncs; i++, at += sizeof(odin3_proc_sync)) {
        put_sync(at, &body->syncs[i]);
    }
}

odin3_status odin3_proc_body_pack(odin3_arena *arena, const odin3_proc_body *body,
                                  odin3_value *out) {
    if (arena == NULL || body == NULL || out == NULL || !arrays_present(body)) {
        odin3_log(ODIN3_LOG_ERROR, "proc_body_pack: no arena, body or output, or an array with "
                                   "entries is missing");
        return ODIN3_ERR_INVALID_ARG;
    }
    blob_layout lay;
    counts_of(body, lay.count);
    layout_of(&lay);
    if (lay.size > UINT32_MAX) {
        odin3_log(ODIN3_LOG_ERROR,
                  "proc_body_pack: the body needs %llu bytes, more than a value holds",
                  (unsigned long long)lay.size);
        return ODIN3_ERR_INVALID_ARG;
    }
    uint8_t *blob = odin3_arena_alloc(arena, (size_t)lay.size); /* zeroed, max_align_t aligned */
    if (blob == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    write_blob(blob, &lay, body);
    odin3_value val = {0};
    val.kind = ODIN3_VAL_PROC;
    val.bits = blob;
    val.len = (uint32_t)lay.size;
    *out = val;
    return ODIN3_OK;
}

/* Array `which` of the blob, NULL when empty (void: the caller's pointer type takes it). */
static const void *array_at(const uint8_t *blob, const blob_layout *lay, uint32_t which) {
    return lay->count[which] > 0 ? blob + (size_t)lay->offset[which] : NULL;
}

bool odin3_proc_body_view(const odin3_value *val, odin3_proc_body *out) {
    if (val == NULL || out == NULL || val->kind != ODIN3_VAL_PROC || val->bits == NULL ||
        val->len < BLOB_HEADER || (uintptr_t)val->bits % BLOB_ALIGN != 0) {
        return false;
    }
    blob_layout lay;
    memcpy(lay.count, val->bits, sizeof lay.count);
    layout_of(&lay);
    if (lay.size != val->len) {
        return false;
    }
    out->pbits = array_at(val->bits, &lay, B_PBITS);
    out->n_pbits = lay.count[B_PBITS];
    out->compares = array_at(val->bits, &lay, B_COMPARES);
    out->n_compares = lay.count[B_COMPARES];
    out->actions = array_at(val->bits, &lay, B_ACTIONS);
    out->n_actions = lay.count[B_ACTIONS];
    out->cases = array_at(val->bits, &lay, B_CASES);
    out->n_cases = lay.count[B_CASES];
    out->switches = array_at(val->bits, &lay, B_SWITCHES);
    out->n_switches = lay.count[B_SWITCHES];
    out->syncs = array_at(val->bits, &lay, B_SYNCS);
    out->n_syncs = lay.count[B_SYNCS];
    return true;
}

/* --- verify (IR-19; never allocates, review I3) -------------------------------------------- */

/*
 * The "assigned" and "twice" checks mark OUT positions in a fixed bitmap on the stack: one pass
 * covers WINDOW_BITS consecutive positions, so a process of up to that many outputs (or an lhs
 * spanning up to that many) takes one pass. The bitmap is all zero between passes.
 */
enum { WORD_BITS = 64, WINDOW_BITS = 8192, WINDOW_WORDS = WINDOW_BITS / WORD_BITS };

/* A partition being walked in order: where the next range must start, and the array's size. */
typedef struct part {
    uint64_t next;
    uint32_t end;
    const char *noun;
} part;

/* The record a message is about: "case 3", "switch 0", ... */
typedef struct owner {
    const char *kind;
    uint32_t index;
} owner;

typedef struct vctx {
    const odin3_proc_body *body;
    odin3_proc_widths widths;
    odin3_proc_why *why;
    part compares, actions, switches, cases;
    uint64_t window[WINDOW_WORDS];
} vctx;

static odin3_status fail(vctx *ctx, const char *fmt, ...) ODIN3_PRINTF(2, 3);

static odin3_status fail(vctx *ctx, const char *fmt, ...) {
    if (ctx->why != NULL) {
        va_list args;
        va_start(args, fmt);
        (void)vsnprintf(ctx->why->text, sizeof ctx->why->text, fmt, args);
        va_end(args);
    }
    return ODIN3_ERR_INVALID_ARG;
}

static uint32_t pbit_tag(odin3_pbit pbit) {
    return pbit & ~ODIN3_PBIT_INDEX_MASK;
}

static uint32_t pbit_index(odin3_pbit pbit) {
    return pbit & ODIN3_PBIT_INDEX_MASK;
}

/* Takes [first, first + len) as the next range of the partition. */
static odin3_status part_take(vctx *ctx, part *pt, owner who, odin3_pspan range) {
    if (range.first != pt->next) {
        return fail(ctx, "%s %u: %s start at %u, expected %llu", who.kind, who.index, pt->noun,
                    range.first, (unsigned long long)pt->next);
    }
    uint64_t end = (uint64_t)range.first + range.len;
    if (end > pt->end) {
        return fail(ctx, "%s %u: %s [%u, %llu) run past the %u %s", who.kind, who.index, pt->noun,
                    range.first, (unsigned long long)end, pt->end, pt->noun);
    }
    pt->next = end;
    return ODIN3_OK;
}

/* Every entry of the array was taken. */
static odin3_status part_done(vctx *ctx, const part *pt) {
    if (pt->next != pt->end) {
        return fail(ctx, "only %llu of the %u %s are used", (unsigned long long)pt->next, pt->end,
                    pt->noun);
    }
    return ODIN3_OK;
}

/* An IN or OUT position is inside its port (other tags are the caller's business). */
static odin3_status check_range(vctx *ctx, odin3_pbit pbit) {
    uint32_t idx = pbit_index(pbit);
    if (pbit_tag(pbit) == ODIN3_PBIT_IN && idx >= ctx->widths.i_width) {
        return fail(ctx, "IN position %u is out of range (I_WIDTH %u)", idx, ctx->widths.i_width);
    }
    if (pbit_tag(pbit) == ODIN3_PBIT_OUT && idx >= ctx->widths.o_width) {
        return fail(ctx, "OUT position %u is out of range (O_WIDTH %u)", idx, ctx->widths.o_width);
    }
    return ODIN3_OK;
}

/* A span lies inside pbits[]. */
static odin3_status check_span(vctx *ctx, owner who, const char *name, odin3_pspan span) {
    uint64_t end = (uint64_t)span.first + span.len;
    if (end > ctx->body->n_pbits) {
        return fail(ctx, "%s %u: %s [%u, %llu) is outside the %u positions", who.kind, who.index,
                    name, span.first, (unsigned long long)end, ctx->body->n_pbits);
    }
    return ODIN3_OK;
}

static bool span_has_any(const vctx *ctx, odin3_pspan span) {
    for (uint32_t i = 0; i < span.len; i++) {
        if (ctx->body->pbits[span.first + i] == ODIN3_PBIT_ANY) {
            return true;
        }
    }
    return false;
}

static bool span_all_out(const vctx *ctx, odin3_pspan span) {
    for (uint32_t i = 0; i < span.len; i++) {
        if (pbit_tag(ctx->body->pbits[span.first + i]) != ODIN3_PBIT_OUT) {
            return false;
        }
    }
    return true;
}

static odin3_status check_shape(vctx *ctx) {
    if (!arrays_present(ctx->body)) {
        return fail(ctx, "an array with entries is missing");
    }
    return ctx->body->n_cases > 0 ? ODIN3_OK : fail(ctx, "no root case");
}

/* Every position is IN or OUT inside its port, or exactly ANY. */
static odin3_status check_pbits(vctx *ctx) {
    for (uint32_t i = 0; i < ctx->body->n_pbits; i++) {
        odin3_pbit pbit = ctx->body->pbits[i];
        uint32_t tag = pbit_tag(pbit);
        if (tag != ODIN3_PBIT_IN && tag != ODIN3_PBIT_OUT && pbit != ODIN3_PBIT_ANY) {
            return fail(ctx, "pbit %u is neither IN, OUT nor ANY (0x%08x)", i, (unsigned)pbit);
        }
        odin3_status st = check_range(ctx, pbit);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Case idx: the root has no compares; its compares, actions and switches come next in order. */
static odin3_status check_case(vctx *ctx, uint32_t idx) {
    const odin3_proc_case *cs = &ctx->body->cases[idx];
    owner who = {"case", idx};
    if (idx == 0 && cs->n_compares != 0) {
        return fail(ctx, "root case has compares");
    }
    odin3_status st =
        part_take(ctx, &ctx->compares, who, (odin3_pspan){cs->first_compare, cs->n_compares});
    if (st == ODIN3_OK) {
        st = part_take(ctx, &ctx->actions, who, (odin3_pspan){cs->first_action, cs->n_actions});
    }
    if (st == ODIN3_OK) {
        st = part_take(ctx, &ctx->switches, who, (odin3_pspan){cs->first_switch, cs->n_switches});
    }
    return st;
}

/* The canonical layout's case side: compares and switches partitioned in case order. */
static odin3_status check_cases(vctx *ctx) {
    for (uint32_t i = 0; i < ctx->body->n_cases; i++) {
        odin3_status st = check_case(ctx, i);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    odin3_status st = part_done(ctx, &ctx->compares);
    return st == ODIN3_OK ? part_done(ctx, &ctx->switches) : st;
}

/* Each compare of each case of sw is inside pbits[] and as wide as the signal. */
static odin3_status check_compares(vctx *ctx, const odin3_proc_switch *sw, owner who) {
    const odin3_proc_body *body = ctx->body;
    for (uint32_t k = sw->first_case; k < sw->first_case + sw->n_cases; k++) {
        const odin3_proc_case *cs = &body->cases[k];
        for (uint32_t j = 0; j < cs->n_compares; j++) {
            odin3_pspan cmp = body->compares[cs->first_compare + j];
            uint64_t end = (uint64_t)cmp.first + cmp.len;
            if (end > body->n_pbits) {
                return fail(ctx,
                            "switch %u: case %u compare %u [%u, %llu) is outside the %u positions",
                            who.index, k, j, cmp.first, (unsigned long long)end, body->n_pbits);
            }
            if (cmp.len != sw->signal.len) {
                return fail(ctx, "switch %u: case %u compare %u has width %u; the signal has %u",
                            who.index, k, j, cmp.len, sw->signal.len);
            }
        }
    }
    return ODIN3_OK;
}

/* A switch and the case that owns it. */
typedef struct sw_at {
    uint32_t idx;
    uint32_t parent;
} sw_at;

static odin3_status check_switch(vctx *ctx, sw_at at) {
    const odin3_proc_switch *sw = &ctx->body->switches[at.idx];
    owner who = {"switch", at.idx};
    odin3_status st = part_take(ctx, &ctx->cases, who, (odin3_pspan){sw->first_case, sw->n_cases});
    if (st != ODIN3_OK) {
        return st;
    }
    if (sw->n_cases > 0 && sw->first_case <= at.parent) {
        return fail(ctx, "switch %u: case %u is not after its parent case %u", at.idx,
                    sw->first_case, at.parent);
    }
    st = check_span(ctx, who, "signal", sw->signal);
    if (st == ODIN3_OK && span_has_any(ctx, sw->signal)) {
        st = fail(ctx, "ANY outside a compare");
    }
    if (st == ODIN3_OK &&
        (sw->flags & ~(unsigned)(ODIN3_SWITCH_FULL_CASE | ODIN3_SWITCH_PARALLEL_CASE)) != 0) {
        st = fail(ctx, "switch %u: unknown flags 0x%x", at.idx, (unsigned)sw->flags);
    }
    return st == ODIN3_OK ? check_compares(ctx, sw, who) : st;
}

/*
 * The canonical layout's switch side: the cases of the switches partition [1, n_cases) in switch
 * order, and every case of a switch comes after the case owning it, so the cases and switches
 * form a tree under case 0 with each used once.
 */
static odin3_status check_switches(vctx *ctx) {
    const odin3_proc_body *body = ctx->body;
    uint32_t parent = 0;
    for (uint32_t i = 0; i < body->n_switches; i++) {
        while (parent + 1 < body->n_cases &&
               i >= (uint64_t)body->cases[parent].first_switch + body->cases[parent].n_switches) {
            parent++; /* the switch ranges partition [0, n_switches) in case order (checked) */
        }
        odin3_status st = check_switch(ctx, (sw_at){i, parent});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return part_done(ctx, &ctx->cases);
}

/* A sync's type and signal: one IN or OUT position for edge and level types (point (h)). */
static odin3_status check_sync_signal(vctx *ctx, uint32_t idx) {
    const odin3_proc_sync *sy = &ctx->body->syncs[idx];
    if (sy->type < ODIN3_SYNC_POSEDGE || sy->type > ODIN3_SYNC_ALWAYS) {
        return fail(ctx, "sync %u: unknown type %u", idx, (unsigned)sy->type);
    }
    if (sy->type == ODIN3_SYNC_ALWAYS) {
        return sy->signal == 0 ? ODIN3_OK : fail(ctx, "sync %u: an ALWAYS sync has a signal", idx);
    }
    uint32_t tag = pbit_tag(sy->signal);
    if (tag != ODIN3_PBIT_IN && tag != ODIN3_PBIT_OUT) {
        return fail(ctx, "sync %u: the signal must be one IN or OUT position", idx);
    }
    return check_range(ctx, sy->signal);
}

/* The sync actions follow the case actions, per sync in order, ending at n_actions. */
static odin3_status check_syncs(vctx *ctx) {
    for (uint32_t k = 0; k < ctx->body->n_syncs; k++) {
        const odin3_proc_sync *sy = &ctx->body->syncs[k];
        odin3_status st = part_take(ctx, &ctx->actions, (owner){"sync", k},
                                    (odin3_pspan){sy->first_action, sy->n_actions});
        if (st == ODIN3_OK) {
            st = check_sync_signal(ctx, k);
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return part_done(ctx, &ctx->actions);
}

static void window_clear(vctx *ctx, uint64_t bit) {
    ctx->window[bit / WORD_BITS] &= ~(UINT64_C(1) << (bit % WORD_BITS));
}

/* Marks a bit; true when it was marked already. */
static bool window_mark(vctx *ctx, uint64_t bit) {
    uint64_t mask = UINT64_C(1) << (bit % WORD_BITS);
    bool was = (ctx->window[bit / WORD_BITS] & mask) != 0;
    ctx->window[bit / WORD_BITS] |= mask;
    return was;
}

/* The positions of an lhs. */
typedef struct lhs_run {
    const odin3_pbit *pos;
    uint32_t len;
} lhs_run;

/* True (with *bit its offset) when OUT index idx falls in the window starting at base. */
static bool in_window(uint32_t idx, uint64_t base, uint64_t *bit) {
    if (idx < base || idx - base >= WINDOW_BITS) {
        return false;
    }
    *bit = idx - base;
    return true;
}

/* One window of the "twice" check: the first index met twice, else UINT32_MAX. */
static uint32_t window_twice(vctx *ctx, lhs_run run, uint64_t base) {
    uint32_t twice = UINT32_MAX;
    uint64_t bit = 0;
    for (uint32_t i = 0; i < run.len; i++) {
        uint32_t idx = pbit_index(run.pos[i]);
        if (in_window(idx, base, &bit) && window_mark(ctx, bit) && twice == UINT32_MAX) {
            twice = idx;
        }
    }
    for (uint32_t i = 0; i < run.len; i++) {
        if (in_window(pbit_index(run.pos[i]), base, &bit)) {
            window_clear(ctx, bit);
        }
    }
    return twice;
}

/* No OUT position twice in action idx's lhs (all OUT, checked). */
static odin3_status lhs_unique(vctx *ctx, uint32_t idx) {
    odin3_pspan lhs = ctx->body->actions[idx].lhs;
    if (lhs.len < 2) {
        return ODIN3_OK;
    }
    lhs_run run = {&ctx->body->pbits[lhs.first], lhs.len};
    uint32_t low = UINT32_MAX;
    uint32_t high = 0;
    for (uint32_t i = 0; i < run.len; i++) {
        uint32_t pos = pbit_index(run.pos[i]);
        low = pos < low ? pos : low;
        high = pos > high ? pos : high;
    }
    for (uint64_t base = low; base <= high; base += WINDOW_BITS) {
        uint32_t twice = window_twice(ctx, run, base);
        if (twice != UINT32_MAX) {
            return fail(ctx, "action %u: OUT position %u twice in one lhs", idx, twice);
        }
    }
    return ODIN3_OK;
}

static odin3_status check_action(vctx *ctx, uint32_t idx) {
    const odin3_proc_action *act = &ctx->body->actions[idx];
    owner who = {"action", idx};
    if (act->lhs.len == 0) {
        return fail(ctx, "action %u: empty", idx); /* Task 3 review M3: it would say nothing */
    }
    odin3_status st = check_span(ctx, who, "lhs", act->lhs);
    if (st == ODIN3_OK) {
        st = check_span(ctx, who, "rhs", act->rhs);
    }
    if (st == ODIN3_OK && !span_all_out(ctx, act->lhs)) {
        st = fail(ctx, "action %u: lhs holds a non-OUT position", idx);
    }
    if (st == ODIN3_OK && act->lhs.len != act->rhs.len) {
        st = fail(ctx, "action %u: lhs and rhs widths differ (%u, %u)", idx, act->lhs.len,
                  act->rhs.len);
    }
    if (st == ODIN3_OK && span_has_any(ctx, act->rhs)) {
        st = fail(ctx, "ANY outside a compare");
    }
    return st == ODIN3_OK ? lhs_unique(ctx, idx) : st;
}

static odin3_status check_actions(vctx *ctx) {
    for (uint32_t i = 0; i < ctx->body->n_actions; i++) {
        odin3_status st = check_action(ctx, i);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Marks every lhs position of every action that falls in the window starting at base. */
static void mark_assigned(vctx *ctx, uint64_t base) {
    const odin3_proc_body *body = ctx->body;
    uint64_t bit = 0;
    for (uint32_t k = 0; k < body->n_actions; k++) {
        odin3_pspan lhs = body->actions[k].lhs;
        for (uint32_t i = 0; i < lhs.len; i++) {
            if (in_window(pbit_index(body->pbits[lhs.first + i]), base, &bit)) {
                (void)window_mark(ctx, bit);
            }
        }
    }
}

/* The first of bits [0, count) not marked, else UINT32_MAX; leaves the window clear. */
static uint32_t first_unmarked(vctx *ctx, uint32_t count) {
    uint32_t missing = UINT32_MAX;
    for (uint32_t bit = 0; bit < count && missing == UINT32_MAX; bit++) {
        if ((ctx->window[bit / WORD_BITS] & (UINT64_C(1) << (bit % WORD_BITS))) == 0) {
            missing = bit;
        }
    }
    memset(ctx->window, 0, sizeof(uint64_t) * ((count + WORD_BITS - 1) / WORD_BITS));
    return missing;
}

/* Every OUT position is the lhs of some action (a pin the body never drives is rejected). */
static odin3_status check_assigned(vctx *ctx) {
    uint32_t width = ctx->widths.o_width;
    for (uint64_t base = 0; base < width; base += WINDOW_BITS) {
        uint64_t left = width - base;
        uint32_t count = left < WINDOW_BITS ? (uint32_t)left : WINDOW_BITS;
        mark_assigned(ctx, base);
        uint32_t missing = first_unmarked(ctx, count);
        if (missing != UINT32_MAX) {
            unsigned long long position = base + missing;
            return fail(ctx, "OUT position %llu is never assigned", position);
        }
    }
    return ODIN3_OK;
}

typedef odin3_status (*verify_step)(vctx *ctx);

/* In order: each step may rely on the ones before it (indices in range, the tree). */
static const verify_step k_steps[] = {check_shape, check_pbits,   check_cases,   check_switches,
                                      check_syncs, check_actions, check_assigned};

odin3_status odin3_proc_body_verify(const odin3_proc_body *body, odin3_proc_widths widths,
                                    odin3_proc_why *why) {
    vctx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.body = body;
    ctx.widths = widths;
    ctx.why = why;
    if (why != NULL) {
        why->text[0] = '\0';
    }
    if (body == NULL) {
        return fail(&ctx, "no body");
    }
    ctx.compares = (part){0, body->n_compares, "compares"};
    ctx.actions = (part){0, body->n_actions, "actions"};
    ctx.switches = (part){0, body->n_switches, "switches"};
    ctx.cases = (part){1, body->n_cases, "cases"}; /* case 0, the root, is in no switch */
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < sizeof k_steps / sizeof k_steps[0]; i++) {
        st = k_steps[i](&ctx);
    }
    return st;
}

/* --- the process counter ------------------------------------------------------------------- */

uint32_t odin3_module_proc_count(const odin3_module *module) {
    return module != NULL ? module->proc_count : 0;
}

odin3_module_id odin3_design_first_proc_module(const odin3_design *design) {
    for (size_t i = 1; design != NULL && i < design->modules.len; i++) {
        const odin3_module *const *slot =
            (const odin3_module *const *)odin3_vec_cat(&design->modules, i);
        if (*slot != NULL && (*slot)->proc_count > 0) {
            return (*slot)->id;
        }
    }
    return (odin3_module_id){0};
}

bool odin3_design_has_processes(const odin3_design *design) {
    return odin3_module_valid(odin3_design_first_proc_module(design));
}

odin3_status odin3_design_refuse_processes(const odin3_design *design, const char *pass) {
    odin3_module_id first = odin3_design_first_proc_module(design);
    if (!odin3_module_valid(first)) {
        return ODIN3_OK;
    }
    const odin3_module *const *slot =
        (const odin3_module *const *)odin3_vec_cat(&design->modules, first.v);
    const char *name = odin3_strtab_get(design->strtab, (*slot)->name);
    if (pass != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "pass '%s': module '%s' has processes; run proc first", pass,
                  name);
    } else {
        odin3_log(ODIN3_LOG_ERROR, "module '%s' has processes; run proc first", name);
    }
    return ODIN3_ERR_INVALID_ARG;
}

/* --- RTLIL text ---------------------------------------------------------------------------- */

/* Drops what was appended to out since mark (failed output leaves the buffer as it was). */
static void restore(odin3_strbuf *out, size_t mark) {
    if (out->len > mark) {
        out->len = mark;
        out->data[mark] = '\0';
    }
}

enum {
    OCTAL_MASK = 7, /* one octal digit */
    CHAR_BUF = 128,
    INT_BITS = 64,
    OCTAL_DIGITS = 3,
    OCTAL_BITS = 3,
    FIRST_PRINTABLE = 32,
    FIRST_HIGH = 128
};

/* Characters appended through a small buffer. */
typedef struct char_out {
    odin3_strbuf *out;
    odin3_status st;
    size_t used;
    char buf[CHAR_BUF];
} char_out;

static void co_flush(char_out *co) {
    if (co->st == ODIN3_OK && co->used > 0) {
        co->st = odin3_strbuf_append(co->out, (odin3_bytes){co->buf, co->used});
    }
    co->used = 0;
}

static void co_put(char_out *co, char chr) {
    if (co->used == CHAR_BUF) {
        co_flush(co);
    }
    co->buf[co->used++] = chr;
}

/* Characters of a 4-state bit (odin3_bit order). */
static const char k_bit_chars[] = "01xz";

/* An INT: decimal inside the 32-bit range (RTLIL's integers), else its 64 bits. */
static odin3_status int_text(int64_t num, odin3_strbuf *out) {
    if (num >= INT32_MIN && num <= INT32_MAX) {
        return odin3_strbuf_appendf(out, "%lld", (long long)num);
    }
    char_out co = {out, odin3_strbuf_appendf(out, "%d'", INT_BITS), 0, {0}};
    for (int bit = INT_BITS - 1; bit >= 0; bit--) {
        co_put(&co, (((uint64_t)num >> bit) & 1U) != 0 ? '1' : '0');
    }
    co_flush(&co);
    return co.st;
}

static odin3_status bits_text(const uint8_t *bits, uint32_t len, odin3_strbuf *out) {
    char_out co = {out, odin3_strbuf_appendf(out, "%u'", len), 0, {0}};
    for (uint32_t k = len; k > 0; k--) {
        co_put(&co, k_bit_chars[bits[k - 1] & 3U]);
    }
    co_flush(&co);
    return co.st;
}

/* One byte of a quoted string, escaped as Yosys's RTLIL backend does. */
static void quoted_byte(char_out *co, uint8_t byte) {
    if (byte == '\n' || byte == '\t') {
        co_put(co, '\\');
        co_put(co, byte == '\n' ? 'n' : 't');
    } else if (byte < FIRST_PRINTABLE || byte >= FIRST_HIGH) { /* Yosys: (signed) char < 32 */
        static const char k_octal[] = "01234567";
        co_put(co, '\\');
        for (unsigned digit = OCTAL_DIGITS; digit > 0; digit--) {
            co_put(co,
                   k_octal[((unsigned)byte >> ((digit - 1) * OCTAL_BITS)) & (unsigned)OCTAL_MASK]);
        }
    } else {
        if (byte == '"' || byte == '\\') {
            co_put(co, '\\');
        }
        co_put(co, (char)byte);
    }
}

static odin3_status quoted_text(odin3_bytes text, odin3_strbuf *out) {
    char_out co = {out, ODIN3_OK, 0, {0}};
    const uint8_t *bytes = text.ptr;
    co_put(&co, '"');
    for (size_t i = 0; i < text.len; i++) {
        quoted_byte(&co, bytes[i]);
    }
    co_put(&co, '"');
    co_flush(&co);
    return co.st;
}

odin3_status odin3_proc_value_text(const odin3_design *design, const odin3_value *val,
                                   odin3_strbuf *out) {
    size_t mark = out->len;
    odin3_status st = ODIN3_ERR_INVALID_ARG;
    const odin3_strtab *strtab = odin3_design_strtab(design);
    switch (val->kind) {
    case ODIN3_VAL_INT:
        st = int_text(val->i, out);
        break;
    case ODIN3_VAL_BITS:
        st = bits_text(val->bits, val->len, out);
        break;
    case ODIN3_VAL_STRING:
        st = quoted_text(
            (odin3_bytes){odin3_strtab_get(strtab, val->str), odin3_strtab_len(strtab, val->str)},
            out);
        break;
    case ODIN3_VAL_COVER:
        st = quoted_text((odin3_bytes){val->bits, val->len}, out);
        break;
    case ODIN3_VAL_PROC: /* a body is never an attribute or a printable parameter value */
        break;
    }
    if (st != ODIN3_OK) {
        restore(out, mark);
    }
    return st;
}

/* --- sigspecs (plan point (s)) ------------------------------------------------------------- */

typedef enum sig_kind { SIG_CONST, SIG_WIRE, SIG_NET } sig_kind;

/* How one net is spelled. */
typedef struct sig_bit {
    sig_kind kind;
    char chr;         /* SIG_CONST: '0', '1', 'x', 'z' or '-' */
    odin3_wirebit wb; /* SIG_WIRE */
    odin3_net_id net;
} sig_bit;

/* Characters of odin3_const (NONE has none). */
static const char k_const_chars[] = "?01xz";

static sig_bit classify(const odin3_module *module, odin3_net_id net) {
    sig_bit bit = {SIG_CONST, '-', {{0}, 0}, net};
    if (!odin3_net_valid(net)) {
        return bit; /* ANY */
    }
    odin3_const value = odin3_net_const_value(module, net);
    if (value != ODIN3_CONST_NONE) {
        bit.chr = k_const_chars[value];
        return bit;
    }
    odin3_wirebit wb = odin3_net_primary(module, net);
    if (odin3_wire_valid(wb.wire) && odin3_wire_name(module, wb.wire) != 0) {
        bit.kind = SIG_WIRE;
        bit.wb = wb;
        return bit;
    }
    bit.kind = SIG_NET;
    return bit;
}

/* The nets being spelled, LSB first. */
typedef struct sig_src {
    const odin3_module *module;
    const odin3_net_id *nets;
} sig_src;

/*
 * True when net `high` continues the chunk of net high - 1: both constants, or the next bit of
 * the same wire.
 */
static bool joins_at(sig_src src, uint32_t high) {
    sig_bit low = classify(src.module, src.nets[high - 1]);
    sig_bit up = classify(src.module, src.nets[high]);
    if (low.kind == SIG_CONST && up.kind == SIG_CONST) {
        return true;
    }
    return low.kind == SIG_WIRE && up.kind == SIG_WIRE && low.wb.wire.v == up.wb.wire.v &&
           (uint64_t)up.wb.bit == (uint64_t)low.wb.bit + 1;
}

static const char *name_of(const odin3_module *module, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(odin3_module_design(module)), str);
}

/* A run of constants [lo, hi] as <n>'<bits>, MSB first. */
static odin3_status const_chunk(sig_src src, odin3_pspan run, odin3_strbuf *out) {
    char_out co = {out, odin3_strbuf_appendf(out, "%u'", run.len), 0, {0}};
    for (uint32_t i = run.len; i > 0; i--) {
        co_put(&co, classify(src.module, src.nets[run.first + i - 1]).chr);
    }
    co_flush(&co);
    return co.st;
}

/* A run of consecutive bits of one wire: \w, \w [k] or \w [hi:lo]. */
static odin3_status wire_chunk(sig_src src, odin3_pspan run, odin3_strbuf *out) {
    sig_bit low = classify(src.module, src.nets[run.first]);
    const char *name = name_of(src.module, odin3_wire_name(src.module, low.wb.wire));
    if (low.wb.bit == 0 && run.len == odin3_wire_width(src.module, low.wb.wire)) {
        return odin3_strbuf_appendf(out, "\\%s", name);
    }
    if (run.len == 1) {
        return odin3_strbuf_appendf(out, "\\%s [%u]", name, low.wb.bit);
    }
    return odin3_strbuf_appendf(out, "\\%s [%u:%u]", name, low.wb.bit + run.len - 1, low.wb.bit);
}

static odin3_status put_chunk(sig_src src, odin3_pspan run, odin3_strbuf *out) {
    sig_bit top = classify(src.module, src.nets[run.first + run.len - 1]);
    switch (top.kind) {
    case SIG_CONST:
        return const_chunk(src, run, out);
    case SIG_WIRE:
        return wire_chunk(src, run, out);
    case SIG_NET:
        break;
    }
    uint32_t name = odin3_net_name(src.module, top.net);
    return name != 0 ? odin3_strbuf_appendf(out, "\\%s", name_of(src.module, name))
                     : odin3_strbuf_appendf(out, "$n%u", top.net.v);
}

/* Writes the chunks MSB first, each followed by sep (braced form) or alone (one chunk). */
static odin3_status put_chunks(sig_src src, uint32_t n, const char *sep, odin3_strbuf *out) {
    odin3_status st = ODIN3_OK;
    for (uint32_t high = n; st == ODIN3_OK && high > 0;) {
        uint32_t low = high - 1; /* the chunk is [low, high) */
        while (low > 0 && joins_at(src, low)) {
            low--;
        }
        st = put_chunk(src, (odin3_pspan){low, high - low}, out);
        if (st == ODIN3_OK) {
            st = odin3_strbuf_append(out, odin3_bytes_cstr(sep));
        }
        high = low;
    }
    return st;
}

odin3_status odin3_proc_sigspec(const odin3_module *module, const odin3_net_id *nets, uint32_t n,
                                odin3_strbuf *out) {
    size_t mark = out->len;
    sig_src src = {module, nets};
    uint32_t chunks = n > 0 ? 1 : 0;
    for (uint32_t i = 1; i < n; i++) {
        chunks += joins_at(src, i) ? 0U : 1U;
    }
    odin3_status st = ODIN3_OK;
    if (chunks == 1) {
        st = put_chunks(src, n, "", out);
    } else {
        st = odin3_strbuf_append(out, odin3_bytes_cstr("{ "));
        if (st == ODIN3_OK) {
            st = put_chunks(src, n, " ", out);
        }
        if (st == ODIN3_OK) {
            st = odin3_strbuf_append(out, odin3_bytes_cstr("}"));
        }
    }
    if (st != ODIN3_OK) {
        restore(out, mark);
    }
    return st;
}

/* --- process blocks (§13, plan point (r)) -------------------------------------------------- */

/* A case or switch being printed: the next switch (of a case) or case (of a switch) to print. */
typedef struct pr_frame {
    uint32_t idx;
    uint32_t next;
    uint32_t indent;
    bool is_switch;
} pr_frame;

typedef struct proc_printer {
    const odin3_module *module;
    odin3_node_id node;
    odin3_proc_body body;
    odin3_pinslice in, outs; /* the I and O pins */
    odin3_net_id *nets;      /* scratch: the nets of one span */
    pr_frame *stack;         /* 2 * n_switches + 1 frames: a case and a switch per level */
    odin3_strbuf *out;
    odin3_status st;
} proc_printer;

/* Where a line goes and what it starts with. */
typedef struct line_at {
    uint32_t indent;
    const char *word;
} line_at;

static void pr_text(proc_printer *pr, const char *text) {
    if (pr->st == ODIN3_OK) {
        pr->st = odin3_strbuf_append(pr->out, odin3_bytes_cstr(text));
    }
}

static void pr_indent(proc_printer *pr, uint32_t indent) {
    static const char k_spaces[] = "                                ";
    enum { SPACES = sizeof k_spaces - 1 };
    for (uint32_t left = indent; pr->st == ODIN3_OK && left > 0;) {
        uint32_t take = left < SPACES ? left : SPACES;
        pr->st = odin3_strbuf_append(pr->out, (odin3_bytes){k_spaces, take});
        left -= take;
    }
}

/*
 * The net at a position; none for ANY. A position on an unconnected pin fails the print (Task 3
 * review M1): spelled as none it would read as the wildcard '-'.
 */
static odin3_net_id pbit_net(proc_printer *pr, odin3_pbit pbit) {
    uint32_t idx = pbit_index(pbit);
    uint32_t tag = pbit_tag(pbit);
    if (tag != ODIN3_PBIT_IN && tag != ODIN3_PBIT_OUT) {
        return (odin3_net_id){0}; /* ANY */
    }
    odin3_pinslice port = tag == ODIN3_PBIT_IN ? pr->in : pr->outs;
    odin3_net_id net = odin3_pin_net(pr->module, (odin3_pin_id){port.first.v + idx});
    if (!odin3_net_valid(net) && pr->st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "proc_print: node %u: position %s|%u is on an unconnected pin",
                  pr->node.v, tag == ODIN3_PBIT_IN ? "IN" : "OUT", idx);
        pr->st = ODIN3_ERR_INVALID_ARG;
    }
    return net;
}

static void pr_span(proc_printer *pr, odin3_pspan span) {
    for (uint32_t i = 0; i < span.len; i++) {
        pr->nets[i] = pbit_net(pr, pr->body.pbits[span.first + i]);
    }
    if (pr->st == ODIN3_OK) {
        pr->st = odin3_proc_sigspec(pr->module, pr->nets, span.len, pr->out);
    }
}

/* One "assign lhs rhs" or "update lhs rhs" line per action of the range. */
static void pr_actions(proc_printer *pr, odin3_pspan range, line_at at) {
    for (uint32_t i = range.first; i < range.first + range.len; i++) {
        const odin3_proc_action *act = &pr->body.actions[i];
        pr_indent(pr, at.indent);
        pr_text(pr, at.word);
        pr_text(pr, " ");
        pr_span(pr, act->lhs);
        pr_text(pr, " ");
        pr_span(pr, act->rhs);
        pr_text(pr, "\n");
    }
}

static void pr_switch_head(proc_printer *pr, const odin3_proc_switch *sw, uint32_t indent) {
    if ((sw->flags & ODIN3_SWITCH_FULL_CASE) != 0) {
        pr_indent(pr, indent);
        pr_text(pr, "attribute \\full_case 1\n");
    }
    if ((sw->flags & ODIN3_SWITCH_PARALLEL_CASE) != 0) {
        pr_indent(pr, indent);
        pr_text(pr, "attribute \\parallel_case 1\n");
    }
    pr_indent(pr, indent);
    pr_text(pr, "switch ");
    pr_span(pr, sw->signal);
    pr_text(pr, "\n");
}

/* "case" and its compares separated by " , " (none: the default case). */
static void pr_case_head(proc_printer *pr, const odin3_proc_case *cs, uint32_t indent) {
    pr_indent(pr, indent);
    pr_text(pr, "case");
    for (uint32_t j = 0; j < cs->n_compares; j++) {
        pr_text(pr, j == 0 ? " " : " , ");
        pr_span(pr, pr->body.compares[cs->first_compare + j]);
    }
    pr_text(pr, "\n");
}

/* A case frame on top: print its next switch's head and push the switch, or pop. */
static size_t pr_case_step(proc_printer *pr, size_t depth) {
    pr_frame *top = &pr->stack[depth - 1];
    const odin3_proc_case *cs = &pr->body.cases[top->idx];
    if (top->next >= (uint64_t)cs->first_switch + cs->n_switches) {
        return depth - 1;
    }
    uint32_t sw_idx = top->next++;
    const odin3_proc_switch *sw = &pr->body.switches[sw_idx];
    pr_switch_head(pr, sw, top->indent);
    pr->stack[depth] = (pr_frame){sw_idx, sw->first_case, top->indent, true};
    return depth + 1;
}

/* A switch frame on top: print its next case (head and actions) and push it, or end and pop. */
static size_t pr_switch_step(proc_printer *pr, size_t depth) {
    pr_frame *top = &pr->stack[depth - 1];
    const odin3_proc_switch *sw = &pr->body.switches[top->idx];
    if (top->next >= (uint64_t)sw->first_case + sw->n_cases) {
        pr_indent(pr, top->indent);
        pr_text(pr, "end\n");
        return depth - 1;
    }
    uint32_t case_idx = top->next++;
    const odin3_proc_case *cs = &pr->body.cases[case_idx];
    pr_case_head(pr, cs, top->indent + 2);
    pr_actions(pr, (odin3_pspan){cs->first_action, cs->n_actions},
               (line_at){top->indent + 4, "assign"});
    pr->stack[depth] = (pr_frame){case_idx, cs->first_switch, top->indent + 4, false};
    return depth + 1;
}

/* The root case: its actions, then its switches, depth first with an explicit stack. */
static void pr_root(proc_printer *pr, uint32_t indent) {
    const odin3_proc_case *root = &pr->body.cases[0];
    pr_actions(pr, (odin3_pspan){root->first_action, root->n_actions}, (line_at){indent, "assign"});
    size_t depth = 1;
    pr->stack[0] = (pr_frame){0, root->first_switch, indent, false};
    while (depth > 0 && pr->st == ODIN3_OK) {
        depth =
            pr->stack[depth - 1].is_switch ? pr_switch_step(pr, depth) : pr_case_step(pr, depth);
    }
}

static void pr_syncs(proc_printer *pr, uint32_t indent) {
    static const char *const k_words[] = {"", "posedge", "negedge", "low", "high", "always"};
    for (uint32_t k = 0; k < pr->body.n_syncs; k++) {
        const odin3_proc_sync *sy = &pr->body.syncs[k];
        pr_indent(pr, indent);
        pr_text(pr, "sync ");
        pr_text(pr, k_words[sy->type]);
        if (sy->type != ODIN3_SYNC_ALWAYS) {
            pr->nets[0] = pbit_net(pr, sy->signal);
            pr_text(pr, " ");
            if (pr->st == ODIN3_OK) {
                pr->st = odin3_proc_sigspec(pr->module, pr->nets, 1, pr->out);
            }
        }
        pr_text(pr, "\n");
        pr_actions(pr, (odin3_pspan){sy->first_action, sy->n_actions},
                   (line_at){indent + 2, "update"});
    }
}

/* attr_foreach visitor: "attribute \key value". */
typedef struct pr_attr_ctx {
    proc_printer *pr;
    uint32_t indent;
} pr_attr_ctx;

static odin3_status pr_attr(void *user, uint32_t key_str, const odin3_value *value) {
    pr_attr_ctx *ctx = user;
    proc_printer *pr = ctx->pr;
    pr_indent(pr, ctx->indent);
    pr_text(pr, "attribute \\");
    pr_text(pr, name_of(pr->module, key_str));
    pr_text(pr, " ");
    if (pr->st == ODIN3_OK) {
        pr->st = odin3_proc_value_text(odin3_module_design(pr->module), value, pr->out);
    }
    pr_text(pr, "\n");
    return pr->st;
}

/* "process $proc$<file>:<line>$<ID>" from the node's SOURCE record, else "$proc$<ID>". */
static void pr_name(proc_printer *pr, uint32_t indent) {
    const odin3_design *design = odin3_module_design(pr->module);
    const odin3_prov_record *rec = odin3_prov_get(design, odin3_node_prov(pr->module, pr->node));
    pr_indent(pr, indent);
    if (pr->st != ODIN3_OK) {
        return;
    }
    if (rec != NULL && rec->kind == ODIN3_PROV_SOURCE && rec->n_locs > 0 &&
        rec->locs[0].file != 0) {
        pr->st = odin3_strbuf_appendf(pr->out, "process $proc$%s:%u$%u\n",
                                      name_of(pr->module, rec->locs[0].file), rec->locs[0].line,
                                      pr->node.v);
    } else {
        pr->st = odin3_strbuf_appendf(pr->out, "process $proc$%u\n", pr->node.v);
    }
}

/* The node's verified body and its pins; INVALID_ARG (logged) when it is not a live process. */
static odin3_status printer_body(proc_printer *pr) {
    const odin3_module *module = pr->module;
    odin3_node_id node = pr->node;
    const odin3_celltype_def *def =
        odin3_node_live(module, node)
            ? odin3_celltype_get(odin3_module_design(module), odin3_node_type(module, node))
            : NULL;
    if (def == NULL || (def->flags & ODIN3_CT_PROCESS) == 0) {
        odin3_log(ODIN3_LOG_ERROR, "proc_print: node %u is not a live process", node.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_proc_widths widths = {(uint32_t)odin3_node_param(module, node, ODIN3_PROC_P_I_WIDTH)->i,
                                (uint32_t)odin3_node_param(module, node, ODIN3_PROC_P_O_WIDTH)->i};
    odin3_proc_why why = {""};
    if (!odin3_proc_body_view(odin3_node_param(module, node, ODIN3_PROC_P_BODY), &pr->body) ||
        odin3_proc_body_verify(&pr->body, widths, &why) != ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "proc_print: node %u: BODY: %s", node.v,
                  why.text[0] != '\0' ? why.text : "not a process body");
        return ODIN3_ERR_INVALID_ARG;
    }
    pr->in = odin3_node_port(module, node, ODIN3_PROC_PORT_I);
    pr->outs = odin3_node_port(module, node, ODIN3_PROC_PORT_O);
    return ODIN3_OK;
}

/* Checks the node and allocates the scratch (one net per position, the frame stack). */
static odin3_status printer_open(proc_printer *pr) {
    odin3_status st = printer_body(pr);
    if (st != ODIN3_OK) {
        return st;
    }
    size_t positions = pr->body.n_pbits > 0 ? pr->body.n_pbits : 1;
    pr->nets = odin3_util_malloc(sizeof *pr->nets * positions);
    pr->stack = odin3_util_malloc(sizeof *pr->stack * (2 * (size_t)pr->body.n_switches + 1));
    return pr->nets != NULL && pr->stack != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

odin3_status odin3_proc_print(const odin3_module *module, odin3_node_id node, uint32_t indent,
                              odin3_strbuf *out) {
    if (module == NULL || out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "proc_print: no module or output");
        return ODIN3_ERR_INVALID_ARG;
    }
    proc_printer pr;
    memset(&pr, 0, sizeof pr);
    pr.module = module;
    pr.node = node;
    pr.out = out;
    size_t mark = out->len;
    pr.st = printer_open(&pr);
    if (pr.st == ODIN3_OK) {
        pr_attr_ctx attrs = {&pr, indent};
        odin3_status st =
            odin3_attr_foreach(module, (odin3_objref){ODIN3_OBJ_NODE, node.v}, pr_attr, &attrs);
        pr.st = pr.st != ODIN3_OK ? pr.st : st;
        pr_name(&pr, indent);
        pr_root(&pr, indent + 2);
        pr_syncs(&pr, indent + 2);
        pr_indent(&pr, indent);
        pr_text(&pr, "end\n");
    }
    odin3_util_free(pr.nets);
    odin3_util_free(pr.stack);
    if (pr.st != ODIN3_OK) {
        restore(out, mark);
    }
    return pr.st;
}
