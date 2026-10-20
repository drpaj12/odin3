/*
 * ceval.c — the constant evaluator (ceval.h): values on nets with an undo journal, an explicit
 * frame stack for eval(sig) and eval(cell), the mux rule of consteval.h and the foldable cells
 * through odin3_opt_eval_cell.
 */
#include "passes/fsm/ceval.h"

#include "frontends/elab/cval.h"
#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/fsm/fsmdata.h"
#include "passes/fsm/index.h"
#include "passes/opt/fold.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { EVAL_ARENA_CHUNK = 4096 };

typedef enum frame_kind { FRAME_SIG, FRAME_CELL } frame_kind;
typedef enum step_result { STEP_CONT, STEP_PUSHED, STEP_OK, STEP_FAIL } step_result;

/* An evaluation in progress: a signal (FRAME_SIG) or a cell (FRAME_CELL). */
typedef struct frame {
    uint8_t kind, phase, is_mux, has_busy;
    uint32_t node;      /* SIG: the busy cell (0 none); CELL: the cell */
    odin3_fsm_sig *ext; /* SIG of the caller's array, else the pool at off */
    uint32_t off, len;
    uint32_t pool_mark, drv_mark, drv_i, drv_n;
    uint32_t a_off, a_n, b_off, b_n, s_off, s_n, y_off, y_n, cand;
} frame;

typedef struct journal_entry {
    uint32_t net, old;
} journal_entry;

struct odin3_fsm_ceval {
    const odin3_fsm_index *ix;
    odin3_fsm_budget *budget;
    odin3_opt_opts opts;
    odin3_opt_ctx oc;
    uint8_t *val, *stop, *busy;
    odin3_vec journal, marks, frames, pool, drivers;
    odin3_fsm_ceval_res *res;
};

/* --- values -------------------------------------------------------------------------------- */

void odin3_fsm_ceval_destroy(odin3_fsm_ceval *ce) {
    if (ce == NULL) {
        return;
    }
    odin3_util_free(ce->val);
    odin3_util_free(ce->stop);
    odin3_util_free(ce->busy);
    odin3_vec_free(&ce->journal);
    odin3_vec_free(&ce->marks);
    odin3_vec_free(&ce->frames);
    odin3_vec_free(&ce->pool);
    odin3_vec_free(&ce->drivers);
    odin3_util_free(ce);
}

odin3_status odin3_fsm_ceval_create(const odin3_fsm_index *ix, odin3_fsm_budget *budget,
                                    odin3_fsm_ceval **out) {
    *out = NULL;
    odin3_fsm_ceval *ce = odin3_util_calloc(sizeof *ce);
    if (ce == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    ce->ix = ix;
    ce->budget = budget;
    ce->val = odin3_util_malloc((size_t)ix->net_end + 1);
    ce->stop = odin3_util_calloc((size_t)ix->net_end + 1);
    ce->busy = odin3_util_calloc((size_t)ix->node_end + 1);
    odin3_vec_init(&ce->journal, sizeof(journal_entry));
    odin3_vec_init(&ce->marks, sizeof(uint32_t));
    odin3_vec_init(&ce->frames, sizeof(frame));
    odin3_vec_init(&ce->pool, sizeof(odin3_fsm_sig));
    odin3_vec_init(&ce->drivers, sizeof(uint32_t));
    if (ce->val == NULL || ce->stop == NULL || ce->busy == NULL) {
        odin3_fsm_ceval_destroy(ce);
        return ODIN3_ERR_NO_MEMORY;
    }
    memset(ce->val, ODIN3_FSM_NO_CONST, (size_t)ix->net_end + 1);
    ce->oc.design = ix->design;
    ce->oc.module = ix->module;
    ce->oc.opts = &ce->opts;
    *out = ce;
    return ODIN3_OK;
}

void odin3_fsm_ceval_stop(odin3_fsm_ceval *ce, const odin3_fsm_sig *sig, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (!odin3_fsm_sig_is_const(sig[i])) {
            ce->stop[sig[i]] = 1;
        }
    }
}

bool odin3_fsm_ceval_is_stop(const odin3_fsm_ceval *ce, odin3_fsm_sig sig) {
    return !odin3_fsm_sig_is_const(sig) && ce->stop[sig] != 0;
}

