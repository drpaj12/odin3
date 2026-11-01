/*
 * proc_mux.c — proc's mux phase (2E spec §6, PR-12; §7, PR-13): Yosys 0.55's proc_mux without -ifx
 * for one process whose $proc node step B has deleted. One tree per snippet (proc_snip.c), in
 * creation order, from the root case with default x; signal_to_mux_tree's recursion becomes an
 * explicit stack of case and switch frames. All signals are raw (§4): the decoded bits are
 * compared as stored, and only the cells read them through the net map (odin3_proc_add_cell).
 *
 * Per switch, memoised: its groups (Yosys recomputes them on every visit with a fresh pool; they
 * depend on the switch only), its provenance record, and per snippet its full_case mask. A mux is
 * a record (its A, its B slices and S bits, its Y on nets made when it opens) while a later case of
 * its switch may still append to it, and becomes one node when the next mux of the switch opens
 * (before that mux's select), when an append closes it, or when the switch's walk ends; so an
 * append never resizes a node. Each snippet's value is recorded with odin3_proc_connect (applied
 * at M1, after the promotions step B recorded first), and the runs a snippet's tree appended to
 * pw->bits are dropped once it is done.
 */
#include "passes/proc/proc_internal.h"

#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/proc/ppool.h"
#include "passes/proc/proc.h"
#include "passes/proc/proc_test.h"
#include "passes/proc/pw.h"
#include "util/log.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    MX_SHIFT = 32,
    MX_FULL_NO = 1,  /* the full_case memo: the switch does not mask the snippet */
    MX_FULL_YES = 2, /* it masks it */
    EQ_PARAMS = 5,   /* $eq: A_SIGNED B_SIGNED A_WIDTH B_WIDTH Y_WIDTH */
    OR_PARAMS = 3,   /* $reduce_or: A_SIGNED A_WIDTH Y_WIDTH */
    PMUX_PARAMS = 2, /* $pmux: WIDTH S_WIDTH ($mux: WIDTH) */
    MUX_INPUTS = 3   /* A B S */
};

/* What the walk keeps about a switch once it has visited it. */
typedef struct mx_switch {
    bool grouped;       /* the groups of its cases are set */
    bool has_prov;      /* prov is set */
    odin3_prov_id prov; /* the record of its muxes and selects (§14) */
} mx_switch;

/* A case (its running result and next switch) or a switch (its cases left, in reverse, and its
 * open mux). */
typedef struct mx_frame {
    uint32_t index;
    uint32_t pos;              /* case: next switch position; switch: cases left to walk */
    odin3_pw_sig result;       /* the running result */
    odin3_pw_sig initial;      /* switch: its cases' default (the masked result) */
    odin3_pw_sig mux_a, mux_y; /* switch: the open mux's A and its Y nets */
    uint32_t base;             /* switch: the open mux's first entry */
    bool is_switch, open;      /* open: a mux of this switch is open */
} mx_frame;

/* One B slice of an open mux with its S bit. */
typedef struct mx_entry {
    odin3_pw_sig value;
    odin3_pw_bit select;
} mx_entry;

typedef struct mx_walk {
    odin3_proc_mctx *mctx;
    odin3_pw *pw;
    odin3_proc_snips *snips;
    uint32_t snip;      /* the snippet whose tree is walked */
    odin3_pw_sig xval;  /* x, the snippet's width */
    odin3_vec stack;    /* mx_frame */
    odin3_vec entries;  /* mx_entry of the open muxes, the innermost last */
    odin3_vec switches; /* mx_switch per pw->switches */
    odin3_vec groups;   /* uint32_t: the group of each case, per pw->cases */
    odin3_vec buf;      /* odin3_pw_bit: a run being assembled */
    odin3_vec selects;  /* odin3_pw_bit: the compare bits of one select */
    odin3_vec parents;  /* odin3_prov_id: a switch's record's parents */
    odin3_vec fstack;   /* uint32_t triples: the full_case walk */
    odin3_u64map *full; /* switch << 32 | snippet -> MX_FULL_NO / MX_FULL_YES */
    odin3_u64map *sels; /* case index + 1 -> the select bit its first snippet made
                           (--share-selects), else how many snippets made the select again */
    bool hinted;        /* the --share-selects hint was logged */
} mx_walk;

