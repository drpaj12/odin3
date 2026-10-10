/*
 * ast_abi.c — the public ABI's AST group (ABI v4): the design's AST stores and keep level, node
 * and store readers, and the builders adapters use, over src/ast/ast.h.
 */
#include "api/api.h"

#include "ast/ast.h"
#include "ast/kinds.h"
#include "ir/design.h"
#include "ir/ids.h"
#include "odin3/odin3.h"
#include "util/alloc.h"
#include "util/attr.h"
#include "util/str.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* --- odin3.h's enums equal the slot table's order (kinds.h generates the table) ------------ */

#define ROW_(KIND, MEMBER, NAME, SUB, FLAGS, SLOTS, TAIL, PAYLOAD) ROW_##KIND,
enum { ROW_NONE = 0, ODIN3_AST_KINDS(ROW_) ROW_COUNT };

#define KIND_EQ_(KIND, MEMBER, NAME, SUB, FLAGS, SLOTS, TAIL, PAYLOAD)                             \
    _Static_assert((unsigned)ROW_##KIND == (unsigned)ODIN3_AST_##KIND, "odin3.h: kind " #KIND);
ODIN3_AST_KINDS(KIND_EQ_)
_Static_assert((unsigned)ROW_COUNT == (unsigned)ODIN3_AST_KIND_COUNT, "odin3.h: kind count");

#define SUBROW_(KIND, NAME, TEXT) SUBROW_##KIND##_##NAME,
#define SUB_EQ_(KIND, NAME, TEXT)                                                                  \
    _Static_assert((unsigned)SUBROW_##KIND##_##NAME == (unsigned)ODIN3_AST_##KIND##_##NAME,        \
                   "odin3.h: sub-kind " #KIND "_" #NAME);
#define SUBS_EQ_(KIND)                                                                             \
    enum { ODIN3_AST_SUBS_##KIND(SUBROW_) SUBROW_COUNT_##KIND };                                   \
    ODIN3_AST_SUBS_##KIND(SUB_EQ_) _Static_assert((unsigned)SUBROW_COUNT_##KIND ==                 \
                                                      (unsigned)ODIN3_AST_##KIND##_COUNT,          \
                                                  "odin3.h: " #KIND " count");

SUBS_EQ_(DIRECTIVE)
SUBS_EQ_(PORT_DECL)
SUBS_EQ_(NET_DECL)
SUBS_EQ_(VAR_DECL)
SUBS_EQ_(PARAM_DECL)
SUBS_EQ_(STRENGTH)
SUBS_EQ_(ALWAYS)
SUBS_EQ_(GATE_DECL)
SUBS_EQ_(PROC_CONT_ASSIGN)
SUBS_EQ_(CASE)
SUBS_EQ_(EVENT_EXPR)
SUBS_EQ_(NUMBER)
SUBS_EQ_(SELECT)
SUBS_EQ_(UNARY)
SUBS_EQ_(BINARY)

/* --- helpers --------------------------------------------------------------------------------- */

/* True for a store and a node ID made in it (1 .. end - 1). */
static bool node_ok(const odin3_ast *ast, uint32_t node) {
    return ast != NULL && node != 0 && node < odin3_ast_node_end(ast).v;
}

static odin3_ast_id id_of(uint32_t node) {
    return (odin3_ast_id){node};
}

static bool form_ok(uint32_t form) {
    return form == ODIN3_AST_FORM_PARSED || form == ODIN3_AST_FORM_ELABORATED;
}

/* --- design handles -------------------------------------------------------------------------- */

ODIN3_EXPORT odin3_status odin3_design_new_ast(odin3_design *design, uint32_t form, uint32_t run,
                                               odin3_ast **ast) {
    odin3_ast *held = NULL;
    if (design == NULL || ast == NULL || !form_ok(form) || run == 0 ||
        odin3_design_find_ast(design, (odin3_passrun_id){run}, (odin3_ast_form)form, &held) !=
            ODIN3_OK) {
        return odin3_api_invalid(__func__);
    }
    if (held != NULL) {
        return odin3_api_invalid_why(__func__, "a store of the same run and form is held");
    }
    odin3_ast *made = NULL;
    odin3_status st =
        odin3_ast_create(design, (odin3_ast_form)form, (odin3_passrun_id){run}, &made);
    if (st == ODIN3_OK) {
        st = odin3_design_set_ast(design, made);
    }
    if (st != ODIN3_OK) {
        odin3_ast_destroy(made);
        return st;
    }
    *ast = made;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_design_get_ast(const odin3_design *design, uint32_t run,
                                               uint32_t form, odin3_ast **ast) {
    if (design == NULL || ast == NULL || !form_ok(form)) {
        return odin3_api_invalid(__func__);
    }
    return odin3_design_find_ast(design, (odin3_passrun_id){run}, (odin3_ast_form)form, ast);
}

ODIN3_EXPORT odin3_status odin3_design_get_ast_count(const odin3_design *design, uint32_t *count) {
    if (design == NULL || count == NULL) {
        return odin3_api_invalid(__func__);
    }
    *count = odin3_design_ast_count(design);
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_design_get_ast_at(const odin3_design *design, uint32_t index,
                                                  odin3_ast **ast) {
    odin3_ast *found = design != NULL ? odin3_design_ast_at(design, index) : NULL;
    if (found == NULL || ast == NULL) {
        return odin3_api_invalid(__func__);
    }
    *ast = found;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_design_get_keep_ast(const odin3_design *design,
                                                    odin3_keep_ast *level) {
    if (design == NULL || level == NULL) {
        return odin3_api_invalid(__func__);
    }
    *level = odin3_design_keep_ast(design);
    return ODIN3_OK;
}

/* --- node readers ---------------------------------------------------------------------------- */

ODIN3_EXPORT odin3_status odin3_ast_get_kind(const odin3_ast *ast, uint32_t node, uint32_t *kind) {
    if (!node_ok(ast, node) || kind == NULL) {
        return odin3_api_invalid(__func__);
    }
    *kind = (uint32_t)odin3_ast_kind_of(ast, id_of(node));
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_ast_get_sub(const odin3_ast *ast, uint32_t node, uint32_t *sub) {
    if (!node_ok(ast, node) || sub == NULL) {
        return odin3_api_invalid(__func__);
    }
    *sub = odin3_ast_sub(ast, id_of(node));
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_ast_get_flags(const odin3_ast *ast, uint32_t node,
                                              uint32_t *flags) {
    if (!node_ok(ast, node) || flags == NULL) {
        return odin3_api_invalid(__func__);
    }
    *flags = odin3_ast_flags(ast, id_of(node));
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_ast_get_loc(const odin3_ast *ast, uint32_t node, uint32_t *loc) {
    if (!node_ok(ast, node) || loc == NULL) {
        return odin3_api_invalid(__func__);
    }
    *loc = odin3_ast_loc(ast, id_of(node)).v;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_ast_get_end(const odin3_ast *ast, uint32_t node, uint32_t *end) {
    if (!node_ok(ast, node) || end == NULL) {
        return odin3_api_invalid(__func__);
    }
    *end = odin3_ast_end(ast, id_of(node)).v;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_ast_get_name(const odin3_ast *ast, uint32_t node,
                                             const char **name) {
    if (!node_ok(ast, node) || name == NULL) {
        return odin3_api_invalid(__func__);
    }
    *name = odin3_ast_name_str(ast, id_of(node));
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_ast_get_nchild(const odin3_ast *ast, uint32_t node,
                                               uint32_t *nchild) {
    if (!node_ok(ast, node) || nchild == NULL) {
        return odin3_api_invalid(__func__);
    }
    *nchild = odin3_ast_nchild(ast, id_of(node));
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_ast_get_child(const odin3_ast *ast, uint32_t node, uint32_t index,
                                              uint32_t *child) {
    if (!node_ok(ast, node) || child == NULL || index >= odin3_ast_nchild(ast, id_of(node))) {
        return odin3_api_invalid(__func__);
    }
    *child = odin3_ast_child(ast, id_of(node), index).v;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_ast_get_kind_name(const odin3_ast *ast, uint32_t node,
                                                  const char **name) {
    if (!node_ok(ast, node) || name == NULL) {
        return odin3_api_invalid(__func__);
    }
    *name = odin3_ast_kind_name((uint32_t)odin3_ast_kind_of(ast, id_of(node)));
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_ast_get_sub_name(const odin3_ast *ast, uint32_t node,
                                                 const char **name) {
    if (!node_ok(ast, node) || name == NULL) {
        return odin3_api_invalid(__func__);
    }
    *name = odin3_ast_sub_name(ast, id_of(node));
    return ODIN3_OK;
}

/* The character of stored bit `bit` (an odin3_bit) of a literal (has_question: z is '?'). */
static char bit_char(uint8_t bit, bool question) {
    switch (bit) {
    case ODIN3_BIT_0:
        return '0';
    case ODIN3_BIT_1:
        return '1';
    case ODIN3_BIT_X:
        return 'x';
    default:
        return question ? '?' : 'z';
    }
}

ODIN3_EXPORT odin3_status odin3_ast_get_number_str(const odin3_ast *ast, uint32_t node,
                                                   const char **bits, uint32_t *width) {
    if (!node_ok(ast, node) || bits == NULL || width == NULL) {
        return odin3_api_invalid(__func__);
    }
    const odin3_ast_number *num = odin3_ast_number_of(ast, id_of(node));
    if (num == NULL) {
        return odin3_api_invalid_why(__func__, "not a NUMBER node");
    }
    char *text = odin3_util_malloc(num->nbits);
    if (text == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < num->nbits; i++) {
        text[i] = bit_char(num->bits[num->nbits - 1 - i], num->has_question); /* MSB first */
    }
    odin3_status st = odin3_api_text(odin3_ast_design(ast), (odin3_bytes){text, num->nbits}, bits);
    odin3_util_free(text);
    if (st == ODIN3_OK) {
        *width = num->width;
    }
    return st;
}

ODIN3_EXPORT odin3_status odin3_ast_get_real(const odin3_ast *ast, uint32_t node, double *value) {
    if (!node_ok(ast, node) || value == NULL) {
        return odin3_api_invalid(__func__);
    }
    if (odin3_ast_kind_of(ast, id_of(node)) != ODIN3_AST_REAL) {
        return odin3_api_invalid_why(__func__, "not a REAL node");
    }
    *value = odin3_ast_real(ast, id_of(node));
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_ast_get_text(const odin3_ast *ast, uint32_t node, const char **text,
                                             uint32_t *len) {
    if (!node_ok(ast, node) || text == NULL || len == NULL) {
        return odin3_api_invalid(__func__);
    }
    const odin3_ast_kind_info *info = odin3_ast_kind_info_of(odin3_ast_kind_of(ast, id_of(node)));
    if (info == NULL || info->payload != ODIN3_AST_PAYLOAD_TEXT) {
        return odin3_api_invalid_why(__func__, "not a TEXT-payload node");
    }
    odin3_bytes bytes = odin3_ast_text(ast, id_of(node));
    *text = bytes.ptr != NULL ? bytes.ptr : "";
    *len = (uint32_t)bytes.len; /* at most ODIN3_AST_MAX_TEXT_BYTES */
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_ast_get_attr_count(const odin3_ast *ast, uint32_t node,
                                                   uint32_t *count) {
    if (!node_ok(ast, node) || count == NULL) {
        return odin3_api_invalid(__func__);
    }
    *count = odin3_ast_attrs(ast, id_of(node)).n;
    return ODIN3_OK;
}

/* Attribute index of node into *attr; false when the node has fewer attributes. */
static bool attr_of(const odin3_ast *ast, odin3_ast_id node, uint32_t index, uint32_t *attr) {
    odin3_ast_span attrs = odin3_ast_attrs(ast, node);
    if (index >= attrs.n) {
        return false;
    }
    *attr = attrs.ids[index].v;
    return true;
}

ODIN3_EXPORT odin3_status odin3_ast_get_attr_at(const odin3_ast *ast, uint32_t node, uint32_t index,
                                                uint32_t *attr) {
    uint32_t found = 0;
    if (!node_ok(ast, node) || attr == NULL || !attr_of(ast, id_of(node), index, &found)) {
        return odin3_api_invalid(__func__);
    }
    *attr = found;
    return ODIN3_OK;
}

/* --- store readers --------------------------------------------------------------------------- */

ODIN3_EXPORT odin3_status odin3_ast_get_node_end(const odin3_ast *ast, uint32_t *end) {
    if (ast == NULL || end == NULL) {
        return odin3_api_invalid(__func__);
    }
    *end = odin3_ast_node_end(ast).v;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_ast_get_form(const odin3_ast *ast, uint32_t *form) {
    if (ast == NULL || form == NULL) {
        return odin3_api_invalid(__func__);
    }
    *form = (uint32_t)odin3_ast_form_of(ast);
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_ast_get_run(const odin3_ast *ast, uint32_t *run) {
    if (ast == NULL || run == NULL) {
        return odin3_api_invalid(__func__);
    }
    *run = odin3_ast_run(ast).v;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_ast_get_root_count(const odin3_ast *ast, uint32_t *count) {
    if (ast == NULL || count == NULL) {
        return odin3_api_invalid(__func__);
    }
    *count = odin3_ast_root_count(ast);
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_ast_get_root_at(const odin3_ast *ast, uint32_t index,
                                                uint32_t *root) {
    if (ast == NULL || root == NULL || index >= odin3_ast_root_count(ast)) {
        return odin3_api_invalid(__func__);
    }
    *root = odin3_ast_root(ast, index).v;
    return ODIN3_OK;
}

/* --- builders -------------------------------------------------------------------------------- */

/*
 * The node of *spec with children ids[0 .. n): the IDs go on the pending stack above its top
 * (the uint32_t array is never read as odin3_ast_id), then make_marked takes them; any failure
 * pops them again, so the store and the caller's pending entries are unchanged.
 */
ODIN3_EXPORT odin3_status odin3_ast_build_node(odin3_ast *ast, const odin3_ast_spec *spec,
                                               const uint32_t *ids, uint32_t n, uint32_t *node) {
    if (ast == NULL || spec == NULL || node == NULL || (ids == NULL && n > 0)) {
        return odin3_api_invalid(__func__);
    }
    uint32_t mark = odin3_ast_mark(ast);
    odin3_status st = ODIN3_OK;
    for (uint32_t i = 0; st == ODIN3_OK && i < n; i++) {
        st = odin3_ast_push(ast, id_of(ids[i]));
    }
    odin3_ast_id made = {0};
    if (st == ODIN3_OK) {
        st = odin3_ast_make_marked(ast, spec, mark, &made);
    }
    if (st != ODIN3_OK) {
        odin3_ast_unwind(ast, mark);
        return st;
    }
    *node = made.v;
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_ast_build_mark(const odin3_ast *ast, uint32_t *mark) {
    if (ast == NULL || mark == NULL) {
        return odin3_api_invalid(__func__);
    }
    *mark = odin3_ast_mark(ast);
    return ODIN3_OK;
}

ODIN3_EXPORT odin3_status odin3_ast_build_push(odin3_ast *ast, uint32_t node) {
    if (ast == NULL) {
        return odin3_api_invalid(__func__);
    }
    return odin3_ast_push(ast, id_of(node));
}

ODIN3_EXPORT odin3_status odin3_ast_build_marked(odin3_ast *ast, const odin3_ast_spec *spec,
                                                 uint32_t mark, uint32_t *node) {
    if (ast == NULL || spec == NULL || node == NULL) {
        return odin3_api_invalid(__func__);
    }
    odin3_ast_id made = {0};
    odin3_status st = odin3_ast_make_marked(ast, spec, mark, &made);
    if (st == ODIN3_OK) {
        *node = made.v;
    }
    return st;
}

ODIN3_EXPORT odin3_status odin3_ast_build_unwind(odin3_ast *ast, uint32_t mark) {
    if (ast == NULL) {
        return odin3_api_invalid(__func__);
    }
    odin3_ast_unwind(ast, mark);
    return ODIN3_OK;
}

/* Stores the MSB-first characters of text[0 .. nbits) LSB first in lsb and sets num's flags;
 * false for a character other than 0 1 x z ?. */
static bool bits_from_text(const char *text, odin3_ast_number *num, uint8_t *lsb) {
    static const char k_digits[] = "01xz?";
    static const uint8_t k_bit[] = {ODIN3_BIT_0, ODIN3_BIT_1, ODIN3_BIT_X, ODIN3_BIT_Z,
                                    ODIN3_BIT_Z};
    for (uint32_t i = 0; i < num->nbits; i++) {
        char chr = text[num->nbits - 1 - i];
        const char *at = chr != '\0' ? strchr(k_digits, chr) : NULL;
        if (at == NULL) {
            return false;
        }
        lsb[i] = k_bit[at - k_digits];
        num->has_xz |= chr == 'x' || chr == 'z' || chr == '?';
        num->has_question |= chr == '?';
    }
    return true;
}

/* odin3_ast_number_new_str past its argument checks. */
static odin3_status number_from_text(odin3_ast *ast, odin3_loc loc, uint32_t width,
                                     const char *bits, uint32_t *payload) {
    /* one past the cap is enough: number_new rejects it (PARSE) before reading any bit */
    size_t len = strnlen(bits, (size_t)ODIN3_AST_MAX_NUMBER_BITS + 1);
    uint8_t *lsb = odin3_util_malloc(len);
    if (lsb == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    odin3_ast_number num = {.width = width, .nbits = (uint32_t)len, .bits = lsb};
    odin3_status st = ODIN3_OK;
    if (len <= (size_t)ODIN3_AST_MAX_NUMBER_BITS && !bits_from_text(bits, &num, lsb)) {
        st = odin3_api_invalid_why("odin3_ast_number_new_str",
                                   "a bit character other than 0 1 x z ?");
    }
    if (st == ODIN3_OK) {
        st = odin3_ast_number_new(ast, loc, &num, payload);
    }
    odin3_util_free(lsb);
    return st;
}

ODIN3_EXPORT odin3_status odin3_ast_number_new_str(odin3_ast *ast, uint32_t loc, uint32_t width,
                                                   const char *bits, uint32_t *payload) {
    if (ast == NULL || bits == NULL || payload == NULL || bits[0] == '\0') {
        return odin3_api_invalid(__func__);
    }
    return number_from_text(ast, (odin3_loc){loc}, width, bits, payload);
}

ODIN3_EXPORT odin3_status odin3_ast_build_real(odin3_ast *ast, double value, uint32_t *payload) {
    if (ast == NULL || payload == NULL) {
        return odin3_api_invalid(__func__);
    }
    return odin3_ast_real_new(ast, value, payload);
}

ODIN3_EXPORT odin3_status odin3_ast_build_text(odin3_ast *ast, const char *text, uint32_t len,
                                               uint32_t *payload) {
    if (ast == NULL || payload == NULL || (text == NULL && len > 0)) {
        return odin3_api_invalid(__func__);
    }
    return odin3_ast_text_new(ast, (odin3_bytes){text, len}, payload);
}

ODIN3_EXPORT odin3_status odin3_ast_build_attach(odin3_ast *ast, uint32_t node,
                                                 const uint32_t *attrs, uint32_t n) {
    if (ast == NULL || attrs == NULL || n == 0) {
        return odin3_api_invalid(__func__);
    }
    odin3_ast_id *ids = odin3_util_malloc((size_t)n * sizeof *ids);
    if (ids == NULL) {
        return ODIN3_ERR_NO_MEMORY;
    }
    for (uint32_t i = 0; i < n; i++) {
        ids[i] = id_of(attrs[i]);
    }
    odin3_status st = odin3_ast_attach(ast, id_of(node), ids, n);
    odin3_util_free(ids);
    return st;
}

ODIN3_EXPORT odin3_status odin3_ast_build_comment(odin3_ast *ast,
                                                  const odin3_ast_comment_spec *spec) {
    if (ast == NULL || spec == NULL) {
        return odin3_api_invalid(__func__);
    }
    return odin3_ast_comment_add(ast, spec);
}

ODIN3_EXPORT odin3_status odin3_ast_build_intern(odin3_ast *ast, const char *str, uint32_t *name) {
    if (ast == NULL || str == NULL || name == NULL) {
        return odin3_api_invalid(__func__);
    }
    return odin3_ast_intern(ast, odin3_bytes_cstr(str), name);
}
