/*
 * miter.c — the temporary design of cone_check (Phase 7 spec 3.3, C-3). One module `cone` in a
 * design of its own (the real design gains no module, so nothing has to be removed): an input port
 * per template input bit, a copy of the group on those inputs, the template cell on the same
 * inputs, and two scalar output ports per template output bit, one fed by the group and one by the
 * template. The group's boundary nets become the inputs and outputs the binding names; every other
 * net of the group gets a fresh net. The real design is only read.
 */
#include "ir/celltype.h"
#include "passes/cone_check/cone_check_int.h"
#include "util/log.h"
#include "util/str.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum { PORT_NAME_BYTES = 32 };

odin3_net_id cc_port_net(const odin3_module *module, odin3_node_id port) {
    return odin3_pin_net(module, odin3_node_pins(module, port).first);
}

/* --- the ports ----------------------------------------------------------------------------- */

/* One scalar port to add: the name `<prefix><index>`, its direction and where its node goes. */
typedef struct cc_port_req {
    const char *prefix;
    uint32_t index;
    odin3_dir dir;
    odin3_vec *ids;
} cc_port_req;

static odin3_status add_port(cc_job *job, const cc_port_req *req) {
    char name[PORT_NAME_BYTES];
    (void)snprintf(name, sizeof name, "%s%" PRIu32, req->prefix, req->index);
    odin3_port_spec spec = {0, req->dir, 1, true, {0}, false, 0, 0, false};
    odin3_status st = odin3_design_intern(job->tmp, odin3_bytes_cstr(name), &spec.name);
    odin3_node_id node = {0};
    st = st == ODIN3_OK ? odin3_module_add_port(job->mod, &spec, &node) : st;
    odin3_node_id *slot = st == ODIN3_OK ? odin3_vec_push(req->ids) : NULL;
    if (st == ODIN3_OK && slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (slot != NULL) {
        *slot = node;
    }
    return st;
}

static odin3_status make_module(cc_job *job) {
    uint32_t name = 0;
    odin3_module_id mid = {0};
    odin3_status st = odin3_design_intern(job->tmp, odin3_bytes_cstr("cone"), &name);
    st = st == ODIN3_OK ? odin3_module_create(job->tmp, name, (odin3_prov_id){0}, &mid) : st;
    st = st == ODIN3_OK ? odin3_design_set_top(job->tmp, mid) : st;
    job->mod = st == ODIN3_OK ? odin3_module_get(job->tmp, mid) : NULL;
    return st;
}

/* A constant template input bit: a `$_CONST0_`/`$_CONST1_` node on a fresh net, in place of a
 * port, so cc_port_net still finds the net through the node's one pin (spec K-3). */
static odin3_status add_const(cc_job *job, uint8_t kind) {
    uint32_t name = 0;
    odin3_celltype_id type = {0};
    odin3_status st = odin3_design_intern(
        job->tmp, odin3_bytes_cstr(kind == CC_BIT_CONST1 ? "$_CONST1_" : "$_CONST0_"), &name);
    if (st == ODIN3_OK && !odin3_celltype_find(job->tmp, name, &type)) {
        return cc_error(job->group.design, (odin3_prov_id){0},
                        "cone_check: constant cell types are not available for the proof");
    }
    odin3_net_id net = {0};
    st = st == ODIN3_OK ? odin3_net_create(job->mod, 0, (odin3_prov_id){0}, &net) : st;
    odin3_node_spec spec = {type, 0, {0}, NULL, 0};
    odin3_netvec ports = {&net, 1};
    odin3_node_id node = {0};
    st = st == ODIN3_OK ? odin3_node_create_connected(job->mod, &spec, &ports, &node) : st;
    odin3_node_id *slot = st == ODIN3_OK ? odin3_vec_push(&job->in_ids) : NULL;
    if (st == ODIN3_OK && slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (slot != NULL) {
        *slot = node;
    }
    return st;
}

/* One entry of in_ids per template input bit, in input order: a port, or a constant node. */
static odin3_status make_inputs(cc_job *job) {
    const uint8_t *cbit = job->tmpl.cbit.data;
    uint32_t total = ((const uint32_t *)job->tmpl.first.data)[job->tmpl.first.len - 1];
    odin3_status st = ODIN3_OK;
    for (uint32_t bit = 0; st == ODIN3_OK && bit < total; bit++) {
        if (cc_tmpl_dir(&job->tmpl, bit) != ODIN3_DIR_IN) {
            continue;
        }
        uint32_t k = ((const uint32_t *)job->tmpl.ord.data)[bit];
        st = cbit[bit] == CC_BIT_NET
                 ? add_port(job, &(cc_port_req){"i", k, ODIN3_DIR_IN, &job->in_ids})
                 : add_const(job, cbit[bit]);
    }
    return st;
}

static odin3_status make_ports(cc_job *job) {
    odin3_status st = make_inputs(job);
    for (uint32_t k = 0; st == ODIN3_OK && k < job->tmpl.n_out; k++) {
        st = add_port(job, &(cc_port_req){"t", k, ODIN3_DIR_OUT, &job->t_ids});
    }
    for (uint32_t k = 0; st == ODIN3_OK && k < job->tmpl.n_out; k++) {
        st = add_port(job, &(cc_port_req){"g", k, ODIN3_DIR_OUT, &job->g_ids});
    }
    return st;
}

/* --- the nets ------------------------------------------------------------------------------ */

static odin3_status fill_map(cc_job *job) {
    for (uint32_t i = 0; i < odin3_module_net_end(job->src); i++) {
        uint32_t *slot = odin3_vec_push(&job->map);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = 0;
    }
    return ODIN3_OK;
}

/* The boundary nets take the ports' nets: inputs by template input bit, outputs the group's. */
static void map_boundary(cc_job *job) {
    const cc_bnet *nets = job->group.nets.data;
    const uint32_t *ord = job->tmpl.ord.data;
    for (size_t i = 0; i < job->group.nets.len; i++) {
        if (nets[i].dir == CC_NET_INTERNAL || nets[i].bit < 0) {
            continue;
        }
        uint32_t k = ord[nets[i].bit];
        const odin3_node_id *ids = nets[i].dir == CC_NET_IN ? job->in_ids.data : job->g_ids.data;
        *(uint32_t *)odin3_vec_at(&job->map, nets[i].net) = cc_port_net(job->mod, ids[k]).v;
    }
}

/* The net of the source net `net` in the temporary module: a fresh one the first time. */
static odin3_status map_net(cc_job *job, odin3_net_id net, odin3_net_id *out) {
    *out = (odin3_net_id){0};
    if (!odin3_net_valid(net)) {
        return ODIN3_OK;
    }
    uint32_t *slot = odin3_vec_at(&job->map, net.v);
    if (*slot == 0) {
        odin3_net_id fresh = {0};
        odin3_status st = odin3_net_create(job->mod, 0, (odin3_prov_id){0}, &fresh);
        if (st != ODIN3_OK) {
            return st;
        }
        *slot = fresh.v;
    }
    *out = (odin3_net_id){*slot};
    return ODIN3_OK;
}

/* --- the nets of one node to create --------------------------------------------------------- */

/* The nets of the ports of one node, as odin3_node_create_connected takes them. */
typedef struct cc_netlist {
    odin3_vec nets;  /* odin3_net_id */
    odin3_vec ports; /* odin3_netvec, pointing into nets (reserved up front, so stable) */
} cc_netlist;

static void netlist_init(cc_netlist *list) {
    odin3_vec_init(&list->nets, sizeof(odin3_net_id));
    odin3_vec_init(&list->ports, sizeof(odin3_netvec));
}

static void netlist_free(cc_netlist *list) {
    odin3_vec_free(&list->nets);
    odin3_vec_free(&list->ports);
}

static odin3_status netlist_reset(cc_netlist *list, size_t total) {
    odin3_vec_clear(&list->nets);
    odin3_vec_clear(&list->ports);
    return odin3_vec_reserve(&list->nets, total + 1U);
}

static odin3_status netlist_add(cc_netlist *list, odin3_net_id net) {
    odin3_net_id *slot = odin3_vec_push(&list->nets);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = net;
    return ODIN3_OK;
}

/* Closes a port: the nets added since index `start`. */
static odin3_status netlist_close(cc_netlist *list, size_t start) {
    odin3_netvec *vec = odin3_vec_push(&list->ports);
    if (vec == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *vec = (odin3_netvec){(const odin3_net_id *)list->nets.data + start,
                          (uint32_t)(list->nets.len - start)};
    return ODIN3_OK;
}

/* --- the group's copy ---------------------------------------------------------------------- */

static odin3_status copy_params(cc_job *job, const odin3_celltype_def *def, odin3_node_id node,
                                odin3_vec *vals) {
    const odin3_strtab *from = odin3_design_strtab(job->group.design);
    for (uint32_t i = 0; i < def->n_params; i++) {
        odin3_value *slot = odin3_vec_push(vals);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = *odin3_node_param(job->src, node, i);
        if (slot->kind == ODIN3_VAL_STRING) {
            odin3_status st = odin3_design_intern(
                job->tmp, odin3_bytes_cstr(odin3_strtab_get(from, slot->str)), &slot->str);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

static odin3_status copy_port(cc_job *job, odin3_pinslice slice, cc_netlist *list) {
    size_t start = list->nets.len;
    for (uint32_t k = 0; k < slice.count; k++) {
        odin3_net_id net = {0};
        odin3_status st =
            map_net(job, odin3_pin_net(job->src, (odin3_pin_id){slice.first.v + k}), &net);
        st = st == ODIN3_OK ? netlist_add(list, net) : st;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return netlist_close(list, start);
}

static odin3_status copy_ports(cc_job *job, const odin3_celltype_def *def, odin3_node_id node,
                               cc_netlist *list) {
    odin3_status st = netlist_reset(list, odin3_node_pins(job->src, node).count);
    for (uint32_t port = 0; st == ODIN3_OK && port < def->n_ports; port++) {
        st = copy_port(job, odin3_node_port(job->src, node, port), list);
    }
    return st;
}

static odin3_status copy_node(cc_job *job, uint32_t nid, cc_netlist *list) {
    odin3_node_id node = {nid};
    const odin3_celltype_def *def =
        odin3_celltype_get(job->group.design, odin3_node_type(job->src, node));
    uint32_t str = 0;
    odin3_celltype_id type = {0};
    odin3_vec vals;
    odin3_vec_init(&vals, sizeof(odin3_value));
    odin3_status st = odin3_design_intern(job->tmp, odin3_bytes_cstr(def->name), &str);
    if (st == ODIN3_OK && !odin3_celltype_find(job->tmp, str, &type)) {
        odin3_vec_free(&vals);
        return cc_error(job->group.design, odin3_node_prov(job->src, node),
                        "cone_check: cell type '%s' of the group is not available for the proof",
                        def->name);
    }
    st = st == ODIN3_OK ? copy_params(job, def, node, &vals) : st;
    st = st == ODIN3_OK ? copy_ports(job, def, node, list) : st;
    odin3_node_spec spec = {type, 0, {0}, vals.data, def->n_params};
    st = st == ODIN3_OK
             ? odin3_node_create_connected(job->mod, &spec, list->ports.data, &(odin3_node_id){0})
             : st;
    odin3_vec_free(&vals);
    return st;
}

static odin3_status copy_group(cc_job *job) {
    cc_netlist list;
    netlist_init(&list);
    const uint32_t *nodes = job->group.nodes.data;
    odin3_status st = ODIN3_OK;
    for (size_t i = 0; st == ODIN3_OK && i < job->group.nodes.len; i++) {
        st = copy_node(job, nodes[i], &list);
    }
    netlist_free(&list);
    return st;
}

/* --- the template cell --------------------------------------------------------------------- */

static odin3_status template_port(const cc_job *job, uint32_t port, cc_netlist *list) {
    const cc_tmpl *tmpl = &job->tmpl;
    const uint32_t *first = tmpl->first.data;
    const uint32_t *ord = tmpl->ord.data;
    const odin3_node_id *ids =
        tmpl->def->ports[port].dir == ODIN3_DIR_IN ? job->in_ids.data : job->t_ids.data;
    size_t start = list->nets.len;
    for (uint32_t bit = first[port]; bit < first[port + 1]; bit++) {
        odin3_status st = netlist_add(list, cc_port_net(job->mod, ids[ord[bit]]));
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return netlist_close(list, start);
}

static odin3_status add_template(cc_job *job) {
    const cc_tmpl *tmpl = &job->tmpl;
    cc_netlist list;
    netlist_init(&list);
    odin3_status st =
        netlist_reset(&list, ((const uint32_t *)tmpl->first.data)[tmpl->def->n_ports]);
    for (uint32_t port = 0; st == ODIN3_OK && port < tmpl->def->n_ports; port++) {
        st = template_port(job, port, &list);
    }
    odin3_node_spec spec = {tmpl->type, 0, {0}, tmpl->params.data, tmpl->def->n_params};
    st = st == ODIN3_OK
             ? odin3_node_create_connected(job->mod, &spec, list.ports.data, &(odin3_node_id){0})
             : st;
    netlist_free(&list);
    return st;
}

odin3_status cc_miter_build(cc_job *job) {
    odin3_status st = make_module(job);
    st = st == ODIN3_OK ? make_ports(job) : st;
    st = st == ODIN3_OK ? fill_map(job) : st;
    if (st == ODIN3_OK) {
        map_boundary(job);
        st = copy_group(job);
    }
    return st == ODIN3_OK ? add_template(job) : st;
}
