/*
 * unused.c — the native "unread and not a port" query that stands for Yosys's `unused_bits`
 * attribute (spec O-1, PHASE4 decision 12 question 2; opt_clean.cc:476-491 sets the attribute from
 * rmunused_module_cells and rmunused_module_signals).
 *
 * The liveness is opt_clean's (clean.c): roots are the module's ports, the nodes and wires with
 * `keep` and the instances of modules with `keep`; a live node makes live the drivers of the nets
 * it reads; a live $memrd keeps the $memwr of its memory. The FSMs stand in for the old logic:
 * the drivers of a control output are the FSM, which reads its control inputs, clock and reset;
 * the old register and tree are live only if something else reads them. A net is *used* when a
 * live node reads it, or it is a module output (inout) or a keep wire's.
 */
#include "ir/celltype.h"
#include "ir/ids.h"
#include "ir/module.h"
#include "odin3/odin3.h"
#include "passes/fsm/fsm.h"
#include "passes/fsm/index.h"
#include "passes/opt/kit.h"
#include "passes/opt/opt.h"
#include "util/alloc.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct live {
    const odin3_fsm_mod *fm;
    const odin3_vec *insts;
    uint8_t *used;      /* per net */
    uint8_t *node_live; /* per node */
    uint8_t *fsm_live;  /* per FSM */
    uint32_t *owner;    /* per net: FSM index + 1 whose control output it is */
    odin3_vec stack;    /* work items: node ID, or FSM index | FSM_BIT */
    odin3_opt_ctx view; /* keep queries */
    uint8_t *keep_mod;  /* per module ID */
} live;

#define FSM_BIT 0x80000000U

static void live_free(live *lv) {
    odin3_util_free(lv->used);
    odin3_util_free(lv->node_live);
    odin3_util_free(lv->fsm_live);
    odin3_util_free(lv->owner);
    odin3_util_free(lv->keep_mod);
    odin3_vec_free(&lv->stack);
}

static const odin3_fsm_inst *inst_at(const live *lv, size_t idx) {
    return (const odin3_fsm_inst *)lv->insts->data + idx;
}

static odin3_status push_work(live *lv, uint32_t item) {
    return odin3_kit_push_u32(&lv->stack, item);
}

static odin3_status make_node_live(live *lv, uint32_t node) {
    if (node == 0 || lv->node_live[node] != 0) {
        return ODIN3_OK;
    }
    lv->node_live[node] = 1;
    return push_work(lv, node);
}

static odin3_status make_fsm_live(live *lv, uint32_t idx) {
    if (lv->fsm_live[idx] != 0) {
        return ODIN3_OK;
    }
    lv->fsm_live[idx] = 1;
    return push_work(lv, idx | FSM_BIT);
}

