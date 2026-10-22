/*
 * splice_a.c — phase A of the splice (3D spec §6.5, AB-18; plan Task 17): the record, the fresh
 * nets, the $lut nodes, the new flip-flops, the buffers phase B needs, and the plan of phase B.
 * It allocates and changes no existing object; sp_rollback undoes it. With abc --gates (gatemap
 * spec 4.4, G-7; plan Task 12) the LUTs are library gate instances and a buffer is the library's
 * BUF cell, else `$_BUF_`.
 */
#include "ir/celltype.h"
#include "ir/lut.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "passes/abc/prov.h"
#include "passes/abc/region.h"
#include "passes/abc/run.h"
#include "passes/abc/splice_int.h"
#include "sim/gate_truth.h"
#include "techlib/libcells.h"
#include "util/arena.h"
#include "util/clock.h"
#include "util/log.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum {
    FIRST_OBJ = 2,
    PORT_Y_BUFFER = 1, /* the Y port of a $lut */
    IDENTITY_TABLE = 0x2,
    INIT_UNKNOWN = 2, /* INIT 2 and 3 are both written back as 2 (P3-12) */
    INIT_UNKNOWN_HI = 3,
    GATE_PORTS_MAX = ODIN3_GATE_TRUTH_MAX_INPUTS + 1 /* a gate cell: its inputs and one output */
};

static uint32_t obj_count(const odin3_abc_result *res) {
    return odin3_abc_result_first_lut(res) + res->n_luts;
}

/* --- the records --------------------------------------------------------------------------- */

/* Anchors and the record of every new object (§8; prov.c). Records come before objects. */
static odin3_status make_records(splice *sp) {
    double started = odin3_now_ms();
    odin3_status st = odin3_abc_anchors(sp->run, sp->res, &sp->anchors);
    st = st == ODIN3_OK ? odin3_abc_records(sp->run, sp->res, &sp->anchors, &sp->objprov) : st;
    sp->run->stats.prov_ms = odin3_now_ms() - started;
    return st;
}

/* --- nets ---------------------------------------------------------------------------------- */

/* The record of a new object: its anchor's; an object no anchor reaches (a buffer of an input, a
 * constant's buffer, restore's NOT of a constant) carries the run's coarse record, made on first
 * use. */
static odin3_status obj_prov(splice *sp, uint32_t obj, odin3_prov_id *out) {
    uint32_t own = sp_u32(&sp->objprov, obj);
    if (own != 0) {
        *out = (odin3_prov_id){own};
        return ODIN3_OK;
    }
    return sp_coarse(sp, out);
}

/* True when a CO or register input is driven by constant 0 or 1; with --gates (no folding) also
 * when a gate reads it. */
static bool uses_constant(const splice *sp, uint32_t value) {
    const odin3_abc_result *res = sp->res;
    for (size_t at = 0; at < res->co_driver.len; at++) {
        if (sp_u32(&res->co_driver, at) == value) {
            return true;
        }
    }
    for (size_t at = 0; sp->run->gates_mode && at < res->fanins.len; at++) {
        if (sp_u32(&res->fanins, at) == value) {
            return true;
        }
    }
    return false;
}

static odin3_status new_net(splice *sp, uint32_t obj, uint32_t *out) {
    odin3_net_id net = {0};
    odin3_prov_id prov = {0};
    odin3_status st = obj_prov(sp, obj, &prov);
    st = st == ODIN3_OK ? odin3_net_create(sp->mod, 0, prov, &net) : st;
    st = st == ODIN3_OK ? sp_note(sp, (sp_created){SP_NET, net.v}) : st;
    if (st != ODIN3_OK && odin3_net_live(sp->mod, net)) {
        (void)odin3_net_delete(sp->mod, net);
    }
    *out = net.v;
    return st;
}

/* The net of result object obj: a constant's canonical net (when a CO uses it), a CI's own net, a
 * fresh net for a register output or a LUT. */
