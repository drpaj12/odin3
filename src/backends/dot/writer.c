/* writer.c — Graphviz dot writer: clusters per module, focus selection, node budget. */
#include "backends/dot/writer.h"

#include "ir/celltype.h"
#include "ir/module.h"
#include "ir/prov.h"
#include "util/alloc.h"
#include "util/file.h"
#include "util/log.h"
#include "util/str.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The nodes to draw: one byte per node ID of each module (indexed by module ID). */
typedef struct dot_mask {
    uint8_t *bytes;
} dot_mask;

typedef struct dot_ctx {
    odin3_design *design;
    odin3_strtab *tab;
    dot_mask *mask;
    uint32_t mod_end;
} dot_ctx;

enum { DEC_BASE = 10 };

static const odin3_module *dot_module(const dot_ctx *dc, uint32_t id) {
    return odin3_module_get(dc->design, (odin3_module_id){id});
}

static void dot_masks_free(dot_ctx *dc) {
    if (dc->mask == NULL) {
        return;
    }
    for (uint32_t i = 0; i < dc->mod_end; i++) {
        odin3_util_free(dc->mask[i].bytes);
    }
    odin3_util_free(dc->mask);
    dc->mask = NULL;
}

/* Every live node selected. Mask slot 0 stays unused. */
static odin3_status dot_masks_alloc(dot_ctx *dc) {
    dc->mod_end = odin3_design_module_end(dc->design);
    dc->mask = odin3_util_calloc(sizeof(dot_mask) * dc->mod_end);
    if (dc->mask == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 1; i < dc->mod_end; i++) {
        const odin3_module *mod = dot_module(dc, i);
        uint32_t end = odin3_module_node_end(mod);
        dc->mask[i].bytes = odin3_util_calloc((size_t)end + 1);
        if (dc->mask[i].bytes == NULL) {
            dot_masks_free(dc);
            return ODIN3_ERR_NO_MEMORY;
        }
        for (uint32_t j = 1; j < end; j++) {
            dc->mask[i].bytes[j] = odin3_node_live(mod, (odin3_node_id){j}) ? 1 : 0;
        }
    }
    return ODIN3_OK;
}

static void dot_masks_clear(dot_ctx *dc) {
    for (uint32_t i = 1; i < dc->mod_end; i++) {
        memset(dc->mask[i].bytes, 0, (size_t)odin3_module_node_end(dot_module(dc, i)) + 1);
    }
}

static size_t dot_masks_count(const dot_ctx *dc) {
    size_t total = 0;
    for (uint32_t i = 1; i < dc->mod_end; i++) {
        uint32_t end = odin3_module_node_end(dot_module(dc, i));
        for (uint32_t j = 1; j < end; j++) {
            total += dc->mask[i].bytes[j];
        }
    }
    return total;
}

static odin3_status dot_invalid(const char *what, const char *value) {
    odin3_log(ODIN3_LOG_ERROR, "dot: %s '%s'", what, value != NULL ? value : "(null)");
    return ODIN3_ERR_INVALID_ARG;
}

/* --- focus: path --------------------------------------------------------------------------- */

static const odin3_module *dot_find_module(const dot_ctx *dc, odin3_bytes name) {
    uint32_t str = 0;
    if (name.len == 0 || !odin3_strtab_find(dc->tab, name, &str)) {
        return NULL;
    }
    for (uint32_t i = 1; i < dc->mod_end; i++) {
        if (odin3_module_name(dot_module(dc, i)) == str) {
            return dot_module(dc, i);
        }
    }
    return NULL;
}

/* The module a node instantiates, NULL when it is not a module instance. */
static const odin3_module *dot_instance_module(const dot_ctx *dc, const odin3_module *mod,
                                               odin3_node_id node) {
    const odin3_celltype_def *def = odin3_celltype_get(dc->design, odin3_node_type(mod, node));
    if (def == NULL || def->gran != ODIN3_GRAN_MODULE) {
        return NULL;
    }
    return dot_find_module(dc, odin3_bytes_cstr(def->name));
}