/* The drivers of net become live: the FSM that owns it, else the nodes with an OUT reference. */
static odin3_status wake_drivers(live *lv, uint32_t net) {
    if (lv->owner[net] != 0) {
        return make_fsm_live(lv, lv->owner[net] - 1U);
    }
    odin3_fsm_refs span = odin3_fsm_ix_refs(&lv->fm->ix, net);
    for (uint32_t k = 0; k < span.count; k++) {
        if ((span.refs[k].flags & (ODIN3_FSM_REF_OUT | ODIN3_FSM_REF_UNKNOWN)) != 0) {
            odin3_status st = make_node_live(lv, span.refs[k].node);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

/* A signal bit is read by a live consumer: it is used, and its drivers are needed. */
static odin3_status read_sig(live *lv, odin3_fsm_sig sig) {
    if (odin3_fsm_sig_is_const(sig) || lv->used[sig] != 0) {
        return ODIN3_OK;
    }
    lv->used[sig] = 1;
    return wake_drivers(lv, sig);
}

/* A live node reads the nets on its input pins (all pins of an unknown node). */
static odin3_status expand_node(live *lv, uint32_t node) {
    const odin3_fsm_mod *fm = lv->fm;
    odin3_pinslice pins = odin3_node_pins(fm->module, (odin3_node_id){node});
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        if (!odin3_pin_reads(fm->module, pin)) {
            continue;
        }
        odin3_status st = read_sig(lv, odin3_fsm_ix_pin_sig(&fm->ix, pin));
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* A live FSM reads its control inputs, clock and reset. */
static odin3_status expand_fsm(live *lv, uint32_t idx) {
    const odin3_fsm_inst *inst = inst_at(lv, idx);
    const odin3_fsm_sig *in = inst->ctrl_in.data;
    odin3_status st = read_sig(lv, inst->clk);
    st = st == ODIN3_OK ? read_sig(lv, inst->arst) : st;
    for (size_t i = 0; st == ODIN3_OK && i < inst->ctrl_in.len; i++) {
        st = read_sig(lv, in[i]);
    }
    return st;
}

/* A live $memrd keeps every $memwr of its memory (clean.c mark_memwr). */
static odin3_status wake_memwr(live *lv, uint32_t memrd) {
    const odin3_fsm_mod *fm = lv->fm;
    const odin3_value *want = odin3_node_param(fm->module, (odin3_node_id){memrd}, 0);
    odin3_celltype_id memwr = odin3_kit_type_named(fm->design, "$memwr");
    if (want == NULL || want->kind != ODIN3_VAL_STRING || memwr.v == 0) {
        return ODIN3_OK;
    }
    for (uint32_t node = 1; node < fm->ix.node_end; node++) {
        const odin3_value *have = NULL;
        if (odin3_fsm_ix_type(&fm->ix, node) == memwr.v) {
            have = odin3_node_param(fm->module, (odin3_node_id){node}, 0);
        }
        if (have != NULL && have->kind == ODIN3_VAL_STRING && have->str == want->str) {
            odin3_status st = make_node_live(lv, node);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

static odin3_status drain(live *lv) {
    odin3_celltype_id memrd = odin3_kit_type_named(lv->fm->design, "$memrd");
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && lv->stack.len > 0) {
        uint32_t item = ((const uint32_t *)lv->stack.data)[lv->stack.len - 1];
        odin3_kit_truncate(&lv->stack, lv->stack.len - 1);
        if ((item & FSM_BIT) != 0) {
            st = expand_fsm(lv, item & ~FSM_BIT);
            continue;
        }
        st = expand_node(lv, item);
        if (st == ODIN3_OK && memrd.v != 0 && odin3_fsm_ix_type(&lv->fm->ix, item) == memrd.v) {
            st = wake_memwr(lv, item);
        }
    }
    return st;
}

/* --- the roots ------------------------------------------------------------------------------- */

/* The modules that have `keep`, or hold something with it, or instantiate one that does. */
static odin3_status keep_modules(live *lv) {
    odin3_vec flags;
    odin3_vec_init(&flags, sizeof(bool));
    odin3_status st = odin3_opt_keep_modules(lv->fm->design, &flags);
    uint32_t count = odin3_design_module_end(lv->fm->design);
    lv->keep_mod = odin3_util_calloc((size_t)count + 1);
    if (st == ODIN3_OK && lv->keep_mod == NULL) {
        st = ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t id = 1; st == ODIN3_OK && id < count; id++) {
        lv->keep_mod[id] = ((const bool *)flags.data)[id] ? 1 : 0;
    }
    odin3_vec_free(&flags);
    return st;
}

/* True when node is an instance of a module with keep. */
static bool instance_of_keep(const live *lv, uint32_t node) {
    odin3_celltype_id type = {odin3_fsm_ix_type(&lv->fm->ix, node)};
    for (uint32_t id = 1; id < odin3_design_module_end(lv->fm->design); id++) {
        const odin3_module *module = odin3_module_get(lv->fm->design, (odin3_module_id){id});
        if (odin3_module_celltype(module).v == type.v && lv->keep_mod[id] != 0) {
            return true;
        }
    }
    return false;
}

/* A module output pin reads its net for good; port nodes and keep nodes are roots. */
static odin3_status node_roots(live *lv) {
    const odin3_fsm_mod *fm = lv->fm;
    for (uint32_t node = 1; node < fm->ix.node_end; node++) {
        if (!odin3_node_live(fm->module, (odin3_node_id){node})) {
            continue;
        }
        const odin3_celltype_def *def =
            odin3_celltype_get(fm->design, odin3_node_type(fm->module, (odin3_node_id){node}));
        bool port = def != NULL && def->gran == ODIN3_GRAN_PORT;
        odin3_objref obj = {ODIN3_OBJ_NODE, node};
        odin3_status st = ODIN3_OK;
        if (port) {
            st = expand_node(lv, node); /* a $port_out reads its net; a $port_in has no input pin */
        } else if (odin3_kit_keep(&lv->view, obj) || instance_of_keep(lv, node)) {
            st = make_node_live(lv, node);
        }
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* The nets of a keep wire are used (and their drivers live). */
static odin3_status wire_roots(live *lv) {
    const odin3_fsm_mod *fm = lv->fm;
    for (uint32_t id = 1; id < odin3_module_wire_end(fm->module); id++) {
        odin3_wire_id wire = {id};
        odin3_objref obj = {ODIN3_OBJ_WIRE, id};
        if (!odin3_wire_live(fm->module, wire) || !odin3_kit_keep(&lv->view, obj)) {
            continue;
        }
        for (uint32_t bit = 0; bit < odin3_wire_width(fm->module, wire); bit++) {
            odin3_status st =
                read_sig(lv, odin3_fsm_ix_sig(&fm->ix, odin3_wire_net(fm->module, wire, bit).v));
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

static odin3_status mark_owners(live *lv) {
    for (size_t idx = 0; idx < lv->insts->len; idx++) {
        const odin3_fsm_inst *inst = inst_at(lv, idx);
        const odin3_fsm_sig *out = inst->ctrl_out.data;
        for (size_t i = 0; i < inst->ctrl_out.len; i++) {
            if (!odin3_fsm_sig_is_const(out[i])) {
                lv->owner[out[i]] = (uint32_t)idx + 1U;
            }
        }
    }
    return ODIN3_OK;
}

odin3_status odin3_fsm_used_nets(const odin3_fsm_mod *fm, const odin3_vec *insts, uint8_t **used) {
    live lv;
    memset(&lv, 0, sizeof lv);
    lv.fm = fm;
    lv.insts = insts;
    odin3_vec_init(&lv.stack, sizeof(uint32_t));
    lv.view.design = fm->design;
    lv.view.module = fm->module;
    lv.used = odin3_util_calloc((size_t)fm->ix.net_end + 1);
    lv.node_live = odin3_util_calloc((size_t)fm->ix.node_end + 1);
    lv.fsm_live = odin3_util_calloc(insts->len + 1);
    lv.owner = odin3_util_calloc(((size_t)fm->ix.net_end + 1) * sizeof *lv.owner);
    *used = NULL;
    odin3_status st =
        lv.used != NULL && lv.node_live != NULL && lv.fsm_live != NULL && lv.owner != NULL
            ? ODIN3_OK
            : ODIN3_ERR_NO_MEMORY;
    st = st == ODIN3_OK ? keep_modules(&lv) : st;
    st = st == ODIN3_OK ? mark_owners(&lv) : st;
    st = st == ODIN3_OK ? node_roots(&lv) : st;
    st = st == ODIN3_OK ? wire_roots(&lv) : st;
    st = st == ODIN3_OK ? drain(&lv) : st;
    if (st == ODIN3_OK) {
        *used = lv.used;
        lv.used = NULL;
    }
    live_free(&lv);
    return st;
}

odin3_status odin3_fsm_unused_flags(const odin3_fsm_inst *inst, const uint8_t *used,
                                    odin3_vec *flags) {
    const odin3_fsm_sig *out = inst->ctrl_out.data;
    odin3_vec_clear(flags);
    for (size_t i = 0; i < inst->ctrl_out.len; i++) {
        uint8_t *slot = odin3_vec_push(flags);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = !odin3_fsm_sig_is_const(out[i]) && used[out[i]] == 0 ? 1 : 0;
    }
    return ODIN3_OK;
}
