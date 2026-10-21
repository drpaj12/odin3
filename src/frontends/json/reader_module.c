/*
 * reader_module.c — modules, ports, black-box modules, the module graph and the top (M-0 to M-2,
 * M-14).
 */
#include "frontends/json/reader_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LIST_MAX = 16 };

uint32_t jr_find_mod(const jr *rd, uint32_t name_str) {
    uint64_t index = 0;
    return odin3_u64map_get(rd->mod_index, name_str, &index) ? (uint32_t)index : UINT32_MAX;
}

static const jr_mod *mod_at(const jr *rd, uint32_t index) {
    return odin3_vec_cat(&rd->mods, index);
}

/* --- ports --------------------------------------------------------------------------------- */

odin3_status jr_shape(const jr *rd, const odin3_json_node *obj, const char *section,
                      jr_shape_out out) {
    const uint32_t width = out.width;
    odin3_wire_spec *spec = out.spec;
    const odin3_json_node *off = jr_member(obj, "offset");
    const odin3_json_node *upto = jr_member(obj, "upto");
    const odin3_json_node *sign = jr_member(obj, "signed");
    int64_t offset = off != NULL && jr_kind(off) == ODIN3_JSON_INT ? jr_int(off) : 0;
    bool up = upto != NULL && jr_kind(upto) == ODIN3_JSON_INT && jr_int(upto) != 0;
    if (offset < INT32_MIN || offset > INT32_MAX ||
        offset + (int64_t)width - 1 > INT32_MAX) { /* offset is in int32 range: no overflow */
        return jr_error(rd, off, &(jr_where){section, odin3_json_key(obj), "offset"},
                        "offset %lld is out of range", (long long)offset);
    }
    int64_t high = offset + (int64_t)width - 1;
    spec->msb = (int32_t)(up ? offset : high);
    spec->lsb = (int32_t)(up ? high : offset);
    spec->is_signed = sign != NULL && jr_kind(sign) == ODIN3_JSON_INT && jr_int(sign) != 0;
    return ODIN3_OK;
}

static odin3_status port_dir(const jr *rd, const odin3_json_node *port, const jr_where *where,
                             odin3_dir *dir) {
    const odin3_json_node *node = jr_member(port, "direction");
    const char *text =
        node != NULL && jr_kind(node) == ODIN3_JSON_STRING ? jr_text(node, NULL) : NULL;
    if (text == NULL) {
        return jr_error(rd, port, where, "the port has no direction");
    }
    if (strcmp(text, "input") == 0) {
        *dir = ODIN3_DIR_IN;
    } else if (strcmp(text, "output") == 0) {
        *dir = ODIN3_DIR_OUT;
    } else if (strcmp(text, "inout") == 0) {
        *dir = ODIN3_DIR_INOUT;
    } else {
        return jr_error(rd, node, where, "direction '%s' is not input, output or inout", text);
    }
    return ODIN3_OK;
}

/* The members every port is read from. */
static const char *const PORT_KEYS[] = {"direction", "bits", "offset", "upto", "signed"};

static odin3_status add_port(jr *rd, odin3_module *module, const odin3_json_node *port,
                             const jr_port_srcs *srcs) {
    jr_where where = {"ports", odin3_json_key(port), NULL};
    if (jr_kind(port) != ODIN3_JSON_OBJECT) {
        return jr_error(rd, port, &where, "the port is not an object");
    }
    odin3_port_spec spec = {.width = 0};
    odin3_wire_spec shape = {0};
    odin3_status st = jr_audit(rd, port, PORT_KEYS, sizeof PORT_KEYS / sizeof PORT_KEYS[0]);
    st = st == ODIN3_OK ? port_dir(rd, port, &where, &spec.dir) : st;
    uint32_t width = 0;
    if (st == ODIN3_OK) {
        st = jr_bits_count(rd, jr_member(port, "bits"), &(jr_where){"ports", where.key, "bits"},
                           false, &width);
    }
    if (st == ODIN3_OK) {
        st = jr_shape(rd, port, "ports", (jr_shape_out){width, &shape});
    }
    odin3_prov_id imported = {0};
    if (st == ODIN3_OK) {
        st = jr_prov(rd, port, &imported);
    }
    if (st == ODIN3_OK) {
        st = jr_port_prov(rd, srcs, port, imported, &spec.prov); /* D-2: its netname's src */
    }
    if (st == ODIN3_OK) {
        st = jr_intern(rd, odin3_json_key(port), &spec.name);
    }
    if (st != ODIN3_OK) {
        return st;
    }
    spec.has_range = !(shape.msb == (int32_t)width - 1 && shape.lsb == 0);
    spec.width = width;
    spec.scalar = width == 1 && !spec.has_range;
    spec.msb = shape.msb;
    spec.lsb = shape.lsb;
    spec.is_signed = shape.is_signed;
    st = odin3_module_add_port(module, &spec, NULL);
    return st == ODIN3_OK ? st : jr_fail(rd, st);
}