static odin3_status object_net(splice *sp, uint32_t obj, odin3_net_id *net) {
    const odin3_abc_result *res = sp->res;
    *net = (odin3_net_id){0};
    if (obj < FIRST_OBJ) {
        return uses_constant(sp, obj) ? sp_canon(sp, (int)obj, net) : ODIN3_OK;
    }
    if (obj < FIRST_OBJ + res->n_cis) {
        *net = (odin3_net_id){sp_u32(&sp->region->cis, obj - FIRST_OBJ)};
        return ODIN3_OK;
    }
    return new_net(sp, obj, &net->v);
}

/* The net of every result object, and the empty node and carrier tables beside them. */
static odin3_status object_nets(splice *sp) {
    const odin3_abc_result *res = sp->res;
    odin3_status st = ODIN3_OK;
    for (uint32_t obj = 0; st == ODIN3_OK && obj < obj_count(res); obj++) {
        odin3_net_id net = {0};
        st = object_net(sp, obj, &net);
        bool is_ci = obj >= FIRST_OBJ && obj < FIRST_OBJ + res->n_cis;
        st = st == ODIN3_OK ? sp_push(&sp->objnet, net.v) : st;
        st = st == ODIN3_OK ? sp_push(&sp->objnode, 0) : st;
        st = st == ODIN3_OK ? sp_push(&sp->carried, is_ci ? net.v : 0) : st;
    }
    return st;
}

/* --- nodes --------------------------------------------------------------------------------- */

typedef struct lut_req {
    uint32_t inputs;
    const uint64_t *words;
    const odin3_net_id *ins;
    odin3_net_id out; /* none: the Y pin stays open */
    odin3_prov_id prov;
} lut_req;

static odin3_status make_lut(splice *sp, const lut_req *rq, odin3_node_id *node) {
    odin3_value params[2];
    odin3_status st = odin3_lut_params(sp->run->arena, rq->inputs, rq->words, params);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_node_spec spec = {odin3_abc_lut_type(sp->run->design), 0, rq->prov, params, 2};
    odin3_netvec ports[2] = {{rq->ins, rq->inputs}, {&rq->out, 1}};
    st = odin3_node_create_connected(sp->mod, &spec, ports, node);
    st = st == ODIN3_OK ? sp_note(sp, (sp_created){SP_NODE, node->v}) : st;
    if (st != ODIN3_OK && odin3_node_live(sp->mod, *node)) {
        (void)odin3_node_delete(sp->mod, *node);
    }
    return st;
}

/* A new cell of type with net ins[j] on port in_ports[j] (n of them) and out on out_port (none:
 * left open); every other port open. */
typedef struct cell_req {
    odin3_celltype_id type;
    uint32_t n;
    const uint32_t *in_ports;
    const odin3_net_id *ins;
    uint32_t out_port;
    odin3_net_id out;
    odin3_prov_id prov;
} cell_req;

static odin3_status make_cell(splice *sp, const cell_req *rq, odin3_node_id *node) {
    const odin3_celltype_def *def = odin3_celltype_get(sp->run->design, rq->type);
    if (def == NULL || def->n_ports > GATE_PORTS_MAX || rq->out_port >= def->n_ports) {
        odin3_log(ODIN3_LOG_ERROR, "abc: internal: gate type %u cannot be instantiated",
                  (unsigned)rq->type.v);
        return ODIN3_ERR_INVALID_ARG;
    }
    static const odin3_net_id k_none = {0};
    odin3_netvec ports[GATE_PORTS_MAX];
    for (uint32_t port = 0; port < def->n_ports; port++) {
        ports[port] = (odin3_netvec){&k_none, 1};
    }
    for (uint32_t j = 0; j < rq->n; j++) {
        if (rq->in_ports[j] >= def->n_ports) {
            odin3_log(ODIN3_LOG_ERROR, "abc: internal: gate type %u has no input port %u",
                      (unsigned)rq->type.v, (unsigned)rq->in_ports[j]);
            return ODIN3_ERR_INVALID_ARG;
        }
        ports[rq->in_ports[j]] = (odin3_netvec){&rq->ins[j], 1};
    }
    ports[rq->out_port] = (odin3_netvec){&rq->out, 1};
    odin3_node_spec spec = {rq->type, 0, rq->prov, NULL, 0};
    odin3_status st = odin3_node_create_connected(sp->mod, &spec, ports, node);
    st = st == ODIN3_OK ? sp_note(sp, (sp_created){SP_NODE, node->v}) : st;
    if (st != ODIN3_OK && odin3_node_live(sp->mod, *node)) {
        (void)odin3_node_delete(sp->mod, *node);
    }
    return st;
}

