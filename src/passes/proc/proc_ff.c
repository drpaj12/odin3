/*
 * proc_ff.c — the flip-flop phase of proc (2E spec §9, §14, §15; PR-15; plan Task 9): Yosys
 * 0.55's proc_dff for one process after the latch step. The sync signals and right-hand sides are
 * first read through the net map (the live view); the left-hand sides stay as decoded, so the
 * groups and their bit order are Yosys's, which sorts the names a process was written with. Then,
 * while a sync assigns anything, find_any_lvalue gives the next group, one register: the level
 * syncs' rules and the edge sync's value, the collapse of equal rules into one $reduce_or trigger,
 * the self-reset $mux, and the cell ($dffsr, $aldff, $adff or $dff), each step in proc_dff's order.
 * The group's bits then leave every action.
 */
#include "passes/proc/proc_internal.h"

#include "ir/ids.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "passes/proc/proc.h"
#include "passes/proc/pw.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    NOT_PARAMS = 3,   /* $not and $reduce_or: A_SIGNED, A_WIDTH, Y_WIDTH */
    MUX_PARAMS = 1,   /* $mux: WIDTH */
    MUX_INPUTS = 3,   /* A, B, S */
    DFF_PARAMS = 2,   /* $dff: WIDTH, CLK_POLARITY */
    ADFF_PARAMS = 4,  /* $adff: WIDTH, CLK_POLARITY, ARST_POLARITY, ARST_VALUE */
    ALDFF_PARAMS = 3, /* $aldff: WIDTH, CLK_POLARITY, ALOAD_POLARITY */
    DFFSR_PARAMS = 4, /* $dffsr: WIDTH, CLK_POLARITY, SET_POLARITY, CLR_POLARITY */
    REG_INPUTS = 4,   /* the most input ports of a register: $aldff's and $dffsr's */
    W4_NAMES = 3      /* W3 and W4 name the first three resets (§15) */
};

#define NO_SYNC UINT32_MAX

/* One asynchronous rule of a group: the value its level sync gives the group's bits (z where the
 * action does not assign them) and its trigger. */
typedef struct ff_rule {
    odin3_pw_sig value;
    odin3_pw_bit signal;
    uint8_t type; /* ODIN3_SYNC_LEVEL0 or ODIN3_SYNC_LEVEL1 */
} ff_rule;

/* The process and the group being built. */
typedef struct ff_ctx {
    odin3_proc_mctx *mctx;
    odin3_pw *pw;
    odin3_pw_sig sig;   /* the group: its distinct raw bits in odin3_proc_raw_key order */
    odin3_pw_sig live;  /* the group's nets: sig through the net map */
    odin3_pw_sig insig; /* the edge sync's value, z where it does not assign */
    uint32_t edge;      /* the edge sync's index, NO_SYNC before one is met */
    odin3_vec rules;    /* ff_rule, in sync and action order */
    odin3_vec resets;   /* odin3_pw_bit: every rule's signal, before the collapse (W3, W4) */
} ff_ctx;

static odin3_status ff_internal(const char *what) {
    odin3_log(ODIN3_LOG_ERROR, "proc: internal: %s", what);
    return ODIN3_ERR_CHECK;
}

/* *out = a new run of n copies of the constant code. */
static odin3_status ff_fill(odin3_pw *pw, odin3_pw_bit code, uint32_t n, odin3_pw_sig *out) {
    odin3_pw_bit *bits = odin3_util_malloc(sizeof *bits * ((size_t)n + 1));
    if (bits == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < n; i++) {
        bits[i] = code;
    }
    odin3_status st = odin3_pw_sig_new(pw, bits, n, out);
    odin3_util_free(bits);
    return st;
}

/* *out = a new run of the one bit `bit`. */
static odin3_status ff_bit(odin3_pw *pw, odin3_pw_bit bit, odin3_pw_sig *out) {
    return odin3_pw_sig_new(pw, &bit, 1, out);
}

/* --- the live view ------------------------------------------------------------------------- */

