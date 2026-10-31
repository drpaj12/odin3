/* writer.c — Graphviz dot writer: clusters per module, focus selection, node budget. */
#include "backends/dot/writer.h"

#include "backends/common/hdl_loc.h"
#include "ir/celltype.h"
#include "ir/module.h"
#include "ir/proc.h"
#include "ir/prov.h"
#include "ir/value.h"
#include "util/alloc.h"
#include "util/file.h"
#include "util/hash.h"
#include "util/log.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The nodes to draw: one byte per node ID of each module (indexed by module ID). */
typedef struct dot_mask {
    uint8_t *bytes;
} dot_mask;

/* One colour class: a distinct attribute text and the drawn nodes that have it. */
typedef struct dot_class {
    const char *text; /* points into dot_color.pool, set once collecting is done */
    size_t count;
    size_t off;  /* the text's offset in dot_color.pool */
    size_t link; /* collecting: the next class of the same hash bucket, plus one; 0 ends */
} dot_class;

/* --color-by state (C-1 to C-3): the classes sorted bytewise, the drawn nodes with no value. */
typedef struct dot_color {
    bool on;
    bool is_type;      /* "@type": the cell type name */
    uint32_t key;      /* strtab ID of the attribute key, 0 when no object uses that string */
    size_t none;       /* drawn nodes in the class (none) */
    odin3_strbuf pool; /* the class texts, NUL separated */
    odin3_strbuf text; /* scratch: the text of the node being looked at */
    odin3_vec classes; /* dot_class, sorted */
} dot_color;

