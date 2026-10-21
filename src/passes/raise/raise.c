/*
 * raise.c — one module's run of `raise` (Phase 7 spec 3.4, R-1 to R-3): the group KEY=V that
 * `cone_check` proved equivalent to one cell of TYPE is replaced by that cell, connected through
 * the binding. Nothing changes unless the module holds the proof for this group, this template and
 * this exact group and binding (R-2). Everything is checked first (proof, group, template,
 * binding); then the cell is created on the boundary nets; then the group's nodes, and the wires
 * and nets only they touched, go. The deletes can fail only for lack of memory; when the first one
 * fails the new cell is removed again and the module is as it was, later failures are logged.
 * The new cell's provenance is one DERIVED record over the provenance of every replaced node (R-3).
 */
#include "ir/celltype.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "passes/cone_check/cone_check_int.h"
#include "passes/raise/raise_int.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { NET_TOUCHED = 1, NET_BOUNDARY = 2 };

/* One run on one module. */
typedef struct rs_job {
    odin3_pass_ctx *ctx;
    odin3_module *module;
    const cc_opts *opts;
    cc_group group;
    cc_tmpl tmpl;
    odin3_vec bit_net; /* odin3_net_id per flat template bit: the bound boundary net */
    odin3_vec ports;   /* odin3_netvec per template port, pointing into bit_net */
    odin3_vec state;   /* uint8_t per net ID: NET_TOUCHED, plus NET_BOUNDARY */
} rs_job;

static void job_init(rs_job *job, odin3_pass_ctx *ctx, odin3_module *module, const cc_opts *opts) {
    memset(job, 0, sizeof *job);
    job->ctx = ctx;
    job->module = module;
    job->opts = opts;
    cc_group_init(&job->group, module);
    cc_tmpl_init(&job->tmpl);
    odin3_vec_init(&job->bit_net, sizeof(odin3_net_id));
    odin3_vec_init(&job->ports, sizeof(odin3_netvec));
    odin3_vec_init(&job->state, sizeof(uint8_t));
}

static void job_free(rs_job *job) {
    cc_group_free(&job->group);
    cc_tmpl_free(&job->tmpl);
    odin3_vec_free(&job->bit_net);
    odin3_vec_free(&job->ports);
    odin3_vec_free(&job->state);
}

/* --- the proof (R-2) ----------------------------------------------------------------------- */

static const char *module_text(const odin3_module *module, const char *key) {
    const odin3_design *design = odin3_module_design(module);
    uint32_t key_str = 0;
    if (!odin3_strtab_find(odin3_design_strtab(design), odin3_bytes_cstr(key), &key_str)) {
        return NULL;
    }
    const odin3_value *val = odin3_attr_get(
        module, (odin3_objref){ODIN3_OBJ_MODULE, odin3_module_id_of(module).v}, key_str);
    return val != NULL && val->kind == ODIN3_VAL_STRING
               ? odin3_strtab_get(odin3_design_strtab(design), val->str)
               : NULL;
}

static const char *module_name(const odin3_module *module) {
    return odin3_strtab_get(odin3_design_strtab(odin3_module_design(module)),
                            odin3_module_name(module));
}

/* The refusals of R-2, in the order of the checks; logged, ODIN3_ERR_INVALID_ARG. */
static odin3_status verify_proof(const rs_job *job) {
    const odin3_module *mod = job->module;
    const cc_opts *opts = job->opts;
    odin3_design *design = job->group.design;
    char group[ODIN3_LOG_BUF / 4];
    (void)snprintf(group, sizeof group, "%s=%s", opts->key, opts->value);
    const char *verdict = module_text(mod, CC_KEY_CHECK);
    if (verdict == NULL) {
        return cc_error(design, (odin3_prov_id){0},
                        "raise: module '%s' has no cone_check result: run cone_check on group '%s' "
                        "first",
                        module_name(mod), group);
    }
    const char *proved = module_text(mod, CC_KEY_GROUP);
    if (strcmp(verdict, "equivalent") != 0 || proved == NULL || strcmp(proved, group) != 0) {
        return cc_error(design, (odin3_prov_id){0},
                        "raise: group '%s' of module '%s' is not equivalent: the cone_check result "
                        "is '%s' for group '%s'",
                        group, module_name(mod), verdict, proved != NULL ? proved : "?");
    }
    odin3_strbuf cell;
    odin3_strbuf_init(&cell);
    odin3_status st = cc_cell_text(opts, &cell);
    const char *was = module_text(mod, CC_KEY_CELL);
    if (st == ODIN3_OK && (was == NULL || strcmp(was, cell.data) != 0)) {
        st = cc_error(design, (odin3_prov_id){0},
                      "raise: cone_check proved group '%s' against '%s', not '%s'", group,
                      was != NULL ? was : "?", cell.data);
    }
    odin3_strbuf_free(&cell);
    char hash[ODIN3_CONE_HASH_BYTES];
    const char *stored = module_text(mod, CC_KEY_HASH);
    if (st == ODIN3_OK) {
        st = odin3_cone_hash(mod, opts->key, opts->value, hash);
    }
    if (st == ODIN3_OK && (stored == NULL || strcmp(stored, hash) != 0)) {
        st = cc_error(design, (odin3_prov_id){0},
                      "raise: group '%s' of module '%s' or its binding changed since the "
                      "cone_check proof: run cone_check again",
                      group, module_name(mod));
    }
    return st;
}