static uint64_t pair_key(uint32_t hi, uint32_t lo) {
    return (uint64_t)hi << MX_SHIFT | lo;
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

static odin3_pw_sig compare_cat(const odin3_pw_case *cs, size_t pos) {
    return *(const odin3_pw_sig *)odin3_vec_cat(&cs->compares, pos);
}

static mx_frame *mx_top(mx_walk *walk) {
    return odin3_vec_at(&walk->stack, walk->stack.len - 1);
}

static mx_switch *mx_state(mx_walk *walk, uint32_t sw) {
    return odin3_vec_at(&walk->switches, sw);
}

static uint32_t mx_group(const mx_walk *walk, uint32_t case_index) {
    return index_cat(&walk->groups, case_index);
}

/* Empties the walk's buf with room for count bits (buf_put fills it, buf_run appends it to
 * pw->bits). */
static odin3_status buf_reset(mx_walk *walk, size_t count) {
    odin3_vec_clear(&walk->buf);
    return odin3_vec_reserve(&walk->buf, count + 1) == ODIN3_OK ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

static void buf_put(mx_walk *walk, odin3_pw_bit bit) {
    *(odin3_pw_bit *)odin3_vec_push(&walk->buf) = bit; /* reserved by buf_reset */
}

static odin3_status buf_run(mx_walk *walk, odin3_pw_sig *out) {
    return odin3_pw_sig_new(walk->pw, walk->buf.data, (uint32_t)walk->buf.len, out);
}

/* --- provenance and groups ------------------------------------------------------------------ */

/* The record of switch sw's muxes and selects: [the switch's, then each case's in case order]
 * (§14), derived once. */
static odin3_status mx_prov(mx_walk *walk, uint32_t sw, odin3_prov_id *out) {
    mx_switch *state = mx_state(walk, sw);
    if (!state->has_prov) {
        const odin3_pw_switch *rule = switch_cat(walk->pw, sw);
        odin3_vec_clear(&walk->parents);
        if (odin3_vec_reserve(&walk->parents, rule->cases.len + 1) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *(odin3_prov_id *)odin3_vec_push(&walk->parents) = rule->prov; /* reserved */
        for (size_t i = 0; i < rule->cases.len; i++) {
            *(odin3_prov_id *)odin3_vec_push(&walk->parents) =
                case_cat(walk->pw, index_cat(&rule->cases, i))->prov;
        }
        odin3_status st = odin3_proc_prov(walk->mctx, walk->pw, walk->parents.data,
                                          (uint32_t)walk->parents.len, &state->prov);
        if (st != ODIN3_OK) {
            return st;
        }
        state->has_prov = true;
    }
    *out = state->prov;
    return ODIN3_OK;
}

/* Every compare of the switch is fully defined and none repeats (Yosys's simple parallel case). */
static odin3_status mx_simple_parallel(const mx_walk *walk, const odin3_pw_switch *rule,
                                       bool *simple) {
    odin3_fdpool pool;
    odin3_status st = odin3_fdpool_init(&pool, rule->signal.len);
    *simple = true;
    for (size_t i = 0; st == ODIN3_OK && *simple && i < rule->cases.len; i++) {
        const odin3_pw_case *cs = case_cat(walk->pw, index_cat(&rule->cases, i));
        for (size_t j = 0; st == ODIN3_OK && *simple && j < cs->compares.len; j++) {
            odin3_pw_sig pat = compare_cat(cs, j);
            bool taken = false;
            *simple = pat.len == rule->signal.len && odin3_pw_sig_is_fully_def(walk->pw, pat);
            st = *simple ? odin3_fdpool_take(&pool, walk->pw, pat, &taken) : st;
            *simple = *simple && taken;
        }
    }
    odin3_fdpool_free(&pool);
    return st;
}

/* W5b, once per switch: a grouping pool went past the cap (§15, §16). */
static void mx_warn_cap(mx_walk *walk, const odin3_pw_switch *rule) {
    odin3_prov_id at = rule->prov.v != 0 ? rule->prov : walk->pw->prov;
    odin3_proc_warn(walk->mctx, ODIN3_PROC_W5B, at,
                    "case analysis needs more than %" PRIu32
                    " pattern bits; the remaining items of this case statement are not grouped "
                    "into a $pmux",
                    odin3_proc_limits_now().max_pattern_bits);
}

/* Case pos of the switch starts a new group (Yosys: a non-constant compare before it, one of its
 * own compares not fully constant or not all in the pool, or no compare at all). */
static bool mx_new_group(const mx_walk *walk, const odin3_ppool *pool, const odin3_pw_case *cs,
                         bool after_nonconst) {
    if (after_nonconst || cs->compares.len == 0) {
        return true;
    }
    for (size_t j = 0; j < cs->compares.len; j++) {
        odin3_pw_sig pat = compare_cat(cs, j);
        if (!odin3_pw_sig_is_fully_const(walk->pw, pat) ||
            !odin3_ppool_has_all(pool, walk->pw, pat)) {
            return true;
        }
    }
    return false;
}

/* Takes case cs's constant compares into the pool; *nonconst when one is not constant. */
static odin3_status mx_take(const mx_walk *walk, odin3_ppool *pool, const odin3_pw_case *cs,
                            bool *nonconst) {
    odin3_status st = ODIN3_OK;
    for (size_t j = 0; st == ODIN3_OK && j < cs->compares.len; j++) {
        odin3_pw_sig pat = compare_cat(cs, j);
        bool taken = false;
        if (!odin3_pw_sig_is_fully_const(walk->pw, pat)) {
            *nonconst = true;
        } else {
            st = odin3_ppool_take(pool, walk->pw, pat, &taken);
        }
    }
    return st;
}

/* The groups of a switch that is neither parallel_case nor simple parallel, as Yosys forms them
 * in case order with a fresh BitPatternPool(width) per group; past the cap the group ends (W5b). */
static odin3_status mx_pool_groups(mx_walk *walk, const odin3_pw_switch *rule) {
    odin3_ppool pool;
    odin3_status st = odin3_ppool_init(&pool, rule->signal.len);
    bool nonconst = false;
    bool over = false;
    uint32_t group = 0;
    for (size_t i = 0; st == ODIN3_OK && i < rule->cases.len; i++) {
        uint32_t case_index = index_cat(&rule->cases, i);
        const odin3_pw_case *cs = case_cat(walk->pw, case_index);
        if (i != 0 && mx_new_group(walk, &pool, cs, nonconst)) {
            group++;
            odin3_ppool_free(&pool);
            st = odin3_ppool_init(&pool, rule->signal.len);
        }
        nonconst = false;
        *(uint32_t *)odin3_vec_at(&walk->groups, case_index) = group;
        st = st == ODIN3_OK ? mx_take(walk, &pool, cs, &nonconst) : st;
        over = over || pool.over_cap;
    }
    odin3_ppool_free(&pool);
    if (st == ODIN3_OK && over) {
        mx_warn_cap(walk, rule);
    }
    return st;
}

/* The groups of switch sw's cases, on its first visit: one group for a parallel_case or simple
 * parallel switch, else as mx_pool_groups. */
static odin3_status mx_groups(mx_walk *walk, uint32_t sw) {
    if (mx_state(walk, sw)->grouped) {
        return ODIN3_OK;
    }
    const odin3_pw_switch *rule = switch_cat(walk->pw, sw);
    bool simple = (rule->flags & ODIN3_SWITCH_PARALLEL_CASE) != 0;
    odin3_status st = simple ? ODIN3_OK : mx_simple_parallel(walk, rule, &simple);
    for (size_t i = 0; st == ODIN3_OK && simple && i < rule->cases.len; i++) {
        *(uint32_t *)odin3_vec_at(&walk->groups, index_cat(&rule->cases, i)) = 0;
    }
    st = st == ODIN3_OK && !simple ? mx_pool_groups(walk, rule) : st;
    if (st == ODIN3_OK) {
        mx_state(walk, sw)->grouped = true;
    }
    return st;
}

/* --- the full_case mask (Yosys get_full_case_bits, per snippet) ------------------------------ */

/* One frame of the full_case walk: a switch, its case being checked, and that case's next nested
 * switch. */
typedef struct fm_frame {
    uint32_t sw, case_pos, nested_pos;
} fm_frame;

static bool fm_full_case(const mx_walk *walk, uint32_t sw) {
    return (switch_cat(walk->pw, sw)->flags & ODIN3_SWITCH_FULL_CASE) != 0;
}

/* The memo of (sw, snippet): 0 unknown, else MX_FULL_NO or MX_FULL_YES. */
static uint64_t fm_memo(const mx_walk *walk, uint32_t sw) {
    uint64_t value = 0;
    (void)odin3_u64map_get(walk->full, pair_key(sw, walk->snip), &value);
    return value;
}

/* Settles the top frame's switch (yes or no) and pops it. */
static odin3_status fm_settle(mx_walk *walk, uint32_t sw, bool yes) {
    odin3_vec_pop(&walk->fstack);
    return odin3_u64map_put(walk->full,
                            (odin3_kv){pair_key(sw, walk->snip), yes ? MX_FULL_YES : MX_FULL_NO});
}

/* Whether case case_index assigns the snippet through a nested full_case switch: 1 yes, 0 no, -1
 * when the nested switch at *pos must be settled first. */
static int fm_nested(const mx_walk *walk, uint32_t case_index, uint32_t *pos) {
    const odin3_pw_case *cs = case_cat(walk->pw, case_index);
    for (; *pos < cs->switches.len; (*pos)++) {
        uint32_t sw = index_cat(&cs->switches, *pos);
        if (!fm_full_case(walk, sw) || !odin3_proc_snips_touches(walk->snips, sw, walk->snip)) {
            continue;
        }
        uint64_t memo = fm_memo(walk, sw);
        if (memo == 0) {
            return -1;
        }
        if (memo == MX_FULL_YES) {
            return 1;
        }
    }
    return 0;
}

/* One step of the full_case walk on its top frame. */
static odin3_status fm_step(mx_walk *walk) {
    fm_frame frame = *(const fm_frame *)odin3_vec_cat(&walk->fstack, walk->fstack.len - 1);
    const odin3_pw_switch *rule = switch_cat(walk->pw, frame.sw);
    while (frame.case_pos < rule->cases.len) {
        uint32_t case_index = index_cat(&rule->cases, frame.case_pos);
        odin3_pw_sig rhs = {0, 0};
        int nested = odin3_proc_snips_assigned(walk->snips, case_index, walk->snip, &rhs)
                         ? 1
                         : fm_nested(walk, case_index, &frame.nested_pos);
        if (nested == 0) {
            return fm_settle(walk, frame.sw, false);
        }
        if (nested < 0) {
            uint32_t child = index_cat(&case_cat(walk->pw, case_index)->switches, frame.nested_pos);
            *(fm_frame *)odin3_vec_at(&walk->fstack, walk->fstack.len - 1) = frame;
            fm_frame *pushed = odin3_vec_push(&walk->fstack);
            if (pushed == NULL) {
                return ODIN3_ERR_NO_MEMORY;
            }
            *pushed = (fm_frame){child, 0, 0};
            return ODIN3_OK;
        }
        frame.case_pos++;
        frame.nested_pos = 0;
    }
    return fm_settle(walk, frame.sw, rule->cases.len > 0);
}

/* True when switch sw masks the snippet: it is full_case and every case assigns the snippet,
 * directly or through a nested full_case switch that masks it (§6; memoised per switch). */
static odin3_status mx_full(mx_walk *walk, uint32_t sw, bool *full) {
    *full = false;
    if (!fm_full_case(walk, sw)) {
        return ODIN3_OK;
    }
    odin3_status st = ODIN3_OK;
    if (fm_memo(walk, sw) == 0) {
        odin3_vec_clear(&walk->fstack);
        fm_frame *frame = odin3_vec_push(&walk->fstack);
        st = frame != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        if (frame != NULL) {
            *frame = (fm_frame){sw, 0, 0};
        }
        while (st == ODIN3_OK && walk->fstack.len > 0) {
            st = fm_step(walk);
        }
    }
    *full = st == ODIN3_OK && fm_memo(walk, sw) == MX_FULL_YES;
    return st;
}

/* --- selects (gen_cmp) ---------------------------------------------------------------------- */

/* Some compare of cs has no bit but '-': its select is empty and the case is always taken. */
static bool mx_select_empty(const mx_walk *walk, const odin3_pw_case *cs) {
    for (size_t j = 0; j < cs->compares.len; j++) {
        odin3_pw_sig pat = compare_cat(cs, j);
        const odin3_pw_bit *bits = odin3_pw_sig_bits(walk->pw, pat);
        bool all_dash = true;
        for (uint32_t k = 0; all_dash && k < pat.len; k++) {
            all_dash = bits[k] == ODIN3_PW_SA;
        }
        if (all_dash) {
            return true;
        }
    }
    return false;
}

/* Appends to buf the bits of sig at the positions where pat is not '-'. */
static void mx_keep(mx_walk *walk, odin3_pw_sig sig, odin3_pw_sig pat) {
    const odin3_pw_bit *bits = odin3_pw_sig_bits(walk->pw, sig);
    const odin3_pw_bit *mask = odin3_pw_sig_bits(walk->pw, pat);
    for (uint32_t k = 0; k < pat.len; k++) {
        if (mask[k] != ODIN3_PW_SA) {
            buf_put(walk, bits[k]);
        }
    }
}

/* A cell of switch sw (its record, its flags) on fresh nets; *ybit gets its one output bit. */
static odin3_status mx_cell1(mx_walk *walk, uint32_t sw, odin3_proc_cell cell, odin3_pw_bit *ybit) {
    odin3_status st = mx_prov(walk, sw, &cell.prov);
    cell.pw = walk->pw;
    cell.switch_flags = switch_cat(walk->pw, sw)->flags;
    odin3_pw_sig out = {0, 0};
    st = st == ODIN3_OK ? odin3_proc_add_cell(walk->mctx, &cell, &out) : st;
    if (st == ODIN3_OK) {
        *ybit = odin3_pw_sig_bits(walk->pw, out)[0];
    }
    return st;
}

/* The bit of one compare (pat, with its '-' bits kept, count kept): the signal bit itself for a
 * 1-bit compare against 1, else a $eq of the kept signal bits and compare bits. */
static odin3_status mx_compare(mx_walk *walk, uint32_t sw, odin3_pw_sig pat, uint32_t kept,
                               odin3_pw_bit *bit) {
    odin3_pw_sig signal = switch_cat(walk->pw, sw)->signal;
    odin3_pw_sig ports[2] = {{0, 0}, {0, 0}};
    odin3_status st = buf_reset(walk, kept);
    if (st == ODIN3_OK) {
        mx_keep(walk, pat, pat);
        if (kept == 1 && *(const odin3_pw_bit *)walk->buf.data == ODIN3_PW_S1) {
            odin3_vec_clear(&walk->buf);
            mx_keep(walk, signal, pat);
            *bit = *(const odin3_pw_bit *)walk->buf.data;
            return ODIN3_OK;
        }
        st = buf_run(walk, &ports[1]);
    }
    st = st == ODIN3_OK ? buf_reset(walk, kept) : st;
    if (st == ODIN3_OK) {
        mx_keep(walk, signal, pat);
        st = buf_run(walk, &ports[0]);
    }
    odin3_value params[EQ_PARAMS] = {odin3_value_int(0), odin3_value_int(0), odin3_value_int(kept),
                                     odin3_value_int(kept), odin3_value_int(1)};
    odin3_proc_cell cell = {
        odin3_proc_type(walk->mctx, "$eq"), params, EQ_PARAMS, NULL, ports, 2, NULL, NULL, 0, {0}};
    return st == ODIN3_OK ? mx_cell1(walk, sw, cell, bit) : st;
}

/* The number of bits of pat that are not '-'. */
static uint32_t mx_kept(const mx_walk *walk, odin3_pw_sig pat) {
    const odin3_pw_bit *bits = odin3_pw_sig_bits(walk->pw, pat);
    uint32_t kept = 0;
    for (uint32_t k = 0; k < pat.len; k++) {
        kept += bits[k] != ODIN3_PW_SA ? 1U : 0U;
    }
    return kept;
}

/* The $reduce_or of the select's compare bits (§6: several compares). */
static odin3_status mx_any(mx_walk *walk, uint32_t sw, odin3_pw_bit *sel) {
    uint32_t count = (uint32_t)walk->selects.len;
    odin3_pw_sig in = {0, 0};
    odin3_status st = odin3_pw_sig_new(walk->pw, walk->selects.data, count, &in);
    odin3_value params[OR_PARAMS] = {odin3_value_int(0), odin3_value_int(count),
                                     odin3_value_int(1)};
    odin3_proc_cell cell = {odin3_proc_type(walk->mctx, "$reduce_or"),
                            params,
                            OR_PARAMS,
                            NULL,
                            &in,
                            1,
                            NULL,
                            NULL,
                            0,
                            {0}};
    return st == ODIN3_OK ? mx_cell1(walk, sw, cell, sel) : st;
}

/*
 * Yosys gen_cmp for case case_index of the switch of frame: each compare in order gives a bit
 * (mx_compare) until one with no bit but '-', which makes the select empty (*empty; the cells made
 * for the earlier compares stay, dead). One bit is the select, several are ORed (mx_any); no
 * compare at all (a default appended to an open mux) gives 0, where Yosys builds a $reduce_or of
 * zero bits, which the IR does not have.
 */
static odin3_status mx_select_make(mx_walk *walk, const mx_frame *frame, uint32_t case_index,
                                   odin3_pw_bit *sel, bool *empty) {
    uint32_t sw = frame->index;
    const odin3_pw_case *cs = case_cat(walk->pw, case_index);
    *empty = false;
    odin3_vec_clear(&walk->selects);
    if (odin3_vec_reserve(&walk->selects, cs->compares.len + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t j = 0; j < cs->compares.len; j++) {
        odin3_pw_sig pat = compare_cat(cs, j);
        if (pat.len != switch_cat(walk->pw, sw)->signal.len) {
            odin3_log(ODIN3_LOG_ERROR, "proc: internal: a compare is not its switch's width");
            return ODIN3_ERR_CHECK;
        }
        uint32_t kept = mx_kept(walk, pat);
        odin3_pw_bit bit = 0;
        if (kept == 0) {
            *empty = true;
            return ODIN3_OK;
        }
        odin3_status st = mx_compare(walk, sw, pat, kept, &bit);
        if (st != ODIN3_OK) {
            return st;
        }
        *(odin3_pw_bit *)odin3_vec_push(&walk->selects) = bit; /* reserved */
    }
    if (walk->selects.len <= 1) {
        *sel = walk->selects.len == 1 ? *(const odin3_pw_bit *)walk->selects.data : ODIN3_PW_S0;
        return ODIN3_OK;
    }
    return mx_any(walk, sw, sel);
}

/* Snippets that make the same case's select again before proc suggests --share-selects. */
enum { MX_SHARE_HINT = 64 };

/* Counts one more snippet making the select of case_index again; logs the hint (once per run)
 * when a case has MX_SHARE_HINT of them. */
static odin3_status mx_count_unshared(mx_walk *walk, uint32_t case_index) {
    uint64_t seen = 0;
    const uint64_t key = (uint64_t)case_index + 1U;
    (void)odin3_u64map_get(walk->sels, key, &seen);
    seen++;
    if (seen == MX_SHARE_HINT && !walk->hinted) {
        walk->hinted = true;
        odin3_log(ODIN3_LOG_INFO,
                  "proc: %d snippets of one switch case make the same select cells; pass "
                  "--share-selects to share them (the cells then differ from Yosys's before opt)",
                  MX_SHARE_HINT);
    }
    return odin3_u64map_put(walk->sels, (odin3_kv){key, seen});
}

/*
 * mx_select_make, once per case under --share-selects: a case's select depends on its switch's
 * signal and its compares only, so the snippets of one process can share it. Yosys makes the
 * select again for every signal and opt_merge joins the copies; with the option the first copy is
 * the one that stays, so a switch of C cases over S signals makes C selects, not C * S (a memory
 * held as registers: 128 words, 4096 snippets, 82,000 $eq cells against 640). Without the option
 * proc's cells are Yosys's one for one (the proc_rows goldens, proc-diff). A case with an empty
 * select is not kept (its cells are dead anyway).
 */
static odin3_status mx_select(mx_walk *walk, const mx_frame *frame, uint32_t case_index,
                              odin3_pw_bit *sel, bool *empty) {
    uint64_t known = 0;
    if (walk->mctx->opts == NULL || !walk->mctx->opts->share_selects) {
        odin3_status made = mx_select_make(walk, frame, case_index, sel, empty);
        return made == ODIN3_OK && !*empty ? mx_count_unshared(walk, case_index) : made;
    }
    if (odin3_u64map_get(walk->sels, (uint64_t)case_index + 1U, &known)) {
        *sel = (odin3_pw_bit)known;
        *empty = false;
        return ODIN3_OK;
    }
    odin3_status st = mx_select_make(walk, frame, case_index, sel, empty);
    if (st == ODIN3_OK && !*empty) {
        st = odin3_u64map_put(walk->sels, (odin3_kv){(uint64_t)case_index + 1U, *sel});
    }
    return st;
}

/* --- muxes (gen_mux, append_pmux) ------------------------------------------------------------ */

/* Builds the open mux of switch frame f as one node: $mux for one entry, else $pmux with B slices
 * and S bits in append order; its Y is the nets made when it opened. */
static odin3_status mx_emit(mx_walk *walk, const mx_frame *frame) {
    uint32_t count = (uint32_t)(walk->entries.len - frame->base);
    uint32_t width = frame->mux_a.len;
    odin3_pw_sig inputs[MUX_INPUTS] = {frame->mux_a, {0, 0}, {0, 0}};
    odin3_status st = buf_reset(walk, (size_t)count * width);
    for (uint32_t i = 0; st == ODIN3_OK && i < count; i++) {
        const mx_entry *entry = odin3_vec_cat(&walk->entries, frame->base + i);
        const odin3_pw_bit *bits = odin3_pw_sig_bits(walk->pw, entry->value);
        for (uint32_t k = 0; k < width; k++) {
            buf_put(walk, bits[k]);
        }
    }
    st = st == ODIN3_OK ? buf_run(walk, &inputs[1]) : st;
    st = st == ODIN3_OK ? buf_reset(walk, count) : st;
    for (uint32_t i = 0; st == ODIN3_OK && i < count; i++) {
        buf_put(walk, ((const mx_entry *)odin3_vec_cat(&walk->entries, frame->base + i))->select);
    }
    st = st == ODIN3_OK ? buf_run(walk, &inputs[2]) : st;
    odin3_value params[PMUX_PARAMS] = {odin3_value_int(width), odin3_value_int(count)};
    odin3_proc_cell cell = {odin3_proc_type(walk->mctx, count == 1 ? "$mux" : "$pmux"),
                            params,
                            count == 1 ? 1 : PMUX_PARAMS,
                            walk->pw,
                            inputs,
                            MUX_INPUTS,
                            &frame->mux_y,
                            NULL,
                            switch_cat(walk->pw, frame->index)->flags,
                            {0}};
    st = st == ODIN3_OK ? mx_prov(walk, frame->index, &cell.prov) : st;
    odin3_pw_sig out = {0, 0};
    return st == ODIN3_OK ? odin3_proc_add_cell(walk->mctx, &cell, &out) : st;
}

/* Ends the open mux of switch frame f (no more appends): it becomes a node. */
static odin3_status mx_close(mx_walk *walk, mx_frame *frame) {
    if (!frame->open) {
        return ODIN3_OK;
    }
    odin3_status st = mx_emit(walk, frame);
    if (st != ODIN3_OK) {
        return st; /* still open: mx_abandon deletes its Y nets if no node took them */
    }
    while (walk->entries.len > frame->base) {
        odin3_vec_pop(&walk->entries);
    }
    frame->open = false;
    return ODIN3_OK;
}

/* Deletes the nets of buf (made by mx_new_nets, still pinless and wireless: never fails). */
static void mx_drop_nets(mx_walk *walk) {
    for (size_t k = 0; k < walk->buf.len; k++) {
        odin3_net_id net = {*(const odin3_pw_bit *)odin3_vec_cat(&walk->buf, k)};
        (void)odin3_net_delete(walk->mctx->module, net);
    }
    odin3_vec_clear(&walk->buf);
}

/* width new unnamed nets with record prov, as the run *out. ODIN3_ERR_NO_MEMORY with none left. */
static odin3_status mx_new_nets(mx_walk *walk, uint32_t width, odin3_prov_id prov,
                                odin3_pw_sig *out) {
    odin3_status st = buf_reset(walk, width);
    if (st != ODIN3_OK) {
        return st;
    }
    for (uint32_t k = 0; st == ODIN3_OK && k < width; k++) {
        odin3_net_id net = {0};
        st = odin3_net_create(walk->mctx->module, 0, prov, &net);
        if (st == ODIN3_OK) {
            buf_put(walk, net.v);
        }
    }
    st = st == ODIN3_OK ? buf_run(walk, out) : st;
    if (st != ODIN3_OK) {
        mx_drop_nets(walk);
    }
    return st;
}

/* Opens a mux of switch frame f: A = its result, B = value, S = sel, Y on new nets (the switch's
 * record), which become its result. */
static odin3_status mx_open(mx_walk *walk, mx_frame *frame, odin3_pw_sig value, odin3_pw_bit sel) {
    odin3_prov_id prov = {0};
    odin3_status st = mx_prov(walk, frame->index, &prov);
    if (st == ODIN3_OK && odin3_vec_reserve(&walk->entries, walk->entries.len + 1) != ODIN3_OK) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    odin3_pw_sig ynets = {0, 0};
    st = st == ODIN3_OK ? mx_new_nets(walk, value.len, prov, &ynets) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    frame->mux_a = frame->result;
    frame->mux_y = ynets;
    frame->base = (uint32_t)walk->entries.len;
    *(mx_entry *)odin3_vec_push(&walk->entries) = (mx_entry){value, sel}; /* reserved */
    frame->open = true;
    frame->result = ynets;
    return ODIN3_OK;
}

/*
 * Yosys gen_mux for case case_index (value: its tree): value equal to the result makes it the
 * result with no cell. No compare, or an empty select (whose earlier compares' cells stay), also
 * makes value the result with no cell; then the open mux ends too (§20 #18: Yosys keeps it open,
 * and a later append to it loses that case's value). Otherwise the open mux ends and a new one
 * opens on the select.
 */
static odin3_status mx_gen(mx_walk *walk, mx_frame *frame, uint32_t case_index,
                           odin3_pw_sig value) {
    const odin3_pw_case *cs = case_cat(walk->pw, case_index);
    if (cs->compares.len != 0 && odin3_pw_sig_equal(walk->pw, value, frame->result)) {
        frame->result = value;
        return ODIN3_OK;
    }
    bool empty = cs->compares.len == 0 || mx_select_empty(walk, cs);
    odin3_status st = mx_close(walk, frame);
    odin3_pw_bit sel = 0;
    if (st == ODIN3_OK && cs->compares.len != 0) {
        st = mx_select(walk, frame, case_index, &sel, &empty);
    }
    if (st != ODIN3_OK || empty) {
        frame->result = st == ODIN3_OK ? value : frame->result;
        return st;
    }
    return mx_open(walk, frame, value, sel);
}

/*
 * Yosys append_pmux for case case_index: nothing when value equals the open mux's A, else a B
 * slice and its select. An empty select (a compare of only '-' in a parallel_case group, where
 * Yosys 0.55 asserts; §6, §20 #17) is lowered as gen_mux lowers it: value becomes the result with
 * no cell, and the open mux ends, so an earlier case of the group opens a new one on that result.
 */
static odin3_status mx_append(mx_walk *walk, mx_frame *frame, uint32_t case_index,
                              odin3_pw_sig value) {
    if (odin3_pw_sig_equal(walk->pw, value, frame->mux_a)) {
        return ODIN3_OK;
    }
    if (odin3_vec_reserve(&walk->entries, walk->entries.len + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_pw_bit sel = 0;
    bool empty = false;
    odin3_status st = mx_select(walk, frame, case_index, &sel, &empty);
    if (st != ODIN3_OK) {
        return st;
    }
    if (empty) {
        frame->result = value;
        return mx_close(walk, frame);
    }
    *(mx_entry *)odin3_vec_push(&walk->entries) = (mx_entry){value, sel}; /* reserved */
    return ODIN3_OK;
}

/* --- one tree (signal_to_mux_tree) ----------------------------------------------------------- */

/* The value of the snippet in case case_index before its switches: the rhs slice of the case's
 * last action assigning it, else dflt. */
static odin3_pw_sig mx_case_value(const mx_walk *walk, uint32_t case_index, odin3_pw_sig dflt) {
    odin3_pw_sig rhs = {0, 0};
    return odin3_proc_snips_assigned(walk->snips, case_index, walk->snip, &rhs) ? rhs : dflt;
}

static odin3_status mx_push(mx_walk *walk, mx_frame frame) {
    mx_frame *slot = odin3_vec_push(&walk->stack);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = frame;
    return ODIN3_OK;
}

static odin3_status mx_push_case(mx_walk *walk, uint32_t case_index, odin3_pw_sig dflt) {
    mx_frame frame = {case_index, 0,      mx_case_value(walk, case_index, dflt),
                      {0, 0},     {0, 0}, {0, 0},
                      0,          false,  false};
    return mx_push(walk, frame);
}

/* Enters switch sw from a case whose result is in: its groups, its full_case mask, then its cases
 * in reverse order from the masked result. */
static odin3_status mx_push_switch(mx_walk *walk, uint32_t sw, odin3_pw_sig in) {
    bool full = false;
    odin3_status st = mx_groups(walk, sw);
    st = st == ODIN3_OK ? mx_full(walk, sw, &full) : st;
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_pw_sig result = full ? walk->xval : in;
    uint32_t count = (uint32_t)switch_cat(walk->pw, sw)->cases.len;
    mx_frame pushed = {sw, count, result, result, {0, 0}, {0, 0}, 0, true, false};
    return mx_push(walk, pushed);
}

/* Case position pos of the top switch frame gave value: append to its open mux when the case is
 * in the group of the case after it, else gen_mux. */
static odin3_status mx_deliver(mx_walk *walk, odin3_pw_sig value) {
    mx_frame *frame = mx_top(walk);
    const odin3_vec *cases = &switch_cat(walk->pw, frame->index)->cases;
    uint32_t pos = frame->pos - 1;
    uint32_t case_index = index_cat(cases, pos);
    bool same = frame->open && pos + 1 < cases->len &&
                mx_group(walk, case_index) == mx_group(walk, index_cat(cases, pos + 1));
    odin3_status st =
        same ? mx_append(walk, frame, case_index, value) : mx_gen(walk, frame, case_index, value);
    frame->pos = pos;
    return st;
}

/* One step on the top case frame: enter its next switch that touches the snippet, or return its
 * result to the switch above (or as the tree's value). */
static odin3_status mx_case_step(mx_walk *walk, odin3_pw_sig *value) {
    mx_frame *frame = mx_top(walk);
    const odin3_vec *switches = &case_cat(walk->pw, frame->index)->switches;
    while (frame->pos < switches->len) {
        uint32_t sw = index_cat(switches, frame->pos++);
        if (odin3_proc_snips_touches(walk->snips, sw, walk->snip)) {
            return mx_push_switch(walk, sw, frame->result);
        }
    }
    odin3_pw_sig result = frame->result;
    odin3_vec_pop(&walk->stack);
    if (walk->stack.len == 0) {
        *value = result;
        return ODIN3_OK;
    }
    return mx_deliver(walk, result);
}

/* One step on the top switch frame: walk its next case (in reverse), or end its open mux and
 * return its result to the case above. */
static odin3_status mx_switch_step(mx_walk *walk) {
    mx_frame *frame = mx_top(walk);
    if (frame->pos > 0) {
        uint32_t case_index = index_cat(&switch_cat(walk->pw, frame->index)->cases, frame->pos - 1);
        return mx_push_case(walk, case_index, frame->initial);
    }
    odin3_status st = mx_close(walk, frame);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_pw_sig result = frame->result;
    odin3_vec_pop(&walk->stack);
    mx_top(walk)->result = result; /* a switch always sits in a case */
    return ODIN3_OK;
}

/* The snippet's tree from the root case with default x (signal_to_mux_tree). */
static odin3_status mx_tree(mx_walk *walk, odin3_pw_sig *value) {
    odin3_vec_clear(&walk->stack);
    odin3_status st = mx_push_case(walk, 0, walk->xval);
    while (st == ODIN3_OK && walk->stack.len > 0) {
        st = mx_top(walk)->is_switch ? mx_switch_step(walk) : mx_case_step(walk, value);
    }
    return st;
}

/* After a failure: the Y nets of the muxes still open have no node (a mux that got its node
 * before an attribute failed keeps them), so they are deleted. */
static void mx_abandon(mx_walk *walk) {
    odin3_module *module = walk->mctx->module;
    for (size_t i = 0; i < walk->stack.len; i++) {
        const mx_frame *frame = odin3_vec_cat(&walk->stack, i);
        for (uint32_t k = 0; frame->is_switch && frame->open && k < frame->mux_y.len; k++) {
            odin3_net_id net = {odin3_pw_sig_bits(walk->pw, frame->mux_y)[k]};
            if (odin3_net_live(module, net) && odin3_net_pins(module, net).count == 0) {
                (void)odin3_net_delete(module, net); /* pinless and wireless: never fails */
            }
        }
    }
    odin3_vec_clear(&walk->stack);
    odin3_vec_clear(&walk->entries);
}

/* Snippet index: its tree, then the connection snippet <- value, bit by bit (Yosys
 * module->connect). */
static odin3_status mx_snippet(mx_walk *walk, uint32_t index) {
    odin3_pw_sig sig = odin3_proc_snip(walk->snips, index);
    walk->snip = index;
    odin3_status st = buf_reset(walk, sig.len);
    for (uint32_t i = 0; st == ODIN3_OK && i < sig.len; i++) {
        buf_put(walk, ODIN3_PW_SX);
    }
    st = st == ODIN3_OK ? buf_run(walk, &walk->xval) : st;
    odin3_pw_sig value = {0, 0};
    st = st == ODIN3_OK ? mx_tree(walk, &value) : st;
    if (st != ODIN3_OK) {
        mx_abandon(walk);
        return st;
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < sig.len; i++) {
        st = odin3_proc_connect(walk->mctx, odin3_pw_sig_bits(walk->pw, sig)[i],
                                odin3_pw_sig_bits(walk->pw, value)[i]);
    }
    return st;
}

/* --- the phase ----------------------------------------------------------------------------- */

static void mx_free(mx_walk *walk) {
    odin3_proc_snips_free(walk->snips);
    odin3_u64map_destroy(walk->full);
    odin3_u64map_destroy(walk->sels);
    odin3_vec_free(&walk->stack);
    odin3_vec_free(&walk->entries);
    odin3_vec_free(&walk->switches);
    odin3_vec_free(&walk->groups);
    odin3_vec_free(&walk->buf);
    odin3_vec_free(&walk->selects);
    odin3_vec_free(&walk->parents);
    odin3_vec_free(&walk->fstack);
}

/* Grows vec to count zeroed elements. */
static odin3_status mx_zeroed(odin3_vec *vec, size_t count) {
    if (odin3_vec_reserve(vec, count) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    while (vec->len < count) {
        (void)odin3_vec_push(vec); /* reserved; zeroed */
    }
    return ODIN3_OK;
}

static odin3_status mx_init(mx_walk *walk, odin3_proc_mctx *mctx, odin3_pw *pw) {
    *walk = (mx_walk){0};
    walk->mctx = mctx;
    walk->pw = pw;
    odin3_vec_init(&walk->stack, sizeof(mx_frame));
    odin3_vec_init(&walk->entries, sizeof(mx_entry));
    odin3_vec_init(&walk->switches, sizeof(mx_switch));
    odin3_vec_init(&walk->groups, sizeof(uint32_t));
    odin3_vec_init(&walk->buf, sizeof(odin3_pw_bit));
    odin3_vec_init(&walk->selects, sizeof(odin3_pw_bit));
    odin3_vec_init(&walk->parents, sizeof(odin3_prov_id));
    odin3_vec_init(&walk->fstack, sizeof(fm_frame));
    walk->full = odin3_u64map_create(0);
    walk->sels = odin3_u64map_create(0);
    odin3_status st = walk->full != NULL && walk->sels != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    st = st == ODIN3_OK ? mx_zeroed(&walk->switches, pw->switches.len) : st;
    st = st == ODIN3_OK ? mx_zeroed(&walk->groups, pw->cases.len) : st;
    return st == ODIN3_OK ? odin3_proc_snips_build(mctx, pw, &walk->snips) : st;
}

odin3_status odin3_proc_mux(odin3_proc_mctx *mctx, odin3_pw *pw) {
    if (mctx == NULL || pw == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "proc: mux: no module context or working form");
        return ODIN3_ERR_INVALID_ARG;
    }
    mx_walk walk;
    odin3_status st = mx_init(&walk, mctx, pw);
    size_t mark = pw->bits.len;
    uint32_t count = odin3_proc_snips_count(walk.snips);
    for (uint32_t k = 0; st == ODIN3_OK && k < count; k++) {
        st = mx_snippet(&walk, k);
        while (pw->bits.len > mark) { /* the tree's runs: nothing refers to them any more */
            odin3_vec_pop(&pw->bits);
        }
    }
    mx_free(&walk);
    return st;
}
