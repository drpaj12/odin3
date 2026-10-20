/*
 * extract_trans.c — the transition table of fsm_extract (spec X-4; Yosys 0.55
 * passes/fsm/fsm_extract.cc:126-254 find_transitions and :407-418): for every state, Q is set to
 * the code and the logic is evaluated with the control inputs as stop signals; where evaluation
 * stops at an input the search branches on 0 and 1 (or takes the input as a don't care when the
 * nostop evaluator can decide it), respecting $pmux exclusivity.
 *
 * The recursion of Yosys is an explicit stack of frames; each frame owns copies of its dff_in,
 * ctrl_out and dont_care arrays (Yosys passes them by value), kept in one word stack.
 */
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "passes/fsm/ceval.h"
#include "passes/fsm/extract_int.h"
#include "passes/fsm/fsm.h"
#include "passes/fsm/fsmdata.h"
#include "passes/fsm/index.h"
#include "passes/opt/kit.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { LABEL_MAX = 160 };

typedef enum tframe_phase { PH_START, PH_BRANCH1, PH_END } tframe_phase;
typedef enum tframe_kind { KIND_NOSTOP, KIND_SPLIT } tframe_kind;

/* One call of find_transitions: where its arrays are in the word stack. */
typedef struct tframe {
    uint32_t base, dc_n; /* dff_in, ctrl_out, dont_care start at base */
    odin3_fsm_sig undef;
    uint8_t phase, kind;
} tframe;

typedef struct tx {
    odin3_fsm_xctx *xc;
    odin3_fsm_ceval *ce, *ce_nostop;
    odin3_vec words;  /* odin3_fsm_sig: the frames' arrays */
    odin3_vec frames; /* tframe */
    uint32_t width, n_in, n_out;
    int32_t state_in;
    bool capped, unsupported;
} tx;

static tframe *top(tx *ctx) {
    return odin3_vec_at(&ctx->frames, ctx->frames.len - 1);
}

static odin3_fsm_sig *words_at(tx *ctx, uint32_t off) {
    odin3_fsm_sig *words = ctx->words.data;
    return words + off;
}

static odin3_fsm_sig *dff_arr(tx *ctx, const tframe *fr) {
    return words_at(ctx, fr->base);
}

static odin3_fsm_sig *out_arr(tx *ctx, const tframe *fr) {
    return words_at(ctx, fr->base + ctx->width);
}

static odin3_fsm_sig *dc_arr(tx *ctx, const tframe *fr) {
    return words_at(ctx, fr->base + ctx->width + ctx->n_out);
}

/* A span of the word stack. */
typedef struct word_span {
    uint32_t from, count;
} word_span;

