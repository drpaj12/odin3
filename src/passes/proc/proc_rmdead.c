/*
 * proc_rmdead.c — proc's rmdead phase (2E spec §5.2, PR-8): Yosys 0.55's proc_rmdead
 * (passes/proc/proc_rmdead.cc) on a working form. proc_rmdead recurses into the switches of each
 * case between its compares and its take_all; here one explicit stack holds a frame per active
 * switch, with that switch's pool (ppool.h). Removed compares and cases are compacted out in one
 * pass per list, and the pattern pools live on the stack together stay within the cap (§16).
 */
#include "passes/proc/proc_internal.h"

#include "ir/proc.h"
#include "odin3/odin3.h"
#include "passes/proc/ppool.h"
#include "passes/proc/proc.h"
#include "passes/proc/proc_test.h"
#include "passes/proc/pw.h"
#include "util/log.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * One switch being processed: proc_rmdead_impl's loop state and its pool. The switch's cases
 * [0, kept) are the kept ones already done, pos is the case being processed, [pos + 1, len) are to
 * come, and [kept, pos) are dead or moved down (compacted out when the switch ends).
 */
typedef struct rd_frame {
    uint32_t sw;     /* the switch */
    uint32_t kept;   /* its cases kept so far */
    uint32_t pos;    /* its case being processed */
    uint32_t child;  /* the next switch of that case to process */
    bool in_case;    /* the case's compares are done: its switches come next */
    bool is_default; /* that case is the default */
    bool fd;         /* the pool is fp (FullyDefinedPool), else pp (BitPatternPool) */
    bool warned;     /* W5a was given for this switch */
    odin3_ppool pp;  /* both are freed with the frame; only one is used */
    odin3_fdpool fp;
} rd_frame;

typedef struct rd_walk {
    odin3_proc_mctx *mctx;
    odin3_pw *pw;
    odin3_vec stack;    /* rd_frame */
    uint64_t live_bits; /* bits_used of every pattern pool on the stack */
} rd_walk;

static odin3_pw_case *case_at(odin3_pw *pw, uint32_t index) {
    return odin3_vec_at(&pw->cases, index);
}

static odin3_pw_switch *switch_at(odin3_pw *pw, uint32_t index) {
    return odin3_vec_at(&pw->switches, index);
}

/* Element pos of a vector of uint32_t indices. */
static uint32_t index_at(const odin3_vec *vec, size_t pos) {
    return *(const uint32_t *)odin3_vec_cat(vec, pos);
}

/* Moves element from to slot to (to <= from), the compaction step of a one-pass removal. */
static void vec_move(odin3_vec *vec, size_t to, size_t from) {
    if (to != from) {
        char *data = vec->data;
        memcpy(data + to * vec->elem_size, data + from * vec->elem_size, vec->elem_size);
    }
}

/* A one-pass removal in progress: [0, kept) stays, [kept, next) is removed, [next, len) is left. */
typedef struct rd_gap {
    size_t kept, next;
} rd_gap;

/* Ends a removal: [next, len) moves down to kept and the rest is dropped. */
static void vec_close_gap(odin3_vec *vec, rd_gap gap) {
    size_t len = vec->len;
    size_t to = gap.kept;
    for (size_t i = gap.next; i < len; i++) {
        vec_move(vec, to++, i);
    }
    while (vec->len > to) {
        odin3_vec_pop(vec);
    }
}

static rd_frame *top_frame(rd_walk *walk) {
    return odin3_vec_at(&walk->stack, walk->stack.len - 1);
}

/* --- the pool of a switch -------------------------------------------------------------------- */

/* Every compare of case cs is fully defined (0 and 1 only). */
static bool compares_fully_def(const odin3_pw *pw, const odin3_pw_case *cs) {
    for (size_t j = 0; j < cs->compares.len; j++) {
        if (!odin3_pw_sig_is_fully_def(pw,
                                       *(const odin3_pw_sig *)odin3_vec_cat(&cs->compares, j))) {
            return false;
        }
    }
    return true;
}