/* *out = a new run of sig's bits through the net map (the live view). */
static odin3_status ff_live_run(odin3_proc_mctx *mctx, odin3_pw *pw, odin3_pw_sig sig,
                                odin3_pw_sig *out) {
    odin3_status st = odin3_pw_sig_new(pw, odin3_pw_sig_bits(pw, sig), sig.len, out);
    if (st != ODIN3_OK || out->len == 0) {
        return st; /* an empty run: no bits, and no offset on a NULL pointer */
    }
    odin3_pw_bit *bits = (odin3_pw_bit *)pw->bits.data + out->first;
    for (uint32_t i = 0; i < out->len; i++) {
        bits[i] = odin3_proc_find(mctx, bits[i]);
    }
    return ODIN3_OK;
}

/* ODIN3_ERR_CHECK when a bit of lhs resolves to a constant (a register output merged into a
 * constant net is a 2E bug, as in the latch phase). */
static odin3_status ff_check_lhs(odin3_proc_mctx *mctx, const odin3_pw *pw, odin3_pw_sig lhs) {
    const odin3_pw_bit *bits = odin3_pw_sig_bits(pw, lhs);
    for (uint32_t i = 0; i < lhs.len; i++) {
        if ((odin3_proc_find(mctx, bits[i]) & ODIN3_PW_CONST) != 0) {
            return ff_internal("register output on a constant net");
        }
    }
    return ODIN3_OK;
}

/* Every edge and level sync's signal and right-hand sides through the net map (the live view,
 * §4); the left-hand sides stay as decoded. The latch phase empties every ALWAYS sync first, so
 * one with an action is a 2E bug. */
static odin3_status ff_live(odin3_proc_mctx *mctx, odin3_pw *pw) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < pw->syncs.len; i++) {
        odin3_pw_sync *sync = odin3_vec_at(&pw->syncs, i);
        if (sync->type == ODIN3_SYNC_ALWAYS) {
            st = sync->actions.len == 0 ? ODIN3_OK
                                        : ff_internal("always sync left for the flip-flop phase");
            continue;
        }
        sync->signal = odin3_proc_find(mctx, sync->signal);
        for (size_t j = 0; st == ODIN3_OK && j < sync->actions.len; j++) {
            odin3_pw_action *act = odin3_vec_at(&sync->actions, j);
            st = ff_check_lhs(mctx, pw, act->lhs);
            st = st == ODIN3_OK ? ff_live_run(mctx, pw, act->rhs, &act->rhs) : st;
        }
    }
    return st;
}

/* --- groups (find_any_lvalue) -------------------------------------------------------------- */

/* The first non-empty action lhs of the last sync that has one (Yosys's find_any_lvalue: its
 * `break` leaves only the loop over one sync's actions, so each later sync overwrites it); empty
 * when no sync assigns anything. */
static odin3_pw_sig ff_first_lhs(const odin3_pw *pw) {
    odin3_pw_sig found = {0, 0};
    for (size_t i = 0; i < pw->syncs.len; i++) {
        const odin3_pw_sync *sync = odin3_vec_cat(&pw->syncs, i);
        for (size_t j = 0; j < sync->actions.len; j++) {
            const odin3_pw_action *act = odin3_vec_cat(&sync->actions, j);
            if (act->lhs.len > 0) {
                found = act->lhs;
                break;
            }
        }
    }
    return found;
}

/* *out = every action lhs of sync, appended in order as one run (each odin3_pw_sig_new appends
 * at the end of pw->bits, so consecutive copies are contiguous). */
static odin3_status ff_sync_lhs(odin3_pw *pw, const odin3_pw_sync *sync, odin3_pw_sig *out) {
    odin3_pw_sig all = {(uint32_t)pw->bits.len, 0};
    odin3_status st = ODIN3_OK;
    for (size_t j = 0; st == ODIN3_OK && j < sync->actions.len; j++) {
        odin3_pw_sig lhs = ((const odin3_pw_action *)odin3_vec_cat(&sync->actions, j))->lhs;
        odin3_pw_sig piece = {0, 0};
        st = odin3_pw_sig_new(pw, odin3_pw_sig_bits(pw, lhs), lhs.len, &piece);
        all.len += st == ODIN3_OK ? lhs.len : 0;
    }
    *out = all;
    return st;
}

/* find_any_lvalue: fc->sig = ff_first_lhs, sorted and unified, then for each sync its common
 * part with every bit the sync assigns, where that part is not empty. Empty when no sync assigns
 * anything. */