/* Walks "module/inst/inst..." from the module named by the first component. */
static const odin3_module *dot_resolve_path(const dot_ctx *dc, const char *path) {
    const odin3_module *cur = NULL;
    const char *part = path;
    while (part != NULL) {
        const char *slash = strchr(part, '/');
        odin3_bytes comp = {part, slash != NULL ? (size_t)(slash - part) : strlen(part)};
        part = slash != NULL ? slash + 1 : NULL;
        uint32_t str = 0;
        if (cur == NULL) {
            cur = dot_find_module(dc, comp);
        } else if (comp.len > 0 && odin3_strtab_find(dc->tab, comp, &str)) {
            odin3_node_id node = odin3_module_find_node(cur, str);
            cur = odin3_node_valid(node) ? dot_instance_module(dc, cur, node) : NULL;
        } else {
            cur = NULL;
        }
        if (cur == NULL) {
            return NULL;
        }
    }
    return cur;
}

typedef struct dot_walk {
    uint8_t *seen;  /* module IDs queued */
    odin3_vec work; /* uint32_t module IDs */
} dot_walk;

static odin3_status dot_walk_push(dot_walk *walk, uint32_t id) {
    if (id == 0 || walk->seen[id]) {
        return ODIN3_OK;
    }
    uint32_t *slot = odin3_vec_push(&walk->work);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = id;
    walk->seen[id] = 1;
    return ODIN3_OK;
}

