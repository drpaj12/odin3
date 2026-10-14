/*
 * graph.c — the pre-flight of `flatten` (3A spec §3.3, §7, §12; plan Task 2): the cell-type ->
 * module map, the instance edges, the Kahn order and cycle report, the roots, the flat sizes, the
 * caps and the budget. Explicit queues only; nothing here mutates the design.
 */
#include "passes/budget.h"
#include "passes/flatten/flatten.h"
#include "passes/flatten/flatten_test.h"

#include "ir/celltype.h"
#include "ir/value.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

/* The estimate constants of spec §12 (plan point (f)): bytes per added node, pin, net, wire, and
 * per provenance record (charged once per added node, net and wire, an upper bound: records are
 * hash-consed). Measured 2026-10-14 on the 19 VTR designs of the Phase 3 exit test
 * (`flatten-vtr --raw`, a Release build): a non-negative least-squares fit of the bytes the pass
 * reserved (`odin3_design_bytes_reserved` delta) on the objects it added gave about 570 bytes per
 * node, 21 per pin and 76 per net (wires and records are collinear with them; bgm is the tightest
 * design at 260 bytes per node+net+wire). Rounded up so that the estimate is 1.16x (bgm) to 2.2x
 * the measured growth on all 19 and never below it. */
enum {
    B_NODE = 448,
    B_PIN = 24,
    B_NET = 8,
    B_WIRE = 16,
    B_REC = 96,
    B_NAME = 32,
    N_CONST_TYPES = 4,
    MAX_CYCLE_NAMES = 16,
    MAX_UNUSED_NAMES = 8
};

typedef struct flat_edge {
    uint32_t node;  /* the instance node in the parent */
    uint32_t child; /* the child module's ID */
    bool kept;
} flat_edge;

struct odin3_flatten_graph {
    odin3_design *design;
    uint32_t n_mod; /* one past the last module ID */
    uint32_t top;
    odin3_u64map *types;
    odin3_celltype_id const_types[N_CONST_TYPES];
    odin3_vec edges;          /* flat_edge, grouped by parent module */
    uint32_t *first;          /* n_mod + 1: the edges of m are first[m] .. first[m + 1] */
    odin3_flatten_size *own;  /* per module, its non-expanded objects */
    odin3_flatten_size *flat; /* per module, all non-kept instances expanded */
    uint32_t *order;          /* reachable modules, parents first */
    uint32_t n_order;
    uint32_t *roots;
    uint32_t n_roots;
    uint8_t *is_root;
    odin3_flatten_totals totals;
};

static const char *const k_const_names[N_CONST_TYPES] = {"$_CONST0_", "$_CONST1_", "$_CONSTX_",
                                                         "$_CONSTZ_"};

/* --- limits (test hook) ------------------------------------------------------------------- */

static odin3_flatten_limits g_limits;

static uint64_t clamp_cap(uint64_t value, uint64_t dflt) {
    return value == 0 || value > dflt ? dflt : value;
}

void odin3_flatten_test_set_limits(const odin3_flatten_limits *limits) {
    if (limits == NULL) {
        memset(&g_limits, 0, sizeof g_limits);
        return;
    }
    g_limits.max_nodes = clamp_cap(limits->max_nodes, ODIN3_FLATTEN_MAX_NODES);
    g_limits.max_pins = clamp_cap(limits->max_pins, ODIN3_FLATTEN_MAX_PINS);
    g_limits.max_nets = clamp_cap(limits->max_nets, ODIN3_FLATTEN_MAX_NETS);
    g_limits.max_wires = clamp_cap(limits->max_wires, ODIN3_FLATTEN_MAX_WIRES);
    g_limits.max_roots = (uint32_t)clamp_cap(limits->max_roots, ODIN3_FLATTEN_MAX_ROOTS);
}