static odin3_status ff_find_lvalue(ff_ctx *fc) {
    odin3_pw *pw = fc->pw;
    odin3_pw_sig lhs = ff_first_lhs(pw);
    fc->sig = (odin3_pw_sig){0, 0};
    if (lhs.len == 0) {
        return ODIN3_OK;
    }
    odin3_status st = odin3_pw_sig_sort_unify(fc->mctx, pw, lhs, &fc->sig);
    for (size_t i = 0; st == ODIN3_OK && i < pw->syncs.len; i++) {
        odin3_pw_sig all = {0, 0};
        odin3_pw_sig common = {0, 0};
        st = ff_sync_lhs(pw, odin3_vec_cat(&pw->syncs, i), &all);
        st = st == ODIN3_OK ? odin3_pw_sig_sort_unify(fc->mctx, pw, all, &all) : st;
        odin3_pw_pair in = {all, {0, 0}};
        st = st == ODIN3_OK ? odin3_pw_sig_extract(fc->mctx, pw, in, fc->sig, &common) : st;
        if (st == ODIN3_OK && common.len > 0) {
            fc->sig = common;
        }
    }
    return st;
}

/* --- one group's rules and edge value ------------------------------------------------------ */

/* A level sync's action: the rule (the value it gives the group's bits, z elsewhere). */
static odin3_status ff_rule_add(ff_ctx *fc, const odin3_pw_sync *sync, const odin3_pw_action *act) {
    ff_rule rule = {{0, 0}, sync->signal, sync->type};
    odin3_pw_sig undef = {0, 0};
    odin3_status st = ff_fill(fc->pw, ODIN3_PW_SZ, fc->sig.len, &undef);
    odin3_pw_replace req = {fc->sig, act->lhs, act->rhs, undef};
    st = st == ODIN3_OK ? odin3_pw_sig_replace(fc->mctx, fc->pw, &req, &rule.value) : st;
    ff_rule *slot = st == ODIN3_OK ? odin3_vec_push(&fc->rules) : NULL;
    if (st == ODIN3_OK && slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (slot != NULL) {
        *slot = rule;
    }
    return st;
}

/* The edge sync's action (index si): its bits of insig. A second edge sync on the group is E1,
 * which the clock pre-check raises before any change (§5.5), so reaching it is a 2E bug. */
static odin3_status ff_edge_add(ff_ctx *fc, uint32_t si, const odin3_pw_action *act) {
    if (fc->edge != NO_SYNC && fc->edge != si) {
        return ff_internal("register with two clock edges");
    }
    fc->edge = si;
    odin3_pw_replace req = {fc->sig, act->lhs, act->rhs, fc->insig};
    return odin3_pw_sig_replace(fc->mctx, fc->pw, &req, &fc->insig);
}

/* One action of sync si against the group (proc_dff's loop body), skipped when it assigns none of
 * the group's bits; then those bits leave the action (remove2). */
static odin3_status ff_action(ff_ctx *fc, uint32_t si, odin3_pw_action *act) {
    odin3_pw_sig meet = {0, 0};
    odin3_pw_pair lhs_only = {act->lhs, {0, 0}};
    odin3_status st = odin3_pw_sig_extract(fc->mctx, fc->pw, lhs_only, fc->sig, &meet);
    if (st != ODIN3_OK || meet.len == 0) {
        return st;
    }
    const odin3_pw_sync *sync = odin3_vec_cat(&fc->pw->syncs, si);
    if (sync->type == ODIN3_SYNC_LEVEL0 || sync->type == ODIN3_SYNC_LEVEL1) {
        st = ff_rule_add(fc, sync, act);
    } else if (sync->type == ODIN3_SYNC_POSEDGE || sync->type == ODIN3_SYNC_NEGEDGE) {
        st = ff_edge_add(fc, si, act);
    } else {
        st = ff_internal("always sync left for the flip-flop phase");
    }
    odin3_pw_pair rest = {{0, 0}, {0, 0}};
    odin3_pw_pair both = {act->lhs, act->rhs};
    st = st == ODIN3_OK ? odin3_pw_sig_remove2(fc->mctx, fc->pw, both, fc->sig, &rest) : st;
    if (st == ODIN3_OK) {
        act->lhs = rest.a;
        act->rhs = rest.b;
    }
    return st;
}

/* Every action of every sync, in order. */
static odin3_status ff_gather(ff_ctx *fc) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < fc->pw->syncs.len; i++) {
        odin3_pw_sync *sync = odin3_vec_at(&fc->pw->syncs, i);
        for (size_t j = 0; st == ODIN3_OK && j < sync->actions.len; j++) {
            st = ff_action(fc, (uint32_t)i, odin3_vec_at(&sync->actions, j));
        }
    }
    return st;
}

