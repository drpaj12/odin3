/*
 * proc_rmdead.c — proc's rmdead phase (2E spec §5.2, PR-8): Yosys 0.55's proc_rmdead
 * (passes/proc/proc_rmdead.cc) on a working form. proc_rmdead recurses into the switches of each
 * case between its compares and its take_all; here one explicit stack holds a frame per active
 * switch, with that switch's pool (ppool.h).
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

/* One switch being processed: proc_rmdead_impl's loop state and its pool. */
typedef struct rd_frame {
    uint32_t sw;     /* the switch */
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
    odin3_vec stack; /* rd_frame */
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

/* Removes element pos of vec, keeping the order of the others. */
static void vec_erase(odin3_vec *vec, size_t pos) {
    char *data = vec->data;
    memmove(data + pos * vec->elem_size, data + (pos + 1) * vec->elem_size,
            (vec->len - pos - 1) * vec->elem_size);
    odin3_vec_pop(vec);
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

/* W5a, once for the switch, when its pattern pool went past the cap. */
static void check_cap(rd_walk *walk, rd_frame *frame) {
    if (frame->fd || !frame->pp.over_cap || frame->warned) {
        return;
    }
    frame->warned = true;
    const odin3_pw_switch *sw = switch_at(walk->pw, frame->sw);
    odin3_prov_id at = sw->prov.v != 0 ? sw->prov : walk->pw->prov;
    odin3_proc_warn(walk->mctx, ODIN3_PROC_W5A, at,
                    "case analysis needs more than %" PRIu32
                    " pattern bits; no more dead cases are removed from this case statement",
                    odin3_proc_limits_now().max_pattern_bits);
}

static odin3_status pool_take(rd_walk *walk, rd_frame *frame, odin3_pw_sig value, bool *taken) {
    odin3_status st = frame->fd ? odin3_fdpool_take(&frame->fp, walk->pw, value, taken)
                                : odin3_ppool_take(&frame->pp, walk->pw, value, taken);
    if (st == ODIN3_OK) {
        check_cap(walk, frame);
    }
    return st;
}

static void pool_take_all(rd_frame *frame) {
    if (frame->fd) {
        odin3_fdpool_take_all(&frame->fp);
    } else {
        odin3_ppool_take_all(&frame->pp);
    }
}

static void frame_free(rd_frame *frame) {
    odin3_ppool_free(&frame->pp);
    odin3_fdpool_free(&frame->fp);
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
        check_cap(walk, frame);
    }
    return st;
}

/*
 * The start of the frame's current case: whether it is a default (no compares, and the pool not
 * empty or no signal), its fully constant compares taken (one not taken is removed), and a
 * non-default case left without compares removed as dead.
 */
static odin3_status start_case(rd_walk *walk, rd_frame *frame) {
    odin3_pw_switch *sw = switch_at(walk->pw, frame->sw);
    odin3_pw_case *cs = case_at(walk->pw, index_at(&sw->cases, frame->pos));
    frame->is_default = cs->compares.len == 0 && (!pool_empty(frame) || sw->signal.len == 0);
    for (size_t j = 0; j < cs->compares.len;) {
        odin3_pw_sig value = *(const odin3_pw_sig *)odin3_vec_cat(&cs->compares, j);
        bool taken = true;
        if (odin3_pw_sig_is_fully_const(walk->pw, value)) {
            odin3_status st = pool_take(walk, frame, value, &taken);
            if (st != ODIN3_OK) {
                return st;
            }
        }
        if (taken) {
            j++;
        } else {
            vec_erase(&cs->compares, j);
        }
    }
    if (!frame->is_default && cs->compares.len == 0) {
        vec_erase(&sw->cases, frame->pos); /* dead */
        return ODIN3_OK;
    }
    frame->in_case = true;
    frame->child = 0;
    return ODIN3_OK;
}

/* The end of the frame's switch: full_case when its pool is empty; the pool is freed. */
static void finish_switch(rd_walk *walk) {
    rd_frame *frame = top_frame(walk);
    if (pool_empty(frame)) {
        switch_at(walk->pw, frame->sw)->flags |= ODIN3_SWITCH_FULL_CASE;
    }
    frame_free(frame);
    odin3_vec_pop(&walk->stack);
}

/* A step of the top switch: its next case's start, then that case's switches one by one, then
 * the default's take_all. */
static odin3_status step(rd_walk *walk) {
    rd_frame *frame = top_frame(walk);
    const odin3_pw_switch *sw = switch_at(walk->pw, frame->sw);
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
        pool_take_all(frame);
    }
    frame->in_case = false;
    frame->pos++;
    return ODIN3_OK;
}

odin3_status odin3_pw_rmdead(odin3_proc_mctx *mctx, odin3_pw *pw) {
    if (mctx == NULL || pw == NULL || pw->cases.len == 0) {
        odin3_log(ODIN3_LOG_ERROR, "proc: internal: rmdead without a process");
        return ODIN3_ERR_INVALID_ARG;
    }
    rd_walk walk = {mctx, pw, {NULL, 0, 0, 0}};
    odin3_vec_init(&walk.stack, sizeof(rd_frame));
    const odin3_pw_case *root = case_at(pw, 0);
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < root->switches.len; i++) {
        st = push_switch(&walk, index_at(&root->switches, i));
        while (st == ODIN3_OK && walk.stack.len > 0) {
            st = step(&walk);
        }
    }
    while (walk.stack.len > 0) {
        frame_free(top_frame(&walk));
        odin3_vec_pop(&walk.stack);
    }
    odin3_vec_free(&walk.stack);
    return st;
}
