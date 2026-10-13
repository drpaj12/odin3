/*
 * elab1_lookup.c — stage 1's definition table and module lookup (2D spec §3.1, EL-6): every
 * MODULE, UDP_DECL and CONFIG_DECL of the parsed store with its library, parameters, port
 * positions and instantiations; Q7 and S2; the names an earlier read took; and the seven-step
 * resolution of an instantiated name.
 */
#include "frontends/verilog/elab1.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "ir/value.h"
#include "odin3/odin3.h"
#include "util/hash.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { E1_HIGH_SHIFT = 32, E1_SLOT_PARAMS = 0, E1_SLOT_PORTS = 1 };

/* The low half of a by_name value: the first definition of the chain + 1. */
static const uint64_t E1_LOW32 = UINT32_MAX;

static const char k_blackbox[] = "blackbox";

static const char *text_of(const odin3_elab1 *e1, uint32_t str) {
    return odin3_strtab_get(odin3_design_strtab(e1->design), str);
}

static odin3_elab_def *def_at(odin3_elab1 *e1, uint32_t idx) {
    return odin3_vec_at(&e1->defs, idx);
}

static odin3_elab1_defx *defx_at(odin3_elab1 *e1, uint32_t idx) {
    return odin3_vec_at(&e1->defx, idx);
}

uint32_t odin3_elab1_def_count(const odin3_elab1 *e1) {
    return (uint32_t)e1->defs.len;
}

const odin3_elab_def *odin3_elab1_def(const odin3_elab1 *e1, uint32_t def) {
    return def < e1->defs.len ? odin3_vec_cat(&e1->defs, def) : NULL;
}

static odin3_status push_id(odin3_vec *vec, odin3_ast_id id) {
    odin3_ast_id *slot = odin3_vec_push(vec);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = id;
    return ODIN3_OK;
}

/* --- a definition's parts -------------------------------------------------------------------- */

/* The DECLARATORs of a PARAM_DECL of sub `parameter` onto params; their count in *n. */
static odin3_status push_param_declarators(odin3_elab1 *e1, odin3_ast_id decl, uint32_t *n) {
    if (odin3_ast_kind_of(e1->parsed, decl) != ODIN3_AST_PARAM_DECL ||
        odin3_ast_sub(e1->parsed, decl) != ODIN3_AST_PARAM_DECL_PARAMETER) {
        return ODIN3_OK;
    }
    odin3_ast_span kids = odin3_ast_children(e1->parsed, decl);
    uint32_t slots = odin3_ast_slot_count(ODIN3_AST_PARAM_DECL);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = slots; st == ODIN3_OK && i < kids.n; i++) {
        st = push_id(&e1->params, kids.ids[i]);
        *n += st == ODIN3_OK ? 1 : 0;
    }
    return st;
}

/* The header's `parameter` groups, then the body's direct `parameter` items. */
static odin3_status gather_params(odin3_elab1 *e1, odin3_elab_def *def) {
    def->first_param = (uint32_t)e1->params.len;
    odin3_ast_id list = odin3_ast_child(e1->parsed, def->node, E1_SLOT_PARAMS);
    odin3_ast_span header = odin3_ast_children(e1->parsed, list);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && list.v != 0 && i < header.n; i++) {
        st = push_param_declarators(e1, header.ids[i], &def->n_header_params);
    }
    odin3_ast_span kids = odin3_ast_children(e1->parsed, def->node);
    for (uint32_t i = E1_SLOT_PORTS + 1; st == ODIN3_OK && i < kids.n; i++) {
        st = push_param_declarators(e1, kids.ids[i], &def->n_body_params);
    }
    return st;
}

/* The port positions: an ANSI PORT_DECL's DECLARATORs, a non-ANSI PORT_REF (0 when empty). */
static odin3_status gather_ports(odin3_elab1 *e1, odin3_elab_def *def) {
    def->first_port = (uint32_t)e1->ports.len;
    odin3_ast_id list = odin3_ast_child(e1->parsed, def->node, E1_SLOT_PORTS);
    odin3_ast_span entries = odin3_ast_children(e1->parsed, list);
    uint32_t decl_slots = odin3_ast_slot_count(ODIN3_AST_PORT_DECL);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && list.v != 0 && i < entries.n; i++) {
        odin3_ast_id entry = entries.ids[i];
        if (odin3_ast_kind_of(e1->parsed, entry) != ODIN3_AST_PORT_DECL) {
            st = push_id(&e1->ports, entry);
            def->n_positions += st == ODIN3_OK ? 1 : 0;
            continue;
        }
        odin3_ast_span decls = odin3_ast_children(e1->parsed, entry);
        for (uint32_t j = decl_slots; st == ODIN3_OK && j < decls.n; j++) {
            st = push_id(&e1->ports, decls.ids[j]);
            def->n_positions += st == ODIN3_OK ? 1 : 0;
        }
    }
    return st;
}