/* --- the connections ----------------------------------------------------------------------- */

static odin3_status fill_u8(odin3_vec *vec, size_t count) {
    for (size_t i = 0; i < count; i++) {
        uint8_t *slot = odin3_vec_push(vec);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = 0;
    }
    return ODIN3_OK;
}

/* Marks every net a group pin is on as touched, and the boundary nets of the group as boundary. */
static odin3_status mark_nets(rs_job *job) {
    odin3_status st = fill_u8(&job->state, odin3_module_net_end(job->module));
    const uint32_t *nodes = job->group.nodes.data;
    for (size_t i = 0; st == ODIN3_OK && i < job->group.nodes.len; i++) {
        odin3_pinslice pins = odin3_node_pins(job->module, (odin3_node_id){nodes[i]});
        for (uint32_t k = 0; k < pins.count; k++) {
            odin3_net_id net = odin3_pin_net(job->module, (odin3_pin_id){pins.first.v + k});
            if (odin3_net_valid(net)) {
                *(uint8_t *)odin3_vec_at(&job->state, net.v) = NET_TOUCHED;
            }
        }
    }
    const cc_bnet *nets = job->group.nets.data;
    for (size_t i = 0; i < job->group.nets.len; i++) {
        *(uint8_t *)odin3_vec_at(&job->state, nets[i].net) = NET_TOUCHED | NET_BOUNDARY;
    }
    return st;
}