typedef struct dot_ctx {
    odin3_design *design;
    odin3_strtab *tab;
    dot_mask *mask;
    uint32_t mod_end;
    const odin3_dot_opts *opts;
    odin3_hdl_ctx *hdl; /* --locs only */
    dot_color color;
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

/* --- colour classes (C-1, C-2) ------------------------------------------------------------- */

/* The fills of C-2, in order (Okabe-Ito extended); sorted values take them in turn. */
enum { PALETTE_SIZE = 12 };
static const char *const k_palette[PALETTE_SIZE] = {"#e69f00", "#56b4e9", "#009e73", "#f0e442",
                                                    "#0072b2", "#d55e00", "#cc79a7", "#999999",
                                                    "#88ccee", "#ddcc77", "#44aa99", "#aa4499"};
static const char k_none_fill[] = "#e8e8e8";
static const char k_more_fill[] = "#cfcfcf"; /* the folded legend entry */
/* The legend names at most this many classes; the others fold into one entry (agent default). */
enum { LEGEND_MAX = 32, LEGEND_MORE_LEN = 48 };
static const char k_none_text[] = "(none)";
static const char k_type_attr[] = "@type";

/* The attribute value as text: an integer in decimal, a string as is, bits MSB first. */
static odin3_status dot_value_text(const dot_ctx *dc, const odin3_value *val, odin3_strbuf *out) {
    switch (val->kind) {
    case ODIN3_VAL_INT:
        return odin3_strbuf_appendf(out, "%lld", (long long)val->i);
    case ODIN3_VAL_STRING:
        return odin3_strbuf_append(out, odin3_bytes_cstr(odin3_strtab_get(dc->tab, val->str)));
    case ODIN3_VAL_BITS: {
        static const char k_bit_char[] = {'0', '1', 'x', 'z'};
        for (uint32_t i = val->len; i > 0; i--) {
            const odin3_status st =
                odin3_strbuf_append(out, (odin3_bytes){&k_bit_char[val->bits[i - 1] & 3], 1});
            if (st != ODIN3_OK) {
                return st;
            }
        }
        return ODIN3_OK;
    }
    case ODIN3_VAL_COVER:
        return odin3_strbuf_append(out, odin3_bytes_cstr("(cover)"));
    case ODIN3_VAL_PROC:
    default:
        return odin3_strbuf_append(out, odin3_bytes_cstr("(proc)"));
    }
}

/* Puts the colour-class text of a node into dc->color.text; *has is false for (none). */
static odin3_status dot_node_class(dot_ctx *dc, const odin3_module *mod, odin3_node_id node,
                                   bool *has) {
    odin3_strbuf_clear(&dc->color.text);
    *has = false;
    if (dc->color.is_type) {
        const odin3_celltype_def *def = odin3_celltype_get(dc->design, odin3_node_type(mod, node));
        *has = true;
        return odin3_strbuf_append(&dc->color.text,
                                   odin3_bytes_cstr(def != NULL ? def->name : "?"));
    }
    const odin3_value *val =
        dc->color.key == 0
            ? NULL
            : odin3_attr_get(mod, (odin3_objref){ODIN3_OBJ_NODE, node.v}, dc->color.key);
    *has = val != NULL;
    return val != NULL ? dot_value_text(dc, val, &dc->color.text) : ODIN3_OK;
}

static int dot_cmp_text(const void *lhs, const void *rhs) {
    return strcmp(((const dot_class *)lhs)->text, ((const dot_class *)rhs)->text);
}

static int dot_cmp_class(const void *key, const void *elem) {
    return strcmp((const char *)key, ((const dot_class *)elem)->text);
}

/* Finds the class with this text in the bucket chain starting at head (index plus one). */
static dot_class *dot_class_find(const dot_color *color, uint64_t head, const char *text) {
    dot_class *classes = color->classes.data;
    for (size_t at = (size_t)head; at != 0; at = classes[at - 1].link) {
        if (strcmp(color->pool.data + classes[at - 1].off, text) == 0) {
            return &classes[at - 1];
        }
    }
    return NULL;
}

/* Adds a class for text (count 1), linked in front of its bucket. */
static odin3_status dot_class_add(dot_color *color, odin3_u64map *index, uint64_t hash,
                                  const char *text) {
    uint64_t head = 0;
    (void)odin3_u64map_get(index, hash, &head);
    dot_class *slot = odin3_vec_push(&color->classes);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = (dot_class){NULL, 1, color->pool.len, (size_t)head};
    odin3_status st = odin3_strbuf_append(&color->pool, (odin3_bytes){text, strlen(text) + 1});
    if (st != ODIN3_OK) {
        color->classes.len--;
        return st;
    }
    st = odin3_u64map_put(index, (odin3_kv){hash, color->classes.len});
    if (st != ODIN3_OK) {
        color->classes.len--;
    }
    return st;
}

/* Interns the node's class text (each distinct text is stored once) or counts (none). */
static odin3_status dot_collect_node(dot_ctx *dc, const odin3_module *mod, odin3_node_id node,
                                     odin3_u64map *index) {
    bool has = false;
    odin3_status st = dot_node_class(dc, mod, node, &has);
    if (st != ODIN3_OK) {
        return st;
    }
    if (!has) {
        dc->color.none++;
        return ODIN3_OK;
    }
    const char *text = dc->color.text.data != NULL ? dc->color.text.data : "";
    const uint64_t hash = odin3_hash_bytes((odin3_bytes){text, strlen(text)}, ODIN3_HASH_SEED);
    uint64_t head = 0;
    dot_class *hit =
        odin3_u64map_get(index, hash, &head) ? dot_class_find(&dc->color, head, text) : NULL;
    if (hit != NULL) {
        hit->count++;
        return ODIN3_OK;
    }
    return dot_class_add(&dc->color, index, hash, text);
}

static odin3_status dot_collect_module(dot_ctx *dc, uint32_t id, odin3_u64map *index) {
    const odin3_module *mod = dot_module(dc, id);
    for (uint32_t j = 1; j < odin3_module_node_end(mod); j++) {
        odin3_status st =
            dc->mask[id].bytes[j] ? dot_collect_node(dc, mod, (odin3_node_id){j}, index) : ODIN3_OK;
        if (st != ODIN3_OK) {
            return st;
        }
    }
    return ODIN3_OK;
}

/* Points every class at its text and sorts the classes bytewise. */
static void dot_classes_finish(dot_color *color) {
    dot_class *classes = color->classes.data;
    for (size_t i = 0; i < color->classes.len; i++) {
        classes[i].text = color->pool.data + classes[i].off;
    }
    if (color->classes.len > 0) {
        qsort(classes, color->classes.len, sizeof(dot_class), dot_cmp_text);
    }
}

static void dot_color_free(dot_ctx *dc) {
    odin3_strbuf_free(&dc->color.pool);
    odin3_strbuf_free(&dc->color.text);
    odin3_vec_free(&dc->color.classes);
}

static void dot_color_warn(const dot_ctx *dc) {
    const char *attr = dc->opts->color_attr;
    if (dc->color.classes.len == 0) {
        odin3_log(ODIN3_LOG_WARN, "dot: --color-by '%s' is on no drawn node; every node is (none)",
                  attr);
    } else if (dc->color.classes.len > PALETTE_SIZE) {
        odin3_log(ODIN3_LOG_WARN,
                  "dot: --color-by '%s' has %zu values but the palette has %d colours; colours "
                  "are reused",
                  attr, dc->color.classes.len, PALETTE_SIZE);
    }
}

/* Reads the attribute of every drawn node and builds the sorted classes. */
static odin3_status dot_color_prepare(dot_ctx *dc) {
    const char *attr = dc->opts->color_attr;
    if (attr == NULL) {
        return ODIN3_OK;
    }
    if (attr[0] == '\0') {
        return dot_invalid("--color-by expects an attribute name, got", attr);
    }
    dc->color.on = true;
    dc->color.is_type = strcmp(attr, k_type_attr) == 0;
    (void)odin3_strtab_find(dc->tab, odin3_bytes_cstr(attr), &dc->color.key);
    odin3_u64map *index = odin3_u64map_create(0);
    odin3_status st = index != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
    for (uint32_t i = 1; st == ODIN3_OK && i < dc->mod_end; i++) {
        st = dot_collect_module(dc, i, index);
    }
    odin3_u64map_destroy(index);
    if (st == ODIN3_OK) {
        dot_classes_finish(&dc->color);
        dot_color_warn(dc);
    }
    return st;
}

/* The fill of the node's class, NULL when colouring is off or the node has no value. */
static odin3_status dot_node_fill(dot_ctx *dc, const odin3_module *mod, odin3_node_id node,
                                  const char **fill) {
    bool has = false;
    odin3_status st = dot_node_class(dc, mod, node, &has);
    const char *text = dc->color.text.data != NULL ? dc->color.text.data : "";
    const dot_class *hit = has && st == ODIN3_OK
                               ? bsearch(text, dc->color.classes.data, dc->color.classes.len,
                                         sizeof(dot_class), dot_cmp_class)
                               : NULL;
    *fill =
        hit != NULL
            ? k_palette[(size_t)(hit - (const dot_class *)dc->color.classes.data) % PALETTE_SIZE]
            : k_none_fill;
    return st;
}

/* --- emit ---------------------------------------------------------------------------------- */

static odin3_status dot_put_bytes(odin3_strbuf *buf, const char *text, size_t len) {
    return odin3_strbuf_append(buf, (odin3_bytes){text, len});
}

static odin3_status dot_put_str(odin3_strbuf *buf, const char *text) {
    return dot_put_bytes(buf, text, strlen(text));
}

enum { CH_DEL = 0x7f, CH_HIGH = 0x80, CH_CONT_MASK = 0xc0 };

static bool dot_ordinary(char chr) {
    const unsigned char byte = (unsigned char)chr;
    return chr != '"' && chr != '\\' && chr != '&' && byte >= ' ' && byte != CH_DEL;
}

/* The well-formed UTF-8 lead bytes: the lead range, the sequence length, the range of byte 2. */
typedef struct utf8_form {
    unsigned char lead_lo, lead_hi, len, second_lo, second_hi;
} utf8_form;

static const utf8_form k_utf8_forms[] = {
    {0xc2, 0xdf, 2, 0x80, 0xbf}, {0xe0, 0xe0, 3, 0xa0, 0xbf}, /* no overlong forms */
    {0xe1, 0xec, 3, 0x80, 0xbf}, {0xed, 0xed, 3, 0x80, 0x9f}, /* no surrogates */
    {0xee, 0xef, 3, 0x80, 0xbf}, {0xf0, 0xf0, 4, 0x90, 0xbf},
    {0xf1, 0xf3, 4, 0x80, 0xbf}, {0xf4, 0xf4, 4, 0x80, 0x8f}, /* nothing above U+10FFFF */
};

/* The length of the well-formed UTF-8 sequence (2 to 4 bytes) at text, 0 if there is none. */
static size_t dot_utf8_len(const unsigned char *text) {
    for (size_t i = 0; i < sizeof k_utf8_forms / sizeof k_utf8_forms[0]; i++) {
        const utf8_form *form = &k_utf8_forms[i];
        if (text[0] < form->lead_lo || text[0] > form->lead_hi) {
            continue;
        }
        if (text[1] < form->second_lo || text[1] > form->second_hi) {
            return 0;
        }
        for (size_t at = 2; at < form->len; at++) {
            if ((text[at] & CH_CONT_MASK) != CH_HIGH) {
                return 0;
            }
        }
        return form->len;
    }
    return 0;
}

/* The length of the run at text that goes through unchanged: ordinary ASCII and valid UTF-8. */
static size_t dot_ordinary_run(const char *text) {
    size_t run = 0;
    for (;;) {
        const unsigned char byte = (unsigned char)text[run];
        const size_t len = byte >= CH_HIGH ? dot_utf8_len((const unsigned char *)text + run) : 1;
        if (len == 0 || (byte < CH_HIGH && !dot_ordinary(text[run]))) {
            return run;
        }
        run += len;
    }
}

/* The replacement for one byte that is not ordinary. */
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

/* Appends text as the inside of a dot quoted string: quote, backslash and newline escaped;
 * control characters, DEL and bytes that are not valid UTF-8 replaced by '?' (Graphviz rejects
 * invalid UTF-8). Ordinary runs go in one append. */
static odin3_status dot_put_escaped(odin3_strbuf *buf, const char *text) {
    const char *at = text;
    odin3_status st = ODIN3_OK;
    while (st == ODIN3_OK && *at != '\0') {
        const size_t run = dot_ordinary_run(at);
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

/* The second label line of C-4: the first HDL location and "+N" for N more; none for a node
 * without an HDL location (a netlist position is not one). */
static odin3_status dot_put_locs(const dot_ctx *dc, odin3_strbuf *buf, const odin3_module *mod,
                                 odin3_node_id node) {
    odin3_hdl_locs_out locs;
    odin3_status st = odin3_hdl_locs(dc->hdl, odin3_node_prov(mod, node), &locs);
    if (st != ODIN3_OK || locs.n == 0) {
        return st;
    }
    st = dot_put_str(buf, "\\n");
    st = st == ODIN3_OK ? dot_put_escaped(buf, odin3_strtab_get(dc->tab, locs.locs[0].file)) : st;
    st = st == ODIN3_OK ? odin3_strbuf_appendf(buf, ":%u", locs.locs[0].line) : st;
    return st == ODIN3_OK && locs.n > 1 ? odin3_strbuf_appendf(buf, " +%u", locs.n - 1) : st;
}

/* `label="<type>\n<name>[\n<file:line>[ +N]]"` of a node. */
static odin3_status dot_put_label(const dot_ctx *dc, odin3_strbuf *buf, const odin3_module *mod,
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
    st = st == ODIN3_OK && dc->hdl != NULL ? dot_put_locs(dc, buf, mod, node) : st;
    return st == ODIN3_OK ? dot_put_str(buf, "\"") : st;
}

static odin3_status dot_emit_node(dot_ctx *dc, odin3_strbuf *buf, const odin3_module *mod,
                                  odin3_node_id node) {
    odin3_status st = dot_put_label(dc, buf, mod, node);
    const char *fill = NULL;
    if (st == ODIN3_OK && dc->color.on) {
        st = dot_node_fill(dc, mod, node, &fill);
        st = st == ODIN3_OK ? odin3_strbuf_appendf(buf, ", style=filled, fillcolor=\"%s\"", fill)
                            : st;
    }
    return st == ODIN3_OK ? dot_put_str(buf, "];\n") : st;
}

static odin3_status dot_emit_cluster(dot_ctx *dc, odin3_strbuf *buf, const odin3_module *mod) {
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

/* One legend entry: a filled node labelled "<text> (<n>)". */
static odin3_status dot_emit_legend_node(odin3_strbuf *buf, size_t index, const char *text,
                                         size_t count, const char *fill) {
    odin3_status st = odin3_strbuf_appendf(buf, "    legend_%zu [label=\"", index);
    st = st == ODIN3_OK ? dot_put_escaped(buf, text) : st;
    return st == ODIN3_OK ? odin3_strbuf_appendf(
                                buf, " (%zu)\", style=filled, fillcolor=\"%s\"];\n", count, fill)
                          : st;
}

/* The folded legend entry for the classes from LEGEND_MAX on: its label and node count. */
static odin3_status dot_emit_legend_more(const dot_color *color, odin3_strbuf *buf) {
    const dot_class *classes = color->classes.data;
    size_t count = 0;
    for (size_t i = LEGEND_MAX; i < color->classes.len; i++) {
        count += classes[i].count;
    }
    char text[LEGEND_MORE_LEN];
    (void)snprintf(text, sizeof text, "(%zu more values)", color->classes.len - LEGEND_MAX);
    return dot_emit_legend_node(buf, LEGEND_MAX, text, count, k_more_fill);
}

/* C-3: cluster_legend, a node per class (sorted values, then (none) when some node has none).
 * Past LEGEND_MAX classes the rest fold into one "(N more values)" entry. */
static odin3_status dot_emit_legend(const dot_ctx *dc, odin3_strbuf *buf) {
    odin3_status st = dot_put_str(buf, "  subgraph cluster_legend {\n    label=\"");
    st = st == ODIN3_OK ? dot_put_escaped(buf, dc->opts->color_attr) : st;
    st = st == ODIN3_OK ? dot_put_str(buf, "\";\n") : st;
    const dot_class *classes = dc->color.classes.data;
    const size_t shown = dc->color.classes.len < LEGEND_MAX ? dc->color.classes.len : LEGEND_MAX;
    for (size_t i = 0; st == ODIN3_OK && i < shown; i++) {
        st = dot_emit_legend_node(buf, i, classes[i].text, classes[i].count,
                                  k_palette[i % PALETTE_SIZE]);
    }
    size_t next = shown;
    if (st == ODIN3_OK && dc->color.classes.len > LEGEND_MAX) {
        st = dot_emit_legend_more(&dc->color, buf);
        next++;
    }
    if (st == ODIN3_OK && dc->color.none > 0) {
        st = dot_emit_legend_node(buf, next, k_none_text, dc->color.none, k_none_fill);
    }
    return st == ODIN3_OK ? dot_put_str(buf, "  }\n") : st;
}

static odin3_status dot_emit(dot_ctx *dc, odin3_strbuf *buf) {
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
    st = st == ODIN3_OK && dc->color.on ? dot_emit_legend(dc, buf) : st;
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
    st = st == ODIN3_OK ? dot_color_prepare(dc) : st;
    return st == ODIN3_OK ? dot_emit(dc, buf) : st;
}

odin3_status odin3_dot_focus_parse(odin3_bytes value, odin3_strbuf *text,
                                   odin3_dot_focus_kind *kind) {
    const char *src = (const char *)value.ptr;
    size_t len = value.len;
    bool cone = len > strlen("cone()") && strncmp(src, "cone(", strlen("cone(")) == 0 &&
                src[len - 1] == ')';
    if (cone) {
        src += strlen("cone(");
        len -= strlen("cone()");
    }
    odin3_status st = odin3_strbuf_append(text, (odin3_bytes){src, len});
    const char *colon = st == ODIN3_OK ? strrchr(text->data, ':') : NULL;
    bool line = !cone && colon != NULL && colon[1] != '\0' &&
                strspn(colon + 1, "0123456789") == strlen(colon + 1);
    *kind = cone ? ODIN3_DOT_FOCUS_CONE : (line ? ODIN3_DOT_FOCUS_LOC : ODIN3_DOT_FOCUS_PATH);
    return st;
}

void odin3_dot_selection_free(odin3_dot_selection *sel) {
    for (uint32_t i = 0; sel->mask != NULL && i < sel->mod_end; i++) {
        odin3_util_free(sel->mask[i]);
    }
    odin3_util_free(sel->mask);
    sel->mask = NULL;
    sel->mod_end = 0;
}

odin3_status odin3_dot_select(const odin3_design *design, odin3_dot_focus_kind kind,
                              const char *focus, odin3_dot_selection *out) {
    out->mask = NULL;
    out->mod_end = 0;
    odin3_dot_opts opts = {kind, focus, 0, NULL, false};
    dot_ctx dc = {
        .design = (odin3_design *)design, .tab = odin3_design_strtab(design), .opts = &opts};
    odin3_status st = dot_masks_alloc(&dc);
    st = st == ODIN3_OK && (focus == NULL || kind == ODIN3_DOT_FOCUS_NONE) ? ODIN3_ERR_INVALID_ARG
                                                                           : st;
    if (st == ODIN3_OK) {
        st = dot_apply_focus(&dc, &opts);
    }
    if (st != ODIN3_OK) {
        dot_masks_free(&dc);
        return st;
    }
    out->mod_end = dc.mod_end;
    out->mask = odin3_util_calloc(sizeof(uint8_t *) * dc.mod_end);
    if (out->mask == NULL) {
        dot_masks_free(&dc);
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 1; i < dc.mod_end; i++) {
        out->mask[i] = dc.mask[i].bytes;
        dc.mask[i].bytes = NULL;
    }
    dot_masks_free(&dc);
    return ODIN3_OK;
}

odin3_status odin3_dot_write(const odin3_design *design, const char *path,
                             const odin3_dot_opts *opts) {
    static const odin3_dot_opts k_defaults = {ODIN3_DOT_FOCUS_NONE, NULL, 0, NULL, false};
    if (opts == NULL) {
        opts = &k_defaults;
    }
    if (design == NULL || path == NULL) {
        odin3_log(ODIN3_LOG_ERROR, "dot: design and path are required");
        return ODIN3_ERR_INVALID_ARG;
    }
    if (odin3_design_refuse_processes(design, NULL) != ODIN3_OK) {
        return ODIN3_ERR_INVALID_ARG; /* 2D EL-33: before the path is opened */
    }
    /* The module and forward-index accessors take a mutable design; nothing here mutates it. */
    dot_ctx dc = {
        .design = (odin3_design *)design, .tab = odin3_design_strtab(design), .opts = opts};
    odin3_strbuf_init(&dc.color.pool);
    odin3_strbuf_init(&dc.color.text);
    odin3_vec_init(&dc.color.classes, sizeof(dot_class));
    odin3_strbuf buf;
    odin3_strbuf_init(&buf);
    dc.hdl = opts->locs ? odin3_hdl_ctx_create(design, "write_dot") : NULL;
    odin3_status st = opts->locs && dc.hdl == NULL ? ODIN3_ERR_NO_MEMORY : ODIN3_OK;
    st = st == ODIN3_OK ? dot_build(&dc, opts, &buf) : st;
    if (st == ODIN3_OK) {
        st = dot_save(path, &buf);
    }
    dot_masks_free(&dc);
    dot_color_free(&dc);
    odin3_hdl_ctx_destroy(dc.hdl);
    odin3_strbuf_free(&buf);
    return st;
}