static odin3_status add_ports(jr *rd, odin3_module *module, const odin3_json_node *mod) {
    const odin3_json_node *ports = jr_member(mod, "ports");
    if (ports != NULL && jr_kind(ports) != ODIN3_JSON_OBJECT) {
        return jr_error(rd, ports, &(jr_where){"ports", NULL, NULL}, "ports is not an object");
    }
    jr_port_srcs srcs;
    odin3_status st = jr_port_srcs_open(rd, mod, &srcs);
    for (const odin3_json_node *port = jr_first(ports); st == ODIN3_OK && port != NULL;
         port = odin3_json_next(port)) {
        st = add_port(rd, module, port, &srcs);
    }
    jr_port_srcs_close(&srcs);
    return st;
}

/* --- attributes of a module ---------------------------------------------------------------- */

/* J-4: parameter_default_values are kept as attributes parameter_default:<NAME>. */
static odin3_status add_default_values(jr *rd, odin3_module *module, odin3_module_id id,
                                       const odin3_json_node *mod) {
    const odin3_json_node *defaults = jr_member(mod, "parameter_default_values");
    if (defaults != NULL && jr_kind(defaults) != ODIN3_JSON_OBJECT) {
        return jr_error(rd, defaults, &(jr_where){NULL, NULL, "parameter_default_values"},
                        "parameter_default_values is not an object");
    }
    odin3_status st = ODIN3_OK;
    for (const odin3_json_node *def = jr_first(defaults); st == ODIN3_OK && def != NULL;
         def = odin3_json_next(def)) {
        char key[JR_NAME_BUF];
        (void)snprintf(key, sizeof key, "parameter_default:%s", odin3_json_key(def));
        jr_where where = {NULL, NULL, "parameter_default_values"};
        odin3_value val;
        st = jr_attr_value(rd, def, &where, &val);
        if (st == ODIN3_OK) {
            st = jr_set_attr_named(rd, module, (odin3_objref){ODIN3_OBJ_MODULE, id.v}, key, &val);
        }
    }
    return st;
}

/* --- modules and black boxes --------------------------------------------------------------- */

/* One port of a black-box module: its name, direction and width (as `.blackbox` declares). */
static odin3_status blackbox_port(const jr *rd, const odin3_json_node *port, odin3_port_def *def) {
    jr_where where = {"ports", odin3_json_key(port), "bits"};
    if (jr_kind(port) != ODIN3_JSON_OBJECT) {
        return jr_error(rd, port, &where, "the port is not an object");
    }
    uint32_t width = 0;
    odin3_status st = port_dir(rd, port, &where, &def->dir);
    if (st == ODIN3_OK) {
        st = jr_bits_count(rd, jr_member(port, "bits"), &where, false, &width);
    }
    def->name = odin3_json_key(port);
    def->width = width;
    def->scalar = width == 1;
    return st;
}

