/*
 * splice.c — the splice's driver, the canonical constant net and the rollback of phase A (3D spec
 * §6.5, AB-18; plan Task 17). Phase A is splice_a.c, phases B and C splice_bc.c, phase D
 * splice_d.c.
 */
#include "passes/abc/splice.h"

#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/module.h"
#include "passes/abc/prov.h"
#include "passes/abc/run.h"
#include "passes/abc/splice_int.h"
#include "util/log.h"
#include "util/str.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

uint32_t sp_u32(const odin3_vec *vec, size_t idx) {
    return ((const uint32_t *)vec->data)[idx];
}

odin3_status sp_push(odin3_vec *vec, uint32_t value) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = value;
    return ODIN3_OK;
}

odin3_status sp_note(splice *sp, sp_created item) {
    sp_created *slot = odin3_vec_push(&sp->created);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = item;
    return ODIN3_OK;
}

/* --- the canonical constant net ------------------------------------------------------------ */

/* True when the driver of net is a $_CONST0_/$_CONST1_ node or a constant $sop. */
static bool const_driver(const odin3_module *mod, odin3_net_id net) {
    if (odin3_net_driver_count(mod, net) != 1) {
        return false;
    }
    odin3_node_id node = odin3_pin_node(mod, odin3_net_driver(mod, net));
    const odin3_celltype_def *def =
        odin3_celltype_get(odin3_module_design(mod), odin3_node_type(mod, node));
    return def != NULL && (strcmp(def->name, "$_CONST0_") == 0 ||
                           strcmp(def->name, "$_CONST1_") == 0 || strcmp(def->name, "$sop") == 0);
}

/* The lowest-ID net driven by a constant `value` that is not undefined; none when there is no. */
static odin3_net_id find_const(const odin3_module *mod, int value) {
    odin3_const want = value != 0 ? ODIN3_CONST_1 : ODIN3_CONST_0;
    for (uint32_t id = 1; id < odin3_module_net_end(mod); id++) {
        odin3_net_id net = {id};
        if (odin3_net_live(mod, net) && odin3_net_const_value(mod, net) == want &&
            const_driver(mod, net) && !odin3_abc_net_undefined(mod, net)) {
            return net;
        }
    }
    return (odin3_net_id){0};
}

/* Creates `$_CONST<value>_` and its net; *node_out and *net_out get them (both or neither). */
static odin3_status make_const(odin3_abc_run *run, int value, odin3_node_id *node_out,
                               odin3_net_id *net_out) {
    odin3_module *mod = run->module;
    const odin3_design *design = run->design;
    uint32_t name = 0;
    odin3_celltype_id type = {0};
    if (!odin3_strtab_find(odin3_design_strtab(design),
                           odin3_bytes_cstr(value != 0 ? "$_CONST1_" : "$_CONST0_"), &name) ||
        !odin3_celltype_find(design, name, &type)) {
        odin3_log(ODIN3_LOG_ERROR, "abc: the constant cell type is missing");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_net_id net = {0};
    odin3_status st = odin3_net_create(mod, 0, (odin3_prov_id){0}, &net);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_node_spec spec = {type, 0, (odin3_prov_id){0}, NULL, 0};
    odin3_netvec port = {&net, 1};
    st = odin3_node_create_connected(mod, &spec, &port, node_out);
    if (st != ODIN3_OK) {
        (void)odin3_net_delete(mod, net);
        return st;
    }
    *net_out = net;
    return ODIN3_OK;
}

odin3_status odin3_abc_canonical_const(odin3_abc_run *run, int value, odin3_net_id *out) {
    if (run == NULL || out == NULL || (value != 0 && value != 1)) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_abc_canonical_const: invalid argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_net_id net = find_const(run->module, value);
    if (odin3_net_valid(net)) {
        *out = net;
        return ODIN3_OK;
    }
    odin3_node_id node = {0};
    return make_const(run, value, &node, out);
}

/* The canonical net of value for the splice: found, else made and logged for the rollback. */
odin3_status sp_canon(splice *sp, int value, odin3_net_id *out) {
    odin3_net_id *cached = &sp->canon[value != 0 ? 1 : 0];
    if (odin3_net_valid(*cached)) {
        *out = *cached;
        return ODIN3_OK;
    }
    odin3_net_id net = find_const(sp->mod, value);
    if (!odin3_net_valid(net)) {
        odin3_node_id node = {0};
        odin3_status st = make_const(sp->run, value, &node, &net);
        st = st == ODIN3_OK ? sp_note(sp, (sp_created){SP_NODE, node.v}) : st;
        st = st == ODIN3_OK ? sp_note(sp, (sp_created){SP_NET, net.v}) : st;
        if (st != ODIN3_OK) {
            if (odin3_node_live(sp->mod, node)) {
                (void)odin3_node_delete(sp->mod, node);
            }
            return st;
        }
    }
    *cached = net;
    *out = net;
    return ODIN3_OK;
}

/* --- net tests ----------------------------------------------------------------------------- */

bool sp_on_port(const odin3_module *mod, odin3_net_id net) {
    odin3_pinlist pins = odin3_net_pins(mod, net);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_node_id node = odin3_pin_node(mod, pins.pins[i]);
        const odin3_celltype_def *def =
            odin3_celltype_get(odin3_module_design(mod), odin3_node_type(mod, node));
        if (def != NULL && def->gran == ODIN3_GRAN_PORT) {
            return true;
        }
    }
    return false;
}

