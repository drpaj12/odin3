/*
 * extract.c — fsm_extract (spec X-1 to X-6; Yosys 0.55 passes/fsm/fsm_extract.cc): the state
 * register, the states, the control inputs and outputs. The transition table is built in
 * extract_trans.c. The IR is not changed (X-5): the result is an odin3_fsm_inst.
 */
#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LABEL_MAX = 160 };

void odin3_fsm_inst_free(odin3_fsm_inst *inst) {
    odin3_vec_free(&inst->dff_out);
    odin3_vec_free(&inst->ctrl_in);
    odin3_vec_free(&inst->ctrl_out);
    odin3_vec_free(&inst->mux_cells);
    odin3_vec_free(&inst->out_drivers);
    odin3_fsm_data_free(&inst->data);
    memset(inst, 0, sizeof *inst);
}

static odin3_status inst_init(odin3_fsm_inst *inst) {
    memset(inst, 0, sizeof *inst);
    odin3_vec_init(&inst->dff_out, sizeof(odin3_fsm_sig));
    odin3_vec_init(&inst->ctrl_in, sizeof(odin3_fsm_sig));
    odin3_vec_init(&inst->ctrl_out, sizeof(odin3_fsm_sig));
    odin3_vec_init(&inst->mux_cells, sizeof(uint32_t));
    odin3_vec_init(&inst->out_drivers, sizeof(uint32_t));
    return odin3_fsm_data_init(&inst->data);
}

/* --- small helpers --------------------------------------------------------------------------- */

static bool is_type(const odin3_fsm_mod *fm, uint32_t node, uint32_t type) {
    return type != 0 && odin3_fsm_ix_type(&fm->ix, node) == type;
}

static bool is_mux(const odin3_fsm_mod *fm, uint32_t node) {
    return is_type(fm, node, fm->ix.types.mux) || is_type(fm, node, fm->ix.types.pmux);
}

static odin3_fsm_sigref sigs_of(const odin3_vec *vec) {
    odin3_fsm_sigref ref = {vec->data, (uint32_t)vec->len};
    return ref;
}

static bool all_const(const odin3_vec *vec) {
    const odin3_fsm_sig *bits = vec->data;
    for (size_t i = 0; i < vec->len; i++) {
        if (!odin3_fsm_sig_is_const(bits[i])) {
            return false;
        }
    }
    return true;
}

static bool all_defined(const odin3_vec *vec) {
    const odin3_fsm_sig *bits = vec->data;
    for (size_t i = 0; i < vec->len; i++) {
        if (!odin3_fsm_sig_defined(bits[i])) {
            return false;
        }
    }
    return true;
}

static bool vec_equal(const odin3_vec *lhs, const odin3_vec *rhs) {
    return lhs->len == rhs->len &&
           (lhs->len == 0 || memcmp(lhs->data, rhs->data, lhs->len * sizeof(odin3_fsm_sig)) == 0);
}