/* can_use_fully_defined_pool: a non-empty signal of nets only, and every compare fully defined. */
static bool can_use_fd(odin3_pw *pw, const odin3_pw_switch *sw) {
    if (sw->signal.len == 0) {
        return false;
    }
    const odin3_pw_bit *bits = odin3_pw_sig_bits(pw, sw->signal);
    for (uint32_t i = 0; i < sw->signal.len; i++) {
        if ((bits[i] & ODIN3_PW_CONST) != 0) {
            return false;
        }
    }
    for (size_t i = 0; i < sw->cases.len; i++) {
        if (!compares_fully_def(pw, case_at(pw, index_at(&sw->cases, i)))) {
            return false;
        }
    }
    return true;
}

static bool pool_empty(const rd_frame *frame) {
    return frame->fd ? odin3_fdpool_empty(&frame->fp) : odin3_ppool_empty(&frame->pp);
}

/*
 * The cap after the frame's pattern pool changed from `before` bits: the walk's total is updated,
 * and past the cap the pool is stopped (nested switches' pools count together, §16). A pool past
 * the cap gives W5a once for its switch (at the switch's record, else the process's) and loses
 * parallel_case: its remaining items may overlap, and a priority chain refines the x a $pmux gives
 * for two hot selects (§20 #7).
 */
static void check_cap(rd_walk *walk, rd_frame *frame, uint64_t before) {
    if (frame->fd) {
        return;
    }
    walk->live_bits = walk->live_bits - before + frame->pp.bits_used;
    uint32_t cap = odin3_proc_limits_now().max_pattern_bits;
    if (!frame->pp.over_cap && walk->live_bits > cap) {
        walk->live_bits -= frame->pp.bits_used;
        odin3_ppool_stop(&frame->pp);
    }
    if (!frame->pp.over_cap || frame->warned) {
        return;
    }
    frame->warned = true;
    odin3_pw_switch *sw = switch_at(walk->pw, frame->sw);
    sw->flags = (uint16_t)(sw->flags & ~ODIN3_SWITCH_PARALLEL_CASE);
    odin3_prov_id at = sw->prov.v != 0 ? sw->prov : walk->pw->prov;
    odin3_proc_warn(walk->mctx, ODIN3_PROC_W5A, at,
                    "case analysis needs more than %" PRIu32
                    " pattern bits; no more dead cases are removed from this case statement",
                    cap);
}

static odin3_status pool_take(rd_walk *walk, rd_frame *frame, odin3_pw_sig value, bool *taken) {
    uint64_t before = frame->pp.bits_used;
    odin3_status st = frame->fd ? odin3_fdpool_take(&frame->fp, walk->pw, value, taken)
                                : odin3_ppool_take(&frame->pp, walk->pw, value, taken);
    if (st == ODIN3_OK) {
        check_cap(walk, frame, before);
    }
    return st;
}

static void pool_take_all(rd_walk *walk, rd_frame *frame) {
    if (frame->fd) {
        odin3_fdpool_take_all(&frame->fp);
    } else {
        walk->live_bits -= frame->pp.bits_used;
        odin3_ppool_take_all(&frame->pp);
        walk->live_bits += frame->pp.bits_used;
    }
}

/* Frees the top frame's pools and pops it. */
static void pop_frame(rd_walk *walk) {
    rd_frame *frame = top_frame(walk);
    walk->live_bits -= frame->fd ? 0 : frame->pp.bits_used;
    odin3_ppool_free(&frame->pp);
    odin3_fdpool_free(&frame->fp);
    odin3_vec_pop(&walk->stack);
}

/* --- the walk ------------------------------------------------------------------------------- */

