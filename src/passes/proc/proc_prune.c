/*
 * proc_prune.c — proc's prune phase (2E spec §5.3, PR-9): Yosys 0.55's proc_prune
 * (passes/proc/proc_prune.cc) on a working form, in the raw view. Yosys's do_case and do_switch
 * recurse and copy the `assigned` set into every case; here one explicit stack holds a frame per
 * open case and switch, and the per-net state lives in the module's scratch index:
 *
 * - `assigned` is a mark with an undo log, so a copy is a save mark (the log's length) and leaving
 *   a case undoes the log down to it;
 * - `affected` is a mark never cleared during the walk (it is shared by the whole process);
 * - "newly assigned in every case" is a count of cases per net: each finished case of a full
 *   switch with several cases saves the nets it newly assigned (its stretch of the log), and when
 *   the switch ends its saved nets are counted, those counted in every case are assigned, and the
 *   counts are reset through the same saved lists.
 *
 * A full switch with one case keeps its case's marks as they are. So each net is logged at most
 * once per case, a count never exceeds the switch's cases, and the cost is O(body + lhs bits)
 * instead of Yosys's O(depth x bits).
 */
#include "passes/proc/proc_internal.h"

#include "ir/proc.h"
#include "odin3/odin3.h"
#include "passes/proc/pw.h"
#include "util/log.h"
#include "util/vec.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* A net's entry in the scratch index: two marks, then the count of cases. A set ASSIGNED or count
 * implies AFFECTED, so an entry once set stays nonzero until the final reset. */
enum {
    PR_ASSIGNED = 1U,
    PR_AFFECTED = 2U,
    PR_MARKS = 3U,
    PR_COUNT_SHIFT = 2,
    PR_COUNT_ONE = 1U << PR_COUNT_SHIFT
};

/* The cases a count can hold. */
#define PR_COUNT_MAX (UINT32_MAX >> PR_COUNT_SHIFT)

/* The kinds of frame. */
enum { PR_CASE, PR_ROOT, PR_SWITCH };

/* The parts an action splits into: the bits it keeps and the bits it promotes, lhs and rhs. */
enum { PR_KEEP_LHS, PR_KEEP_RHS, PR_PROMO_LHS, PR_PROMO_RHS, PR_N_PARTS };

/*
 * An open case or switch. A case (PR_CASE, or PR_ROOT for the root case): pos is the number of its
 * switches not yet run (they run last to first). A switch: pos is its next case, mark the undo
 * log's length when it was entered, saved where its finished cases' nets start in walk->saved,
 * and full whether it is full_case or has a default case.
 */
typedef struct pr_frame {
    uint32_t index;
    uint32_t pos;
    size_t mark;
    size_t saved;
    uint8_t kind;
    bool full;
} pr_frame;

typedef struct pr_walk {
    odin3_proc_mctx *mctx;
    odin3_pw *pw;
    odin3_vec stack;             /* pr_frame */
    odin3_vec log;               /* uint32_t nets: the undo log of PR_ASSIGNED, in order */
    odin3_vec saved;             /* uint32_t nets: per finished case of the open full switches */
    odin3_vec parts[PR_N_PARTS]; /* odin3_pw_bit: the action being pruned, split */
} pr_walk;

static odin3_pw_case *case_at(odin3_pw *pw, uint32_t index) {
    return odin3_vec_at(&pw->cases, index);
}

static const odin3_pw_switch *switch_at(const odin3_pw *pw, uint32_t index) {
    return odin3_vec_cat(&pw->switches, index);
}

/* Element pos of a vector of uint32_t. */
static uint32_t u32_at(const odin3_vec *vec, size_t pos) {
    return *(const uint32_t *)odin3_vec_cat(vec, pos);
}

/* Appends value to a vector of uint32_t whose room was reserved. */
static void u32_push(odin3_vec *vec, uint32_t value) {
    *(uint32_t *)odin3_vec_push(vec) = value; /* reserved */
}

