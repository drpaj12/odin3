/*
 * proc_clean.c — proc's clean phase (2E spec §5.1, PR-7): Yosys 0.55's proc_clean
 * (passes/proc/proc_clean.cc) on a working form. proc_clean_case and proc_clean_switch call each
 * other; here one explicit stack holds a frame per active call, visiting the tree in Yosys's order,
 * and the sweeps repeat on Yosys's did_something, which a constant fold and the full_case rule do
 * not set.
 */
#include "passes/proc/proc_internal.h"

#include "ir/proc.h"
#include "odin3/odin3.h"
#include "passes/proc/proc_test.h"
#include "passes/proc/pw.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The sweeps of the last odin3_pw_clean call (proc_test.h). */
static uint32_t g_sweeps;

uint32_t odin3_proc_test_clean_sweeps(void) {
    return g_sweeps;
}

/* --- the working form's vectors -------------------------------------------------------------- */

/* Removes element pos of vec, keeping the order of the others. */
static void vec_erase(odin3_vec *vec, size_t pos) {
    char *data = vec->data;
    memmove(data + pos * vec->elem_size, data + (pos + 1) * vec->elem_size,
            (vec->len - pos - 1) * vec->elem_size);
    odin3_vec_pop(vec);
}

/* Inserts the n elements at src in front of vec's, whose room the caller reserved. */
static void vec_insert_front(odin3_vec *vec, const void *src, size_t n) {
    size_t len = vec->len;
    for (size_t i = 0; i < n; i++) {
        (void)odin3_vec_push(vec);
    }
    char *data = vec->data;
    if (n > 0) {
        memmove(data + n * vec->elem_size, data, len * vec->elem_size);
        memcpy(data, src, n * vec->elem_size);
    }
}

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

/* Removes every action whose lhs is empty; *did when one goes. */
static void drop_empty_actions(odin3_vec *actions, bool *did) {
    for (size_t i = 0; i < actions->len;) {
        if (((const odin3_pw_action *)odin3_vec_cat(actions, i))->lhs.len == 0) {
            vec_erase(actions, i);
            *did = true;
        } else {
            i++;
        }
    }
}

/* proc_clean's sync step (before the sweeps): empty actions, then syncs left without actions. */
static void clean_syncs(odin3_pw *pw) {
    bool unused = false; /* Yosys sets no did_something here */
    for (size_t i = 0; i < pw->syncs.len;) {
        odin3_pw_sync *sync = odin3_vec_at(&pw->syncs, i);
        drop_empty_actions(&sync->actions, &unused);
        if (sync->actions.len == 0) {
            odin3_vec_free(&sync->actions);
            vec_erase(&pw->syncs, i);
        } else {
            i++;
        }
    }
}

/* --- the switch rules (proc_clean_switch) ---------------------------------------------------- */

/*
 * The compares of cs against the constant signal: the first fully defined compare equal to it
 * clears them all (true: the case is the match); any other fully defined compare is removed; the
 * others (x, z, '-', nets) are kept and skipped.
 */
static bool match_compares(const odin3_pw *pw, odin3_pw_case *cs, odin3_pw_sig signal) {
    for (size_t j = 0; j < cs->compares.len;) {
        odin3_pw_sig val = *(const odin3_pw_sig *)odin3_vec_cat(&cs->compares, j);
        if (!odin3_pw_sig_is_fully_def(pw, val)) {
            j++;
        } else if (odin3_pw_sig_equal(pw, val, signal)) {
            odin3_vec_clear(&cs->compares);
            return true;
        } else {
            vec_erase(&cs->compares, j);
        }
    }
    return false;
}

/*
 * The constant rule: on a non-empty, fully defined signal the cases are walked in order until a
 * default case or the match; a case left without compares before the match is removed; after the
 * match every later case is removed, and a match at index 0 clears the signal.
 */
static void fold_constant(odin3_pw *pw, odin3_pw_switch *sw) {
    if (sw->signal.len == 0 || !odin3_pw_sig_is_fully_def(pw, sw->signal)) {
        return;
    }
    bool found = false;
    size_t at = 0;
    while (at < sw->cases.len && !found) {
        odin3_pw_case *cs = case_at(pw, index_at(&sw->cases, at));
        if (cs->compares.len == 0) {
            break;
        }
        found = match_compares(pw, cs, sw->signal);
        if (!found && cs->compares.len == 0) {
            vec_erase(&sw->cases, at);
        } else if (!found) {
            at++;
        }
    }
    while (found && sw->cases.len > at + 1) {
        odin3_vec_pop(&sw->cases);
    }
    if (found && at == 0) {
        sw->signal = (odin3_pw_sig){0, 0};
    }
}