/* proc_rmdead(sw_id): a frame for switch sw_id with its pool. */
static odin3_status push_switch(rd_walk *walk, uint32_t sw_id) {
    rd_frame *frame = odin3_vec_push(&walk->stack); /* zeroed: both pools free as empty */
    if (frame == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    const odin3_pw_switch *sw = switch_at(walk->pw, sw_id);
    frame->sw = sw_id;
    frame->fd = can_use_fd(walk->pw, sw);
    odin3_status st = frame->fd ? odin3_fdpool_init(&frame->fp, sw->signal.len)
                                : odin3_ppool_init_sig(&frame->pp, walk->pw, sw->signal);
    if (st == ODIN3_OK) {
        check_cap(walk, frame, 0);
    }
    return st;
}

/* The case's fully constant compares taken in order; one not taken is removed (one pass). After a
 * failure the compares not yet reached are kept. */
static odin3_status take_compares(rd_walk *walk, rd_frame *frame, odin3_pw_case *cs) {
    odin3_status st = ODIN3_OK;
    size_t kept = 0;
    size_t j = 0;
    while (st == ODIN3_OK && j < cs->compares.len) {
        odin3_pw_sig value = *(const odin3_pw_sig *)odin3_vec_cat(&cs->compares, j);
        bool taken = true;
        if (odin3_pw_sig_is_fully_const(walk->pw, value)) {
            st = pool_take(walk, frame, value, &taken);
        }
        if (st == ODIN3_OK) {
            if (taken) {
                vec_move(&cs->compares, kept++, j);
            }
            j++;
        }
    }
    vec_close_gap(&cs->compares, (rd_gap){kept, j});
    return st;
}

/*
 * The start of the frame's current case: whether it is a default (no compares, and the pool not
 * empty or no signal), its compares taken, and a non-default case left without compares dropped
 * as dead (it is not moved down to the kept cases).
 */
static odin3_status start_case(rd_walk *walk, rd_frame *frame) {
    odin3_pw_switch *sw = switch_at(walk->pw, frame->sw);
    odin3_pw_case *cs = case_at(walk->pw, index_at(&sw->cases, frame->pos));
    frame->is_default = cs->compares.len == 0 && (!pool_empty(frame) || sw->signal.len == 0);
    odin3_status st = take_compares(walk, frame, cs);
    if (st != ODIN3_OK) {
        return st;
    }
    if (!frame->is_default && cs->compares.len == 0) {
        frame->pos++; /* dead */
        return ODIN3_OK;
    }
    frame->in_case = true;
    frame->child = 0;
    return ODIN3_OK;
}

/* The end of the frame's switch: the dead cases compacted out, full_case when its pool is empty,
 * the frame popped. */
static void finish_switch(rd_walk *walk) {
    const rd_frame *frame = top_frame(walk);
    odin3_pw_switch *sw = switch_at(walk->pw, frame->sw);
    vec_close_gap(&sw->cases, (rd_gap){frame->kept, frame->pos});
    if (pool_empty(frame)) {
        sw->flags |= ODIN3_SWITCH_FULL_CASE;
    }
    pop_frame(walk);
}

/* A step of the top switch: its next case's start, then that case's switches one by one, then
 * the default's take_all and the case moved down to the kept ones. */
static odin3_status step(rd_walk *walk) {
    rd_frame *frame = top_frame(walk);
    odin3_pw_switch *sw = switch_at(walk->pw, frame->sw);
    if (!frame->in_case) {
        if (frame->pos >= sw->cases.len) {
            finish_switch(walk);
            return ODIN3_OK;
        }
        return start_case(walk, frame);
    }
    const odin3_pw_case *cs = case_at(walk->pw, index_at(&sw->cases, frame->pos));
    if (frame->child < cs->switches.len) {
        return push_switch(walk, index_at(&cs->switches, frame->child++));
    }
    if (frame->is_default) {
        pool_take_all(walk, frame);
    }
    vec_move(&sw->cases, frame->kept++, frame->pos++);
    frame->in_case = false;
    return ODIN3_OK;
}

/* After a failure: every switch on the stack keeps its kept cases and the ones not yet done. */
static void unwind(rd_walk *walk) {
    while (walk->stack.len > 0) {
        const rd_frame *frame = top_frame(walk);
        vec_close_gap(&switch_at(walk->pw, frame->sw)->cases, (rd_gap){frame->kept, frame->pos});
        pop_frame(walk);
    }
}

odin3_status odin3_pw_rmdead(odin3_proc_mctx *mctx, odin3_pw *pw) {
    if (mctx == NULL || pw == NULL || pw->cases.len == 0) {
        odin3_log(ODIN3_LOG_ERROR, "proc: internal: rmdead without a process");
        return ODIN3_ERR_INVALID_ARG;
    }
    rd_walk walk = {mctx, pw, {NULL, 0, 0, 0}, 0};
    odin3_vec_init(&walk.stack, sizeof(rd_frame));
    const odin3_pw_case *root = case_at(pw, 0);
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < root->switches.len; i++) {
        st = push_switch(&walk, index_at(&root->switches, i));
        while (st == ODIN3_OK && walk.stack.len > 0) {
            st = step(&walk);
        }
    }
    unwind(&walk);
    odin3_vec_free(&walk.stack);
    return st;
}