/* The kinds an INSTANTIATION can sit under inside a module body (module items, generate). */
static bool may_hold_instances(odin3_ast_kind kind) {
    switch (kind) {
    case ODIN3_AST_MODULE:
    case ODIN3_AST_GENERATE:
    case ODIN3_AST_GEN_FOR:
    case ODIN3_AST_GEN_IF:
    case ODIN3_AST_GEN_CASE:
    case ODIN3_AST_CASE_ITEM:
    case ODIN3_AST_GEN_BLOCK:
        return true;
    default:
        return false;
    }
}

/* One INSTANTIATION of the body: recorded in order, its name marked instantiated. */
static odin3_status note_instance(odin3_elab1 *e1, odin3_ast_id node, odin3_elab1_defx *ext) {
    odin3_status st = push_id(&e1->insts, node);
    ext->n_inst += st == ODIN3_OK ? 1 : 0;
    uint32_t name = odin3_ast_name(e1->parsed, node);
    return st != ODIN3_OK ? st : odin3_u64map_put(e1->instantiated, (odin3_kv){name, 1});
}

/* Every INSTANTIATION anywhere in the body, every generate branch included (divergence 20), in
 * source order: an explicit-stack walk that skips subtrees that cannot hold one. */
static odin3_status gather_insts(odin3_elab1 *e1, odin3_ast_id module, odin3_elab1_defx *ext) {
    ext->first_inst = (uint32_t)e1->insts.len;
    odin3_ast_cursor cur;
    odin3_status st = odin3_ast_cursor_init(&cur, e1->parsed, module);
    odin3_ast_event ev = ODIN3_AST_ENTER;
    odin3_ast_id node = {0};
    while (st == ODIN3_OK && odin3_ast_cursor_next(&cur, &ev, &node)) {
        if (ev != ODIN3_AST_ENTER) {
            continue;
        }
        odin3_ast_kind kind = odin3_ast_kind_of(e1->parsed, node);
        if (kind == ODIN3_AST_INSTANTIATION) {
            st = note_instance(e1, node, ext);
        }
        if (!may_hold_instances(kind)) {
            odin3_ast_cursor_skip(&cur);
        }
    }
    odin3_ast_cursor_free(&cur);
    return st;
}

/* `(* blackbox *)` on the MODULE, true as Yosys reads it: no value, a NUMBER with a 1 bit, or
 * any other value (a non-constant value is the attribute check's concern). */
static bool is_blackbox(const odin3_elab1 *e1, odin3_ast_id module) {
    odin3_ast_span attrs = odin3_ast_attrs(e1->parsed, module);
    for (uint32_t i = 0; i < attrs.n; i++) {
        if (strcmp(odin3_ast_name_str(e1->parsed, attrs.ids[i]), k_blackbox) != 0) {
            continue;
        }
        const odin3_ast_number *num =
            odin3_ast_number_of(e1->parsed, odin3_ast_child(e1->parsed, attrs.ids[i], 0));
        if (num == NULL) {
            return true;
        }
        for (uint32_t bit = 0; bit < num->nbits; bit++) {
            if (num->bits[bit] == ODIN3_BIT_1) {
                return true;
            }
        }
        return false;
    }
    return false;
}

/* Why the name of a MODULE is taken in the design already (EL-6, IR-11). */
static uint8_t taken_of(const odin3_elab1 *e1, uint32_t name) {
    odin3_celltype_id type;
    if (!odin3_celltype_find(e1->design, name, &type)) {
        return ODIN3_TAKEN_NONE;
    }
    const odin3_celltype_def *def = odin3_celltype_get(e1->design, type);
    switch (def->gran) {
    case ODIN3_GRAN_HARD:
        return ODIN3_TAKEN_HARD;
    case ODIN3_GRAN_BLACKBOX:
        return ODIN3_TAKEN_BLACKBOX;
    case ODIN3_GRAN_MODULE:
        return ODIN3_TAKEN_MODULE;
    default:
        return ODIN3_TAKEN_BUILTIN;
    }
}