/* --- cells (§9, §14) ----------------------------------------------------------------------- */

/* Adds cell with the process's lowering record; *out its outputs (out may be NULL). */
static odin3_status ff_add(const ff_ctx *fc, odin3_proc_cell cell, odin3_pw_sig *out) {
    odin3_pw_sig ignored = {0, 0};
    cell.pw = fc->pw;
    cell.prov = fc->pw->lowered;
    return odin3_proc_add_cell(fc->mctx, &cell, out != NULL ? out : &ignored);
}

/* $not(in), as Yosys's Module::Not: A_SIGNED 0, A_WIDTH = Y_WIDTH = |in|. */
static odin3_status ff_not(const ff_ctx *fc, odin3_pw_sig in, odin3_pw_sig *out) {
    odin3_value params[NOT_PARAMS] = {odin3_value_int(0), odin3_value_int(in.len),
                                      odin3_value_int(in.len)};
    odin3_proc_cell cell = {
        odin3_proc_type(fc->mctx, "$not"), params, NOT_PARAMS, NULL, &in, 1, NULL, NULL, 0, {0}};
    return ff_add(fc, cell, out);
}

/* $reduce_or(in): A_SIGNED 0, A_WIDTH |in|, Y_WIDTH 1. */
static odin3_status ff_reduce_or(const ff_ctx *fc, odin3_pw_sig in, odin3_pw_sig *out) {
    odin3_value params[NOT_PARAMS] = {odin3_value_int(0), odin3_value_int(in.len),
                                      odin3_value_int(1)};
    odin3_proc_cell cell = {odin3_proc_type(fc->mctx, "$reduce_or"),
                            params,
                            NOT_PARAMS,
                            NULL,
                            &in,
                            1,
                            NULL,
                            NULL,
                            0,
                            {0}};
    return ff_add(fc, cell, out);
}

/* $mux(A = ins[0], B = ins[1], S = ins[2]) of width |A|, as Yosys's Module::Mux. */
static odin3_status ff_mux(const ff_ctx *fc, const odin3_pw_sig *ins, odin3_pw_sig *out) {
    odin3_value params[MUX_PARAMS] = {odin3_value_int(ins[0].len)};
    odin3_proc_cell cell = {odin3_proc_type(fc->mctx, "$mux"),
                            params,
                            MUX_PARAMS,
                            NULL,
                            ins,
                            MUX_INPUTS,
                            NULL,
                            NULL,
                            0,
                            {0}};
    return ff_add(fc, cell, out);
}

/* *out = the rule's trigger as a level-1 bit: the signal for LEVEL1, else a new $not of it. */
static odin3_status ff_trigger(const ff_ctx *fc, const ff_rule *rule, odin3_pw_bit *out) {
    *out = rule->signal;
    if (rule->type == ODIN3_SYNC_LEVEL1) {
        return ODIN3_OK;
    }
    odin3_pw_sig in = {0, 0};
    odin3_pw_sig not_y = {0, 0};
    odin3_status st = ff_bit(fc->pw, rule->signal, &in);
    st = st == ODIN3_OK ? ff_not(fc, in, &not_y) : st;
    if (st == ODIN3_OK) {
        *out = odin3_pw_sig_bits(fc->pw, not_y)[0];
    }
    return st;
}

/* --- collapse and self reset (§9 steps 1-2) ------------------------------------------------ */

/* True when there are rules and all their values are the first one's (bit by bit). */
static bool ff_rules_equal(const ff_ctx *fc) {
    const ff_rule *rules = fc->rules.data;
    for (size_t i = 1; i < fc->rules.len; i++) {
        if (!odin3_pw_sig_equal(fc->pw, rules[i].value, rules[0].value)) {
            return false;
        }
    }
    return fc->rules.len > 0;
}

/* Equal rules become one LEVEL1 rule on $reduce_or of their triggers (even of one), made after
 * the triggers' $nots, in rule order. */