/* The inline's condition: switch sw_id is parent's first switch, has one case, and its signal is
 * empty or that case has no compares. */
static bool can_inline(odin3_pw *pw, const odin3_pw_case *parent, uint32_t sw_id) {
    const odin3_pw_switch *sw = switch_at(pw, sw_id);
    if (parent->switches.len == 0 || index_at(&parent->switches, 0) != sw_id ||
        sw->cases.len != 1) {
        return false;
    }
    return sw->signal.len == 0 || case_at(pw, index_at(&sw->cases, 0))->compares.len == 0;
}

/* Empties case cs, which the tree no longer reaches (Yosys deletes it). */
static void drop_case(odin3_pw_case *cs) {
    odin3_vec_clear(&cs->compares);
    odin3_vec_clear(&cs->actions);
    odin3_vec_clear(&cs->switches);
}

/* Is case case_id empty (Yosys's CaseRule::empty: no actions and no switches)? */
static bool case_empty(odin3_pw *pw, uint32_t case_id) {
    const odin3_pw_case *cs = case_at(pw, case_id);
    return cs->actions.len == 0 && cs->switches.len == 0;
}

/* --- the walk ------------------------------------------------------------------------------- */

/* One active call: proc_clean_case on a case, or proc_clean_switch on a switch. */
typedef struct clean_frame {
    uint32_t index; /* the case or the switch */
    uint32_t next;  /* the case's next switch, or the switch's next case */
    int32_t depth;  /* the call's max_depth */
    bool is_switch;
} clean_frame;

typedef struct clean_walk {
    odin3_pw *pw;
    odin3_vec stack; /* clean_frame */
    bool did;        /* Yosys's did_something */
} clean_walk;

/* The max_depth one level down (negative stays: the whole tree). */
static int32_t below(int32_t depth) {
    return depth < 0 ? depth : depth - 1;
}

static clean_frame *top_frame(clean_walk *walk) {
    return odin3_vec_at(&walk->stack, walk->stack.len - 1);
}