/* --- the table ------------------------------------------------------------------------------- */

/* Links definition idx at the end of its name's chain. */
static odin3_status chain(odin3_elab1 *e1, uint32_t idx) {
    uint32_t name = def_at(e1, idx)->name;
    uint64_t ends = 0;
    uint64_t self = (uint64_t)idx + 1;
    if (!odin3_u64map_get(e1->by_name, name, &ends)) {
        return odin3_u64map_put(e1->by_name, (odin3_kv){name, self | self << E1_HIGH_SHIFT});
    }
    uint32_t last = (uint32_t)(ends >> E1_HIGH_SHIFT) - 1;
    odin3_status st =
        odin3_u64map_put(e1->by_name, (odin3_kv){name, (ends & E1_LOW32) | self << E1_HIGH_SHIFT});
    if (st == ODIN3_OK) {
        defx_at(e1, last)->next = idx + 1;
    }
    return st;
}

/* A MODULE's parts: parameters, ports, instantiations, the taken check. */
static odin3_status module_parts(odin3_elab1 *e1, odin3_elab_def *def, odin3_elab1_defx *ext) {
    odin3_status st = gather_params(e1, def);
    st = st != ODIN3_OK ? st : gather_ports(e1, def);
    st = st != ODIN3_OK ? st : gather_insts(e1, def->node, ext);
    if (def->kind == ODIN3_DEF_MODULE) {
        ext->taken = taken_of(e1, def->name);
        /* A `read_verilog --lib` unit may share a hard or black-box cell type's name: which of the
         * two an instance means is odin3_elab1_prefer_unit's decision (spec 3.1a). */
        bool cell = ext->taken == ODIN3_TAKEN_HARD || ext->taken == ODIN3_TAKEN_BLACKBOX;
        ext->shadows_cell = cell && (def->unit_flags & ODIN3_UNIT_LIB) != 0;
        ext->taken = ext->shadows_cell ? ODIN3_TAKEN_NONE : ext->taken;
    }
    return st;
}

/* Adds the definition node of kind (and its parts) from a UNIT of library. */
static odin3_status add_def(odin3_elab1 *e1, odin3_ast_id node, uint8_t kind,
                            const odin3_elab_def *where) {
    odin3_elab_def def = *where;
    def.kind = kind;
    def.name = odin3_ast_name(e1->parsed, node);
    def.node = node;
    odin3_elab1_defx ext = {0};
    odin3_status st = kind == ODIN3_DEF_MODULE || kind == ODIN3_DEF_BLACKBOX
                          ? module_parts(e1, &def, &ext)
                          : ODIN3_OK;
    odin3_elab_def *slot = st == ODIN3_OK ? odin3_vec_push(&e1->defs) : NULL;
    odin3_elab1_defx *xslot = slot != NULL ? odin3_vec_push(&e1->defx) : NULL;
    if (xslot == NULL) {
        if (slot != NULL) {
            odin3_vec_pop(&e1->defs);
        }
        return st != ODIN3_OK ? st : ODIN3_ERR_NO_MEMORY;
    }
    *slot = def;
    *xslot = ext;
    return chain(e1, (uint32_t)e1->defs.len - 1);
}

/* The library of a UNIT: its FILE buffer's (2A §3.1), "work" when the buffer has none. Out of
 * memory creating the source manager latches. */
static uint32_t unit_library(odin3_elab1 *e1, odin3_ast_id unit) {
    odin3_srcman *sm = NULL;
    odin3_srcbuf_info info;
    odin3_status st = odin3_design_get_srcman(e1->design, &sm);
    e1->fatal = e1->fatal != ODIN3_OK ? e1->fatal : st;
    if (st != ODIN3_OK ||
        !odin3_srcman_buffer_info(sm, odin3_srcman_buffer_of(sm, odin3_ast_loc(e1->parsed, unit)),
                                  &info) ||
        info.library == 0) {
        return e1->work;
    }
    return info.library;
}