/* A black-box module: a cell type with the ports of the module (as `.blackbox` makes one). */
static odin3_status declare_blackbox(jr *rd, const jr_mod *mod) {
    odin3_vec defs;
    odin3_vec_init(&defs, sizeof(odin3_port_def));
    odin3_status st = ODIN3_OK;
    const odin3_json_node *ports = jr_member(mod->node, "ports");
    for (const odin3_json_node *port = jr_first(ports); st == ODIN3_OK && port != NULL;
         port = odin3_json_next(port)) {
        odin3_port_def *def = odin3_vec_push(&defs);
        st = def != NULL ? blackbox_port(rd, port, def) : jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    odin3_celltype_id type = {0};
    odin3_celltype_def def = {.name = mod->name,
                              .gran = ODIN3_GRAN_BLACKBOX,
                              .ports = defs.data,
                              .n_ports = (uint32_t)defs.len};
    if (st == ODIN3_OK) {
        st = odin3_celltype_declare_blackbox(rd->design, &def, &type);
        st = st == ODIN3_ERR_INVALID_ARG
                 ? jr_error(rd, mod->node, NULL, "black box '%s' cannot be declared", mod->name)
                 : jr_fail(rd, st);
    }
    odin3_vec_free(&defs);
    return st;
}

static odin3_status make_module(jr *rd, jr_mod *mod) {
    odin3_celltype_id taken = {0};
    if (odin3_celltype_find(rd->design, mod->name_str, &taken)) {
        return jr_error(rd, mod->node, NULL, "module '%s' has the name of a cell type", mod->name);
    }
    odin3_prov_id prov = {0};
    odin3_status st = jr_prov(rd, mod->node, &mod->base);
    if (st == ODIN3_OK) {
        st = jr_restore_src(
            rd, &(jr_restore_in){jr_member(mod->node, "attributes"), mod->base, true, NULL}, &prov);
    }
    if (st == ODIN3_OK) {
        st = jr_fail(rd, odin3_module_create(rd->design, mod->name_str, prov, &mod->module));
    }
    odin3_module *module = st == ODIN3_OK ? odin3_module_get(rd->design, mod->module) : NULL;
    if (st == ODIN3_OK) {
        st = jr_set_attrs(rd, module, (odin3_objref){ODIN3_OBJ_MODULE, mod->module.v},
                          jr_member(mod->node, "attributes"), &(jr_where){0});
    }
    if (st == ODIN3_OK) {
        st = add_default_values(rd, module, mod->module, mod->node);
    }
    return st == ODIN3_OK ? add_ports(rd, module, mod->node) : st;
}

static const char *const MODULE_KEYS[] = {
    "attributes", "parameter_default_values", "ports", "cells", "memories", "netnames"};

/* Reads the facts pass 1 needs of a module object into a jr_mod. */
static odin3_status describe(jr *rd, const odin3_json_node *obj, jr_mod *mod) {
    *mod = (jr_mod){.node = obj, .name = odin3_json_key(obj)};
    if (jr_kind(obj) != ODIN3_JSON_OBJECT) {
        return jr_error(rd, obj, NULL, "the module is not an object");
    }
    odin3_status st = jr_audit(rd, obj, MODULE_KEYS, sizeof MODULE_KEYS / sizeof MODULE_KEYS[0]);
    if (st != ODIN3_OK) {
        return st;
    }
    const odin3_json_node *attrs = jr_member(obj, "attributes");
    const odin3_json_node *cells = jr_member(obj, "cells");
    const odin3_json_node *black = attrs != NULL ? jr_member(attrs, "blackbox") : NULL;
    const odin3_json_node *top = attrs != NULL ? jr_member(attrs, "top") : NULL;
    mod->has_cells = cells != NULL && jr_count(cells) > 0;
    mod->blackbox = black != NULL && jr_attr_true(black) && !mod->has_cells;
    mod->top_attr = top != NULL && jr_attr_true(top);
    return jr_intern(rd, mod->name, &mod->name_str);
}

static odin3_status read_one_module(jr *rd, const odin3_json_node *obj, uint32_t index) {
    jr_mod *mod = odin3_vec_push(&rd->mods);
    if (mod == NULL) {
        return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    rd->cur_name = odin3_json_key(obj);
    odin3_status st = describe(rd, obj, mod);
    if (st == ODIN3_OK) {
        st = jr_fail(rd, odin3_u64map_put(rd->mod_index, (odin3_kv){mod->name_str, index}));
    }
    if (st == ODIN3_OK) {
        st = mod->blackbox ? declare_blackbox(rd, mod) : make_module(rd, mod);
    }
    return st;
}

odin3_status jr_read_modules(jr *rd) {
    odin3_status st = ODIN3_OK;
    uint32_t index = 0;
    for (const odin3_json_node *obj = jr_first(rd->modules); st == ODIN3_OK && obj != NULL;
         obj = odin3_json_next(obj)) {
        st = read_one_module(rd, obj, index++);
    }
    rd->cur_name = NULL;
    return st;
}

/* --- cell types and instantiation edges (before pass 2) -------------------------------------- */

static odin3_status add_edge(jr *rd, uint32_t from, uint32_t to) {
    jr_edge *edge = odin3_vec_push(&rd->edges);
    if (edge == NULL) {
        return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    *edge = (jr_edge){from, to};
    return ODIN3_OK;
}

/* One cell: a type that is not a module, a registered type or a black box is the error of M-5. */
static odin3_status scan_cell_type(jr *rd, const odin3_json_node *cell, uint32_t from) {
    jr_where where = {"cells", odin3_json_key(cell), NULL};
    const odin3_json_node *type =
        jr_kind(cell) == ODIN3_JSON_OBJECT ? jr_member(cell, "type") : NULL;
    if (type == NULL || jr_kind(type) != ODIN3_JSON_STRING) {
        return jr_error(rd, cell, &where, "the cell has no type string");
    }
    const char *name = jr_text(type, NULL);
    odin3_celltype_id id = {0};
    if (strcmp(name, "$scopeinfo") == 0) {
        rd->n_scopeinfo++;
        return ODIN3_OK;
    }
    if (!jr_lookup_type(rd, name, &id)) {
        return jr_note_implicit(rd, cell, name);
    }
    const odin3_celltype_def *def = odin3_celltype_get(rd->design, id);
    if (def != NULL && def->gran == ODIN3_GRAN_PORT) {
        return jr_error(rd, type, &where, "'%s' is a port type, not a cell", name);
    }
    uint32_t to = UINT32_MAX;
    uint32_t str = 0;
    if (odin3_strtab_find(odin3_design_strtab(rd->design), odin3_bytes_cstr(name), &str)) {
        to = jr_find_mod(rd, str);
    }
    return to != UINT32_MAX && !mod_at(rd, to)->blackbox ? add_edge(rd, from, to) : ODIN3_OK;
}

odin3_status jr_scan_types(jr *rd) {
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < rd->mods.len; i++) {
        const jr_mod *mod = mod_at(rd, i);
        const odin3_json_node *cells = jr_member(mod->node, "cells");
        rd->cur_name = mod->name;
        if (cells != NULL && jr_kind(cells) != ODIN3_JSON_OBJECT) {
            st = jr_error(rd, cells, &(jr_where){"cells", NULL, NULL}, "cells is not an object");
        } else if (cells != NULL && jr_count(cells) > JR_MAX_OBJECTS) {
            st = jr_error(rd, cells, &(jr_where){"cells", NULL, NULL}, "more than %u cells",
                          (unsigned)JR_MAX_OBJECTS);
        }
        for (const odin3_json_node *cell = jr_first(cells); st == ODIN3_OK && cell != NULL;
             cell = odin3_json_next(cell)) {
            st = scan_cell_type(rd, cell, i);
        }
    }
    rd->cur_name = NULL;
    return st;
}

/* --- cycles (M-14): an iterative depth-first search over the instantiation edges ------------ */

typedef struct mod_graph {
    uint32_t n;
    uint32_t *start; /* n + 1: the edges of m are adj[start[m] .. start[m + 1]) */
    uint32_t *adj;
} mod_graph;

static void graph_free(mod_graph *graph) {
    free(graph->start);
    free(graph->adj);
    *graph = (mod_graph){0};
}

/* The adjacency of the edges by counting sort. */
static odin3_status graph_build(jr *rd, mod_graph *graph) {
    graph->n = (uint32_t)rd->mods.len;
    graph->start = calloc((size_t)graph->n + 2, sizeof(uint32_t));
    graph->adj = malloc((rd->edges.len + 1) * sizeof(uint32_t));
    if (graph->start == NULL || graph->adj == NULL) {
        graph_free(graph);
        return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    for (size_t i = 0; i < rd->edges.len; i++) {
        graph->start[((const jr_edge *)odin3_vec_cat(&rd->edges, i))->from + 2]++;
    }
    for (uint32_t at = 2; at <= graph->n + 1; at++) {
        graph->start[at] += graph->start[at - 1];
    }
    for (size_t i = 0; i < rd->edges.len; i++) {
        const jr_edge *edge = odin3_vec_cat(&rd->edges, i);
        graph->adj[graph->start[edge->from + 1]++] = edge->to;
    }
    return ODIN3_OK;
}

typedef struct dfs {
    uint8_t *color; /* 0 new, 1 on the stack, 2 done */
    uint32_t *pos;  /* next edge to try of each module */
    uint32_t *stack;
} dfs;

/* Reports the cycle closed by the edge top -> target. */
static odin3_status report_cycle(const jr *rd, uint32_t from, uint32_t target) {
    const jr_mod *loop = mod_at(rd, target);
    if (from == target) {
        return jr_error(rd, loop->node, NULL, "module '%s' instantiates itself", loop->name);
    }
    return jr_error(rd, loop->node, NULL, "module '%s' instantiates itself through '%s'",
                    loop->name, mod_at(rd, from)->name);
}

/* One step of the search from the module on top of the stack; *depth is the stack size. */
static odin3_status dfs_step(const jr *rd, const mod_graph *graph, dfs *state, uint32_t *depth) {
    uint32_t cur = state->stack[*depth - 1];
    if (state->pos[cur] == graph->start[cur + 1]) {
        state->color[cur] = 2;
        (*depth)--;
        return ODIN3_OK;
    }
    uint32_t next = graph->adj[state->pos[cur]++];
    if (state->color[next] == 1) {
        return report_cycle(rd, cur, next);
    }
    if (state->color[next] == 0) {
        state->color[next] = 1;
        state->stack[(*depth)++] = next;
    }
    return ODIN3_OK;
}

static odin3_status find_cycle(const jr *rd, const mod_graph *graph, dfs *state) {
    for (uint32_t root = 0; root < graph->n; root++) {
        if (state->color[root] != 0) {
            continue;
        }
        uint32_t depth = 0;
        state->color[root] = 1;
        state->stack[depth++] = root;
        while (depth > 0) {
            odin3_status st = dfs_step(rd, graph, state, &depth);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

odin3_status jr_check_cycles(jr *rd) {
    mod_graph graph = {0};
    odin3_status st = rd->edges.len > 0 ? graph_build(rd, &graph) : ODIN3_OK;
    if (st != ODIN3_OK || rd->edges.len == 0) {
        return st;
    }
    dfs state = {calloc(graph.n, 1), malloc(graph.n * sizeof(uint32_t)),
                 malloc(graph.n * sizeof(uint32_t))};
    if (state.color == NULL || state.pos == NULL || state.stack == NULL) {
        st = jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    } else {
        for (uint32_t mod_idx = 0; mod_idx < graph.n; mod_idx++) {
            state.pos[mod_idx] = graph.start[mod_idx];
        }
        st = find_cycle(rd, &graph, &state);
    }
    free(state.color);
    free(state.pos);
    free(state.stack);
    graph_free(&graph);
    return st;
}

/* --- the top (M-1) ---------------------------------------------------------------------------- */

static odin3_status set_top(jr *rd, uint32_t index) {
    odin3_log(ODIN3_LOG_INFO, "read_json: top %s", mod_at(rd, index)->name);
    return jr_fail(rd, odin3_design_set_top(rd->design, mod_at(rd, index)->module));
}

/* The names of the modules in `chosen` (a flag per module), joined, at most LIST_MAX of them. */
static odin3_status candidate_error(jr *rd, const bool *chosen, const char *why) {
    odin3_strbuf list;
    odin3_strbuf_init(&list);
    uint32_t count = 0;
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < rd->mods.len; i++) {
        if (chosen[i] && count++ < LIST_MAX) {
            st = odin3_strbuf_appendf(&list, "%s%s", list.len > 0 ? ", " : "", mod_at(rd, i)->name);
        }
    }
    if (st == ODIN3_OK) {
        st = jr_error(rd, rd->modules, NULL, "cannot choose a top module: %s: %s%s (use --top)",
                      why, list.data != NULL ? list.data : "", count > LIST_MAX ? ", ..." : "");
    }
    odin3_strbuf_free(&list);
    return st == ODIN3_ERR_PARSE ? st : jr_fail(rd, st);
}

/* The module named by --top. */
static odin3_status named_top(jr *rd) {
    uint32_t str = 0;
    uint32_t index = UINT32_MAX;
    if (odin3_strtab_find(odin3_design_strtab(rd->design), odin3_bytes_cstr(rd->opts.top), &str)) {
        index = jr_find_mod(rd, str);
    }
    if (index == UINT32_MAX || mod_at(rd, index)->blackbox) {
        return jr_error(rd, rd->modules, NULL, "--top: no module named '%s'", rd->opts.top);
    }
    return set_top(rd, index);
}

/* The flags of every module: has the top attribute, or is instantiated (or a black box). */
typedef struct top_marks {
    bool *flag;
    bool *used;
    uint32_t n_flag, n_free, n_ir;
} top_marks;

static void mark_tops(const jr *rd, top_marks *marks) {
    for (size_t i = 0; i < rd->edges.len; i++) {
        marks->used[((const jr_edge *)odin3_vec_cat(&rd->edges, i))->to] = true;
    }
    for (uint32_t i = 0; i < rd->mods.len; i++) {
        const jr_mod *mod = mod_at(rd, i);
        marks->flag[i] = mod->top_attr && !mod->blackbox;
        marks->used[i] = marks->used[i] || mod->blackbox;
        marks->n_flag += marks->flag[i] ? 1U : 0U;
        marks->n_free += marks->used[i] ? 0U : 1U;
        marks->n_ir += mod->blackbox ? 0U : 1U;
    }
}

/* The index of the one module whose flag[] is set. */
static uint32_t first_flagged(const jr *rd, const bool *flag) {
    uint32_t i = 0;
    while (i < rd->mods.len && !flag[i]) {
        i++;
    }
    return i;
}

static odin3_status pick_top(jr *rd, const top_marks *marks) {
    if (marks->n_flag == 1) {
        return set_top(rd, first_flagged(rd, marks->flag));
    }
    if (marks->n_flag > 1) {
        return candidate_error(rd, marks->flag, "several modules have the top attribute");
    }
    if (marks->n_free == 1) {
        bool *none = calloc(rd->mods.len + 1, sizeof(bool));
        if (none == NULL) {
            return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
        }
        for (uint32_t i = 0; i < rd->mods.len; i++) {
            none[i] = !marks->used[i];
        }
        odin3_status st = set_top(rd, first_flagged(rd, none));
        free(none);
        return st;
    }
    if (marks->n_ir == 0) {
        return ODIN3_OK; /* only black boxes: no top */
    }
    bool *open = calloc(rd->mods.len + 1, sizeof(bool));
    if (open == NULL) {
        return jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    }
    for (uint32_t i = 0; i < rd->mods.len; i++) {
        open[i] = !marks->used[i];
    }
    odin3_status st = candidate_error(rd, open, "no unique uninstantiated module");
    free(open);
    return st;
}

static odin3_status auto_top(jr *rd) {
    top_marks marks = {calloc(rd->mods.len + 1, sizeof(bool)),
                       calloc(rd->mods.len + 1, sizeof(bool)), 0, 0, 0};
    odin3_status st = ODIN3_OK;
    if (marks.flag == NULL || marks.used == NULL) {
        st = jr_fail(rd, ODIN3_ERR_NO_MEMORY);
    } else {
        mark_tops(rd, &marks);
        st = pick_top(rd, &marks);
    }
    free(marks.flag);
    free(marks.used);
    return st;
}

odin3_status jr_choose_top(jr *rd) {
    if (rd->opts.top != NULL) {
        return named_top(rd);
    }
    return auto_top(rd);
}

/* --- pass 2 of one module ------------------------------------------------------------------------
 */

odin3_status jr_read_module_body(jr *rd, uint32_t index) {
    const jr_mod *mod = mod_at(rd, index);
    rd->cur_name = mod->name;
    odin3_status st = jr_cur_begin(rd, mod);
    if (st == ODIN3_OK) {
        st = jr_names_begin(rd, mod->node);
    }
    if (st == ODIN3_OK) {
        st = jr_scan_bits(rd);
    }
    if (st == ODIN3_OK) {
        st = jr_make_nets(rd);
    }
    if (st == ODIN3_OK) {
        st = jr_read_netnames(rd, jr_member(mod->node, "netnames"));
    }
    if (st == ODIN3_OK) {
        st = jr_read_cells(rd, jr_member(mod->node, "cells"));
    }
    if (st == ODIN3_OK) {
        st = jr_read_memories(rd, jr_member(mod->node, "memories"));
    }
    jr_names_end(rd);
    jr_cur_end(rd);
    return st;
}