/* Enters proc_clean_case(case_id, depth): its empty actions go, then its frame. */
static odin3_status push_case(clean_walk *walk, uint32_t case_id, int32_t depth) {
    drop_empty_actions(&case_at(walk->pw, case_id)->actions, &walk->did);
    clean_frame *frame = odin3_vec_push(&walk->stack);
    if (frame == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *frame = (clean_frame){case_id, 0, depth, false};
    return ODIN3_OK;
}

/*
 * The inline: the only case's actions are appended to parent's and its switches inserted in front
 * of parent's; switch sw_id keeps its slot without cases until the parent's walk reaches it again.
 * ODIN3_ERR_NO_MEMORY with nothing changed.
 */
static odin3_status inline_switch(clean_walk *walk, odin3_pw_case *parent, uint32_t sw_id) {
    odin3_pw_switch *sw = switch_at(walk->pw, sw_id);
    odin3_pw_case *only = case_at(walk->pw, index_at(&sw->cases, 0));
    if (odin3_vec_reserve(&parent->actions, parent->actions.len + only->actions.len) != ODIN3_OK ||
        odin3_vec_reserve(&parent->switches, parent->switches.len + only->switches.len) !=
            ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < only->actions.len; i++) {
        *(odin3_pw_action *)odin3_vec_push(&parent->actions) =
            *(const odin3_pw_action *)odin3_vec_cat(&only->actions, i);
    }
    vec_insert_front(&parent->switches, only->switches.data, only->switches.len);
    drop_case(only);
    odin3_vec_clear(&sw->cases);
    walk->did = true;
    return ODIN3_OK;
}

/* proc_clean_switch(sw_id, parent, depth) up to its loop over the cases: the constant rule, then
 * the inline, or the switch's frame. */
static odin3_status start_switch(clean_walk *walk, odin3_pw_case *parent, uint32_t sw_id,
                                 int32_t depth) {
    fold_constant(walk->pw, switch_at(walk->pw, sw_id));
    if (can_inline(walk->pw, parent, sw_id)) {
        return inline_switch(walk, parent, sw_id);
    }
    clean_frame *frame = odin3_vec_push(&walk->stack);
    if (frame == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *frame = (clean_frame){sw_id, 0, depth, true};
    return ODIN3_OK;
}

/* A step of proc_clean_case: a switch without cases is removed; the next is cleaned (depth
 * permitting); after the last the frame ends. */
static odin3_status step_case(clean_walk *walk) {
    clean_frame *frame = top_frame(walk);
    odin3_pw_case *cs = case_at(walk->pw, frame->index);
    if (frame->next >= cs->switches.len) {
        odin3_vec_pop(&walk->stack);
        return ODIN3_OK;
    }
    uint32_t sw_id = index_at(&cs->switches, frame->next);
    if (switch_at(walk->pw, sw_id)->cases.len == 0) {
        vec_erase(&cs->switches, frame->next);
        walk->did = true;
        return ODIN3_OK;
    }
    frame->next++;
    if (frame->depth == 0) {
        return ODIN3_OK;
    }
    return start_switch(walk, cs, sw_id, below(frame->depth));
}

/* full_case: every case goes when all are empty (Yosys sets no did_something for it). */
static void drop_full_cases(clean_walk *walk, odin3_pw_switch *sw) {
    for (size_t i = 0; i < sw->cases.len; i++) {
        if (!case_empty(walk->pw, index_at(&sw->cases, i))) {
            return;
        }
    }
    odin3_vec_clear(&sw->cases);
}

/* parallel_case without full_case: every empty case goes. */
static void drop_parallel_cases(clean_walk *walk, odin3_pw_switch *sw) {
    for (size_t i = 0; i < sw->cases.len;) {
        if (case_empty(walk->pw, index_at(&sw->cases, i))) {
            vec_erase(&sw->cases, i);
            walk->did = true;
        } else {
            i++;
        }
    }
}

/* The empty-case rules: full_case, parallel_case, else the trailing empty cases go. */
static void drop_empty_cases(clean_walk *walk, odin3_pw_switch *sw) {
    if ((sw->flags & ODIN3_SWITCH_FULL_CASE) != 0) {
        drop_full_cases(walk, sw);
        return;
    }
    if ((sw->flags & ODIN3_SWITCH_PARALLEL_CASE) != 0) {
        drop_parallel_cases(walk, sw);
        return;
    }
    while (sw->cases.len > 0 && case_empty(walk->pw, index_at(&sw->cases, sw->cases.len - 1))) {
        odin3_vec_pop(&sw->cases);
        walk->did = true;
    }
}

/* A step of proc_clean_switch: the next case is cleaned (depth permitting); after the last, the
 * empty-case rules, and the frame ends. */
static odin3_status step_switch(clean_walk *walk) {
    clean_frame *frame = top_frame(walk);
    odin3_pw_switch *sw = switch_at(walk->pw, frame->index);
    if (frame->depth != 0 && frame->next < sw->cases.len) {
        uint32_t case_id = index_at(&sw->cases, frame->next++);
        return push_case(walk, case_id, below(frame->depth));
    }
    drop_empty_cases(walk, sw);
    odin3_vec_pop(&walk->stack);
    return ODIN3_OK;
}

/* One sweep: proc_clean_case(case_id, depth) to the end. */
static odin3_status sweep(clean_walk *walk, uint32_t case_id, int32_t depth) {
    odin3_status st = push_case(walk, case_id, depth);
    while (st == ODIN3_OK && walk->stack.len > 0) {
        st = top_frame(walk)->is_switch ? step_switch(walk) : step_case(walk);
    }
    odin3_vec_clear(&walk->stack);
    return st;
}

odin3_status odin3_pw_clean(odin3_pw *pw, uint32_t case_index, int32_t max_depth) {
    if (pw == NULL || case_index >= pw->cases.len) {
        odin3_log(ODIN3_LOG_ERROR, "proc: internal: clean of a case the process does not have");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (case_index == 0 && max_depth < 0) {
        clean_syncs(pw);
    }
    clean_walk walk = {pw, {NULL, 0, 0, 0}, true};
    odin3_vec_init(&walk.stack, sizeof(clean_frame));
    odin3_status st = ODIN3_OK;
    g_sweeps = 0;
    while (st == ODIN3_OK && walk.did) {
        walk.did = false;
        g_sweeps++;
        st = sweep(&walk, case_index, max_depth);
    }
    odin3_vec_free(&walk.stack);
    return st;
}