static odin3_status copy_vec(odin3_vec *dst, const odin3_vec *src) {
    odin3_vec_clear(dst);
    const odin3_fsm_sig *bits = src->data;
    for (size_t i = 0; i < src->len; i++) {
        odin3_status st = odin3_kit_push_u32(dst, bits[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static odin3_status append_vec(odin3_vec *dst, const odin3_vec *src) {
    const odin3_fsm_sig *bits = src->data;
    for (size_t i = 0; i < src->len; i++) {
        odin3_status st = odin3_kit_push_u32(dst, bits[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* SigSpec::replace(pattern, with) in place: bits equal to pattern[i] become with[i]. */
static void replace_bits(odin3_vec *dst, const odin3_vec *pattern, const odin3_vec *with) {
    odin3_fsm_sig *bits = dst->data;
    const odin3_fsm_sig *pat = pattern->data;
    const odin3_fsm_sig *rep = with->data;
    for (size_t i = 0; i < pattern->len && i < with->len; i++) {
        if (odin3_fsm_sig_is_const(pat[i])) {
            continue;
        }
        for (size_t j = 0; j < dst->len; j++) {
            if (bits[j] == pat[i]) {
                bits[j] = rep[i];
            }
        }
    }
}

static void label_of(const odin3_fsm_mod *fm, odin3_wire_id wire, char *buf, size_t size) {
    odin3_fsm_wire_label(fm, wire, buf, size);
}

static void fail_log(const odin3_fsm_mod *fm, odin3_wire_id wire, const char *reason) {
    char label[LABEL_MAX];
    label_of(fm, wire, label, sizeof label);
    odin3_log(ODIN3_LOG_INFO, "fsm: extraction of '%s' failed: %s", label, reason);
}

/* --- the states ------------------------------------------------------------------------------ */

odin3_fsm_state_ent *odin3_fsm_xctx_find_state(const odin3_fsm_xctx *xc, const uint8_t *bits) {
    odin3_fsm_state_ent *ents = xc->states.data;
    uint32_t width = (uint32_t)xc->dff_in.len;
    size_t lo = 0;
    size_t hi = xc->states.len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = odin3_fsm_const_cmp(ents[mid].bits, width, bits, width);
        if (cmp == 0) {
            return &ents[mid];
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return NULL;
}

/* Adds `bits` to the sorted set (states[bits] = -1) unless it is there. */
static odin3_status add_state(odin3_fsm_xctx *xc, const uint8_t *bits) {
    odin3_fsm_state_ent *ents = xc->states.data;
    uint32_t width = (uint32_t)xc->dff_in.len;
    size_t lo = 0;
    size_t hi = xc->states.len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = odin3_fsm_const_cmp(ents[mid].bits, width, bits, width);
        if (cmp == 0) {
            return ODIN3_OK;
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    uint8_t *copy = odin3_fsm_data_bits(&xc->inst->data, width);
    if (copy == NULL || odin3_vec_push(&xc->states) == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(copy, bits, width);
    ents = xc->states.data;
    memmove(ents + lo + 1, ents + lo, (xc->states.len - 1 - lo) * sizeof *ents);
    ents[lo].bits = copy;
    ents[lo].idx = -1;
    return ODIN3_OK;
}

/* The bits of a fully constant signal as a code (x for a bit that is not constant). */
static void code_of(const odin3_fsm_sig *sig, uint32_t width, uint8_t *out) {
    for (uint32_t i = 0; i < width; i++) {
        out[i] =
            odin3_fsm_sig_is_const(sig[i]) ? odin3_fsm_sig_bit(sig[i]) : (uint8_t)ODIN3_FSM_BIT_X;
    }
}

/* --- find_states (fsm_extract.cc:41-124) ------------------------------------------------------ */

typedef struct fs_state {
    odin3_fsm_xctx *xc;
    odin3_vec tasks;       /* odin3_fsm_sig: the signals to visit, `width` bits each */
    odin3_vec cur, aa, bb; /* the signal in hand, its A and B replacements */
    odin3_vec pat, a_sig, b_sig, s_sig, y_sig;
    uint8_t *reset_state; /* NULL after the root */
    uint8_t *code;
} fs_state;

static void fs_free(fs_state *fs) {
    odin3_vec_free(&fs->tasks);
    odin3_vec_free(&fs->cur);
    odin3_vec_free(&fs->aa);
    odin3_vec_free(&fs->bb);
    odin3_vec_free(&fs->pat);
    odin3_vec_free(&fs->a_sig);
    odin3_vec_free(&fs->b_sig);
    odin3_vec_free(&fs->s_sig);
    odin3_vec_free(&fs->y_sig);
    odin3_util_free(fs->code);
}

static odin3_status fs_init(fs_state *fs, odin3_fsm_xctx *xc, uint8_t *reset_state) {
    memset(fs, 0, sizeof *fs);
    fs->xc = xc;
    fs->reset_state = reset_state;
    odin3_vec *vecs[] = {&fs->tasks, &fs->cur,   &fs->aa,    &fs->bb,   &fs->pat,
                         &fs->a_sig, &fs->b_sig, &fs->s_sig, &fs->y_sig};
    for (size_t i = 0; i < sizeof vecs / sizeof vecs[0]; i++) {
        odin3_vec_init(vecs[i], sizeof(odin3_fsm_sig));
    }
    fs->code = odin3_util_malloc(xc->dff_in.len + 1);
    return fs->code != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

/* Pops the last task (width bits) into fs->cur. */
static odin3_status fs_pop_task(fs_state *fs) {
    size_t width = fs->xc->dff_in.len;
    const odin3_fsm_sig *bits = fs->tasks.data;
    odin3_vec_clear(&fs->cur);
    for (size_t i = 0; i < width; i++) {
        odin3_status st = odin3_kit_push_u32(&fs->cur, bits[fs->tasks.len - width + i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    odin3_kit_truncate(&fs->tasks, fs->tasks.len - width);
    return ODIN3_OK;
}

/* A signal that is fully constant: a state code when fully defined (:51-57). */
static odin3_status fs_constant(fs_state *fs) {
    if (!all_defined(&fs->cur)) {
        return ODIN3_OK;
    }
    code_of(fs->cur.data, (uint32_t)fs->cur.len, fs->code);
    return add_state(fs->xc, fs->code);
}

/* Loads A, B, S and Y of the mux in hand into the scratch vectors. */
static odin3_status fs_load_mux(fs_state *fs, uint32_t node) {
    const odin3_fsm_index *ix = &fs->xc->fm->ix;
    odin3_vec *outs[] = {&fs->a_sig, &fs->b_sig, &fs->s_sig, &fs->y_sig};
    const uint32_t ports[] = {ODIN3_FSM_P_MUX_A, ODIN3_FSM_P_MUX_B, ODIN3_FSM_P_MUX_S,
                              ODIN3_FSM_P_MUX_Y};
    for (size_t i = 0; i < 4; i++) {
        odin3_vec_clear(outs[i]);
        odin3_status st = odin3_fsm_ix_port_sigs(ix, (odin3_node_id){node}, ports[i], outs[i]);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* The reset state guessed from the mux tree (:93-105): the first fully defined replacement. */
static odin3_status fs_guess_reset(fs_state *fs) {
    uint32_t width = (uint32_t)fs->xc->dff_in.len;
    uint8_t *rst = fs->reset_state;
    bool undef = true;
    for (uint32_t i = 0; i < width; i++) {
        undef = undef && (rst[i] == ODIN3_FSM_BIT_X || rst[i] == ODIN3_FSM_BIT_Z);
    }
    if (!undef) {
        return ODIN3_OK;
    }
    const odin3_vec *from = NULL;
    if (all_defined(&fs->aa)) {
        from = &fs->aa;
    } else if (all_defined(&fs->bb)) {
        from = &fs->bb; /* the whole concatenation: its first `width` bits are slice 0 */
    }
    if (from != NULL) {
        code_of(from->data, width, rst);
        odin3_log(ODIN3_LOG_DEBUG, "fsm: found reset state (guessed from mux tree)");
    }
    return ODIN3_OK;
}

/* The control inputs: every select bit not yet in ctrl_in, in order (:107-112). */
static odin3_status fs_add_ctrl(fs_state *fs) {
    const odin3_fsm_sig *sel = fs->s_sig.data;
    for (size_t i = 0; i < fs->s_sig.len; i++) {
        if (!odin3_fsm_sigvec_has(&fs->xc->inst->ctrl_in, sel[i])) {
            odin3_status st = odin3_kit_push_u32(&fs->xc->inst->ctrl_in, sel[i]);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

/* Remembers the mux node for the provenance record. */
static odin3_status fs_note_mux(fs_state *fs, uint32_t node) {
    const uint32_t *cells = fs->xc->inst->mux_cells.data;
    for (size_t i = 0; i < fs->xc->inst->mux_cells.len; i++) {
        if (cells[i] == node) {
            return ODIN3_OK;
        }
    }
    return odin3_kit_push_u32(&fs->xc->inst->mux_cells, node);
}

/* sig_aa, sig_bb of the mux in hand: cur with Y replaced by A, and by each B slice. */
static odin3_status fs_replacements(fs_state *fs) {
    odin3_status st = copy_vec(&fs->aa, &fs->cur);
    replace_bits(&fs->aa, &fs->y_sig, &fs->a_sig);
    size_t slices = fs->a_sig.len > 0 ? fs->b_sig.len / fs->a_sig.len : 0;
    odin3_vec_clear(&fs->bb);
    for (size_t i = 0; st == ODIN3_OK && i < slices; i++) {
        st = copy_vec(&fs->pat, &fs->cur);
        odin3_vec slice;
        odin3_vec_init(&slice, sizeof(odin3_fsm_sig));
        const odin3_fsm_sig *bits = fs->b_sig.data;
        for (size_t k = 0; st == ODIN3_OK && k < fs->a_sig.len; k++) {
            st = odin3_kit_push_u32(&slice, bits[i * fs->a_sig.len + k]);
        }
        if (st == ODIN3_OK) {
            replace_bits(&fs->pat, &fs->y_sig, &slice);
            st = append_vec(&fs->bb, &fs->pat);
        }
        odin3_vec_free(&slice);
    }
    return st;
}

/* Pushes the children so that A pops first, then the B slices in order. */
static odin3_status fs_push_children(fs_state *fs) {
    size_t width = fs->xc->dff_in.len;
    const odin3_fsm_sig *bb = fs->bb.data;
    for (size_t i = fs->bb.len / width; i-- > 0;) {
        for (size_t k = 0; k < width; k++) {
            odin3_status st = odin3_kit_push_u32(&fs->tasks, bb[i * width + k]);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return append_vec(&fs->tasks, &fs->aa);
}

/* One mux of the selection tree; false (with fs->why) when the tree is not closed. */
static odin3_status fs_visit_mux(fs_state *fs, bool root, bool *ok) {
    odin3_vec pairs;
    odin3_vec_init(&pairs, sizeof(uint32_t));
    uint32_t count = 0;
    const odin3_fsm_mod *fm = fs->xc->fm;
    odin3_status st = odin3_fsm_ix_pairs(
        &fm->ix, sigs_of(&fs->cur), ODIN3_FSM_REF_OUT | ODIN3_FSM_PAIRS_STRICT, &pairs, &count);
    *ok = false;
    if (st == ODIN3_OK && count == 1) {
        /* several drivers, none, or a cell that is no mux: the tree is not closed (:68-85) */
        const uint32_t *pair = odin3_fsm_pair_at(&pairs, 0, 0);
        *ok = is_mux(fm, pair[0]) && pair[1] == ODIN3_FSM_P_MUX_Y;
        st = *ok ? fs_load_mux(fs, pair[0]) : ODIN3_OK;
        st = st == ODIN3_OK && *ok ? fs_note_mux(fs, pair[0]) : st;
    }
    odin3_vec_free(&pairs);
    if (st == ODIN3_OK && *ok) {
        st = fs_replacements(fs);
        st = st == ODIN3_OK && root ? fs_guess_reset(fs) : st;
        st = st == ODIN3_OK ? fs_add_ctrl(fs) : st;
    }
    return st;
}

/* Walks the selection tree from dff_in: states, control inputs, the guessed reset state. */
static odin3_status find_states(odin3_fsm_xctx *xc, uint8_t *reset_state, bool *closed) {
    fs_state fs;
    odin3_status st = fs_init(&fs, xc, reset_state);
    bool root = true;
    *closed = true;
    if (st == ODIN3_OK) {
        st = append_vec(&fs.tasks, &xc->dff_in);
    }
    while (st == ODIN3_OK && *closed && fs.tasks.len > 0) {
        st = fs_pop_task(&fs);
        if (st != ODIN3_OK || vec_equal(&fs.cur, &xc->inst->dff_out)) {
            root = false;
            continue;
        }
        if (all_const(&fs.cur)) {
            st = fs_constant(&fs);
        } else {
            bool ok = false;
            st = fs_visit_mux(&fs, root, &ok);
            *closed = ok;
            st = st == ODIN3_OK && ok ? fs_push_children(&fs) : st;
        }
        root = false;
    }
    fs_free(&fs);
    return st;
}

/* --- the register (X-1) ----------------------------------------------------------------------- */

static odin3_fsm_sig pin0_sig(const odin3_fsm_index *ix, odin3_node_id node, uint32_t port) {
    odin3_pinslice pins = odin3_node_port(ix->module, node, port);
    return pins.count > 0 ? odin3_fsm_ix_pin_sig(ix, pins.first)
                          : odin3_fsm_sig_of_bit(ODIN3_FSM_BIT_X);
}

static bool param_true(const odin3_fsm_index *ix, odin3_node_id node, uint32_t index) {
    const odin3_value *val = odin3_node_param(ix->module, node, index);
    return val != NULL && val->i != 0;
}

/* Copies ARST_VALUE (parameter 3 of $adff) into the reset code. */
static void read_arst_value(const odin3_fsm_index *ix, odin3_node_id node, uint8_t *reset_state,
                            uint32_t width) {
    const odin3_value *val = odin3_node_param(ix->module, node, 3);
    for (uint32_t i = 0; val != NULL && val->bits != NULL && i < width && i < val->len; i++) {
        reset_state[i] = val->bits[i];
    }
}

/* The clock and the reset of the register in hand (:262-289). */
static void read_clock_reset(odin3_fsm_xctx *xc, odin3_node_id node, uint8_t *reset_state) {
    const odin3_fsm_index *ix = &xc->fm->ix;
    odin3_fsm_inst *inst = xc->inst;
    bool is_adff = is_type(xc->fm, node.v, ix->types.adff);
    inst->clk = pin0_sig(ix, node, ODIN3_FSM_P_DFF_CLK); /* CLK is port 0 of both */
    inst->clk_pol = param_true(ix, node, 1);
    inst->arst = odin3_fsm_sig_of_bit(ODIN3_FSM_BIT_0);
    inst->arst_pol = true;
    if (is_adff) {
        inst->arst = pin0_sig(ix, node, ODIN3_FSM_P_ADFF_ARST);
        inst->arst_pol = param_true(ix, node, 2);
        read_arst_value(ix, node, reset_state, (uint32_t)xc->dff_in.len);
    }
}

/* dff_in[j] = D[i] where Q[j] is dff_out[i] (SigSpec::replace with an `other`). */
static odin3_status set_dff_in(odin3_fsm_xctx *xc, const odin3_vec *q_sig, const odin3_vec *d_sig) {
    const odin3_fsm_sig *out = xc->inst->dff_out.data;
    const odin3_fsm_sig *q_bits = q_sig->data;
    const odin3_fsm_sig *d_bits = d_sig->data;
    odin3_fsm_sig *in = xc->dff_in.data;
    for (size_t i = 0; i < xc->inst->dff_out.len; i++) {
        for (size_t j = 0; j < q_sig->len && j < xc->dff_in.len; j++) {
            if (!odin3_fsm_sig_is_const(out[i]) && q_bits[j] == out[i] && i < d_sig->len) {
                in[j] = d_bits[i];
            }
        }
    }
    return ODIN3_OK;
}

/* A $dff or $adff driving the wire through Q. */
typedef struct reg_ref {
    uint32_t node;
    bool found, adff;
} reg_ref;

/* The first register of the wire's drivers that is a $dff or $adff with the wire on Q (:262-268).
 */
static odin3_status find_register(const odin3_fsm_xctx *xc, reg_ref *reg) {
    const odin3_fsm_mod *fm = xc->fm;
    odin3_vec pairs;
    odin3_vec_init(&pairs, sizeof(uint32_t));
    uint32_t count = 0;
    odin3_status st =
        odin3_fsm_ix_pairs(&fm->ix, sigs_of(&xc->inst->dff_out),
                           ODIN3_FSM_REF_OUT | ODIN3_FSM_PAIRS_STRICT, &pairs, &count);
    memset(reg, 0, sizeof *reg);
    for (uint32_t i = 0; st == ODIN3_OK && i < count && !reg->found; i++) {
        const uint32_t *pair = odin3_fsm_pair_at(&pairs, 0, i);
        bool adff = is_type(fm, pair[0], fm->ix.types.adff);
        bool dff = is_type(fm, pair[0], fm->ix.types.dff);
        bool q_port = pair[1] == (adff ? ODIN3_FSM_P_ADFF_Q : ODIN3_FSM_P_DFF_Q);
        if ((adff || dff) && q_port) {
            reg->node = pair[0];
            reg->adff = adff;
            reg->found = true;
        }
    }
    odin3_vec_free(&pairs);
    return st;
}

/* Reads the register's Q and D, its clock and reset, and sets dff_in. */
static odin3_status load_register(odin3_fsm_xctx *xc, reg_ref reg, uint8_t *reset_state) {
    const odin3_fsm_index *ix = &xc->fm->ix;
    odin3_node_id node = {reg.node};
    odin3_vec q_sig;
    odin3_vec d_sig;
    odin3_vec_init(&q_sig, sizeof(odin3_fsm_sig));
    odin3_vec_init(&d_sig, sizeof(odin3_fsm_sig));
    odin3_status st =
        odin3_fsm_ix_port_sigs(ix, node, reg.adff ? ODIN3_FSM_P_ADFF_Q : ODIN3_FSM_P_DFF_Q, &q_sig);
    if (st == ODIN3_OK) {
        st = odin3_fsm_ix_port_sigs(ix, node, reg.adff ? ODIN3_FSM_P_ADFF_D : ODIN3_FSM_P_DFF_D,
                                    &d_sig);
    }
    if (st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_DEBUG, "fsm: found %s cell for state register",
                  reg.adff ? "$adff" : "$dff");
        xc->inst->dff_node = reg.node;
        read_clock_reset(xc, node, reset_state);
        st = set_dff_in(xc, &q_sig, &d_sig);
    }
    odin3_vec_free(&q_sig);
    odin3_vec_free(&d_sig);
    return st;
}

/* Finds the register that drives the wire and sets clk, arst, the reset value and dff_in. */
static odin3_status read_register(odin3_fsm_xctx *xc, uint8_t *reset_state) {
    reg_ref reg;
    odin3_status st = find_register(xc, &reg);
    return st == ODIN3_OK && reg.found ? load_register(xc, reg, reset_state) : st;
}

static bool has_marked_bits(const odin3_vec *vec) {
    const odin3_fsm_sig *bits = vec->data;
    for (size_t i = 0; i < vec->len; i++) {
        if (bits[i] == odin3_fsm_sig_of_bit(ODIN3_FSM_BIT_MARK)) {
            return true;
        }
    }
    return false;
}

/* --- control outputs (X-3) ---------------------------------------------------------------------
 */

/* Adds the Y of a reader of Q that has a 1-bit Y and a constant other operand (:318-334). */
static odin3_status add_trigger(odin3_fsm_xctx *xc, const uint32_t *pair) {
    const odin3_fsm_index *ix = &xc->fm->ix;
    odin3_celltype_id type = {odin3_fsm_ix_type(ix, pair[0])};
    const odin3_celltype_def *def = odin3_celltype_get(ix->design, type);
    uint32_t port_a = odin3_kit_port_index(def, "A");
    uint32_t port_b = odin3_kit_port_index(def, "B");
    uint32_t port_y = odin3_kit_port_index(def, "Y");
    odin3_node_id node = {pair[0]};
    if (port_y == ODIN3_KIT_NONE || odin3_node_port(ix->module, node, port_y).count != 1 ||
        (pair[1] != port_a && pair[1] != port_b)) {
        return ODIN3_OK;
    }
    odin3_vec a_sig;
    odin3_vec b_sig;
    odin3_vec_init(&a_sig, sizeof(odin3_fsm_sig));
    odin3_vec_init(&b_sig, sizeof(odin3_fsm_sig));
    odin3_status st = ODIN3_OK;
    if (port_a != ODIN3_KIT_NONE) {
        st = odin3_fsm_ix_port_sigs(ix, node, port_a, &a_sig);
    }
    if (st == ODIN3_OK && port_b != ODIN3_KIT_NONE) {
        st = odin3_fsm_ix_port_sigs(ix, node, port_b, &b_sig);
    }
    bool skip =
        (pair[1] == port_a && !all_const(&b_sig)) || (pair[1] == port_b && !all_const(&a_sig));
    if (st == ODIN3_OK && !skip) {
        st = odin3_fsm_ix_port_sigs(ix, node, port_y, &xc->inst->ctrl_out);
    }
    odin3_vec_free(&a_sig);
    odin3_vec_free(&b_sig);
    return st;
}

/* Removes from ctrl_in the (non-constant) bits that are in ctrl_out. */
static void remove_outputs_from_inputs(odin3_fsm_inst *inst) {
    odin3_fsm_sig *bits = inst->ctrl_in.data;
    size_t kept = 0;
    for (size_t i = 0; i < inst->ctrl_in.len; i++) {
        if (odin3_fsm_sig_is_const(bits[i]) || !odin3_fsm_sigvec_has(&inst->ctrl_out, bits[i])) {
            bits[kept++] = bits[i];
        }
    }
    odin3_kit_truncate(&inst->ctrl_in, kept);
}

static odin3_status find_ctrl_out(odin3_fsm_xctx *xc) {
    odin3_fsm_inst *inst = xc->inst;
    odin3_status st = copy_vec(&inst->ctrl_out, &xc->dff_in);
    odin3_vec pairs;
    odin3_vec_init(&pairs, sizeof(uint32_t));
    uint32_t count = 0;
    if (st == ODIN3_OK) {
        st = odin3_fsm_ix_pairs(&xc->fm->ix, sigs_of(&inst->dff_out),
                                ODIN3_FSM_REF_IN | ODIN3_FSM_PAIRS_STRICT, &pairs, &count);
    }
    for (uint32_t i = 0; st == ODIN3_OK && i < count; i++) {
        st = add_trigger(xc, odin3_fsm_pair_at(&pairs, 0, i));
    }
    odin3_vec_free(&pairs);
    remove_outputs_from_inputs(inst);
    odin3_fsm_sigvec_sort_unify(&inst->ctrl_in);
    odin3_fsm_sigvec_sort_unify(&inst->ctrl_out);
    return st;
}

/* The nodes that drive the control outputs, ascending (provenance parents, P-2). */
static odin3_status find_out_drivers(odin3_fsm_xctx *xc) {
    odin3_fsm_inst *inst = xc->inst;
    odin3_vec pairs;
    odin3_vec_init(&pairs, sizeof(uint32_t));
    uint32_t count = 0;
    odin3_status st =
        odin3_fsm_ix_pairs(&xc->fm->ix, sigs_of(&inst->ctrl_out),
                           ODIN3_FSM_REF_OUT | ODIN3_FSM_PAIRS_STRICT, &pairs, &count);
    for (uint32_t i = 0; st == ODIN3_OK && i < count; i++) {
        uint32_t node = odin3_fsm_pair_at(&pairs, 0, i)[0];
        const uint32_t *have = inst->out_drivers.data;
        bool seen = inst->out_drivers.len > 0 && have[inst->out_drivers.len - 1] == node;
        st = seen ? ODIN3_OK : odin3_kit_push_u32(&inst->out_drivers, node);
    }
    odin3_vec_free(&pairs);
    return st;
}

static int cmp_node(const void *lhs, const void *rhs) {
    uint32_t lhs_v = *(const uint32_t *)lhs;
    uint32_t rhs_v = *(const uint32_t *)rhs;
    return (lhs_v > rhs_v) - (lhs_v < rhs_v);
}

static void sort_nodes(odin3_vec *vec) {
    if (vec->len > 0) {
        qsort(vec->data, vec->len, sizeof(uint32_t), cmp_node);
    }
}

/* --- one wire ----------------------------------------------------------------------------------
 */

/* Numbers the states in the Const order and finds the reset state (:301-317). */
static odin3_status init_data(odin3_fsm_xctx *xc, const uint8_t *reset_state) {
    odin3_fsm_inst *inst = xc->inst;
    odin3_fsm_data *data = &inst->data;
    data->num_inputs = (int32_t)inst->ctrl_in.len;
    data->num_outputs = (int32_t)inst->ctrl_out.len;
    data->state_bits = (int32_t)xc->dff_in.len;
    data->reset_state = -1;
    odin3_fsm_state_ent *ents = xc->states.data;
    for (size_t i = 0; i < xc->states.len; i++) {
        ents[i].idx = (int32_t)i;
        odin3_status st = odin3_fsm_data_add_state(data, ents[i].bits);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    bool rst_const = odin3_fsm_sig_is_const(inst->arst);
    bool defined = true;
    for (size_t i = 0; i < xc->dff_in.len; i++) {
        defined = defined && odin3_fsm_bit_defined(reset_state[i]);
    }
    if (!rst_const || defined) {
        const odin3_fsm_state_ent *ent = odin3_fsm_xctx_find_state(xc, reset_state);
        data->reset_state = ent != NULL ? ent->idx : -1;
    }
    return ODIN3_OK;
}

/* The extraction proper, after the register is known; *res says how it ended. */
static odin3_status extract_body(odin3_fsm_xctx *xc, uint8_t *reset_state, odin3_fsm_xres *res) {
    const odin3_fsm_inst *inst = xc->inst;
    const odin3_fsm_mod *fm = xc->fm;
    bool closed = true;
    *res = ODIN3_FSM_X_FAILED;
    if (!odin3_fsm_sig_is_const(inst->arst)) {
        odin3_status st = add_state(xc, reset_state);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    odin3_status st = find_states(xc, reset_state, &closed);
    if (st != ODIN3_OK) {
        return st;
    }
    if (!closed) {
        fail_log(fm, inst->wire, "state selection tree is not closed");
        return ODIN3_OK;
    }
    if (xc->states.len <= 1) {
        fail_log(fm, inst->wire, "at least two states are required");
        return ODIN3_OK;
    }
    st = find_ctrl_out(xc);
    st = st == ODIN3_OK ? find_out_drivers(xc) : st;
    st = st == ODIN3_OK ? init_data(xc, reset_state) : st;
    if (st == ODIN3_OK) {
        st = odin3_fsm_extract_transitions(xc, res);
    }
    return st;
}

/* The INFO line of a good extraction, the WARN of a capped one, the INFO of an unusable one. */
static void report(const odin3_fsm_mod *fm, const odin3_fsm_inst *inst, odin3_fsm_xres res) {
    char label[LABEL_MAX];
    label_of(fm, inst->wire, label, sizeof label);
    if (res == ODIN3_FSM_X_OK) {
        odin3_log(ODIN3_LOG_INFO,
                  "fsm: extracting '%s': %zu states, %zu control inputs, %zu control outputs",
                  label, inst->data.states.len, inst->ctrl_in.len, inst->ctrl_out.len);
    } else if (res == ODIN3_FSM_X_CAPPED) {
        odin3_log(ODIN3_LOG_WARN,
                  "fsm: module '%s': state register '%s': giving up after %llu evaluations; not "
                  "recoded",
                  odin3_fsm_mod_name(fm), label + strlen(odin3_fsm_mod_name(fm)) + 1,
                  (unsigned long long)(fm->opts->max_evals != 0 ? fm->opts->max_evals
                                                                : (uint64_t)ODIN3_FSM_MAX_EVALS));
    }
}

/* The setup of one extraction: dff_out, the all-marked dff_in, the reset code, the register. */
static odin3_status extract_setup(odin3_fsm_xctx *xc, odin3_wire_id wire, uint8_t **reset_state) {
    odin3_fsm_inst *inst = xc->inst;
    inst->wire = wire;
    odin3_vec_init(&xc->dff_in, sizeof(odin3_fsm_sig));
    odin3_vec_init(&xc->states, sizeof(odin3_fsm_state_ent));
    odin3_status st = odin3_fsm_wire_sigs(xc->fm, wire, &inst->dff_out);
    for (size_t i = 0; st == ODIN3_OK && i < inst->dff_out.len; i++) {
        st = odin3_kit_push_u32(&xc->dff_in, odin3_fsm_sig_of_bit(ODIN3_FSM_BIT_MARK));
    }
    *reset_state = odin3_util_malloc(inst->dff_out.len + 1);
    if (st == ODIN3_OK && *reset_state == NULL) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    if (st == ODIN3_OK) {
        memset(*reset_state, ODIN3_FSM_BIT_X, inst->dff_out.len);
        (void)snprintf(inst->encoding, sizeof inst->encoding, "%s",
                       odin3_fsm_wire_encoding(xc->fm, wire));
        st = read_register(xc, *reset_state);
    }
    return st;
}

/* The extraction of one wire into xc->inst; *res says how it ended. */
static odin3_status extract_into(odin3_fsm_xctx *xc, odin3_wire_id wire, odin3_fsm_xres *res) {
    const odin3_fsm_mod *fm = xc->fm;
    uint8_t *reset_state = NULL;
    *res = ODIN3_FSM_X_FAILED;
    odin3_status st = extract_setup(xc, wire, &reset_state);
    xc->budget.max = fm->opts->max_evals != 0 ? fm->opts->max_evals : (uint64_t)ODIN3_FSM_MAX_EVALS;
    if (st == ODIN3_OK && has_marked_bits(&xc->dff_in)) {
        fail_log(fm, wire, "incomplete input selection tree root");
    } else if (st == ODIN3_OK) {
        st = extract_body(xc, reset_state, res);
    }
    odin3_util_free(reset_state);
    odin3_vec_free(&xc->dff_in);
    odin3_vec_free(&xc->states);
    sort_nodes(&xc->inst->mux_cells);
    if (st == ODIN3_OK && xc->inst->data.states.len > 0) {
        report(fm, xc->inst, *res);
    }
    return st;
}

/* extract_fsm(wire): X-1 to X-6. The instance stays in insts only on success. */
static odin3_status extract_wire(odin3_fsm_xctx *xc, odin3_wire_id wire, odin3_vec *insts) {
    odin3_fsm_inst *slot = odin3_vec_push(insts);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_status st = inst_init(slot);
    odin3_fsm_xres res = ODIN3_FSM_X_FAILED;
    xc->inst = slot;
    if (st == ODIN3_OK) {
        st = extract_into(xc, wire, &res);
    }
    xc->inst = NULL;
    if (st != ODIN3_OK || res != ODIN3_FSM_X_OK) {
        odin3_fsm_inst_free(slot);
        odin3_vec_pop(insts);
    }
    return st;
}

/* --- the module --------------------------------------------------------------------------------
 */

/* Adds the (bit, other) keys of every ordered pair of different select bits of one $pmux. */
static odin3_status add_exclusive_keys(const odin3_vec *sel, odin3_vec *excl) {
    const odin3_fsm_sig *bits = sel->data;
    for (size_t i = 0; i < sel->len; i++) {
        for (size_t j = 0; j < sel->len; j++) {
            odin3_status st = bits[i] != bits[j]
                                  ? odin3_kit_push_u64(excl, odin3_fsm_excl_key(bits[i], bits[j]))
                                  : ODIN3_OK;
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

/* The pmux select bits that exclude each other (:463-469). */
static odin3_status build_exclusive(const odin3_fsm_mod *fm, odin3_vec *excl) {
    const odin3_fsm_index *ix = &fm->ix;
    for (uint32_t node = 1; node < ix->node_end; node++) {
        if (!is_type(fm, node, ix->types.pmux)) {
            continue;
        }
        odin3_vec sel;
        odin3_vec_init(&sel, sizeof(odin3_fsm_sig));
        odin3_status st =
            odin3_fsm_ix_port_sigs(ix, (odin3_node_id){node}, ODIN3_FSM_P_MUX_S, &sel);
        st = st == ODIN3_OK ? add_exclusive_keys(&sel, excl) : st;
        odin3_vec_free(&sel);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    odin3_kit_sort_unify(excl, 0);
    return ODIN3_OK;
}

odin3_status odin3_fsm_extract_module(odin3_fsm_mod *fm, odin3_vec *insts) {
    odin3_vec excl;
    odin3_vec_init(&excl, sizeof(uint64_t));
    odin3_status st = build_exclusive(fm, &excl);
    odin3_fsm_xctx xc;
    memset(&xc, 0, sizeof xc);
    xc.fm = fm;
    xc.excl = &excl;
    for (uint32_t id = 1; st == ODIN3_OK && id < odin3_module_wire_end(fm->module); id++) {
        odin3_wire_id wire = {id};
        const char *enc =
            odin3_wire_live(fm->module, wire) ? odin3_fsm_wire_encoding(fm, wire) : NULL;
        if (enc != NULL && strcmp(enc, "none") != 0) {
            st = extract_wire(&xc, wire, insts);
        }
    }
    odin3_vec_free(&excl);
    return st;
}