static odin3_status ff_collapse(ff_ctx *fc) {
    if (!ff_rules_equal(fc)) {
        return ODIN3_OK;
    }
    size_t count = fc->rules.len;
    odin3_pw_bit *triggers = odin3_util_malloc(sizeof *triggers * (count + 1));
    if (triggers == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < count; i++) {
        st = ff_trigger(fc, odin3_vec_cat(&fc->rules, i), &triggers[i]);
    }
    odin3_pw_sig in = {0, 0};
    odin3_pw_sig or_y = {0, 0};
    st = st == ODIN3_OK ? odin3_pw_sig_new(fc->pw, triggers, (uint32_t)count, &in) : st;
    odin3_util_free(triggers);
    st = st == ODIN3_OK ? ff_reduce_or(fc, in, &or_y) : st;
    if (st == ODIN3_OK) {
        ff_rule *first = odin3_vec_at(&fc->rules, 0);
        first->signal = odin3_pw_sig_bits(fc->pw, or_y)[0];
        first->type = ODIN3_SYNC_LEVEL1;
        while (fc->rules.len > 1) {
            odin3_vec_pop(&fc->rules);
        }
    }
    return st;
}

/* One rule whose value is the group itself (a register the reset does not touch): insig =
 * $mux(insig, sig, trigger) for LEVEL1 ($mux(sig, insig, signal) for LEVEL0), and the rule goes. */
static odin3_status ff_self_reset(ff_ctx *fc) {
    if (fc->rules.len != 1) {
        return ODIN3_OK;
    }
    const ff_rule *rule = odin3_vec_cat(&fc->rules, 0);
    if (!odin3_pw_sig_equal(fc->pw, rule->value, fc->live)) {
        return ODIN3_OK;
    }
    bool high = rule->type == ODIN3_SYNC_LEVEL1;
    odin3_pw_sig ins[MUX_INPUTS] = {
        high ? fc->insig : fc->live, high ? fc->live : fc->insig, {0, 0}};
    odin3_status st = ff_bit(fc->pw, rule->signal, &ins[2]);
    st = st == ODIN3_OK ? ff_mux(fc, ins, &fc->insig) : st;
    if (st == ODIN3_OK) {
        odin3_vec_clear(&fc->rules);
    }
    return st;
}

/* --- warnings (§15) ------------------------------------------------------------------------ */

/* Appends 'name' of one bit. */
static odin3_status ff_put_name(const ff_ctx *fc, odin3_pw_bit bit, odin3_strbuf *out) {
    odin3_status st = odin3_strbuf_append(out, odin3_bytes_cstr("'"));
    st = st == ODIN3_OK ? odin3_proc_name_bits(fc->mctx, &bit, 1, out) : st;
    return st == ODIN3_OK ? odin3_strbuf_append(out, odin3_bytes_cstr("'")) : st;
}

static const char *text_of(const odin3_strbuf *buf) {
    return buf->data != NULL ? buf->data : "";
}

/* Appends the names of the resets' signals (n of them) as W3 and W4 list them: 'r1', 'r2', the
 * first three and then `…` when there are more. */
static odin3_status ff_put_names(const ff_ctx *fc, const odin3_pw_bit *signals, size_t n,
                                 odin3_strbuf *list) {
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < n && i < W4_NAMES; i++) {
        st = i > 0 ? odin3_strbuf_append(list, odin3_bytes_cstr(", ")) : ODIN3_OK;
        st = st == ODIN3_OK ? ff_put_name(fc, signals[i], list) : st;
    }
    if (st == ODIN3_OK && n > W4_NAMES) {
        st = odin3_strbuf_append(list, odin3_bytes_cstr(", …"));
    }
    return st;
}

/* The texts W3 and W4 fill in: the group's name and the list of its resets' names. */
typedef struct ff_texts {
    odin3_strbuf group;
    odin3_strbuf list;
} ff_texts;

/* Both texts for fc's group and resets (fc->resets); free them with ff_texts_free. */
static odin3_status ff_texts_make(const ff_ctx *fc, ff_texts *texts) {
    odin3_strbuf_init(&texts->group);
    odin3_strbuf_init(&texts->list);
    odin3_status st = odin3_proc_name_bits(fc->mctx, odin3_pw_sig_bits(fc->pw, fc->live),
                                           fc->live.len, &texts->group);
    return st == ODIN3_OK ? ff_put_names(fc, fc->resets.data, fc->resets.len, &texts->list) : st;
}