/* The definition kind of a UNIT child; 0 for one that defines nothing (a directive). */
static uint8_t def_kind_of(const odin3_elab1 *e1, odin3_ast_id node) {
    switch (odin3_ast_kind_of(e1->parsed, node)) {
    case ODIN3_AST_MODULE:
        return is_blackbox(e1, node) ? ODIN3_DEF_BLACKBOX : ODIN3_DEF_MODULE;
    case ODIN3_AST_UDP_DECL:
        return ODIN3_DEF_UDP;
    case ODIN3_AST_CONFIG_DECL:
        return ODIN3_DEF_CONFIG;
    default:
        return 0;
    }
}

/* One UNIT's definitions; Q7 and S2 at a design unit's UDP_DECL and CONFIG_DECL. */
static odin3_status unit_defs(odin3_elab1 *e1, uint32_t idx, bool library_unit) {
    odin3_ast_id unit = odin3_ast_root(e1->parsed, idx);
    odin3_elab_def where = {.library_unit = library_unit,
                            .unit_flags = odin3_ast_unit_flags(e1->parsed, idx),
                            .library = unit_library(e1, unit)};
    odin3_ast_span kids = odin3_ast_children(e1->parsed, unit);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < kids.n; i++) {
        uint8_t kind = def_kind_of(e1, kids.ids[i]);
        st = kind != 0 ? add_def(e1, kids.ids[i], kind, &where) : ODIN3_OK;
        bool rejected = kind == ODIN3_DEF_UDP || kind == ODIN3_DEF_CONFIG;
        if (st == ODIN3_OK && rejected && !library_unit) {
            odin3_elab1_reject_def(e1, (uint32_t)e1->defs.len - 1);
        }
    }
    return st;
}

bool odin3_elab1_def_shadows_cell(const odin3_elab1 *e1, uint32_t def) {
    return ((const odin3_elab1_defx *)odin3_vec_cat(&e1->defx, def))->shadows_cell;
}

void odin3_elab1_reject_def(odin3_elab1 *e1, uint32_t def) {
    odin3_elab1_defx *ext = defx_at(e1, def);
    const odin3_elab_def *rec = def_at(e1, def);
    if (ext->reported || odin3_elab1_pending(e1, rec->node)) {
        return;
    }
    ext->reported = true;
    odin3_elab1_error(e1, odin3_ast_loc(e1->parsed, rec->node),
                      rec->kind == ODIN3_DEF_UDP ? "user-defined primitive '%s' is not supported"
                                                 : "configuration '%s' is not supported",
                      text_of(e1, rec->name));
}

odin3_status odin3_elab1_collect_defs(odin3_elab1 *e1) {
    uint32_t roots = odin3_ast_root_count(e1->parsed);
    uint32_t from = odin3_ast_library_units_from(e1->parsed);
    odin3_status st = ODIN3_OK;
    for (uint32_t idx = 0; st == ODIN3_OK && idx < roots; idx++) {
        st = unit_defs(e1, idx, from != 0 && idx >= from);
    }
    return st;
}

/* --- resolution (EL-6 items 1–7) ------------------------------------------------------------- */

/* Marks name noted; true the first time (the info line is due). Out of memory latches. */
static bool first_note(odin3_elab1 *e1, uint32_t name) {
    if (odin3_u64map_get(e1->noted, name, NULL)) {
        return false;
    }
    odin3_status st = odin3_u64map_put(e1->noted, (odin3_kv){name, 1});
    if (st != ODIN3_OK) {
        e1->fatal = e1->fatal != ODIN3_OK ? e1->fatal : st;
        return false;
    }
    return true;
}

/* The design-unit modules of name's chain in other libraries: the first taken; the info line
 * naming every library when there are several (item 2). */