odin3_flatten_limits odin3_flatten_limits_now(void) {
    odin3_flatten_limits now = {ODIN3_FLATTEN_MAX_NODES, ODIN3_FLATTEN_MAX_PINS,
                                ODIN3_FLATTEN_MAX_NETS, ODIN3_FLATTEN_MAX_WIRES,
                                ODIN3_FLATTEN_MAX_ROOTS};
    if (g_limits.max_nodes != 0) {
        now = g_limits;
    }
    return now;
}

/* --- saturating arithmetic ----------------------------------------------------------------- */

static uint64_t sat_add(uint64_t lhs, uint64_t rhs) {
    return lhs > UINT64_MAX - rhs ? UINT64_MAX : lhs + rhs;
}

static uint64_t sat_mul(uint64_t lhs, uint64_t rhs) {
    return rhs != 0 && lhs > UINT64_MAX / rhs ? UINT64_MAX : lhs * rhs;
}

/* --- the keep_hierarchy rule (spec §7) ----------------------------------------------------- */

static bool value_true(const odin3_strtab *tab, const odin3_value *val) {
    switch (val->kind) {
    case ODIN3_VAL_INT:
        return val->i != 0;
    case ODIN3_VAL_BITS:
        for (uint32_t i = 0; val->bits != NULL && i < val->len; i++) {
            if (val->bits[i] == ODIN3_BIT_1) {
                return true;
            }
        }
        return false;
    case ODIN3_VAL_STRING:
        return odin3_strtab_len(tab, val->str) > 0;
    default:
        return false;
    }
}

bool odin3_flatten_is_kept(const odin3_module *parent, odin3_node_id inst,
                           const odin3_module *child) {
    const odin3_strtab *tab = odin3_design_strtab(odin3_module_design(parent));
    uint32_t key = 0;
    if (!odin3_strtab_find(tab, odin3_bytes_cstr("keep_hierarchy"), &key)) {
        return false;
    }
    odin3_objref on_inst = {ODIN3_OBJ_NODE, inst.v};
    odin3_objref on_mod = {ODIN3_OBJ_MODULE, odin3_module_id_of(child).v};
    const odin3_value *val = odin3_attr_get(parent, on_inst, key);
    if (val != NULL && value_true(tab, val)) {
        return true;
    }
    val = odin3_attr_get(child, on_mod, key);
    return val != NULL && value_true(tab, val);
}

/* --- building ------------------------------------------------------------------------------ */

void odin3_flatten_graph_free(odin3_flatten_graph *graph) {
    if (graph == NULL) {
        return;
    }
    odin3_u64map_destroy(graph->types);
    odin3_vec_free(&graph->edges);
    odin3_util_free(graph->first);
    odin3_util_free(graph->own);
    odin3_util_free(graph->flat);
    odin3_util_free(graph->order);
    odin3_util_free(graph->roots);
    odin3_util_free(graph->is_root);
    odin3_util_free(graph);
}

static odin3_status graph_alloc(odin3_flatten_graph *graph) {
    size_t n_mod = graph->n_mod;
    graph->types = odin3_u64map_create(n_mod);
    graph->first = odin3_util_calloc((n_mod + 1) * sizeof *graph->first);
    graph->own = odin3_util_calloc(n_mod * sizeof *graph->own);
    graph->flat = odin3_util_calloc(n_mod * sizeof *graph->flat);
    graph->order = odin3_util_calloc(n_mod * sizeof *graph->order);
    graph->roots = odin3_util_calloc(n_mod * sizeof *graph->roots);
    graph->is_root = odin3_util_calloc(n_mod * sizeof *graph->is_root);
    bool ok = graph->types != NULL && graph->first != NULL && graph->own != NULL &&
              graph->flat != NULL && graph->order != NULL && graph->roots != NULL &&
              graph->is_root != NULL;
    return ok ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
}