/* LUT lut's library gate (--gates): its inputs on the gate's input ports in pin order. */
static odin3_status make_gate(splice *sp, uint32_t lut, const lut_req *rq, odin3_node_id *node) {
    const odin3_abc_result *res = sp->res;
    assert(lut < res->gate.len); /* run.c checked the tags before the splice */
    const odin3_libgate *gate = odin3_vec_cat(&sp->run->lib.gates, sp_u32(&res->gate, lut));
    cell_req cr = {gate->type,     gate->n_inputs, gate->in_port, rq->ins,
                   gate->out_port, rq->out,        rq->prov};
    return make_cell(sp, &cr, node);
}

static odin3_status create_luts(splice *sp) {
    odin3_abc_result *res = sp->res;
    for (uint32_t lut = 0; lut < res->n_luts; lut++) {
        uint32_t inputs = sp_u32(&res->lut_k, lut);
        const uint32_t *fanins =
            (const uint32_t *)res->fanins.data + sp_u32(&res->fanin_start, lut);
        odin3_net_id ins[ODIN3_LUT_MAX_WIDTH];
        for (uint32_t var = 0; var < inputs; var++) {
            ins[var] = (odin3_net_id){sp_u32(&sp->objnet, fanins[var])};
        }
        uint32_t obj = odin3_abc_result_first_lut(res) + lut;
        lut_req rq = {inputs,
                      (const uint64_t *)res->truth.data + sp_u32(&res->truth_start, lut),
                      ins,
                      {sp_u32(&sp->objnet, obj)},
                      {0}};
        odin3_node_id node = {0};
        odin3_status st = obj_prov(sp, obj, &rq.prov);
        if (st == ODIN3_OK) {
            st = sp->run->gates_mode ? make_gate(sp, lut, &rq, &node) : make_lut(sp, &rq, &node);
        }
        if (st != ODIN3_OK) {
            return st;
        }
        ((uint32_t *)sp->objnode.data)[obj] = node.v;
    }
    return ODIN3_OK;
}

/* The INIT the new flip-flop gets: INIT(k*), with 3 written back as 2. */
static int64_t new_init(const odin3_value *init) {
    int64_t val = init != NULL && init->kind == ODIN3_VAL_INT ? init->i : 0;
    return val == INIT_UNKNOWN_HI ? INIT_UNKNOWN : val;
}

/* The net on the first pin of port `port` of node, or none. */
static odin3_net_id port_net(const odin3_module *mod, odin3_node_id node, uint32_t port) {
    odin3_pinslice pins = odin3_node_port(mod, node, port);
    if (pins.count == 0) {
        return (odin3_net_id){0};
    }
    return odin3_pin_net(mod, pins.first);
}

/* The net on the representative's clock pin (port 0 of a flip-flop with C, D, Q); none for $_FF_.
 */
static odin3_net_id rep_clock(const odin3_module *mod, odin3_node_id rep, uint32_t n_ports) {
    if (n_ports != 3) {
        return (odin3_net_id){0};
    }
    return port_net(mod, rep, 0);
}

