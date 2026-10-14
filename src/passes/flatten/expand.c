/*
 * expand.c — the expansion of one root (3A spec §3.3, §3.4, §5; plan Task 3): the FIFO worklist,
 * the per-instance net map, the joins and the aliases. Nodes and wires are copied in copy.c.
 */
#include "passes/flatten/flatten_int.h"

#include "ir/celltype.h"
#include "ir/module.h"
#include "util/alloc.h"
#include "util/log.h"

#include <assert.h>
#include <string.h>

/* --- context ------------------------------------------------------------------------------- */

odin3_status odin3_flatten_ctx_init(odin3_flatten_ctx *fc, odin3_pass_ctx *pass,
                                    const odin3_flatten_graph *graph, const char *sep) {
    static const char *const k_const_names[FLAT_N_CONST] = {"$_CONST0_", "$_CONST1_", "$_CONSTX_",
                                                            "$_CONSTZ_"};
    memset(fc, 0, sizeof *fc);
    fc->pass = pass;
    fc->design = pass->design;
    fc->graph = graph;
    fc->sep = sep != NULL ? sep : ODIN3_FLATTEN_DEFAULT_SEPARATOR;
    odin3_vec_init(&fc->work, sizeof(flat_work));
    odin3_vec_init(&fc->pnets, sizeof(odin3_net_id));
    odin3_vec_init(&fc->map, sizeof(odin3_net_id));
    odin3_vec_init(&fc->joined, sizeof(uint8_t));
    odin3_vec_init(&fc->bits, sizeof(flat_joined));
    odin3_vec_init(&fc->tmp_nets, sizeof(odin3_net_id));
    odin3_vec_init(&fc->tmp_ports, sizeof(odin3_netvec));
    odin3_vec_init(&fc->tmp_vals, sizeof(odin3_value));
    odin3_strbuf_init(&fc->prefix);
    odin3_strbuf_init(&fc->scratch);
    for (uint32_t i = 0; i < FLAT_N_CONST; i++) {
        uint32_t name = 0;
        odin3_status st =
            odin3_design_intern(fc->design, odin3_bytes_cstr(k_const_names[i]), &name);
        if (st != ODIN3_OK) {
            return st;
        }
        (void)odin3_celltype_find(fc->design, name, &fc->const_type[i]);
    }
    return ODIN3_OK;
}

void odin3_flatten_ctx_free(odin3_flatten_ctx *fc) {
    odin3_vec_free(&fc->work);
    odin3_vec_free(&fc->pnets);
    odin3_vec_free(&fc->map);
    odin3_vec_free(&fc->joined);
    odin3_vec_free(&fc->bits);
    odin3_vec_free(&fc->tmp_nets);
    odin3_vec_free(&fc->tmp_ports);
    odin3_vec_free(&fc->tmp_vals);
    odin3_strbuf_free(&fc->prefix);
    odin3_strbuf_free(&fc->scratch);
    odin3_u64map_destroy(fc->redirect);
    odin3_u64map_destroy(fc->memids);
    odin3_u64map_destroy(fc->memmap);
    fc->redirect = NULL;
    fc->memids = NULL;
    fc->memmap = NULL;
}

/* --- the net map ----------------------------------------------------------------------------- */

odin3_net_id odin3_flatten_resolve(const odin3_flatten_ctx *fc, odin3_net_id net) {
    uint64_t next = 0;
    size_t guard = odin3_u64map_count(fc->redirect) + 1;
    while (net.v != 0 && guard-- > 0 && odin3_u64map_get(fc->redirect, net.v, &next)) {
        net.v = (uint32_t)next;
    }
    assert(guard != SIZE_MAX);
    return net;
}

odin3_net_id odin3_flatten_mapped(const odin3_flatten_ctx *fc, odin3_net_id net) {
    if (net.v == 0 || net.v >= fc->map.len) {
        return (odin3_net_id){0};
    }
    return odin3_flatten_resolve(fc, *(const odin3_net_id *)odin3_vec_cat(&fc->map, net.v));
}