odin3_status odin3_fsm_ceval_push(odin3_fsm_ceval *ce) {
    uint32_t *mark = odin3_vec_push(&ce->marks);
    if (mark == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *mark = (uint32_t)ce->journal.len;
    return ODIN3_OK;
}

void odin3_fsm_ceval_pop(odin3_fsm_ceval *ce) {
    const uint32_t *mark = odin3_vec_cat(&ce->marks, ce->marks.len - 1);
    size_t keep = *mark;
    while (ce->journal.len > keep) {
        const journal_entry *entry = odin3_vec_cat(&ce->journal, ce->journal.len - 1);
        ce->val[entry->net] = (uint8_t)entry->old;
        odin3_vec_pop(&ce->journal);
    }
    odin3_vec_pop(&ce->marks);
}

size_t odin3_fsm_ceval_depth(const odin3_fsm_ceval *ce) {
    return ce->marks.len;
}

void odin3_fsm_ceval_unwind(odin3_fsm_ceval *ce, size_t depth) {
    while (ce->marks.len > depth) {
        odin3_fsm_ceval_pop(ce);
    }
}

static odin3_status set_one(odin3_fsm_ceval *ce, odin3_fsm_sig net, uint8_t bit) {
    if (ce->marks.len > 0) {
        journal_entry *entry = odin3_vec_push(&ce->journal);
        if (entry == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        entry->net = net;
        entry->old = ce->val[net];
    }
    ce->val[net] = bit;
    return ODIN3_OK;
}

odin3_status odin3_fsm_ceval_set(odin3_fsm_ceval *ce, const odin3_fsm_sig *sig, const uint8_t *bits,
                                 uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (odin3_fsm_sig_is_const(sig[i]) || bits[i] > ODIN3_FSM_BIT_Z) {
            continue;
        }
        odin3_status st = set_one(ce, sig[i], bits[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

odin3_fsm_sig odin3_fsm_ceval_get(const odin3_fsm_ceval *ce, odin3_fsm_sig sig) {
    if (odin3_fsm_sig_is_const(sig) || ce->val[sig] == ODIN3_FSM_NO_CONST) {
        return sig;
    }
    return odin3_fsm_sig_of_bit(ce->val[sig]);
}

/* values_map.apply(sig): true when every bit is constant afterwards. */
static bool apply(const odin3_fsm_ceval *ce, odin3_fsm_sig *sig, uint32_t count) {
    bool all_const = true;
    for (uint32_t i = 0; i < count; i++) {
        sig[i] = odin3_fsm_ceval_get(ce, sig[i]);
        all_const = all_const && odin3_fsm_sig_is_const(sig[i]);
    }
    return all_const;
}

/* --- the frame stack ----------------------------------------------------------------------- */

static frame *top_frame(odin3_fsm_ceval *ce) {
    return odin3_vec_at(&ce->frames, ce->frames.len - 1);
}

static odin3_fsm_sig *frame_sig(odin3_fsm_ceval *ce, const frame *fr) {
    odin3_fsm_sig *pool = ce->pool.data;
    return fr->ext != NULL ? fr->ext : pool + fr->off;
}

static odin3_fsm_sig *pool_at(odin3_fsm_ceval *ce, uint32_t off) {
    odin3_fsm_sig *pool = ce->pool.data;
    return pool + off;
}

typedef struct frame_req {
    frame_kind kind;
    uint32_t node;
} frame_req;

/* A span of the pool: the signal bits [off, off + len). */
typedef struct pool_span {
    uint32_t off, len;
} pool_span;

/* Pushes a frame; the caller sets what it needs. NO_MEMORY with nothing pushed. */
static odin3_status push_frame(odin3_fsm_ceval *ce, frame_req req) {
    frame *fr = odin3_vec_push(&ce->frames);
    if (fr == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    fr->kind = (uint8_t)req.kind;
    fr->node = req.node;
    fr->pool_mark = (uint32_t)ce->pool.len;
    fr->drv_mark = (uint32_t)ce->drivers.len;
    return ODIN3_OK;
}

/* Releases the pool and driver space of the top frame and drops it. */
static void pop_frame(odin3_fsm_ceval *ce) {
    const frame *fr = top_frame(ce);
    odin3_kit_truncate(&ce->pool, fr->pool_mark);
    odin3_kit_truncate(&ce->drivers, fr->drv_mark);
    odin3_vec_pop(&ce->frames);
}

/* A failure: every frame goes, a busy mark with it. */
static void unwind(odin3_fsm_ceval *ce) {
    while (ce->frames.len > 0) {
        const frame *fr = top_frame(ce);
        if (fr->kind == FRAME_SIG && fr->has_busy) {
            ce->busy[fr->node] = 0;
        }
        pop_frame(ce);
    }
}

/* Copies the signal bits of port `port` of node into the pool. */
static odin3_status pool_port(odin3_fsm_ceval *ce, odin3_node_id node, uint32_t port, uint32_t *off,
                              uint32_t *count) {
    *off = (uint32_t)ce->pool.len;
    odin3_status st = odin3_fsm_ix_port_sigs(ce->ix, node, port, &ce->pool);
    *count = (uint32_t)ce->pool.len - *off;
    return st;
}

/* --- eval(sig) ------------------------------------------------------------------------------- */

static int cmp_u32(const void *lhs, const void *rhs) {
    uint32_t lhs_v = *(const uint32_t *)lhs;
    uint32_t rhs_v = *(const uint32_t *)rhs;
    return (lhs_v > rhs_v) - (lhs_v < rhs_v);
}

/* The drivers of the bits of sig: internal cells with an OUT reference, sorted by node ID. */
static odin3_status collect_drivers(odin3_fsm_ceval *ce, const odin3_fsm_sig *sig, uint32_t count,
                                    uint32_t *out_n) {
    size_t first = ce->drivers.len;
    for (uint32_t i = 0; i < count; i++) {
        if (odin3_fsm_sig_is_const(sig[i])) {
            continue;
        }
        odin3_fsm_refs span = odin3_fsm_ix_refs(ce->ix, sig[i]);
        for (uint32_t k = 0; k < span.count; k++) {
            uint32_t flags = span.refs[k].flags;
            if ((flags & ODIN3_FSM_REF_OUT) != 0 &&
                (flags & (ODIN3_FSM_REF_UNKNOWN | ODIN3_FSM_REF_SEQ)) == 0 &&
                odin3_kit_push_u32(&ce->drivers, span.refs[k].node) != ODIN3_OK) {
                return ODIN3_ERR_NO_MEMORY;
            }
        }
    }
    *out_n = 0;
    if (ce->drivers.len == first) {
        return ODIN3_OK;
    }
    uint32_t *base = ce->drivers.data;
    qsort(base + first, ce->drivers.len - first, sizeof *base, cmp_u32);
    size_t kept = first;
    for (size_t i = first; i < ce->drivers.len; i++) {
        if (kept == first || base[kept - 1] != base[i]) {
            base[kept++] = base[i];
        }
    }
    odin3_kit_truncate(&ce->drivers, kept);
    *out_n = (uint32_t)(kept - first);
    return ODIN3_OK;
}

static step_result fail_undef(odin3_fsm_ceval *ce, odin3_fsm_sig bit) {
    ce->res->has_undef = true;
    ce->res->undef = bit;
    return STEP_FAIL;
}

/* The first bit that is not constant. */
static odin3_fsm_sig first_wire_bit(const odin3_fsm_sig *sig, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        if (!odin3_fsm_sig_is_const(sig[i])) {
            return sig[i];
        }
    }
    return sig[0];
}

/* Phase 0 of eval(sig): constants, stop signals, the busy guard, the drivers. */
static step_result sig_begin(odin3_fsm_ceval *ce, odin3_status *st) {
    frame *fr = top_frame(ce);
    odin3_fsm_sig *sig = frame_sig(ce, fr);
    if (apply(ce, sig, fr->len)) {
        return STEP_OK;
    }
    for (uint32_t i = 0; i < fr->len; i++) {
        if (!odin3_fsm_sig_is_const(sig[i]) && ce->stop[sig[i]] != 0) {
            return fail_undef(ce, sig[i]);
        }
    }
    if (fr->node != 0) {
        if (ce->busy[fr->node] != 0) {
            return fail_undef(ce, sig[0]);
        }
        ce->busy[fr->node] = 1;
        fr->has_busy = 1;
    }
    uint32_t n_drivers = 0;
    *st = collect_drivers(ce, sig, fr->len, &n_drivers);
    fr = top_frame(ce);
    fr->drv_n = n_drivers;
    fr->phase = 1;
    return *st == ODIN3_OK ? STEP_CONT : STEP_FAIL;
}

/* The end of eval(sig): the bits the drivers set, or the first one nothing drives. */
static step_result sig_end(odin3_fsm_ceval *ce) {
    frame *fr = top_frame(ce);
    odin3_fsm_sig *sig = frame_sig(ce, fr);
    if (fr->has_busy) {
        ce->busy[fr->node] = 0;
        fr->has_busy = 0;
    }
    if (apply(ce, sig, fr->len)) {
        return STEP_OK;
    }
    return fail_undef(ce, first_wire_bit(sig, fr->len));
}

static step_result push_cell(odin3_fsm_ceval *ce, odin3_status *st) {
    frame *fr = top_frame(ce);
    const uint32_t *drv = ce->drivers.data;
    uint32_t node = drv[fr->drv_mark + fr->drv_i];
    fr->drv_i++;
    *st = push_frame(ce, (frame_req){FRAME_CELL, node});
    return *st == ODIN3_OK ? STEP_PUSHED : STEP_FAIL;
}

static step_result step_sig(odin3_fsm_ceval *ce, odin3_status *st) {
    const frame *fr = top_frame(ce);
    switch (fr->phase) {
    case 0:
        return sig_begin(ce, st);
    case 1:
        if (fr->drv_i < fr->drv_n) {
            return push_cell(ce, st);
        }
        top_frame(ce)->phase = 2;
        return STEP_CONT;
    default:
        return sig_end(ce);
    }
}

/* Pushes eval(sig) on the pool array [off, off + count) (or ext) with `busy` as the busy cell. */
static step_result push_sig(odin3_fsm_ceval *ce, pool_span span, odin3_status *st) {
    uint32_t busy_node = top_frame(ce)->node;
    *st = push_frame(ce, (frame_req){FRAME_SIG, busy_node});
    if (*st != ODIN3_OK) {
        return STEP_FAIL;
    }
    frame *fr = top_frame(ce);
    fr->off = span.off;
    fr->len = span.len;
    return STEP_PUSHED;
}

/* --- eval(cell) ------------------------------------------------------------------------------ */

typedef struct cell_ports {
    uint32_t a, b, s, y;
} cell_ports;

static cell_ports ports_of(const odin3_fsm_ceval *ce, odin3_node_id node, bool *is_mux) {
    const odin3_celltype_def *def =
        odin3_celltype_get(ce->ix->design, odin3_node_type(ce->ix->module, node));
    cell_ports ports = {odin3_kit_port_index(def, "A"), odin3_kit_port_index(def, "B"),
                        odin3_kit_port_index(def, "S"), odin3_kit_port_index(def, "Y")};
    *is_mux = strcmp(def->name, "$mux") == 0 || strcmp(def->name, "$pmux") == 0 ||
              strcmp(def->name, "$_MUX_") == 0;
    return ports;
}

/* Copies one port into the pool unless the type lacks it. */
static odin3_status load_port(odin3_fsm_ceval *ce, odin3_node_id node, uint32_t port,
                              uint32_t *span) {
    if (port == ODIN3_KIT_NONE) {
        span[0] = (uint32_t)ce->pool.len;
        span[1] = 0;
        return ODIN3_OK;
    }
    return pool_port(ce, node, port, &span[0], &span[1]);
}

/* True when every bit of Y already has a value. */
static bool y_known(odin3_fsm_ceval *ce, pool_span span) {
    bool known = span.len > 0;
    for (uint32_t i = 0; i < span.len; i++) {
        known = known && odin3_fsm_sig_is_const(odin3_fsm_ceval_get(ce, pool_at(ce, span.off)[i]));
    }
    return known;
}

/* Phase 0 of eval(cell): count it, test the cap, copy the ports into the pool. */
static step_result cell_begin(odin3_fsm_ceval *ce, odin3_status *st) {
    if (++ce->budget->used > ce->budget->max) {
        ce->res->capped = true;
        return STEP_FAIL;
    }
    frame *fr = top_frame(ce);
    odin3_node_id node = {fr->node};
    bool is_mux = false;
    cell_ports ports = ports_of(ce, node, &is_mux);
    if (ports.y == ODIN3_KIT_NONE) {
        return STEP_FAIL;
    }
    const uint32_t order[4] = {ports.y, ports.a, ports.b, ports.s};
    uint32_t spans[4][2];
    *st = ODIN3_OK;
    for (uint32_t k = 0; *st == ODIN3_OK && k < 4; k++) {
        *st = load_port(ce, node, order[k], spans[k]);
    }
    fr = top_frame(ce);
    if (*st != ODIN3_OK) {
        return STEP_FAIL;
    }
    fr->is_mux = is_mux;
    fr->y_off = spans[0][0];
    fr->y_n = spans[0][1];
    fr->a_off = spans[1][0];
    fr->a_n = spans[1][1];
    fr->b_off = spans[2][0];
    fr->b_n = spans[2][1];
    fr->s_off = spans[3][0];
    fr->s_n = spans[3][1];
    fr->phase = 1;
    return y_known(ce, (pool_span){fr->y_off, fr->y_n}) ? STEP_OK : STEP_CONT;
}

/* Builds an odin3_cval of the constant bits of sig[0 .. count). */
static odin3_status cval_of(odin3_arena *arena, const odin3_fsm_sig *sig, uint32_t count,
                            odin3_cval *out) {
    odin3_cval_spec spec = {count, 0, ODIN3_BIT_0};
    odin3_status st = odin3_cval_new(arena, &spec, out);
    for (uint32_t i = 0; st == ODIN3_OK && i < count; i++) {
        uint8_t bit = odin3_fsm_sig_bit(sig[i]);
        uint64_t mask = (uint64_t)1 << (i % ODIN3_CVAL_WORD_BITS);
        uint32_t word = i / ODIN3_CVAL_WORD_BITS;
        bool is_one = bit == ODIN3_FSM_BIT_1;
        bool is_x = bit != ODIN3_FSM_BIT_0 && bit != ODIN3_FSM_BIT_1 && bit != ODIN3_FSM_BIT_Z;
        if (is_one || is_x) {
            out->aval[word] |= mask;
        }
        if (is_x || bit == ODIN3_FSM_BIT_Z) {
            out->bval[word] |= mask;
        }
    }
    return st;
}

/* The odin3_fsm_bit of bit k of val. */
static uint8_t bit_of(const odin3_cval *val, uint32_t pos) {
    switch (odin3_cval_bit(val, pos)) {
    case ODIN3_BIT_0:
        return ODIN3_FSM_BIT_0;
    case ODIN3_BIT_1:
        return ODIN3_FSM_BIT_1;
    case ODIN3_BIT_Z:
        return ODIN3_FSM_BIT_Z;
    default:
        return ODIN3_FSM_BIT_X;
    }
}

/* Sets Y from a value of Y's width. */
static odin3_status set_y(odin3_fsm_ceval *ce, const frame *fr, const odin3_cval *val) {
    for (uint32_t i = 0; i < fr->y_n; i++) {
        odin3_fsm_sig net = pool_at(ce, fr->y_off)[i];
        if (odin3_fsm_sig_is_const(net)) {
            continue;
        }
        odin3_status st = set_one(ce, net, bit_of(val, i));
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* A foldable cell, its operands evaluated: the value of Y (CellTypes::eval). */
static step_result cell_compute(odin3_fsm_ceval *ce, odin3_status *st) {
    const frame *fr = top_frame(ce);
    odin3_arena *arena = odin3_arena_create(EVAL_ARENA_CHUNK);
    if (arena == NULL) {
        *st = ODIN3_ERR_NO_MEMORY;
        return STEP_FAIL;
    }
    odin3_cval ops[2];
    const odin3_cval *given[2] = {NULL, NULL};
    *st = ODIN3_OK;
    if (fr->a_n > 0) {
        *st = cval_of(arena, pool_at(ce, fr->a_off), fr->a_n, &ops[0]);
        given[0] = &ops[0];
    }
    if (*st == ODIN3_OK && fr->b_n > 0) {
        *st = cval_of(arena, pool_at(ce, fr->b_off), fr->b_n, &ops[1]);
        given[1] = &ops[1];
    }
    odin3_cval out;
    bool evaluable = false;
    if (*st == ODIN3_OK) {
        odin3_opt_eval_req req = {{fr->node}, given[0], given[1], NULL, arena};
        *st = odin3_opt_eval_cell(&ce->oc, &req, &out, &evaluable);
    }
    if (*st == ODIN3_OK && evaluable) {
        *st = set_y(ce, fr, &out);
    }
    odin3_arena_destroy(arena);
    return *st == ODIN3_OK && evaluable ? STEP_OK : STEP_FAIL;
}

/* The generic cell: A, then B, then the value. */
static step_result cell_generic(odin3_fsm_ceval *ce, odin3_status *st) {
    frame *fr = top_frame(ce);
    switch (fr->phase) {
    case 1:
        fr->phase = 2;
        return fr->a_n > 0 ? push_sig(ce, (pool_span){fr->a_off, fr->a_n}, st) : STEP_CONT;
    case 2:
        fr->phase = 3;
        return fr->b_n > 0 ? push_sig(ce, (pool_span){fr->b_off, fr->b_n}, st) : STEP_CONT;
    default:
        return cell_compute(ce, st);
    }
}

/* True when slice `cand` (0 .. S_WIDTH - 1 is a B slice, S_WIDTH is A) takes part. */
static bool cand_taken(odin3_fsm_ceval *ce, const frame *fr, uint32_t cand) {
    const odin3_fsm_sig *sel = pool_at(ce, fr->s_off);
    if (cand < fr->s_n) {
        uint8_t bit = odin3_fsm_sig_bit(sel[cand]);
        return bit == ODIN3_FSM_BIT_X || bit == ODIN3_FSM_BIT_1;
    }
    for (uint32_t i = 0; i < fr->s_n; i++) {
        if (odin3_fsm_sig_bit(sel[i]) == ODIN3_FSM_BIT_1) {
            return false;
        }
    }
    return true;
}

/* The pool offset of slice cand. */
static uint32_t cand_off(const frame *fr, uint32_t cand) {
    return cand < fr->s_n ? fr->b_off + cand * fr->y_n : fr->a_off;
}

/* Merges the taken slices: equal bits stay, different bits give x (consteval.h:150-171). */
static odin3_status mux_combine(odin3_fsm_ceval *ce, const frame *fr) {
    uint8_t *bits = odin3_util_malloc((size_t)fr->y_n + 1);
    if (bits == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    bool first = true;
    for (uint32_t cand = 0; cand <= fr->s_n; cand++) {
        if (!cand_taken(ce, fr, cand)) {
            continue;
        }
        const odin3_fsm_sig *slice = pool_at(ce, cand_off(fr, cand));
        for (uint32_t i = 0; i < fr->y_n; i++) {
            uint8_t bit = odin3_fsm_sig_bit(slice[i]);
            bits[i] = first || bits[i] == bit ? bit : (uint8_t)ODIN3_FSM_BIT_X;
        }
        first = false;
    }
    odin3_status st = odin3_fsm_ceval_set(ce, pool_at(ce, fr->y_off), bits, fr->y_n);
    odin3_util_free(bits);
    return st;
}

/* The mux: S, then the taken slices in order, then their merge. */
static step_result cell_mux(odin3_fsm_ceval *ce, odin3_status *st) {
    frame *fr = top_frame(ce);
    if (fr->phase == 1) {
        fr->phase = 2;
        return push_sig(ce, (pool_span){fr->s_off, fr->s_n}, st);
    }
    while (fr->cand <= fr->s_n && !cand_taken(ce, fr, fr->cand)) {
        fr->cand++;
    }
    if (fr->cand <= fr->s_n) {
        uint32_t cand = fr->cand++;
        return push_sig(ce, (pool_span){cand_off(fr, cand), fr->y_n}, st);
    }
    *st = mux_combine(ce, fr);
    return *st == ODIN3_OK ? STEP_OK : STEP_FAIL;
}

static step_result step_cell(odin3_fsm_ceval *ce, odin3_status *st) {
    const frame *fr = top_frame(ce);
    if (fr->phase == 0) {
        return cell_begin(ce, st);
    }
    return fr->is_mux ? cell_mux(ce, st) : cell_generic(ce, st);
}

/* --- the driver ------------------------------------------------------------------------------ */

static void finish_frame(odin3_fsm_ceval *ce) {
    const frame *fr = top_frame(ce);
    if (fr->kind == FRAME_SIG && fr->has_busy) {
        ce->busy[fr->node] = 0;
    }
    pop_frame(ce);
}

odin3_status odin3_fsm_ceval_eval(odin3_fsm_ceval *ce, odin3_fsm_sig *sig, uint32_t count,
                                  odin3_fsm_ceval_res *res) {
    memset(res, 0, sizeof *res);
    ce->res = res;
    odin3_status st = push_frame(ce, (frame_req){FRAME_SIG, 0});
    if (st != ODIN3_OK) {
        return st;
    }
    frame *fr = top_frame(ce);
    fr->ext = sig;
    fr->len = count;
    res->ok = true;
    while (ce->frames.len > 0 && res->ok) {
        step_result step =
            top_frame(ce)->kind == FRAME_SIG ? step_sig(ce, &st) : step_cell(ce, &st);
        if (step == STEP_OK) {
            finish_frame(ce);
        } else if (step == STEP_FAIL) {
            res->ok = false;
        }
    }
    unwind(ce);
    return st;
}