static odin3_status create_ff(splice *sp, uint32_t reg) {
    const odin3_abc_result *res = sp->res;
    odin3_node_id rep = {sp_u32(&sp->region->regs, sp_u32(&res->reg_from, reg))};
    odin3_celltype_id type = odin3_node_type(sp->mod, rep);
    uint32_t obj = FIRST_OBJ + res->n_cis + reg;
    odin3_net_id data = {
        sp_u32(&sp->objnet, sp_u32(&res->co_driver, res->n_cos + res->n_obs + reg))};
    odin3_net_id qout = {sp_u32(&sp->objnet, obj)};
    uint32_t n_ports = odin3_celltype_get(sp->run->design, type)->n_ports;
    odin3_net_id clock = rep_clock(sp->mod, rep, n_ports);
    odin3_netvec ports[3];
    if (n_ports != 2 && n_ports != 3) { /* $_FF_ has D, Q; the others C, D, Q: ports[3] below */
        odin3_log(ODIN3_LOG_ERROR, "abc: flip-flop type with %u ports cannot be rebuilt",
                  (unsigned)n_ports);
        return ODIN3_ERR_INVALID_ARG;
    }
    ports[0] = (odin3_netvec){&clock, 1}; /* C, D, Q; a $_FF_ has D, Q: this slot is then D's */
    ports[n_ports - 2] = (odin3_netvec){&data, 1};
    ports[n_ports - 1] = (odin3_netvec){&qout, 1};
    odin3_value init = odin3_value_int(new_init(odin3_node_param(sp->mod, rep, 0)));
    odin3_node_spec spec = {type, 0, {0}, &init, 1};
    odin3_node_id node = {0};
    odin3_status st = obj_prov(sp, obj, &spec.prov);
    st = st == ODIN3_OK ? odin3_node_create_connected(sp->mod, &spec, ports, &node) : st;
    st = st == ODIN3_OK ? sp_note(sp, (sp_created){SP_NODE, node.v}) : st;
    st = st == ODIN3_OK ? sp_push(&sp->ffnode, node.v) : st;
    if (st != ODIN3_OK && odin3_node_live(sp->mod, node)) {
        (void)odin3_node_delete(sp->mod, node);
    }
    return st;
}