static void ff_texts_free(ff_texts *texts) {
    odin3_strbuf_free(&texts->group);
    odin3_strbuf_free(&texts->list);
}

/* W4: the group and its resets' names. */
static odin3_status ff_warn_w4(const ff_ctx *fc) {
    ff_texts texts;
    odin3_status st = ff_texts_make(fc, &texts);
    if (st == ODIN3_OK) {
        odin3_proc_warn(fc->mctx, ODIN3_PROC_W4, fc->pw->prov,
                        "'%s' has asynchronous resets with different values (%s); it is built as "
                        "a set/reset register ($dffsr)",
                        text_of(&texts.group), text_of(&texts.list));
    }
    ff_texts_free(&texts);
    return st;
}

/* W3: the group and every reset that collapsed into the trigger, as written: §15's text for one,
 * `asynchronous resets ('r1', 'r2') are active` for several. */
static odin3_status ff_warn_w3(const ff_ctx *fc) {
    ff_texts texts;
    odin3_status st = ff_texts_make(fc, &texts);
    if (st == ODIN3_OK && fc->resets.len == 1) {
        odin3_proc_warn(fc->mctx, ODIN3_PROC_W3, fc->pw->prov,
                        "'%s' takes a non-constant value while asynchronous reset %s is active; "
                        "it is built as an asynchronous-load register ($aldff)",
                        text_of(&texts.group), text_of(&texts.list));
    } else if (st == ODIN3_OK) {
        odin3_proc_warn(fc->mctx, ODIN3_PROC_W3, fc->pw->prov,
                        "'%s' takes a non-constant value while asynchronous resets (%s) are "
                        "active; it is built as an asynchronous-load register ($aldff)",
                        text_of(&texts.group), text_of(&texts.list));
    }
    ff_texts_free(&texts);
    return st;
}

/* --- registers (§9 steps 3-5) -------------------------------------------------------------- */

/* The edge sync's signal as a run (CLK) and its polarity (1 for POSEDGE). */
static odin3_status ff_clock(const ff_ctx *fc, odin3_pw_sig *clk, int64_t *polarity) {
    const odin3_pw_sync *edge = odin3_vec_cat(&fc->pw->syncs, fc->edge);
    *polarity = edge->type == ODIN3_SYNC_POSEDGE ? 1 : 0;
    return ff_bit(fc->pw, edge->signal, clk);
}

/* The register cell: Q on the group's own nets, the $proc node's attributes (§9, §14). */
static odin3_status ff_register(const ff_ctx *fc, odin3_proc_cell cell) {
    cell.output = &fc->live;
    cell.attrs_from = fc->pw;
    return ff_add(fc, cell, NULL);
}

/* gen_dffsr_complex's loop body: set = $mux(set, value, trig), then clr = $mux(clr, $not(value),
 * trig), where trig is the rule's signal or a $not of it, made first. */
static odin3_status ff_dffsr_rule(const ff_ctx *fc, const ff_rule *rule, odin3_pw_sig *chains) {
    odin3_pw_bit trig = 0;
    odin3_pw_sig select = {0, 0};
    odin3_pw_sig inv = {0, 0};
    odin3_status st = ff_trigger(fc, rule, &trig);
    st = st == ODIN3_OK ? ff_bit(fc->pw, trig, &select) : st;
    odin3_pw_sig set_ins[MUX_INPUTS] = {chains[0], rule->value, select};
    st = st == ODIN3_OK ? ff_mux(fc, set_ins, &chains[0]) : st;
    st = st == ODIN3_OK ? ff_not(fc, rule->value, &inv) : st;
    odin3_pw_sig clr_ins[MUX_INPUTS] = {chains[1], inv, select};
    return st == ODIN3_OK ? ff_mux(fc, clr_ins, &chains[1]) : st;
}

/* Several rules with different values (§9 step 3): W4, the set and clear chains from the last rule
 * to the first, starting from 0, then $dffsr(CLK, SET, CLR, D = insig, Q). */
