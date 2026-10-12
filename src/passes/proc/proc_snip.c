/*
 * proc_snip.c — proc_mux's snippets (2E spec §6, PR-12): Yosys 0.55's SigSnippets (the bits the
 * case tree of a process assigns, partitioned into runs where one action's lhs overlaps another's
 * partly) and SnippetSwCache (per switch, the snippets an action below it assigns), plus, per case
 * and snippet, the rhs slice of the case's last action assigning that snippet. Yosys inserts an lhs
 * recursively; here a split re-inserts its pieces directly (their bits belong to no other snippet,
 * so each piece is a new snippet), and both tree walks use an explicit stack.
 *
 * Every snippet is a contiguous sub-run of some action's lhs run in pw->bits, so it is returned as
 * an odin3_pw_sig without appending to the working form. After the partition is final, every
 * action lhs is a concatenation of whole snippets, each in its own bit order (a split cuts a
 * snippet into contiguous pieces, and an insert matches runs bit for bit), so an action that
 * assigns any bit of a snippet assigns all of it, and the case's last such action decides the
 * snippet's value in that case.
 */
#include "passes/proc/proc_internal.h"

#include "odin3/odin3.h"
#include "passes/proc/pw.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SNIP_SHIFT = 32 };

/* A snippet while the partition is built; dead once a split replaced it. */
typedef struct sn_run {
    uint32_t first, len;
    bool live;
} sn_run;

struct odin3_proc_snips {
    odin3_vec runs;         /* sn_run while building; then odin3_pw_sig, the final snippets */
    odin3_u64map *bit2snip; /* net -> snippet << 32 | offset in it */
    odin3_u64map *swcache;  /* switch << 32 | snippet -> 1: an action below the switch assigns it */
    odin3_u64map *assigns;  /* case << 32 | snippet -> first bit of the last action's rhs slice */
    uint32_t count;
};

/* One frame of a pre-order walk of the case tree: a case and its next switch, or a switch and its
 * next case. */
typedef struct sn_frame {
    uint32_t index;
    uint32_t pos;
    bool is_switch;
} sn_frame;

/* A walk in progress: the working form, the snippets being built and the open frames. */
typedef struct sn_walk {
    const odin3_pw *pw;
    odin3_proc_snips *snips;
    odin3_vec stack; /* sn_frame */
} sn_walk;

/* Called for every case reached, in pre-order (its actions before its switches' cases). */
typedef odin3_status (*sn_visit)(sn_walk *walk, uint32_t case_index);

static uint64_t pair_key(uint32_t hi, uint32_t lo) {
    return (uint64_t)hi << SNIP_SHIFT | lo;
}

static const odin3_pw_case *case_cat(const odin3_pw *pw, uint32_t index) {
    return odin3_vec_cat(&pw->cases, index);
}

static const odin3_pw_switch *switch_cat(const odin3_pw *pw, uint32_t index) {
    return odin3_vec_cat(&pw->switches, index);
}

static uint32_t index_cat(const odin3_vec *vec, size_t pos) {
    return *(const uint32_t *)odin3_vec_cat(vec, pos);
}

static sn_frame *sn_top(sn_walk *walk) {
    return odin3_vec_at(&walk->stack, walk->stack.len - 1);
}