static odin3_net_id *map_slot(odin3_flatten_ctx *fc, odin3_net_id child_net) {
    return odin3_vec_at(&fc->map, child_net.v);
}

/* A vec of n zeroed elements. */
static odin3_status zero_resize(odin3_vec *vec, size_t count) {
    odin3_vec_clear(vec);
    if (odin3_vec_reserve(vec, count) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memset(vec->data, 0, count * vec->elem_size);
    vec->len = count;
    return ODIN3_OK;
}

/* A new net of the root for child net cn: its name prefixed, its record derived. */
static odin3_status fresh_net(odin3_flatten_ctx *fc, odin3_net_id cn, odin3_net_id *out) {
    uint32_t name = 0;
    odin3_prov_id prov = {0};
    odin3_status st =
        odin3_flatten_name(fc, (flat_key){FLAT_NET, odin3_net_name(fc->child, cn)}, &name);
    st = st == ODIN3_OK ? odin3_flatten_derive(fc, odin3_net_prov(fc->child, cn), &prov) : st;
    return st == ODIN3_OK ? odin3_net_create(fc->root, name, prov, out) : st;
}

/* The root's shared constant net of kind k (found or created once per root, IR-4). */
static odin3_status const_net_of(odin3_flatten_ctx *fc, uint32_t kind, odin3_net_id *out) {
    odin3_net_id net = odin3_flatten_resolve(fc, fc->const_net[kind]);
    if (net.v == 0) {
        odin3_status st = odin3_net_create(fc->root, 0, (odin3_prov_id){0}, &net);
        odin3_netvec port = {&net, 1};
        odin3_node_spec spec = {fc->const_type[kind], 0, {0}, NULL, 0};
        st = st == ODIN3_OK ? odin3_node_create_connected(fc->root, &spec, &port, NULL) : st;
        if (st != ODIN3_OK) {
            return st;
        }
        fc->const_net[kind] = net;
    }
    *out = net;
    return ODIN3_OK;
}

/* The constant kind (0..3) driving child net cn, -1 when none. */
static int const_kind(const odin3_flatten_ctx *fc, odin3_net_id cn) {
    odin3_pin_id pin = odin3_net_driver(fc->child, cn);
    if (pin.v == 0) {
        return -1;
    }
    odin3_celltype_id type = odin3_node_type(fc->child, odin3_pin_node(fc->child, pin));
    for (int i = 0; i < FLAT_N_CONST; i++) {
        if (fc->const_type[i].v != 0 && fc->const_type[i].v == type.v) {
            return i;
        }
    }
    return -1;
}

/* Merges drop into keep in the root and remembers where drop went. */
static odin3_status merge_nets(odin3_flatten_ctx *fc, odin3_net_id keep, odin3_net_id drop) {
    odin3_status st = odin3_net_merge(fc->root, (odin3_net_pair){keep, drop});
    return st == ODIN3_OK ? odin3_u64map_put(fc->redirect, (odin3_kv){drop.v, keep.v}) : st;
}

enum { FLAG_JOINED = 1, FLAG_FRESH = 2 };

/* One port bit (spec §3.4): child net cn joins root net `to` (none when unconnected). A net's flag
 * is FLAG_JOINED when it keeps its own name only as an alias, FLAG_FRESH when it was named at
 * creation. */
static odin3_status join_bit(odin3_flatten_ctx *fc, odin3_net_id cn, odin3_net_id to) {
    odin3_net_id *slot = map_slot(fc, cn);
    uint8_t *flag = odin3_vec_at(&fc->joined, cn.v);
    odin3_net_id cur = odin3_flatten_resolve(fc, *slot);
    to = odin3_flatten_resolve(fc, to);
    if (cur.v == 0) {
        bool fresh = to.v == 0;
        odin3_status st = fresh ? fresh_net(fc, cn, &to) : ODIN3_OK;
        *flag = fresh ? FLAG_FRESH : FLAG_JOINED;
        *slot = to;
        return st;
    }
    *flag = FLAG_JOINED;
    return to.v != 0 && to.v != cur.v ? merge_nets(fc, cur, to) : ODIN3_OK;
}

static odin3_status map_constants(odin3_flatten_ctx *fc) {
    for (uint32_t idx = 1; idx < odin3_module_net_end(fc->child); idx++) {
        odin3_net_id cn = {idx};
        int kind = odin3_net_live(fc->child, cn) ? const_kind(fc, cn) : -1;
        if (kind < 0) {
            continue;
        }
        odin3_status st = const_net_of(fc, (uint32_t)kind, map_slot(fc, cn));
        *(uint8_t *)odin3_vec_at(&fc->joined, idx) = FLAG_JOINED;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Bit `bit` of child port `port`, whose instance pin is number `pin_no`. */
static odin3_status join_port_bit(odin3_flatten_ctx *fc, uint32_t port, odin3_pin_id pin,
                                  uint32_t pin_no) {
    odin3_net_id cn = odin3_pin_net(fc->child, pin);
    if (cn.v == 0) {
        return ODIN3_OK;
    }
    flat_joined *rec = odin3_vec_push(&fc->bits);
    if (rec == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *rec = (flat_joined){cn.v, port};
    return join_bit(fc, cn, *(odin3_net_id *)odin3_vec_at(&fc->pnets, pin_no));
}

static odin3_status map_ports(odin3_flatten_ctx *fc) {
    uint32_t offset = 0;
    odin3_vec_clear(&fc->bits);
    for (uint32_t port = 0; port < odin3_module_port_count(fc->child); port++) {
        odin3_pinslice pins = odin3_node_pins(fc->child, odin3_module_port(fc->child, port));
        for (uint32_t bit = 0; bit < pins.count; bit++) {
            odin3_status st =
                join_port_bit(fc, port, (odin3_pin_id){pins.first.v + bit}, offset + bit);
            if (st != ODIN3_OK) {
                return st;
            }
        }
        offset += pins.count;
    }
    return ODIN3_OK;
}

/* A bare name becomes an alias of root net target (spec §5.2): a temporary net with the prefixed
 * name, merged into the joined net. */
static odin3_status add_alias(odin3_flatten_ctx *fc, odin3_net_id target, uint32_t orig) {
    odin3_net_id temp = {0};
    uint32_t name = 0;
    odin3_status st = odin3_flatten_name(fc, (flat_key){FLAT_NET, orig}, &name);
    st = st == ODIN3_OK ? odin3_net_create(fc->root, name, (odin3_prov_id){0}, &temp) : st;
    return st == ODIN3_OK ? merge_nets(fc, odin3_flatten_resolve(fc, target), temp) : st;
}

/* The names of a child net that its root net does not carry yet: its own (when own_too) and its
 * bare aliases. */
static odin3_status alias_names(odin3_flatten_ctx *fc, odin3_net_id cn, bool own_too) {
    odin3_net_id target = odin3_flatten_mapped(fc, cn);
    odin3_status st = ODIN3_OK;
    if (own_too && odin3_net_name(fc->child, cn) != 0) {
        st = add_alias(fc, target, odin3_net_name(fc->child, cn));
    }
    uint32_t cursor = 0;
    odin3_net_alias alias;
    while (st == ODIN3_OK && odin3_net_alias_next(fc->child, cn, &cursor, &alias)) {
        st = alias.wb.wire.v == 0 && alias.name != 0 ? add_alias(fc, target, alias.name) : st;
    }
    return st;
}

/* Internal nets get fresh nets; joined ones keep their names as aliases. */
static odin3_status map_rest(odin3_flatten_ctx *fc) {
    for (uint32_t idx = 1; idx < odin3_module_net_end(fc->child); idx++) {
        odin3_net_id cn = {idx};
        if (!odin3_net_live(fc->child, cn)) {
            continue;
        }
        odin3_status st = ODIN3_OK;
        bool joined = *(uint8_t *)odin3_vec_at(&fc->joined, idx) == FLAG_JOINED;
        if (map_slot(fc, cn)->v == 0) {
            st = fresh_net(fc, cn, map_slot(fc, cn));
        }
        st = st == ODIN3_OK ? alias_names(fc, cn, joined) : st;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static odin3_status map_nets(odin3_flatten_ctx *fc) {
    size_t end = odin3_module_net_end(fc->child);
    odin3_status st = zero_resize(&fc->map, end);
    st = st == ODIN3_OK ? zero_resize(&fc->joined, end) : st;
    st = st == ODIN3_OK ? map_constants(fc) : st;
    st = st == ODIN3_OK ? map_ports(fc) : st;
    return st == ODIN3_OK ? map_rest(fc) : st;
}

/* --- one instance ---------------------------------------------------------------------------- */

static odin3_status capture_pins(odin3_flatten_ctx *fc, odin3_node_id inst) {
    odin3_pinslice pins = odin3_node_pins(fc->root, inst);
    odin3_vec_clear(&fc->pnets);
    for (uint32_t i = 0; i < pins.count; i++) {
        odin3_net_id *slot = odin3_vec_push(&fc->pnets);
        if (slot == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *slot = odin3_pin_net(fc->root, (odin3_pin_id){pins.first.v + i});
    }
    return ODIN3_OK;
}

static void log_kept(odin3_flatten_ctx *fc, odin3_node_id inst) {
    if (++fc->kept > FLAT_KEEP_LOG_CAP) {
        return;
    }
    const odin3_strtab *tab = odin3_design_strtab(fc->design);
    const char *name = odin3_strtab_get(tab, odin3_node_name(fc->root, inst));
    odin3_log(ODIN3_LOG_INFO, "flatten: keeping '%s' (module '%s', keep_hierarchy)",
              name != NULL ? name : "?", odin3_strtab_get(tab, odin3_module_name(fc->child)));
}

static odin3_status expand_one(odin3_flatten_ctx *fc, const flat_work *item) {
    odin3_node_id inst = {item->node};
    fc->child_id = odin3_flatten_graph_module_of(fc->graph, odin3_node_type(fc->root, inst));
    if (fc->child_id == 0) {
        return ODIN3_OK;
    }
    fc->child = odin3_module_get(fc->design, (odin3_module_id){fc->child_id});
    if (odin3_flatten_is_kept(fc->root, inst, fc->child)) {
        log_kept(fc, inst);
        return ODIN3_OK;
    }
    odin3_prov_begin_op(fc->pass);
    odin3_u64map_destroy(fc->memmap);
    fc->memmap = odin3_u64map_create(0);
    if (fc->memmap == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    fc->inst_prov = item->prov;
    fc->cache_to = 0;
    odin3_status st = odin3_flatten_path(fc, item);
    st = st == ODIN3_OK
             ? odin3_flatten_set_prefix(
                   fc, (flat_label){odin3_node_name(fc->root, inst), inst.v, item->parent_prefix})
             : st;
    st = st == ODIN3_OK ? capture_pins(fc, inst) : st;
    st = st == ODIN3_OK ? odin3_node_delete(fc->root, inst) : st;
    st = st == ODIN3_OK ? map_nets(fc) : st;
    st = st == ODIN3_OK ? odin3_flatten_copy_nodes(fc) : st;
    st = st == ODIN3_OK ? odin3_flatten_copy_wires(fc) : st;
    st = st == ODIN3_OK ? odin3_flatten_check_joins(fc) : st;
    fc->instances += st == ODIN3_OK ? 1 : 0;
    return st;
}

/* --- one root -------------------------------------------------------------------------------- */

static int memid_param(const odin3_celltype_def *def) {
    for (uint32_t i = 0; i < def->n_params; i++) {
        if (strcmp(def->params[i].name, "MEMID") == 0 && def->params[i].kind == ODIN3_VAL_STRING) {
            return (int)i;
        }
    }
    return -1;
}

/* Queues instance node `node` of the root. */
static odin3_status queue_root_instance(odin3_flatten_ctx *fc, odin3_node_id node) {
    flat_work *item = odin3_vec_push(&fc->work);
    if (item == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *item = (flat_work){node.v, fc->root_name, odin3_node_name(fc->root, node),
                        odin3_node_prov(fc->root, node), 0};
    return ODIN3_OK;
}

/* Notes one live node of the root: a shared constant, a MEMID, an instance. */
static odin3_status scan_node(odin3_flatten_ctx *fc, odin3_node_id node) {
    odin3_celltype_id type = odin3_node_type(fc->root, node);
    const odin3_celltype_def *def = odin3_celltype_get(fc->design, type);
    for (int i = 0; i < FLAT_N_CONST; i++) {
        if (fc->const_type[i].v == type.v && fc->const_net[i].v == 0) {
            fc->const_net[i] = odin3_pin_net(fc->root, odin3_node_pins(fc->root, node).first);
        }
    }
    int memid = memid_param(def);
    if (memid >= 0) {
        uint32_t str = odin3_node_param(fc->root, node, (uint32_t)memid)->str;
        if (odin3_u64map_put(fc->memids, (odin3_kv){str, 1}) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    bool instance =
        def->gran == ODIN3_GRAN_MODULE && odin3_flatten_graph_module_of(fc->graph, type) != 0;
    return instance ? queue_root_instance(fc, node) : ODIN3_OK;
}

static odin3_status scan_root(odin3_flatten_ctx *fc) {
    for (uint32_t nid = 1; nid < odin3_module_node_end(fc->root); nid++) {
        odin3_status st = odin3_node_live(fc->root, (odin3_node_id){nid})
                              ? scan_node(fc, (odin3_node_id){nid})
                              : ODIN3_OK;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static void assert_flat(const odin3_flatten_ctx *fc) {
#ifndef NDEBUG
    for (uint32_t nid = 1; nid < odin3_module_node_end(fc->root); nid++) {
        odin3_node_id node = {nid};
        if (!odin3_node_live(fc->root, node)) {
            continue;
        }
        const odin3_celltype_def *def =
            odin3_celltype_get(fc->design, odin3_node_type(fc->root, node));
        uint32_t child = odin3_flatten_graph_module_of(fc->graph, odin3_node_type(fc->root, node));
        assert(def->gran != ODIN3_GRAN_MODULE || child == 0 ||
               odin3_flatten_is_kept(fc->root, node,
                                     odin3_module_get(fc->design, (odin3_module_id){child})));
    }
#else
    (void)fc;
#endif
}

odin3_status odin3_flatten_expand_root(odin3_flatten_ctx *fc, odin3_module *root) {
    fc->root = root;
    fc->root_name = odin3_module_name(root);
    memset(fc->const_net, 0, sizeof fc->const_net);
    odin3_u64map_destroy(fc->redirect);
    odin3_u64map_destroy(fc->memids);
    fc->redirect = odin3_u64map_create(0);
    fc->memids = odin3_u64map_create(0);
    if (fc->redirect == NULL || fc->memids == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_vec_clear(&fc->work);
    fc->head = 0;
    odin3_status st = scan_root(fc);
    while (st == ODIN3_OK && fc->head < fc->work.len) {
        flat_work item = *(const flat_work *)odin3_vec_cat(&fc->work, fc->head++);
        st = expand_one(fc, &item);
    }
    if (st == ODIN3_OK) {
        assert_flat(fc);
    }
    return st;
}