/* The net of each template port bit and the netvec of each template port. */
static odin3_status make_ports(rs_job *job) {
    const uint32_t *first = job->tmpl.first.data;
    uint32_t bits = first[job->tmpl.def->n_ports];
    for (uint32_t bit = 0; bit < bits; bit++) {
        odin3_net_id *slot = odin3_vec_push(&job->bit_net);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = (odin3_net_id){0};
    }
    const cc_bnet *nets = job->group.nets.data;
    for (size_t i = 0; i < job->group.nets.len; i++) {
        if (nets[i].dir != CC_NET_INTERNAL && nets[i].bit >= 0) {
            *(odin3_net_id *)odin3_vec_at(&job->bit_net, (size_t)nets[i].bit) =
                (odin3_net_id){nets[i].net};
        }
    }
    for (uint32_t port = 0; port < job->tmpl.def->n_ports; port++) {
        odin3_netvec *vec = odin3_vec_push(&job->ports);
        if (vec == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *vec = (odin3_netvec){(const odin3_net_id *)job->bit_net.data + first[port],
                              first[port + 1] - first[port]};
    }
    return ODIN3_OK;
}

/* --- the provenance (R-3) ------------------------------------------------------------------ */

/* The DERIVED record over the provenance of every group node; 0 when no node has any. */
static odin3_status make_prov(const rs_job *job, odin3_prov_id *out) {
    odin3_vec parents;
    odin3_vec_init(&parents, sizeof(odin3_prov_id));
    const uint32_t *nodes = job->group.nodes.data;
    odin3_status st = ODIN3_OK;
    *out = (odin3_prov_id){0};
    for (size_t i = 0; st == ODIN3_OK && i < job->group.nodes.len; i++) {
        odin3_prov_id prov = odin3_node_prov(job->module, (odin3_node_id){nodes[i]});
        odin3_prov_id *slot = prov.v != 0 ? odin3_vec_push(&parents) : NULL;
        if (prov.v != 0 && slot == NULL) {
            st = ODIN3_ERR_NO_MEMORY;
        } else if (slot != NULL) {
            *slot = prov;
        }
    }
    if (st == ODIN3_OK && parents.len > 0) {
        odin3_prov_begin_op(job->ctx);
        st = odin3_prov_derive(job->ctx, (odin3_prov_list){parents.data, (uint32_t)parents.len},
                               out);
    }
    odin3_vec_free(&parents);
    return st;
}

/* --- the replacement ----------------------------------------------------------------------- */

static odin3_status create_cell(rs_job *job, odin3_node_id *cell) {
    odin3_prov_id prov = {0};
    odin3_status st = make_prov(job, &prov);
    odin3_node_spec spec = {job->tmpl.type, 0, prov, job->tmpl.params.data,
                            job->tmpl.def->n_params};
    return st == ODIN3_OK ? odin3_node_create_connected(job->module, &spec, job->ports.data, cell)
                          : st;
}

/* True when net is a net only the group touched: no pin left, not on the boundary. */
static bool net_is_spent(rs_job *job, uint32_t nid) {
    return *(uint8_t *)odin3_vec_at(&job->state, nid) == NET_TOUCHED &&
           odin3_net_pins(job->module, (odin3_net_id){nid}).count == 0;
}

/* The primary wire of a spent net goes when every bit of it is on a spent net and it is no port
 * wire; a wire that also holds a net of the boundary stays, its spent nets stay with it. */
static odin3_status drop_wire(rs_job *job, uint32_t nid) {
    odin3_wire_id wire = odin3_net_primary(job->module, (odin3_net_id){nid}).wire;
    if (!odin3_wire_valid(wire) || odin3_wire_port_node(job->module, wire).v != 0) {
        return ODIN3_OK;
    }
    for (uint32_t bit = 0; bit < odin3_wire_width(job->module, wire); bit++) {
        odin3_net_id net = odin3_wire_net(job->module, wire, bit);
        if (!odin3_net_valid(net) || !net_is_spent(job, net.v)) {
            return ODIN3_OK;
        }
    }
    return odin3_wire_delete(job->module, wire);
}

/* Deletes the group's nodes, then the wires and nets only they touched that nothing else holds. */
static odin3_status delete_group(rs_job *job, size_t *deleted) {
    const uint32_t *nodes = job->group.nodes.data;
    for (; *deleted < job->group.nodes.len; (*deleted)++) {
        odin3_status st = odin3_node_delete(job->module, (odin3_node_id){nodes[*deleted]});
        if (st != ODIN3_OK) {
            return st;
        }
    }
    odin3_status st = ODIN3_OK;
    for (uint32_t nid = 1; st == ODIN3_OK && nid < job->state.len; nid++) {
        st = net_is_spent(job, nid) ? drop_wire(job, nid) : ODIN3_OK;
    }
    for (uint32_t nid = 1; st == ODIN3_OK && nid < job->state.len; nid++) {
        odin3_net_id net = {nid};
        if (net_is_spent(job, nid) && odin3_net_live(job->module, net) &&
            !odin3_wire_valid(odin3_net_primary(job->module, net).wire) &&
            odin3_net_alias_count(job->module, net) == 0) {
            st = odin3_net_delete(job->module, net);
        }
    }
    return st;
}

static odin3_status prepare(rs_job *job) {
    const cc_opts *opts = job->opts;
    odin3_status st = verify_proof(job);
    st = st == ODIN3_OK ? cc_group_check(&job->group, opts->key, opts->value) : st;
    st = st == ODIN3_OK ? cc_tmpl_build(&job->tmpl, job->group.design, opts) : st;
    st = st == ODIN3_OK ? cc_tmpl_bind(&job->tmpl, &job->group) : st;
    st = st == ODIN3_OK ? mark_nets(job) : st;
    return st == ODIN3_OK ? make_ports(job) : st;
}

odin3_status raise_check_module(odin3_pass_ctx *ctx, odin3_module *module, const cc_opts *opts,
                                bool *found) {
    rs_job job;
    job_init(&job, ctx, module, opts);
    odin3_status st = cc_group_collect(&job.group, opts);
    *found = st == ODIN3_OK && job.group.nodes.len > 0;
    st = *found && st == ODIN3_OK ? prepare(&job) : st;
    job_free(&job);
    return st;
}

/* Creates the cell, then deletes the group; the cell is taken back when no node is gone yet. */
static odin3_status replace_group(rs_job *job) {
    odin3_node_id cell = {0};
    odin3_status st = create_cell(job, &cell);
    size_t deleted = 0;
    if (st != ODIN3_OK) {
        return st;
    }
    st = delete_group(job, &deleted);
    if (st != ODIN3_OK && deleted == 0) {
        /* nothing of the group is gone yet: take the new cell back so it never double-drives */
        (void)odin3_node_delete(job->module, cell);
    }
    if (st != ODIN3_OK) {
        odin3_log(ODIN3_LOG_ERROR, "raise: module '%s': replacing the group failed (%d)%s",
                  module_name(job->module), (int)st,
                  deleted == 0 ? "" : ": the module is partly changed");
    }
    return st;
}

odin3_status raise_run_module(odin3_pass_ctx *ctx, odin3_module *module, const cc_opts *opts,
                              bool *found) {
    rs_job job;
    job_init(&job, ctx, module, opts);
    odin3_status st = cc_group_collect(&job.group, opts);
    *found = st == ODIN3_OK && job.group.nodes.len > 0;
    st = *found && st == ODIN3_OK ? prepare(&job) : st;
    size_t count = job.group.nodes.len;
    st = *found && st == ODIN3_OK ? replace_group(&job) : st;
    if (*found && st == ODIN3_OK) {
        odin3_log(ODIN3_LOG_INFO, "raise: module '%s': group '%s=%s': %zu nodes replaced by one %s",
                  module_name(module), opts->key, opts->value, count, opts->cell);
    }
    job_free(&job);
    return st;
}