static odin3_status sn_push(sn_walk *walk, uint32_t index, bool is_switch) {
    sn_frame *frame = odin3_vec_push(&walk->stack);
    if (frame == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *frame = (sn_frame){index, 0, is_switch};
    return ODIN3_OK;
}

/* The pre-order walk of the tree from the root case (SigSnippets::insert(CaseRule *)'s order). */
static odin3_status sn_walk_tree(sn_walk *walk, sn_visit visit) {
    odin3_vec_clear(&walk->stack);
    odin3_status st = sn_push(walk, 0, false);
    st = st == ODIN3_OK ? visit(walk, 0) : st;
    while (st == ODIN3_OK && walk->stack.len > 0) {
        sn_frame *top = sn_top(walk);
        const odin3_vec *children = top->is_switch ? &switch_cat(walk->pw, top->index)->cases
                                                   : &case_cat(walk->pw, top->index)->switches;
        if (top->pos >= children->len) {
            odin3_vec_pop(&walk->stack);
            continue;
        }
        uint32_t child = index_cat(children, top->pos++);
        bool child_is_case = top->is_switch;
        st = sn_push(walk, child, !child_is_case);
        st = st == ODIN3_OK && child_is_case ? visit(walk, child) : st;
    }
    return st;
}

/* --- the partition (SigSnippets::insert) ---------------------------------------------------- */

/* Records run [first, first + len) of pw->bits as a new live snippet and maps its bits to it. */
static odin3_status sn_add(sn_walk *walk, uint32_t first, uint32_t len) {
    odin3_proc_snips *snips = walk->snips;
    if (len == 0) {
        return ODIN3_OK;
    }
    uint32_t index = (uint32_t)snips->runs.len;
    sn_run *run = odin3_vec_push(&snips->runs);
    if (run == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *run = (sn_run){first, len, true};
    const odin3_pw_bit *bits = odin3_pw_sig_bits(walk->pw, (odin3_pw_sig){first, len});
    for (uint32_t j = 0; j < len; j++) {
        if (odin3_u64map_put(snips->bit2snip, (odin3_kv){bits[j], pair_key(index, j)}) !=
            ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    return ODIN3_OK;
}

/* A run [at, at + len) of a snippet. */
typedef struct sn_cut {
    uint32_t at, len;
} sn_cut;

/* Splits snippet other into its runs before, at and after cut, each a new snippet. */
static odin3_status sn_split(sn_walk *walk, uint32_t other, sn_cut cut) {
    sn_run *run = odin3_vec_at(&walk->snips->runs, other);
    sn_run old = *run;
    run->live = false;
    odin3_status st = sn_add(walk, old.first, cut.at);
    st = st == ODIN3_OK ? sn_add(walk, old.first + cut.at, cut.len) : st;
    return st == ODIN3_OK ? sn_add(walk, old.first + cut.at + cut.len, old.len - cut.at - cut.len)
                          : st;
}

/* True when lhs is already a snippet, bit for bit (Yosys: snippets.count(sigidx(sig))). */
static bool sn_known(const sn_walk *walk, odin3_pw_sig lhs) {
    const odin3_pw_bit *bits = odin3_pw_sig_bits(walk->pw, lhs);
    uint64_t found = 0;
    if (!odin3_u64map_get(walk->snips->bit2snip, bits[0], &found)) {
        return false;
    }
    const sn_run *run = odin3_vec_cat(&walk->snips->runs, (size_t)(found >> SNIP_SHIFT));
    return run->len == lhs.len &&
           odin3_pw_sig_equal(walk->pw, lhs, (odin3_pw_sig){run->first, run->len});
}

/* Where an lhs bit sits: its position in the lhs, and its snippet << 32 | offset there. */
typedef struct sn_at {
    uint32_t pos;
    uint64_t found;
} sn_at;

/*
 * The run of lhs from position at.pos that matches snippet `other` bit for bit from its offset
 * (SigSnippets::insert's n): at least 1, as that bit is the snippet's bit at the offset.
 */
static uint32_t sn_match_len(const sn_walk *walk, odin3_pw_sig lhs, sn_at at) {
    const sn_run *run = odin3_vec_cat(&walk->snips->runs, (size_t)(at.found >> SNIP_SHIFT));
    uint32_t off = (uint32_t)at.found;
    const odin3_pw_bit *bits = odin3_pw_sig_bits(walk->pw, lhs);
    const odin3_pw_bit *other = odin3_pw_sig_bits(walk->pw, (odin3_pw_sig){run->first, run->len});
    uint32_t len = 1;
    while (at.pos + len < lhs.len && off + len < run->len &&
           bits[at.pos + len] == other[off + len]) {
        len++;
    }
    return len;
}

/* SigSnippets::insert(sig) for one action lhs. A match of a whole snippet changes nothing (Yosys
 * erases and re-inserts it), so the snippet keeps its place in creation order. */
static odin3_status sn_insert(sn_walk *walk, odin3_pw_sig lhs) {
    if (lhs.len == 0 || sn_known(walk, lhs)) {
        return ODIN3_OK;
    }
    uint32_t start = 0; /* [start, i): bits of no snippet yet (Yosys's new_sig) */
    uint32_t i = 0;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && i < lhs.len) {
        uint64_t found = 0;
        if (!odin3_u64map_get(walk->snips->bit2snip, odin3_pw_sig_bits(walk->pw, lhs)[i], &found)) {
            i++;
            continue;
        }
        st = sn_add(walk, lhs.first + start, i - start);
        uint32_t matched = sn_match_len(walk, lhs, (sn_at){i, found});
        uint32_t other = (uint32_t)(found >> SNIP_SHIFT);
        uint32_t off = (uint32_t)found;
        const sn_run *run = odin3_vec_cat(&walk->snips->runs, other);
        if (st == ODIN3_OK && (off != 0 || matched != run->len)) {
            st = sn_split(walk, other, (sn_cut){off, matched});
        }
        i += matched;
        start = i;
    }
    return st == ODIN3_OK ? sn_add(walk, lhs.first + start, lhs.len - start) : st;
}

static odin3_status sn_visit_insert(sn_walk *walk, uint32_t case_index) {
    const odin3_pw_case *cs = case_cat(walk->pw, case_index);
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < cs->actions.len; i++) {
        st = sn_insert(walk, ((const odin3_pw_action *)odin3_vec_cat(&cs->actions, i))->lhs);
    }
    return st;
}

/* Keeps the live snippets in creation order as odin3_pw_sig and maps their bits to the final
 * indices. */
static odin3_status sn_finish(sn_walk *walk) {
    odin3_proc_snips *snips = walk->snips;
    uint32_t kept = 0;
    for (size_t i = 0; i < snips->runs.len; i++) {
        sn_run run = *(const sn_run *)odin3_vec_cat(&snips->runs, i);
        if (run.live) {
            *(sn_run *)odin3_vec_at(&snips->runs, kept++) = run;
        }
    }
    odin3_vec runs;
    odin3_vec_init(&runs, sizeof(odin3_pw_sig));
    if (odin3_vec_reserve(&runs, kept) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t k = 0; k < kept; k++) {
        sn_run run = *(const sn_run *)odin3_vec_cat(&snips->runs, k);
        *(odin3_pw_sig *)odin3_vec_push(&runs) = (odin3_pw_sig){run.first, run.len}; /* reserved */
        const odin3_pw_bit *bits = odin3_pw_sig_bits(walk->pw, (odin3_pw_sig){run.first, run.len});
        for (uint32_t j = 0; st == ODIN3_OK && j < run.len; j++) {
            st = odin3_u64map_put(snips->bit2snip, (odin3_kv){bits[j], pair_key(k, j)});
        }
    }
    odin3_vec_free(&snips->runs);
    snips->runs = runs;
    snips->count = kept;
    return st;
}

/* --- the switch cache and the case assignments ----------------------------------------------- */

/* Marks snippet snip as assigned below every switch on the walk's stack, innermost first, up to
 * the first already marked (its outer switches were marked with it). */
static odin3_status sn_mark(sn_walk *walk, uint32_t snip) {
    for (size_t i = walk->stack.len; i > 0; i--) {
        const sn_frame *frame = odin3_vec_cat(&walk->stack, i - 1);
        if (!frame->is_switch) {
            continue;
        }
        uint64_t key = pair_key(frame->index, snip);
        if (odin3_u64map_get(walk->snips->swcache, key, NULL)) {
            return ODIN3_OK;
        }
        if (odin3_u64map_put(walk->snips->swcache, (odin3_kv){key, 1}) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    return ODIN3_OK;
}

/* One action of case case_index: each snippet of its lhs is assigned by it in this case (a later
 * action overwrites) and marked in the switch cache. */
static odin3_status sn_action(sn_walk *walk, uint32_t case_index, const odin3_pw_action *act) {
    odin3_status st = ODIN3_OK;
    for (uint32_t j = 0; st == ODIN3_OK && j < act->lhs.len;) {
        uint64_t found = 0;
        odin3_pw_bit bit = odin3_pw_sig_bits(walk->pw, act->lhs)[j];
        if (!odin3_u64map_get(walk->snips->bit2snip, bit, &found) || (uint32_t)found != 0) {
            odin3_log(ODIN3_LOG_ERROR, "proc: internal: an action does not cover whole snippets");
            return ODIN3_ERR_CHECK;
        }
        uint32_t snip = (uint32_t)(found >> SNIP_SHIFT);
        st = odin3_u64map_put(walk->snips->assigns,
                              (odin3_kv){pair_key(case_index, snip), (uint64_t)act->rhs.first + j});
        st = st == ODIN3_OK ? sn_mark(walk, snip) : st;
        j += odin3_proc_snip(walk->snips, snip).len;
    }
    return st;
}

static odin3_status sn_visit_assigns(sn_walk *walk, uint32_t case_index) {
    const odin3_pw_case *cs = case_cat(walk->pw, case_index);
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < cs->actions.len; i++) {
        st = sn_action(walk, case_index, odin3_vec_cat(&cs->actions, i));
    }
    return st;
}

/* --- the API ------------------------------------------------------------------------------- */

void odin3_proc_snips_free(odin3_proc_snips *snips) {
    if (snips == NULL) {
        return;
    }
    odin3_vec_free(&snips->runs);
    odin3_u64map_destroy(snips->bit2snip);
    odin3_u64map_destroy(snips->swcache);
    odin3_u64map_destroy(snips->assigns);
    odin3_util_free(snips);
}

static odin3_proc_snips *sn_create(void) {
    odin3_proc_snips *snips = odin3_util_calloc(sizeof *snips);
    if (snips == NULL) {
        return NULL;
    }
    odin3_vec_init(&snips->runs, sizeof(sn_run));
    snips->bit2snip = odin3_u64map_create(0);
    snips->swcache = odin3_u64map_create(0);
    snips->assigns = odin3_u64map_create(0);
    if (snips->bit2snip == NULL || snips->swcache == NULL || snips->assigns == NULL) {
        odin3_proc_snips_free(snips);
        return NULL;
    }
    return snips;
}

odin3_status odin3_proc_snips_build(const odin3_proc_mctx *mctx, const odin3_pw *pw,
                                    odin3_proc_snips **out) {
    if (mctx == NULL || pw == NULL || out == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "proc: snips_build: no module context, working form or result");
        return ODIN3_ERR_INVALID_ARG;
    }
    *out = NULL;
    sn_walk walk = {pw, sn_create(), {0}};
    odin3_vec_init(&walk.stack, sizeof(sn_frame));
    odin3_status st = walk.snips != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    if (st == ODIN3_OK && pw->cases.len > 0) {
        st = sn_walk_tree(&walk, sn_visit_insert);
        st = st == ODIN3_OK ? sn_finish(&walk) : st;
        st = st == ODIN3_OK ? sn_walk_tree(&walk, sn_visit_assigns) : st;
    } else if (st == ODIN3_OK) {
        st = sn_finish(&walk);
    }
    odin3_vec_free(&walk.stack);
    if (st != ODIN3_OK) {
        odin3_proc_snips_free(walk.snips);
        return st;
    }
    *out = walk.snips;
    return ODIN3_OK;
}

uint32_t odin3_proc_snips_count(const odin3_proc_snips *snips) {
    return snips != NULL ? snips->count : 0;
}

odin3_pw_sig odin3_proc_snip(const odin3_proc_snips *snips, uint32_t index) {
    return *(const odin3_pw_sig *)odin3_vec_cat(&snips->runs, index);
}

bool odin3_proc_snips_touches(const odin3_proc_snips *snips, uint32_t sw, uint32_t snip) {
    return odin3_u64map_get(snips->swcache, pair_key(sw, snip), NULL);
}

bool odin3_proc_snips_assigned(const odin3_proc_snips *snips, uint32_t case_index, uint32_t snip,
                               odin3_pw_sig *rhs) {
    uint64_t first = 0;
    if (!odin3_u64map_get(snips->assigns, pair_key(case_index, snip), &first)) {
        return false;
    }
    *rhs = (odin3_pw_sig){(uint32_t)first, odin3_proc_snip(snips, snip).len};
    return true;
}