static odin3_status push_words(tx *ctx, word_span span) {
    for (uint32_t i = 0; i < span.count; i++) {
        odin3_status st = odin3_kit_push_u32(&ctx->words, words_at(ctx, span.from)[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* A child frame holding copies of the parent's arrays (plus `extra` don't-care bit if given). */
static odin3_status push_child(tx *ctx, const odin3_fsm_sig *extra) {
    uint32_t parent_base = top(ctx)->base;
    uint32_t parent_dc = top(ctx)->dc_n;
    uint32_t base = (uint32_t)ctx->words.len;
    odin3_status st =
        push_words(ctx, (word_span){parent_base, ctx->width + ctx->n_out + parent_dc});
    uint32_t dc_n = parent_dc;
    if (st == ODIN3_OK && extra != NULL) {
        st = odin3_kit_push_u32(&ctx->words, *extra);
        dc_n++;
    }
    tframe *fr = st == ODIN3_OK ? odin3_vec_push(&ctx->frames) : NULL;
    if (fr == NULL) {
        odin3_kit_truncate(&ctx->words, base);
        return ODIN3_ERR_NO_MEMORY;
    }
    fr->base = base;
    fr->dc_n = dc_n;
    return ODIN3_OK;
}

static void pop_frame(tx *ctx) {
    odin3_kit_truncate(&ctx->words, top(ctx)->base);
    odin3_vec_pop(&ctx->frames);
}

/* --- exclusive controls ----------------------------------------------------------------------- */

/* A range [lo, hi) of the exclusion list. */
typedef struct excl_span {
    size_t lo, hi;
} excl_span;

/* The keys of excl that start with bit. */
static excl_span excl_range(const odin3_vec *excl, odin3_fsm_sig bit) {
    const uint64_t *keys = excl->data;
    uint64_t first = odin3_fsm_excl_key(bit, 0);
    size_t left = 0;
    size_t right = excl->len;
    while (left < right) {
        size_t mid = left + (right - left) / 2;
        if (keys[mid] < first) {
            left = mid + 1;
        } else {
            right = mid;
        }
    }
    excl_span span = {left, left};
    while (span.hi < excl->len && odin3_fsm_excl_bit(keys[span.hi]) == bit) {
        span.hi++;
    }
    return span;
}

/* --- recording a transition (:161-205) ----------------------------------------------------------
 */

static bool in_dont_care(tx *ctx, const tframe *fr, odin3_fsm_sig bit) {
    const odin3_fsm_sig *dc = dc_arr(ctx, fr);
    for (uint32_t i = 0; i < fr->dc_n; i++) {
        if (dc[i] == bit) {
            return true;
        }
    }
    return false;
}

/* sig2const(ce, ctrl_in, Sa, dont_care): the value of each control input as the search saw it. */
static void ctrl_in_pattern(tx *ctx, const tframe *fr, uint8_t *pattern) {
    const odin3_fsm_sig *ctrl = ctx->xc->inst->ctrl_in.data;
    for (uint32_t i = 0; i < ctx->n_in; i++) {
        odin3_fsm_sig sig = ctrl[i];
        if (in_dont_care(ctx, fr, sig)) {
            pattern[i] = ODIN3_FSM_BIT_DC;
            continue;
        }
        sig = odin3_fsm_ceval_get(ctx->ce, sig);
        pattern[i] =
            odin3_fsm_sig_is_const(sig) ? odin3_fsm_sig_bit(sig) : (uint8_t)ODIN3_FSM_BIT_DC;
    }
}

/* Control inputs that cannot be 1 together with a selected pmux bit become don't care (:178-183).
 */
static void apply_exclusive(tx *ctx, uint8_t *pattern) {
    const odin3_fsm_sig *ctrl = ctx->xc->inst->ctrl_in.data;
    for (uint32_t i = 0; i < ctx->n_in; i++) {
        if (pattern[i] != ODIN3_FSM_BIT_1) {
            continue;
        }
        excl_span span = excl_range(ctx->xc->excl, ctrl[i]);
        const uint64_t *keys = ctx->xc->excl->data;
        for (size_t k = span.lo; k < span.hi; k++) {
            odin3_fsm_sig other = odin3_fsm_excl_other(keys[k]);
            for (uint32_t j = 0; j < ctx->n_in; j++) {
                if (ctrl[j] == other) {
                    pattern[j] = ODIN3_FSM_BIT_DC;
                }
            }
        }
    }
}

/* The code of the next state as the evaluation left it, as bits. */
static void next_code(tx *ctx, const tframe *fr, uint8_t *code) {
    const odin3_fsm_sig *dff = dff_arr(ctx, fr);
    for (uint32_t i = 0; i < ctx->width; i++) {
        odin3_fsm_sig sig = odin3_fsm_ceval_get(ctx->ce, dff[i]);
        code[i] = odin3_fsm_sig_is_const(sig) ? odin3_fsm_sig_bit(sig) : (uint8_t)ODIN3_FSM_BIT_X;
    }
}

static bool code_defined(const uint8_t *code, uint32_t width) {
    for (uint32_t i = 0; i < width; i++) {
        if (!odin3_fsm_bit_defined(code[i])) {
            return false;
        }
    }
    return true;
}

static void log_transition(tx *ctx, const char *what, const odin3_fsm_trans *tr) {
    if (odin3_log_get_level() < ODIN3_LOG_DEBUG) {
        return;
    }
    char in_bits[LABEL_MAX];
    char out_bits[LABEL_MAX];
    size_t n_in = ctx->n_in < LABEL_MAX - 1U ? ctx->n_in : LABEL_MAX - 1U;
    size_t n_out = ctx->n_out < LABEL_MAX - 1U ? ctx->n_out : LABEL_MAX - 1U;
    for (size_t i = 0; i < n_in; i++) {
        in_bits[i] = odin3_fsm_bit_char(tr->ctrl_in[i]);
    }
    for (size_t i = 0; i < n_out; i++) {
        out_bits[i] = odin3_fsm_bit_char(tr->ctrl_out[i]);
    }
    in_bits[n_in] = out_bits[n_out] = '\0';
    odin3_log(ODIN3_LOG_DEBUG, "fsm: transition: %d %s -> %d %s %s", tr->state_in, in_bits,
              tr->state_out, out_bits, what);
}

/* All of ctrl_out and dff_in are constants: records the transition, or drops it (:161-205). */
static odin3_status record(tx *ctx) {
    const tframe *fr = top(ctx);
    odin3_fsm_data *data = &ctx->xc->inst->data;
    odin3_fsm_trans tr;
    tr.state_in = ctx->state_in;
    tr.state_out = -1;
    tr.ctrl_in = odin3_fsm_data_bits(data, ctx->n_in);
    tr.ctrl_out = odin3_fsm_data_bits(data, ctx->n_out);
    uint8_t *code = odin3_util_malloc((size_t)ctx->width + 1);
    if (tr.ctrl_in == NULL || tr.ctrl_out == NULL || code == NULL) {
        odin3_util_free(code);
        return ODIN3_ERR_NO_MEMORY;
    }
    ctrl_in_pattern(ctx, fr, tr.ctrl_in);
    apply_exclusive(ctx, tr.ctrl_in);
    const odin3_fsm_sig *out = out_arr(ctx, fr);
    for (uint32_t i = 0; i < ctx->n_out; i++) {
        tr.ctrl_out[i] =
            odin3_fsm_sig_is_const(out[i]) ? odin3_fsm_sig_bit(out[i]) : (uint8_t)ODIN3_FSM_BIT_X;
    }
    next_code(ctx, fr, code);
    const odin3_fsm_state_ent *ent = odin3_fsm_xctx_find_state(ctx->xc, code);
    odin3_status st = ODIN3_OK;
    if (ent == NULL) {
        log_transition(ctx, "<ignored invalid transition!>", &tr);
    } else {
        tr.state_out = ent->idx;
        if (code_defined(code, ctx->width)) {
            log_transition(ctx, "", &tr);
            st = odin3_fsm_data_add_trans(data, &tr);
        } else {
            log_transition(ctx, "<ignored undef transition!>", &tr);
        }
    }
    odin3_util_free(code);
    return st;
}

/* --- the search ------------------------------------------------------------------------------- */

static bool is_const_x(odin3_fsm_sig sig) {
    return odin3_fsm_sig_is_const(sig) && odin3_fsm_sig_bit(sig) == ODIN3_FSM_BIT_X;
}

/* The undefined-bit case: dff_in holds an x after the failed evaluation (:151-157). */
static bool dff_has_x(tx *ctx, const tframe *fr) {
    const odin3_fsm_sig *dff = dff_arr(ctx, fr);
    for (uint32_t i = 0; i < ctx->width; i++) {
        if (is_const_x(dff[i])) {
            return true;
        }
    }
    return false;
}

/* True when the budget ran out in res (the search then stops). */
static bool note_cap(tx *ctx, const odin3_fsm_ceval_res *res) {
    ctx->capped = ctx->capped || res->capped;
    return res->capped;
}

/* The first evaluation of a frame: ctrl_out, then dff_in; ok says both are constant. */
static odin3_status eval_frame(tx *ctx, bool *ok, odin3_fsm_ceval_res *res) {
    const tframe *fr = top(ctx);
    odin3_status st = odin3_fsm_ceval_eval(ctx->ce, out_arr(ctx, fr), ctx->n_out, res);
    *ok = st == ODIN3_OK && res->ok;
    if (*ok) {
        fr = top(ctx);
        st = odin3_fsm_ceval_eval(ctx->ce, dff_arr(ctx, fr), ctx->width, res);
        *ok = st == ODIN3_OK && res->ok;
    }
    return st;
}

static void set_one(odin3_fsm_ceval *ce, odin3_fsm_sig sig, uint8_t bit, odin3_status *st) {
    if (*st == ODIN3_OK) {
        *st = odin3_fsm_ceval_set(ce, &sig, &bit, 1);
    }
}

/* Evaluates the single bit `bit` in ce; the evaluator's answer in *value. */
static odin3_status eval_bit(odin3_fsm_ceval *ce, odin3_fsm_sig *bit, odin3_fsm_ceval_res *res) {
    return odin3_fsm_ceval_eval(ce, bit, 1, res);
}

/*
 * Sets every bit that excludes `undef` to 0 after undef was set to 1 (:208-214, :243-251);
 * *contradiction when one of them is already 1 (or undecidable as 1). Both evaluators when
 * `both`.
 */
static odin3_status apply_excl(tx *ctx, odin3_fsm_sig undef, bool both, bool *contradiction) {
    excl_span span = excl_range(ctx->xc->excl, undef);
    *contradiction = false;
    odin3_status st = ODIN3_OK;
    for (size_t k = span.lo; st == ODIN3_OK && !*contradiction && k < span.hi; k++) {
        odin3_fsm_sig bit = odin3_fsm_excl_other(((const uint64_t *)ctx->xc->excl->data)[k]);
        odin3_fsm_ceval_res res;
        st = eval_bit(ctx->ce, &bit, &res);
        bool known = st == ODIN3_OK && res.ok;
        if (st == ODIN3_OK && !known && both) {
            st = eval_bit(ctx->ce_nostop, &bit, &res);
            known = st == ODIN3_OK && res.ok;
        }
        if (st == ODIN3_OK && known && bit != odin3_fsm_sig_of_bit(ODIN3_FSM_BIT_0)) {
            *contradiction = true;
        } else {
            set_one(ctx->ce, bit, ODIN3_FSM_BIT_0, &st);
            if (both) {
                set_one(ctx->ce_nostop, bit, ODIN3_FSM_BIT_0, &st);
            }
        }
    }
    return st;
}

/* The control input could be decided without stops: take it as a don't care (:214-232). */
static odin3_status branch_nostop(tx *ctx, odin3_fsm_sig undef, uint8_t value) {
    size_t depth = odin3_fsm_ceval_depth(ctx->ce);
    odin3_status st = odin3_fsm_ceval_push(ctx->ce);
    set_one(ctx->ce, undef, value, &st);
    bool contradiction = false;
    if (st == ODIN3_OK && value == ODIN3_FSM_BIT_1) {
        st = apply_excl(ctx, undef, false, &contradiction);
    }
    if (st != ODIN3_OK || contradiction) {
        odin3_fsm_ceval_unwind(ctx->ce, depth);
        pop_frame(ctx);
        return st;
    }
    top(ctx)->phase = PH_END;
    top(ctx)->kind = KIND_NOSTOP;
    top(ctx)->undef = undef;
    return push_child(ctx, &undef);
}

/* Sets up one branch of a split (value 0 or 1) and descends, or pops on a contradiction. */
static odin3_status branch_split(tx *ctx, odin3_fsm_sig undef, uint8_t value) {
    size_t depth = odin3_fsm_ceval_depth(ctx->ce);
    size_t depth_ns = odin3_fsm_ceval_depth(ctx->ce_nostop);
    odin3_status st = odin3_fsm_ceval_push(ctx->ce);
    st = st == ODIN3_OK ? odin3_fsm_ceval_push(ctx->ce_nostop) : st;
    set_one(ctx->ce, undef, value, &st);
    set_one(ctx->ce_nostop, undef, value, &st);
    bool contradiction = false;
    if (st == ODIN3_OK && value == ODIN3_FSM_BIT_1) {
        st = apply_excl(ctx, undef, true, &contradiction);
    }
    if (st != ODIN3_OK || contradiction) {
        odin3_fsm_ceval_unwind(ctx->ce, depth);
        odin3_fsm_ceval_unwind(ctx->ce_nostop, depth_ns);
        pop_frame(ctx);
        return st;
    }
    top(ctx)->phase = value == ODIN3_FSM_BIT_0 ? PH_BRANCH1 : PH_END;
    top(ctx)->kind = KIND_SPLIT;
    top(ctx)->undef = undef;
    return push_child(ctx, NULL);
}

/* The start of a frame: evaluate; record, drop, or branch on the first undefined input. */
static odin3_status step_start(tx *ctx) {
    bool ok = false;
    odin3_fsm_ceval_res res;
    odin3_status st = eval_frame(ctx, &ok, &res);
    if (st != ODIN3_OK || note_cap(ctx, &res)) {
        return st;
    }
    if (ok) {
        st = record(ctx);
        pop_frame(ctx);
        return st;
    }
    if (dff_has_x(ctx, top(ctx))) { /* shortened mode: the transition is dropped (:151-157) */
        odin3_log(ODIN3_LOG_DEBUG, "fsm: transition ignored (undefined bit in the next state)");
        pop_frame(ctx);
        return ODIN3_OK;
    }
    if (!res.has_undef || odin3_fsm_sig_is_const(res.undef) ||
        !odin3_fsm_ceval_is_stop(ctx->ce, res.undef)) {
        ctx->unsupported = true; /* Yosys asserts here: the logic holds a cell we cannot evaluate */
        return ODIN3_OK;
    }
    odin3_fsm_sig probe = res.undef;
    odin3_fsm_ceval_res nostop;
    st = eval_bit(ctx->ce_nostop, &probe, &nostop);
    if (st != ODIN3_OK || note_cap(ctx, &nostop)) {
        return st;
    }
    if (nostop.ok) {
        return branch_nostop(ctx, res.undef, odin3_fsm_sig_bit(probe));
    }
    return branch_split(ctx, res.undef, ODIN3_FSM_BIT_0);
}

/* The second branch of a split. */
static odin3_status step_branch1(tx *ctx) {
    odin3_fsm_ceval_pop(ctx->ce);
    odin3_fsm_ceval_pop(ctx->ce_nostop);
    return branch_split(ctx, top(ctx)->undef, ODIN3_FSM_BIT_1);
}

/* The end of a frame: the evaluators go back to where the frame found them. */
static void step_end(tx *ctx) {
    odin3_fsm_ceval_pop(ctx->ce);
    if (top(ctx)->kind == KIND_SPLIT) {
        odin3_fsm_ceval_pop(ctx->ce_nostop);
    }
    pop_frame(ctx);
}

/* The search for one state. */
static odin3_status search_state(tx *ctx) {
    odin3_fsm_inst *inst = ctx->xc->inst;
    tframe *root = odin3_vec_push(&ctx->frames);
    if (root == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    root->base = (uint32_t)ctx->words.len;
    odin3_status st = ODIN3_OK;
    const odin3_fsm_sig *dff = ctx->xc->dff_in.data;
    const odin3_fsm_sig *out = inst->ctrl_out.data;
    for (uint32_t i = 0; st == ODIN3_OK && i < ctx->width; i++) {
        st = odin3_kit_push_u32(&ctx->words, dff[i]);
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < ctx->n_out; i++) {
        st = odin3_kit_push_u32(&ctx->words, out[i]);
    }
    while (st == ODIN3_OK && !ctx->capped && !ctx->unsupported && ctx->frames.len > 0) {
        switch (top(ctx)->phase) {
        case PH_START:
            st = step_start(ctx);
            break;
        case PH_BRANCH1:
            st = step_branch1(ctx);
            break;
        default:
            step_end(ctx);
            break;
        }
    }
    odin3_vec_clear(&ctx->frames);
    odin3_vec_clear(&ctx->words);
    return st;
}

/* Sets Q of the old register to the code of state `idx` in both evaluators. */
static odin3_status set_state(tx *ctx, uint32_t idx) {
    const uint8_t *code = odin3_fsm_data_state(&ctx->xc->inst->data, idx);
    const odin3_fsm_sig *out = ctx->xc->inst->dff_out.data;
    odin3_status st = odin3_fsm_ceval_set(ctx->ce, out, code, ctx->width);
    return st == ODIN3_OK ? odin3_fsm_ceval_set(ctx->ce_nostop, out, code, ctx->width) : st;
}

/* The search for state idx between a push and a pop of both evaluators. */
static odin3_status run_state(tx *ctx, uint32_t idx) {
    ctx->state_in = (int32_t)idx;
    odin3_status st = odin3_fsm_ceval_push(ctx->ce);
    st = st == ODIN3_OK ? odin3_fsm_ceval_push(ctx->ce_nostop) : st;
    st = st == ODIN3_OK ? set_state(ctx, idx) : st;
    st = st == ODIN3_OK ? search_state(ctx) : st;
    odin3_fsm_ceval_unwind(ctx->ce, 0);
    odin3_fsm_ceval_unwind(ctx->ce_nostop, 0);
    return st;
}

/* Every state in turn (:407-418), until the budget runs out or the logic is unusable. */
static odin3_status run_states(tx *ctx) {
    const odin3_fsm_inst *inst = ctx->xc->inst;
    odin3_status st = odin3_fsm_ceval_create(&ctx->xc->fm->ix, &ctx->xc->budget, &ctx->ce);
    st = st == ODIN3_OK
             ? odin3_fsm_ceval_create(&ctx->xc->fm->ix, &ctx->xc->budget, &ctx->ce_nostop)
             : st;
    if (st == ODIN3_OK) {
        odin3_fsm_ceval_stop(ctx->ce, inst->ctrl_in.data, ctx->n_in);
    }
    for (uint32_t idx = 0; st == ODIN3_OK && idx < inst->data.states.len; idx++) {
        st = run_state(ctx, idx);
        if (ctx->capped || ctx->unsupported) {
            break;
        }
    }
    odin3_fsm_ceval_destroy(ctx->ce);
    odin3_fsm_ceval_destroy(ctx->ce_nostop);
    return st;
}

odin3_status odin3_fsm_extract_transitions(odin3_fsm_xctx *xc, odin3_fsm_xres *res) {
    const odin3_fsm_inst *inst = xc->inst;
    tx ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.xc = xc;
    ctx.width = (uint32_t)xc->dff_in.len;
    ctx.n_in = (uint32_t)inst->ctrl_in.len;
    ctx.n_out = (uint32_t)inst->ctrl_out.len;
    odin3_vec_init(&ctx.words, sizeof(odin3_fsm_sig));
    odin3_vec_init(&ctx.frames, sizeof(tframe));
    xc->budget.used = 0;
    odin3_status st = run_states(&ctx);
    *res = ODIN3_FSM_X_OK;
    if (ctx.capped) {
        *res = ODIN3_FSM_X_CAPPED;
    } else if (ctx.unsupported) {
        *res = ODIN3_FSM_X_FAILED;
    }
    odin3_vec_free(&ctx.words);
    odin3_vec_free(&ctx.frames);
    return st;
}
