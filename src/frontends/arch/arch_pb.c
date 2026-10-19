/*
 * arch_pb.c — the pb_type hierarchy of a VPR architecture (spec P-1 … P-7): the leaves with their
 * paths, K, the interconnect pack patterns, the directlist, and the VPR port sizes of the models.
 * The walk is iterative: an explicit stack of frames, never recursion.
 */
#include "frontends/arch/arch_read.h"

#include <stdlib.h>
#include <string.h>

typedef struct pb_frame {
    uint32_t cursor;   /* the next child element index or ODIN3_XML_NONE */
    uint32_t depth;    /* index of the pb_type in the level path */
    const char *mode;  /* the mode this frame stands for, NULL for a pb_type frame */
} pb_frame;

typedef struct pb_walk {
    arch_rd *rd;
    odin3_vec stack;  /* pb_frame */
    odin3_vec levels; /* odin3_arch_level: the path to the current pb_type */
} pb_walk;

/* --- leaves ---------------------------------------------------------------------------------- */

static bool is_blank(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

/* Splits ".subckt NAME" into the kind and the model name (arena). */
static odin3_status leaf_kind(const arch_rd *rd, const odin3_xml_elem *elem, const char *text,
                              odin3_arch_leaf *leaf) {
    static const struct {
        const char *text;
        odin3_arch_leaf_kind kind;
    } k_plain[] = {{".input", ODIN3_ARCH_LEAF_INPUT},
                   {".output", ODIN3_ARCH_LEAF_OUTPUT},
                   {".names", ODIN3_ARCH_LEAF_NAMES},
                   {".latch", ODIN3_ARCH_LEAF_LATCH}};
    for (size_t i = 0; i < sizeof k_plain / sizeof k_plain[0]; i++) {
        if (strcmp(text, k_plain[i].text) == 0) {
            leaf->kind = k_plain[i].kind;
            return ODIN3_OK;
        }
    }
    static const char k_subckt[] = ".subckt";
    if (strncmp(text, k_subckt, sizeof k_subckt - 1) == 0 && is_blank(text[sizeof k_subckt - 1])) {
        const char *name = text + sizeof k_subckt;
        while (is_blank(*name)) {
            name++;
        }
        size_t len = 0;
        while (name[len] != '\0' && !is_blank(name[len])) {
            len++;
        }
        if (len > 0) {
            leaf->kind = ODIN3_ARCH_LEAF_SUBCKT;
            leaf->model = odin3_arena_strndup(rd->arch->arena, name, len);
            return leaf->model != NULL ? ODIN3_OK : ODIN3_ERR_NO_MEMORY;
        }
    }
    return arch_fail(rd, elem, "unsupported blif_model '%s'", text);
}

static odin3_status read_pin(const arch_rd *rd, const odin3_xml_elem *xp, odin3_arch_pin *pin) {
    pin->dir = arch_tag_is(xp, "input")    ? ODIN3_ARCH_PIN_IN
               : arch_tag_is(xp, "output") ? ODIN3_ARCH_PIN_OUT
                                           : ODIN3_ARCH_PIN_CLOCK;
    odin3_status st = arch_attr_req(rd, xp, "name", &pin->name);
    st = st != ODIN3_OK ? st : arch_attr_u32(rd, xp, "num_pins", 1, &pin->num_pins);
    return st != ODIN3_OK ? st : arch_attr(rd, xp, "port_class", &pin->port_class);
}

static bool is_pin_tag(const odin3_xml_elem *elem) {
    return arch_tag_is(elem, "input") || arch_tag_is(elem, "output") || arch_tag_is(elem, "clock");
}

static odin3_status read_pins(const arch_rd *rd, const odin3_xml_elem *elem, odin3_arch_leaf *leaf) {
    uint32_t count = 0;
    for (const odin3_xml_elem *c = arch_first(rd, elem); c != NULL; c = arch_next(rd, c)) {
        count += is_pin_tag(c) ? 1u : 0u;
    }
    odin3_arch_pin *pins = odin3_arena_alloc(rd->arch->arena, (count + 1u) * sizeof *pins);
    if (pins == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t n = 0;
    for (const odin3_xml_elem *c = arch_first(rd, elem); c != NULL; c = arch_next(rd, c)) {
        if (is_pin_tag(c)) {
            odin3_status st = read_pin(rd, c, &pins[n++]);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    leaf->pins = pins;
    leaf->n_pins = n;
    return ODIN3_OK;
}

static const odin3_arch_mport *find_mport(const odin3_arch_model *model, const char *name) {
    for (uint32_t i = 0; i < model->n_ports; i++) {
        if (strcmp(model->ports[i].name, name) == 0) {
            return &model->ports[i];
        }
    }
    return NULL;
}

/* VPR's checks of a leaf against its model (arch_util.cpp). */
static odin3_status check_leaf(const pb_walk *w, const odin3_xml_elem *elem,
                               const odin3_arch_leaf *leaf) {
    const arch_rd *rd = w->rd;
    if (leaf->kind != ODIN3_ARCH_LEAF_SUBCKT) {
        return ODIN3_OK;
    }
    const char *pb = ((const odin3_arch_level *)w->levels.data)[w->levels.len - 1].pb;
    const odin3_arch_model *model = odin3_arch_model_find(rd->arch, leaf->model);
    if (model == NULL) {
        return arch_fail(rd, elem, "pb_type '%s' uses model '%s', which is not in <models>", pb,
                         leaf->model);
    }
    for (uint32_t i = 0; i < leaf->n_pins; i++) {
        const odin3_arch_pin *pin = &leaf->pins[i];
        const odin3_arch_mport *port = find_mport(model, pin->name);
        bool out = pin->dir == ODIN3_ARCH_PIN_OUT;
        if (port == NULL) {
            return arch_fail(rd, elem, "pb_type '%s' port '%s' is not a port of model '%s'", pb,
                             pin->name, model->name);
        }
        if (port->is_out != out) {
            return arch_fail(rd, elem, "pb_type '%s' port '%s' is %s but model '%s' declares %s",
                             pb, pin->name, out ? "an output" : "an input", model->name,
                             out ? "an input" : "an output");
        }
        if ((pin->dir == ODIN3_ARCH_PIN_CLOCK) != port->is_clock) {
            return arch_fail(rd, elem, "pb_type '%s' port '%s' %s a clock but model '%s' %s", pb,
                             pin->name, port->is_clock ? "is not" : "is", model->name,
                             port->is_clock ? "declares it one" : "does not");
        }
    }
    return ODIN3_OK;
}

static void note_lut(odin3_arch *arch, const odin3_arch_leaf *leaf) {
    for (uint32_t i = 0; leaf->kind == ODIN3_ARCH_LEAF_NAMES && i < leaf->n_pins; i++) {
        if (leaf->pins[i].dir != ODIN3_ARCH_PIN_IN) {
            continue;
        }
        uint32_t k = leaf->pins[i].num_pins;
        arch->lut_k = k > arch->lut_k ? k : arch->lut_k;
        arch->lut_k_min = (arch->lut_k_min == 0 || k < arch->lut_k_min) ? k : arch->lut_k_min;
    }
}

static odin3_status record_leaf(pb_walk *w, const odin3_xml_elem *elem, const char *blif) {
    arch_rd *rd = w->rd;
    odin3_arch_leaf leaf = {0};
    leaf.line = elem->line;
    odin3_status st = leaf_kind(rd, elem, blif, &leaf);
    st = st != ODIN3_OK ? st : arch_attr(rd, elem, "class", &leaf.cls);
    st = st != ODIN3_OK ? st : read_pins(rd, elem, &leaf);
    st = st != ODIN3_OK ? st : check_leaf(w, elem, &leaf);
    if (st != ODIN3_OK) {
        return st;
    }
    size_t bytes = w->levels.len * sizeof(odin3_arch_level);
    odin3_arch_level *path = odin3_arena_alloc(rd->arch->arena, bytes + 1u);
    odin3_arch_leaf *slot = path != NULL ? odin3_vec_push(&rd->arch->leaves) : NULL;
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    memcpy(path, w->levels.data, bytes);
    leaf.levels = path;
    leaf.n_levels = (uint32_t)w->levels.len;
    leaf.num_pb = path[leaf.n_levels - 1].num_pb;
    *slot = leaf;
    note_lut(rd->arch, &leaf);
    return ODIN3_OK;
}

/* --- interconnect ------------------------------------------------------------------------------ */

static odin3_status read_packs(const arch_rd *rd, const odin3_xml_elem *elem, odin3_arch_ic *ic) {
    uint32_t count = arch_count(rd, elem, "pack_pattern");
    odin3_arch_pack *packs = odin3_arena_alloc(rd->arch->arena, (count + 1u) * sizeof *packs);
    if (packs == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint32_t n = 0;
    for (const odin3_xml_elem *c = arch_first(rd, elem); c != NULL; c = arch_next(rd, c)) {
        if (!arch_tag_is(c, "pack_pattern")) {
            continue;
        }
        odin3_status st = arch_attr_req(rd, c, "name", &packs[n].name);
        st = st != ODIN3_OK ? st : arch_attr(rd, c, "in_port", &packs[n].in_port);
        st = st != ODIN3_OK ? st : arch_attr(rd, c, "out_port", &packs[n].out_port);
        if (st != ODIN3_OK) {
            return st;
        }
        n++;
    }
    ic->packs = packs;
    ic->n_packs = n;
    return ODIN3_OK;
}

static odin3_status record_ic(pb_walk *w, const odin3_xml_elem *elem, const pb_frame *frame) {
    arch_rd *rd = w->rd;
    odin3_arch_ic ic = {0};
    ic.line = elem->line;
    ic.in_mode = frame->mode;
    ic.kind = arch_tag_is(elem, "direct") ? "direct" : arch_tag_is(elem, "mux") ? "mux" : "complete";
    ic.pb = ((const odin3_arch_level *)w->levels.data)[frame->depth].pb;
    odin3_status st = arch_attr(rd, elem, "name", &ic.name);
    st = st != ODIN3_OK ? st : arch_attr(rd, elem, "input", &ic.input);
    st = st != ODIN3_OK ? st : arch_attr(rd, elem, "output", &ic.output);
    st = st != ODIN3_OK ? st : read_packs(rd, elem, &ic);
    odin3_arch_ic *slot = st == ODIN3_OK ? odin3_vec_push(&rd->arch->ics) : NULL;
    if (st == ODIN3_OK && slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (slot != NULL) {
        *slot = ic;
    }
    return st;
}

static odin3_status record_interconnect(pb_walk *w, const odin3_xml_elem *group,
                                        const pb_frame *frame) {
    for (const odin3_xml_elem *c = arch_first(w->rd, group); c != NULL; c = arch_next(w->rd, c)) {
        if (arch_tag_is(c, "direct") || arch_tag_is(c, "mux") || arch_tag_is(c, "complete")) {
            odin3_status st = record_ic(w, c, frame);
            if (st != ODIN3_OK) {
                return st;
            }
        }
    }
    return ODIN3_OK;
}

/* --- the walk ---------------------------------------------------------------------------------- */

static odin3_status push_frame(pb_walk *w, uint32_t cursor, uint32_t depth, const char *mode) {
    pb_frame *frame = odin3_vec_push(&w->stack);
    if (frame == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *frame = (pb_frame){cursor, depth, mode};
    return ODIN3_OK;
}

static odin3_status set_level(pb_walk *w, uint32_t depth, odin3_arch_level level) {
    w->levels.len = depth; /* the level of a sibling subtree is dead */
    odin3_arch_level *slot = odin3_vec_push(&w->levels);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = level;
    return ODIN3_OK;
}

static odin3_status enter_pb(pb_walk *w, const odin3_xml_elem *elem, const char *in_mode,
                             uint32_t depth) {
    odin3_arch_level level = {NULL, in_mode, 1};
    const char *blif = NULL;
    odin3_status st = arch_attr_req(w->rd, elem, "name", &level.pb);
    st = st != ODIN3_OK ? st : arch_attr_u32(w->rd, elem, "num_pb", 1, &level.num_pb);
    st = st != ODIN3_OK ? st : arch_attr(w->rd, elem, "blif_model", &blif);
    st = st != ODIN3_OK ? st : set_level(w, depth, level);
    if (st != ODIN3_OK) {
        return st;
    }
    if (blif != NULL) {
        return record_leaf(w, elem, blif);
    }
    return push_frame(w, elem->first_child, depth, NULL);
}

static odin3_status step(pb_walk *w) {
    pb_frame *top = odin3_vec_at(&w->stack, w->stack.len - 1);
    if (top->cursor == ODIN3_XML_NONE) {
        odin3_vec_pop(&w->stack);
        return ODIN3_OK;
    }
    const odin3_xml_elem *child = arch_elem(w->rd, top->cursor);
    pb_frame frame = *top;
    top->cursor = child->next_sibling;
    if (arch_tag_is(child, "pb_type")) {
        return enter_pb(w, child, frame.mode, frame.depth + 1);
    }
    if (arch_tag_is(child, "interconnect")) {
        return record_interconnect(w, child, &frame);
    }
    if (arch_tag_is(child, "mode") && frame.mode == NULL) {
        const char *mode = NULL;
        odin3_status st = arch_attr_req(w->rd, child, "name", &mode);
        return st != ODIN3_OK ? st : push_frame(w, child->first_child, frame.depth, mode);
    }
    return ODIN3_OK;
}

static odin3_status walk_top(pb_walk *w, const odin3_xml_elem *top) {
    odin3_status st = enter_pb(w, top, NULL, 0);
    while (st == ODIN3_OK && w->stack.len > 0) {
        st = step(w);
    }
    return st;
}

odin3_status arch_parse_pbs(arch_rd *rd, const odin3_xml_elem *blocks) {
    if (blocks == NULL) {
        return ODIN3_OK;
    }
    pb_walk walk = {.rd = rd};
    odin3_vec_init(&walk.stack, sizeof(pb_frame));
    odin3_vec_init(&walk.levels, sizeof(odin3_arch_level));
    odin3_status st = ODIN3_OK;
    for (const odin3_xml_elem *c = arch_first(rd, blocks); st == ODIN3_OK && c != NULL;
         c = arch_next(rd, c)) {
        st = arch_tag_is(c, "pb_type") ? walk_top(&walk, c) : ODIN3_OK;
    }
    odin3_vec_free(&walk.stack);
    odin3_vec_free(&walk.levels);
    return st;
}

/* --- directlist -------------------------------------------------------------------------------- */

static odin3_status attr_i32(const arch_rd *rd, const odin3_xml_elem *elem, const char *name,
                             int32_t *out) {
    const char *text = NULL;
    odin3_status st = arch_attr(rd, elem, name, &text);
    *out = 0;
    if (st != ODIN3_OK || text == NULL) {
        return st;
    }
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < INT32_MIN || value > INT32_MAX) {
        return arch_fail(rd, elem, "attribute '%s' must be an integer, not '%s'", name, text);
    }
    *out = (int32_t)value;
    return ODIN3_OK;
}

static odin3_status read_direct(const arch_rd *rd, const odin3_xml_elem *xd, odin3_arch_direct *d) {
    d->line = xd->line;
    odin3_status st = arch_attr_req(rd, xd, "name", &d->name);
    st = st != ODIN3_OK ? st : arch_attr(rd, xd, "from_pin", &d->from_pin);
    st = st != ODIN3_OK ? st : arch_attr(rd, xd, "to_pin", &d->to_pin);
    st = st != ODIN3_OK ? st : attr_i32(rd, xd, "x_offset", &d->x_offset);
    st = st != ODIN3_OK ? st : attr_i32(rd, xd, "y_offset", &d->y_offset);
    return st != ODIN3_OK ? st : attr_i32(rd, xd, "z_offset", &d->z_offset);
}

odin3_status arch_parse_directlist(arch_rd *rd, const odin3_xml_elem *list) {
    if (list == NULL) {
        return ODIN3_OK;
    }
    for (const odin3_xml_elem *c = arch_first(rd, list); c != NULL; c = arch_next(rd, c)) {
        if (!arch_tag_is(c, "direct")) {
            continue;
        }
        odin3_arch_direct d = {0};
        odin3_status st = read_direct(rd, c, &d);
        odin3_arch_direct *slot = st == ODIN3_OK ? odin3_vec_push(&rd->arch->directs) : NULL;
        if (st == ODIN3_OK && slot == NULL) {
            st = ODIN3_ERR_NO_MEMORY;
        }
        if (st != ODIN3_OK) {
            return st;
        }
        *slot = d;
    }
    return ODIN3_OK;
}

/* --- sizes and flags after the walk ------------------------------------------------------------ */

static bool is_adder_model(const odin3_arch_model *model) {
    static const char *const k_names[] = {"a", "b", "cin", "cout", "sumout"};
    if (model->n_ports != sizeof k_names / sizeof k_names[0]) {
        return false;
    }
    for (size_t i = 0; i < sizeof k_names / sizeof k_names[0]; i++) {
        if (find_mport(model, k_names[i]) == NULL) {
            return false;
        }
    }
    return true;
}

void arch_finish_widths(arch_rd *rd) {
    odin3_arch *arch = rd->arch;
    for (uint32_t i = 0; i < arch->leaves.len; i++) {
        const odin3_arch_leaf *leaf = odin3_vec_cat(&arch->leaves, i);
        const odin3_arch_model *model =
            leaf->kind == ODIN3_ARCH_LEAF_SUBCKT ? odin3_arch_model_find(arch, leaf->model) : NULL;
        if (model == NULL) {
            continue;
        }
        arch->has_adder = arch->has_adder || (strcmp(model->name, "adder") == 0 && is_adder_model(model));
        for (uint32_t p = 0; p < leaf->n_pins; p++) {
            odin3_arch_mport *port = (odin3_arch_mport *)find_mport(model, leaf->pins[p].name);
            uint32_t width = leaf->pins[p].num_pins;
            port->max_width = width > port->max_width ? width : port->max_width;
            port->min_width = (port->min_width == 0 || width < port->min_width) ? width : port->min_width;
        }
    }
}