/* Selects the module's live nodes and queues the modules they instantiate. */
static odin3_status dot_walk_visit(dot_ctx *dc, dot_walk *walk, uint32_t id) {
    const odin3_module *mod = dot_module(dc, id);
    for (uint32_t j = 1; j < odin3_module_node_end(mod); j++) {
        odin3_node_id node = {j};
        if (!odin3_node_live(mod, node)) {
            continue;
        }
        dc->mask[id].bytes[j] = 1;
        const odin3_module *sub = dot_instance_module(dc, mod, node);
        odin3_status st = sub != NULL ? dot_walk_push(walk, odin3_module_id_of(sub).v) : ODIN3_OK;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Selects every node of the module and of the modules it instantiates, transitively. */
static odin3_status dot_select_subtree(dot_ctx *dc, const odin3_module *root) {
    dot_walk walk = {odin3_util_calloc(dc->mod_end), {0}};
    odin3_vec_init(&walk.work, sizeof(uint32_t));
    odin3_status st =
        walk.seen != NULL ? dot_walk_push(&walk, odin3_module_id_of(root).v) : ODIN3_ERR_NO_MEMORY;
    while (st == ODIN3_OK && walk.work.len > 0) {
        uint32_t id = *(const uint32_t *)odin3_vec_cat(&walk.work, walk.work.len - 1);
        odin3_vec_pop(&walk.work);
        st = dot_walk_visit(dc, &walk, id);
    }
    odin3_vec_free(&walk.work);
    odin3_util_free(walk.seen);
    return st;
}

static odin3_status dot_focus_path(dot_ctx *dc, const char *path) {
    const odin3_module *root = dot_resolve_path(dc, path);
    if (root == NULL) {
        return dot_invalid("no module or instance for path", path);
    }
    dot_masks_clear(dc);
    return dot_select_subtree(dc, root);
}

/* --- focus: file:line ---------------------------------------------------------------------- */

/* Splits "file:line" at the last ':'; the line is decimal digits that fit 32 bits. */
static bool dot_parse_loc(const char *text, odin3_bytes *file, uint32_t *line) {
    const char *colon = strrchr(text, ':');
    if (colon == NULL || colon == text || colon[1] == '\0') {
        return false;
    }
    uint64_t value = 0;
    for (const char *at = colon + 1; *at != '\0'; at++) {
        if (*at < '0' || *at > '9') {
            return false;
        }
        value = value * DEC_BASE + (uint64_t)(*at - '0');
        if (value > UINT32_MAX) {
            return false;
        }
    }
    file->ptr = text;
    file->len = (size_t)(colon - text);
    *line = (uint32_t)value;
    return true;
}

/* Selects exactly the live nodes the provenance forward index returns for the location. */
static odin3_status dot_focus_loc(dot_ctx *dc, const char *text) {
    odin3_bytes file = {NULL, 0};
    odin3_srcloc loc = {0, 0, 0, 0, 0, 0};
    if (!dot_parse_loc(text, &file, &loc.line)) {
        return dot_invalid("focus is not file:line:", text);
    }
    dot_masks_clear(dc);
    if (!odin3_strtab_find(dc->tab, file, &loc.file)) {
        return ODIN3_OK; /* a file no object came from: nothing to draw */
    }
    odin3_prov_index *ix = odin3_prov_index_build(dc->design);
    if (ix == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_prov_hits hits = odin3_prov_index_by_loc(ix, loc);
    for (uint32_t i = 0; i < hits.count; i++) {
        const odin3_prov_hit *hit = &hits.hits[i];
        if (hit->obj.kind == ODIN3_OBJ_NODE && hit->live) {
            dc->mask[hit->module.v].bytes[hit->obj.id] = 1;
        }
    }
    odin3_prov_index_destroy(ix);
    return ODIN3_OK;
}

/* --- focus: cone(net) ---------------------------------------------------------------------- */

typedef struct dot_cone {
    const odin3_module *mod;
    uint8_t *nodes; /* the module's mask */
    uint8_t *seen;  /* nets queued */
    odin3_vec work; /* uint32_t net IDs */
} dot_cone;

static odin3_status dot_cone_push(dot_cone *cone, odin3_net_id net) {
    if (!odin3_net_valid(net) || cone->seen[net.v]) {
        return ODIN3_OK;
    }
    uint32_t *slot = odin3_vec_push(&cone->work);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = net.v;
    cone->seen[net.v] = 1;
    return ODIN3_OK;
}

/* Takes the node into the cone and queues the nets its input pins read. */
static odin3_status dot_cone_node(dot_cone *cone, odin3_node_id node) {
    cone->nodes[node.v] = 1;
    odin3_pinslice pins = odin3_node_pins(cone->mod, node);
    for (uint32_t k = 0; k < pins.count; k++) {
        odin3_pin_id pin = {pins.first.v + k};
        if (!odin3_pin_reads(cone->mod, pin)) {
            continue;
        }
        odin3_status st = dot_cone_push(cone, odin3_pin_net(cone->mod, pin));
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

static odin3_status dot_cone_net(dot_cone *cone, odin3_net_id net) {
    odin3_pinlist pins = odin3_net_pins(cone->mod, net);
    uint32_t drivers = odin3_net_driver_count(cone->mod, net);
    for (uint32_t k = 0; k < drivers; k++) {
        odin3_node_id node = odin3_pin_node(cone->mod, pins.pins[k]);
        if (cone->nodes[node.v]) {
            continue;
        }
        odin3_status st = dot_cone_node(cone, node);
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Iterative fan-in walk from a net: an explicit worklist of nets, no recursion. */
static odin3_status dot_select_cone(dot_ctx *dc, const odin3_module *mod, odin3_net_id root) {
    dot_cone cone = {mod, dc->mask[odin3_module_id_of(mod).v].bytes, NULL, {0}};
    odin3_vec_init(&cone.work, sizeof(uint32_t));
    cone.seen = odin3_util_calloc((size_t)odin3_module_net_end(mod) + 1);
    odin3_status st = cone.seen != NULL ? dot_cone_push(&cone, root) : ODIN3_ERR_NO_MEMORY;
    while (st == ODIN3_OK && cone.work.len > 0) {
        odin3_net_id net = {*(const uint32_t *)odin3_vec_cat(&cone.work, cone.work.len - 1)};
        odin3_vec_pop(&cone.work);
        st = dot_cone_net(&cone, net);
    }
    odin3_vec_free(&cone.work);
    odin3_util_free(cone.seen);
    return st;
}

/* A live net by name in one module (or none). */
static odin3_net_id dot_net_in(const dot_ctx *dc, const odin3_module *mod, odin3_bytes name) {
    uint32_t str = 0;
    if (name.len == 0 || !odin3_strtab_find(dc->tab, name, &str)) {
        return (odin3_net_id){0};
    }
    return odin3_module_find_net(mod, str);
}

/* "net" (the first module that has it) or "module:net". */
static const odin3_module *dot_resolve_net(const dot_ctx *dc, const char *text, odin3_net_id *net) {
    for (uint32_t i = 1; i < dc->mod_end; i++) {
        *net = dot_net_in(dc, dot_module(dc, i), odin3_bytes_cstr(text));
        if (odin3_net_valid(*net)) {
            return dot_module(dc, i);
        }
    }
    const char *colon = strchr(text, ':');
    if (colon == NULL) {
        return NULL;
    }
    const odin3_module *mod = dot_find_module(dc, (odin3_bytes){text, (size_t)(colon - text)});
    if (mod == NULL) {
        return NULL;
    }
    *net = dot_net_in(dc, mod, odin3_bytes_cstr(colon + 1));
    return odin3_net_valid(*net) ? mod : NULL;
}

static odin3_status dot_focus_cone(dot_ctx *dc, const char *text) {
    odin3_net_id net = {0};
    const odin3_module *mod = dot_resolve_net(dc, text, &net);
    if (mod == NULL) {
        return dot_invalid("no net for cone", text);
    }
    dot_masks_clear(dc);
    return dot_select_cone(dc, mod, net);
}

static odin3_status dot_apply_focus(dot_ctx *dc, const odin3_dot_opts *opts) {
    if (opts->focus == NULL) {
        return dot_invalid("focus value missing for kind", "(null)");
    }
    switch (opts->focus_kind) {
    case ODIN3_DOT_FOCUS_PATH:
        return dot_focus_path(dc, opts->focus);
    case ODIN3_DOT_FOCUS_LOC:
        return dot_focus_loc(dc, opts->focus);
    case ODIN3_DOT_FOCUS_CONE:
        return dot_focus_cone(dc, opts->focus);
    case ODIN3_DOT_FOCUS_NONE:
    default:
        return dot_invalid("unknown focus kind for", opts->focus);
    }
}

/* --- emit ---------------------------------------------------------------------------------- */

static odin3_status dot_put_bytes(odin3_strbuf *buf, const char *text, size_t len) {
    return odin3_strbuf_append(buf, (odin3_bytes){text, len});
}

static odin3_status dot_put_str(odin3_strbuf *buf, const char *text) {
    return dot_put_bytes(buf, text, strlen(text));
}

static bool dot_ordinary(char chr) {
    return chr != '"' && chr != '\\' && chr != '&' && (unsigned char)chr >= ' ';
}

/* The replacement for one character that is not ordinary. */
static const char *dot_replacement(char chr) {
    switch (chr) {
    case '"':
        return "\\\"";
    case '\\':
        return "\\\\";
    case '\n':
        return "\\n";
    case '&':
        return "&amp;"; /* Graphviz expands HTML entities inside quoted labels */
    default:
        return "?";
    }
}

/* Appends text as the inside of a dot quoted string: quote, backslash and newline escaped,
 * other control characters replaced by '?'. Ordinary runs go in one append. */
static odin3_status dot_put_escaped(odin3_strbuf *buf, const char *text) {
    const char *at = text;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && *at != '\0') {
        size_t run = 0;
        while (dot_ordinary(at[run])) {
            run++;
        }
        st = run > 0 ? dot_put_bytes(buf, at, run) : ODIN3_OK;
        at += run;
        if (st == ODIN3_OK && *at != '\0') {
            st = dot_put_str(buf, dot_replacement(*at));
            at++;
        }
    }
    return st;
}

/* The name of the port wire whose port node is node, 0 if none. */
static uint32_t dot_port_name(const odin3_module *mod, odin3_node_id node) {
    const uint32_t ports = odin3_module_port_count(mod);
    for (uint32_t i = 0; i < ports; i++) {
        if (odin3_module_port(mod, i).v == node.v) {
            return odin3_wire_name(mod, odin3_module_port_wire(mod, i));
        }
    }
    return 0;
}

static odin3_status dot_emit_node(const dot_ctx *dc, odin3_strbuf *buf, const odin3_module *mod,
                                  odin3_node_id node) {
    const odin3_celltype_def *def = odin3_celltype_get(dc->design, odin3_node_type(mod, node));
    uint32_t name = odin3_node_name(mod, node);
    if (name == 0 && def != NULL && def->gran == ODIN3_GRAN_PORT) {
        name = dot_port_name(mod, node); /* a port node is labelled by its port */
    }
    odin3_status st =
        odin3_strbuf_appendf(buf, "    m%un%u [label=\"", odin3_module_id_of(mod).v, node.v);
    st = st == ODIN3_OK ? dot_put_escaped(buf, def != NULL ? def->name : "?") : st;
    st = st == ODIN3_OK ? dot_put_str(buf, "\\n") : st;
    if (st == ODIN3_OK && name != 0) {
        st = dot_put_escaped(buf, odin3_strtab_get(dc->tab, name));
    } else if (st == ODIN3_OK) {
        st = odin3_strbuf_appendf(buf, "$n%u", node.v);
    }
    return st == ODIN3_OK ? dot_put_str(buf, "\"];\n") : st;
}

static odin3_status dot_emit_cluster(const dot_ctx *dc, odin3_strbuf *buf,
                                     const odin3_module *mod) {
    uint32_t id = odin3_module_id_of(mod).v;
    const char *name = odin3_strtab_get(dc->tab, odin3_module_name(mod));
    /* The module ID alone names the cluster: injective whatever the module names look like. */
    odin3_status st = odin3_strbuf_appendf(buf, "  subgraph cluster_%u {\n", id);
    st = st == ODIN3_OK ? dot_put_str(buf, "    label=\"") : st;
    st = st == ODIN3_OK ? dot_put_escaped(buf, name) : st;
    st = st == ODIN3_OK ? dot_put_str(buf, "\";\n") : st;
    for (uint32_t j = 1; st == ODIN3_OK && j < odin3_module_node_end(mod); j++) {
        if (dc->mask[id].bytes[j]) {
            st = dot_emit_node(dc, buf, mod, (odin3_node_id){j});
        }
    }
    return st == ODIN3_OK ? dot_put_str(buf, "  }\n") : st;
}

typedef struct dot_edge {
    const odin3_module *mod;
    odin3_net_id net;
    odin3_node_id from, to;
} dot_edge;

static odin3_status dot_emit_edge(const dot_ctx *dc, odin3_strbuf *buf, const dot_edge *edge) {
    uint32_t mid = odin3_module_id_of(edge->mod).v;
    uint32_t name = odin3_net_name(edge->mod, edge->net);
    odin3_status st =
        odin3_strbuf_appendf(buf, "  m%un%u -> m%un%u", mid, edge->from.v, mid, edge->to.v);
    if (st == ODIN3_OK && name != 0) {
        st = dot_put_str(buf, " [label=\"");
        st = st == ODIN3_OK ? dot_put_escaped(buf, odin3_strtab_get(dc->tab, name)) : st;
        st = st == ODIN3_OK ? dot_put_str(buf, "\"]") : st;
    }
    return st == ODIN3_OK ? dot_put_str(buf, ";\n") : st;
}

/* One edge per (driver pin, sink pin) pair of the net whose nodes are both drawn. */
static odin3_status dot_emit_net(const dot_ctx *dc, odin3_strbuf *buf, const odin3_module *mod,
                                 odin3_net_id net) {
    const uint8_t *mask = dc->mask[odin3_module_id_of(mod).v].bytes;
    odin3_pinlist pins = odin3_net_pins(mod, net);
    uint32_t drivers = odin3_net_driver_count(mod, net);
    for (uint32_t i = 0; i < drivers; i++) {
        dot_edge edge = {mod, net, odin3_pin_node(mod, pins.pins[i]), {0}};
        for (uint32_t j = drivers; mask[edge.from.v] && j < pins.count; j++) {
            edge.to = odin3_pin_node(mod, pins.pins[j]);
            odin3_status st = mask[edge.to.v] ? dot_emit_edge(dc, buf, &edge) : ODIN3_OK;
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

static bool dot_module_drawn(const dot_ctx *dc, uint32_t id) {
    uint32_t end = odin3_module_node_end(dot_module(dc, id));
    for (uint32_t j = 1; j < end; j++) {
        if (dc->mask[id].bytes[j]) {
            return true;
        }
    }
    return false;
}

static odin3_status dot_emit(const dot_ctx *dc, odin3_strbuf *buf) {
    odin3_status st = dot_put_str(buf, "digraph odin3 {\n  rankdir=LR;\n  node [shape=box];\n");
    for (uint32_t i = 1; st == ODIN3_OK && i < dc->mod_end; i++) {
        st = dot_module_drawn(dc, i) ? dot_emit_cluster(dc, buf, dot_module(dc, i)) : ODIN3_OK;
    }
    for (uint32_t i = 1; st == ODIN3_OK && i < dc->mod_end; i++) {
        const odin3_module *mod = dot_module(dc, i);
        for (uint32_t j = 1; st == ODIN3_OK && j < odin3_module_net_end(mod); j++) {
            st = odin3_net_live(mod, (odin3_net_id){j})
                     ? dot_emit_net(dc, buf, mod, (odin3_net_id){j})
                     : ODIN3_OK;
        }
    }
    return st == ODIN3_OK ? dot_put_str(buf, "}\n") : st;
}

/* Writes a private temporary beside the destination, then renames it over the destination: a
 * failure leaves the destination as it was. */
static odin3_status dot_save(const char *path, const odin3_strbuf *buf) {
    odin3_atomic_file file;
    odin3_status st = odin3_atomic_file_open(&file, path);
    if (st != ODIN3_OK) {
        return st;
    }
    if (buf->len != 0 && fwrite(buf->data, 1, buf->len, file.fp) != buf->len) {
        odin3_log(ODIN3_LOG_ERROR, "dot: cannot write '%s'", path);
        st = ODIN3_ERR_IO;
    }
    return odin3_atomic_file_close(&file, st);
}

static odin3_status dot_check_budget(const dot_ctx *dc, const odin3_dot_opts *opts) {
    size_t limit = opts->max_nodes != 0 ? opts->max_nodes : (size_t)ODIN3_DOT_DEFAULT_MAX_NODES;
    size_t count = dot_masks_count(dc);
    if (count <= limit) {
        return ODIN3_OK;
    }
    odin3_log(ODIN3_LOG_ERROR,
              "dot: refusing to draw %zu nodes, more than --max-nodes %zu; narrow it with --focus",
              count, limit);
    return ODIN3_ERR_INVALID_ARG;
}

static odin3_status dot_build(dot_ctx *dc, const odin3_dot_opts *opts, odin3_strbuf *buf) {
    odin3_status st = dot_masks_alloc(dc);
    if (st != ODIN3_OK) {
        return st;
    }
    st = opts->focus_kind == ODIN3_DOT_FOCUS_NONE ? dot_check_budget(dc, opts)
                                                  : dot_apply_focus(dc, opts);
    if (st == ODIN3_OK && opts->focus_kind != ODIN3_DOT_FOCUS_NONE && dot_masks_count(dc) == 0) {
        odin3_log(ODIN3_LOG_WARN, "dot: focus '%s' selects no node; the graph is empty",
                  opts->focus);
    }
    return st == ODIN3_OK ? dot_emit(dc, buf) : st;
}

odin3_status odin3_dot_write(const odin3_design *design, const char *path,
                             const odin3_dot_opts *opts) {
    static const odin3_dot_opts k_defaults = {ODIN3_DOT_FOCUS_NONE, NULL, 0};
    if (design == NULL || path == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "dot: design and path are required");
        return ODIN3_ERR_INVALID_ARG;
    }
    /* The module and forward-index accessors take a mutable design; nothing here mutates it. */
    dot_ctx dc = {(odin3_design *)design, odin3_design_strtab(design), NULL, 0};
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    odin3_status st = dot_build(&dc, opts != NULL ? opts : &k_defaults, &buf);
    if (st == ODIN3_OK) {
        st = dot_save(path, &buf);
    }
    dot_masks_free(&dc);
    odin3_strbuf_free(&buf);
    return st;
}