bool sp_is_clock(const odin3_module *mod, odin3_net_id net) {
    odin3_pinlist pins = odin3_net_pins(mod, net);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_node_id node = odin3_pin_node(mod, pins.pins[i]);
        const odin3_celltype_def *def =
            odin3_celltype_get(odin3_module_design(mod), odin3_node_type(mod, node));
        if (def != NULL && (def->flags & ODIN3_CT_CLOCK_PIN0) != 0 &&
            odin3_pin_reads(mod, pins.pins[i]) && odin3_pin_port(mod, pins.pins[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* --- rollback ------------------------------------------------------------------------------ */

static void drop_node(splice *sp, odin3_node_id node) {
    if (!odin3_node_live(sp->mod, node)) {
        return;
    }
    odin3_pinslice pins = odin3_node_pins(sp->mod, node);
    for (uint32_t i = 0; i < pins.count; i++) {
        (void)odin3_pin_disconnect(sp->mod, (odin3_pin_id){pins.first.v + i});
    }
    (void)odin3_node_delete(sp->mod, node);
}

void sp_rollback(splice *sp) {
    for (size_t at = sp->created.len; at > 0; at--) {
        const sp_created *item = odin3_vec_cat(&sp->created, at - 1);
        if (item->kind == SP_NODE) {
            drop_node(sp, (odin3_node_id){item->id});
        } else if (odin3_net_live(sp->mod, (odin3_net_id){item->id})) {
            (void)odin3_net_delete(sp->mod, (odin3_net_id){item->id});
        }
    }
    odin3_vec_clear(&sp->created);
}

/* --- the driver ---------------------------------------------------------------------------- */

static void sp_init(splice *sp, odin3_abc_run *run, const odin3_abc_region *region,
                    odin3_abc_result *res) {
    *sp = (splice){0};
    sp->run = run;
    sp->mod = run->module;
    sp->region = region;
    sp->res = res;
    odin3_vec_init(&sp->created, sizeof(sp_created));
    odin3_vec_init(&sp->objnet, sizeof(uint32_t));
    odin3_vec_init(&sp->objprov, sizeof(uint32_t));
    odin3_vec_init(&sp->objnode, sizeof(uint32_t));
    odin3_vec_init(&sp->anchors, sizeof(odin3_abc_anchor));
    odin3_vec_init(&sp->carried, sizeof(uint32_t));
    odin3_vec_init(&sp->ffnode, sizeof(uint32_t));
    odin3_vec_init(&sp->oldq, sizeof(uint32_t));
    odin3_vec_init(&sp->repname, sizeof(uint32_t));
    odin3_vec_init(&sp->steps, sizeof(sp_step));
    odin3_vec_init(&sp->old_nets, sizeof(uint32_t));
}

static void sp_free(splice *sp) {
    odin3_vec_free(&sp->created);
    odin3_vec_free(&sp->objnet);
    odin3_vec_free(&sp->objprov);
    odin3_vec_free(&sp->objnode);
    odin3_vec_free(&sp->anchors);
    odin3_vec_free(&sp->carried);
    odin3_vec_free(&sp->ffnode);
    odin3_vec_free(&sp->oldq);
    odin3_vec_free(&sp->repname);
    odin3_vec_free(&sp->steps);
    odin3_vec_free(&sp->old_nets);
}

/* How many of the created objects still live, by what they are (for the stats). */
static void count_survivors(splice *sp) {
    sp->stats.luts = 0;
    sp->stats.ffs = 0;
    for (size_t at = 0; at < sp->created.len; at++) {
        const sp_created *item = odin3_vec_cat(&sp->created, at);
        odin3_node_id node = {item->id};
        if (item->kind != SP_NODE || !odin3_node_live(sp->mod, node)) {
            continue;
        }
        odin3_celltype_id type = odin3_node_type(sp->mod, node);
        const odin3_celltype_def *def = odin3_celltype_get(sp->run->design, type);
        if (def != NULL && strcmp(def->name, "$lut") == 0) {
            sp->stats.luts++;
        } else if (def != NULL && (def->flags & ODIN3_CT_SEQ_EDGE) != 0) {
            sp->stats.ffs++;
        }
    }
    sp->stats.luts = sp->stats.luts > sp->stats.buffers ? sp->stats.luts - sp->stats.buffers : 0;
}

odin3_status odin3_abc_splice(odin3_abc_run *run, const odin3_abc_region *region,
                              odin3_abc_result *res, odin3_abc_splice_stats *stats) {
    if (run == NULL || region == NULL || res == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_abc_splice: invalid argument");
        return ODIN3_ERR_INVALID_ARG;
    }
    splice sp;
    sp_init(&sp, run, region, res);
    odin3_status st = odin3_abc_restore(run, res);
    st = st == ODIN3_OK ? sp_phase_a(&sp) : st;
    if (st != ODIN3_OK) {
        sp_rollback(&sp);
        sp_free(&sp);
        return st;
    }
    st = sp_phase_b(&sp);
    if (st == ODIN3_OK) {
        st = sp_phase_c(&sp);
    }
    sp_phase_d(&sp);
    count_survivors(&sp);
    if (stats != NULL && st == ODIN3_OK) {
        *stats = sp.stats;
    }
    sp_free(&sp);
    return st;
}