static odin3_status ff_dffsr(const ff_ctx *fc) {
    odin3_pw_sig chains[2] = {{0, 0}, {0, 0}}; /* set, clr */
    odin3_status st = ff_warn_w4(fc);
    st = st == ODIN3_OK ? ff_fill(fc->pw, ODIN3_PW_S0, fc->sig.len, &chains[0]) : st;
    st = st == ODIN3_OK ? ff_fill(fc->pw, ODIN3_PW_S0, fc->sig.len, &chains[1]) : st;
    for (size_t k = fc->rules.len; st == ODIN3_OK && k > 0; k--) {
        st = ff_dffsr_rule(fc, odin3_vec_cat(&fc->rules, k - 1), chains);
    }
    odin3_pw_sig ins[REG_INPUTS] = {{0, 0}, chains[0], chains[1], fc->insig};
    int64_t clk_pol = 0;
    st = st == ODIN3_OK ? ff_clock(fc, &ins[0], &clk_pol) : st;
    odin3_value params[DFFSR_PARAMS] = {odin3_value_int(fc->sig.len), odin3_value_int(clk_pol),
                                        odin3_value_int(1), odin3_value_int(1)};
    odin3_proc_cell cell = {odin3_proc_type(fc->mctx, "$dffsr"),
                            params,
                            DFFSR_PARAMS,
                            NULL,
                            ins,
                            REG_INPUTS,
                            NULL,
                            NULL,
                            0,
                            {0}};
    return st == ODIN3_OK ? ff_register(fc, cell) : st;
}

/* One rule whose value is not made of constant nets (§9 step 4): W3, then $aldff(CLK, ALOAD =
 * the trigger, AD = the value, D = insig, Q). */
static odin3_status ff_aldff(const ff_ctx *fc, const ff_rule *rule) {
    odin3_pw_sig ins[REG_INPUTS] = {{0, 0}, {0, 0}, rule->value, fc->insig};
    int64_t clk_pol = 0;
    odin3_status st = ff_warn_w3(fc);
    st = st == ODIN3_OK ? ff_clock(fc, &ins[0], &clk_pol) : st;
    st = st == ODIN3_OK ? ff_bit(fc->pw, rule->signal, &ins[1]) : st;
    odin3_value params[ALDFF_PARAMS] = {odin3_value_int(fc->sig.len), odin3_value_int(clk_pol),
                                        odin3_value_int(rule->type == ODIN3_SYNC_LEVEL1 ? 1 : 0)};
    odin3_proc_cell cell = {odin3_proc_type(fc->mctx, "$aldff"),
                            params,
                            ALDFF_PARAMS,
                            NULL,
                            ins,
                            REG_INPUTS,
                            NULL,
                            NULL,
                            0,
                            {0}};
    return st == ODIN3_OK ? ff_register(fc, cell) : st;
}

/* The constant value's bits (ARST_VALUE), LSB first; '-' and the marked state read as x. */
static void ff_value_bits(const ff_ctx *fc, odin3_pw_sig value, uint8_t *bits) {
    const odin3_pw_bit *from = odin3_pw_sig_bits(fc->pw, value);
    for (uint32_t i = 0; i < value.len; i++) {
        uint32_t state = from[i] & ~ODIN3_PW_CONST;
        bits[i] = (uint8_t)(state <= ODIN3_BIT_Z ? state : ODIN3_BIT_X);
    }
}

/* §9 step 5 without a rule: $dff(CLK, D = insig, Q). */
static odin3_status ff_dff(const ff_ctx *fc) {
    odin3_pw_sig ins[2] = {{0, 0}, fc->insig}; /* CLK, D */
    int64_t clk_pol = 0;
    odin3_status st = ff_clock(fc, &ins[0], &clk_pol);
    odin3_value params[DFF_PARAMS] = {odin3_value_int(fc->sig.len), odin3_value_int(clk_pol)};
    odin3_proc_cell cell = {
        odin3_proc_type(fc->mctx, "$dff"), params, DFF_PARAMS, NULL, ins, 2, NULL, NULL, 0, {0}};
    return st == ODIN3_OK ? ff_register(fc, cell) : st;
}

/* §9 step 5 with a rule of constant value: $adff(CLK, ARST = the trigger, D = insig, Q;
 * ARST_POLARITY 1 for a LEVEL1 rule, ARST_VALUE the constant). */
