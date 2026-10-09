/* build_hier.c — module instances of odin3_sim_build: type map and recursive-hierarchy check. */
#include "build_internal.h"
#include "ir/module.h"
#include "util/alloc.h"
#include "util/log.h"
#include "util/str.h"

#include <stdint.h>

uint32_t odin3_sim_child_module(const odin3_sim_builder *bld, const odin3_module *module,
                                odin3_node_id node) {
    uint64_t child = 0;
    if (!odin3_node_live(module, node) ||
        !odin3_u64map_get(bld->types, odin3_node_type(module, node).v, &child)) {
        return 0;
    }
    return (uint32_t)child;
}

static odin3_status map_types(odin3_sim_builder *bld) {
    uint32_t end = odin3_design_module_end(bld->design);
    bld->types = odin3_u64map_create(end);
    if (bld->types == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t mod_id = 1; mod_id < end; mod_id++) {
        const odin3_module *mod = odin3_module_get(bld->design, (odin3_module_id){mod_id});
        odin3_kv entry = {odin3_module_celltype(mod).v, mod_id};
        if (odin3_u64map_put(bld->types, entry) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    return ODIN3_OK;
}

/*
 * The instance graph of the modules reachable from top. order: module IDs in reach order; pos[m]:
 * 1 + the position of module m in order (0: unreached); the edges of order[i] (child positions)
 * are edges[first[i] .. first[i + 1]); indeg: per position.
 */
typedef struct hier_graph {
    uint32_t *pos;
    odin3_vec order;
    odin3_vec first;
    odin3_vec edges;
    uint32_t *indeg;
} hier_graph;

static odin3_status push_u32(odin3_vec *vec, uint32_t val) {
    uint32_t *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = val;
    return ODIN3_OK;
}

/* Appends the edges of module mod (instances in node order), reaching new children. */
static odin3_status reach_children(const odin3_sim_builder *bld, hier_graph *graph,
                                   const odin3_module *mod) {
    uint32_t end = odin3_module_node_end(mod);
    for (uint32_t nid = 1; nid < end; nid++) {
        uint32_t child = odin3_sim_child_module(bld, mod, (odin3_node_id){nid});
        if (child == 0) {
            continue;
        }
        if (graph->pos[child] == 0) {
            graph->pos[child] = (uint32_t)graph->order.len + 1;
            if (push_u32(&graph->order, child) != ODIN3_OK) {
                return ODIN3_ERR_NO_MEMORY;
            }
        }
        if (push_u32(&graph->edges, graph->pos[child] - 1) != ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    return ODIN3_OK;
}

static odin3_status reach(odin3_sim_builder *bld, hier_graph *graph, odin3_module_id top) {
    graph->pos[top.v] = 1;
    if (push_u32(&graph->order, top.v) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (size_t i = 0; i < graph->order.len; i++) {
        uint32_t mod_id = *(const uint32_t *)odin3_vec_cat(&graph->order, i);
        if (push_u32(&graph->first, (uint32_t)graph->edges.len) != ODIN3_OK ||
            reach_children(bld, graph, odin3_module_get(bld->design, (odin3_module_id){mod_id})) !=
                ODIN3_OK) {
            return ODIN3_ERR_NO_MEMORY;
        }
    }
    return push_u32(&graph->first, (uint32_t)graph->edges.len);
}

static uint32_t at(const odin3_vec *vec, size_t index) {
    return *(const uint32_t *)odin3_vec_cat(vec, index);
}

/* Kahn's algorithm over positions; returns how many were peeled (all of them: no recursion). */
static uint32_t peel(hier_graph *graph, uint32_t *queue) {
    uint32_t n_pos = (uint32_t)graph->order.len;
    for (size_t edge = 0; edge < graph->edges.len; edge++) {
        graph->indeg[at(&graph->edges, edge)]++;
    }
    uint32_t tail = 0;
    for (uint32_t i = 0; i < n_pos; i++) {
        if (graph->indeg[i] == 0) {
            queue[tail++] = i;
        }
    }
    for (uint32_t head = 0; head < tail; head++) {
        uint32_t place = queue[head];
        for (uint32_t edge = at(&graph->first, place); edge < at(&graph->first, place + 1);
             edge++) {
            uint32_t child_pos = at(&graph->edges, edge);
            if (--graph->indeg[child_pos] == 0) {
                queue[tail++] = child_pos;
            }
        }
    }
    return tail;
}

/*
 * After peel: every unpeeled position has an unpeeled parent. Records one per position in back,
 * walks back from any unpeeled position until one repeats, and returns that module (on a cycle).
 */
static uint32_t module_on_cycle(const hier_graph *graph, uint32_t *back) {
    uint32_t n_pos = (uint32_t)graph->order.len;
    uint32_t start = 0;
    for (uint32_t place = 0; place < n_pos; place++) {
        back[place] = ODIN3_SIM_UNSET;
    }
    for (uint32_t place = 0; place < n_pos; place++) {
        for (uint32_t edge = at(&graph->first, place);
             graph->indeg[place] != 0 && edge < at(&graph->first, place + 1); edge++) {
            back[at(&graph->edges, edge)] = place;
        }
        start = graph->indeg[place] != 0 ? place : start;
    }
    uint32_t cur = start;
    while (graph->indeg[cur] != 0) {
        graph->indeg[cur] = 0; /* visited */
        cur = back[cur];
    }
    return at(&graph->order, cur);
}

static void report_cycle(const odin3_sim_builder *bld, uint32_t module) {
    const odin3_module *mod = odin3_module_get(bld->design, (odin3_module_id){module});
    odin3_log(ODIN3_LOG_ERROR,
              "cannot simulate a recursive module hierarchy: module `%s` instantiates itself "
              "through its instances",
              odin3_strtab_get(odin3_design_strtab(bld->design), odin3_module_name(mod)));
}

static odin3_status check_graph(odin3_sim_builder *bld, hier_graph *graph, odin3_module_id top) {
    odin3_status st = reach(bld, graph, top);
    if (st != ODIN3_OK) {
        return st;
    }
    size_t nid = graph->order.len;
    graph->indeg = odin3_util_calloc(nid * sizeof *graph->indeg);
    uint32_t *scratch = odin3_util_malloc(nid * sizeof *scratch);
    if (graph->indeg == NULL || scratch == NULL) {
        odin3_util_free(scratch);
        return ODIN3_ERR_NO_MEMORY;
    }
    if (peel(graph, scratch) != nid) {
        report_cycle(bld, module_on_cycle(graph, scratch));
        st = ODIN3_ERR_INVALID_ARG;
    }
    odin3_util_free(scratch);
    return st;
}

odin3_status odin3_sim_check_hierarchy(odin3_sim_builder *bld, odin3_module_id top) {
    odin3_status st = map_types(bld);
    if (st != ODIN3_OK) {
        return st;
    }
    hier_graph graph = {0};
    odin3_vec_init(&graph.order, sizeof(uint32_t));
    odin3_vec_init(&graph.first, sizeof(uint32_t));
    odin3_vec_init(&graph.edges, sizeof(uint32_t));
    graph.pos = odin3_util_calloc((size_t)odin3_design_module_end(bld->design) * sizeof *graph.pos);
    st = graph.pos == NULL ? ODIN3_ERR_NO_MEMORY : check_graph(bld, &graph, top);
    odin3_util_free(graph.pos);
    odin3_util_free(graph.indeg);
    odin3_vec_free(&graph.order);
    odin3_vec_free(&graph.first);
    odin3_vec_free(&graph.edges);
    return st;
}