static uint32_t entry(const pr_walk *walk, uint32_t net) {
    return odin3_proc_scratch_get(walk->mctx, (odin3_net_id){net});
}

static void set_entry(pr_walk *walk, uint32_t net, uint32_t value) {
    odin3_proc_scratch_set(walk->mctx, (odin3_net_id){net}, value);
}

/* Room for more entries in a vector of uint32_t. */
static odin3_status reserve_more(odin3_vec *vec, size_t more) {
    return odin3_vec_reserve(vec, vec->len + more);
}

/* Undoes `assigned` down to the log's length mark. */
static void restore(pr_walk *walk, size_t mark) {
    while (walk->log.len > mark) {
        uint32_t net = u32_at(&walk->log, walk->log.len - 1);
        odin3_vec_pop(&walk->log);
        set_entry(walk, net, entry(walk, net) & ~(uint32_t)PR_ASSIGNED);
    }
}

/* --- actions -------------------------------------------------------------------------------- */

/*
 * Splits act's bits (§5.3), first to last: a constant bit or one already assigned goes; any other
 * is promoted when root and not affected, else kept; either way it becomes assigned and affected.
 */
static odin3_status split_action(pr_walk *walk, const odin3_pw_action *act, bool root) {
    if (act->lhs.len != act->rhs.len) {
        odin3_log(ODIN3_LOG_ERROR, "proc: internal: prune: an action's sides differ in width");
        return ODIN3_ERR_INVALID_ARG;
    }
    for (int k = 0; k < PR_N_PARTS; k++) {
        odin3_vec_clear(&walk->parts[k]);
        if (odin3_vec_reserve(&walk->parts[k], act->lhs.len) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    if (reserve_more(&walk->log, act->lhs.len) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < act->lhs.len; i++) {
        odin3_pw_bit bit = odin3_pw_sig_bits(walk->pw, act->lhs)[i];
        if ((bit & ODIN3_PW_CONST) != 0) {
            continue; /* Yosys: `bit.wire` is null */
        }
        uint32_t state = entry(walk, bit);
        if ((state & PR_ASSIGNED) != 0) {
            continue;
        }
        int to = root && (state & PR_AFFECTED) == 0 ? PR_PROMO_LHS : PR_KEEP_LHS;
        *(odin3_pw_bit *)odin3_vec_push(&walk->parts[to]) = bit; /* reserved */
        *(odin3_pw_bit *)odin3_vec_push(&walk->parts[to + 1]) =
            odin3_pw_sig_bits(walk->pw, act->rhs)[i];
        u32_push(&walk->log, bit);
        set_entry(walk, bit, state | PR_MARKS);
    }
    return ODIN3_OK;
}

/* A new run holding part `which`. */
static odin3_status part_sig(pr_walk *walk, int which, odin3_pw_sig *out) {
    const odin3_vec *part = &walk->parts[which];
    return odin3_pw_sig_new(walk->pw, part->data, (uint32_t)part->len, out);
}

/*
 * One action of a case: split, then its promotion appended to pw->promoted and its kept bits
 * written back (every allocation before any change). *drop when it keeps no bit.
 */
static odin3_status prune_action(pr_walk *walk, odin3_pw_action *act, bool root, bool *drop) {
    odin3_status st = split_action(walk, act, root);
    size_t kept = walk->parts[PR_KEEP_LHS].len;
    bool promotes = walk->parts[PR_PROMO_LHS].len > 0;
    bool narrows = kept > 0 && kept < act->lhs.len;
    odin3_pw_sig promo[2] = {{0, 0}, {0, 0}};
    odin3_pw_sig keep[2] = {{0, 0}, {0, 0}};
    if (st == ODIN3_OK && promotes) {
        st = part_sig(walk, PR_PROMO_LHS, &promo[0]);
        st = st == ODIN3_OK ? part_sig(walk, PR_PROMO_RHS, &promo[1]) : st;
        st = st == ODIN3_OK ? reserve_more(&walk->pw->promoted, 1) : st;
    }
    if (st == ODIN3_OK && narrows) {
        st = part_sig(walk, PR_KEEP_LHS, &keep[0]);
        st = st == ODIN3_OK ? part_sig(walk, PR_KEEP_RHS, &keep[1]) : st;
    }
    if (st != ODIN3_OK) {
        return st;
    }
    if (promotes) {
        odin3_pw_action *slot = odin3_vec_push(&walk->pw->promoted); /* reserved */
        *slot = (odin3_pw_action){promo[0], promo[1], act->prov};
    }
    if (narrows) {
        act->lhs = keep[0];
        act->rhs = keep[1];
    }
    *drop = kept == 0;
    return ODIN3_OK;
}

/* Copies element `from` of vec to slot `to` (nothing when they are the same). */
static void vec_move(odin3_vec *vec, size_t to, size_t from) {
    if (to != from) {
        char *data = vec->data;
        memcpy(data + to * vec->elem_size, data + from * vec->elem_size, vec->elem_size);
    }
}

/*
 * The actions of case case_index, last to first. The kept ones move up to [write, len) as they
 * are done; at the end (or after a failure, the actions not yet done staying in front) they move
 * down behind the ones not done and the rest is dropped, so the order is kept in one pass.
 */
static odin3_status prune_actions(pr_walk *walk, uint32_t case_index, bool root) {
    odin3_vec *actions = &case_at(walk->pw, case_index)->actions;
    size_t len = actions->len;
    size_t write = len;
    size_t next = len; /* [0, next) not yet done */
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && next > 0) {
        bool drop = false;
        st = prune_action(walk, odin3_vec_at(actions, next - 1), root, &drop);
        if (st == ODIN3_OK) {
            next--;
            if (!drop) {
                vec_move(actions, --write, next);
            }
        }
    }
    size_t to = next;
    for (size_t i = write; i < len; i++) {
        vec_move(actions, to++, i);
    }
    while (actions->len > to) {
        odin3_vec_pop(actions);
    }
    return st;
}

/* --- the walk ------------------------------------------------------------------------------- */

static odin3_status push_frame(pr_walk *walk, pr_frame frame) {
    pr_frame *slot = odin3_vec_push(&walk->stack);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = frame;
    return ODIN3_OK;
}

static odin3_status open_case(pr_walk *walk, uint32_t case_index, uint8_t kind) {
    uint32_t n_switches = (uint32_t)case_at(walk->pw, case_index)->switches.len;
    return push_frame(walk, (pr_frame){case_index, n_switches, 0, 0, kind, false});
}

/* do_switch's entry: the save mark, and whether the switch is full_case or has a default. */
static odin3_status open_switch(pr_walk *walk, uint32_t sw_index) {
    const odin3_pw_switch *sw = switch_at(walk->pw, sw_index);
    bool full = (sw->flags & ODIN3_SWITCH_FULL_CASE) != 0;
    for (size_t i = 0; !full && i < sw->cases.len; i++) {
        full = case_at(walk->pw, u32_at(&sw->cases, i))->compares.len == 0;
    }
    assert(sw->cases.len <= PR_COUNT_MAX);
    return push_frame(walk,
                      (pr_frame){sw_index, 0, walk->log.len, walk->saved.len, PR_SWITCH, full});
}

/* A case of the switch frame has finished: back to the frame's save mark, the nets the case newly
 * assigned saved first when they count. A full switch with one case keeps them assigned. */
static odin3_status end_case(pr_walk *walk, const pr_frame *frame, size_t n_cases) {
    if (frame->full && n_cases == 1) {
        return ODIN3_OK;
    }
    if (frame->full) {
        if (reserve_more(&walk->saved, walk->log.len - frame->mark) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
        for (size_t i = frame->mark; i < walk->log.len; i++) {
            u32_push(&walk->saved, u32_at(&walk->log, i));
        }
    }
    restore(walk, frame->mark);
    return ODIN3_OK;
}

/* The switch frame has run all its n_cases cases: the nets every case newly assigned become
 * assigned (full switches with several cases), and its saved nets are dropped. */
static odin3_status end_switch(pr_walk *walk, const pr_frame *frame, size_t n_cases) {
    if (!frame->full || n_cases < 2) {
        return ODIN3_OK;
    }
    size_t end = walk->saved.len;
    if (reserve_more(&walk->log, end - frame->saved) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = frame->saved; i < end; i++) {
        uint32_t net = u32_at(&walk->saved, i);
        set_entry(walk, net, entry(walk, net) + PR_COUNT_ONE);
    }
    for (size_t i = frame->saved; i < end; i++) {
        uint32_t net = u32_at(&walk->saved, i);
        uint32_t state = entry(walk, net);
        if (state >> PR_COUNT_SHIFT == n_cases) {
            u32_push(&walk->log, net);
            state |= PR_ASSIGNED;
        }
        set_entry(walk, net, state & PR_MARKS); /* 0 counts left: later copies of net skip */
    }
    while (walk->saved.len > frame->saved) {
        odin3_vec_pop(&walk->saved);
    }
    return ODIN3_OK;
}

/* One step of the switch on top: the case that just ran closed, then the next case opened, or the
 * switch ended and popped. */
static odin3_status step_switch(pr_walk *walk) {
    pr_frame *top = odin3_vec_at(&walk->stack, walk->stack.len - 1);
    const odin3_pw_switch *sw = switch_at(walk->pw, top->index);
    odin3_status st = top->pos > 0 ? end_case(walk, top, sw->cases.len) : ODIN3_OK;
    if (st != ODIN3_OK) {
        return st;
    }
    if (top->pos < sw->cases.len) {
        uint32_t case_index = u32_at(&sw->cases, top->pos++);
        return open_case(walk, case_index, PR_CASE); /* may move the stack: top not used after */
    }
    pr_frame done = *top;
    odin3_vec_pop(&walk->stack);
    return end_switch(walk, &done, sw->cases.len);
}

/* One step of the walk: a case runs its switches last to first, then its actions; a switch steps
 * through its cases. */
static odin3_status step(pr_walk *walk) {
    pr_frame *top = odin3_vec_at(&walk->stack, walk->stack.len - 1);
    if (top->kind == PR_SWITCH) {
        return step_switch(walk);
    }
    if (top->pos > 0) {
        top->pos--;
        return open_switch(walk, u32_at(&case_at(walk->pw, top->index)->switches, top->pos));
    }
    pr_frame done = *top;
    odin3_vec_pop(&walk->stack);
    return prune_actions(walk, done.index, done.kind == PR_ROOT);
}

odin3_status odin3_pw_prune(odin3_proc_mctx *mctx, odin3_pw *pw) {
    if (mctx == NULL || pw == NULL || pw->cases.len == 0) {
        odin3_log(ODIN3_LOG_ERROR, "proc: internal: prune without a process");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_status st = odin3_proc_scratch_grow(mctx);
    if (st != ODIN3_OK) {
        return st;
    }
    pr_walk walk;
    memset(&walk, 0, sizeof walk);
    walk.mctx = mctx;
    walk.pw = pw;
    odin3_vec_init(&walk.stack, sizeof(pr_frame));
    odin3_vec_init(&walk.log, sizeof(uint32_t));
    odin3_vec_init(&walk.saved, sizeof(uint32_t));
    for (int k = 0; k < PR_N_PARTS; k++) {
        odin3_vec_init(&walk.parts[k], sizeof(odin3_pw_bit));
    }
    st = open_case(&walk, 0, PR_ROOT);
    while (st == ODIN3_OK && walk.stack.len > 0) {
        st = step(&walk);
    }
    odin3_proc_scratch_reset(mctx);
    odin3_vec_free(&walk.stack);
    odin3_vec_free(&walk.log);
    odin3_vec_free(&walk.saved);
    for (int k = 0; k < PR_N_PARTS; k++) {
        odin3_vec_free(&walk.parts[k]);
    }
    return st;
}