/* The cell type -> module map, in module-ID order, and the four constant cell types. */
static odin3_status map_types(odin3_flatten_graph *graph) {
    for (uint32_t mod = 1; mod < graph->n_mod; mod++) {
        const odin3_module *module = odin3_module_get(graph->design, (odin3_module_id){mod});
        odin3_kv entry = {odin3_module_celltype(module).v, mod};
        if (odin3_u64map_put(graph->types, entry) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    const odin3_strtab *tab = odin3_design_strtab(graph->design);
    for (uint32_t i = 0; i < N_CONST_TYPES; i++) {
        uint32_t name = 0;
        if (odin3_strtab_find(tab, odin3_bytes_cstr(k_const_names[i]), &name)) {
            (void)odin3_celltype_find(graph->design, name, &graph->const_types[i]);
        }
    }
    return ODIN3_OK;
}

bool odin3_flatten_graph_is_const(const odin3_flatten_graph *graph, odin3_celltype_id type) {
    for (uint32_t i = 0; i < N_CONST_TYPES; i++) {
        if (graph->const_types[i].v != 0 && graph->const_types[i].v == type.v) {
            return true;
        }
    }
    return false;
}

bool odin3_flatten_graph_is_root(const odin3_flatten_graph *graph, uint32_t mod) {
    return mod < graph->n_mod && graph->is_root[mod] != 0;
}

uint32_t odin3_flatten_graph_module_of(const odin3_flatten_graph *graph, odin3_celltype_id type) {
    uint64_t mod = 0;
    return odin3_u64map_get(graph->types, type.v, &mod) ? (uint32_t)mod : 0;
}

/* One live node of module mod: an edge when it instantiates a module, else an own object. */
static odin3_status scan_node(odin3_flatten_graph *graph, const odin3_module *module, uint32_t mod,
                              odin3_node_id node) {
    odin3_celltype_id type = odin3_node_type(module, node);
    const odin3_celltype_def *def = odin3_celltype_get(graph->design, type);
    uint32_t pins = odin3_node_pins(module, node).count;
    odin3_flatten_size *own = &graph->own[mod];
    if (def->gran == ODIN3_GRAN_PORT || odin3_flatten_graph_is_const(graph, type)) {
        return ODIN3_OK;
    }
    uint32_t child =
        def->gran == ODIN3_GRAN_MODULE ? odin3_flatten_graph_module_of(graph, type) : 0;
    if (child != 0) {
        const odin3_module *cmod = odin3_module_get(graph->design, (odin3_module_id){child});
        flat_edge *edge = odin3_vec_push(&graph->edges);
        if (edge == NULL) {
            return ODIN3_ERR_NO_MEMORY;
        }
        *edge = (flat_edge){node.v, child, odin3_flatten_is_kept(module, node, cmod)};
        if (!edge->kept) {
            return ODIN3_OK;
        }
    }
    own->nodes = sat_add(own->nodes, 1);
    own->pins = sat_add(own->pins, pins);
    return ODIN3_OK;
}

/* The live nets and wires of module mod. */
static void count_objects(odin3_flatten_graph *graph, const odin3_module *module, uint32_t mod) {
    for (uint32_t net = 1; net < odin3_module_net_end(module); net++) {
        graph->own[mod].nets += odin3_net_live(module, (odin3_net_id){net}) ? 1 : 0;
    }
    for (uint32_t wire = 1; wire < odin3_module_wire_end(module); wire++) {
        graph->own[mod].wires += odin3_wire_live(module, (odin3_wire_id){wire}) ? 1 : 0;
    }
}

/* The edges and own counts of every module. */
static odin3_status scan_modules(odin3_flatten_graph *graph) {
    for (uint32_t mod = 1; mod < graph->n_mod; mod++) {
        const odin3_module *module = odin3_module_get(graph->design, (odin3_module_id){mod});
        graph->first[mod] = (uint32_t)graph->edges.len;
        for (uint32_t nid = 1; nid < odin3_module_node_end(module); nid++) {
            if (!odin3_node_live(module, (odin3_node_id){nid})) {
                continue;
            }
            odin3_status st = scan_node(graph, module, mod, (odin3_node_id){nid});
            if (st != ODIN3_OK) {
                return st;
            }
        }
        count_objects(graph, module, mod);
    }
    graph->first[graph->n_mod] = (uint32_t)graph->edges.len;
    return ODIN3_OK;
}

static const flat_edge *edges_of(const odin3_flatten_graph *graph, uint32_t mod, uint32_t *n) {
    *n = graph->first[mod + 1] - graph->first[mod];
    return (const flat_edge *)graph->edges.data + graph->first[mod];
}

/* --- Kahn order and the cycle report -------------------------------------------------------- */

/* The lowest remaining module with an edge to target, 0 when none. */
static uint32_t find_pred(const odin3_flatten_graph *graph, const uint8_t *remaining,
                          uint32_t target) {
    for (uint32_t mod = 1; mod < graph->n_mod; mod++) {
        uint32_t n_edges = 0;
        const flat_edge *edges = edges_of(graph, mod, &n_edges);
        for (uint32_t i = 0; remaining[mod] != 0 && i < n_edges; i++) {
            if (edges[i].child == target) {
                return mod;
            }
        }
    }
    return 0;
}

static void name_into(const odin3_flatten_graph *graph, uint32_t mod, odin3_strbuf *out) {
    const odin3_module *module = odin3_module_get(graph->design, (odin3_module_id){mod});
    const char *name =
        odin3_strtab_get(odin3_design_strtab(graph->design), odin3_module_name(module));
    (void)odin3_strbuf_appendf(out, "%s", name != NULL ? name : "?");
}

/* The cycle among the remaining modules, found by walking parents back from the lowest one;
 * seq gets it in walk order (the first and last entries equal); returns its length. */
static uint32_t walk_cycle(const odin3_flatten_graph *graph, const uint8_t *remaining,
                           uint32_t *seq, uint8_t *seen) {
    uint32_t cur = 1;
    while (cur < graph->n_mod && remaining[cur] == 0) {
        cur++;
    }
    uint32_t len = 0;
    while (cur != 0 && !seen[cur]) {
        seen[cur] = 1;
        seq[len++] = cur;
        cur = find_pred(graph, remaining, cur);
    }
    uint32_t first = 0;
    while (cur != 0 && seq[first] != cur) {
        first++;
    }
    if (cur != 0) {
        seq[len++] = cur;
    }
    memmove(seq, seq + first, (len - first) * sizeof *seq);
    return len - first;
}

/* Logs `flatten: recursive instantiation: a -> b -> a` for a cycle among the remaining modules. */
static void report_cycle(const odin3_flatten_graph *graph, const uint8_t *remaining) {
    uint32_t *seq = odin3_util_calloc(((size_t)graph->n_mod + 2) * sizeof *seq);
    uint8_t *seen = odin3_util_calloc((size_t)graph->n_mod + 1);
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    uint32_t len = seq != NULL && seen != NULL ? walk_cycle(graph, remaining, seq, seen) : 0;
    for (uint32_t i = len; i > 0; i--) { /* walked backwards: print forwards */
        if (len - i >= MAX_CYCLE_NAMES) {
            (void)odin3_strbuf_appendf(&text, " -> ...");
            break;
        }
        (void)odin3_strbuf_appendf(&text, "%s", i == len ? "" : " -> ");
        name_into(graph, seq[i - 1], &text);
    }
    odin3_log(ODIN3_LOG_ERROR, "flatten: recursive instantiation: %s",
              text.data != NULL ? text.data : "?");
    odin3_strbuf_free(&text);
    odin3_util_free(seq);
    odin3_util_free(seen);
}

/* Scratch of the sort: reached[m], the edges into m not yet processed, a module queue. */
typedef struct sort_ws {
    uint8_t *reached;
    uint32_t *indeg;
    uint32_t *queue;
} sort_ws;

/* Marks the modules reachable from the top and counts the edges into each. */
static void reach_modules(const odin3_flatten_graph *graph, sort_ws *ws) {
    uint8_t *reached = ws->reached;
    uint32_t *indeg = ws->indeg;
    uint32_t *queue = ws->queue;
    uint32_t head = 0;
    uint32_t tail = 0;
    reached[graph->top] = 1;
    queue[tail++] = graph->top;
    while (head < tail) {
        uint32_t n_edges = 0;
        const flat_edge *edges = edges_of(graph, queue[head++], &n_edges);
        for (uint32_t i = 0; i < n_edges; i++) {
            indeg[edges[i].child]++;
            if (!reached[edges[i].child]) {
                reached[edges[i].child] = 1;
                queue[tail++] = edges[i].child;
            }
        }
    }
}

/* Kahn's loop from the top (when nothing instantiates it) into graph->order. */
static void kahn(odin3_flatten_graph *graph, const sort_ws *ws) {
    uint32_t *indeg = ws->indeg;
    uint32_t *queue = ws->queue;
    uint32_t head = 0;
    uint32_t tail = 0;
    if (indeg[graph->top] == 0) {
        queue[tail++] = graph->top;
    }
    while (head < tail) {
        uint32_t mod = queue[head++];
        uint32_t n_edges = 0;
        const flat_edge *edges = edges_of(graph, mod, &n_edges);
        graph->order[graph->n_order++] = mod;
        for (uint32_t i = 0; i < n_edges; i++) {
            if (--indeg[edges[i].child] == 0) {
                queue[tail++] = edges[i].child;
            }
        }
    }
}

/* Kahn's sort of the reachable modules into graph->order; a cycle is reported and refused. */
static odin3_status sort_modules(odin3_flatten_graph *graph) {
    size_t n_mod = graph->n_mod;
    uint8_t *reached = odin3_util_calloc(n_mod);
    uint32_t *indeg = odin3_util_calloc(n_mod * sizeof *indeg);
    uint32_t *queue = odin3_util_calloc(n_mod * sizeof *queue);
    odin3_status st = ODIN3_ERR_NO_MEMORY;
    if (reached != NULL && indeg != NULL && queue != NULL) {
        st = ODIN3_OK;
        uint32_t n_reach = 0;
        sort_ws ws = {reached, indeg, queue};
        reach_modules(graph, &ws);
        for (uint32_t mod = 1; mod < graph->n_mod; mod++) {
            n_reach += reached[mod];
        }
        kahn(graph, &ws);
        if (graph->n_order < n_reach) {
            for (uint32_t i = 0; i < graph->n_order; i++) {
                reached[graph->order[i]] = 0;
            }
            report_cycle(graph, reached);
            st = ODIN3_ERR_INVALID_ARG;
        }
    }
    odin3_util_free(reached);
    odin3_util_free(indeg);
    odin3_util_free(queue);
    return st;
}

/* --- roots and sizes ------------------------------------------------------------------------ */

/* The roots: the top, and every module a kept instance of a root's expansion names. */
static odin3_status find_roots(odin3_flatten_graph *graph) {
    uint8_t *mesh = odin3_util_calloc(graph->n_mod);
    uint32_t *queue = odin3_util_calloc((size_t)graph->n_mod * sizeof *queue);
    if (mesh == NULL || queue == NULL) {
        odin3_util_free(mesh);
        odin3_util_free(queue);
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t head = 0;
    uint32_t tail = 0;
    graph->is_root[graph->top] = 1;
    mesh[graph->top] = 1;
    queue[tail++] = graph->top;
    while (head < tail) {
        uint32_t n_edges = 0;
        const flat_edge *edges = edges_of(graph, queue[head++], &n_edges);
        for (uint32_t i = 0; i < n_edges; i++) {
            graph->is_root[edges[i].child] |= edges[i].kept ? 1 : 0;
            if (!mesh[edges[i].child]) {
                mesh[edges[i].child] = 1;
                queue[tail++] = edges[i].child;
            }
        }
    }
    for (uint32_t mod = 1; mod < graph->n_mod; mod++) {
        if (graph->is_root[mod] != 0) {
            graph->roots[graph->n_roots++] = mod;
        }
    }
    odin3_util_free(mesh);
    odin3_util_free(queue);
    return ODIN3_OK;
}

/* flat[mod] from its children's, children first (reverse Kahn order); then the totals. */
static void compute_sizes(odin3_flatten_graph *graph) {
    for (uint32_t pos = graph->n_order; pos > 0; pos--) {
        uint32_t mod = graph->order[pos - 1];
        odin3_flatten_size size = graph->own[mod];
        uint32_t n_edges = 0;
        const flat_edge *edges = edges_of(graph, mod, &n_edges);
        for (uint32_t i = 0; i < n_edges; i++) {
            const odin3_flatten_size *sub = &graph->flat[edges[i].child];
            if (edges[i].kept) {
                size.kept = sat_add(size.kept, 1);
                continue;
            }
            size.nodes = sat_add(size.nodes, sub->nodes);
            size.pins = sat_add(size.pins, sub->pins);
            size.nets = sat_add(size.nets, sub->nets);
            size.wires = sat_add(size.wires, sub->wires);
            size.instances = sat_add(size.instances, sat_add(1, sub->instances));
            size.kept = sat_add(size.kept, sub->kept);
        }
        graph->flat[mod] = size;
    }
    odin3_flatten_totals *tot = &graph->totals;
    for (uint32_t i = 0; i < graph->n_roots; i++) {
        const odin3_flatten_size *size = &graph->flat[graph->roots[i]];
        tot->instances = sat_add(tot->instances, size->instances);
        tot->kept = sat_add(tot->kept, size->kept);
        tot->nodes = sat_add(tot->nodes, size->nodes);
        tot->pins = sat_add(tot->pins, size->pins);
        tot->nets = sat_add(tot->nets, size->nets);
        tot->wires = sat_add(tot->wires, size->wires);
    }
    tot->reach = graph->n_order;
    tot->roots = graph->n_roots;
    tot->unused = graph->n_mod - 1 - graph->n_roots;
}

odin3_status odin3_flatten_graph_build(odin3_design *design, odin3_module_id top,
                                       odin3_flatten_graph **out) {
    uint32_t n_mod = odin3_design_module_end(design);
    if (out == NULL || top.v == 0 || top.v >= n_mod) {
        odin3_log(ODIN3_LOG_ERROR, "flatten: the design has no top module (run hierarchy)");
        return ODIN3_ERR_INVALID_ARG;
    }
    odin3_flatten_graph *graph = odin3_util_calloc(sizeof *graph);
    if (graph == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    graph->design = design;
    graph->n_mod = n_mod;
    graph->top = top.v;
    odin3_vec_init(&graph->edges, sizeof(flat_edge));
    odin3_status st = graph_alloc(graph);
    st = st == ODIN3_OK ? map_types(graph) : st;
    st = st == ODIN3_OK ? scan_modules(graph) : st;
    st = st == ODIN3_OK ? sort_modules(graph) : st;
    st = st == ODIN3_OK ? find_roots(graph) : st;
    if (st != ODIN3_OK) {
        odin3_flatten_graph_free(graph);
        return st;
    }
    compute_sizes(graph);
    *out = graph;
    return ODIN3_OK;
}

/* --- accessors ------------------------------------------------------------------------------ */

odin3_flatten_totals odin3_flatten_graph_totals(const odin3_flatten_graph *graph) {
    return graph->totals;
}

const uint32_t *odin3_flatten_graph_order(const odin3_flatten_graph *graph, uint32_t *n) {
    *n = graph->n_order;
    return graph->order;
}

const uint32_t *odin3_flatten_graph_roots(const odin3_flatten_graph *graph, uint32_t *n) {
    *n = graph->n_roots;
    return graph->roots;
}

odin3_flatten_size odin3_flatten_graph_size(const odin3_flatten_graph *graph, uint32_t mod) {
    return graph->flat[mod];
}

void odin3_flatten_graph_log_unused(const odin3_flatten_graph *graph) {
    if (graph->totals.unused == 0) {
        return;
    }
    odin3_strbuf text;
    odin3_strbuf_init(&text);
    uint32_t listed = 0;
    for (uint32_t mod = 1; mod < graph->n_mod && listed < MAX_UNUSED_NAMES; mod++) {
        if (graph->is_root[mod] == 0) {
            (void)odin3_strbuf_appendf(&text, listed++ == 0 ? "" : ", ");
            name_into(graph, mod, &text);
        }
    }
    odin3_log(ODIN3_LOG_INFO,
              "flatten: %" PRIu32 " modules are no longer instantiated: %s%s (the IR cannot remove "
              "modules)",
              graph->totals.unused, text.data != NULL ? text.data : "",
              graph->totals.unused > listed ? ", ..." : "");
    odin3_strbuf_free(&text);
}

/* --- estimate, caps, budget ------------------------------------------------------------------ */

static uint64_t sat_sub(uint64_t lhs, uint64_t rhs) {
    return lhs > rhs ? lhs - rhs : 0;
}

odin3_flatten_size odin3_flatten_graph_added(const odin3_flatten_graph *graph) {
    odin3_flatten_size added = {0, 0, 0, 0, 0, 0};
    for (uint32_t i = 0; i < graph->n_roots; i++) {
        const odin3_flatten_size *flat = &graph->flat[graph->roots[i]];
        const odin3_flatten_size *own = &graph->own[graph->roots[i]];
        added.nodes = sat_add(added.nodes, sat_sub(flat->nodes, own->nodes));
        added.pins = sat_add(added.pins, sat_sub(flat->pins, own->pins));
        added.nets = sat_add(added.nets, sat_sub(flat->nets, own->nets));
        added.wires = sat_add(added.wires, sat_sub(flat->wires, own->wires));
    }
    return added;
}

uint64_t odin3_flatten_graph_estimate(const odin3_flatten_graph *graph) {
    odin3_flatten_size add = odin3_flatten_graph_added(graph);
    uint64_t bytes = sat_mul(add.nodes, B_NODE + B_NAME);
    bytes = sat_add(bytes, sat_mul(add.pins, B_PIN));
    bytes = sat_add(bytes, sat_mul(add.nets, B_NET));
    bytes = sat_add(bytes, sat_mul(add.wires, B_WIRE + B_NAME));
    return sat_add(bytes, sat_mul(sat_add(add.nodes, sat_add(add.nets, add.wires)), B_REC));
}

static bool over(uint64_t have, uint64_t cap, const char *what) {
    if (have <= cap) {
        return false;
    }
    odin3_log(ODIN3_LOG_ERROR,
              "flatten: the flat design would have %" PRIu64 " %s (cap %" PRIu64 ")", have, what,
              cap);
    return true;
}

odin3_status odin3_flatten_preflight(const odin3_pass_ctx *ctx, const odin3_design *design,
                                     const odin3_flatten_graph *graph) {
    odin3_flatten_limits cap = odin3_flatten_limits_now();
    if (over(graph->n_roots, cap.max_roots, "roots")) {
        return ODIN3_ERR_INVALID_ARG;
    }
    for (uint32_t i = 0; i < graph->n_roots; i++) {
        const odin3_flatten_size *size = &graph->flat[graph->roots[i]];
        if (over(size->nodes, cap.max_nodes, "nodes") || over(size->pins, cap.max_pins, "pins") ||
            over(size->nets, cap.max_nets, "nets") || over(size->wires, cap.max_wires, "wires")) {
            return ODIN3_ERR_INVALID_ARG;
        }
    }
    odin3_status st = odin3_budget_check(ctx, design, odin3_flatten_graph_estimate(graph));
    if (st != ODIN3_OK) {
        odin3_strbuf msg;
        odin3_strbuf_init(&msg);
        if (odin3_budget_message(ctx, &msg) == ODIN3_OK) {
            odin3_log(ODIN3_LOG_ERROR, "flatten: %s", msg.data);
        }
        odin3_strbuf_free(&msg);
    }
    return st;
}
