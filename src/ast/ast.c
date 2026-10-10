/* ast.c — the AST store: create/destroy, the shape-checking builder, payloads, accessors. */
#include "ast/ast.h"
#include "ast/ast_internal.h"
#include "ast/kinds.h"
#include "ast/srcman.h"
#include "ir/celltype.h"
#include "ir/design.h"
#include "util/alloc.h"
#include "util/arena.h"
#include "util/log.h"
#include "util/pagevec.h"
#include "util/str.h"
#include "util/u64map.h"
#include "util/vec.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert(ODIN3_AST_MAX_NUMBER_BITS == ODIN3_READER_MAX_WIDTH,
               "a literal is capped at the readers' width cap (PHASE2 #5)");
_Static_assert(ODIN3_AST_MAX_DEPTH <= UINT16_MAX, "heights are uint16_t");

/* --- lifetime ------------------------------------------------------------------------------ */

static odin3_status misuse(const char *fn, const char *why) {
    odin3_log(ODIN3_LOG_ERROR, "%s: %s", fn, why);
    return ODIN3_ERR_INVALID_ARG;
}

static odin3_ast *ast_alloc(odin3_design *design) {
    odin3_ast *ast = odin3_util_calloc(sizeof *ast);
    if (ast == NULL) {
        return NULL;
    }
    ast->design = design;
    odin3_vec_init(&ast->children, sizeof(odin3_ast_id));
    odin3_vec_init(&ast->payloads, sizeof(odin3_ast_payload_rec));
    odin3_vec_init(&ast->units, sizeof(odin3_ast_id));
    odin3_vec_init(&ast->pending, sizeof(odin3_ast_id));
    odin3_vec_init(&ast->heights, sizeof(uint16_t));
    ast->max_children = ODIN3_AST_MAX_CHILDREN;
    ast->max_nodes = ODIN3_AST_MAX_NODES;
    ast->max_depth = ODIN3_AST_MAX_DEPTH;
    ast->nodes = odin3_pagevec_create(sizeof(odin3_ast_node));
    ast->arena = odin3_arena_create(0);
    if (ast->nodes == NULL || ast->arena == NULL || odin3_pagevec_push(ast->nodes, NULL) == NULL ||
        odin3_vec_push(&ast->heights) == NULL) {
        odin3_ast_destroy(ast);
        return NULL;
    }
    return ast;
}