static void several_libraries(odin3_elab1 *e1, uint32_t name, uint32_t first) {
    odin3_strbuf libs;
    odin3_strbuf_init(&libs);
    uint32_t count = 0;
    odin3_status st = ODIN3_OK;
    for (uint32_t at = first; st == ODIN3_OK && at != 0; at = defx_at(e1, at - 1)->next) {
        const odin3_elab_def *def = def_at(e1, at - 1);
        if (def->library_unit ||
            (def->kind != ODIN3_DEF_MODULE && def->kind != ODIN3_DEF_BLACKBOX)) {
            continue;
        }
        st =
            odin3_strbuf_appendf(&libs, "%s'%s'", count > 0 ? ", " : "", text_of(e1, def->library));
        count++;
    }
    const odin3_elab_def *taken = def_at(e1, first - 1);
    if (st != ODIN3_OK) {
        e1->fatal = e1->fatal != ODIN3_OK ? e1->fatal : st;
    } else if (count > 1 && first_note(e1, name)) {
        odin3_elab1_info(e1, (odin3_loc){0}, "module '%s' found in libraries %s; using '%s.%s'",
                         text_of(e1, name), libs.data, text_of(e1, taken->library),
                         text_of(e1, name));
    }
    odin3_strbuf_free(&libs);
}

/* The classes of a name's definitions as resolution ranks them (items 1–4). */
enum { E1_SAME = 0, E1_OTHER, E1_LIBUNIT, E1_UDP, E1_CLASSES, E1_SKIP = E1_CLASSES };

/* A name instantiated in a module of library. */
typedef struct e1_want {
    uint32_t name;
    uint32_t library;
} e1_want;

/* The first definition of each class in name's chain (1-based, 0 = none). */
typedef struct e1_found {
    uint32_t at[E1_CLASSES];
} e1_found;

static uint32_t classify(const odin3_elab_def *def, uint32_t library) {
    if (def->kind == ODIN3_DEF_UDP) {
        return E1_UDP;
    }
    if (def->kind != ODIN3_DEF_MODULE && def->kind != ODIN3_DEF_BLACKBOX) {
        return E1_SKIP;
    }
    if (def->library_unit) {
        return E1_LIBUNIT;
    }
    return def->library == library ? E1_SAME : E1_OTHER;
}

/* Walks the chain until a design unit of the library itself is found (item 1 wins). */
static e1_found scan_chain(odin3_elab1 *e1, const e1_want *want) {
    e1_found found = {{0, 0, 0, 0}};
    uint64_t ends = 0;
    uint32_t at =
        odin3_u64map_get(e1->by_name, want->name, &ends) ? (uint32_t)(ends & E1_LOW32) : 0;
    for (; at != 0 && found.at[E1_SAME] == 0; at = defx_at(e1, at - 1)->next) {
        uint32_t cls = classify(def_at(e1, at - 1), want->library);
        if (cls != E1_SKIP && found.at[cls] == 0) {
            found.at[cls] = at;
        }
    }
    return found;
}

/* Items 5 and 6: a hard or black-box cell type, else (--auto-blackbox) an implicit black box. */
static odin3_elab_res resolve_outside(odin3_elab1 *e1, uint32_t name) {
    odin3_elab_res res = {ODIN3_RES_NONE, 0, {0}};
    odin3_celltype_id type;
    if (odin3_celltype_find(e1->design, name, &type)) {
        odin3_granularity gran = odin3_celltype_get(e1->design, type)->gran;
        if (gran == ODIN3_GRAN_HARD || gran == ODIN3_GRAN_BLACKBOX) {
            res.kind = ODIN3_RES_CELLTYPE;
            res.type = type;
            return res;
        }
    }
    if (e1->opts.auto_blackbox) {
        res.kind = ODIN3_RES_IMPLICIT_BLACKBOX;
        if (first_note(e1, name)) {
            odin3_elab1_info(e1, (odin3_loc){0},
                             "module '%s' is not defined; it becomes an implicit black box "
                             "(--auto-blackbox)",
                             text_of(e1, name));
        }
    }
    return res;
}

odin3_elab_res odin3_elab1_resolve(odin3_elab1 *e1, uint32_t name, uint32_t library) {
    e1_want want = {name, library};
    e1_found found = scan_chain(e1, &want);
    odin3_elab_res res = {ODIN3_RES_MODULE, 0, {0}};
    for (uint32_t cls = E1_SAME; cls < E1_CLASSES; cls++) {
        if (found.at[cls] == 0) {
            continue;
        }
        if (cls == E1_OTHER) {
            several_libraries(e1, name, found.at[cls]);
        }
        res.kind = cls == E1_UDP ? ODIN3_RES_UDP : ODIN3_RES_MODULE;
        res.def = found.at[cls] - 1;
        return res;
    }
    return resolve_outside(e1, name);
}