static odin3_status ff_adff(const ff_ctx *fc, const ff_rule *rule) {
    odin3_pw_sig ins[3] = {{0, 0}, {0, 0}, fc->insig}; /* CLK, ARST, D */
    int64_t clk_pol = 0;
    uint8_t *bits = odin3_util_malloc((size_t)rule->value.len + 1);
    odin3_status st = bits != NULL ? ff_clock(fc, &ins[0], &clk_pol) : ODIN3_ERR_NO_MEMORY;
    st = st == ODIN3_OK ? ff_bit(fc->pw, rule->signal, &ins[1]) : st;
    if (st == ODIN3_OK) {
        ff_value_bits(fc, rule->value, bits);
        odin3_value params[ADFF_PARAMS] = {
            odin3_value_int(fc->sig.len),
            odin3_value_int(clk_pol),
            odin3_value_int(rule->type == ODIN3_SYNC_LEVEL1 ? 1 : 0),
            {.kind = ODIN3_VAL_BITS, .bits = bits, .len = rule->value.len}};
        odin3_proc_cell cell = {odin3_proc_type(fc->mctx, "$adff"),
                                params,
                                ADFF_PARAMS,
                                NULL,
                                ins,
                                3,
                                NULL,
                                NULL,
                                0,
                                {0}};
        st = ff_register(fc, cell);
    }
    odin3_util_free(bits);
    return st;
}

/* The register of the group, by §9's order: $dffsr, $aldff, then $adff or $dff. A group without an
 * edge sync is E5, raised by the clock pre-check (§5.5), so reaching it is a 2E bug. */
static odin3_status ff_emit(const ff_ctx *fc) {
    if (fc->edge == NO_SYNC) {
        return ff_internal("register without a clock edge");
    }
    if (fc->rules.len > 1) {
        return ff_dffsr(fc);
    }
    const ff_rule *rule = fc->rules.len > 0 ? odin3_vec_cat(&fc->rules, 0) : NULL;
    if (rule == NULL) {
        return ff_dff(fc);
    }
    return odin3_pw_sig_is_fully_const(fc->pw, rule->value) ? ff_adff(fc, rule)
                                                            : ff_aldff(fc, rule);
}

/* fc->resets = every rule's signal, in rule order (the names W3 and W4 give). */
static odin3_status ff_note_resets(ff_ctx *fc) {
    odin3_vec_clear(&fc->resets);
    if (odin3_vec_reserve(&fc->resets, fc->rules.len) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < fc->rules.len; i++) {
        *(odin3_pw_bit *)odin3_vec_push(&fc->resets) =
            ((const ff_rule *)odin3_vec_cat(&fc->rules, i))->signal; /* reserved */
    }
    return ODIN3_OK;
}

/* One group (fc->sig): its rules and edge value, the collapse, the self reset, the register. */
static odin3_status ff_group(ff_ctx *fc) {
    odin3_vec_clear(&fc->rules);
    fc->edge = NO_SYNC;
    odin3_status st = ff_live_run(fc->mctx, fc->pw, fc->sig, &fc->live);
    st = st == ODIN3_OK ? ff_fill(fc->pw, ODIN3_PW_SZ, fc->sig.len, &fc->insig) : st;
    st = st == ODIN3_OK ? ff_gather(fc) : st;
    st = st == ODIN3_OK ? ff_note_resets(fc) : st;
    st = st == ODIN3_OK ? ff_collapse(fc) : st;
    st = st == ODIN3_OK ? ff_self_reset(fc) : st;
    return st == ODIN3_OK ? ff_emit(fc) : st;
}

odin3_status odin3_proc_ffs(odin3_proc_mctx *mctx, odin3_pw *pw) {
    if (mctx == NULL || pw == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "proc: ffs: no module context or process");
        return ODIN3_ERR_INVALID_ARG;
    }
    ff_ctx fc = {mctx, pw, {0, 0}, {0, 0}, {0, 0}, NO_SYNC, {0}, {0}};
    odin3_vec_init(&fc.rules, sizeof(ff_rule));
    odin3_vec_init(&fc.resets, sizeof(odin3_pw_bit));
    odin3_status st = ff_live(mctx, pw);
    bool more = st == ODIN3_OK;
    while (more) {
        st = ff_find_lvalue(&fc);
        more = st == ODIN3_OK && fc.sig.len > 0;
        st = more ? ff_group(&fc) : st;
        more = more && st == ODIN3_OK;
    }
    odin3_vec_free(&fc.rules);
    odin3_vec_free(&fc.resets);
    return st;
}