odin3_status odin3_ast_create(odin3_design *design, odin3_ast_form form, odin3_passrun_id run,
                              odin3_ast **out) {
    if (design == NULL || out == NULL) {
        return misuse("odin3_ast_create", "NULL argument");
    }
    if (form != ODIN3_AST_FORM_PARSED && form != ODIN3_AST_FORM_ELABORATED) {
        return misuse("odin3_ast_create", "unknown form");
    }
    odin3_ast *ast = ast_alloc(design);
    if (ast == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    ast->form = form;
    ast->run = run;
    *out = ast;
    return ODIN3_OK;
}

void odin3_ast_destroy(odin3_ast *ast) {
    if (ast == NULL) {
        return;
    }
    odin3_pagevec_destroy(ast->nodes);
    odin3_vec_free(&ast->children);
    odin3_vec_free(&ast->payloads);
    odin3_vec_free(&ast->units);
    odin3_vec_free(&ast->pending);
    odin3_vec_free(&ast->heights);
    odin3_vec_free(&ast->comments);
    odin3_u64map_destroy(ast->attrs);
    odin3_arena_destroy(ast->arena);
    odin3_util_free(ast);
}

/* --- internal lookups ---------------------------------------------------------------------- */

static uint32_t node_end(const odin3_ast *ast) {
    return (uint32_t)odin3_pagevec_len(ast->nodes);
}

const odin3_ast_node *odin3_ast_rec(const odin3_ast *ast, odin3_ast_id node) {
    if (ast == NULL || node.v == 0 || node.v >= node_end(ast)) {
        return NULL;
    }
    return odin3_pagevec_cat(ast->nodes, node.v);
}

const odin3_ast_payload_rec *odin3_ast_payload_rec_of(const odin3_ast *ast, uint32_t id) {
    if (ast == NULL || id == 0 || id > ast->payloads.len) {
        return NULL;
    }
    return odin3_vec_cat(&ast->payloads, id - 1);
}

odin3_status odin3_ast_payload_push(odin3_ast *ast, odin3_ast_payload_rec rec, uint32_t *payload) {
    if (ast->payloads.len >= UINT32_MAX - 1) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_ast_payload_rec *slot = odin3_vec_push(&ast->payloads);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = rec;
    *payload = (uint32_t)ast->payloads.len;
    return ODIN3_OK;
}

/* --- shape checks (§5): a reason string, NULL when the spec fits the slot table ------------ */

static const char *check_name(const odin3_ast *ast, const odin3_ast_kind_info *info,
                              uint32_t name) {
    if (info->name_rule == ODIN3_AST_NAME_NO && name != 0) {
        return "name on a kind without one";
    }
    if (info->name_rule == ODIN3_AST_NAME_REQ && name == 0) {
        return "missing name";
    }
    if (name >= odin3_strtab_count(odin3_design_strtab(ast->design))) {
        return "name is not a strtab ID";
    }
    return NULL;
}

static const char *check_payload(const odin3_ast *ast, const odin3_ast_kind_info *info,
                                 uint32_t payload) {
    if (info->payload == ODIN3_AST_PAYLOAD_NONE) {
        return payload != 0 ? "payload on a kind without one" : NULL;
    }
    const odin3_ast_payload_rec *rec = odin3_ast_payload_rec_of(ast, payload);
    if (rec == NULL) {
        return "missing or invalid payload";
    }
    return rec->kind != info->payload ? "payload of the wrong type" : NULL;
}

static const char *check_scalars(const odin3_ast *ast, const odin3_ast_kind_info *info,
                                 const odin3_ast_spec *spec) {
    if (info == NULL) {
        return "unknown kind";
    }
    if (spec->reserved[0] != 0 || spec->reserved[1] != 0) {
        return "reserved fields must be zero";
    }
    if (spec->sub >= info->sub_count && !(info->sub_count == 0 && spec->sub == 0)) {
        return "sub-kind out of range";
    }
    if ((spec->flags & ~info->flags) != 0) {
        return "flag bit the kind does not define";
    }
    const char *why = check_name(ast, info, spec->name);
    return why != NULL ? why : check_payload(ast, info, spec->payload);
}

static const char *check_count(const odin3_ast_kind_info *info, uint32_t n) {
    if (n < info->nslots) {
        return "fewer children than slots";
    }
    if (info->tail == ODIN3_AST_CLS_NONE) {
        return n != info->nslots ? "children beyond the slots of a kind without a tail" : NULL;
    }
    uint32_t tail = n - info->nslots;
    return tail < info->tail_min || tail > info->tail_max ? "tail count out of bounds" : NULL;
}

static const char *check_children(const odin3_ast *ast, const odin3_ast_kind_info *info,
                                  uint16_t flags, odin3_ast_span kids) {
    const char *why = check_count(info, kids.n);
    uint32_t end = node_end(ast);
    for (uint32_t i = 0; why == NULL && i < kids.n; i++) {
        uint32_t id = kids.ids[i].v;
        if (id >= end) {
            why = "child not made yet";
        } else if (id == 0 && i < info->nslots) {
            why = (info->slots[i] & ODIN3_AST_SLOT_OPT) == 0 ? "mandatory slot is 0" : NULL;
        } else if (id == 0) {
            why = (info->tail_zero & flags) == 0 ? "0 tail entry outside E*0" : NULL;
        }
    }
    return why;
}

/* --- caps and reservation ------------------------------------------------------------------ */

static uint32_t height_of(const odin3_ast *ast, odin3_ast_span kids) {
    const uint16_t *heights = ast->heights.data;
    uint32_t height = 0;
    for (uint32_t i = 0; i < kids.n; i++) {
        uint32_t child = heights[kids.ids[i].v];
        height = child > height ? child : height;
    }
    return height + 1;
}

/* PARSE (located at spec->loc) past the child or depth cap; NO_MEMORY past the ID space. */
static odin3_status check_caps(odin3_ast *ast, const odin3_ast_spec *spec, odin3_ast_span kids,
                               uint32_t *height) {
    if (kids.n > ast->max_children) {
        odin3_diag(ast->design, ODIN3_LOG_ERROR, spec->loc, "more than %" PRIu32 " children",
                   ast->max_children);
        return ODIN3_ERR_PARSE;
    }
    *height = height_of(ast, kids);
    if (*height > ast->max_depth) {
        odin3_diag(ast->design, ODIN3_LOG_ERROR, spec->loc, "nesting deeper than %" PRIu32,
                   ast->max_depth);
        return ODIN3_ERR_PARSE;
    }
    if (node_end(ast) - 1 >= ast->max_nodes ||
        ast->children.len + kids.n > ODIN3_AST_MAX_CHILD_TABLE) {
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

static odin3_status reserve_node(odin3_ast *ast, const odin3_ast_spec *spec, odin3_ast_span kids) {
    if (odin3_pagevec_reserve(ast->nodes, 1) != ODIN3_OK ||
        odin3_vec_reserve(&ast->children, ast->children.len + kids.n) != ODIN3_OK ||
        odin3_vec_reserve(&ast->heights, ast->heights.len + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (spec->kind == ODIN3_AST_UNIT &&
        odin3_vec_reserve(&ast->units, ast->units.len + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    return ODIN3_OK;
}

/* Appends the record (room reserved, so kids may be a view of the child table). */
static odin3_ast_id append_node(odin3_ast *ast, const odin3_ast_spec *spec, odin3_ast_span kids,
                                uint32_t height) {
    size_t index = 0;
    odin3_ast_node *node = odin3_pagevec_push(ast->nodes, &index);
    *node = (odin3_ast_node){spec->kind,  spec->sub,  spec->flags, spec->loc.v,
                             spec->end.v, spec->name, 0,           kids.n};
    if (spec->payload != 0) {
        node->child = spec->payload;
    } else if (kids.n > 0) {
        node->child = (uint32_t)ast->children.len;
    }
    for (uint32_t i = 0; i < kids.n; i++) {
        *(odin3_ast_id *)odin3_vec_push(&ast->children) = kids.ids[i];
    }
    *(uint16_t *)odin3_vec_push(&ast->heights) = (uint16_t)height;
    odin3_ast_id id = {(uint32_t)index};
    if (spec->kind == ODIN3_AST_UNIT) {
        *(odin3_ast_id *)odin3_vec_push(&ast->units) = id;
    }
    ast->max_height = height > ast->max_height ? height : ast->max_height;
    return id;
}

/* The offset of kids in the child table when the caller passed a view of it, else SIZE_MAX. */
static size_t alias_offset(const odin3_ast *ast, odin3_ast_span kids) {
    uintptr_t base = (uintptr_t)ast->children.data;
    uintptr_t at = (uintptr_t)kids.ids;
    if (kids.n == 0 || base == 0 || at < base ||
        at >= base + ast->children.len * sizeof(odin3_ast_id)) {
        return SIZE_MAX;
    }
    return (at - base) / sizeof(odin3_ast_id);
}

static odin3_status build(odin3_ast *ast, const odin3_ast_spec *spec, odin3_ast_span kids,
                          odin3_ast_id *out) {
    const odin3_ast_kind_info *info = odin3_ast_kind_info_of(spec->kind);
    const char *why = check_scalars(ast, info, spec);
    why = why != NULL ? why : check_children(ast, info, spec->flags, kids);
    if (why != NULL) {
        odin3_log(ODIN3_LOG_ERROR, "odin3_ast_make: %s (kind %" PRIu32 " %s)", why,
                  (uint32_t)spec->kind, odin3_ast_kind_name(spec->kind));
        return ODIN3_ERR_INVALID_ARG;
    }
    uint32_t height = 0;
    odin3_status st = check_caps(ast, spec, kids, &height);
    size_t alias = alias_offset(ast, kids);
    st = st != ODIN3_OK ? st : reserve_node(ast, spec, kids);
    if (st != ODIN3_OK) {
        return st;
    }
    odin3_ast_span copy = kids;
    if (alias != SIZE_MAX) { /* a view of the child table: reserving may have moved it */
        copy.ids = (const odin3_ast_id *)ast->children.data + alias;
    }
    *out = append_node(ast, spec, copy, height);
    return ODIN3_OK;
}

/* --- builders ------------------------------------------------------------------------------ */

static const char *check_open(const odin3_ast *ast) {
    if (ast == NULL) {
        return "NULL argument";
    }
    return ast->finished ? "the store is finished" : NULL;
}

odin3_status odin3_ast_make(odin3_ast *ast, const odin3_ast_spec *spec, const odin3_ast_id *ids,
                            uint32_t n, odin3_ast_id *out) {
    const char *why = check_open(ast);
    if (why == NULL && (spec == NULL || out == NULL || (ids == NULL && n > 0))) {
        why = "NULL argument";
    }
    if (why != NULL) {
        return misuse("odin3_ast_make", why);
    }
    return build(ast, spec, (odin3_ast_span){ids, n}, out);
}

uint32_t odin3_ast_mark(const odin3_ast *ast) {
    return ast != NULL ? (uint32_t)ast->pending.len : 0;
}

odin3_status odin3_ast_push(odin3_ast *ast, odin3_ast_id node) {
    const char *why = check_open(ast);
    if (why == NULL && node.v >= node_end(ast)) {
        why = "node not made yet";
    }
    if (why != NULL) {
        return misuse("odin3_ast_push", why);
    }
    if (ast->pending.len >= UINT32_MAX) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_ast_id *slot = odin3_vec_push(&ast->pending);
    if (slot == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    *slot = node;
    return ODIN3_OK;
}

odin3_status odin3_ast_make_marked(odin3_ast *ast, const odin3_ast_spec *spec, uint32_t mark,
                                   odin3_ast_id *out) {
    const char *why = check_open(ast);
    if (why == NULL && (spec == NULL || out == NULL)) {
        why = "NULL argument";
    }
    if (why == NULL && mark > ast->pending.len) {
        why = "mark above the pending stack";
    }
    if (why != NULL) {
        return misuse("odin3_ast_make_marked", why);
    }
    const odin3_ast_id *top = ast->pending.data;
    odin3_ast_span kids = {top + mark, (uint32_t)ast->pending.len - mark};
    odin3_status st = build(ast, spec, kids, out);
    if (st == ODIN3_OK) {
        odin3_ast_unwind(ast, mark);
    }
    return st;
}

void odin3_ast_unwind(odin3_ast *ast, uint32_t mark) {
    if (ast == NULL) {
        return;
    }
    while (ast->pending.len > mark) {
        odin3_vec_pop(&ast->pending);
    }
}

odin3_status odin3_ast_real_new(odin3_ast *ast, double value, uint32_t *payload) {
    const char *why = check_open(ast);
    if (why == NULL && payload == NULL) {
        why = "NULL argument";
    }
    if (why != NULL) {
        return misuse("odin3_ast_real_new", why);
    }
    odin3_ast_payload_rec rec = {.kind = ODIN3_AST_PAYLOAD_REAL, .u.real = value};
    return odin3_ast_payload_push(ast, rec, payload);
}

odin3_status odin3_ast_text_new(odin3_ast *ast, odin3_bytes text, uint32_t *payload) {
    const char *why = check_open(ast);
    if (why == NULL && (payload == NULL || (text.ptr == NULL && text.len > 0))) {
        why = "NULL argument";
    }
    if (why != NULL) {
        return misuse("odin3_ast_text_new", why);
    }
    if (text.len > ODIN3_AST_MAX_TEXT_BYTES) {
        odin3_diag(ast->design, ODIN3_LOG_ERROR, (odin3_loc){0},
                   "opaque text longer than %" PRIu32 " bytes", ODIN3_AST_MAX_TEXT_BYTES);
        return ODIN3_ERR_PARSE;
    }
    if (odin3_vec_reserve(&ast->payloads, ast->payloads.len + 1) != ODIN3_OK) {
        return ODIN3_ERR_NO_MEMORY;
    }
    uint8_t *bytes = odin3_arena_alloc(ast->arena, text.len);
    if (bytes == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    if (text.len > 0) {
        memcpy(bytes, text.ptr, text.len);
    }
    odin3_ast_payload_rec rec = {.kind = ODIN3_AST_PAYLOAD_TEXT, .u.text = {bytes, text.len}};
    return odin3_ast_payload_push(ast, rec, payload);
}

odin3_status odin3_ast_intern(odin3_ast *ast, odin3_bytes str, uint32_t *name) {
    if (ast == NULL || name == NULL) {
        return misuse("odin3_ast_intern", "NULL argument");
    }
    return odin3_design_intern(ast->design, str, name);
}

odin3_status odin3_ast_finish(odin3_ast *ast) {
    const char *why = check_open(ast);
    if (why != NULL) {
        return misuse("odin3_ast_finish", why);
    }
    odin3_vec_free(&ast->pending);
    odin3_vec_free(&ast->heights);
    ast->finished = true;
    return ODIN3_OK;
}

/* --- accessors ----------------------------------------------------------------------------- */

odin3_ast_kind odin3_ast_kind_of(const odin3_ast *ast, odin3_ast_id node) {
    const odin3_ast_node *rec = odin3_ast_rec(ast, node);
    return rec != NULL ? (odin3_ast_kind)rec->kind : ODIN3_AST_NONE;
}

uint32_t odin3_ast_sub(const odin3_ast *ast, odin3_ast_id node) {
    const odin3_ast_node *rec = odin3_ast_rec(ast, node);
    return rec != NULL ? rec->sub : 0;
}

uint16_t odin3_ast_flags(const odin3_ast *ast, odin3_ast_id node) {
    const odin3_ast_node *rec = odin3_ast_rec(ast, node);
    return rec != NULL ? rec->flags : 0;
}

odin3_loc odin3_ast_loc(const odin3_ast *ast, odin3_ast_id node) {
    const odin3_ast_node *rec = odin3_ast_rec(ast, node);
    return (odin3_loc){rec != NULL ? rec->loc : 0};
}

odin3_loc odin3_ast_end(const odin3_ast *ast, odin3_ast_id node) {
    const odin3_ast_node *rec = odin3_ast_rec(ast, node);
    return (odin3_loc){rec != NULL ? rec->end : 0};
}

uint32_t odin3_ast_name(const odin3_ast *ast, odin3_ast_id node) {
    const odin3_ast_node *rec = odin3_ast_rec(ast, node);
    return rec != NULL ? rec->name : 0;
}

const char *odin3_ast_name_str(const odin3_ast *ast, odin3_ast_id node) {
    const odin3_ast_node *rec = odin3_ast_rec(ast, node);
    const char *str =
        rec != NULL ? odin3_strtab_get(odin3_design_strtab(ast->design), rec->name) : NULL;
    return str != NULL ? str : "";
}

const char *odin3_ast_sub_name(const odin3_ast *ast, odin3_ast_id node) {
    const odin3_ast_node *rec = odin3_ast_rec(ast, node);
    const odin3_ast_kind_info *info = rec != NULL ? odin3_ast_kind_info_of(rec->kind) : NULL;
    return info != NULL && rec->sub < info->sub_count ? info->sub_names[rec->sub] : "";
}

uint32_t odin3_ast_nchild(const odin3_ast *ast, odin3_ast_id node) {
    const odin3_ast_node *rec = odin3_ast_rec(ast, node);
    return rec != NULL ? rec->nchild : 0;
}

odin3_ast_id odin3_ast_child(const odin3_ast *ast, odin3_ast_id node, uint32_t idx) {
    const odin3_ast_node *rec = odin3_ast_rec(ast, node);
    if (rec == NULL || idx >= rec->nchild) {
        return (odin3_ast_id){0};
    }
    return *(const odin3_ast_id *)odin3_vec_cat(&ast->children, (size_t)rec->child + idx);
}

odin3_ast_span odin3_ast_children(const odin3_ast *ast, odin3_ast_id node) {
    const odin3_ast_node *rec = odin3_ast_rec(ast, node);
    if (rec == NULL || rec->nchild == 0) {
        return (odin3_ast_span){NULL, 0};
    }
    return (odin3_ast_span){(const odin3_ast_id *)ast->children.data + rec->child, rec->nchild};
}

uint32_t odin3_ast_payload(const odin3_ast *ast, odin3_ast_id node) {
    const odin3_ast_node *rec = odin3_ast_rec(ast, node);
    const odin3_ast_kind_info *info = rec != NULL ? odin3_ast_kind_info_of(rec->kind) : NULL;
    return info != NULL && info->payload != ODIN3_AST_PAYLOAD_NONE ? rec->child : 0;
}

double odin3_ast_real(const odin3_ast *ast, odin3_ast_id node) {
    const odin3_ast_payload_rec *rec = odin3_ast_payload_rec_of(ast, odin3_ast_payload(ast, node));
    return rec != NULL && rec->kind == ODIN3_AST_PAYLOAD_REAL ? rec->u.real : 0.0;
}

odin3_bytes odin3_ast_text(const odin3_ast *ast, odin3_ast_id node) {
    const odin3_ast_payload_rec *rec = odin3_ast_payload_rec_of(ast, odin3_ast_payload(ast, node));
    if (rec == NULL || rec->kind != ODIN3_AST_PAYLOAD_TEXT) {
        return (odin3_bytes){NULL, 0};
    }
    return (odin3_bytes){rec->u.text.ptr, rec->u.text.len};
}

odin3_ast_id odin3_ast_node_end(const odin3_ast *ast) {
    return (odin3_ast_id){ast != NULL ? node_end(ast) : 0};
}

odin3_ast_form odin3_ast_form_of(const odin3_ast *ast) {
    return ast != NULL ? ast->form : (odin3_ast_form)0;
}

odin3_passrun_id odin3_ast_run(const odin3_ast *ast) {
    return ast != NULL ? ast->run : (odin3_passrun_id){0};
}

uint32_t odin3_ast_max_height(const odin3_ast *ast) {
    return ast != NULL ? ast->max_height : 0;
}

uint32_t odin3_ast_root_count(const odin3_ast *ast) {
    return ast != NULL ? (uint32_t)ast->units.len : 0;
}

odin3_ast_id odin3_ast_root(const odin3_ast *ast, uint32_t idx) {
    return ast != NULL && idx < ast->units.len
               ? *(const odin3_ast_id *)odin3_vec_cat(&ast->units, idx)
               : (odin3_ast_id){0};
}

const odin3_design *odin3_ast_design(const odin3_ast *ast) {
    return ast != NULL ? ast->design : NULL;
}

size_t odin3_ast_bytes_reserved(const odin3_ast *ast) {
    return odin3_pagevec_bytes_reserved(ast->nodes) + ast->children.cap * ast->children.elem_size +
           ast->payloads.cap * ast->payloads.elem_size + odin3_arena_bytes_reserved(ast->arena);
}