static odin3_status create_ffs(splice *sp) {
    for (uint32_t reg = 0; reg < sp->res->n_regs_out; reg++) {
        odin3_status st = create_ff(sp, reg);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* --- the old cells ------------------------------------------------------------------------- */

/* The net on the output pin of node (none when it drives nothing). */
static odin3_net_id output_net(const odin3_module *mod, odin3_node_id node) {
    odin3_pinslice pins = odin3_node_pins(mod, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_pin_id pin = {pins.first.v + i};
        if (odin3_pin_drives(mod, pin)) {
            return odin3_pin_net(mod, pin);
        }
    }
    return (odin3_net_id){0};
}

/* The output nets of the old cells, and each result register's representative's old Q net. */
static odin3_status collect_old(splice *sp) {
    const odin3_abc_region *region = sp->region;
    odin3_status st = ODIN3_OK;
    for (size_t at = 0; st == ODIN3_OK && at < region->soft.len + region->regs.len; at++) {
        const odin3_vec *vec = at < region->soft.len ? &region->soft : &region->regs;
        odin3_node_id node = {sp_u32(vec, at < region->soft.len ? at : at - region->soft.len)};
        odin3_net_id net = output_net(sp->mod, node);
        st = odin3_net_valid(net) ? sp_push(&sp->old_nets, net.v) : st;
    }
    for (uint32_t reg = 0; st == ODIN3_OK && reg < sp->res->n_regs_out; reg++) {
        odin3_node_id rep = {sp_u32(&region->regs, sp_u32(&sp->res->reg_from, reg))};
        st = sp_push(&sp->oldq, output_net(sp->mod, rep).v);
        st = st == ODIN3_OK ? sp_push(&sp->repname, odin3_node_name(sp->mod, rep)) : st;
    }
    return st;
}

/* --- the plan of phase B ------------------------------------------------------------------- */

/* A net a port, clock pin, keep or loop cut makes special (§6.5, I13): never dropped. */
static bool co_special(const splice *sp, odin3_net_id net) {
    return sp_u32(&sp->region->ci_of, net.v) != 0 || sp_on_port(sp->mod, net) ||
           sp_is_clock(sp->mod, net) || odin3_abc_net_keep(sp->mod, net);
}

/* With --gates, a buffer of in: the library's BUF cell, else `$_BUF_` (G-7); its output open. */
static odin3_status make_gate_buffer(splice *sp, odin3_net_id in, odin3_prov_id prov,
                                     odin3_node_id *node) {
    odin3_celltype_id type = sp->run->lib.buf;
    if (!odin3_celltype_valid(type) && !sp_type_named(sp->run->design, "$_BUF_", &type)) {
        odin3_log(ODIN3_LOG_ERROR, "abc: the buffer cell type is missing");
        return ODIN3_ERR_INVALID_ARG;
    }
    const odin3_celltype_def *def = odin3_celltype_get(sp->run->design, type);
    uint32_t in_port = 0;
    while (def != NULL && in_port < def->n_ports && def->ports[in_port].dir != ODIN3_DIR_IN) {
        in_port++;
    }
    cell_req cr = {type, 1, &in_port, &in, sp_out_port(sp->run->design, type), {0}, prov};
    return make_cell(sp, &cr, node);
}

static odin3_status make_buffer(splice *sp, uint32_t obj, sp_step *step) {
    odin3_net_id in = {sp_u32(&sp->objnet, obj)};
    static const uint64_t k_identity = IDENTITY_TABLE;
    lut_req rq = {1, &k_identity, &in, {0}, {0}};
    odin3_node_id node = {0};
    odin3_status st = obj_prov(sp, obj, &rq.prov);
    if (st == ODIN3_OK) {
        st = sp->run->gates_mode ? make_gate_buffer(sp, in, rq.prov, &node)
                                 : make_lut(sp, &rq, &node);
    }
    step->act = SP_BUFFER;
    step->buffer = node.v;
    return st;
}

/* The step for CO number idx, driven by constant object obj (0 or 1). */
static odin3_status plan_constant(splice *sp, uint32_t idx, sp_step *step) {
    odin3_net_id net = {sp_u32(&sp->region->cos, idx)};
    uint32_t obj = sp_u32(&sp->res->co_driver, idx);
    odin3_net_id canon = {sp_u32(&sp->objnet, obj)};
    bool clock = sp_is_clock(sp->mod, net);
    bool cut = sp_u32(&sp->region->ci_of, net.v) != 0;
    bool canon_special = sp_on_port(sp->mod, canon) || sp_is_clock(sp->mod, canon) ||
                         odin3_abc_net_keep(sp->mod, canon);
    if (clock || cut || canon_special) {
        return make_buffer(sp, obj, step);
    }
    step->act = SP_CONST;
    step->keep = canon.v;
    return ODIN3_OK;
}

static odin3_status plan_co(splice *sp, uint32_t idx) {
    odin3_net_id net = {sp_u32(&sp->region->cos, idx)};
    uint32_t obj = sp_u32(&sp->res->co_driver, idx);
    sp_step *step = odin3_vec_push(&sp->steps);
    if (step == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (obj < FIRST_OBJ) {
        return plan_constant(sp, idx, step);
    }
    uint32_t carrier = sp_u32(&sp->carried, obj);
    if (carrier == 0) {
        step->act = SP_TAKE;
        step->take = sp_u32(&sp->objnet, obj);
        ((uint32_t *)sp->carried.data)[obj] = net.v;
        return ODIN3_OK;
    }
    if (carrier == net.v || co_special(sp, net)) {
        return make_buffer(sp, obj, step);
    }
    step->act = SP_FOLLOW;
    step->keep = carrier;
    return ODIN3_OK;
}

static odin3_status plan_all(splice *sp) {
    for (uint32_t idx = 0; idx < sp->res->n_cos; idx++) {
        odin3_status st = plan_co(sp, idx);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

odin3_status sp_phase_a(splice *sp) {
    odin3_status st = make_records(sp);
    st = st == ODIN3_OK ? object_nets(sp) : st;
    st = st == ODIN3_OK ? create_luts(sp) : st;
    st = st == ODIN3_OK ? create_ffs(sp) : st;
    st = st == ODIN3_OK ? plan_all(sp) : st;
    st = st == ODIN3_OK ? collect_old(sp) : st;
    return st;
}
